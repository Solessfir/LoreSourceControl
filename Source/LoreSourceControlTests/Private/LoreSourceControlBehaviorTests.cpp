// Copyright Solessfir 2026. All Rights Reserved.
//
// Behaviour the plugin must have, pinned before it does. Each test names the defect it pins.
// They drive the real command runner against a fake lore: a small shell script, written by
// the test, that records every argument it receives and can answer, sleep, or die on cue.

#include "LoreSourceControlCommand.h"
#include "LoreSourceControlOperations.h"
#include "LoreSourceControlProvider.h"
#include "LoreSourceControlState.h"
#include "LoreSourceControlUtils.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "SourceControlOperations.h"

#if PLATFORM_MAC || PLATFORM_LINUX
#include <sys/stat.h>
#endif

#if WITH_DEV_AUTOMATION_TESTS && (PLATFORM_MAC || PLATFORM_LINUX)

namespace
{
	// Records each call as "CALL" then one "ARG <value>" line per argument (newlines become \x1e).
	// Answers <dir>/<subcommand words joined by _>.out and exits with .code, if present.
	const TCHAR* FakeLoreScript = TEXT(
		"#!/bin/sh\n"
		"if [ -n \"$FAKE_LORE_LOG\" ]; then\n"
		"  printf 'CALL\\n' >> \"$FAKE_LORE_LOG\"\n"
		"  for a in \"$@\"; do printf 'ARG %s\\n' \"$(printf '%s' \"$a\" | tr '\\n' '\\036')\" >> \"$FAKE_LORE_LOG\"; done\n"
		"fi\n"
		"key=''\n"
		"for a in \"$@\"; do\n"
		"  case \"$a\" in --json) continue;; -*) break;; esac\n"
		"  key=\"${key:+${key}_}$a\"\n"
		"done\n"
		"case \"$FAKE_LORE_MODE\" in\n"
		"  sleep:*) sleep \"${FAKE_LORE_MODE#sleep:}\";;\n"
		"  kill) kill -9 $$;;\n"
		"esac\n"
		"if [ -n \"$FAKE_LORE_DIR\" ] && [ -f \"$FAKE_LORE_DIR/$key.out\" ]; then cat \"$FAKE_LORE_DIR/$key.out\"; fi\n"
		"code=0\n"
		"if [ -n \"$FAKE_LORE_DIR\" ] && [ -f \"$FAKE_LORE_DIR/$key.code\" ]; then code=$(cat \"$FAKE_LORE_DIR/$key.code\"); fi\n"
		"exit $code\n");

	struct FFakeLore
	{
		FFakeLore()
		{
			Dir = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation/LoreSourceControlBehavior"), FGuid::NewGuid().ToString(EGuidFormats::Digits)));
			Root = FPaths::Combine(Dir, TEXT("repo"));
			IFileManager::Get().MakeDirectory(*FPaths::Combine(Root, TEXT(".lore")), true);
			IFileManager::Get().MakeDirectory(*FPaths::Combine(Root, TEXT("Content")), true);
			IFileManager::Get().MakeDirectory(*FPaths::Combine(Dir, TEXT("responses")), true);
			Binary = FPaths::Combine(Dir, TEXT("fake-lore"));
			Log = FPaths::Combine(Dir, TEXT("calls.log"));
			FFileHelper::SaveStringToFile(FakeLoreScript, *Binary);
			chmod(TCHAR_TO_UTF8(*Binary), 0755);
			FPlatformMisc::SetEnvironmentVar(TEXT("FAKE_LORE_LOG"), *Log);
			FPlatformMisc::SetEnvironmentVar(TEXT("FAKE_LORE_DIR"), *FPaths::Combine(Dir, TEXT("responses")));
			FPlatformMisc::SetEnvironmentVar(TEXT("FAKE_LORE_MODE"), TEXT(""));
		}

		~FFakeLore()
		{
			FPlatformMisc::SetEnvironmentVar(TEXT("FAKE_LORE_LOG"), TEXT(""));
			FPlatformMisc::SetEnvironmentVar(TEXT("FAKE_LORE_DIR"), TEXT(""));
			FPlatformMisc::SetEnvironmentVar(TEXT("FAKE_LORE_MODE"), TEXT(""));
			IFileManager::Get().DeleteDirectory(*Dir, false, true);
		}

		void Respond(const FString& Key, const FString& Output, int32 ExitCode = 0) const
		{
			FFileHelper::SaveStringToFile(Output, *FPaths::Combine(Dir, TEXT("responses"), Key + TEXT(".out")));
			FFileHelper::SaveStringToFile(FString::FromInt(ExitCode), *FPaths::Combine(Dir, TEXT("responses"), Key + TEXT(".code")));
		}

