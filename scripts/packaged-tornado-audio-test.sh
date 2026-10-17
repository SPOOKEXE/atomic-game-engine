#!/usr/bin/env bash
#
# Checks staged audio demand, then runs TornadoSim against an empty local store.

set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
binary=${1:-$root/.cache/build/dev/client/client}

if [ ! -x "$binary" ]; then
	echo "no client at $binary - build it first (just build client)" >&2
	exit 2
fi
build=$(cd -- "$(dirname -- "$binary")/.." && pwd)

store=$(mktemp -d "${TMPDIR:-/tmp}/atomic-empty-content.XXXXXX")
cache=$(mktemp -d "${TMPDIR:-/tmp}/atomic-content-cache.XXXXXX")
log=$(mktemp)
fixtures=$(mktemp -d "${TMPDIR:-/tmp}/atomic-packaged-audio.XXXXXX")
cleanup() {
	rm -rf -- "$store" "$cache" "$fixtures"
	rm -f "$log"
}
trap cleanup EXIT

run_client() {
	if ! SDL_AUDIO_DRIVER=dummy timeout 60s "$binary" \
		--headless --frames 20 --uncapped --max-fps 60 \
		--cdn "dir:$store" \
		--content-cache "$cache" \
		--publisher-key 430f6fbd2ad9d7783aa781eccd5ec4b32c0a7032e86206d806043de24a0319da \
		"$@" > "$log" 2>&1; then
		tail -40 "$log" >&2
		return 1
	fi
}

cat > "$fixtures/silent.luau" <<'LUA'
print("packaged-audio: silent scene")
LUA
run_client --script "$fixtures/silent.luau"
if grep -q "packaged sound(s) registered" "$log"; then
	echo "FAIL: a scene without Sound rows decoded packaged audio" >&2
	tail -40 "$log" >&2
	exit 1
fi

cat > "$fixtures/delayed.luau" <<'LUA'
local workspace = game:GetService("Workspace")
local ticks = 0
game:GetService("RunService").Heartbeat:Connect(function()
	ticks += 1
	if ticks ~= 3 then return end
	print("packaged-audio: requested")
	for index = 1, 2 do
		local sound = Instance.new("Sound")
		sound.SoundId = "audio/tornado-wind.wav"
		sound.Looped = true
		sound.Playing = true
		sound.Parent = workspace
	end
end)
LUA
run_client --script "$fixtures/delayed.luau" --worlds 2
if [ "$(grep -c "audio: 1 packaged sound(s) registered" "$log" || true)" -ne 1 ] || \
	[ "$(grep -c "packaged sound(s) registered" "$log" || true)" -ne 1 ]; then
	echo "FAIL: late duplicate SoundIds did not decode exactly one packaged asset across worlds" >&2
	tail -40 "$log" >&2
	exit 1
fi
requested=$(grep -n -m1 "packaged-audio: requested" "$log" | cut -d: -f1)
registered=$(grep -n -m1 "packaged sound(s) registered" "$log" | cut -d: -f1)
if [ -z "$requested" ] || [ "$registered" -le "$requested" ]; then
	echo "FAIL: packaged audio was decoded before a world requested it" >&2
	tail -40 "$log" >&2
	exit 1
fi

run_client --game "$build/assets/examples/worlds/TornadoSim.aworld"

if ! grep -q "audio: 5 packaged sound(s) registered" "$log"; then
	echo "FAIL: TornadoSim did not register its five packaged sounds from a clean store" >&2
	tail -40 "$log" >&2
	exit 1
fi

if [ -e "$store/catalogue" ] || [ -e "$store/manifest" ]; then
	echo "FAIL: packaged audio check unexpectedly published content" >&2
	exit 1
fi

echo "Packaged audio loads once on demand, including TornadoSim's five sounds from an empty store"
