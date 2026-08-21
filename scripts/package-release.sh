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
# `Paths::Base()` reads it that way - and flattening them would put five
# copies of libSDL3 in one directory and four `shaders/render/` that disagree.
# The launcher needs that shape for a second reason: it locates the programs it
# starts at `<archive root>/<program>/<program>`, so the layout is what makes an
# unpacked tarball a working front door rather than five unrelated directories.
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

# The five products. `unified_tests` is a diagnostic harness rather than
# a product and is deliberately not here; RUNNING.md is where it is described.
#
# The launcher travels with the other four rather than instead of them, and the
# archive layout is what makes it work. `launcher::StageRoot` takes the parent
# of the directory the running binary sits in and looks for `<root>/client/client`
# under it - which is exactly the shape below, one directory per program with
# the archive root as their common parent. So the launcher started out of an
# unpacked tarball finds all four; a launcher shipped on its own would open a
# window with every mode greyed out.
programs="client studio server cdn launcher"

# The programs that link `Engine::examples`, and therefore the ones the demo
# scenes have to travel with. `cdn` does not, and a copy of them in its
# directory would be a claim that it can run one. Neither does `launcher`: it
# starts other programs and opens no scene itself, and each child resolves the
# scenes from its own directory.
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
		#
		# **`.ilk` goes with it, and it is the larger half by a long way.** That
		# is the incremental linker's database - state for the *next* link, of no
		# use to anything that runs the program. The first MSVC build to get this
		# far shipped 795.7 MB of them beside 106.9 MB of actual executable:
		# `studio.ilk` alone was 271 MB against a 35.8 MB `studio.exe`, and the
		# archive was three and a half times the size it needed to be.
		#
		# It was never noticed because no MSVC build had ever reached the
		# packaging step before v0.18.0. A mingw build produces none of these.
		find "$work/$name" \( -name '*.pdb' -o -name '*.ilk' \) -delete

		# **A GCC build of the same platform is the other case, and it does not
		# look like one until you weigh it.** mingw-w64 puts DWARF *inside* the
		# PE exactly as it does inside an ELF, so the .pdb sweep above finds
		# nothing and every binary ships its debug information: `studio.exe` is
		# 1227 MB unstripped.
		#
		# That needs a strip that understands PE. A Linux host's own `strip` is
		# configured for ELF and refuses the file, so the cross one is looked
		# for first and the plain name is the fallback for a Windows runner,
		# where it is whatever MSYS provides.
		#
		# Not fatal when there is none, and not fatal when it fails: an MSVC
		# build has nothing here to remove and must not be held up over a tool
		# it does not need. It says so on the way past rather than silently
		# producing an archive twenty times the size it should be.
		windows_strip=${STRIP:-}
		if [ -z "$windows_strip" ]; then
			for candidate in x86_64-w64-mingw32-strip strip; do
				if command -v "$candidate" > /dev/null 2>&1; then
					windows_strip=$candidate
					break
				fi
			done
		fi

		if [ -z "$windows_strip" ]; then
			echo "no strip found - shipping Windows binaries as they are." >&2
		else
			for program in $programs; do
				directory="$work/$name/$program"
				for binary in "$directory/$program.exe" "$directory"/*.dll; do
					[ -f "$binary" ] || continue
					"$windows_strip" --strip-debug "$binary" ||
						echo "  $windows_strip could not strip $binary" >&2
				done
			done
		fi
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
#
# Three images, and the third is the whole engine. `client` and `studio` are
# each one windowed program and stand alone. `launcher` carries the other four
# with it, because a launcher that cannot see them is a window with every mode
# greyed out - package-appimage.sh explains the AppDir shape that makes
# `StageRoot` find them. `server` and `cdn` get none: they are daemons, they are
# inside the launcher image already, and they are in the tarball above.
case $platform in
	linux-*)
		"$root/scripts/package-appimage.sh" client "$work/$name/client" "$version" "$absolute"
		"$root/scripts/package-appimage.sh" studio "$work/$name/studio" "$version" "$absolute"
		"$root/scripts/package-appimage.sh" launcher "$work/$name/launcher" "$version" "$absolute" \
			"$work/$name/client" "$work/$name/studio" "$work/$name/server" "$work/$name/cdn"
		;;
esac
