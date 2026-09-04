#!/usr/bin/env bash
#
# One AppImage from one staged program directory, optionally carrying others.
#
#   scripts/package-appimage.sh client .cache/build/release/client 0.18.0 dist
#   scripts/package-appimage.sh launcher <stage>/launcher 0.18.0 dist \
#       <stage>/client <stage>/studio <stage>/server <stage>/cdn
#
# The staged directory is already the whole payload. `mono_add_program` puts the
# binary, `libSDL3.so`, `shaders/` and `fonts/` in it and links with
# INSTALL_RPATH "$ORIGIN", and `engine::core::Paths::Base()` resolves from the
# running executable rather than the working directory - so the tree needs no
# patching to run from a squashfs mount at an unpredictable path. Copy it into
# the AppDir and the program finds everything beside itself.
#
# **Each stage goes to `AppDir/usr/<program>/`, not to a shared `usr/bin`.**
# Two reasons, and the first applies even to a single-program image: the staged
# trees are not mergeable, because four of them carry their own `libSDL3.so` and
# their own `shaders/render/` compiled from different modules. The second is the
# launcher. `launcher::StageRoot` takes the parent of the directory it is
# running from and looks for `<root>/client/client` beneath it, so putting the
# launcher at `usr/launcher/launcher` makes `usr` the stage root and its
# siblings land exactly where it looks. That is the same shape the tarball has,
# which is the point - one layout, checked by both.
#
# Every argument after the fourth is another staged directory to carry along,
# named after its own basename. Nothing else changes: the entry program is
# always the one AppRun starts, and the extras are there to be found.
#
# Which programs are worth an image, and why the daemons are not. `client` and
# `studio` open a window, and a single file a person can download and run is
# what an AppImage is for. `launcher` opens a window too and is the front door,
# so it ships as an all-in-one carrying the other four - a launcher alone would
# start, find no programs beside it, and grey out every mode it offers. `server`
# and `cdn` get no image of their own: they are daemons driven entirely by
# command-line flags, they are already inside the launcher image and in the
# tarball, and an AppImage of a daemon is a tarball with a mount step in front
# of it.
#
# Command-line arguments reach the program in every case - AppRun forwards
# "$@" - so the flags in RUNNING.md work against an AppImage unchanged.
#
# Known limit: the mount is read only. `client --profile-snapshot` writes
# `frame-graph-snapshot.txt` into Paths::Base() and that write fails inside an
# AppImage. The studio is unaffected - its configuration root is
# ~/Documents/atomic-game-engine/studio, and only the pre-v0.15 fallback paths
# it still *reads* live beside the binary.

set -euo pipefail

program=${1:?program name}
stage=${2:?staged directory}
version=${3:?version}
outdir=${4:?output directory}
shift 4
# Every remaining argument is a staged directory to carry beside the entry
# program. Usually none.
carried=("$@")

if [ ! -x "$stage/$program" ]; then
	echo "no $program binary in $stage - build the release preset first." >&2
	exit 1
fi

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

appdir="$work/AppDir"
mkdir -p "$appdir/usr/$program"
cp -a "$stage/." "$appdir/usr/$program/"

for other in ${carried[@]+"${carried[@]}"}; do
	if [ ! -d "$other" ]; then
		echo "no staged directory at $other" >&2
		exit 1
	fi
	cp -a "$other" "$appdir/usr/$(basename "$other")"
done

# The icon has to sit at the AppDir root under the name the .desktop file's
# Icon= key gives, and again as .DirIcon for file managers that show one before
# anything is mounted. appimagetool warns about a missing .DirIcon and then
# produces an image nothing displays an icon for, which is easy to not notice.
cp "$root/assets/small-icon.png" "$appdir/atomic-$program.png"
cp "$root/assets/small-icon.png" "$appdir/.DirIcon"

case $program in
	studio)   display="Atomic Studio";   categories="Development;IDE;" ;;
	launcher) display="Atomic";          categories="Game;" ;;
	*)        display="Atomic Client";   categories="Game;" ;;
esac

cat > "$appdir/atomic-$program.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=$display
Comment=A 3D game engine for Roblox developers
Exec=$program
Icon=atomic-$program
Categories=$categories
Terminal=false
DESKTOP

# exec rather than a plain call, so the program is PID 1 of the mount and a
# signal sent to the AppImage reaches it rather than a shell that ignores it.
# "$@" forwards the command line - every program here takes flags.
cat > "$appdir/AppRun" <<APPRUN
#!/bin/sh
exec "\$(dirname "\$(readlink -f "\$0")")/usr/$program/$program" "\$@"
APPRUN
chmod +x "$appdir/AppRun"

arch=$(uname -m)

# appimagetool is itself an AppImage, and GitHub runners have no FUSE. The
# extract-and-run flag unpacks it to a temporary directory instead of mounting,
# which is the supported answer and not a workaround.
tool=$work/appimagetool
curl -fsSL -o "$tool" \
	"https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-$arch.AppImage"
chmod +x "$tool"

mkdir -p "$outdir"
output="$outdir/atomic-$program-$version-linux-$arch.AppImage"
ARCH="$arch" "$tool" --appimage-extract-and-run "$appdir" "$output"

echo "$output"
