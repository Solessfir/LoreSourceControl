// Copyright Solessfir 2026. All Rights Reserved.

#include "LoreSourceControlModule.h"
#include "LoreSourceControlOperations.h"
#include "LoreSourceControlProvider.h"
#include "LoreSourceControlUtils.h"
#include "Misc/App.h"
#include "Modules/ModuleManager.h"
#include "Features/IModularFeatures.h"
#include "ILoreSourceControlWorker.h"
#include "ISourceControlModule.h"
#include "SourceControlOperations.h"
#include "HAL/IConsoleManager.h"
#include "Misc/Paths.h"
#include "Async/Async.h"
#if SOURCE_CONTROL_WITH_SLATE
#include "SourceControlWindows.h"
#include "ToolMenus.h"
#include "Misc/MessageDialog.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/SBoxPanel.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "RevisionControlStyle/RevisionControlStyle.h"
#include "Styling/AppStyle.h"
#include "Interfaces/IMainFrameModule.h"
#include "Editor.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "Toolkits/IToolkitHost.h"
#endif

#define LOCTEXT_NAMESPACE "LoreSourceControl"

// Local helper to create workers (avoids issues taking address of member templates)
template <typename TWorker>
static FLoreSourceControlWorkerRef CreateLoreWorker()
{
	return MakeShareable(new TWorker());
}

void FLoreSourceControlModule::StartupModule()
{
	// Register our operations with the provider
	// Core operations for the main goal: Sync, Commit (CheckIn), Lock/Unlock (via CheckOut / Unlock)
	LoreSourceControlProvider.RegisterWorker("Connect", FLoreGetSourceControlWorker::CreateStatic(&CreateLoreWorker<FLoreConnectWorker>));
	LoreSourceControlProvider.RegisterWorker("UpdateStatus", FLoreGetSourceControlWorker::CreateStatic(&CreateLoreWorker<FLoreUpdateStatusWorker>));
	LoreSourceControlProvider.RegisterWorker("CheckIn", FLoreGetSourceControlWorker::CreateStatic(&CreateLoreWorker<FLoreCheckInWorker>));
	LoreSourceControlProvider.RegisterWorker("Sync", FLoreGetSourceControlWorker::CreateStatic(&CreateLoreWorker<FLoreSyncWorker>));
	LoreSourceControlProvider.RegisterWorker("CheckOut", FLoreGetSourceControlWorker::CreateStatic(&CreateLoreWorker<FLoreCheckOutWorker>));
	LoreSourceControlProvider.RegisterWorker("Revert", FLoreGetSourceControlWorker::CreateStatic(&CreateLoreWorker<FLoreRevertWorker>));
	LoreSourceControlProvider.RegisterWorker("MarkForAdd", FLoreGetSourceControlWorker::CreateStatic(&CreateLoreWorker<FLoreMarkForAddWorker>));
	LoreSourceControlProvider.RegisterWorker("Delete", FLoreGetSourceControlWorker::CreateStatic(&CreateLoreWorker<FLoreDeleteWorker>));
	LoreSourceControlProvider.RegisterWorker("Unlock", FLoreGetSourceControlWorker::CreateStatic(&CreateLoreWorker<FLoreUnlockWorker>));

	// Load settings (binary path, etc.)
	LoreSourceControlProvider.LoadSettings();

	// Bind our source control provider to the editor
	IModularFeatures::Get().RegisterModularFeature("SourceControl", &LoreSourceControlProvider);

	// Console commands for quick access inside editor (main goal: no need to close editor)
	IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("LoreSync"),
		TEXT("Perform a Lore sync (pull) and update source control states."),
		FConsoleCommandDelegate::CreateLambda([]()
		{
			ISourceControlModule& SCModule = FModuleManager::LoadModuleChecked<ISourceControlModule>("SourceControl");
			if (SCModule.GetProvider().GetName() == "Lore")
			{
				const TSharedRef<FSync> SyncOp = ISourceControlOperation::Create<FSync>();
				SCModule.GetProvider().Execute(SyncOp, EConcurrency::Synchronous);
			}
		}),
		ECVF_Default
	);

	IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("LoreStatus"),
		TEXT("Force update Lore source control status for the project and print it in human-readable form."),
		FConsoleCommandDelegate::CreateLambda([]()
		{
			ISourceControlModule& SCModule = FModuleManager::LoadModuleChecked<ISourceControlModule>("SourceControl");
			if (SCModule.GetProvider().GetName() != "Lore")
			{
				return;
			}

			// Refresh the editor's own state cache (Content Browser icons, etc.) - this leg always needs
			// --json, same as every other internal caller.
			const TSharedRef<FUpdateStatus> StatusOp = ISourceControlOperation::Create<FUpdateStatus>();
			SCModule.GetProvider().Execute(StatusOp, TArray<FString>{ FPaths::ProjectDir() }, EConcurrency::Synchronous);

			// Separate call, without --json, so the console shows lore's own readable status text
			// instead of raw JSON. Runs on the pool - unlike Execute() above (which pumps Slate while
			// it waits), a direct RunLoreCommand here would freeze the editor for the whole scan.
			FLoreSourceControlProvider& Provider = static_cast<FLoreSourceControlProvider&>(SCModule.GetProvider());
			const FString LoreBinary = Provider.GetLoreBinaryPath();
			const FString RepositoryRoot = Provider.GetRepositoryRoot();

			Async(EAsyncExecution::ThreadPool, [LoreBinary, RepositoryRoot]()
			{
				TArray<FString> Results, Errors;
				FLoreSourceControlUtils::RunLoreCommand(TEXT("status"), LoreBinary, RepositoryRoot, { TEXT("--scan") }, TArray<FString>(), Results, Errors, /*bUseJson=*/false);

				AsyncTask(ENamedThreads::GameThread, [Results, Errors]()
				{
					for (const FString& Line : Results)
					{
						UE_LOG(LogSourceControl, Display, TEXT("%s"), *Line);
					}

					for (const FString& Line : Errors)
					{
						UE_LOG(LogSourceControl, Error, TEXT("%s"), *Line);
					}
				});
			});
		}),
		ECVF_Default
	);

	IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("LoreCommit"),
		TEXT("Open the Submit Files dialog to commit and push pending changes via Lore."),
		FConsoleCommandDelegate::CreateLambda([]()
		{
#if SOURCE_CONTROL_WITH_SLATE
			ISourceControlModule& SCModule = FModuleManager::LoadModuleChecked<ISourceControlModule>("SourceControl");
			if (SCModule.GetProvider().GetName() == "Lore")
			{
				// Same dialog as the toolbar's "Submit Content" - drives our CheckIn worker (commit + push).
				FSourceControlWindows::ChoosePackagesToCheckIn();
			}
#endif
		}),
		ECVF_Default
	);

