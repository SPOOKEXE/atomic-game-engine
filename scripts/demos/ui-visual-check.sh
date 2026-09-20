#!/usr/bin/env bash
#
# Captures the shipped client's GUI compositor at two stable viewport sizes and
# asks the report to prove that its solid fills, checker images and click-driven
# status line all reached the final pixels.
#
#   just ui-check
#   PRESET=release OUT=/tmp/ui-check scripts/demos/ui-visual-check.sh

set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$here/../.." && pwd)

preset=${PRESET:-dev}
build="$root/.cache/build/$preset"
out=${OUT:-$build/ui-check}

cmake -S "$root" --preset "$preset" > /dev/null
cmake --build "$build" --target client

client="$build/client/client"
scene="$build/assets/examples/scripts/GuiInteraction.luau"
if [ ! -x "$client" ]; then
	echo "no client at $client" >&2
	exit 1
fi
if [ ! -f "$scene" ]; then
	echo "no staged UI scene at $scene" >&2
	exit 1
fi

mkdir -p "$out"
rm -f "$out"/*.bmp "$out"/*.png "$out"/*.log "$out"/report.html "$out"/report.json

capture() {
	local name="$1"
	local width="$2"
	local height="$3"
	local click="${4:-}"
	local selected_scene="${5:-$scene}"
	local shot="$out/$name.bmp"
	local log="$out/$name.log"
	local args=(--headless --frames 180 --width "$width" --height "$height" --script "$selected_scene" --capture "$shot")
	if [ -n "$click" ]; then
		args+=(--click "$click")
	fi

	echo "capturing $name (${width}x${height}${click:+, click $click})"
	if timeout 120 "$client" "${args[@]}" > "$log" 2>&1; then
		:
	else
		status=$?
		tail -20 "$log" >&2
		echo "client failed for $name ($status, 124 means timeout)" >&2
		exit 1
	fi
	if [ ! -s "$shot" ]; then
		tail -20 "$log" >&2
		echo "client wrote no capture for $name" >&2
		exit 1
	fi
	if [ -n "$click" ] && ! grep -q "click: pressed '$click'" "$log"; then
		tail -20 "$log" >&2
		echo "client did not press $click for $name" >&2
		exit 1
	fi
}

# Baselines prove the composition at both responsive placements. The pair at
# 1280x720 differ by one real synthetic input event, so their status-line delta
# is a visible event result rather than a log-only claim.
capture baseline-960 960 540
capture baseline-1280 1280 720
capture text-button-1280 1280 720 TextButton
capture image-button-1280 1280 720 ImageButton
capture interface-1280 1280 720 "" "$build/assets/examples/scripts/Interface.luau"

python3 "$here/ui-visual-report.py" "$out"
echo "ui visual check ok - review $out/report.html"
