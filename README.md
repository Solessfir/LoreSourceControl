# Lore Source Control

Unreal Engine plugin implementing `ISourceControlProvider` for [lore](https://github.com/EpicGames/lore)

![Editor Screenshot](Resources/Screenshot.png)

## Supported Platforms

- Windows
- Linux
- macOS

## Features

- **Sync** - pull latest (`lore sync`) from the toolbar dropdown, with progress and completion feedback
- **Commit** - stages and commits in one step, then auto-pushes when a remote is configured
- **Lock / Unlock** - advisory file locking (`lore lock acquire` / `lore lock release`)
- **Branch Switching** - toolbar dropdown next to the Revision Control icon, with unsaved-work protection, progress feedback, and asset reload or restart handling
- **File History** - revision browsing, diffing, and "Diff Against Depot"
- **Status Tracking** - live Content Browser and asset-dialog icons
- **Revert / Add** - standard source control workflow, fully wired up

## Requirements

- Lore CLI available, either on your `PATH` or at an explicit path set in Project Settings. Versions outside the tested 0.8.6 through 0.8.x range are allowed with a warning.
- A Lore repository with a `.lore` directory in the Unreal project directory or one of its parents. Configure an identity for commits and a remote URL for sync, push, and locks.

## Installation

Clone into your project's `Plugins/` folder and build.

Prebuilt binary available on [Fab](https://fab.com/s/34bd22b20f98) if you'd like to support the project.

## Create or Clone a Lore Repository

Clone a remote repository and store the commit identity in `.lore/config.toml`:

```bash
lore clone --identity your-name lore://server.example.com:41337/my-project MyProject
```

To initialize the current directory as a fully local repository without a server, run:

```bash
lore repository create --offline --identity your-name my-project
```

Lore does not use a separate `init` command. The offline form above creates the `.lore` directory in the current directory and leaves `remote_url` empty. The standard Submit action commits locally and skips pushing when no remote is configured. Lore locks are also skipped automatically because they require a server.

## Getting Started

1. **Revision Control** (bottom right of the main editor window) → **Connect to Revision Control...**
2. In the popup: **Provider** → select **Lore**.
3. If Lore is installed correctly, the binary path auto-detects.
4. **Accept Settings**. All set - a branch/actions dropdown appears next to the Revision Control icon.

## Configuration

**Project Settings → Plugins → Lore Source Control**:

- **Lore Path** - override the `lore` binary location. Leave empty to auto-detect from `PATH`, then common install locations.
- **Use Lore Locks On Check Out** - acquire advisory Lore locks during Check Out. Files remain editable, and locks are released after submit or revert.

## Console Commands

- **`LoreSync`** - performs a Lore sync (pull) and updates source control states.
- **`LoreStatus`** - force-refreshes Lore source control status for the project and prints Lore's human-readable status to the log.
- **`LoreCommit`** - opens the Submit Files dialog to stage and commit pending changes, then pushes when a remote is configured.

## Automated Tests

Parser and behavior tests live in the separate `LoreSourceControlTests` editor module. Run the `LoreSourceControl` test group from Unreal's Session Frontend Automation tab or from the command line:

```text
UnrealEditor-Cmd.exe YourProject.uproject -unattended -nullrhi -ExecCmds="Automation RunTests LoreSourceControl; Quit" -TestExit="Automation Test Queue Empty"
```

## Notes

- **Locking is advisory, not enforced.** A lock is a courtesy signal to teammates, not a hard guarantee. Lore's server never rejects a commit or push from someone who skipped locking entirely, and files stay editable regardless of lock state, matching Git's model rather than Perforce's.

## License

MIT, see [LICENSE](LICENSE). This plugin talks to a separately installed `lore` binary rather than bundling it. Lore itself is [MIT-licensed](https://github.com/EpicGames/lore) too, under its own terms.
