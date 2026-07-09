// Copyright Solessfir 2026. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ISourceControlRevision.h"

/** One entry in a file's history, backed by a single Lore revision. */
class FLoreSourceControlRevision : public ISourceControlRevision
{
public:
	//~ ISourceControlRevision interface
	virtual bool Get(FString& InOutFilename, EConcurrency::Type InConcurrency = EConcurrency::Synchronous) const override;
	virtual bool GetAnnotated(TArray<FAnnotationLine>& OutLines) const override { return false; }
	virtual bool GetAnnotated(FString& InOutFilename) const override { return false; }
	virtual const FString& GetFilename() const override { return Filename; }
	virtual int32 GetRevisionNumber() const override { return RevisionNumber; }
	virtual const FString& GetRevision() const override { return RevisionString; }
	virtual const FString& GetDescription() const override { return Description; }
	virtual const FString& GetUserName() const override { return UserName; }
	virtual const FString& GetClientSpec() const override { static FString Empty; return Empty; }
	virtual const FString& GetAction() const override { return Action; }
	virtual TSharedPtr<ISourceControlRevision> GetBranchSource() const override { return nullptr; }
	virtual const FDateTime& GetDate() const override { return Date; }
	virtual int32 GetCheckInIdentifier() const override { return RevisionNumber; }
	// ISourceControlState caps this at int32 - clamp instead of wrapping for >2GB files.
	virtual int32 GetFileSize() const override { return static_cast<int32>(FMath::Min<int64>(FileSize, MAX_int32)); }

	/** Absolute local path of the file this revision belongs to */
	FString Filename;

	/** Path to the lore binary and repository root, needed to fetch this revision's content on demand */
	FString PathToLoreBinary;
	FString PathToRepositoryRoot;

	/** Full revision hash ("signature") this history entry refers to */
	FString RevisionHash;

	/** Sequential revision number, also used as the display "Revision" and check-in identifier */
	int32 RevisionNumber = 0;

	/** Sequential revision number as text (what the "Revision" column displays - matches Lore CLI's own "Revision" field, not the Signature hash) */
	FString RevisionString;

	/** Commit message, if any metadata was found for this revision */
	FString Description;

	/** Commit author ("created-by" metadata) */
	FString UserName;

	/** Add/Delete/Move/Copy/Edit, from Lore's LoreFileAction for this entry */
	FString Action;

	/** Commit timestamp, if found in this revision's metadata */
	FDateTime Date = FDateTime::MinValue();

	/** Size of the file at this revision, in bytes (0 if deleted) */
	int64 FileSize = 0;
};

typedef TArray<TSharedRef<FLoreSourceControlRevision>> FLoreSourceControlHistory;
