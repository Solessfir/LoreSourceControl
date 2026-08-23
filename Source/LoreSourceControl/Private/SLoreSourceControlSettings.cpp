// Copyright Solessfir 2026. All Rights Reserved.

#include "SLoreSourceControlSettings.h"
#include "LoreSourceControlProvider.h"
#include "LoreSourceControlUtils.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SFilePathPicker.h"
#include "EditorDirectories.h"
#include "Styling/AppStyle.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Images/SImage.h"
#include "Framework/Application/SlateApplication.h"

#define LOCTEXT_NAMESPACE "SLoreSourceControlSettings"

void SLoreSourceControlSettings::Construct(const FArguments& InArgs)
{
	Provider = InArgs._Provider;
	check(Provider);
	const FText FileFilterType = LOCTEXT("Executables", "Executables");
#if PLATFORM_WINDOWS
	const FString FileFilterText = FString::Printf(TEXT("%s (*.exe)|*.exe"), *FileFilterType.ToString());
#else
	const FString FileFilterText = FileFilterType.ToString();
#endif

	ChildSlot
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(2.f)
		[
			SNew(SBox)
			.Visibility(this, &SLoreSourceControlSettings::GetLoreVersionVisibility)
			[
				MakeInfoRow(
					LOCTEXT("VersionLabel", "Lore Version"),
					TAttribute<FText>(this, &SLoreSourceControlSettings::GetLoreVersionText),
					LOCTEXT("VersionLabel_Tooltip", "Version reported by the selected Lore executable"))
			]
		]

		// Connection info: server, user, branch - only meaningful once a repository is found
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SVerticalBox)
			.Visibility(this, &SLoreSourceControlSettings::GetConnectionInfoVisibility)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(2.f)
			[
				MakeInfoRow(
					LOCTEXT("ServerLabel", "Server"),
					TAttribute<FText>(this, &SLoreSourceControlSettings::GetServerText),
					LOCTEXT("ServerLabel_Tooltip", "Remote server this repository pushes to and clones from (.lore/config.toml)"))
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(2.f)
			[
				MakeInfoRow(
					LOCTEXT("UserLabel", "User"),
					TAttribute<FText>(this, &SLoreSourceControlSettings::GetUserText),
					LOCTEXT("UserLabel_Tooltip", "Commit identity configured for this repository (.lore/config.toml)"))
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(2.f)
			[
				MakeInfoRow(
					LOCTEXT("BranchLabel", "Branch"),
					TAttribute<FText>(this, &SLoreSourceControlSettings::GetBranchText),
					LOCTEXT("BranchLabel_Tooltip", "Currently checked out branch"))
			]
		]

		// Lore binary path
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(2.f)
		.VAlign(VAlign_Center)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1.f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("BinaryPathLabel", "Lore Path"))
				.ToolTipText(LOCTEXT("BinaryPathLabel_Tooltip", "Path to the lore executable."))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(2.f)
			[
				SNew(SFilePathPicker)
				.BrowseButtonImage(FAppStyle::GetBrush("PropertyWindow.Button_Ellipsis"))
				.BrowseButtonStyle(FAppStyle::Get(), "HoverHintOnly")
				.BrowseButtonToolTip(LOCTEXT("BinaryPathLabel_Tooltip", "Path to the lore executable."))
				.BrowseDirectory(FEditorDirectories::Get().GetLastDirectory(ELastDirectory::GENERIC_OPEN))
				.BrowseTitle(LOCTEXT("BinaryPathBrowseTitle", "Select lore executable..."))
				.FilePath(this, &SLoreSourceControlSettings::GetBinaryPathString)
				.FileTypeFilter(FileFilterText)
				.OnPathPicked(this, &SLoreSourceControlSettings::OnBinaryPathPicked)
			]
		]

		// Warning area if not valid
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(2.f, 6.f)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("RoundedWarning"))
			.Padding(FMargin(8.f, 6.f))
			.Visibility(this, &SLoreSourceControlSettings::GetWarningVisibility)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Top)
				.Padding(0.f, 1.f, 8.f, 0.f)
				[
					SNew(SImage)
					.Image(FAppStyle::GetBrush("Icons.Warning.Solid"))
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1.f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(this, &SLoreSourceControlSettings::GetWarningText)
					.AutoWrapText(true)
				]
			]
		]
	];
}

