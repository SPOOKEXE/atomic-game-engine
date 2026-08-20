#!/usr/bin/env bash
#
# One AppImage from one staged program directory.
#
#   scripts/package-appimage.sh client .cache/build/release/client 0.18.0 dist
#
# The staged directory is already the whole payload. `mono_add_program` puts the
# binary, `libSDL3.so`, `shaders/` and `fonts/` in it and links with
# INSTALL_RPATH "$ORIGIN", and `engine::core::Paths::Base()` resolves from the
# running executable rather than the working directory - so the tree needs no
# patching to run from a squashfs mount at an unpredictable path. Copy it into
# AppDir/usr/bin and the program finds everything beside itself.
#
# Only the two programs that open a window are worth an AppImage. `server` and
# `cdn` are daemons; an AppImage of one is a tarball with a mount step.
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

if [ ! -x "$stage/$program" ]; then
	echo "no $program binary in $stage - build the release preset first." >&2
	exit 1
fi

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

appdir="$work/AppDir"
mkdir -p "$appdir/usr/bin"
cp -a "$stage/." "$appdir/usr/bin/"

# The icon has to sit at the AppDir root under the name the .desktop file's
# Icon= key gives, and again as .DirIcon for file managers that show one before
# anything is mounted. appimagetool warns about a missing .DirIcon and then
# produces an image nothing displays an icon for, which is easy to not notice.
cp "$root/assets/icon.png" "$appdir/atomic-$program.png"
cp "$root/assets/icon.png" "$appdir/.DirIcon"

case $program in
	studio) display="Atomic Studio"; categories="Development;IDE;" ;;
	*)      display="Atomic Client"; categories="Game;" ;;
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
exec "\$(dirname "\$(readlink -f "\$0")")/usr/bin/$program" "\$@"
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
