#!/usr/bin/env bash
# Builds the launcher's .deb and .rpm, laid out as Tauri's (same package name, the tools in
# /usr/lib/STAR FORGE Launcher/tools), so either upgrades an installed star-forge package:
#
#   packaging/linux-packages.sh   -> build/packages/STAR_FORGE_Launcher_<version>_amd64.deb
#                                    build/packages/STAR_FORGE_Launcher-<version>-1.x86_64.rpm
#
# Needs dpkg-deb, dpkg-shlibdeps and rpmbuild (Ubuntu: apt install dpkg-dev rpm), and the
# Windows sc-inject.exe and sc_agent.dll: from the win64 build, or from WIN_TOOLS=<folder>.
#
# The launcher runs on systems with this one's glibc or newer: build releases on an old distro.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
out="$root/build/packages"
version="$(sed -n 's/^project(starforge VERSION \([0-9.]*\).*/\1/p' "$root/CMakeLists.txt")"
summary="STAR FORGE Launcher: Star Conflict launcher with switchable server profiles"

for tool in dpkg-deb dpkg-shlibdeps rpmbuild; do
    command -v "$tool" >/dev/null || { echo "missing $tool" >&2; exit 1; }
done

cmake --build --preset linux --target star_forge
win_tools="${WIN_TOOLS:-}"
if [ -z "$win_tools" ]; then
    cmake --build --preset win64 --target sc_inject sc_agent sc_server
    win_tools="$root/build/win64"
fi

# The installed files, shared by both packages.
files="$out/files"
rm -rf "$out"
mkdir -p "$files/usr/bin" "$files/usr/lib/STAR FORGE Launcher/tools" "$files/usr/share/applications"
install -m755 "$root/build/linux/star-forge" "$files/usr/bin/star-forge"
strip "$files/usr/bin/star-forge"
install -m644 "$win_tools/sc-inject.exe" "$win_tools/sc_agent.dll" "$win_tools/sc-server.exe" "$files/usr/lib/STAR FORGE Launcher/tools/"
umu="$("$root/packaging/fetch-umu.sh")"
install -m755 "$umu/umu-run" "$files/usr/lib/STAR FORGE Launcher/tools/"
install -m644 "$umu/umu-run.NOTICE" "$files/usr/lib/STAR FORGE Launcher/tools/"
for size in 32x32 128x128; do
    install -Dm644 "$root/gui/icons/$size.png" "$files/usr/share/icons/hicolor/$size/apps/star-forge.png"
done
install -Dm644 "$root/gui/icons/128x128@2x.png" "$files/usr/share/icons/hicolor/256x256/apps/star-forge.png"
# %u: starforge:// links arrive as the argument. (Installed under /usr, the launcher leaves the
# link registration to this file.)
cat > "$files/usr/share/applications/STAR FORGE Launcher.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=STAR FORGE Launcher
Comment=$summary
Exec=star-forge %u
Icon=star-forge
StartupWMClass=star-forge
Terminal=false
Categories=Game;
MimeType=x-scheme-handler/starforge;
EOF

# --- deb ---
deb="$out/deb"
cp -a "$files" "$deb"
mkdir -p "$deb/DEBIAN" "$out/shlibdeps/debian"
# dpkg-shlibdeps names the packages that hold the libraries star-forge links to.
touch "$out/shlibdeps/debian/control"
depends="$(cd "$out/shlibdeps" && dpkg-shlibdeps -O "$deb/usr/bin/star-forge" | sed -n 's/^shlibs:Depends=//p')"
cat > "$deb/DEBIAN/control" <<EOF
Package: star-forge-launcher
Version: $version
Architecture: amd64
Installed-Size: $(du -sk --exclude=DEBIAN "$deb" | cut -f1)
Maintainer: star-forge
Priority: optional
Section: games
Depends: $depends, python3
Description: $summary
EOF
dpkg-deb --root-owner-group -Zxz --build "$deb" "$out/STAR_FORGE_Launcher_${version}_amd64.deb" >/dev/null

# --- rpm ---
# Requirements by library name only: rpmbuild's own would also ask for this distro's symbol
# versions (libcurl's CURL_OPENSSL_4 on Debian), which other distros' libraries don't declare.
rpmtop="$out/rpmbuild"
mkdir -p "$rpmtop/SPECS"
cat > "$rpmtop/SPECS/star-forge-launcher.spec" <<EOF
Name: star-forge-launcher
Version: $version
Release: 1
Summary: $summary
License: MIT and GPL-3.0-only
BuildArch: x86_64
AutoReqProv: no
Requires: libcurl.so.4()(64bit), libgtk-3.so.0()(64bit), libwebkit2gtk-4.1.so.0()(64bit), libjavascriptcoregtk-4.1.so.0()(64bit), python3
Provides: star-forge-launcher

%description
$summary

%install
cp -a "$files/." "%{buildroot}/"

%files
/usr/bin/star-forge
"/usr/lib/STAR FORGE Launcher"
"/usr/share/applications/STAR FORGE Launcher.desktop"
/usr/share/icons/hicolor/32x32/apps/star-forge.png
/usr/share/icons/hicolor/128x128/apps/star-forge.png
/usr/share/icons/hicolor/256x256/apps/star-forge.png
EOF
rpmbuild -bb --quiet --define "_topdir $rpmtop" --define "_build_id_links none" \
    --define "__strip /bin/true" --define "__brp_mangle_shebangs /bin/true" "$rpmtop/SPECS/star-forge-launcher.spec"
cp "$rpmtop"/RPMS/x86_64/*.rpm "$out/STAR_FORGE_Launcher-${version}-1.x86_64.rpm"

ls -l "$out"/*.deb "$out"/*.rpm
