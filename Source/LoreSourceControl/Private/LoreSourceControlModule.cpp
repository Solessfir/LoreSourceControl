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
#if SOURCE_CONTROL_WITH_SLATE
#include "SourceControlWindows.h"
#include "ToolMenus.h"
#include "Misc/MessageDialog.h"
#include "Logging/MessageLog.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/SWindow.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/SBoxPanel.h"
#include "Framework/Commands/Commands.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "RevisionControlStyle/RevisionControlStyle.h"
#include "Styling/AppStyle.h"
#include "Styling/SlateStyle.h"
#include "Styling/SlateTypes.h"
#include "Interfaces/IMainFrameModule.h"
#include "Editor.h"
#include "FileHelpers.h"
#include "Subsystems/AssetEditorSubsystem.h"
#endif

#define LOCTEXT_NAMESPACE "LoreSourceControl"

#if SOURCE_CONTROL_WITH_SLATE
static TWeakPtr<SNotificationItem> ShowLoreProgressNotification(const FText& Text)
{
	FNotificationInfo Info(Text);
	Info.bFireAndForget = false;
	Info.ExpireDuration = 5.f;
	Info.HyperlinkText = LOCTEXT("ShowSourceControlLog", "Show Message Log");
	Info.Hyperlink = FSimpleDelegate::CreateStatic([]() { FMessageLog("SourceControl").Open(EMessageSeverity::Info, true); });
	const TSharedPtr<SNotificationItem> Notification = FSlateNotificationManager::Get().AddNotification(Info);
	if (Notification)
	{
		Notification->SetCompletionState(SNotificationItem::CS_Pending);
	}
	return Notification;
}

static void CompleteLoreProgressNotification(TWeakPtr<SNotificationItem>& Notification, const FText& Text, SNotificationItem::ECompletionState CompletionState)
{
	if (const TSharedPtr<SNotificationItem> Item = Notification.Pin())
	{
		Item->SetText(Text);
		Item->SetCompletionState(CompletionState);
		Item->ExpireAndFadeout();
	}
	Notification.Reset();
}

class FLoreSourceControlCommands final : public TCommands<FLoreSourceControlCommands>
{
public:
	FLoreSourceControlCommands()
		: TCommands("LoreSourceControl", LOCTEXT("LoreSourceControlCommands", "Lore Source Control"), NAME_None, FAppStyle::GetAppStyleSetName())
	{
	}

	virtual void RegisterCommands() override
	{
		UI_COMMAND(Sync, "Sync", "Pull latest from Lore", EUserInterfaceActionType::Check, FInputChord());
	}

	TSharedPtr<FUICommandInfo> Sync;
};
#endif

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
	LoreSourceControlProvider.RegisterWorker("LorePrintStatus", FLoreGetSourceControlWorker::CreateStatic(&CreateLoreWorker<FLorePrintStatusWorker>));
	LoreSourceControlProvider.RegisterWorker("LoreRefreshBranches", FLoreGetSourceControlWorker::CreateStatic(&CreateLoreWorker<FLoreRefreshBranchesWorker>));
	LoreSourceControlProvider.RegisterWorker("LoreSwitchBranch", FLoreGetSourceControlWorker::CreateStatic(&CreateLoreWorker<FLoreSwitchBranchWorker>));

	// Load settings (binary path, etc.)
	LoreSourceControlProvider.LoadSettings();

	// Bind our source control provider to the editor
	IModularFeatures::Get().RegisterModularFeature("SourceControl", &LoreSourceControlProvider);

	// Console commands for quick access inside editor (main goal: no need to close editor)
	LoreSyncCommand = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("LoreSync"),
		TEXT("Perform a Lore sync (pull) and update source control states."),
		FConsoleCommandDelegate::CreateLambda([this]()
		{
			if (ISourceControlModule::Get().GetProvider().GetName() == LoreSourceControlProvider.GetName())
			{
				const TSharedRef<FSync> SyncOp = ISourceControlOperation::Create<FSync>();
				LoreSourceControlProvider.Execute(SyncOp, EConcurrency::Asynchronous);
			}
		}),
		ECVF_Default
	);

	LoreStatusCommand = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("LoreStatus"),
		TEXT("Force update Lore source control status for the project and print it in human-readable form."),
		FConsoleCommandDelegate::CreateLambda([this]()
		{
			if (ISourceControlModule::Get().GetProvider().GetName() != LoreSourceControlProvider.GetName())
			{
				return;
			}

			// Refresh the editor's own state cache (Content Browser icons, etc.) - this leg always needs --json, same as every other internal caller.
			const TSharedRef<FUpdateStatus> StatusOp = ISourceControlOperation::Create<FUpdateStatus>();
			LoreSourceControlProvider.Execute(StatusOp, TArray<FString>{ FPaths::ProjectDir() }, EConcurrency::Asynchronous);

			// FIFO command dispatch guarantees the readable output is generated after the cache refresh.
			LoreSourceControlProvider.Execute(ISourceControlOperation::Create<FLorePrintStatusOperation>(), EConcurrency::Asynchronous);
		}),
		ECVF_Default
	);

	LoreCommitCommand = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("LoreCommit"),
		TEXT("Open the Submit Files dialog to commit and push pending changes via Lore."),
		FConsoleCommandDelegate::CreateLambda([this]()
		{
#if SOURCE_CONTROL_WITH_SLATE
			if (ISourceControlModule::Get().GetProvider().GetName() == LoreSourceControlProvider.GetName())
			{
				// Same dialog as the toolbar's "Submit Content" - drives our CheckIn worker (commit + push).
				FSourceControlWindows::ChoosePackagesToCheckIn();
			}
#endif
		}),
		ECVF_Default
	);

