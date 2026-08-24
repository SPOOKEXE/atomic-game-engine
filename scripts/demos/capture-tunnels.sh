#!/usr/bin/env bash
#
# Three stills of the tunnels scene, and the numbers that say the walk down
# each one is clear.
#
#   scripts/demos/capture-tunnels.sh          # all three
#   scripts/demos/capture-tunnels.sh long     # one of them
#   OUT=/tmp/shots scripts/demos/capture-tunnels.sh
#
# **The view down a tunnel is the whole of what this scene demonstrates**, and
# it was blocked by the scene's own props: the drifting blocks travelled each
# tunnel's centre line at eye height, so one stood in the mouth on the approach
# and was the first thing in the picture through every pane. `DRIFT_SIDE` moves
# them to one side; this is what photographs the result.
#
# `mono.engine/examples/tests/Scene.cpp` asserts the same claim headlessly, as a
# volume nothing may stand in. That test is the one that runs on every change;
# this script answers the half it cannot - what the tunnel looks like - and is
# the one to run when the portal pass itself has been touched.
#
# The client is killed rather than waited for: there is a shutdown race in the
# GPU teardown that predates the portal pass - `NON-EUCLIDEAN.md` Appendix A -
# and the capture is written well before the exit that hangs.

set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$here/../.." && pwd)

preset=${PRESET:-dev}
build="$root/.cache/build/$preset"
out=${OUT:-$build/captures}

cmake -S "$root" --preset "$preset" > /dev/null
cmake --build "$build" --target client

mkdir -p "$out"

capture() {
	local view="$1"

	local source="$build/assets/examples/Tunnels.luau"
	if [ ! -f "$source" ]; then
		echo "no staged scene at $source" >&2
		exit 1
	fi

	# **The line is rewritten rather than a global set.** `_G` is readonly in
	# the script sandbox, so a scene cannot be told anything from outside except
	# by editing it - see the note beside `VIEW` in the scene.
	local staged="$out/Tunnels-$view.luau"
	sed "s/^local VIEW = \"\"$/local VIEW = \"$view\"/" "$source" > "$staged"

	if ! grep -q "^local VIEW = \"$view\"$" "$staged"; then
		echo "  the view line in Tunnels.luau moved; this script did not follow" >&2
		exit 1
	fi

	# **A fixed frame count, because the blocks are moving.** Their route is a
	# function of elapsed simulated time, so the same number of frames puts them
	# in the same place and two runs are comparable. Thirty is far enough along
	# each route for a block to have entered its tunnel.
	local shot="$out/tunnels-$view.bmp"
	rm -f "$shot"

	echo "capturing $view"
	timeout --signal=KILL 120 "$build/client/client" \
		--script "$staged" --frames 30 --capture "$shot" > /dev/null 2>&1 || true

	if [ ! -f "$shot" ]; then
		echo "  no capture written - run the client by hand to see why" >&2
		exit 1
	fi

	python3 "$here/tunnels-report.py" "$shot"
}

if [ $# -ge 1 ]; then
	capture "$1"
else
	for view in long-north long-south short-north short-south plain; do
		capture "$view"
	done
fi

echo "stills in $out"