		/** Every call lore received, each as its exact argument list. */
		TArray<TArray<FString>> Calls() const
		{
			TArray<FString> Lines;
			FFileHelper::LoadFileToStringArray(Lines, *Log);
			TArray<TArray<FString>> Out;
			for (const FString& Line : Lines)
			{
				if (Line == TEXT("CALL"))
				{
					Out.AddDefaulted();
				}
				else if (Line.StartsWith(TEXT("ARG ")) && Out.Num() > 0)
				{
					Out.Last().Add(Line.Mid(4).Replace(TEXT("\x1e"), TEXT("\n")));
				}
			}
			return Out;
		}

		FString Dir;
		FString Root;
		FString Binary;
		FString Log;
	};

	/** The arguments after "--" in one call. */
	TArray<FString> FilesOf(const TArray<FString>& Call)
	{
		const int32 Separator = Call.Find(TEXT("--"));
		return Separator == INDEX_NONE ? TArray<FString>() : TArray<FString>(Call.GetData() + Separator + 1, Call.Num() - Separator - 1);
	}
}

// Fix H2: lore must receive exactly what the plugin meant to send. On Mac the plugin quotes
// arguments the Windows way, which the Mac process launcher never decodes.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreArgumentsReachLoreUnchangedTest, "LoreSourceControl.Process.ArgumentsReachLoreUnchanged", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLoreArgumentsReachLoreUnchangedTest::RunTest(const FString& Parameters)
{
	FFakeLore Fake;
	const TArray<FString> Values = {
		TEXT("Fix \"door\" bug"),
		TEXT("two  spaces"),
		TEXT("trailing backslash\\"),
		TEXT("back\\slash path"),
		TEXT("na\u00EFve caf\u00E9"),
		TEXT("line one\nline two"),
	};
	TArray<FString> Params;
	for (const FString& Value : Values)
	{
		Params.Add(FLoreSourceControlUtils::QuoteCommandLineArgument(Value));
	}
	const TArray<FString> Files = {
		FPaths::Combine(Fake.Root, TEXT("Content/My File.uasset")),
		FPaths::Combine(Fake.Root, TEXT("Content/-leading.uasset")),
	};

	TArray<FString> Results, Errors;
	const bool bOk = FLoreSourceControlUtils::RunLoreCommand(TEXT("commit"), Fake.Binary, Fake.Root, Params, Files, Results, Errors);
	TestTrue(TEXT("the command ran"), bOk);

	const TArray<TArray<FString>> Calls = Fake.Calls();
	if (!TestEqual(TEXT("one call"), Calls.Num(), 1))
	{
		return false;
	}
	TArray<FString> Expected = { TEXT("--json"), TEXT("commit") };
	Expected.Append(Values);
	Expected.Add(TEXT("--"));
	Expected.Add(TEXT("Content/My File.uasset"));
	Expected.Add(TEXT("Content/-leading.uasset"));
	TestEqual(TEXT("argument count"), Calls[0].Num(), Expected.Num());
	for (int32 i = 0; i < FMath::Min(Expected.Num(), Calls[0].Num()); ++i)
	{
		TestEqual(*FString::Printf(TEXT("argument %d"), i), Calls[0][i], Expected[i]);
	}
	return true;
}

// Fix H1 (Mac) and H4 (Windows): every selected file must reach lore. On Mac, more than 256
// arguments crash the engine's process launcher (SIGSEGV in FMacPlatformProcess::CreateProcInternal,
// measured 2026-11-12); Windows fails past 32,767 characters. A crash probe until fixed:
//   -ExecCmds="Automation RunTests LoreCrashProbe"
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreEveryFileReachesLoreTest, "LoreCrashProbe.Process.EveryFileReachesLore", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLoreEveryFileReachesLoreTest::RunTest(const FString& Parameters)
{
	FFakeLore Fake;
	TArray<FString> Files, Expected;
	for (int32 i = 0; i < 1000; ++i)
	{
		const FString Relative = FString::Printf(TEXT("Content/Assets/Long Folder Name/Asset_%04d.uasset"), i);
		Files.Add(FPaths::Combine(Fake.Root, Relative));
		Expected.Add(Relative);
	}

	TArray<FString> Results, Errors;
	const bool bOk = FLoreSourceControlUtils::RunLoreCommand(TEXT("stage"), Fake.Binary, Fake.Root, TArray<FString>(), Files, Results, Errors);
	TestTrue(TEXT("the command ran"), bOk);

	TArray<FString> Received;
	const TArray<TArray<FString>> Calls = Fake.Calls();
	for (const TArray<FString>& Call : Calls)
	{
		Received.Append(FilesOf(Call));
	}
	TestEqual(TEXT("every file reached lore"), Received.Num(), Expected.Num());
	TestTrue(TEXT("in order, unchanged"), Received == Expected);
	TestTrue(TEXT("at most 2 lore processes for 1,000 files"), Calls.Num() >= 1 && Calls.Num() <= 2);
	return true;
}

