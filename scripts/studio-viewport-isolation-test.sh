#!/usr/bin/env bash
#
# Proves a Studio Play server panel and client panel retain separate rendered
# images while focus moves between them.
#
# This starts the Bladeborne fixture in an isolated X display and Studio
# configuration, clicks each half of the split repeatedly, then uses Studio's
# control surface to capture both renderer slots after every click. The paired
# BMP files must differ byte for byte. The XWD files preserve what the host
# overlay showed for inspection when a platform-specific compositor issue is
# reported.
#
#   scripts/studio-viewport-isolation-test.sh
#   scripts/studio-viewport-isolation-test.sh .cache/build/release/studio/studio
#
# Output is retained below the selected build directory.

set -u

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
binary=${1:-"$root/.cache/build/dev/studio/studio"}
requested_game=${2:-}

if [ ! -x "$binary" ]; then
	echo "no Studio binary at $binary - build it first (just studio)" >&2
	exit 2
fi
for required in ffmpeg nc xdotool xwd xvfb-run; do
	if ! command -v "$required" > /dev/null; then
		echo "$required is required for Studio viewport isolation" >&2
		exit 2
	fi
done

# The test controls both the window and the host display, so it never clicks a
# Studio session someone is already using. Re-entering keeps the normal path
# useful on developer machines that already have DISPLAY set.
if [ -z "${ATOMIC_STUDIO_VIEWPORT_X11:-}" ]; then
	export ATOMIC_STUDIO_VIEWPORT_X11=1
	exec xvfb-run -a -s "-screen 0 1600x1000x24 -nolisten tcp" "$0" "$binary" "$requested_game"
fi

build=$(cd -- "$(dirname -- "$binary")/.." && pwd)
stamp=$(date +%Y%m%d-%H%M%S)
output="$build/studio-viewport-isolation-$stamp"
config=$(mktemp -d "${TMPDIR:-/tmp}/atomic-studio-viewport.XXXXXX")
log=$(mktemp)
port=$((40000 + RANDOM % 10000))
server_path=/tmp/atomic-game-engine/screenshots/scene-Bladeborne_Demo.bmp
client_path=/tmp/atomic-game-engine/screenshots/scene-Bladeborne_Demo__client_1_.bmp
mkdir -p "$output"

# The checked-in example is a world document. Studio opens games, so wrap the
# exact versioned Bladeborne world in the small game envelope it normally saves.
# A caller may pass a different `.agame` as the second argument when diagnosing
# a custom scene.
if [ -n "$requested_game" ]; then
	game=$requested_game
else
	world="$root/mono.engine/examples/assets/worlds/BladeborneDemo.aworld"
	game="$output/BladeborneVisual.agame"
	if [ ! -f "$world" ]; then
		echo "no checked-in Bladeborne world at $world" >&2
		exit 2
	fi
	{
		printf '%s\n' '<?xml version="1.0" encoding="UTF-8"?>' '<Game format="3" name="Bladeborne Visual">'
		sed '1d' "$world"
		printf '%s\n' '</Game>'
	} > "$game"
fi
if [ ! -f "$game" ]; then
	echo "no Bladeborne game at $game" >&2
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

# Xvfb has no window manager of its own. Metacity gives the docked split the
# same configure and focus lifecycle Studio receives on a desktop.
if command -v metacity > /dev/null; then
	metacity --sm-disable > /dev/null 2>&1 &
	wm_pid=$!
	sleep 0.25
fi

"$binary" \
	--config-root "$config" \
	--game "$game" \
	--run play \
	--viewports 1 \
	--width 1500 \
	--height 900 \
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

# Wait for both sides to receive a real draw. The first imgui pass only learns
# panel extents, so capturing before both slots exist would prove nothing.
request_screenshots() {
	printf '%s\n' \
		'{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"screenshot","arguments":{"target":"scene"}}}' |
		nc -N -w 5 127.0.0.1 "$port"
}

