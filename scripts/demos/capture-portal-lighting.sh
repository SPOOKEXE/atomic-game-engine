#!/usr/bin/env bash
#
# Four stills of the portal lighting scenes, and the counts that say light
# crossed the seams.
#
#   scripts/demos/capture-portal-lighting.sh                    # all four
#   scripts/demos/capture-portal-lighting.sh PortalLightOut far # one of them
#   OUT=/tmp/shots scripts/demos/capture-portal-lighting.sh
#
# **A measurement rather than a screenshot to squint at**, the arrangement
# `capture-portal-seam.sh` proves out. `PortalLightOut.luau` stands one amber
# lamp in a pane with the sun off, so the far room is lit by the transported
# copy or by nothing; `PortalLightMix.luau` stands a red lamp on one side and a
# green one on the other, so each room's pool holds a channel that can only
# have arrived through the hole. `portal-lighting-report.py` counts both.
#
# `mono.client/tests/PortalLighting.cpp` asserts the same scenes' light lists
# headlessly; this script is the half only a GPU can answer - the lit pixels.
#
set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$here/../.." && pwd)

preset=${PRESET:-dev}
build="$root/.cache/build/$preset"
out=${OUT:-$build/captures}
skip_build=${SKIP_BUILD:-0}

if [ "$skip_build" != 0 ] && [ "$skip_build" != 1 ]; then
	echo "SKIP_BUILD must be 0 or 1" >&2
	exit 1
fi

if [ "$skip_build" = 0 ]; then
	cmake -S "$root" --preset "$preset" > /dev/null
	cmake --build "$build" --target client
elif [ ! -x "$build/client/client" ]; then
	echo "no prebuilt client at $build/client/client" >&2
	exit 1
fi

mkdir -p "$out"

capture() {
	local scene="$1"
	local view="$2"

	local source="$build/assets/examples/scripts/$scene.luau"
	if [ ! -f "$source" ]; then
		echo "no staged scene at $source" >&2
		exit 1
	fi

	# **The line is rewritten rather than a global set.** `_G` is readonly in
	# the script sandbox, so a scene cannot be told anything from outside except
	# by editing it - see the note beside `VIEW` in the scenes.
	#
	# **Whatever the scene's own default is, not only an empty one.** A scene
	# that picks a sensible viewpoint when nobody says otherwise is a scene that
	# shows something when it is run by hand, and matching only `""` made this
	# script fail the moment one of them did. The guard below is what turns that
	# into a message rather than a capture of the wrong view.
	local fixture="$out/fixtures/portal-lighting-$scene-$view"
	mkdir -p "$fixture"
	local staged="$fixture/$scene.luau"
	sed "s/^local VIEW = \"[^\"]*\"$/local VIEW = \"$view\"/" "$source" > "$staged"

	if ! grep -q "^local VIEW = \"$view\"$" "$staged"; then
		echo "  the view line in $scene.luau moved; this script did not follow" >&2
		exit 1
	fi

	local shot="$out/portal-lighting-$scene-$view.bmp"
	local runtime_log="$out/portal-lighting-$scene-$view.runtime.log"
	rm -f "$shot"

	echo "capturing $scene $view"
	# Keep the log and require a clean client exit; a BMP can outlive a failure.
	if timeout --signal=KILL 120 "$build/client/client" \
		--script "$staged" --frames 30 --capture "$shot" > "$runtime_log" 2>&1; then
		:
	else
		local status=$?
		echo "  client failed with status $status; log: $runtime_log" >&2
		tail -n 40 "$runtime_log" >&2
		exit "$status"
	fi

	if [ ! -f "$shot" ]; then
		echo "  no capture written - run the client by hand to see why" >&2
		exit 1
	fi

	python3 "$here/portal-lighting-report.py" "$shot"
}

if [ $# -ge 2 ]; then
	capture "$1" "$2"
else
	for scene in PortalLightOut PortalLightMix; do
		for view in near far; do
			capture "$scene" "$view"
		done
	done
fi

echo "stills in $out"