#if SOURCE_CONTROL_WITH_SLATE
	// Headless commandlets (cook, UAT, etc.) load this module too but never initialize Slate - skip all
	// UI registration in that case, since MainFrame/toolbar/window delegates all require a live Slate app.
	if (FSlateApplication::IsInitialized())
	{
		// The engine's own Revision Control widget registers inside SStatusBar::Construct(), which only runs
		// once the level editor's main tab is built - well after this module loads. Registering via
		// UToolMenus::RegisterStartupCallback (module load time) would be too early for that widget to exist
		// yet, so wait for the main frame instead.
		IMainFrameModule& MainFrameModule = FModuleManager::LoadModuleChecked<IMainFrameModule>("MainFrame");
		if (MainFrameModule.IsWindowInitialized())
		{
			RegisterToolbarExtension();
		}
		else
		{
			MainFrameModule.OnMainFrameCreationFinished().AddRaw(this, &FLoreSourceControlModule::OnMainFrameCreationFinished);
		}

		// The branch switcher's entry only appears once Lore is connected (see RegisterToolbarExtension) -
		// refresh the toolbar whenever that state changes so it appears/disappears without an editor restart.
		SourceControlStateChangedHandle = LoreSourceControlProvider.RegisterSourceControlStateChanged_Handle(
			FSourceControlStateChanged::FDelegate::CreateRaw(this, &FLoreSourceControlModule::RefreshToolbarExtension));

		// Toggling "Enable Source Control" (or switching providers) in Editor Preferences doesn't touch
		// our own provider's state at all, so it needs its own refresh trigger.
		SourceControlProviderChangedHandle = ISourceControlModule::Get().RegisterProviderChanged(
			FSourceControlProviderChanged::FDelegate::CreateRaw(this, &FLoreSourceControlModule::OnSourceControlProviderChanged));

		// See OnWindowBeingDestroyed()'s comment - catches the Submit dialog closing (accept or cancel).
		WindowBeingDestroyedHandle = FSlateApplication::Get().OnWindowBeingDestroyed().AddRaw(this, &FLoreSourceControlModule::OnWindowBeingDestroyed);
	}
#endif
}

