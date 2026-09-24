#!/usr/bin/env bash
#
# Exercises Studio's View menu virtual-camera position lock through a real
# window, while retaining a product-capture bundle for inspection.
#
# The test freezes the behaviour position, translates the free inspection
# camera without turning it, recaptures the frozen position, then unlocks it.
# It keeps one BMP and the available read-only renderer diagnostics for every
# phase. A translated inspection view is expected to change pixels. The bounded
# viewport behavior diagnostic supplies the pass or fail signature because the
# physical camera is still used for raster projection.
#
#   scripts/studio-virtual-camera-capture-test.sh
#   scripts/studio-virtual-camera-capture-test.sh .cache/build/release/studio/studio game.agame "Scene"
#
# Output is retained below the selected build directory.

set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
binary=${1:-"$root/.cache/build/dev/studio/studio"}
requested_game=${2:-}
requested_world=${3:-}

if [ ! -x "$binary" ]; then
	echo "no Studio binary at $binary, build it first with just studio" >&2
	exit 2
fi
for required in nc python3 sha256sum xdotool xwd xvfb-run; do
	if ! command -v "$required" > /dev/null; then
		echo "$required is required for Studio virtual-camera capture" >&2
		exit 2
	fi
done

# A private display prevents the menu clicks and camera keys from reaching a
# developer's Studio session. Re-entering preserves the normal path inside Xvfb.
if [ -z "${ATOMIC_STUDIO_VIRTUAL_CAMERA_X11:-}" ]; then
	export ATOMIC_STUDIO_VIRTUAL_CAMERA_X11=1
	exec xvfb-run -a -s "-screen 0 1600x1440x24 -nolisten tcp" "$0" "$binary" "$requested_game" "$requested_world"
fi

build=$(cd -- "$(dirname -- "$binary")/.." && pwd)
stamp=$(date +%Y%m%d-%H%M%S)
output="$build/studio-virtual-camera-capture-$stamp"
config=$(mktemp -d "${TMPDIR:-/tmp}/atomic-studio-virtual-camera.XXXXXX")
log=$(mktemp)
port=$((40000 + RANDOM % 10000))
screenshot_directory="${TMPDIR:-/tmp}/atomic-game-engine/screenshots"
mkdir -p "$output"

# The capture gate needs one authored scene with visible culling, configured
# automatic LOD, local lights, particle batches, and portal demand. Studio
# scene names are stable strings at this boundary.
if [ -n "$requested_game" ]; then
	game=$requested_game
	world_name=$requested_world
	if [ -z "$world_name" ]; then
		echo "a scene name is required with a custom game" >&2
		exit 2
	fi
else
	game="$root/mono.engine/examples/assets/worlds/VirtualCameraCapture.agame"
	world_name="Virtual Camera Capture"
fi
if [ ! -f "$game" ]; then
	echo "no Studio game at $game" >&2
	exit 2
fi

studio_pid=""
wm_pid=""
cleanup() {
	if [ -n "$studio_pid" ]; then
		kill "$studio_pid" 2> /dev/null || true
		wait "$studio_pid" 2> /dev/null || true
	fi
	if [ -n "$wm_pid" ]; then
		kill "$wm_pid" 2> /dev/null || true
	fi
	rm -rf -- "$config"
	rm -f -- "$log"
}
trap cleanup EXIT

if command -v metacity > /dev/null; then
	metacity --sm-disable > /dev/null 2>&1 &
	wm_pid=$!
	sleep 0.25
fi

"$binary" \
	--config-root "$config" \
	--game "$game" \
	--run play \
	--width 1500 \
	--height 1280 \
	--mcp-port "$port" > "$log" 2>&1 &
studio_pid=$!

for _ in $(seq 1 100); do
	if grep -q "control: listening on 127.0.0.1:$port" "$log"; then
		break
	fi
	if ! kill -0 "$studio_pid" 2> /dev/null; then
		echo "FAIL: Studio exited before its control surface opened" >&2
		tail -40 "$log" >&2
		exit 1
	fi
	sleep 0.1
