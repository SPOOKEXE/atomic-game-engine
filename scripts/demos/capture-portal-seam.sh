#!/usr/bin/env bash
#
# Three stills of a bar standing in a hole, and the count that says it was cut.
#
#   scripts/demos/capture-portal-seam.sh              # all three views
#   scripts/demos/capture-portal-seam.sh side         # one of them
#   OUT=/tmp/shots scripts/demos/capture-portal-seam.sh
#
# **A measurement rather than a screenshot to squint at.** `PortalSeam.luau`
# stands a ten-stud bar through a ten-stud hole, five studs on each side, beside
# a control bar of the same size well clear of the pane. In the `side` view the
# pane is edge-on and the two bars are broadside, so the straddler's coloured
# pixels are half the control's when the cut is working and equal to them when
# it is not. That ratio is what this prints.
#
# **Ninety frames rather than a handful**, because one of the things a still has
# to show is a particle that has travelled: a plume driven at the hole covers six
# studs in about a second, and a capture taken a third of a second in shows a
# blob sitting on its nozzle. Everything else in the scene is anchored and would
# be the same on frame one.
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

scene="$build/assets/examples/PortalSeam.luau"
if [ ! -f "$scene" ]; then
	echo "no staged scene at $scene" >&2
	exit 1
fi

mkdir -p "$out"

views=("$@")
if [ ${#views[@]} -eq 0 ]; then
	views=(side front far)
fi

for view in "${views[@]}"; do
	# **The line is rewritten rather than a global set.** `_G` is readonly in
	# the script sandbox, so a scene cannot be told anything from outside except
	# by editing it - see the note beside `VIEW` in `PortalSeam.luau`.
	staged="$out/PortalSeam-$view.luau"
	sed "s/^local VIEW = \"\"$/local VIEW = \"$view\"/" "$scene" > "$staged"

	if ! grep -q "^local VIEW = \"$view\"$" "$staged"; then
		echo "  the view line in PortalSeam.luau moved; this script did not follow" >&2
		exit 1
	fi

	shot="$out/portal-seam-$view.bmp"
	rm -f "$shot"

	echo "capturing $view"
	timeout --signal=KILL 120 "$build/client/client" \
		--script "$staged" --frames 90 --capture "$shot" > /dev/null 2>&1 || true

	if [ ! -f "$shot" ]; then
		echo "  no capture written - run the client by hand to see why" >&2
		exit 1
	fi

	python3 "$here/portal-seam-report.py" "$shot"
done

echo "stills in $out"
