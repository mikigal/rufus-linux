#!/bin/sh
# Rufus: The Reliable USB Formatting Utility
# The version of the packages: the one of the release tag (v4.16.0 gives 4.16.0), or, for any other
# commit, the version of the project with the date and hash of the commit ("~" sorts it before the
# release in dpkg and rpm)
# Copyright © 2026 Rufus for Linux contributors
# SPDX-License-Identifier: GPL-3.0-or-later
set -e
cd "$(dirname "$0")/.."
project=$(sed -n 's/^[[:space:]]*VERSION \([0-9.]*\)$/\1/p' CMakeLists.txt | head -n 1)
tag=$(git describe --tags --exact-match --match 'v*' 2>/dev/null || true)
if [ -n "$tag" ]; then
	if [ "${tag#v}" != "$project" ]; then
		echo "The tag $tag is not the version of the project ($project)" >&2
		exit 1
	fi
	echo "$project"
else
	echo "$project~git$(git log -1 --format=%cd --date=format:%Y%m%d).$(git rev-parse --short=8 HEAD)"
fi
