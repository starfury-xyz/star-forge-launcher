#!/usr/bin/env bash
# Downloads the umu-launcher zipapp the Linux packages bundle as tools/umu-run (the default way the
# launcher runs the game; see src/launch/launch.hpp), checks it against the pinned hash, and
# prints the folder holding umu-run and its notice. Cached in build/umu/<version>.
#
#   packaging/fetch-umu.sh   -> build/umu/1.4.4
#
# umu-launcher is GPL-3.0 (https://github.com/Open-Wine-Components/umu-launcher); it is shipped
# unmodified next to the launcher, which only runs it, with a notice saying where its source is.
# To update: change version and sha256 (the release's umu-launcher-<version>-zipapp.tar).
set -euo pipefail

version=1.4.4
sha256=eb590691841f7fad3fc3ad8fd5db4ccb87849fe7948e62b28ece7a4ee48cc851

root="$(cd "$(dirname "$0")/.." && pwd)"
out="$root/build/umu/$version"
if [ ! -x "$out/umu-run" ]; then
    tmp="$(mktemp -d)"
    trap 'rm -rf "$tmp"' EXIT
    url="https://github.com/Open-Wine-Components/umu-launcher/releases/download/$version/umu-launcher-$version-zipapp.tar"
    curl -fsSLo "$tmp/umu.tar" "$url"
    echo "$sha256  $tmp/umu.tar" | sha256sum -c --quiet
    tar -xf "$tmp/umu.tar" -C "$tmp" umu/umu-run
    mkdir -p "$out"
    install -m755 "$tmp/umu/umu-run" "$out/umu-run"
    cat > "$out/umu-run.NOTICE" <<EOF
umu-run is umu-launcher $version by Open Wine Components, unmodified, licensed under the GNU
General Public License version 3 (https://www.gnu.org/licenses/gpl-3.0.html).
Source: https://github.com/Open-Wine-Components/umu-launcher/tree/$version
        https://github.com/Open-Wine-Components/umu-launcher/archive/refs/tags/$version.tar.gz
Release file: $url (sha256 $sha256)
EOF
fi
echo "$out"