done
if ! grep -q "control: listening on 127.0.0.1:$port" "$log"; then
	echo "FAIL: Studio did not open its control surface" >&2
	tail -40 "$log" >&2
	exit 1
fi

window=""
for _ in $(seq 1 80); do
	for candidate in $(xdotool search --onlyvisible --pid "$studio_pid" 2> /dev/null); do
		if [ "$(xdotool getwindowname "$candidate" 2> /dev/null)" = "atomic studio" ]; then
			window=$candidate
			break 2
		fi
	done
	sleep 0.1
done
if [ -z "$window" ]; then
	echo "FAIL: Studio opened no controllable window" >&2
	exit 1
fi
eval "$(xdotool getwindowgeometry --shell "$window")"

request_tool() {
	local tool=$1
	local arguments=$2
	printf '%s\n' \
		"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":\"$tool\",\"arguments\":$arguments}}" |
		nc -N -w 5 127.0.0.1 "$port"
}

request_tool_list() {
	printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"tools/list","params":{}}' |
		nc -N -w 5 127.0.0.1 "$port"
}

capture_diagnostics() {
	local label=$1
	request_tool world_list '{}' > "$output/$label-world-list.json"
	request_tool metrics_read '{}' > "$output/$label-renderer-metrics.json"
	request_tool get_scene_snapshot "{\"instance_id\":\"$world_name\",\"options\":{\"limit\":256}}" \
		> "$output/$label-scene-snapshot.json"
	request_tool get_camera_rendering_data "{\"instance_id\":\"$world_name\",\"options\":{\"object_limit\":64}}" \
		> "$output/$label-camera-calibration.json"
	request_tool viewport_behavior_diagnostics '{}' > "$output/$label-viewport-behavior.json"
	python3 - "$output/$label-viewport-behavior.json" "$output/$label-viewport-behavior-signature.json" <<'PY'
import json
import sys

response = json.load(open(sys.argv[1], encoding="utf-8"))
text = response["result"]["content"][0]["text"]
value = json.loads(text)
if not value["completed"]:
    raise SystemExit("viewport behavior diagnostic did not describe a completed frame")
signature = {
    "frustum_locked": value["frustum_locked"],
    "inspection_pose": value["inspection_pose"],
    "behavior_pose": value["behavior_pose"],
    "culling": {"visible_draw_calls": value["culling"]["visible_draw_calls"]},
    "lod": value["lod"],
    "lighting": value["lighting"],
    "particles": {
        "batches": value["particles"]["batches"],
        "blocks": value["particles"]["blocks"],
    },
    "portal_demand": {"requested_portals": value["portal_demand"]["requested_portals"]},
}
json.dump(signature, open(sys.argv[2], "w", encoding="utf-8"), sort_keys=True)
PY
}

assert_locked_invariant() {
	local before=$1
	local after=$2
	python3 - "$before" "$after" <<'PY'
import json
import sys

before = json.load(open(sys.argv[1], encoding="utf-8"))
after = json.load(open(sys.argv[2], encoding="utf-8"))
if not before["frustum_locked"] or not after["frustum_locked"]:
    raise SystemExit("virtual camera lock was not active for both captures")
if before["inspection_pose"]["position"] == after["inspection_pose"]["position"]:
    raise SystemExit("inspection position did not change")
if before["inspection_pose"]["rotation"] != after["inspection_pose"]["rotation"]:
    raise SystemExit("inspection direction changed during translation-only motion")
before.pop("inspection_pose")
after.pop("inspection_pose")
if before != after:
    raise SystemExit("locked behavior signature changed after inspection translation")
PY
}

assert_combined_baseline() {
	local signature=$1
	python3 - "$signature" <<'PY'
import json
import sys

value = json.load(open(sys.argv[1], encoding="utf-8"))
required = {
    "culling visible draw calls": value["culling"]["visible_draw_calls"],
    # A completed frame's submitted triangles prove the configured automatic-LOD
    # mesh participates in the view. This endpoint has no selected-LOD id, so
    # the fixture's AutoLodStrategy setting and this nonzero proxy are the
    # bounded LOD subgate.
    "LOD submitted triangles": value["lod"]["submitted_triangles"],
    "selected local lights": value["lighting"]["selected_lights"],
    "particle batches": value["particles"]["batches"],
    "requested portals": value["portal_demand"]["requested_portals"],
}
if not value["lod"]["enabled"]:
    raise SystemExit("automatic LOD culling is disabled")
missing = [name for name, count in required.items() if count <= 0]
if missing:
    raise SystemExit("combined fixture did not exercise: " + ", ".join(missing))
PY
}

