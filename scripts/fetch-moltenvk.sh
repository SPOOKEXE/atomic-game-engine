#!/usr/bin/env bash
# Fetch the pinned macOS MoltenVK runtime that SDL loads dynamically.

set -euo pipefail

version=1.4.2
expected=f95765a6229cb7b915990a2890ce12ebe36a730b021545d3d52ae69ce4c4024e
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cache="$root/.cache/vendor/moltenvk/$version"
archive="$cache/MoltenVK-macos.tar"
library="$cache/MoltenVK/MoltenVK/dynamic/dylib/macOS/libMoltenVK.dylib"

if [ -f "$library" ]; then
	printf '%s\n' "$cache/MoltenVK"
	exit 0
fi

mkdir -p "$cache"
if [ ! -f "$archive" ]; then
	curl -fL --retry 3 \
		"https://github.com/KhronosGroup/MoltenVK/releases/download/v${version}/MoltenVK-macos.tar" \
		-o "$archive"
fi

actual=$(cmake -E sha256sum "$archive" | awk '{print $1}')
if [ "$actual" != "$expected" ]; then
	echo "MoltenVK $version checksum mismatch: $actual" >&2
	exit 1
fi

tar xf "$archive" -C "$cache"
if [ ! -f "$library" ]; then
	echo "MoltenVK $version archive did not contain the macOS dylib" >&2
	exit 1
fi

printf '%s\n' "$cache/MoltenVK"
