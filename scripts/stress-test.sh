#!/usr/bin/env bash
#
# One server, N real clients, and a flamegraph of what the server did.
#
# **A script rather than a recipe body, for `client-exit-test.sh`'s reason**: it
# has two processes to start, one to wait for and a renderer to run afterwards,
# and a hang in any of them has to fail the run rather than stop the build.
# Everything here is bounded by a `timeout`.
#
#   scripts/stress-test.sh .cache/build/release baseline 200 45 45100
#
# What it leaves behind, in .cache/stress/:
#
#   <label>_flamegraph.svg   the graph
#   <label>.folded           what it was rendered from
#   <label>_top.txt          the same capture as greppable text
#   <label>_server.log       the server's own output, including its tick p50/p95/p99
#   <label>_clients.txt      the harness's report
#   <label>_meta.txt         the commit the capture was taken at, and whether the tree was dirty
#
# The last one is not bookkeeping. Two graphs are only comparable when they came
# from the same tree, and this repository has more than one agent working in it.

set -euo pipefail

build=${1:?usage: stress-test.sh BUILD LABEL CLIENTS SECONDS PORT}
label=${2:?}
clients=${3:?}
seconds=${4:?}
port=${5:?}

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
out="$root/.cache/stress"
mkdir -p "$out"

server="$build/server/server"
harness="$build/tools/loadtest"
scene="$build/assets/examples/Stress.luau"

for needed in "$server" "$harness" "$scene"; do
	test -e "$needed" || { echo "FAIL: nothing at $needed - build first"; exit 1; }
done

# The commit and the state of the tree, beside the capture rather than in a
# note somewhere. A flamegraph whose provenance is not written down is one
# nobody can compare anything with six months later.
{
	echo "label     $label"
	echo "commit    $(git -C "$root" rev-parse HEAD)"
	echo "dirty     $(test -n "$(git -C "$root" status --porcelain)" && echo yes || echo no)"
	echo "build     $build"
	echo "clients   $clients"
	echo "seconds   $seconds"
	echo "captured  $(date -Is)"
} > "$out/${label}_meta.txt"

# **The server outlives the harness deliberately.** It has to still be up while
# the clients disconnect, and its own `--seconds` is what ends it - so the
# profile covers the whole run rather than being cut off mid-tick by a signal.
serverSeconds=$((seconds + 12))

echo "stress: starting the server on port $port for ${serverSeconds}s"
timeout $((serverSeconds + 30)) "$server" \
	--game "$scene" \
	--listen "$port" \
	--max-clients $((clients + 16)) \
	--tick-rate 30 \
	--seconds "$serverSeconds" \
	--profile-out "$out/$label.folded" \
	> "$out/${label}_server.log" 2>&1 &
serverPid=$!

# The bind is not instant and a client that dials a closed port simply retries,
# so this is politeness rather than correctness - it keeps the first seconds of
# the capture from being a handshake nobody was listening for.
sleep 2
kill -0 "$serverPid" 2>/dev/null || {
	echo "FAIL: the server exited before any client dialled"
	tail -20 "$out/${label}_server.log"
	exit 1
}

echo "stress: $clients clients for ${seconds}s"
set +e
timeout $((seconds + 60)) "$harness" \
	--port "$port" \
	--clients "$clients" \
	--seconds "$seconds" \
	--tick-rate 30 \
	> "$out/${label}_clients.txt" 2>&1
harnessStatus=$?
set -e

wait "$serverPid" || true

if [ $harnessStatus -ne 0 ]; then
	echo "FAIL: the harness exited $harnessStatus (124 means it never exited at all)"
	tail -30 "$out/${label}_clients.txt"
	exit 1
fi

test -s "$out/$label.folded" || {
	echo "FAIL: the server wrote no profile"
	tail -20 "$out/${label}_server.log"
	exit 1
}

python3 "$root/scripts/flamegraph.py" "$out/$label.folded" \
	--svg "$out/${label}_flamegraph.svg" \
	--top "$out/${label}_top.txt" \
	--title "server · $label · $clients clients · $(git -C "$root" rev-parse --short HEAD)"

echo
grep -E "tick ms p50|tick\(s\) over|profile:" "$out/${label}_server.log" || true
echo
# From the report rather than from the top of the file: the harness logs a line
# per session about the unpinned server identity before it prints anything.
sed -n '/^load test/,$p' "$out/${label}_clients.txt" | head -20
echo
echo "stress ok - $out/${label}_flamegraph.svg"
