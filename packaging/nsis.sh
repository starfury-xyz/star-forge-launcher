#!/usr/bin/env bash
# Builds the Windows installer (packaging/windows/installer.nsi) with makensis, on Linux:
#
#   packaging/nsis.sh   -> build/packages/STAR_FORGE_Launcher_<version>_x64-setup.exe
#
# Needs makensis (Ubuntu: apt install nsis); the Windows star-forge.exe, sc-inject.exe and
# sc_agent.dll, from the win64 build or from WIN_BUILD=<folder>; and Microsoft's WebView2
# bootstrapper, downloaded once to build/MicrosoftEdgeWebview2Setup.exe (or WEBVIEW2=<file>).
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
out="$root/build/packages"
version="$(sed -n 's/^project(starforge VERSION \([0-9.]*\).*/\1/p' "$root/CMakeLists.txt")"

command -v makensis >/dev/null || { echo "missing makensis" >&2; exit 1; }
win="${WIN_BUILD:-}"
if [ -z "$win" ]; then
    cmake --build --preset win64 --target star_forge sc_inject sc_agent
    win="$root/build/win64"
fi
webview2="${WEBVIEW2:-$root/build/MicrosoftEdgeWebview2Setup.exe}"
if [ ! -s "$webview2" ]; then
    curl -fsSLo "$webview2" "https://go.microsoft.com/fwlink/p/?LinkId=2124703"
fi

mkdir -p "$out"
setup="$out/STAR_FORGE_Launcher_${version}_x64-setup.exe"
makensis -V2 -INPUTCHARSET UTF8 \
    -DVERSION="$version" -DEXE="$win/star-forge.exe" -DTOOLS="$win" -DICON="$root/gui/icons/icon.ico" \
    -DWEBVIEW2="$webview2" -DOUTFILE="$setup" "$root/packaging/windows/installer.nsi"
ls -l "$setup"
