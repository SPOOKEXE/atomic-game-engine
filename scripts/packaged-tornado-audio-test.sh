#!/usr/bin/env bash
#
# Runs TornadoSim against an empty local store and proves its staged audio is
# registered before delivery has anything to provide.

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
cleanup() {
	rm -rf -- "$store" "$cache"
	rm -f "$log"
}
trap cleanup EXIT

timeout 60s "$binary" \
	--headless --frames 20 \
	--game "$build/assets/examples/worlds/TornadoSim.aworld" \
	--cdn "dir:$store" \
	--content-cache "$cache" \
	--publisher-key "$(printf '00%.0s' {1..32})" \
	> "$log" 2>&1

if ! grep -q "audio: 5 packaged sound(s) registered" "$log"; then
	echo "FAIL: TornadoSim did not register its five packaged sounds from a clean store" >&2
	tail -40 "$log" >&2
	exit 1
fi

if [ -e "$store/catalogue" ] || [ -e "$store/manifest" ]; then
	echo "FAIL: packaged audio check unexpectedly published content" >&2
	exit 1
fi

echo "TornadoSim registered five packaged sounds from an empty content store"
