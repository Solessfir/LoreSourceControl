// Copyright Solessfir 2026. All Rights Reserved.

#pragma once

#include "Widgets/SCompoundWidget.h"

class SLoreSourceControlSettings : public SCompoundWidget
{
public:

	SLATE_BEGIN_ARGS(SLoreSourceControlSettings) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:

	/** Get the configured (or auto) lore binary path for display/picker */
	FString GetBinaryPathString() const;

	/** Called when user picks a new path via the file picker */
	void OnBinaryPathPicked(const FString& PickedPath) const;

	/** Whether we successfully found and can execute lore */
	bool IsLoreBinaryValid() const;

	/** Warning visibility if we couldn't resolve */
	EVisibility GetWarningVisibility() const;

	/** The warning text to show */
	FText GetWarningText() const;

	/** Connection info rows - only shown once we have found a repository */
	EVisibility GetConnectionInfoVisibility() const;
	FText GetServerText() const;
	FText GetUserText() const;
	FText GetBranchText() const;

	/** Builds a "Label: Value" row matching the rest of the panel's layout */
	static TSharedRef<SWidget> MakeInfoRow(const FText& Label, const TAttribute<FText>& Value, const FText& Tooltip);
};
