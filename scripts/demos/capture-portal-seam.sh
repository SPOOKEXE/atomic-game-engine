#!/usr/bin/env bash
#
# A ninety-frame reference sequence for each PortalSeam camera route.
#
#   scripts/demos/capture-portal-seam.sh              # all three views
#   scripts/demos/capture-portal-seam.sh side         # one route
#   OUT=/tmp/shots scripts/demos/capture-portal-seam.sh
#
# The client writes each rendered frame and its camera-state JSON sidecar. The
# report keeps the side and far image measurements, then records the presented
# frame, camera route revisions, fixture, resolution, backend, pipeline revision
# and tolerance in one reviewable report. The backend comes from the renderer
# startup log; each capture sidecar records its selected pipeline identity.

set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$here/../.." && pwd)

preset=${PRESET:-dev}
build="$root/.cache/build/$preset"
out=${OUT:-$build/captures}
frames=${FRAMES:-90}
width=${WIDTH:-1920}
height=${HEIGHT:-1080}
fps=${FPS:-60}
capture_alpha=${CAPTURE_ALPHA:-0.5}
capture_timeout=${CAPTURE_TIMEOUT:-300}
backend=${BACKEND:-unknown}
detected_backend=
pipeline_revision=${PIPELINE_REVISION:-unknown}
side_ratio_maximum=${SIDE_RATIO_MAXIMUM:-0.75}
skip_build=${SKIP_BUILD:-0}
profile_snapshot=${PROFILE_SNAPSHOT:-0}

if ! [[ "$frames" =~ ^[1-9][0-9]*$ ]]; then
	echo "FRAMES must be a positive integer" >&2
	exit 1
fi
for setting in width height fps capture_timeout; do
	if ! [[ "${!setting}" =~ ^[1-9][0-9]*$ ]]; then
		echo "${setting^^} must be a positive integer" >&2
		exit 1
	fi
done
for setting in skip_build profile_snapshot; do
	if [ "${!setting}" != 0 ] && [ "${!setting}" != 1 ]; then
		echo "${setting^^} must be 0 or 1" >&2
		exit 1
	fi
done

if [ "$skip_build" = 0 ]; then
	cmake -S "$root" --preset "$preset" > /dev/null
	cmake --build "$build" --target client
elif [ ! -x "$build/client/client" ]; then
	echo "no prebuilt client at $build/client/client" >&2
	exit 1
fi

scene="$build/assets/examples/scripts/PortalSeam.luau"
reference_scene="$build/assets/examples/scripts/PortalSeamMatchedRoom.luau"
if [ ! -f "$scene" ]; then
	echo "no staged scene at $scene" >&2
	exit 1
fi
if [ ! -f "$reference_scene" ]; then
	echo "no staged matched-room reference at $reference_scene" >&2
	exit 1
fi

mkdir -p "$out"

capture_sequence() {
	local staged_scene=$1
	local sequence=$2
	local label=$3
	mkdir -p "$sequence"
	rm -f "$sequence"/*.bmp "$sequence"/*.json
	echo "capturing $label ($frames frames)"
	local profile_arguments=()
	if [ "$profile_snapshot" = 1 ]; then
		profile_arguments=(--profile-snapshot "$sequence/frame-graph-snapshot.txt")
	fi
	timeout "$capture_timeout" "$build/client/client" \
		--headless --uncapped --max-fps "$fps" --width "$width" --height "$height" \
		--script "$staged_scene" --frames "$frames" \
		--capture-alpha "$capture_alpha" \
		"${profile_arguments[@]}" \
		--capture-sequence "$sequence" > "$sequence/runtime.log" 2>&1
	local ready_line
	ready_line=$(grep -E 'renderer ready on [[:alnum:]_-]+' "$sequence/runtime.log" | tail -1 || true)
	if [ -z "$ready_line" ]; then
		echo "capture did not report a renderer backend for $label" >&2
		exit 1
	fi
	local current_backend=${ready_line##* }
	if [ -n "$detected_backend" ] && [ "$detected_backend" != "$current_backend" ]; then
		echo "capture backend changed from $detected_backend to $current_backend" >&2
		exit 1
	fi
	detected_backend=$current_backend
	if [ "$backend" != unknown ] && [ "$backend" != "$detected_backend" ]; then
		echo "requested backend $backend differs from runtime backend $detected_backend" >&2
		exit 1
	fi
	for ((frame = 0; frame < frames; frame++)); do
		if [ ! -f "$sequence/$frame.bmp" ] || [ ! -f "$sequence/$frame.json" ]; then
			echo "capture sequence is missing frame $frame for $label" >&2
			exit 1
		fi
	done
	shopt -s nullglob
	local images=("$sequence"/*.bmp)
	local sidecars=("$sequence"/*.json)
	if [ ${#images[@]} -ne "$frames" ] || [ ${#sidecars[@]} -ne "$frames" ]; then
		echo "capture sequence count differs from $frames for $label" >&2
		exit 1
	fi
}

views=("$@")
if [ ${#views[@]} -eq 0 ]; then
	views=(side front far)
fi

sequences=()
front_requested=false
for view in "${views[@]}"; do
	case "$view" in
		side|front|far) ;;
		*) echo "unsupported portal view: $view" >&2; exit 1 ;;
	esac
	staged="$out/PortalSeam-$view.luau"
	sequence="$out/portal-seam-$view"
	sed "s/^local VIEW = \"[^\"]*\"$/local VIEW = \"$view\"/" "$scene" > "$staged"

	if ! grep -q "^local VIEW = \"$view\"$" "$staged"; then
		echo "the view line in PortalSeam.luau moved; this script did not follow" >&2
		exit 1
	fi

	capture_sequence "$staged" "$sequence" "$view portal sequence"
	sequences+=("$sequence")
	if [ "$view" = "front" ]; then
		front_requested=true
	fi
done

# The continuous-room reference uses the same moving front camera route. It is
# intentionally captured separately so its timeline contains no portal images
# or aperture history, then paired by frame number in the CPU report.
report_requirements=()
if [ "$front_requested" = true ]; then
	reference_staged="$out/PortalSeamMatchedRoom-front.luau"
	reference_sequence="$out/portal-seam-reference-front"
	sed 's/^local VIEW = "[^"]*"$/local VIEW = "front"/' "$reference_scene" > "$reference_staged"
	if ! grep -q '^local VIEW = "front"$' "$reference_staged"; then
		echo "the view line in PortalSeamMatchedRoom.luau moved; this script did not follow" >&2
		exit 1
	fi
	capture_sequence "$reference_staged" "$reference_sequence" "matched-room front reference"
	sequences+=("$reference_sequence")
	report_requirements=(--require-full-reference --require-moving-front)
fi

python3 "$here/portal-seam-report.py" \
	--fixture PortalSeam \
	--backend "$detected_backend" \
	--pipeline-revision "$pipeline_revision" \
	--target-fps "$fps" \
	--expected-alpha "$capture_alpha" \
	--require-pipeline-identity \
	--aperture-only \
	--side-ratio-maximum "$side_ratio_maximum" \
	"${report_requirements[@]}" \
	--output "$out/portal-seam-report.json" \
	"${sequences[@]}"

echo "reference sequences and report in $out"
