#!/usr/bin/env bash
#
# One server and several clients, all on this machine.
#
# Each client is admitted, given a blocky character on the spawn pad and told
# which player it is; WASD walks it and Space jumps. Every client sees every
# other character move, because the movement happens once — on the server — and
# what crosses is the intent going up and the transform coming down.
#
#   scripts/demos/run-local-server.sh              # a server and two clients
#   scripts/demos/run-local-server.sh 4            # four clients instead
#   PORT=9100 scripts/demos/run-local-server.sh    # a different port
#   SCENE=Slide.luau scripts/demos/run-local-server.sh
#   PRESET=release scripts/demos/run-local-server.sh
#
# **This is not `mono.unified_server_client`**, and the difference is the whole
# reason to run it. That harness cuts `net` out of the middle to prove the
# serialise/deserialise seam; this puts the socket, the handshake, the cipher and
# the bandwidth budget back, and adds the thing neither of them had — more than
# one player.
#
# Ctrl-C stops everything: the clients are children of this shell and the trap
# takes the server down with them.

set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$here/../.." && pwd)

preset=${PRESET:-dev}
build="$root/.cache/build/$preset"

clients=${1:-2}
port=${PORT:-9099}
scene=${SCENE:-Playground.luau}

if [ "$preset" = "server" ]; then
	echo "the 'server' preset builds no client — there would be nobody to connect." >&2
	echo "  try:  PRESET=dev $0" >&2
	exit 1
fi

cmake -S "$root" --preset "$preset" > /dev/null
cmake --build "$build" --target client server

client="$build/client/client"
server="$build/server/server"
for program in "$client" "$server"; do
	if [ ! -x "$program" ]; then
		echo "built, but there is no program at $program" >&2
		exit 1
	fi
done

# The staged copy, not the source — a demo that ran the source tree would work
# here and nowhere a staged tree was copied to. `_common.sh` says the same.
staged="$build/client/assets/examples/$scene"
if [ ! -f "$staged" ]; then
	staged="$build/assets/examples/$scene"
fi
if [ ! -f "$staged" ]; then
	echo "no staged scene at $staged" >&2
	exit 1
fi

children=()
cleanup() {
	# Reverse order, so the clients go before the thing they are talking to and
	# nobody logs a connection error on the way out.
	for ((index = ${#children[@]} - 1; index >= 0; index--)); do
		kill "${children[index]}" 2> /dev/null || true
	done
	wait 2> /dev/null || true
}
trap cleanup EXIT INT TERM

echo "server: hosting $scene on 127.0.0.1:$port"
"$server" --game "$staged" --listen "$port" &
children+=($!)

# **A moment before the first client, and it is not a race this can lose
# gracefully.** A UDP connector sent at a port nothing is bound to gets an ICMP
# refusal, and the client's retry is slower than simply waiting. Half a second is
# comfortably more than binding a socket takes and is invisible to a person.
sleep 0.5

for ((index = 1; index <= clients; index++)); do
	echo "client $index: connecting"

	# **`--net`, because the F4 panel is where this demo is read from.** Two
	# characters that do not move have three explanations — never admitted, never
	# told which player, never sent an input — and the panel separates them.
	"$client" --connect "127.0.0.1:$port" --net --stats &
	children+=($!)

	# Staggered, so two clients do not hand the listener two admissions in one
	# poll. Not required — the listener handles it — but it makes the log read in
	# the order things happened.
	sleep 0.3
done

echo
echo "WASD walks, Space jumps, right mouse turns the camera. Ctrl-C stops it all."
wait