// Fix B1: no lore call may run forever. A quick read (lock query) must give up within its time
// limit (30 s) and say so, instead of freezing the editor. Longer work (a full status scan, a commit,
// a push) gets longer limits, since it can legitimately take minutes on a large repository.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreHungLoreTimesOutTest, "LoreSourceControl.Process.HungLoreTimesOut", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLoreHungLoreTimesOutTest::RunTest(const FString& Parameters)
{
	FFakeLore Fake;
	FPlatformMisc::SetEnvironmentVar(TEXT("FAKE_LORE_MODE"), TEXT("sleep:50"));

	TArray<FString> Results, Errors;
	const double Start = FPlatformTime::Seconds();
	const bool bOk = FLoreSourceControlUtils::RunLoreCommand(TEXT("lock query"), Fake.Binary, Fake.Root, TArray<FString>(), TArray<FString>(), Results, Errors);
	const double Elapsed = FPlatformTime::Seconds() - Start;

	TestFalse(TEXT("a hung read fails"), bOk);
	TestTrue(*FString::Printf(TEXT("it gives up within 40 s (took %.1f s)"), Elapsed), Elapsed < 40.0);
	TestTrue(TEXT("the error says it timed out"), Errors.ContainsByPredicate([](const FString& E) { return E.Contains(TEXT("timed out")); }));
	return true;
}

// Fix B3: in lore, locks are advisory and any file can be edited. A modified file that nobody
// else has locked is ours to submit, so the editor must see it as checked out; otherwise its
// Submit dialog silently drops it (the engine's pre-submit filter keeps checked-out files only).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreModifiedUnlockedIsCheckedOutTest, "LoreSourceControl.State.ModifiedUnlockedFileIsCheckedOut", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLoreModifiedUnlockedIsCheckedOutTest::RunTest(const FString& Parameters)
{
	FLoreSourceControlState Modified(TEXT("/repo/ArenaShooter.uproject"));
	Modified.bIsSourceControlled = true;
	Modified.bIsModified = true;
	Modified.bCanCheckIn = true;
	TestTrue(TEXT("modified, unlocked: checked out"), Modified.IsCheckedOut());
	TestTrue(TEXT("and it can still be locked explicitly"), Modified.CanCheckout());

	FLoreSourceControlState LockedByOther = Modified;
	LockedByOther.bIsCheckedOutOther = true;
	LockedByOther.CheckedOutOther = TEXT("someone else");
	TestFalse(TEXT("modified, locked by someone else: not checked out by us"), LockedByOther.IsCheckedOut());

	FLoreSourceControlState Clean(TEXT("/repo/Content/Clean.uasset"));
	Clean.bIsSourceControlled = true;
	TestFalse(TEXT("clean and unlocked: not checked out"), Clean.IsCheckedOut());
	return true;
}

// Fix H3: a state the plugin made up (a file it hasn't scanned yet) must look stale, or the
// engine's refresh after a save is skipped for five minutes and Submit stays greyed.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreUnscannedStateIsStaleTest, "LoreSourceControl.Provider.UnscannedStateIsStale", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLoreUnscannedStateIsStaleTest::RunTest(const FString& Parameters)
{
	FFakeLore Fake;
	FLoreSourceControlProvider Provider;
	Provider.CheckRepositoryStatus(Fake.Root);

	TArray<FSourceControlStateRef> States;
	Provider.GetState(TArray<FString>{ FPaths::Combine(Fake.Root, TEXT("Content/NotScannedYet.uasset")) }, States, EStateCacheUsage::Use);
	if (!TestEqual(TEXT("one state"), States.Num(), 1))
	{
		return false;
	}
	const FTimespan Age = FDateTime::Now() - States[0]->GetTimeStamp();
	TestTrue(*FString::Printf(TEXT("older than the engine's 5-minute refresh window (age %s)"), *Age.ToString()), Age > FTimespan::FromMinutes(5));
	Provider.Close(false);
	return true;
}

