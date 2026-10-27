#!/bin/sh
# Rufus: The Reliable USB Formatting Utility
# Build the RPM of this commit for the distribution that runs the script (Fedora or openSUSE), into
# the directory of the argument. The build dependencies must be installed (rpm/rufus.spec).
# Copyright © 2026 Rufus for Linux contributors
# SPDX-License-Identifier: GPL-3.0-or-later
set -e
out=$(realpath "${1:-.}")
root=$(realpath "$(dirname "$0")/../..")
version=$("$root/packaging/version.sh")
top=$(mktemp -d)
trap 'rm -rf "$top"' EXIT
mkdir -p "$top/SOURCES"
"$root/packaging/source.sh" "$top/SOURCES" > /dev/null
. /etc/os-release
# The dist tag of openSUSE is empty: the release of the package tells the distributions apart
release_dist=$(rpm --eval '%{?dist}')
if [ -z "$release_dist" ]; then
	case "$ID" in
	opensuse-tumbleweed) release_dist=".tw" ;;
	*) release_dist=".$(echo "$ID$VERSION_ID" | tr -d .-)" ;;
	esac
fi
rpmbuild -bb --define "_topdir $top" --define "rufus_version $version" --define "dist $release_dist" \
	"$root/packaging/rpm/rufus.spec"
mkdir -p "$out"
find "$top/RPMS" -name 'rufus-*.rpm' ! -name '*debug*' -exec cp {} "$out/" \;
ls "$out"/rufus-*.rpm
