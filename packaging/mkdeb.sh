#!/bin/sh
# Usage: packaging/mkdeb.sh <binary> <output-dir>
# Builds an APPLaunch-compatible .deb for the CardputerZero.
set -eu

BIN="$1"
OUTDIR="$2"
HERE="$(cd "$(dirname "$0")" && pwd)"
VERSION="${VERSION:-0.1.0}"
PKG=czdoom
PREFIX=/usr/share/APPLaunch
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
chmod 0755 "$STAGE"

install -D -m 0755 "$BIN" "$STAGE$PREFIX/apps/$PKG/czdoom"
"${STRIP:-strip}" "$STAGE$PREFIX/apps/$PKG/czdoom"
install -D -m 0755 "$HERE/czdoom" "$STAGE$PREFIX/bin/czdoom"
install -D -m 0644 "$HERE/czdoom.desktop" "$STAGE$PREFIX/applications/$PKG.desktop"
install -D -m 0644 "$HERE/icon.png" "$STAGE$PREFIX/share/images/$PKG.png"
install -D -m 0644 "$HERE/../LICENSE" "$STAGE/usr/share/doc/$PKG/copyright"

DEPENDS="freedoom | doom-wad"
RECOMMENDS=""
if [ -z "${NOSOUND:-}" ]; then
    DEPENDS="libsdl2-2.0-0, libsdl2-mixer-2.0-0, $DEPENDS"
    RECOMMENDS="timgm6mb-soundfont"
fi

mkdir -p "$STAGE/DEBIAN"
{
    echo "Package: $PKG"
    echo "Version: $VERSION"
    echo "Architecture: arm64"
    echo "Maintainer: czdoom contributors"
    echo "Section: games"
    echo "Priority: optional"
    echo "Installed-Size: $(du -sk "$STAGE" | cut -f1)"
    echo "Depends: $DEPENDS"
    [ -n "$RECOMMENDS" ] && echo "Recommends: $RECOMMENDS"
    echo "Homepage: https://github.com/droomurray/czdoom"
    echo "Description: Doom for the M5Stack CardputerZero"
    echo " A doomgeneric port that draws straight to the 320x170 panel and reads"
    echo " the built-in keyboard. Plays Doom, Doom II and Freedoom WADs."
} >"$STAGE/DEBIAN/control"

mkdir -p "$OUTDIR"
dpkg-deb --root-owner-group -Zxz -b "$STAGE" "$OUTDIR/${PKG}_${VERSION}_arm64.deb"
