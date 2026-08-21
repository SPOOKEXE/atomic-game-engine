#!/usr/bin/env bash
#
# The release archive for one platform, from a built `release` preset.
#
#   scripts/package-release.sh .cache/build/release 0.18.0 linux-x86_64 dist
#
# Runs on Linux, macOS and Windows - git-bash on a Windows runner is a bash, and
# the archive is written by `cmake -E tar`, which is the one archiver guaranteed
# to be on a machine that has just finished a CMake build. That is why this is
# not three scripts.
#
# What goes in: the staged directory of every program, as it stands. Nothing is
# rearranged into a bin/lib/share shape, because each staged tree is already
# runnable where it sits - `mono_add_program` builds it that way and
# `Paths::Base()` reads it that way - and flattening them would put four
# copies of libSDL3 in one directory and four `shaders/render/` that disagree.
#
# On Linux it also writes the AppImages. See scripts/package-appimage.sh.
#
# Debug information is stripped out of the copy that ships. The `release` preset
# is RelWithDebInfo, which puts 456 MB of DWARF in a 478 MB `client` - a
# seventeen-fold download for something a player never opens. `--strip-debug`
# rather than a full strip, so `.symtab` survives and a backtrace still has
# function names in it. Nothing is published in its place: the tag builds the
# same binary with the same symbols, and a debug archive nobody downloads is
# half a gigabyte per platform per release.

set -euo pipefail

build=${1:?build directory}
version=${2:?version}
platform=${3:?platform label, e.g. linux-x86_64}
outdir=${4:?output directory}

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

# The four products. `unified_tests` is a diagnostic harness rather than
# a product and is deliberately not here; RUNNING.md is where it is described.
programs="client studio server cdn"

# The programs that link `Engine::examples`, and therefore the ones the demo
# scenes have to travel with. `cdn` does not, and a copy of them in its
# directory would be a claim that it can run one.
scene_programs="client studio server"

name="atomic-$version-$platform"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/$name"

missing=""
for program in $programs; do
	if [ ! -d "$build/$program" ]; then
		missing="$missing $program"
		continue
	fi
	cp -a "$build/$program" "$work/$name/$program"
done

if [ -n "$missing" ]; then
	echo "not staged in $build:$missing" >&2
	echo "  The release preset builds all four. A missing one is a build that stopped early." >&2
	exit 1
fi

# The demo scenes, into each program that can run one.
#
# **The staged tree is not self-contained without this**, which is the one place
# the build's layout and the runtime's disagree. Shaders stage into the
# program's own directory and `Paths::Assets()` defaults to that directory, so
# those line up; the example scenes stage into `<build>/assets/examples`, a
# *sibling* of every program directory, and `examples::ExamplePath` reaches them
# through a `Base().parent_path()` fallback it documents as a mismatch. That
# fallback holds in a build tree and nowhere else - a client copied anywhere on
# its own starts, opens Vulkan, and dies with "could not open .../Rings.luau".
#
# Copying into `<stage>/examples` puts them where `ExamplePath` looks *first*,
# so the shipped tree needs no fallback and no `--assets`.
#
# `panels/` is staged beside them and is deliberately not copied: nothing loads
# it yet - mono.studio/CMakeLists.txt calls them "not yet the editor's panels" -
# and shipping a directory no program opens is shipping a claim. Add it here
# when something reads it.
scenes="$build/assets/examples"
if [ ! -d "$scenes" ]; then
	echo "no example scenes at $scenes" >&2
	echo "  Engine::examples stages them during the build; an absent tree is a partial build." >&2
	exit 1
fi
for program in $scene_programs; do
	cp -a "$scenes" "$work/$name/$program/examples"
done

# Named explicitly rather than found by walking the tree and testing each file
# for an object header. The list is short and known - one binary and whatever
# shared libraries were staged beside it - and `strip` handed something it does
# not recognise is a thing to not find out about in a release.
case $(uname -s) in
	Darwin) strip_flags="-S" ;;
	*)      strip_flags="--strip-debug" ;;
esac

case $platform in
	windows-*)
		# MSVC keeps debug information in a .pdb beside the binary rather than
		# inside it, so there is nothing to strip and the .pdb simply does not
		# ship.
		find "$work/$name" -name '*.pdb' -delete
		;;
	*)
		for program in $programs; do
			directory="$work/$name/$program"
			strip $strip_flags "$directory/$program"
			for library in "$directory"/*.so* "$directory"/*.dylib; do
				[ -f "$library" ] || continue
				strip $strip_flags "$library"
			done
		done
		;;
esac

# The licence travels with the binaries or the licence is not served. MPL-2.0
# and the vendored notices both.
cp "$root/LICENSE" "$root/THIRD_PARTY_NOTICES.md" "$root/README.md" "$work/$name/"

printf '%s\n' "$version" > "$work/$name/VERSION"

mkdir -p "$outdir"
absolute=$(cd "$outdir" && pwd)

case $platform in
	windows-*)
		archive="$absolute/$name.zip"
		(cd "$work" && cmake -E tar cf "$archive" --format=zip -- "$name")
		;;
	*)
		archive="$absolute/$name.tar.gz"
		(cd "$work" && cmake -E tar czf "$archive" -- "$name")
		;;
esac
echo "$archive"

# From the staged copy rather than from `$build`, so the AppImages carry the
# same stripped binaries the tarball does. Pointed at the build tree they were
# 171 MB and 214 MB for no reason anybody wanted.
case $platform in
	linux-*)
		"$root/scripts/package-appimage.sh" client "$work/$name/client" "$version" "$absolute"
		"$root/scripts/package-appimage.sh" studio "$work/$name/studio" "$version" "$absolute"
		;;
esac