// Fix M1: when the lock query fails, the status that did succeed must still be used; today the
// whole refresh is thrown away and the Content Browser keeps showing stale state.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreLockQueryFailureKeepsStatusTest, "LoreSourceControl.Status.LockQueryFailureKeepsStatus", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLoreLockQueryFailureKeepsStatusTest::RunTest(const FString& Parameters)
{
	FFakeLore Fake;
	Fake.Respond(TEXT("status"), TEXT(R"({"tagName":"repositoryStatusFile","data":{"path":"Content/Modified.uasset","action":"keep","type":"file","flagDirty":1,"flagStaged":0,"flagConflict":0,"flagConflictUnresolved":0}})"));
	Fake.Respond(TEXT("lock_query"), TEXT(R"({"tagName":"complete","data":{"status":1,"error":{"errorCode":7,"message":"server unavailable"}}})"), 1);

	FLoreSourceControlProvider Provider;
	Provider.CheckRepositoryStatus(Fake.Root);
	TArray<FString> Errors;
	TArray<FLoreSourceControlState> States;
	const bool bOk = FLoreSourceControlUtils::RunUpdateStatus(Fake.Binary, Fake.Root, TArray<FString>{ FPaths::Combine(Fake.Root, TEXT("Content")) }, Provider, /*bQueryLocks=*/true, Errors, States);

	TestTrue(TEXT("the refresh succeeds on the status alone"), bOk);
	TestTrue(TEXT("with the modified file in it"), States.ContainsByPredicate([](const FLoreSourceControlState& S) { return S.LocalFilename.EndsWith(TEXT("Content/Modified.uasset")) && S.bIsModified; }));
	TestTrue(TEXT("and the lock failure is reported"), Errors.Num() > 0);
	Provider.Close(false);
	return true;
}

// Fix M3: Connect must scan everything the Submit dialog offers (Content, Config and the
// .uproject), so a change to Config or the project file enables Submit too.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreConnectScansProjectFilesTest, "LoreSourceControl.Connect.ScansConfigAndProjectFile", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLoreConnectScansProjectFilesTest::RunTest(const FString& Parameters)
{
	FFakeLore Fake;
	const FString ProjectRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	FLoreSourceControlProvider Provider;

	TSharedRef<FConnect> Operation = ISourceControlOperation::Create<FConnect>();
	TSharedRef<FLoreConnectWorker> Worker = MakeShared<FLoreConnectWorker>();
	FLoreSourceControlCommand Command(Operation, Worker);
	Command.Provider = &Provider;
	Command.PathToLoreBinary = Fake.Binary;
	Command.PathToRepositoryRoot = ProjectRoot;
	Command.bShouldLockFiles = false;
	Worker->Execute(Command);

	const FString ProjectFile = FPaths::GetCleanFilename(FPaths::GetProjectFilePath());
	bool bScannedContent = false, bScannedConfig = false, bScannedProjectFile = false;
	for (const TArray<FString>& Call : Fake.Calls())
	{
		if (Call.Num() < 2 || Call[1] != TEXT("status"))
		{
			continue;
		}
		for (const FString& File : FilesOf(Call))
		{
			bScannedContent |= File.StartsWith(TEXT("Content"));
			bScannedConfig |= File.StartsWith(TEXT("Config"));
			bScannedProjectFile |= File == ProjectFile;
		}
	}
	TestTrue(TEXT("scans Content"), bScannedContent);
	TestTrue(TEXT("scans Config"), bScannedConfig);
	TestTrue(TEXT("scans the .uproject"), bScannedProjectFile);
	Provider.Close(false);
	return true;
}

#endif

// Fix B2: lore dying from a signal must be a failed command, never an editor crash (it used to fail the
// engine's check(bGotReturnCode) in ExecProcess on Mac and Linux).
#if WITH_DEV_AUTOMATION_TESTS && (PLATFORM_MAC || PLATFORM_LINUX)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoreKilledLoreIsAFailureTest, "LoreSourceControl.Process.KilledLoreIsAFailure", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLoreKilledLoreIsAFailureTest::RunTest(const FString& Parameters)
{
	FFakeLore Fake;
	FPlatformMisc::SetEnvironmentVar(TEXT("FAKE_LORE_MODE"), TEXT("kill"));
	TArray<FString> Results, Errors;
	const bool bOk = FLoreSourceControlUtils::RunLoreCommand(TEXT("status"), Fake.Binary, Fake.Root, TArray<FString>(), TArray<FString>(), Results, Errors);
	TestFalse(TEXT("a killed lore is a failed command"), bOk);
	TestTrue(TEXT("and the error says lore was killed"), Errors.ContainsByPredicate([](const FString& E) { return E.Contains(TEXT("killed")); }));
	return true;
}

#endif
