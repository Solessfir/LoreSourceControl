// Copyright Solessfir 2026. All Rights Reserved.

#include "LoreSourceControlUtils.h"
#include "LoreSourceControlState.h"
#include "LoreSourceControlProvider.h"
#include "LoreSourceControlSettings.h"
#include "ISourceControlModule.h"
#include "Misc/Paths.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformProcess.h"
#include "UObject/UObjectGlobals.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/SoftObjectPath.h"

#define LOCTEXT_NAMESPACE "LoreSourceControl"

namespace FLoreSourceControlUtils
{
	static FString ExtractBranchFromStatusOutput(const FString& InResults);

	FString QuoteCommandLineArgument(const FString& InArgument)
	{
		FString Escaped;
		Escaped.Reserve(InArgument.Len() + 2);

		int32 PendingBackslashes = 0;
		for (const TCHAR Char : InArgument)
		{
			if (Char == TEXT('\\'))
			{
				++PendingBackslashes;
				continue;
			}

			if (Char == TEXT('"'))
			{
				Escaped.Append(FString::ChrN(PendingBackslashes * 2 + 1, TEXT('\\')));
				Escaped.AppendChar(TEXT('"'));
			}
			else
			{
				Escaped.Append(FString::ChrN(PendingBackslashes, TEXT('\\')));
				Escaped.AppendChar(Char);
			}
			PendingBackslashes = 0;
		}

		Escaped.Append(FString::ChrN(PendingBackslashes * 2, TEXT('\\')));
		return FString::Printf(TEXT("\"%s\""), *Escaped);
	}

	static bool ParseJsonLine(const FString& InLine, TSharedPtr<FJsonObject>& OutObject)
	{
		const TSharedRef<TJsonReader<>>& Reader = TJsonReaderFactory<>::Create(InLine);
		return FJsonSerializer::Deserialize(Reader, OutObject) && OutObject.IsValid();
	}

	static const FJsonObject* GetObjectField(const FJsonObject& InObject, const TCHAR* InFieldName)
	{
		const TSharedPtr<FJsonObject>* Field = nullptr;
		if (InObject.TryGetObjectField(InFieldName, Field) && Field && Field->IsValid())
		{
			return Field->Get();
		}
		return nullptr;
	}

	static FString GetDefaultLoreBinaryName()
	{
	#if PLATFORM_WINDOWS
		return TEXT("lore.exe");
	#else
		return TEXT("lore");
	#endif
	}

	static TArray<FString> GetDefaultLoreInstallSearchPaths()
	{
		TArray<FString> Paths;
		const FString BinaryName = GetDefaultLoreBinaryName();

	#if PLATFORM_WINDOWS
		Paths.Add(TEXT("C:/Program Files/lore/") + BinaryName);
		Paths.Add(TEXT("C:/Program Files/lore/bin/") + BinaryName);
		Paths.Add(TEXT("C:/Program Files (x86)/lore/") + BinaryName);
	#elif PLATFORM_MAC
		// /usr/local/bin and /opt/homebrew/bin are standard PATH dirs - already covered by the direct PATH exec attempt above, so only non-PATH-guaranteed locations are listed here.
		Paths.Add(TEXT("/opt/lore/bin/") + BinaryName);
		{
			FString Home = FPlatformMisc::GetEnvironmentVariable(TEXT("HOME"));
			if (Home.IsEmpty())
			{
				Home = FPlatformProcess::UserDir();
			}

			if (!Home.IsEmpty())
			{
				Paths.Add(FPaths::Combine(Home, TEXT(".local/bin/"), BinaryName));
			}
		}
	#elif PLATFORM_LINUX
		// /usr/local/bin and /usr/bin are standard PATH dirs - already covered by the direct PATH exec attempt above, so only non-PATH-guaranteed locations are listed here.
		Paths.Add(TEXT("/opt/lore/bin/") + BinaryName);
		{
			FString Home = FPlatformMisc::GetEnvironmentVariable(TEXT("HOME"));
			if (Home.IsEmpty())
			{
				Home = FPlatformProcess::UserDir();
			}

			if (!Home.IsEmpty())
			{
				Paths.Add(FPaths::Combine(Home, TEXT(".local/bin/"), BinaryName));
			}
		}
	#endif

		return Paths;
	}

	static bool ParseLoreVersion(const FString& InOutput, FString& OutVersion, int32& OutMajor, int32& OutMinor, int32& OutPatch)
	{
		TArray<FString> Tokens;
		InOutput.ParseIntoArrayWS(Tokens);
		if (Tokens.Num() < 2 || !Tokens[0].Equals(TEXT("lore"), ESearchCase::IgnoreCase))
		{
			return false;
		}

		OutVersion = Tokens[1];
		FString NumericVersion = OutVersion;
		int32 SuffixIndex = NumericVersion.Len();
		const int32 DashIndex = NumericVersion.Find(TEXT("-"));
		const int32 PlusIndex = NumericVersion.Find(TEXT("+"));
		if (DashIndex != INDEX_NONE)
		{
			SuffixIndex = FMath::Min(SuffixIndex, DashIndex);
		}
		if (PlusIndex != INDEX_NONE)
		{
			SuffixIndex = FMath::Min(SuffixIndex, PlusIndex);
		}
		NumericVersion.LeftInline(SuffixIndex);

		TArray<FString> Parts;
		NumericVersion.ParseIntoArray(Parts, TEXT("."), false);
		return Parts.Num() == 3
			&& LexTryParseString(OutMajor, *Parts[0])
			&& LexTryParseString(OutMinor, *Parts[1])
			&& LexTryParseString(OutPatch, *Parts[2]);
	}

