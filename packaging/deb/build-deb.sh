#!/usr/bin/env bash
# Builds a .deb on Debian/Ubuntu from this checkout:
#   sudo apt install build-essential dpkg-dev libgtk-3-dev libcurl4-openssl-dev libjson-glib-dev libsecret-1-dev
#   packaging/deb/build-deb.sh
# Runtime dependencies are computed with dpkg-shlibdeps, so build on the oldest
# release you want to support (CI uses Ubuntu 22.04).
set -euo pipefail
cd "$(dirname "$0")/../.."

VERSION=$(sed -n 's/^VERSION *:= *//p' Makefile)
ARCH=$(dpkg --print-architecture)
PKG=deepseek-native
STAGE=packaging/deb/stage
OUT="${PKG}_${VERSION}-1_${ARCH}.deb"

rm -rf "$STAGE"
make clean
make BUILD=release
make install DESTDIR="$PWD/$STAGE" PREFIX=/usr
strip --strip-unneeded "$STAGE/usr/bin/deepseek-native"
install -Dm644 README.md "$STAGE/usr/share/doc/$PKG/README.md"
install -Dm644 THIRD_PARTY_NOTICES.md "$STAGE/usr/share/doc/$PKG/THIRD_PARTY_NOTICES.md"
install -Dm644 LICENSE "$STAGE/usr/share/doc/$PKG/copyright"

# dpkg-shlibdeps needs a debian/control next to it.
mkdir -p "$STAGE/DEBIAN" debian
printf 'Source: %s\n\nPackage: %s\nArchitecture: any\n' "$PKG" "$PKG" > debian/control
DEPENDS=$(dpkg-shlibdeps -O "$STAGE/usr/bin/deepseek-native" 2>/dev/null | sed -n 's/^shlibs:Depends=//p')
rm -rf debian

SIZE=$(du -sk "$STAGE/usr" | cut -f1)
cat > "$STAGE/DEBIAN/control" <<CONTROL
Package: $PKG
Version: $VERSION-1
Architecture: $ARCH
Maintainer: Lukasz Sromek <https://github.com/lszl84>
Installed-Size: $SIZE
Depends: $DEPENDS
Recommends: ripgrep, zstd
Section: devel
Priority: optional
Homepage: https://github.com/lszl84/deepseek-native-linux
Description: Fast native GTK3 client for the DeepSeek Harness coding agent
 A native Linux desktop app for the DeepSeek Harness coding agent, written
 in C with GTK 3. It runs the agent loop and its tools in one small process,
 sandboxes shell commands with Landlock, and follows your GTK theme.
CONTROL

dpkg-deb --root-owner-group --build "$STAGE" "$OUT"
rm -rf "$STAGE"
echo "Built $OUT"
