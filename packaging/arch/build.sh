#!/bin/sh
# Rufus: The Reliable USB Formatting Utility
# Build the Arch Linux package rufus (from the source of this commit) or rufus-bin (from the portable
# archive of packaging/portable/build.sh, the second argument), into the directory of the third one,
# with their PKGBUILDs (those of the AUR). As a user, with the build dependencies installed.
# Copyright © 2026 Rufus for Linux contributors
# SPDX-License-Identifier: GPL-3.0-or-later
set -e
package=$1
root=$(realpath "$(dirname "$0")/../..")
version=$("$root/packaging/version.sh")
# pacman's version comparison does not know "~"
pkgver=$(echo "$version" | tr '~' '_')
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
case "$package" in
rufus)
	out=$(realpath "${2:-.}")
	git -C "$root" archive --format=tar.gz --prefix="rufus-linux-$pkgver/" -o "$work/rufus-linux-$pkgver.tar.gz" HEAD
	source="rufus-linux-$pkgver.tar.gz"
	;;
rufus-bin)
	out=$(realpath "${3:-.}")
	mkdir "$work/tree"
	tar -xzf "$2" -C "$work/tree"
	mv "$work/tree/"* "$work/tree/rufus-$pkgver-x86_64"
	tar -czf "$work/rufus-$pkgver-x86_64.tar.gz" -C "$work/tree" "rufus-$pkgver-x86_64"
	source="rufus-$pkgver-x86_64.tar.gz"
	;;
*)
	echo "Usage: $0 rufus [OUT] | rufus-bin ARCHIVE [OUT]" >&2
	exit 2
	;;
esac
sed -e "s/^pkgver=.*/pkgver=$pkgver/" -e "s|^source=.*|source=(\"$source\")|" \
	"$root/packaging/arch/$package/PKGBUILD" > "$work/PKGBUILD"
(cd "$work" && makepkg -f --nodeps --noconfirm)
mkdir -p "$out"
# Without the package of the debugging symbols
cp "$work/$package-$pkgver"-*.pkg.tar.zst "$out/"
ls "$out/$package-$pkgver"-*.pkg.tar.zst
