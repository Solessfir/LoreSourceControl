// Copyright Solessfir 2026. All Rights Reserved.

#include "SLoreBranchHistory.h"
#include "LoreSourceControlOperations.h"
#include "LoreSourceControlProvider.h"
#include "LoreSourceControlUtils.h"
#include "Input/Reply.h"
#include "Styling/AppStyle.h"
#include "Widgets/Images/SThrobber.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SOverlay.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SHeaderRow.h"
#include "Widgets/Views/SListView.h"

#define LOCTEXT_NAMESPACE "LoreSourceControl"

namespace
{
	const FName RevisionColumn(TEXT("Revision"));
	const FName DescriptionColumn(TEXT("Description"));
	const FName AuthorColumn(TEXT("Author"));
	const FName DateColumn(TEXT("Date"));
	const FName HashColumn(TEXT("Hash"));

	class SLoreBranchHistoryRow final : public SMultiColumnTableRow<FLoreBranchHistoryItem>
	{
	public:
		SLATE_BEGIN_ARGS(SLoreBranchHistoryRow) {}
			SLATE_ARGUMENT(FLoreBranchHistoryItem, Item)
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& InOwnerTable)
		{
			Item = InArgs._Item;
			SMultiColumnTableRow<FLoreBranchHistoryItem>::Construct(
				FSuperRowType::FArguments().Padding(FMargin(4.f, 2.f)),
				InOwnerTable);
		}

		virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& InColumnName) override
		{
			FString Value;
			if (InColumnName == RevisionColumn)
			{
				Value = FString::Printf(TEXT("%d"), Item->RevisionNumber);
			}
			else if (InColumnName == DescriptionColumn)
			{
				Value = Item->Description;
			}
			else if (InColumnName == AuthorColumn)
			{
				Value = Item->Author;
			}
			else if (InColumnName == DateColumn)
			{
				Value = Item->Date.GetTicks() > 0
					? Item->Date.ToString(TEXT("%Y-%m-%d %H:%M:%S UTC"))
					: FString();
			}
			else if (InColumnName == HashColumn)
			{
				Value = Item->RevisionHash.Left(12);
			}

			return SNew(STextBlock)
				.Text(FText::FromString(Value))
				.ToolTipText(FText::FromString(InColumnName == HashColumn ? Item->RevisionHash : Value));
		}

	private:
		FLoreBranchHistoryItem Item;
	};
}

void SLoreBranchHistory::Construct(const FArguments& InArgs, FLoreSourceControlProvider* InProvider)
{
	Provider = InProvider;

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(8.f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.f, 0.f, 0.f, 8.f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(this, &SLoreBranchHistory::GetHeadingText)
					.TextStyle(FAppStyle::Get(), "HeadingExtraSmall")
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("RefreshBranchHistory", "Refresh"))
					.ToolTipText(LOCTEXT("RefreshBranchHistoryTooltip", "Reload the current Lore branch history"))
					.IsEnabled_Lambda([this]() { return !bLoading; })
					.OnClicked(this, &SLoreBranchHistory::OnRefreshClicked)
				]
			]
			+ SVerticalBox::Slot()
			.FillHeight(1.f)
			[
				SNew(SOverlay)
				+ SOverlay::Slot()
				[
					SAssignNew(HistoryList, SListView<FLoreBranchHistoryItem>)
					.ListItemsSource(&HistoryItems)
					.OnGenerateRow(this, &SLoreBranchHistory::OnGenerateRow)
					.SelectionMode(ESelectionMode::Single)
					.HeaderRow
					(
						SNew(SHeaderRow)
						+ SHeaderRow::Column(RevisionColumn)
							.DefaultLabel(LOCTEXT("RevisionColumn", "Revision"))
							.FixedWidth(80.f)
						+ SHeaderRow::Column(DescriptionColumn)
							.DefaultLabel(LOCTEXT("DescriptionColumn", "Description"))
							.FillWidth(0.45f)
						+ SHeaderRow::Column(AuthorColumn)
							.DefaultLabel(LOCTEXT("AuthorColumn", "Author"))
							.FillWidth(0.2f)
						+ SHeaderRow::Column(DateColumn)
							.DefaultLabel(LOCTEXT("DateColumn", "Date"))
							.FillWidth(0.2f)
						+ SHeaderRow::Column(HashColumn)
							.DefaultLabel(LOCTEXT("HashColumn", "Hash"))
							.FillWidth(0.15f)
					)
				]
				+ SOverlay::Slot()
				.HAlign(HAlign_Center)
				.VAlign(VAlign_Center)
				[
					SNew(SThrobber)
					.Visibility(this, &SLoreBranchHistory::GetLoadingVisibility)
				]
				+ SOverlay::Slot()
				.HAlign(HAlign_Center)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(this, &SLoreBranchHistory::GetStatusText)
					.Visibility(this, &SLoreBranchHistory::GetStatusVisibility)
				]
			]
		]
	];

	Refresh();
}