#if SOURCE_CONTROL_WITH_SLATE
	// Headless commandlets (cook, UAT, etc.) load this module too but never initialize Slate.
	// Skip all UI registration in that case because MainFrame, toolbar, and window delegates require a live Slate app.
	if (FSlateApplication::IsInitialized())
	{
		FLoreSourceControlCommands::Register();

		// The engine's own Revision Control widget registers inside SStatusBar::Construct(), which only runs once the level editor's main tab is built - well after this module loads.
		// Registering via UToolMenus::RegisterStartupCallback (module load time) would be too early for that widget to exist yet, so wait for the main frame instead.
		IMainFrameModule& MainFrameModule = FModuleManager::LoadModuleChecked<IMainFrameModule>("MainFrame");
		if (MainFrameModule.IsWindowInitialized())
		{
			RegisterToolbarExtension();
		}
		else
		{
			MainFrameModule.OnMainFrameCreationFinished().AddRaw(this, &FLoreSourceControlModule::OnMainFrameCreationFinished);
		}

		// The branch switcher's entry only appears once Lore is connected (see RegisterToolbarExtension).
		// Refresh the toolbar whenever that state changes so it appears or disappears without an editor restart.
		SourceControlStateChangedHandle = LoreSourceControlProvider.RegisterSourceControlStateChanged_Handle(FSourceControlStateChanged::FDelegate::CreateRaw(this, &FLoreSourceControlModule::RefreshToolbarExtension));

		// Toggling "Enable Source Control" (or switching providers) in Editor Preferences doesn't touch our own provider's state at all, so it needs its own refresh trigger.
		SourceControlProviderChangedHandle = ISourceControlModule::Get().RegisterProviderChanged(FSourceControlProviderChanged::FDelegate::CreateRaw(this, &FLoreSourceControlModule::OnSourceControlProviderChanged));

		// See OnWindowBeingDestroyed()'s comment - catches the Submit dialog closing (accept or cancel).
		WindowBeingDestroyedHandle = FSlateApplication::Get().OnWindowBeingDestroyed().AddRaw(this, &FLoreSourceControlModule::OnWindowBeingDestroyed);
	}
#endif
}

