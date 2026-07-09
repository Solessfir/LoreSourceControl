// Copyright Solessfir 2026. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "UObject/SoftObjectPath.h"
#include "LoreSourceControlSettings.generated.h"

UCLASS(Config = "EditorPerProjectUserSettings", Meta = (DisplayName = "Lore Source Control"))
class ULoreSourceControlSettings : public UDeveloperSettings
{
    GENERATED_BODY()

public:
    ULoreSourceControlSettings()
    {
        CategoryName = FName("Plugins");
    }

    UPROPERTY(EditAnywhere, Config, Meta = (DisplayName = "Lore Path"), Category = "Lore")
    FFilePath BinaryPath;

    UPROPERTY(EditAnywhere, Config, Meta = (DisplayName = "Lock Files On Check Out"), Category = "Lore")
    bool bLockFiles = true;

    /** Returns the user-configured binary path (can be empty = auto-detect) */
    const FString& GetConfiguredBinaryPath() const
    {
        return BinaryPath.FilePath;
    }

    /** Returns whether Check Out/Revert/Submit should actually acquire/release Lore locks */
    bool ShouldLockFiles() const
    {
        return bLockFiles;
    }
};
