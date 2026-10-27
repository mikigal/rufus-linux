#!/bin/sh
# Rufus: The Reliable USB Formatting Utility
# Build the portable tree of this commit, with its own Qt and libraries (linuxdeploy and its Qt
# plugin), then the AppImage and the archive of the tree (tar.gz), into the directory of the
# argument. $LINUXDEPLOY is linuxdeploy, with its Qt and AppImage plugins in the PATH, and qmake is
# the one of the Qt to bundle. Built on the oldest base that has what Rufus needs (Ubuntu 24.04),
# so that it runs on the distributions that have the same or a newer glibc.
# Copyright © 2026 Rufus for Linux contributors
# SPDX-License-Identifier: GPL-3.0-or-later
set -e
out=$(realpath "${1:-.}")
root=$(realpath "$(dirname "$0")/../..")
version=$("$root/packaging/version.sh")
name="rufus-$version-x86_64"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cmake -S "$root" -B "$work/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
	-DRUFUS_BUILD_TESTS=OFF
cmake --build "$work/build"
DESTDIR="$work/AppDir" cmake --install "$work/build"

# Qt, with its Wayland and X11 platforms, and the platform theme of the desktop portal (file dialogs,
# colour scheme): the platform themes of KDE and GNOME are built for the Qt of the system
export QMAKE="${QMAKE:-qmake}"
export EXTRA_PLATFORM_PLUGINS="libqwayland-generic.so;libqwayland-egl.so"
export EXTRA_QT_PLUGINS="platformthemes;wayland-shell-integration;wayland-decoration-client;wayland-graphics-integration-client"
export LDAI_OUTPUT="$out/$name.AppImage"
mkdir -p "$out"
"$LINUXDEPLOY" --appdir "$work/AppDir" \
	--executable "$work/AppDir/usr/bin/rufus" \
	--executable "$work/AppDir/usr/bin/rufus-cli" \
	--deploy-deps-only "$work/AppDir/usr/libexec/rufus-helper" \
	--desktop-file "$work/AppDir/usr/share/applications/rufus.desktop" \
	--icon-file "$work/AppDir/usr/share/icons/hicolor/256x256/apps/rufus.png" \
	--custom-apprun "$root/packaging/portable/AppRun" \
	--plugin qt --output appimage

# The helper finds its libraries next to it, as pkexec drops LD_LIBRARY_PATH
readelf -d "$work/AppDir/usr/libexec/rufus-helper" | grep -q 'R.*PATH.*\$ORIGIN/../lib' || {
	echo "rufus-helper has no RUNPATH to the libraries of the tree" >&2
	exit 1
}

# The archive: the same tree
cp -a "$work/AppDir" "$work/$name"
tar -czf "$out/$name.tar.gz" -C "$work" --owner=0 --group=0 "$name"
ls -l "$out/$name.AppImage" "$out/$name.tar.gz"