	static bool ProbeLoreVersion(const FString& Candidate, FString& OutVersion, bool* OutTestedVersion = nullptr)
	{
		if (OutTestedVersion)
		{
			*OutTestedVersion = false;
		}

		if (Candidate.IsEmpty())
		{
			return false;
		}

		int32 ReturnCode = 0;
		FString OutResults;
		FString OutErrors;

		FPlatformProcess::ExecProcess(*Candidate, TEXT("--version"), &ReturnCode, &OutResults, &OutErrors);

		int32 Major = 0;
		int32 Minor = 0;
		int32 Patch = 0;
		if (ReturnCode == 0 && ParseLoreVersion(OutResults, OutVersion, Major, Minor, Patch))
		{
			if (OutTestedVersion)
			{
				*OutTestedVersion = Major == 0 && Minor == 8 && Patch >= 6;
			}
			return true;
		}

		return false;
	}

	static bool TryExecuteLoreVersion(const FString& Candidate, FString& OutUsedCommand)
	{
		FString Version;
		if (!ProbeLoreVersion(Candidate, Version))
		{
			return false;
		}

		OutUsedCommand = Candidate;
		return true;
	}

	// Resolves a bare command name (e.g. "lore.exe") to the full path of the first match on PATH.
	static bool ResolveViaSystemPath(const FString& InBinaryName, FString& OutFullPath)
	{
		TArray<FString> Directories;
		FPlatformMisc::GetEnvironmentVariable(TEXT("PATH")).ParseIntoArray(Directories, FPlatformMisc::GetPathVarDelimiter());

		for (const FString& Directory : Directories)
		{
			const FString Candidate = FPaths::Combine(Directory, InBinaryName);
			if (FPaths::FileExists(Candidate))
			{
				OutFullPath = Candidate;
				return true;
			}
		}

		return false;
	}

	FString FindLoreBinaryPath()
	{
		// User-configured path in Project Settings (highest priority)
		const FString UserPath = GetUserConfiguredLoreBinaryPath();
		if (!UserPath.IsEmpty())
		{
			// Return it even if the file doesn't exist yet — the user explicitly set it.
			// CheckLoreAvailability will validate and give feedback.
			return UserPath;
		}

		// Try "lore" / "lore.exe" directly from PATH (most common after official install)
		FString PathCommand = GetDefaultLoreBinaryName();
		if (TryExecuteLoreVersion(PathCommand, PathCommand))
		{
			FString ResolvedPath;
			if (ResolveViaSystemPath(GetDefaultLoreBinaryName(), ResolvedPath))
			{
				return ResolvedPath;
			}
			return PathCommand; // still works even if we could not resolve where it actually lives
		}

		// Default official / common install locations (platform specific)
		TArray<FString> DefaultInstallPaths = GetDefaultLoreInstallSearchPaths();
		FString FirstExistingInstallPath;
		for (const FString& InstallPath : DefaultInstallPaths)
		{
			if (FPaths::FileExists(InstallPath))
			{
				if (FirstExistingInstallPath.IsEmpty())
				{
					FirstExistingInstallPath = InstallPath;
				}

				FString Validated;
				if (TryExecuteLoreVersion(InstallPath, Validated))
				{
					return Validated;
				}
			}
		}
		if (!FirstExistingInstallPath.IsEmpty())
		{
			return FirstExistingInstallPath;
		}

		// 4. Last resort: bare name (will likely fail availability check, which will instruct the user)
		return GetDefaultLoreBinaryName();
	}

	bool ReadRepositoryConfig(const FString& InRepositoryRoot, FString& OutRemoteUrl, FString& OutIdentity)
	{
		OutRemoteUrl.Empty();
		OutIdentity.Empty();

		TArray<FString> Lines;
		if (!FFileHelper::LoadFileToStringArray(Lines, *FPaths::Combine(InRepositoryRoot, TEXT(".lore"), TEXT("config.toml"))))
		{
			return false;
		}

		for (const FString& Line : Lines)
		{
			const FString Trimmed = Line.TrimStartAndEnd();

			// remote_url/identity are top-level scalars written before any [table] - stop once we reach one, everything past that is a nested table (e.g. [store]) we don't need here.
			if (Trimmed.StartsWith(TEXT("[")))
			{
				break;
			}

			FString Key, Value;
			if (!Trimmed.Split(TEXT("="), &Key, &Value))
			{
				continue;
			}

			Key = Key.TrimStartAndEnd();
			Value = Value.TrimStartAndEnd();
			Value.RemoveFromStart(TEXT("\""));
			Value.RemoveFromEnd(TEXT("\""));

			if (Key == TEXT("remote_url"))
			{
				OutRemoteUrl = Value;
			}
			else if (Key == TEXT("identity"))
			{
				OutIdentity = Value;
			}
		}

		return true;
	}

	bool CheckLoreAvailability(const FString& InLoreBinaryPath, FString* OutVersion, bool* OutTestedVersion)
	{
		FString Version;
		const bool bAvailable = ProbeLoreVersion(InLoreBinaryPath, Version, OutTestedVersion);
		if (OutVersion)
		{
			*OutVersion = MoveTemp(Version);
		}
		return bAvailable;
	}

	bool FindRootDirectory(const FString& InPath, FString& OutRepositoryRoot)
	{
		FString SearchPath = InPath;
		FPaths::NormalizeDirectoryName(SearchPath);

		// Walk up the tree
		while (!SearchPath.IsEmpty())
		{
			FString LoreDir = FPaths::Combine(SearchPath, TEXT(".lore"));
			if (FPaths::DirectoryExists(LoreDir))
			{
				OutRepositoryRoot = SearchPath;
				return true;
			}

			// Stop at drive root
			FString Parent = FPaths::GetPath(SearchPath);
			if (Parent == SearchPath || Parent.IsEmpty())
			{
				break;
			}
			SearchPath = Parent;
		}

		return false;
	}

