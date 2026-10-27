#!/bin/sh
# Rufus: The Reliable USB Formatting Utility
# The source archive of a version: rufus-linux-VERSION.tar.gz, with a rufus-linux-VERSION directory
# Copyright © 2026 Rufus for Linux contributors
# SPDX-License-Identifier: GPL-3.0-or-later
set -e
out=${1:-.}
cd "$(dirname "$0")/.."
version=$(packaging/version.sh)
mkdir -p "$out"
git archive --format=tar.gz --prefix="rufus-linux-$version/" -o "$out/rufus-linux-$version.tar.gz" HEAD
echo "$out/rufus-linux-$version.tar.gz"
