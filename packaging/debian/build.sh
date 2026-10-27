#!/bin/sh
# Rufus: The Reliable USB Formatting Utility
# Build the .deb of this commit for the distribution that runs the script (Debian or Ubuntu), into
# the directory of the argument. The build dependencies must be installed (debian/control).
# Copyright © 2026 Rufus for Linux contributors
# SPDX-License-Identifier: GPL-3.0-or-later
set -e
out=$(realpath "${1:-.}")
root=$(realpath "$(dirname "$0")/../..")
version=$("$root/packaging/version.sh")
. /etc/os-release
# 4.16.0-1~debian13, 4.16.0-1~ubuntu26.04: the build of each distribution has its own version
revision="1~$ID$VERSION_ID"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
"$root/packaging/source.sh" "$work" > /dev/null
tar -xzf "$work/rufus-linux-$version.tar.gz" -C "$work"
src="$work/rufus-linux-$version"
cp -r "$root/packaging/debian" "$src/debian"
rm -f "$src/debian/build.sh"
cat > "$src/debian/changelog" <<CHANGELOG
rufus ($version-$revision) unstable; urgency=medium

  * Build of $(git -C "$root" rev-parse --short=8 HEAD) for $PRETTY_NAME.

 -- mikigal <mikigal.priv@gmail.com>  $(date -R)
CHANGELOG
(cd "$src" && dpkg-buildpackage -b -us -uc)
mkdir -p "$out"
cp "$work"/rufus_*.deb "$out/"
ls "$out"/rufus_*.deb