	bool RunLoreCommand(const FString& InCommand, const FString& InLoreBinary, const FString& InRepositoryRoot, const TArray<FString>& InParameters, const TArray<FString>& InFiles, TArray<FString>& OutResults, TArray<FString>& OutErrorMessages, bool bUseJson)
	{
		int32 ReturnCode = 0;
		FString Results;
		FString Errors;

		// --json provides reliable structured output capture and avoids pager, anstream, and human-text quirks when piping from UE.
		// Every internal caller needs it to parse events from OutResults.
		// lore [--json] <command> [params] ["files" ...]
		FString FullCommand = bUseJson ? TEXT("--json ") : FString();
		FullCommand += InCommand;

		for (const FString& Param : InParameters)
		{
			FullCommand += TEXT(" ");
			FullCommand += Param;
		}

		const FString WorkingDir = FPaths::ConvertRelativePathToFull(InRepositoryRoot.IsEmpty() ? FPaths::ProjectDir() : InRepositoryRoot);

		// To make a path relative to a *directory* root (not a file), append a dummy leaf so that internal GetPath() in MakePathRelativeTo returns the directory itself.
		FString RelativeToForMake = WorkingDir;
		FPaths::NormalizeFilename(RelativeToForMake);
		if (!RelativeToForMake.EndsWith(TEXT("/")))
		{
			RelativeToForMake += TEXT("/");
		}
		RelativeToForMake += TEXT("dummy");

		for (const FString& File : InFiles)
		{
			FString LorePath = File;
			FPaths::MakePathRelativeTo(LorePath, *RelativeToForMake);
			FPaths::NormalizeFilename(LorePath);
			LorePath = LorePath.Replace(TEXT("\\"), TEXT("/"));
			FullCommand += TEXT(" \"");
			FullCommand += LorePath;
			FullCommand += TEXT("\"");
		}

		UE_LOG(LogSourceControl, Verbose, TEXT("[Lore] %s %s (cwd=%s)"), *InLoreBinary, *FullCommand, *WorkingDir);

		// Pass the correct working directory.
		// Lore discovers the repository by walking up for a .lore folder, but running from the correct root makes status/stage/commit/sync more reliable across platforms.
		FPlatformProcess::ExecProcess(*InLoreBinary, *FullCommand, &ReturnCode, &Results, &Errors, *WorkingDir);

		Results.ParseIntoArray(OutResults, TEXT("\n"), true);
		Errors.ParseIntoArray(OutErrorMessages, TEXT("\n"), true);

		if (bUseJson)
		{
			for (const FString& Line : OutResults)
			{
				TSharedPtr<FJsonObject> JsonObj;
				if (!ParseJsonLine(Line.TrimStartAndEnd(), JsonObj))
				{
					continue;
				}

				FString TagName;
				if (!JsonObj->TryGetStringField(TEXT("tagName"), TagName))
				{
					continue;
				}

				const FJsonObject* Data = GetObjectField(*JsonObj, TEXT("data"));
				if (!Data)
				{
					continue;
				}

				if (TagName == TEXT("complete"))
				{
					if (const FJsonObject* ErrorObj = GetObjectField(*Data, TEXT("error")))
					{
						int32 ErrorCode = 0;
						if (ErrorObj->TryGetNumberField(TEXT("errorCode"), ErrorCode) && ErrorCode != 0)
						{
							FString ErrorMessage;
							ErrorObj->TryGetStringField(TEXT("message"), ErrorMessage);
							OutErrorMessages.Add(FString::Printf(TEXT("lore: %s"), ErrorMessage.IsEmpty() ? TEXT("command reported a failure") : *ErrorMessage));
						}
					}
				}
				else if (TagName == TEXT("log"))
				{
					FString Level;
					if (Data->TryGetStringField(TEXT("level"), Level) && Level == TEXT("error"))
					{
						FString LogMessage;
						Data->TryGetStringField(TEXT("message"), LogMessage);
						if (!LogMessage.IsEmpty())
						{
							OutErrorMessages.Add(FString::Printf(TEXT("lore: %s"), *LogMessage));
						}
					}
				}
			}
		}

		UE_LOG(LogSourceControl, Verbose, TEXT("[Lore] ReturnCode=%d, Stdout:\n%s"), ReturnCode, *Results);
		if (!Errors.IsEmpty())
		{
			UE_LOG(LogSourceControl, Warning, TEXT("[Lore] Stderr:\n%s"), *Errors);
		}

		return ReturnCode == 0;
	}

	bool RunUpdateStatus(const FString& InLoreBinary, const FString& InRepositoryRoot, const TArray<FString>& InFiles, FLoreSourceControlProvider& InProvider, bool bQueryLocks, TArray<FString>& OutErrorMessages, TArray<FLoreSourceControlState>& OutStates)
	{
		TArray<FString> Results;
		TArray<FString> Params;

		// Use --scan to get accurate filesystem state
		Params.Add(TEXT("--scan"));

		bool bSuccess = RunLoreCommand(TEXT("status"), InLoreBinary, InRepositoryRoot, Params, InFiles, Results, OutErrorMessages);

		const FString Combined = FString::Join(Results, TEXT("\n"));

		// One pass yields the per-file states and the repository-level facts together.
		FLoreStatusSummary Summary;
		ParseStatusResults(Combined, InFiles, InRepositoryRoot, OutStates, &Summary);

		// Also update current branch from this status output (cheap and keeps it fresh)
		if (bSuccess)
		{
			if (!Summary.BranchName.IsEmpty())
			{
				InProvider.SetBranchName(Summary.BranchName);
			}

			// "isRemoteAhead" is safe to trust from a scan of any scope because Lore reports it as a whole-repository fact on every status call.
			// It is never narrowed to the files scanned, unlike per-file dirty state (see FLoreSourceControlProvider::HasChangesToCheckIn).
			InProvider.SetHasChangesToSync(Summary.bIsRemoteAhead);
			InProvider.SetHasChangesToPush(Summary.bIsLocalAhead);
		}

		if (!bSuccess)
		{
			return false;
		}

		// A write-capable Lore command can resolve and save a previously missing identity.
		InProvider.RefreshRepositoryConfig();

		if (!bQueryLocks)
		{
			return true;
		}

		// Also query locks and merge in.
		// A locked but unmodified file has no entry in OutStates because locking alone does not change content, so --scan never flags it as dirty.
		// Synthesize a clean and checked-out state for every unmatched lock so its checkout icon appears.
		TMap<FString, FLoreLockOwner> LockedBy;
		if (!GetLoreLockStatus(InLoreBinary, InRepositoryRoot, InProvider, LockedBy, &OutErrorMessages))
		{
			return false;
		}

		// "lock query" reports the owner as a raw user ID, and our own ID is the repository's configured identity in .lore/config.toml.
		// Compare against that identity, with "me" and "self" retained as fallbacks.
		const FString OwnIdentity = InProvider.GetIdentity();

		auto ApplyLockOwner = [&OwnIdentity](FLoreSourceControlState& State, const FLoreLockOwner& Owner)
		{
			const bool bOther = !Owner.Identity.Equals(TEXT("me"), ESearchCase::IgnoreCase)
				&& !Owner.Identity.Equals(TEXT("self"), ESearchCase::IgnoreCase)
				&& (OwnIdentity.IsEmpty() || !Owner.Identity.Equals(OwnIdentity, ESearchCase::IgnoreCase));
			State.bIsCheckedOut = !bOther;
			State.bIsCheckedOutOther = bOther;
			if (bOther)
			{
				State.CheckedOutOther = Owner.GetDisplayName();
			}
		};

		for (FLoreSourceControlState& State : OutStates)
		{
			if (const FLoreLockOwner* Owner = LockedBy.Find(State.LocalFilename); Owner && !Owner->Identity.IsEmpty())
			{
				ApplyLockOwner(State, *Owner);
				LockedBy.Remove(State.LocalFilename);
			}
		}

		for (const auto& Pair : LockedBy)
		{
			if (Pair.Value.Identity.IsEmpty())
			{
				continue;
			}

			FLoreSourceControlState State(Pair.Key);
			State.bIsSourceControlled = true;
			State.bIsCurrent = true;
			ApplyLockOwner(State, Pair.Value);
			OutStates.Add(State);
		}

		return true;
	}

