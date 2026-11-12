#!/bin/bash
# Build the plugin, install it into Tests/HostProject, run an Automation test filter headless,
# and print each result. Exit 0 only when every test that ran passed.
#
#   Tools/run-tests.sh [filter]        default filter: the plugin's own test prefix
#
# Env: UE_ROOT (default: the Epic launcher's 5.8 on this OS), LORE_BIN_DIR (put a lore first on
# PATH for the native tests), KEEP_BUILD=1 to reuse the last build.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
plugin="$(ls "$here"/*.uplugin | head -1)"
name="$(basename "$plugin" .uplugin)"
filter="${1:-$( [ "$name" = Lorepit ] && echo Lorepit || echo LoreSourceControl )}"
case "$(uname)" in
  Darwin) ue="${UE_ROOT:-/Users/Shared/Epic Games/UE_5.8}"; platform=Mac
          editor="$ue/Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor"
          logdir="$HOME/Library/Logs/Unreal Engine/HostProjectEditor";;
  Linux)  ue="${UE_ROOT:?set UE_ROOT}"; platform=Linux; editor="$ue/Engine/Binaries/Linux/UnrealEditor"
          logdir="$here/Tests/HostProject/Saved/Logs";;
  *) echo "use Tools/run-tests.ps1 on Windows" >&2; exit 2;;
esac
out="$here/Tests/HostProject/Saved/PluginBuild"; mkdir -p "$here/Tests/HostProject/Saved"
if [ "${KEEP_BUILD:-}" != 1 ] || [ ! -d "$out/Binaries" ]; then
  rm -rf "$out"
  "$ue/Engine/Build/BatchFiles/RunUAT.sh" BuildPlugin -Plugin="$plugin" -Package="$out" -TargetPlatforms=$platform -Rocket > "$here/Tests/HostProject/Saved/build.log" 2>&1 \
    || { tail -40 "$here/Tests/HostProject/Saved/build.log"; echo "BUILD FAILED"; exit 1; }
fi
rm -rf "$here/Tests/HostProject/Plugins"; mkdir -p "$here/Tests/HostProject/Plugins"
cp -R "$out" "$here/Tests/HostProject/Plugins/$name"
rm -rf "$here/Tests/HostProject/Plugins/$name/Intermediate"
[ -n "${LORE_BIN_DIR:-}" ] && export PATH="$LORE_BIN_DIR:$PATH"
rm -f "$logdir/HostProject.log"
"$editor" "$here/Tests/HostProject/HostProject.uproject" -unattended -nullrhi -nosplash -nosound -nopause -nocrashreports \
  -ExecCmds="Automation RunTests $filter" -TestExit="Automation Test Queue Empty" -log >/dev/null 2>&1 || true
log="$logdir/HostProject.log"
# A crashed run must not leave a crash reporter behind.
pkill -f "CrashReportClient.*CrashReport-UE-HostProject-" 2>/dev/null || true
[ -f "$log" ] || { echo "no editor log at $log (the editor probably crashed)"; exit 1; }
grep -E "Test Completed\. Result=" "$log" | sed -E 's/.*Result=\{([A-Za-z]+)\}.*Path=\{([^}]*)\}.*/\1\t\2/' | sort -k2 > "$here/Tests/HostProject/Saved/results.tsv" || true
pass=$(grep -c "^Success" "$here/Tests/HostProject/Saved/results.tsv" || true); fail=$(grep -c -v "^Success" "$here/Tests/HostProject/Saved/results.tsv" || true)
cat "$here/Tests/HostProject/Saved/results.tsv"
echo "---"; echo "$pass passed, $fail failed (filter: $filter; log: $log)"
if grep -q "Assertion failed\|=== Critical error: ===" "$log"; then echo "THE EDITOR CRASHED"; grep -m3 -A2 "Assertion failed\|Critical error" "$log"; exit 1; fi
[ "$fail" = 0 ] && [ "$pass" -gt 0 ]
