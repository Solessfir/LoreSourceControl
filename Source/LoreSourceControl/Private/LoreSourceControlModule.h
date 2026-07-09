// Copyright Solessfir 2026. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"
#include "LoreSourceControlProvider.h"

class SWindow;

class FLoreSourceControlModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;

	virtual void ShutdownModule() override;

	void SaveSettings() const;

private:
#if SOURCE_CONTROL_WITH_SLATE
	/** Adds the branch switcher combo button to the level editor's Source Control toolbar, and hooks up asset editors to get the same treatment as they open */
	void RegisterToolbarExtension();

	/** Extends a single status-bar toolbar menu with the branch switcher combo button; no-ops if that menu was already extended */
	void RegisterToolbarExtensionForMenu(FName InMenuName);

	/** Extends a newly-opened asset editor's own status bar toolbar - unlike the level editor, asset editors (Blueprint, Material, etc.) don't share a fixed menu name: SStandaloneAssetEditorToolkitHost generates one at runtime per window, so this can't be registered ahead of time and has to happen per open instead */
	void OnAssetEditorOpened(UObject* InAsset, class IAssetEditorInstance* InInstance);

	/** Builds the dropdown menu content listing all local branches (called when the combo button is opened) */
	TSharedRef<SWidget> GenerateBranchMenu();

	/** Prompts for confirmation, then switches to the given branch (hot-reloading Content, or asking for a restart if Source/Config was touched) */
	void OnBranchSelected(FString InBranchName);

	/** Runs an async Sync (pull) - the same worker Content/Source reload and restart-prompt handling as branch switch applies here too */
	void OnSyncClicked();

	/** Deferred RegisterToolbarExtension() trigger - see StartupModule() for why this can't run at module load time */
	void OnMainFrameCreationFinished(TSharedPtr<SWindow> InRootWindow, bool bIsNewProjectWindow);

	/** Refreshes the toolbar widget so the branch switcher's dynamic entry re-evaluates (e.g. once Lore finishes connecting) */
	void RefreshToolbarExtension() const;

	/** Fired when the global Source Control enabled/provider selection changes (Editor Preferences toggle) */
	void OnSourceControlProviderChanged(ISourceControlProvider& OldProvider, ISourceControlProvider& NewProvider);

	/**
	 * Whatever the Submit dialog listed can be stale by the time it closes, so re-scans it once it
	 * does - regardless of accept or cancel, since neither actually refreshes our cached state.
	 */
	void OnWindowBeingDestroyed(const SWindow& Window);

	/** Handle for the state-changed binding that drives RefreshToolbarExtension() */
	FDelegateHandle SourceControlStateChangedHandle;

	/** Handle for the global provider-changed binding that drives RefreshToolbarExtension() */
	FDelegateHandle SourceControlProviderChangedHandle;

	/** Handle for the window-being-destroyed binding that drives OnWindowBeingDestroyed() */
	FDelegateHandle WindowBeingDestroyedHandle;

	/** Menu names already extended with the branch switcher (the level editor, plus one per opened asset editor) - guards against re-adding a duplicate entry if an editor instance opens more than one asset */
	TSet<FName> RegisteredToolbarMenus;
#endif

	/** The one and only source control provider */
	FLoreSourceControlProvider LoreSourceControlProvider;
};