	void ParseStatusResults(const FString& InResults, const TArray<FString>& InFiles, const FString& InRepositoryRoot, TArray<FLoreSourceControlState>& OutStates, FLoreStatusSummary* OutSummary)
	{
		// RunLoreCommand always forces --json, so every line here is a JSON event - no plain-text status format ever reaches this function.

		TArray<FString> Lines;
		InResults.ParseIntoArray(Lines, TEXT("\n"), true);

		const FString RepoAbs = FPaths::ConvertRelativePathToFull(InRepositoryRoot);

		struct FParsedFileStatus
		{
			FString Action;
			FString FromPath;
			FString Type;
			bool bDirty = false;
			bool bStaged = false;
			bool bConflicted = false;
		};

		TMap<FString, FParsedFileStatus> FileStatuses;

		// pathIgnore means Lore intentionally excludes the path; it must not be offered for add.
		TSet<FString> IgnoredPaths;

		for (const FString& Line : Lines)
		{
			const FString Trimmed = Line.TrimStartAndEnd();

			TSharedPtr<FJsonObject> JsonObj;
			if (!ParseJsonLine(Trimmed, JsonObj))
			{
				continue;
			}

			FString TagName;
			if (!JsonObj->TryGetStringField(TEXT("tagName"), TagName))
			{
				continue;
			}

			const FJsonObject* Data = GetObjectField(*JsonObj, TEXT("data"));
			if (!Data)
			{
				continue;
			}

			if (TagName == TEXT("repositoryStatusFile"))
			{
				FString JPath;
				if (Data->TryGetStringField(TEXT("path"), JPath) && !JPath.IsEmpty())
				{
					FString JAbs = FPaths::Combine(RepoAbs, JPath);
					FPaths::NormalizeFilename(JAbs);
					JAbs = JAbs.Replace(TEXT("\\"), TEXT("/"));

					FParsedFileStatus& Status = FileStatuses.FindOrAdd(JAbs);
					Data->TryGetBoolField(TEXT("flagDirty"), Status.bDirty);
					Data->TryGetBoolField(TEXT("flagStaged"), Status.bStaged);

					bool bConflict = false;
					bool bConflictUnresolved = false;
					Data->TryGetBoolField(TEXT("flagConflict"), bConflict);
					Data->TryGetBoolField(TEXT("flagConflictUnresolved"), bConflictUnresolved);
					Status.bConflicted = bConflict || bConflictUnresolved;

					Data->TryGetStringField(TEXT("action"), Status.Action);
					Status.Action.ToLowerInline();
					Data->TryGetStringField(TEXT("fromPath"), Status.FromPath);
					Data->TryGetStringField(TEXT("type"), Status.Type);
					Status.Type.ToLowerInline();
				}
			}
			else if (TagName == TEXT("pathIgnore"))
			{
				FString JPath;
				if (Data->TryGetStringField(TEXT("path"), JPath) && !JPath.IsEmpty())
				{
					FString JAbs = FPaths::Combine(RepoAbs, JPath);
					FPaths::NormalizeFilename(JAbs);
					IgnoredPaths.Add(JAbs.Replace(TEXT("\\"), TEXT("/")));
				}
			}
			else if (TagName == TEXT("repositoryStatusRevision") && OutSummary)
			{
				FString Name;
				if (Data->TryGetStringField(TEXT("branchName"), Name) && !Name.IsEmpty())
				{
					OutSummary->BranchName = Name;
				}

				bool bRemoteAhead = false;
				if (Data->TryGetBoolField(TEXT("isRemoteAhead"), bRemoteAhead))
				{
					OutSummary->bIsRemoteAhead = bRemoteAhead;
				}

				bool bLocalAhead = false;
				if (Data->TryGetBoolField(TEXT("isLocalAhead"), bLocalAhead))
				{
					OutSummary->bIsLocalAhead = bLocalAhead;
				}
			}
		}

		// Emit a state for every dirty file found, even when specific files were requested.
		// A directory-scoped scan, such as Sync or Connect warming the cache, passes that directory as the sole entry in InFiles.
		// Keying off InFiles would collapse the recursive scan into one bogus per-directory state instead of real per-file results.
		for (const auto& Pair : FileStatuses)
		{
			const FParsedFileStatus& Parsed = Pair.Value;
			if (Parsed.Type == TEXT("directory")
				|| (!Parsed.bDirty && !Parsed.bStaged && !Parsed.bConflicted && Parsed.Action.IsEmpty()))
			{
				continue;
			}

			FLoreSourceControlState State(Pair.Key);
			State.bIsSourceControlled = true;
			State.bIsStaged = Parsed.bStaged;
			State.bIsConflicted = Parsed.bConflicted;

			if (Parsed.Action == TEXT("add") || Parsed.Action == TEXT("copy"))
			{
				State.bIsAdded = true;
				State.bCanCheckIn = true;
			}
			else if (Parsed.Action == TEXT("delete"))
			{
				State.bIsDeleted = true;
				State.bCanCheckIn = true;
			}
			else if (Parsed.Action == TEXT("move"))
			{
				State.bIsAdded = true;
				State.bCanCheckIn = true;
			}
			else
			{
				State.bIsModified = true;
				State.bCanCheckIn = true;
			}

			OutStates.Add(State);

			if (Parsed.Action == TEXT("move") && !Parsed.FromPath.IsEmpty())
			{
				FString FromAbs = FPaths::Combine(RepoAbs, Parsed.FromPath);
				FPaths::NormalizeFilename(FromAbs);
				FromAbs = FromAbs.Replace(TEXT("\\"), TEXT("/"));

				FLoreSourceControlState DeletedState(FromAbs);
				DeletedState.bIsSourceControlled = true;
				DeletedState.bIsDeleted = true;
				DeletedState.bIsStaged = Parsed.bStaged;
				DeletedState.bCanCheckIn = true;
				OutStates.Add(DeletedState);
			}
		}

		// Also emit an explicit state for every specifically requested real file that the scan did not flag as dirty.
		// This gives single-file queries, such as one immediately after CheckOut, a proper SCC state and icon instead of the provider's generic "unknown file" default.
		for (const FString& File : InFiles)
		{
			FString AbsFile = FPaths::ConvertRelativePathToFull(File);
			FPaths::NormalizeFilename(AbsFile);
			AbsFile = AbsFile.Replace(TEXT("\\"), TEXT("/"));

			if (FileStatuses.Contains(AbsFile) || FPaths::DirectoryExists(AbsFile))
			{
				// Already covered above, or this was a directory scan target rather than a real file.
				continue;
			}

			FLoreSourceControlState State(AbsFile);
			if (IgnoredPaths.Contains(AbsFile))
			{
				State.bIsIgnored = true;
			}
			else
			{
				State.bIsSourceControlled = true;
				State.bIsCurrent = true;
			}
			OutStates.Add(State);
		}
	}

