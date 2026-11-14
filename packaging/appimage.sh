#!/usr/bin/env bash
# Builds the launcher as an AppImage, the way Tauri's bundler does: linuxdeploy with its GTK
# plugin bundles GTK and WebKitGTK, WebKitGTK's helper processes go in AppDir/usr/lib, and the
# plugin rewrites the library's built-in path to them to a relative one ("/usr/lib/..." ->
# "././/lib/..."), which works because the AppRun starts the launcher in AppDir/usr.
#
#   packaging/appimage.sh            -> build/appimage/STAR_FORGE_Launcher-<version>-x86_64.AppImage
#
# Needs the Windows sc-inject.exe and sc_agent.dll (they go in tools/): from the win64 build, or
# from WIN_TOOLS=<folder> (CI builds them on Windows). And linuxdeploy, its appimage plugin,
# Tauri's fork of its gtk plugin (upstream's doesn't patch WebKitGTK) and AppImageKit's AppRun:
# by default Tauri's cached copies in ~/.cache/tauri (TOOLS=<folder> to use others).
#
# The AppImage runs on systems with this one's glibc or newer: build releases on an old distro.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
tools="${TOOLS:-$HOME/.cache/tauri}"
out="$root/build/appimage"
appdir="$out/AppDir"
version="$(sed -n 's/^project(starforge VERSION \([0-9.]*\).*/\1/p' "$root/CMakeLists.txt")"

for f in linuxdeploy-x86_64.AppImage linuxdeploy-plugin-gtk.sh linuxdeploy-plugin-appimage.AppImage AppRun-x86_64; do
    [ -e "$tools/$f" ] || { echo "missing $tools/$f" >&2; exit 1; }
done
# linuxdeploy finds its plugins on PATH.
export PATH="$tools:$PATH"
chmod +x "$tools"/linuxdeploy-x86_64.AppImage "$tools"/linuxdeploy-plugin-*

cmake --build --preset linux --target star_forge
win_tools="${WIN_TOOLS:-}"
if [ -z "$win_tools" ]; then
    cmake --build --preset win64 --target sc_inject sc_agent sc_server
    win_tools="$root/build/win64"
fi

rm -rf "$appdir"
mkdir -p "$appdir/usr/bin/tools" "$appdir/usr/bin/icons" "$appdir/usr/share/applications"
install -m755 "$root/build/linux/star-forge" "$appdir/usr/bin/star-forge"
strip "$appdir/usr/bin/star-forge"
install -m644 "$win_tools/sc-inject.exe" "$win_tools/sc_agent.dll" "$win_tools/sc-server.exe" "$appdir/usr/bin/tools/"
# For "Add to applications menu" (Backend::resources is the program's folder).
cp "$root"/gui/icons/*.png "$appdir/usr/bin/icons/"
for size in 32x32 128x128; do
    install -Dm644 "$root/gui/icons/$size.png" "$appdir/usr/share/icons/hicolor/$size/apps/star-forge.png"
done
install -Dm644 "$root/gui/icons/128x128@2x.png" "$appdir/usr/share/icons/hicolor/256x256/apps/star-forge.png"
cat > "$appdir/usr/share/applications/star-forge.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=STAR FORGE Launcher
Comment=STAR FORGE Launcher: Star Conflict launcher with switchable server profiles
Exec=star-forge %u
Icon=star-forge
StartupWMClass=star-forge
Terminal=false
Categories=Game;
MimeType=x-scheme-handler/starforge;
EOF

# WebKitGTK's helper processes (web, network, GPU) and the bundle it injects into pages.
# They keep their place under usr/ (/usr/lib or /usr/lib/x86_64-linux-gnu, by distro).
libdir="$(pkg-config --variable=libdir webkit2gtk-4.1)"
case "$libdir" in /usr/*) ;; *) echo "unexpected WebKitGTK folder $libdir" >&2; exit 1 ;; esac
webkit="$libdir/webkit2gtk-4.1"
bundled="$appdir/usr/${libdir#/usr/}/webkit2gtk-4.1"
mkdir -p "$bundled"
cp -r "$webkit/WebKitWebProcess" "$webkit/WebKitNetworkProcess" "$webkit/injected-bundle" "$bundled/"
if [ -e "$webkit/WebKitGPUProcess" ]; then cp "$webkit/WebKitGPUProcess" "$bundled/"; fi
helpers=()
for f in "$bundled"/WebKit*Process; do helpers+=(--executable "$f"); done

cd "$out"
# linuxdeploy's own strip is too old for current distros' libraries (as Tauri also finds), so
# only the launcher is stripped, with the system's.
export APPIMAGE_EXTRACT_AND_RUN=1 DEPLOY_GTK_VERSION=3 NO_STRIP=1
linuxdeploy-x86_64.AppImage --appdir "$appdir" \
    --executable "$appdir/usr/bin/star-forge" "${helpers[@]}" \
    --library "$bundled/injected-bundle/libwebkit2gtkinjectedbundle.so" \
    --desktop-file "$appdir/usr/share/applications/star-forge.desktop" \
    --icon-file "$appdir/usr/share/icons/hicolor/128x128/apps/star-forge.png" \
    --custom-apprun "$tools/AppRun-x86_64" \
    --plugin gtk

# The gtk plugin points the bundled WebKitGTK at its own helpers ("/usr/" -> "././/" in the
# library); make sure it did.
lib="$(find "$appdir/usr/lib" -maxdepth 1 -name 'libwebkit2gtk-4.1.so*' -type f | head -1)"
grep -qaF "././/${webkit#/usr/}" "$lib" || { echo "the bundled WebKitGTK still looks for its helpers in $webkit" >&2; exit 1; }

rm -f "$out"/*.AppImage
OUTPUT="STAR_FORGE_Launcher-$version-x86_64.AppImage" linuxdeploy-plugin-appimage.AppImage --appdir "$appdir"
echo "$out/STAR_FORGE_Launcher-$version-x86_64.AppImage"
