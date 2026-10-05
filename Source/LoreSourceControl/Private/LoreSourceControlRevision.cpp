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
		const FString Prefix = FString::Printf(TEXT("%s-Rev-%s-"), *FPaths::GetBaseFilename(Filename), *RevisionHash.Left(12));
		const FString Extension = FPaths::GetExtension(Filename, true);
		InOutFilename = FPaths::ConvertRelativePathToFull(FPaths::CreateTempFilename(*FPaths::DiffDir(), *Prefix, *Extension));
	}

	// Relativize Filename to the repository root the same way RunLoreCommand does for its own file arguments, since --path here needs a lore-relative path, not an absolute one.
	FString RelativeToForMake = PathToRepositoryRoot;
	FPaths::NormalizeFilename(RelativeToForMake);
	if (!RelativeToForMake.EndsWith(TEXT("/")))
	{
		RelativeToForMake += TEXT("/");
	}

	// MakePathRelativeTo treats its second argument as a file and relativizes against its containing directory by stripping the last path segment.
	// Appending a fake leaf makes it strip "dummy" instead, leaving PathToRepositoryRoot as the base directory.
	RelativeToForMake += TEXT("dummy");

	FString RelativePath = Filename;
	FPaths::MakePathRelativeTo(RelativePath, *RelativeToForMake);
	FPaths::NormalizeFilename(RelativePath);
	RelativePath.ReplaceInline(TEXT("\\"), TEXT("/"));

	TArray<FString> Params;
	Params.Add(TEXT("--force"));
	Params.Add(TEXT("--path=") + FLoreSourceControlUtils::QuoteCommandLineArgument(RelativePath));
	Params.Add(TEXT("--revision=") + FLoreSourceControlUtils::QuoteCommandLineArgument(RevisionHash));
	Params.Add(TEXT("--output=") + FLoreSourceControlUtils::QuoteCommandLineArgument(InOutFilename));

	TArray<FString> Results;
	TArray<FString> Errors;
	return FLoreSourceControlUtils::RunLoreCommand(TEXT("file write"), PathToLoreBinary, PathToRepositoryRoot, Params, TArray<FString>(), Results, Errors);
}