	bool RunGetHistory(const FString& InLoreBinary, const FString& InRepositoryRoot, const FString& InFile, TArray<FString>& OutErrorMessages, FLoreSourceControlHistory& OutHistory)
	{
		TArray<FString> Files;
		Files.Add(InFile);

		TArray<FString> Results;
		const bool bOk = RunLoreCommand(TEXT("file history"), InLoreBinary, InRepositoryRoot, TArray<FString>(), Files, Results, OutErrorMessages);

		// Lore reports each revision as a "fileHistory" event followed by zero or more related "metadata" events for its message, creator, timestamp, and other fields.
		// Flush the entry only when the next "fileHistory" event or the end of the results is reached.
		TSharedPtr<FLoreSourceControlRevision> Current;

		for (const FString& Line : Results)
		{
			TSharedPtr<FJsonObject> JsonObj;
			if (!ParseJsonLine(Line.TrimStartAndEnd(), JsonObj))
			{
				continue;
			}

			FString TagName;
			if (!JsonObj->TryGetStringField(TEXT("tagName"), TagName))
			{
				continue;
			}

			const FJsonObject* Data = GetObjectField(*JsonObj, TEXT("data"));
			if (!Data)
			{
				continue;
			}

			if (TagName == TEXT("fileHistory"))
			{
				if (Current.IsValid())
				{
					OutHistory.Add(Current.ToSharedRef());
				}

				Current = MakeShared<FLoreSourceControlRevision, ESPMode::ThreadSafe>();
				Current->Filename = InFile;
				Current->PathToLoreBinary = InLoreBinary;
				Current->PathToRepositoryRoot = InRepositoryRoot;

				Data->TryGetStringField(TEXT("revision"), Current->RevisionHash);

				double RevisionNumber = 0.0;
				Data->TryGetNumberField(TEXT("revisionNumber"), RevisionNumber);
				Current->RevisionNumber = static_cast<int32>(RevisionNumber);
				Current->RevisionString = FString::FromInt(Current->RevisionNumber);

				double FileSize = 0.0;
				Data->TryGetNumberField(TEXT("size"), FileSize);
				Current->FileSize = static_cast<int64>(FileSize);

				FString ActionStr;
				Data->TryGetStringField(TEXT("action"), ActionStr);
				if (ActionStr == TEXT("add")) { Current->Action = TEXT("Add"); }
				else if (ActionStr == TEXT("delete")) { Current->Action = TEXT("Delete"); }
				else if (ActionStr == TEXT("move")) { Current->Action = TEXT("Move"); }
				else if (ActionStr == TEXT("copy")) { Current->Action = TEXT("Branch"); }
				else { Current->Action = TEXT("Edit"); } // "keep": content changed, path/type did not
			}
			else if (TagName == TEXT("metadata") && Current.IsValid())
			{
				FString Key;
				Data->TryGetStringField(TEXT("key"), Key);

				const FJsonObject* Value = GetObjectField(*Data, TEXT("value"));
				if (!Value)
				{
					continue;
				}

				if (Key == TEXT("message"))
				{
					Value->TryGetStringField(TEXT("data"), Current->Description);
				}
				else if (Key == TEXT("created-by"))
				{
					Value->TryGetStringField(TEXT("data"), Current->UserName);
				}
				else if (Key == TEXT("timestamp"))
				{
					double TimestampMs = 0.0;
					if (Value->TryGetNumberField(TEXT("data"), TimestampMs) && TimestampMs > 0.0)
					{
						Current->Date = FDateTime::FromUnixTimestamp(static_cast<int64>(TimestampMs / 1000.0));
					}
				}
			}
		}

		if (Current.IsValid())
		{
			OutHistory.Add(Current.ToSharedRef());
		}

		return bOk;
	}