void FLoreSourceControlModule::ShutdownModule()
{
#if SOURCE_CONTROL_WITH_SLATE
	if (IMainFrameModule* MainFrameModule = FModuleManager::GetModulePtr<IMainFrameModule>("MainFrame"))
	{
		MainFrameModule->OnMainFrameCreationFinished().RemoveAll(this);
	}
	LoreSourceControlProvider.UnregisterSourceControlStateChanged_Handle(SourceControlStateChangedHandle);
	ISourceControlModule::Get().UnregisterProviderChanged(SourceControlProviderChangedHandle);
	if (FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().OnWindowBeingDestroyed().Remove(WindowBeingDestroyedHandle);
	}
	if (GEditor)
	{
		if (UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
		{
			AssetEditorSubsystem->OnAssetOpenedInEditor().RemoveAll(this);
		}
	}
	if (const UToolMenus* ToolMenus = UToolMenus::TryGet())
	{
		ToolMenus->UnregisterOwner(this);
	}
#endif

	// Shut down the provider
	LoreSourceControlProvider.Close();

	// Unbind
	IModularFeatures::Get().UnregisterModularFeature("SourceControl", &LoreSourceControlProvider);
}

#if SOURCE_CONTROL_WITH_SLATE
void FLoreSourceControlModule::OnMainFrameCreationFinished(TSharedPtr<SWindow> InRootWindow, bool bIsNewProjectWindow)
{
	if (IMainFrameModule* MainFrameModule = FModuleManager::GetModulePtr<IMainFrameModule>("MainFrame"))
	{
		MainFrameModule->OnMainFrameCreationFinished().RemoveAll(this);
	}

	RegisterToolbarExtension();
}

void FLoreSourceControlModule::OnSourceControlProviderChanged(ISourceControlProvider& OldProvider, ISourceControlProvider& NewProvider)
{
	RefreshToolbarExtension();
}

void FLoreSourceControlModule::OnWindowBeingDestroyed(const SWindow& Window)
{
	if (!LoreSourceControlProvider.IsAvailable())
	{
		return;
	}

	// Matches the default title set in SourceControlWindows.cpp "SourceControl.ConfirmSubmit"
	static const FString SubmitDialogTitle = TEXT("Confirm Submit");
	if (Window.GetTitle().ToString() != SubmitDialogTitle)
	{
		return;
	}

	// Same broad scope as the dialog's own opening scan (Content + Config + the .uproject).
	const TSharedRef<FUpdateStatus> StatusOp = ISourceControlOperation::Create<FUpdateStatus>();
	LoreSourceControlProvider.Execute(StatusOp, TArray<FString>{ FPaths::ProjectDir() }, EConcurrency::Asynchronous);
}

void FLoreSourceControlModule::RefreshToolbarExtension() const
{
	if (UToolMenus* ToolMenus = UToolMenus::TryGet())
	{
		for (const FName& MenuName : RegisteredToolbarMenus)
		{
			ToolMenus->RefreshMenuWidget(MenuName);
		}
	}
}

void FLoreSourceControlModule::RegisterToolbarExtension()
{
	RegisterToolbarExtensionForMenu(TEXT("LevelEditor.StatusBar.ToolBar"));

	// Asset editors (Blueprint, Material, etc.) don't share the level editor's status bar and have no
	// fixed menu name to register against ahead of time - mirror the engine's own Source Control
	// widget (SStatusBar::RegisterSourceControlStatus) and extend each one as it opens instead.
	if (GEditor)
	{
		if (UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
		{
			AssetEditorSubsystem->OnAssetOpenedInEditor().AddRaw(this, &FLoreSourceControlModule::OnAssetEditorOpened);
		}
	}
}

void FLoreSourceControlModule::OnAssetEditorOpened(UObject* InAsset, IAssetEditorInstance* InInstance)
{
	if (!InInstance)
	{
		return;
	}

	// Every asset editor instance in the engine derives from FAssetEditorToolkit - IAssetEditorInstance
	// itself has no GetToolkitHost(), so this cast is required to reach it. Epic relies on the same
	// cast elsewhere (e.g. FBlueprintEditorUtils::MarkBlueprintAsModified).
	const FAssetEditorToolkit* Toolkit = static_cast<FAssetEditorToolkit*>(InInstance);
	if (!Toolkit->IsHosted())
	{
		return;
	}

	// World-centric editors (inline in the Level Editor) share its status bar, which is already
	// covered by RegisterToolbarExtension() above - only standalone editors get their own here.
	const FName StatusBarName = Toolkit->GetToolkitHost()->GetStatusBarName();
	if (StatusBarName.IsNone())
	{
		return;
	}

	// GetPlainNameString() (not ToString()) strips the FName's numeric instance suffix, matching what
	// SStatusBar::GetToolbarName() actually generates - e.g. StatusBarName "BlueprintEditor_3" still
	// yields toolbar menu "BlueprintEditor.ToolBar", shared per editor *type*, not per window.
	RegisterToolbarExtensionForMenu(FName(*(StatusBarName.GetPlainNameString() + TEXT(".ToolBar"))));
}

void FLoreSourceControlModule::RegisterToolbarExtensionForMenu(FName InMenuName)
{
	if (RegisteredToolbarMenus.Contains(InMenuName))
	{
		return;
	}

	FToolMenuOwnerScoped OwnerScoped(this);

	UToolMenu* Menu = UToolMenus::Get()->ExtendMenu(InMenuName);
	if (!Menu)
	{
		return;
	}

	RegisteredToolbarMenus.Add(InMenuName);

	FToolMenuSection& Section = Menu->FindOrAddSection("SourceControlActions");

	// Dynamic entry so this re-evaluates every time the toolbar regenerates - if Lore isn't the
	// active/available provider, skip adding the entry entirely (a Collapsed widget would still
	// reserve its slot and add a stray separator).
	Section.AddDynamicEntry("LoreBranchSwitcher", FNewToolMenuSectionDelegate::CreateLambda([this](FToolMenuSection& InSection)
	{
		// Not checking SCModule.IsEnabled() here - it's intentionally false for the entire duration
		// of the internal connect flow (FScopedDisableSourceControl), which would hide us right after
		// a successful reconnect. The active provider name alone already distinguishes "disabled"
		// (provider is reset to "None") from "connecting" (provider stays "Lore" throughout).
		const bool bLoreIsActiveProvider = ISourceControlModule::Get().GetProvider().GetName() == LoreSourceControlProvider.GetName();
		if (!bLoreIsActiveProvider || !LoreSourceControlProvider.IsAvailable())
		{
			return;
		}

		// Populate the cache once up front so the first click on the branch switcher isn't empty.
		if (LoreSourceControlProvider.GetCachedBranchCount() == -1)
		{
			LoreSourceControlProvider.RefreshBranchesAsync();
		}

		InSection.AddEntry(FToolMenuEntry::InitWidget(
			"LoreBranchSwitcherWidget",
			SNew(SComboButton)
			.ComboButtonStyle(&FAppStyle::Get().GetWidgetStyle<FComboButtonStyle>("SimpleComboButton"))
			.ContentPadding(FMargin(4.f, 0.f))
			// Default MenuPlacement_ComboBox forces the dropdown's width to match this (narrow)
			// button's width, squeezing/centering wider menu content instead of sizing it naturally.
			.MenuPlacement(MenuPlacement_BelowAnchor)
			.OnGetMenuContent(FOnGetContent::CreateRaw(this, &FLoreSourceControlModule::GenerateBranchMenu))
			.ToolTipText(LOCTEXT("BranchSwitcherTooltip", "Lore actions"))
			.ButtonContent()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0.f, 0.f, 4.f, 0.f)
				[
					SNew(SImage)
					.Image(FRevisionControlStyleManager::Get().GetBrush("RevisionControl.Icon"))
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text_Lambda([this]() { return FText::FromString(LoreSourceControlProvider.GetBranchName()); })
				]
			],
			FText::GetEmpty(),
			true
		));
	}));
}