void FLoreSourceControlModule::ShutdownModule()
{
	IConsoleManager& ConsoleManager = IConsoleManager::Get();
	if (LoreSyncCommand)
	{
		ConsoleManager.UnregisterConsoleObject(LoreSyncCommand, false);
		LoreSyncCommand = nullptr;
	}

	if (LoreStatusCommand)
	{
		ConsoleManager.UnregisterConsoleObject(LoreStatusCommand, false);
		LoreStatusCommand = nullptr;
	}

	if (LoreCommitCommand)
	{
		ConsoleManager.UnregisterConsoleObject(LoreCommitCommand, false);
		LoreCommitCommand = nullptr;
	}

#if SOURCE_CONTROL_WITH_SLATE
	if (IMainFrameModule* MainFrameModule = FModuleManager::GetModulePtr<IMainFrameModule>("MainFrame"))
	{
		MainFrameModule->OnMainFrameCreationFinished().RemoveAll(this);
	}

	LoreSourceControlProvider.UnregisterSourceControlStateChanged_Handle(SourceControlStateChangedHandle);
	ISourceControlModule::Get().UnregisterProviderChanged(SourceControlProviderChangedHandle);

	if (FSlateApplication::IsInitialized())
	{
		if (const TSharedPtr<SNotificationItem> Notification = SyncNotification.Pin())
		{
			Notification->ExpireAndFadeout();
		}
		if (const TSharedPtr<SNotificationItem> Notification = BranchSwitchNotification.Pin())
		{
			Notification->ExpireAndFadeout();
		}
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

	if (FLoreSourceControlCommands::IsRegistered())
	{
		FLoreSourceControlCommands::Unregister();
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

	// Matches the localized default title set in SourceControlWindows.cpp.
	static const FText SubmitDialogTitle = NSLOCTEXT("SourceControl.ConfirmSubmit", "Title", "Confirm Submit");
	if (!Window.GetTitle().EqualTo(SubmitDialogTitle))
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

	// Asset editors (Blueprint, Material, etc.) don't share the level editor's status bar and have no fixed menu name to register against ahead of time.
	// Mirror the engine's own Source Control widget (SStatusBar::RegisterSourceControlStatus) and extend each asset editor as it opens instead.
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

	// GetEditorName is part of IAssetEditorInstance and works for toolkit-based editors, UAssetEditor, and other implementations.
	// The status bar strips FName instance suffixes the same way.
	const FName EditorName = InInstance->GetEditorName();
	if (EditorName.IsNone())
	{
		return;
	}

	RegisterToolbarExtensionForMenu(FName(*(EditorName.GetPlainNameString() + TEXT(".ToolBar"))));
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

	// This dynamic entry is reevaluated every time the toolbar regenerates.
	// If Lore isn't the active and available provider, skip the entry entirely because a Collapsed widget would still reserve its slot and add a stray separator.
	Section.AddDynamicEntry("LoreBranchSwitcher", FNewToolMenuSectionDelegate::CreateLambda([this](FToolMenuSection& InSection)
	{
		// Do not check SCModule.IsEnabled() here.
		// It is intentionally false throughout the internal connect flow (FScopedDisableSourceControl), which would hide us right after a successful reconnect.
		// The active provider name alone already distinguishes "disabled" (provider is reset to "None") from "connecting" (provider stays "Lore" throughout).
		const bool bLoreIsActiveProvider = ISourceControlModule::Get().GetProvider().GetName() == LoreSourceControlProvider.GetName();
		if (!bLoreIsActiveProvider || !LoreSourceControlProvider.IsAvailable())
		{
			return;
		}

		// Populate the cache once up front so the first click on the branch switcher can show a loading state immediately.
		if (LoreSourceControlProvider.GetBranchCacheState() == ELoreBranchCacheState::NotLoaded)
		{
			LoreSourceControlProvider.RefreshBranchesAsync();
		}

		InSection.AddEntry(FToolMenuEntry::InitWidget(
			"LoreBranchSwitcherWidget",
			SNew(SComboButton)
			.ComboButtonStyle(&FAppStyle::Get().GetWidgetStyle<FComboButtonStyle>("SimpleComboButton"))
			.ContentPadding(FMargin(4.f, 0.f))
			// Default MenuPlacement_ComboBox forces the dropdown's width to match this (narrow) button's width, squeezing/centering wider menu content instead of sizing it naturally.
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
	ELoreBranchCacheState BranchState = LoreSourceControlProvider.GetBranchCacheState();

	if (BranchState == ELoreBranchCacheState::NotLoaded)
	{
		LoreSourceControlProvider.RefreshBranchesAsync();
		BranchState = ELoreBranchCacheState::Loading;
	}
	else if (BranchState == ELoreBranchCacheState::Loaded)
	{
		// Keep showing the last good list while refreshing it for the next open.
		LoreSourceControlProvider.RefreshBranchesAsync();
	}

	if (!BranchMenuStyle)
	{
		BranchMenuStyle = MakeShared<FSlateStyleSet>("LoreSourceControl.BranchMenu");
		BranchMenuStyle->SetParentStyleName(FAppStyle::GetAppStyleSetName());
		const FSlateBrush& SyncBrush = *FRevisionControlStyleManager::Get().GetBrush("RevisionControl.Actions.Sync");
		BranchMenuStyle->Set("LoreSourceControl.SyncAction", FCheckBoxStyle().SetUncheckedImage(SyncBrush).SetUncheckedHoveredImage(SyncBrush));
	}

	const TSharedRef<FUICommandList> BranchMenuCommandList = MakeShared<FUICommandList>();
	BranchMenuCommandList->MapAction(FLoreSourceControlCommands::Get().Sync, FExecuteAction::CreateRaw(this, &FLoreSourceControlModule::OnSyncClicked), FCanExecuteAction::CreateLambda([this]() { return !IsToolbarOperationInProgress(); }));
	FMenuBuilder MenuBuilder(true, BranchMenuCommandList, TSharedPtr<FExtender>(), false, BranchMenuStyle.Get());

	MenuBuilder.BeginSection("LoreActions", LOCTEXT("BranchSwitcherMenuActions", "Actions"));
	MenuBuilder.SetCheckBoxStyle("LoreSourceControl.SyncAction");
	MenuBuilder.AddMenuEntry(FLoreSourceControlCommands::Get().Sync);
	MenuBuilder.SetCheckBoxStyle(NAME_None);
	if (BranchState == ELoreBranchCacheState::Failed)
	{
		MenuBuilder.AddMenuEntry(
			LOCTEXT("RetryBranchRefresh", "Retry Branch Refresh"),
			LOCTEXT("RetryBranchRefreshTooltip", "Try to load the Lore branch list again"),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([this]() { LoreSourceControlProvider.RefreshBranchesAsync(); })));
	}
	MenuBuilder.EndSection();

	MenuBuilder.BeginSection("LoreBranches", LOCTEXT("BranchSwitcherMenuHeading", "Branches"));
	if (!Branches.IsEmpty() && BranchState == ELoreBranchCacheState::Failed)
	{
		MenuBuilder.AddMenuEntry(LOCTEXT("BranchesStale", "Refresh failed; showing cached branches"), FText::GetEmpty(), FSlateIcon(), FUIAction());
	}
	if (Branches.IsEmpty())
	{
		FText EmptyStateText = LOCTEXT("NoBranchesFound", "No branches found");
		if (BranchState == ELoreBranchCacheState::Loading)
		{
			EmptyStateText = LOCTEXT("BranchesLoading", "Loading branches...");
		}
		else if (BranchState == ELoreBranchCacheState::Failed)
		{
			EmptyStateText = LOCTEXT("BranchesFailed", "Branches could not be loaded");
		}
		MenuBuilder.AddMenuEntry(EmptyStateText, FText::GetEmpty(), FSlateIcon(), FUIAction());
	}
	else
	{
		for (const FLoreBranchInfo& Branch : Branches)
		{
			const bool bIsCurrent = Branch.bIsCurrent;
			FUIAction Action(
				FExecuteAction::CreateRaw(this, &FLoreSourceControlModule::OnBranchSelected, Branch.Name),
				FCanExecuteAction::CreateLambda([this, bIsCurrent]() { return !bIsCurrent && !IsToolbarOperationInProgress(); }),
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

	// Without a height cap, SScrollBox sizes to its unconstrained content height and never actually scrolls.
	// With enough branches, the dropdown would grow off the bottom of the screen.
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
	if (IsToolbarOperationInProgress() || InBranchName == LoreSourceControlProvider.GetBranchName())
	{
		return;
	}

	const FText ConfirmText = FText::Format(
		LOCTEXT("SwitchBranchConfirm", "Switch to branch '{0}'?\n\nFiles in the working copy may change. Unreal will ask you to save unsaved maps and assets before continuing."),
		FText::FromString(InBranchName));

	if (FMessageDialog::Open(EAppMsgType::YesNo, ConfirmText) != EAppReturnType::Yes)
	{
		return;
	}
	if (!FEditorFileUtils::SaveDirtyPackages(true, true, true, false, false, false))
	{
		return;
	}

	bBranchSwitchInProgress = true;
	BranchSwitchNotification = ShowLoreProgressNotification(FText::Format(LOCTEXT("SwitchBranchInProgress", "Switching to branch '{0}'..."), FText::FromString(InBranchName)));
	RefreshToolbarExtension();

	const TSharedRef<FLoreSwitchBranchOperation> Operation = ISourceControlOperation::Create<FLoreSwitchBranchOperation>(MoveTemp(InBranchName));
	LoreSourceControlProvider.Execute(
		Operation,
		TArray<FString>(),
		EConcurrency::Asynchronous,
		FSourceControlOperationComplete::CreateRaw(this, &FLoreSourceControlModule::OnBranchSwitchComplete));
}

void FLoreSourceControlModule::OnBranchSwitchComplete(const FSourceControlOperationRef& InOperation, ECommandResult::Type InResult)
{
	const TSharedRef<FLoreSwitchBranchOperation> Operation = StaticCastSharedRef<FLoreSwitchBranchOperation>(InOperation);
	bBranchSwitchInProgress = false;
	RefreshToolbarExtension();
	if (InResult == ECommandResult::Succeeded)
	{
		CompleteLoreProgressNotification(BranchSwitchNotification, FText::Format(LOCTEXT("SwitchBranchSucceeded", "Switched to branch '{0}'."), FText::FromString(Operation->GetBranchName())), SNotificationItem::CS_Success);
		LoreSourceControlProvider.RefreshBranchesAsync();
		return;
	}
	CompleteLoreProgressNotification(BranchSwitchNotification, LOCTEXT("SwitchBranchNotificationFailed", "Failed to switch Lore branch."), SNotificationItem::CS_Fail);

	switch (Operation->GetOutcome())
	{
	case FLoreSwitchBranchOperation::EOutcome::StagedStateBlocked:
		FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("SwitchBranchStaged", "Can't switch branch: you have a staged change on the current branch that hasn't been pushed yet.\n\nPush or revert it, then try again."));
		break;

	case FLoreSwitchBranchOperation::EOutcome::Failed:
	default:
		FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("SwitchBranchFailed", "Failed to switch branch. See the Source Control message log for details."));
		break;
	}
}

void FLoreSourceControlModule::OnSyncClicked()
{
	if (IsToolbarOperationInProgress())
	{
		return;
	}

	bSyncInProgress = true;
	SyncNotification = ShowLoreProgressNotification(LOCTEXT("SyncInProgress", "Syncing Lore workspace..."));
	RefreshToolbarExtension();

	// Queue this asynchronously.
	// FLoreSyncWorker::UpdateStates() is called from FLoreSourceControlProvider::Tick() when the operation completes and handles the Content auto-reload or restart prompt, as it does for branch switches.
	const TSharedRef<FSync> SyncOp = ISourceControlOperation::Create<FSync>();
	LoreSourceControlProvider.Execute(SyncOp, EConcurrency::Asynchronous, FSourceControlOperationComplete::CreateRaw(this, &FLoreSourceControlModule::OnSyncComplete));
}

void FLoreSourceControlModule::OnSyncComplete(const FSourceControlOperationRef& InOperation, ECommandResult::Type InResult)
{
	bSyncInProgress = false;
	RefreshToolbarExtension();
	if (InResult == ECommandResult::Succeeded)
	{
		CompleteLoreProgressNotification(SyncNotification, LOCTEXT("SyncSucceeded", "Lore sync complete."), SNotificationItem::CS_Success);
	}
	else
	{
		CompleteLoreProgressNotification(SyncNotification, LOCTEXT("SyncFailed", "Lore sync failed. See the Source Control message log for details."), SNotificationItem::CS_Fail);
	}
}

bool FLoreSourceControlModule::IsToolbarOperationInProgress() const
{
	return bSyncInProgress || bBranchSwitchInProgress;
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