	bool RunGetBranches(const FString& InLoreBinary, const FString& InRepositoryRoot, TArray<FLoreBranchInfo>& OutBranches)
	{
		TArray<FString> Results;
		TArray<FString> Errors;
		const bool bOk = RunLoreCommand(TEXT("branch list"), InLoreBinary, InRepositoryRoot, TArray<FString>(), TArray<FString>(), Results, Errors);

		for (const FString& Line : Results)
		{
			TSharedPtr<FJsonObject> JsonObj;
			if (!ParseJsonLine(Line.TrimStartAndEnd(), JsonObj))
			{
				continue;
			}

			FString TagName;
			if (!JsonObj->TryGetStringField(TEXT("tagName"), TagName) || TagName != TEXT("branchListEntry"))
			{
				continue;
			}

			const FJsonObject* Data = GetObjectField(*JsonObj, TEXT("data"));
			if (!Data)
			{
				continue;
			}

			FString Name;
			Data->TryGetStringField(TEXT("name"), Name);
			if (Name.IsEmpty())
			{
				continue;
			}

			bool bIsCurrent = false;
			Data->TryGetBoolField(TEXT("isCurrent"), bIsCurrent);

			// "branch list" reports Local and Remote sections separately - the same branch usually shows up in both, so dedupe by name instead of tracking which section we're in.
			FLoreBranchInfo* Existing = OutBranches.FindByPredicate([&Name](const FLoreBranchInfo& Branch) { return Branch.Name == Name; });
			if (Existing)
			{
				Existing->bIsCurrent |= bIsCurrent;
			}
			else
			{
				OutBranches.Add(FLoreBranchInfo{ Name, bIsCurrent });
			}
		}

		return bOk;
	}

	bool RunSwitchBranch(const FString& InLoreBinary, const FString& InRepositoryRoot, const FString& InBranchName, TArray<FString>& OutErrorMessages, TArray<FString>* OutChangedPaths)
	{
		TArray<FString> Params;
		Params.Add(TEXT("--"));
		Params.Add(QuoteCommandLineArgument(InBranchName));

		TArray<FString> Results;
		const bool bOk = RunLoreCommand(TEXT("branch switch"), InLoreBinary, InRepositoryRoot, Params, TArray<FString>(), Results, OutErrorMessages);
		if (OutChangedPaths)
		{
			for (const FString& Line : Results)
			{
				TSharedPtr<FJsonObject> JsonObj;
				if (!ParseJsonLine(Line.TrimStartAndEnd(), JsonObj))
				{
					continue;
				}

				FString TagName;
				if (!JsonObj->TryGetStringField(TEXT("tagName"), TagName) || TagName != TEXT("revisionSyncFile"))
				{
					continue;
				}

				const FJsonObject* Data = GetObjectField(*JsonObj, TEXT("data"));
				FString Path;
				if (Data && Data->TryGetStringField(TEXT("path"), Path) && !Path.IsEmpty())
				{
					OutChangedPaths->AddUnique(Path);
				}
			}
		}
		return bOk;
	}

	bool RunGetBranchDiff(const FString& InLoreBinary, const FString& InRepositoryRoot, const FString& InTargetBranch, TArray<FString>& OutChangedPaths, TArray<FString>& OutErrorMessages)
	{
		TArray<FString> Params;
		Params.Add(TEXT("--"));
		Params.Add(QuoteCommandLineArgument(InTargetBranch));

		TArray<FString> Results;
		const bool bOk = RunLoreCommand(TEXT("branch diff"), InLoreBinary, InRepositoryRoot, Params, TArray<FString>(), Results, OutErrorMessages);

		for (const FString& Line : Results)
		{
			TSharedPtr<FJsonObject> JsonObj;
			if (!ParseJsonLine(Line.TrimStartAndEnd(), JsonObj))
			{
				continue;
			}

			FString TagName;
			if (!JsonObj->TryGetStringField(TEXT("tagName"), TagName)
				|| (TagName != TEXT("branchDiffChange") && TagName != TEXT("branchDiffConflict")))
			{
				continue;
			}

			const FJsonObject* Data = GetObjectField(*JsonObj, TEXT("data"));
			if (!Data)
			{
				continue;
			}

			auto AddChangePath = [&OutChangedPaths](const FJsonObject* Change)
			{
				FString Path;
				if (Change && Change->TryGetStringField(TEXT("path"), Path) && !Path.IsEmpty())
				{
					OutChangedPaths.AddUnique(Path);
				}
			};

			if (TagName == TEXT("branchDiffChange"))
			{
				AddChangePath(GetObjectField(*Data, TEXT("change")));
			}
			else
			{
				AddChangePath(GetObjectField(*Data, TEXT("sourceChange")));
				AddChangePath(GetObjectField(*Data, TEXT("targetChange")));
			}
		}

		return bOk;
	}

	bool ClassifyChangedPaths(const TArray<FString>& InPaths, TArray<FString>& OutContentPaths)
	{
		bool bTouchesCode = false;
		for (const FString& Path : InPaths)
		{
			if (Path.StartsWith(TEXT("Source/")) || Path.Contains(TEXT("/Source/"))
				|| Path.StartsWith(TEXT("Config/")) || Path.Contains(TEXT("/Config/"))
				|| Path.EndsWith(TEXT(".uplugin")) || Path.EndsWith(TEXT(".uproject")))
			{
				bTouchesCode = true;
			}
			else
			{
				OutContentPaths.Add(Path);
			}
		}

		return bTouchesCode;
	}