TSharedRef<SWidget> FLoreSourceControlModule::GenerateBranchMenu()
{
	if (!LoreSourceControlProvider.IsAvailable())
	{
		return SNew(STextBlock).Text(LOCTEXT("BranchSwitcherUnavailable", "Lore is not available"));
	}

	TArray<FLoreBranchInfo> Branches = LoreSourceControlProvider.GetCachedBranches();

	// Refresh in the background for next time - shelling out to lore.exe synchronously here would
	// hang the menu open animation for however long that takes.
	LoreSourceControlProvider.RefreshBranchesAsync();

	FMenuBuilder MenuBuilder(true, nullptr);

	MenuBuilder.BeginSection("LoreActions", LOCTEXT("BranchSwitcherMenuActions", "Actions"));
	MenuBuilder.AddWidget(
		SNew(SButton)
		.ButtonStyle(FAppStyle::Get(), "Menu.Button")
		.ContentPadding(FMargin(12.f, 4.f, 8.f, 4.f))
		.HAlign(HAlign_Fill)
		.OnClicked_Lambda([this]() { OnSyncClicked(); return FReply::Handled(); })
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.f, 0.f, 6.f, 0.f)
			[
				SNew(SImage)
				.Image(FRevisionControlStyleManager::Get().GetBrush("RevisionControl.Actions.Sync"))
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("SyncAction", "Sync"))
			]
		],
		FText::GetEmpty(),
		true,
		true,
		LOCTEXT("SyncAction_Tooltip", "Pull latest from Lore")
	);
	MenuBuilder.EndSection();

	MenuBuilder.BeginSection("LoreBranches", LOCTEXT("BranchSwitcherMenuHeading", "Branches"));
	if (Branches.IsEmpty())
	{
		MenuBuilder.AddMenuEntry(LOCTEXT("NoBranchesFound", "No branches found"), FText::GetEmpty(), FSlateIcon(), FUIAction());
	}
	else
	{
		for (const FLoreBranchInfo& Branch : Branches)
		{
			const bool bIsCurrent = Branch.bIsCurrent;
			FUIAction Action(
				FExecuteAction::CreateRaw(this, &FLoreSourceControlModule::OnBranchSelected, Branch.Name),
				FCanExecuteAction(),
				FIsActionChecked::CreateLambda([bIsCurrent]() { return bIsCurrent; })
			);

			MenuBuilder.AddMenuEntry(
				FText::FromString(Branch.Name),
				FText::GetEmpty(),
				FSlateIcon(),
				Action,
				NAME_None,
				EUserInterfaceActionType::RadioButton
			);
		}
	}
	MenuBuilder.EndSection();

	// Without a height cap, SScrollBox sizes to its unconstrained content height and never actually
	// scrolls - with enough branches the dropdown would just grow off the bottom of the screen.
	return SNew(SBox)
		.MaxDesiredHeight(300.f)
		[
			SNew(SScrollBox)
			+ SScrollBox::Slot()
			[
				MenuBuilder.MakeWidget()
			]
		];
}