TSharedRef<SWidget> SLoreSourceControlSettings::MakeInfoRow(const FText& Label, const TAttribute<FText>& Value, const FText& Tooltip)
{
	return SNew(SHorizontalBox)
		.ToolTipText(Tooltip)
		+ SHorizontalBox::Slot()
		.FillWidth(1.f)
		[
			SNew(STextBlock)
			.Text(Label)
		]
		+ SHorizontalBox::Slot()
		.FillWidth(2.f)
		[
			SNew(STextBlock)
			.Text(Value)
		];
}

FString SLoreSourceControlSettings::GetBinaryPathString() const
{
	return FLoreSourceControlUtils::GetUserConfiguredLoreBinaryPath();
}

void SLoreSourceControlSettings::OnBinaryPathPicked(const FString& PickedPath) const
{
	FLoreSourceControlUtils::SetUserConfiguredLoreBinaryPath(PickedPath);

	// Notify the provider so it can re-check availability
	Provider->SetLoreBinaryPath(PickedPath);
	Provider->CheckLoreAvailability();
	Provider->CheckRepositoryStatus();
}

bool SLoreSourceControlSettings::IsLoreBinaryValid() const
{
	return Provider->IsLoreBinaryAvailable();
}

bool SLoreSourceControlSettings::IsLoreRepositoryValid() const
{
	return Provider->IsLoreRepositoryFound();
}

EVisibility SLoreSourceControlSettings::GetLoreVersionVisibility() const
{
	return IsLoreBinaryValid() ? EVisibility::Visible : EVisibility::Collapsed;
}

FText SLoreSourceControlSettings::GetLoreVersionText() const
{
	return FText::FromString(Provider->GetLoreVersion());
}

EVisibility SLoreSourceControlSettings::GetWarningVisibility() const
{
	return IsLoreBinaryValid() && IsLoreRepositoryValid() && Provider->IsLoreVersionTested() ? EVisibility::Collapsed : EVisibility::Visible;
}

FText SLoreSourceControlSettings::GetWarningText() const
{
	if (IsLoreBinaryValid())
	{
		if (!IsLoreRepositoryValid())
		{
			return LOCTEXT("LoreRepositoryNotFoundWarning", "Lore is installed, but this project is not inside a Lore repository. A .lore directory must exist in the project directory or one of its parents.");
		}
		return FText::Format(LOCTEXT("LoreVersionUntestedWarning", "Lore {0} has not been tested with this plugin. It will remain enabled, but check the Source Control message log if an operation behaves unexpectedly."), FText::FromString(Provider->GetLoreVersion()));
	}

	FString Path = Provider->GetLoreBinaryPath();
	if (Path.IsEmpty())
	{
		Path = TEXT("<none>");
	}

#if PLATFORM_WINDOWS
	static const TCHAR* DefaultLocationText = TEXT("C:\\Program Files\\lore");
#elif PLATFORM_MAC
	static const TCHAR* DefaultLocationText = TEXT("/usr/local/bin or /opt/homebrew/bin");
#else
	static const TCHAR* DefaultLocationText = TEXT("/usr/local/bin or /usr/bin");
#endif

	return FText::Format(
		LOCTEXT("LoreNotFoundWarning",
			"Could not find or execute 'lore' at:\n{0}\n\n"
			"lore is typically installed to {1}, or available on PATH."),
		FText::FromString(Path),
		FText::FromString(DefaultLocationText)
	);
}

EVisibility SLoreSourceControlSettings::GetConnectionInfoVisibility() const
{
	return Provider->IsAvailable() ? EVisibility::Visible : EVisibility::Collapsed;
}

FText SLoreSourceControlSettings::GetServerText() const
{
	const FString Server = Provider->GetRemoteUrl();
	return FText::FromString(Server.IsEmpty() ? TEXT("(offline, no remote configured)") : Server);
}

FText SLoreSourceControlSettings::GetUserText() const
{
	const FString User = Provider->GetIdentity();
	return FText::FromString(User.IsEmpty() ? TEXT("(not configured)") : User);
}

FText SLoreSourceControlSettings::GetBranchText() const
{
	const FString Branch = Provider->GetBranchName();
	return FText::FromString(Branch.IsEmpty() ? TEXT("(unknown)") : Branch);
}

#undef LOCTEXT_NAMESPACE