assert_recaptured_pose() {
	local before=$1
	local after=$2
	python3 - "$before" "$after" <<'PY'
import json
import sys

before = json.load(open(sys.argv[1], encoding="utf-8"))
after = json.load(open(sys.argv[2], encoding="utf-8"))
if not after["frustum_locked"]:
    raise SystemExit("virtual camera lock was not active after recapture")
if before["behavior_pose"] == after["behavior_pose"]:
    raise SystemExit("recapture did not adopt the translated inspection pose")
PY
}

assert_unlocked_pose() {
	local signature=$1
	python3 - "$signature" <<'PY'
import json
import sys

value = json.load(open(sys.argv[1], encoding="utf-8"))
if value["frustum_locked"]:
    raise SystemExit("virtual camera lock remained active after unlock")
if value["behavior_pose"] != value.get("inspection_pose", value["behavior_pose"]):
    raise SystemExit("unlocked behavior pose differs from the inspection pose")
PY
}

request_screenshot() {
	local arguments
	arguments=$(python3 - "$world_name" <<'PY'
import json
import sys

print(json.dumps({"target": "scene", "scene": sys.argv[1]}))
PY
)
	request_tool screenshot "$arguments"
}

capture_phase() {
	local label=$1
	local before="missing"
	local after=""
	local path=""
	local candidate=""
	declare -A existing_screenshots=()
	for candidate in "$screenshot_directory"/scene-*.bmp; do
		if [ -f "$candidate" ]; then
			existing_screenshots["$candidate"]=$(stat -c '%y:%s' "$candidate")
		fi
	done
	request_screenshot > "$output/$label-screenshot-control.json"
	path=$(python3 - "$output/$label-screenshot-control.json" "$world_name" <<'PY'
import json
import sys

response = json.load(open(sys.argv[1], encoding="utf-8"))
result = response.get("result", {})
if result.get("isError"):
    raise SystemExit("screenshot request failed: " + result["content"][0]["text"])
payload = json.loads(result["content"][0]["text"])
captures = [
    item["path"]
    for item in payload.get("captures", [])
    if item.get("kind") == "scene" and item.get("scene") == sys.argv[2]
]
if len(captures) != 1:
    raise SystemExit("screenshot request did not return one path for the visible capture scene")
print(captures[0])
PY
)
	before=${existing_screenshots["$path"]:-missing}
	for _ in $(seq 1 100); do
		if [ -f "$path" ]; then
			after=$(stat -c '%y:%s' "$path")
			if [ "$after" != "$before" ]; then
				break
			fi
		fi
		sleep 0.1
	done
	if [ -z "$path" ] || [ ! -s "$path" ] || [ "$after" = "$before" ]; then
		echo "FAIL: $label did not produce a fresh Studio scene BMP" >&2
		tail -40 "$log" >&2
		exit 1
	fi
	cp "$path" "$output/$label-scene.bmp"
	sha256sum "$output/$label-scene.bmp" > "$output/$label-scene.sha256"
	xwd -silent -id "$window" -out "$output/$label-host.xwd"
	capture_diagnostics "$label"
}

# These coordinates intentionally target the actual View menu. The private
# 1280px Studio window leaves the long menu visible. They can be adjusted for a
# local UI scale without altering the scenario; retained XWDs show any drift.
view_menu_x=${ATOMIC_STUDIO_VIEW_MENU_X:-170}
menu_bar_y=${ATOMIC_STUDIO_MENU_BAR_Y:-12}
lock_item_y=${ATOMIC_STUDIO_LOCK_ITEM_Y:-894}

open_view_menu() {
	xdotool mousemove --window "$window" "$view_menu_x" "$menu_bar_y"
	xdotool click 1
	sleep 0.2
}

