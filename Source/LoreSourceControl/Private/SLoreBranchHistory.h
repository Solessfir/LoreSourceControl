// Copyright Solessfir 2026. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ISourceControlOperation.h"
#include "ISourceControlProvider.h"
#include "Widgets/SCompoundWidget.h"

class FLoreSourceControlProvider;
class ITableRow;
class STableViewBase;
template <typename ItemType> class SListView;
struct FLoreBranchHistoryEntry;

using FLoreBranchHistoryItem = TSharedPtr<FLoreBranchHistoryEntry>;

class SLoreBranchHistory : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SLoreBranchHistory) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, FLoreSourceControlProvider* InProvider);

private:
	FReply OnRefreshClicked();
	void Refresh();
	void OnRefreshComplete(const FSourceControlOperationRef& InOperation, ECommandResult::Type InResult);
	TSharedRef<ITableRow> OnGenerateRow(FLoreBranchHistoryItem InItem, const TSharedRef<STableViewBase>& InOwnerTable);
	FText GetHeadingText() const;
	FText GetStatusText() const;
	EVisibility GetLoadingVisibility() const;
	EVisibility GetStatusVisibility() const;

	FLoreSourceControlProvider* Provider = nullptr;
	FString BranchName;
	TArray<FLoreBranchHistoryItem> HistoryItems;
	TSharedPtr<SListView<FLoreBranchHistoryItem>> HistoryList;
	FText ErrorText;
	bool bLoading = false;
};
