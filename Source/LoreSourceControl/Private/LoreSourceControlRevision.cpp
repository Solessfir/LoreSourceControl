// Copyright Solessfir 2026. All Rights Reserved.

#include "LoreSourceControlRevision.h"
#include "LoreSourceControlUtils.h"
#include "ISourceControlModule.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"

bool FLoreSourceControlRevision::Get(FString& InOutFilename, EConcurrency::Type InConcurrency) const
{
	if (InConcurrency != EConcurrency::Synchronous)
	{
		UE_LOG(LogSourceControl, Warning, TEXT("FLoreSourceControlRevision::Get only supports EConcurrency::Synchronous."));
	}

	if (InOutFilename.IsEmpty())
	{
		IFileManager::Get().MakeDirectory(*FPaths::DiffDir(), true);
		InOutFilename = FPaths::ConvertRelativePathToFull(FString::Printf(TEXT("%stemp-%s-%s"), *FPaths::DiffDir(), *RevisionHash.Left(12), *FPaths::GetCleanFilename(Filename)));
	}

	if (FPaths::FileExists(InOutFilename))
	{
		return true;
	}

	// Relativize Filename to the repository root the same way RunLoreCommand does for its own
	// file arguments, since --path here needs a lore-relative path, not an absolute one.
	FString RelativeToForMake = PathToRepositoryRoot;
	FPaths::NormalizeFilename(RelativeToForMake);
	if (!RelativeToForMake.EndsWith(TEXT("/")))
	{
		RelativeToForMake += TEXT("/");
	}

	// MakePathRelativeTo treats its second argument as a file and relativizes against its
	// containing directory (stripping the last path segment) - appending a fake leaf here makes
	// it strip "dummy" instead, leaving PathToRepositoryRoot itself as the base directory.
	RelativeToForMake += TEXT("dummy");

	FString RelativePath = Filename;
	FPaths::MakePathRelativeTo(RelativePath, *RelativeToForMake);
	FPaths::NormalizeFilename(RelativePath);
	RelativePath.ReplaceInline(TEXT("\\"), TEXT("/"));

	TArray<FString> Params;
	Params.Add(FString::Printf(TEXT("--path \"%s\""), *RelativePath));
	Params.Add(FString::Printf(TEXT("--revision %s"), *RevisionHash));
	Params.Add(FString::Printf(TEXT("--output \"%s\""), *InOutFilename));

	TArray<FString> Results;
	TArray<FString> Errors;
	return FLoreSourceControlUtils::RunLoreCommand(TEXT("file write"), PathToLoreBinary, PathToRepositoryRoot, Params, TArray<FString>(), Results, Errors);
}