void FLoreSourceControlModule::OnBranchSelected(FString InBranchName)
{
	if (InBranchName == LoreSourceControlProvider.GetBranchName())
	{
		return;
	}

	TArray<FString> ChangedContentPaths;
	const bool bRequiresRestart = LoreSourceControlProvider.DoesBranchSwitchTouchCode(InBranchName, ChangedContentPaths);

	const FText ConfirmText = bRequiresRestart
		? FText::Format(LOCTEXT("SwitchBranchConfirmRestart", "Switch to branch '{0}'?\n\nThis branch has Source/Config changes, so restart the editor after switching to pick up the new code. Close any open assets first."), FText::FromString(InBranchName))
		: FText::Format(LOCTEXT("SwitchBranchConfirmContent", "Switch to branch '{0}'?\n\nThis only changes Content - affected assets will be reloaded automatically. Save any unsaved work first."), FText::FromString(InBranchName));

	if (FMessageDialog::Open(EAppMsgType::YesNo, ConfirmText) != EAppReturnType::Yes)
	{
		return;
	}

	switch (LoreSourceControlProvider.SwitchBranch(InBranchName))
	{
	case FLoreSourceControlProvider::ELoreSwitchResult::Success:
		LoreSourceControlProvider.RefreshBranchesAsync();
		if (bRequiresRestart)
		{
			FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("SwitchBranchRestart", "Branch switched. Please restart the editor now to pick up the new code."));
		}
		else
		{
			LoreSourceControlProvider.ReloadContentPackages(ChangedContentPaths);
		}
		break;

	case FLoreSourceControlProvider::ELoreSwitchResult::StagedStateBlocked:
		FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("SwitchBranchStaged", "Can't switch branch: you have a staged change on the current branch that hasn't been pushed yet.\n\nPush or revert it, then try again."));
		break;

	case FLoreSourceControlProvider::ELoreSwitchResult::Failed:
	default:
		FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("SwitchBranchFailed", "Failed to switch branch. See the Source Control message log for details."));
		break;
	}
}

void FLoreSourceControlModule::OnSyncClicked()
{
	// Queued asynchronously - FLoreSyncWorker::UpdateStates() (called from FLoreSourceControlProvider::Tick()
	// once this completes) handles the Content auto-reload / restart prompt, same as branch switch does.
	const TSharedRef<FSync> SyncOp = ISourceControlOperation::Create<FSync>();
	LoreSourceControlProvider.Execute(SyncOp, EConcurrency::Asynchronous);
}

#endif

void FLoreSourceControlModule::SaveSettings() const
{
	if (FApp::IsUnattended() || IsRunningCommandlet())
	{
		return;
	}

	LoreSourceControlProvider.SaveSettings();
}

IMPLEMENT_MODULE(FLoreSourceControlModule, LoreSourceControl);

#undef LOCTEXT_NAMESPACE