capture_pair() {
	local label=$1
	local before_server="missing"
	local before_client="missing"
	[ -f "$server_path" ] && before_server=$(stat -c '%y:%s' "$server_path")
	[ -f "$client_path" ] && before_client=$(stat -c '%y:%s' "$client_path")

	request_screenshots > "$output/$label-control.json"
	for _ in $(seq 1 100); do
		if [ -f "$server_path" ] && [ -f "$client_path" ]; then
			local after_server
			local after_client
			after_server=$(stat -c '%y:%s' "$server_path")
			after_client=$(stat -c '%y:%s' "$client_path")
			if [ "$after_server" != "$before_server" ] && [ "$after_client" != "$before_client" ]; then
				break
			fi
		fi
		sleep 0.1
	done
	if [ ! -s "$server_path" ] || [ ! -s "$client_path" ]; then
		echo "FAIL: $label did not produce both renderer-slot captures" >&2
		tail -40 "$log" >&2
		exit 1
	fi
	cp "$server_path" "$output/$label-server.bmp"
	cp "$client_path" "$output/$label-client.bmp"
	server_size=$(stat -c %s "$output/$label-server.bmp")
	client_size=$(stat -c %s "$output/$label-client.bmp")
	if [ "$server_size" != "$client_size" ]; then
		echo "FAIL: $label server and client targets differ in size ($server_size vs $client_size)" >&2
		exit 1
	fi
	if cmp -s "$output/$label-server.bmp" "$output/$label-client.bmp"; then
		echo "FAIL: $label server and client slots contain identical pixels" >&2
		exit 1
	fi
	sha256sum "$output/$label-server.bmp" "$output/$label-client.bmp" > "$output/$label-sha256.txt"
}

capture_pair initial

# The split lives in the first 38% of the default Studio layout. These points
# are the centres of its two halves, clear of the menu, tab strips, transport
# and output pane. They exercise the server and client focus paths rather than
# the adjacent empty centre dockspace.
eval "$(xdotool getwindowgeometry --shell "$window")"
left_x=$((WIDTH * 9 / 100))
right_x=$((WIDTH * 28 / 100))
view_y=$((HEIGHT * 42 / 100))
crop_width=$((WIDTH * 30 / 100))
crop_height=$((HEIGHT * 25 / 100))
left_crop_x=$((WIDTH * 4 / 100))
right_crop_x=$((WIDTH * 42 / 100))
crop_y=$((HEIGHT * 30 / 100))
for phase in server client server-again client-again; do
	case "$phase" in
		server|server-again) x=$left_x ;;
		client|client-again) x=$right_x ;;
	esac
	xdotool mousemove --window "$window" "$x" "$view_y"
	xdotool click --window "$window" 1
	sleep 0.35
	xwd -silent -id "$window" -out "$output/$phase-host.xwd"
	ffmpeg -v error -i "$output/$phase-host.xwd" \
		-vf "crop=$crop_width:$crop_height:$left_crop_x:$crop_y" -f rawvideo -pix_fmt rgb24 - |
		sha256sum | awk '{print $1}' > "$output/$phase-host-left.sha256"
	ffmpeg -v error -i "$output/$phase-host.xwd" \
		-vf "crop=$crop_width:$crop_height:$right_crop_x:$crop_y" -f rawvideo -pix_fmt rgb24 - |
		sha256sum | awk '{print $1}' > "$output/$phase-host-right.sha256"
	if cmp -s "$output/$phase-host-left.sha256" "$output/$phase-host-right.sha256"; then
		echo "FAIL: $phase host viewport crops contain identical pixels" >&2
		exit 1
	fi
	capture_pair "$phase"
done

# Move the server's free camera after focus has returned to it. The client is
# not being driven and Bladeborne's HUD is static, so its slot must stay byte
# identical while only the server image changes. This catches a shared camera
# or a focus handoff that rewrites the other panel's render request.
# SDL observes XTEST's real pointer stream here. Window-relative SendEvents
# reach the click handler but do not carry the relative motion used for aiming.
xdotool mousemove $((X + left_x)) $((Y + view_y))
xdotool mousedown 3
xdotool mousemove_relative $((WIDTH / 12)) 0
xdotool mouseup 3
sleep 0.35
xwd -silent -id "$window" -out "$output/server-turned-host.xwd"
capture_pair server-turned
if cmp -s "$output/server-again-server.bmp" "$output/server-turned-server.bmp"; then
	echo "FAIL: moving the server camera did not change the server slot" >&2
	exit 1
fi
if ! cmp -s "$output/client-again-client.bmp" "$output/server-turned-client.bmp"; then
	echo "FAIL: moving the server camera changed the client slot" >&2
	exit 1
fi

cp "$log" "$output/studio.log"
echo "studio viewport isolation ok - server and client renderer slots stayed distinct across focus changes"
echo "artifacts: $output"