	bool RunSync(const FString& InLoreBinary, const FString& InRepositoryRoot, TArray<FString>& OutResults, TArray<FString>& OutErrorMessages, TArray<FString>& OutChangedContentPaths, bool& OutRequiresRestart)
	{
		const bool bOk = RunLoreCommand(TEXT("sync"), InLoreBinary, InRepositoryRoot, TArray<FString>(), TArray<FString>(), OutResults, OutErrorMessages);

		TArray<FString> ChangedPaths;
		for (const FString& Line : OutResults)
		{
			TSharedPtr<FJsonObject> JsonObj;
			if (!ParseJsonLine(Line.TrimStartAndEnd(), JsonObj))
			{
				continue;
			}

			FString TagName;
			if (!JsonObj->TryGetStringField(TEXT("tagName"), TagName) || TagName != TEXT("revisionSyncFile"))
			{
				continue;
			}

			const FJsonObject* Data = GetObjectField(*JsonObj, TEXT("data"));
			FString Path;
			if (Data && Data->TryGetStringField(TEXT("path"), Path) && !Path.IsEmpty())
			{
				ChangedPaths.Add(Path);
			}
		}

		OutRequiresRestart = ClassifyChangedPaths(ChangedPaths, OutChangedContentPaths);
		return bOk;
	}

	bool GetLoreLockStatus(const FString& InLoreBinary, const FString& InRepositoryRoot, const FLoreSourceControlProvider& InProvider, TMap<FString, FLoreLockOwner>& OutLockedBy, TArray<FString>* OutErrorMessages)
	{
		// "lock status" requires exact file paths (no --scan/recursive option), so a directory (as the broad Connect/Sync scan passes) silently matches nothing.
		// "lock query" filtered by --branch lists every lock on the branch regardless of path - what we actually want either way.
		const FString CurrentBranch = InProvider.GetBranchName();

		TArray<FString> Results;
		TArray<FString> Errors;
		TArray<FString> Params;
		if (!CurrentBranch.IsEmpty())
		{
			Params.Add(FString::Printf(TEXT("--branch=%s"), *QuoteCommandLineArgument(CurrentBranch)));
		}

		const bool bOk = RunLoreCommand(TEXT("lock query"), InLoreBinary, InRepositoryRoot, Params, TArray<FString>(), Results, Errors);
		if (OutErrorMessages)
		{
			OutErrorMessages->Append(Errors);
		}

		FString RepoAbs = FPaths::ConvertRelativePathToFull(InRepositoryRoot);
		TMap<FString, FString> OwnerNames;

		for (const FString& Line : Results)
		{
			TSharedPtr<FJsonObject> JsonObj;
			if (!ParseJsonLine(Line.TrimStartAndEnd(), JsonObj))
			{
				continue;
			}

			FString TagName;
			if (!JsonObj->TryGetStringField(TEXT("tagName"), TagName))
			{
				continue;
			}

			const FJsonObject* Data = GetObjectField(*JsonObj, TEXT("data"));
			if (!Data)
			{
				continue;
			}

			if (TagName == TEXT("authUserInfo"))
			{
				FString Id, Name;
				Data->TryGetStringField(TEXT("id"), Id);
				Data->TryGetStringField(TEXT("name"), Name);
				if (!Id.IsEmpty() && !Name.IsEmpty())
				{
					OwnerNames.Add(Id, Name);
				}
				continue;
			}

			if (TagName != TEXT("lockFileQuery"))
			{
				continue;
			}

			FString Path, Owner;
			Data->TryGetStringField(TEXT("path"), Path);
			Data->TryGetStringField(TEXT("owner"), Owner);
			if (!Path.IsEmpty())
			{
				FString Abs = FPaths::Combine(RepoAbs, Path);
				FPaths::NormalizeFilename(Abs);
				Abs = Abs.Replace(TEXT("\\"), TEXT("/"));
				FLoreLockOwner LockOwner;
				LockOwner.Identity = Owner;
				OutLockedBy.Add(Abs, MoveTemp(LockOwner));
			}
		}

		for (TPair<FString, FLoreLockOwner>& Lock : OutLockedBy)
		{
			if (const FString* ResolvedName = OwnerNames.Find(Lock.Value.Identity))
			{
				Lock.Value.DisplayName = *ResolvedName;
			}
		}

		return bOk;
	}

	bool RunGetStagedPaths(const FString& InLoreBinary, const FString& InRepositoryRoot, TArray<FString>& OutStagedFiles, TArray<FString>& OutStagedDirectories, TArray<FString>& OutErrorMessages)
	{
		TArray<FString> Results;
		const bool bOk = RunLoreCommand(TEXT("status"), InLoreBinary, InRepositoryRoot, TArray<FString>(), TArray<FString>(), Results, OutErrorMessages);

		const FString RepoAbs = FPaths::ConvertRelativePathToFull(InRepositoryRoot);
		for (const FString& Line : Results)
		{
			TSharedPtr<FJsonObject> JsonObj;
			if (!ParseJsonLine(Line.TrimStartAndEnd(), JsonObj))
			{
				continue;
			}

			FString TagName;
			if (!JsonObj->TryGetStringField(TEXT("tagName"), TagName) || TagName != TEXT("repositoryStatusFile"))
			{
				continue;
			}

			const FJsonObject* Data = GetObjectField(*JsonObj, TEXT("data"));
			if (!Data)
			{
				continue;
			}

			bool bStaged = false;
			FString Type;
			FString Path;
			Data->TryGetBoolField(TEXT("flagStaged"), bStaged);
			Data->TryGetStringField(TEXT("type"), Type);
			Data->TryGetStringField(TEXT("path"), Path);
			if (!bStaged || Path.IsEmpty())
			{
				continue;
			}

			FString AbsolutePath = FPaths::Combine(RepoAbs, Path);
			FPaths::NormalizeFilename(AbsolutePath);
			AbsolutePath.ReplaceInline(TEXT("\\"), TEXT("/"));
			if (Type.Equals(TEXT("directory"), ESearchCase::IgnoreCase))
			{
				OutStagedDirectories.AddUnique(AbsolutePath);
			}
			else
			{
				OutStagedFiles.AddUnique(AbsolutePath);
			}
		}

		return bOk;
	}