FReply SLoreBranchHistory::OnRefreshClicked()
{
	Refresh();
	return FReply::Handled();
}

void SLoreBranchHistory::Refresh()
{
	if (bLoading)
	{
		return;
	}

	ErrorText = FText::GetEmpty();
	if (!Provider || !Provider->IsAvailable())
	{
		ErrorText = LOCTEXT("BranchHistoryUnavailable", "Lore is not available.");
		return;
	}

	BranchName = Provider->GetBranchName();
	bLoading = true;

	const TSharedRef<FLoreRefreshBranchHistoryOperation> Operation = ISourceControlOperation::Create<FLoreRefreshBranchHistoryOperation>(BranchName);
	const ECommandResult::Type StartResult = Provider->Execute(
		Operation,
		TArray<FString>(),
		EConcurrency::Asynchronous,
		FSourceControlOperationComplete::CreateSP(this, &SLoreBranchHistory::OnRefreshComplete));

	if (StartResult == ECommandResult::Failed)
	{
		bLoading = false;
		ErrorText = LOCTEXT("BranchHistoryStartFailed", "Could not start the Lore branch history query.");
	}
}

void SLoreBranchHistory::OnRefreshComplete(const FSourceControlOperationRef& InOperation, ECommandResult::Type InResult)
{
	bLoading = false;
	HistoryItems.Empty();

	if (InResult == ECommandResult::Succeeded)
	{
		const TSharedRef<FLoreRefreshBranchHistoryOperation> Operation = StaticCastSharedRef<FLoreRefreshBranchHistoryOperation>(InOperation);
		for (const FLoreBranchHistoryEntry& Entry : Operation->GetHistory())
		{
			HistoryItems.Add(MakeShared<FLoreBranchHistoryEntry>(Entry));
		}
	}
	else
	{
		ErrorText = LOCTEXT("BranchHistoryQueryFailed", "Lore branch history could not be loaded. See the Source Control log for details.");
	}

	if (HistoryList)
	{
		HistoryList->RequestListRefresh();
	}
}

TSharedRef<ITableRow> SLoreBranchHistory::OnGenerateRow(FLoreBranchHistoryItem InItem, const TSharedRef<STableViewBase>& InOwnerTable)
{
	return SNew(SLoreBranchHistoryRow, InOwnerTable)
		.Item(MoveTemp(InItem));
}

FText SLoreBranchHistory::GetHeadingText() const
{
	return FText::Format(
		LOCTEXT("BranchHistoryHeading", "Branch History — {0}"),
		FText::FromString(BranchName.IsEmpty() ? TEXT("unknown") : BranchName));
}

FText SLoreBranchHistory::GetStatusText() const
{
	return ErrorText.IsEmpty()
		? LOCTEXT("BranchHistoryEmpty", "No revisions found on this branch.")
		: ErrorText;
}

EVisibility SLoreBranchHistory::GetLoadingVisibility() const
{
	return bLoading ? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility SLoreBranchHistory::GetStatusVisibility() const
{
	return !bLoading && HistoryItems.IsEmpty() ? EVisibility::Visible : EVisibility::Collapsed;
}

#undef LOCTEXT_NAMESPACE