toggle_virtual_camera_lock() {
	open_view_menu
	xdotool mousemove --window "$window" "$view_menu_x" "$lock_item_y"
	xdotool click 1
	sleep 0.4
}

recapture_virtual_camera_position() {
	# Enabling the lock inserts Recapture directly below Lock, leaving Lock's
	# position stable and placing Recapture one row lower in the same menu.
	open_view_menu
	xdotool mousemove --window "$window" "$view_menu_x" "$((lock_item_y + 22))"
	xdotool click 1
	sleep 0.4
}

translate_inspection_camera() {
	# Focus the capture viewport and move only along its existing forward vector.
	# No pointer-relative motion is sent, so this scenario never turns the live
	# direction that the virtual behavior pose intentionally retains.
	local viewport_x=$((WIDTH * 28 / 100))
	local viewport_y=$((HEIGHT * 42 / 100))
	xdotool mousemove --window "$window" "$viewport_x" "$viewport_y"
	xdotool click 1
	xdotool keydown w
	sleep 0.7
	xdotool keyup w
	sleep 0.4
}

focus_capture_viewport() {
	# The MCP screenshot returns the sibling capture viewport, immediately right
	# of the primary inspection viewport. Keep the View menu and keyboard motion
	# on that panel so each diagnostic names the same completed frame.
	xdotool mousemove --window "$window" "$((WIDTH * 28 / 100))" "$((HEIGHT * 42 / 100))"
	xdotool click 1
	sleep 0.2
}

focus_capture_viewport
request_tool_list > "$output/tools-list.json"
capture_phase initial

open_view_menu
xwd -silent -id "$window" -out "$output/lock-menu-before.xwd"
xdotool key Escape
toggle_virtual_camera_lock
open_view_menu
xwd -silent -id "$window" -out "$output/lock-menu-checked.xwd"
xdotool key Escape
capture_phase locked
if ! assert_combined_baseline "$output/locked-viewport-behavior-signature.json"; then
	echo "FAIL: combined fixture did not exercise every virtual-camera subgate" >&2
	exit 1
fi

translate_inspection_camera
capture_phase locked-translated
if cmp -s "$output/locked-scene.bmp" "$output/locked-translated-scene.bmp"; then
	echo "FAIL: translating the free inspection camera did not change the locked capture" >&2
	exit 1
fi
if ! assert_locked_invariant \
	"$output/locked-viewport-behavior-signature.json" \
	"$output/locked-translated-viewport-behavior-signature.json"; then
	echo "FAIL: locked culling, LOD, lighting, particle, or portal evidence changed" >&2
	exit 1
fi

recapture_virtual_camera_position
capture_phase recaptured
if ! assert_recaptured_pose \
	"$output/locked-translated-viewport-behavior-signature.json" \
	"$output/recaptured-viewport-behavior-signature.json"; then
	echo "FAIL: recapture did not update the virtual behavior pose" >&2
	exit 1
fi

translate_inspection_camera
capture_phase recaptured-translated
if cmp -s "$output/recaptured-scene.bmp" "$output/recaptured-translated-scene.bmp"; then
	echo "FAIL: translating the free inspection camera did not change the recaptured capture" >&2
	exit 1
fi

toggle_virtual_camera_lock
open_view_menu
xwd -silent -id "$window" -out "$output/lock-menu-unchecked.xwd"
xdotool key Escape
capture_phase unlocked
if ! assert_unlocked_pose "$output/unlocked-viewport-behavior-signature.json"; then
	echo "FAIL: unlock did not restore the inspection behavior pose" >&2
	exit 1
fi

translate_inspection_camera
capture_phase unlocked-translated
if cmp -s "$output/unlocked-scene.bmp" "$output/unlocked-translated-scene.bmp"; then
	echo "FAIL: translating the unlocked inspection camera did not change the capture" >&2
	exit 1
fi

cp "$log" "$output/studio.log"
echo "studio virtual-camera capture ok, retained lock, recapture, unlock BMPs and diagnostics"
echo "artifacts: $output"