	FString GetUserConfiguredLoreBinaryPath()
	{
		if (const ULoreSourceControlSettings* Settings = GetDefault<ULoreSourceControlSettings>())
		{
			return Settings->GetConfiguredBinaryPath();
		}
		return FString();
	}

	void SetUserConfiguredLoreBinaryPath(const FString& InPath)
	{
		if (ULoreSourceControlSettings* Settings = GetMutableDefault<ULoreSourceControlSettings>())
		{
			Settings->BinaryPath.FilePath = InPath;
			Settings->SaveConfig();
		}
	}

	bool ShouldLockFiles()
	{
		if (const ULoreSourceControlSettings* Settings = GetDefault<ULoreSourceControlSettings>())
		{
			return Settings->ShouldLockFiles();
		}
		return true;
	}

	void LoadSettings(FString& OutLoreBinaryPath)
	{
		// New preferred storage: UDeveloperSettings (Project Settings)
		const FString UserPath = GetUserConfiguredLoreBinaryPath();
		if (!UserPath.IsEmpty())
		{
			OutLoreBinaryPath = UserPath;
			return;
		}

		// Backward compatibility: migrate from old manual ini if present
		const FString OldIniPath = FPaths::ProjectSavedDir() / TEXT("Config") / TEXT("LoreSourceControl.ini");
		FString OldValue;
		if (GConfig->GetString(TEXT("LoreSourceControl"), TEXT("BinaryPath"), OldValue, OldIniPath))
		{
			if (!OldValue.IsEmpty())
			{
				OutLoreBinaryPath = OldValue;
				// Migrate into the new settings system
				SetUserConfiguredLoreBinaryPath(OldValue);
			}
		}
	}

	void SaveSettings(const FString& InLoreBinaryPath)
	{
		// Persist using the proper Developer Settings
		SetUserConfiguredLoreBinaryPath(InLoreBinaryPath);
	}

	bool UpdateCachedStates(FLoreSourceControlProvider* InProvider, const TArray<FLoreSourceControlState>& InStates, const TArray<FString>& InScanPaths, bool bApplyResults)
	{
		if (!InProvider || !bApplyResults)
		{
			return false;
		}

		InProvider->ReplaceStatesInCache(InStates, InScanPaths);

		// Caller (FLoreSourceControlProvider::Tick) broadcasts once after UpdateStates() returns true.
		return true;
	}

	static FString ExtractBranchFromStatusOutput(const FString& InResults)
	{
		TArray<FString> Lines;
		InResults.ParseIntoArray(Lines, TEXT("\n"), true);

		// RunLoreCommand always forces --json, so every line here is a JSON event.
		for (const FString& Line : Lines)
		{
			const FString Trimmed = Line.TrimStartAndEnd();

			TSharedPtr<FJsonObject> JsonObj;
			if (!ParseJsonLine(Trimmed, JsonObj))
			{
				continue;
			}

			FString TagName;
			if (!JsonObj->TryGetStringField(TEXT("tagName"), TagName))
			{
				continue;
			}

			const FJsonObject* Data = GetObjectField(*JsonObj, TEXT("data"));
			if (!Data)
			{
				continue;
			}

			if (TagName == TEXT("repositoryStatusRevision"))
			{
				FString Name;
				if (Data->TryGetStringField(TEXT("branchName"), Name) && !Name.IsEmpty())
				{
					return Name;
				}
			}
			else if (TagName == TEXT("branchInfo"))
			{
				FString Name;
				if (Data->TryGetStringField(TEXT("name"), Name) && !Name.IsEmpty())
				{
					return Name;
				}
			}
		}
		return FString();
	}

	FString GetCurrentBranchName(const FString& InLoreBinary, const FString& InRepositoryRoot)
	{
		if (InLoreBinary.IsEmpty())
		{
			return FString();
		}

		TArray<FString> Results;
		TArray<FString> Errors;

		// Preferred: `lore branch info` (no args) directly reports the current branch
		// With --json: {"tagName":"branchInfo","data":{"name":"main", ...}}
		if (RunLoreCommand(TEXT("branch info"), InLoreBinary, InRepositoryRoot, TArray<FString>(), TArray<FString>(), Results, Errors))
		{
			for (const FString& Line : Results)
			{
				const FString Trimmed = Line.TrimStartAndEnd();
				TSharedPtr<FJsonObject> JsonObj;
				if (!ParseJsonLine(Trimmed, JsonObj))
				{
					continue;
				}

				FString TagName;
				if (JsonObj->TryGetStringField(TEXT("tagName"), TagName) && TagName == TEXT("branchInfo"))
				{
					if (const FJsonObject* Data = GetObjectField(*JsonObj, TEXT("data")))
					{
						FString Name;
						if (Data->TryGetStringField(TEXT("name"), Name) && !Name.IsEmpty())
						{
							return Name;
						}
					}
				}
			}
		}

		// Next: `lore status --revision-only`, which reports the branch via "repositoryStatusRevision"
		Results.Empty();
		Errors.Empty();
		TArray<FString> Params;
		Params.Add(TEXT("--revision-only"));

		if (RunLoreCommand(TEXT("status"), InLoreBinary, InRepositoryRoot, Params, TArray<FString>(), Results, Errors))
		{
			FString Combined;
			for (const FString& Line : Results)
			{
				Combined += Line + TEXT("\n");
			}
			FString Branch = ExtractBranchFromStatusOutput(Combined);
			if (!Branch.IsEmpty())
			{
				return Branch;
			}
		}

		// Fallback: `lore branch list`, picking out the entry with isCurrent set
		TArray<FLoreBranchInfo> Branches;
		if (RunGetBranches(InLoreBinary, InRepositoryRoot, Branches))
		{
			for (const FLoreBranchInfo& Branch : Branches)
			{
				if (Branch.bIsCurrent)
				{
					return Branch.Name;
				}
			}
		}

		return FString();
	}

} // namespace FLoreSourceControlUtils

#undef LOCTEXT_NAMESPACE
