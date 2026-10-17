#!/usr/bin/env bash
# One local driver, eight hosted worlds, 200 real clients, and a signed CDN run.
# Run from a release build so these numbers describe shipped-cost code.

set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
source "$root/scripts/integrated-stress-common.sh"
build_arg=${1:?usage: integrated-stress-test.sh RELEASE_BUILD [SECONDS] [PORT_BASE]}
seconds=${2:-45}
port_base=${3:-45100}
contended=${INTEGRATED_STRESS_CONTENDED:-no}
[[ $seconds =~ ^[0-9]+$ ]] || { echo "FAIL: seconds must be a positive integer" >&2; exit 2; }
[[ $port_base =~ ^[0-9]+$ ]] || { echo "FAIL: port base must be an integer" >&2; exit 2; }
[[ $contended == yes || $contended == no ]] || { echo "FAIL: INTEGRATED_STRESS_CONTENDED must be yes or no" >&2; exit 2; }
((seconds >= 10)) || { echo "FAIL: use at least 10 seconds so hosts can report metrics" >&2; exit 2; }
((port_base >= 1024 && port_base <= 65000)) || { echo "FAIL: port base is outside the usable test range" >&2; exit 2; }
build=$(cd -- "$build_arg" && pwd)
[[ $(basename -- "$build") == release ]] || { echo "FAIL: this run requires the release preset directory" >&2; exit 2; }

server="$build/server/server"
loadtest="$build/tools/loadtest"
cdn="$build/cdn/cdn"
scene="$build/assets/examples/scripts/Stress.luau"
for needed in "$server" "$loadtest" "$cdn"; do
	test -x "$needed" || { echo "FAIL: missing executable: $needed" >&2; exit 1; }
done
test -f "$scene" || { echo "FAIL: missing stress scene: $scene" >&2; exit 1; }

run_id=$(date +%Y%m%d-%H%M%S)-$$
out="$build/integrated-stress/$run_id"
content="$out/content"
store="$out/store"
mkdir -p "$content" "$out"
server_log="$out/server.log"
cdn_log="$out/cdn.log"
cdn_fetch_log="$out/cdn-fetch.log"
client_pids=()
server_pid=
cdn_pid=
fetch_pid=

cleanup() {
	local pid
	for pid in "${client_pids[@]}" "${fetch_pid:-}" "${server_pid:-}" "${cdn_pid:-}"; do
		[[ -n $pid ]] && kill -TERM "$pid" 2>/dev/null || true
	done
}
trap cleanup EXIT INT TERM

worlds=()
for index in $(seq 0 7); do
	worlds+=("stress-world-$(printf '%02d' "$index")")
done

# Six distinct groups let the fetch cohort keep five cold keys genuinely cold.
bundle_bytes=$((16 * 1024 * 1024 + 64 * 1024))
for index in $(seq 0 5); do
	head -c "$bundle_bytes" /dev/urandom > "$content/bundle-$(printf '%02d' "$index").agame"
done
signing_key=$(od -An -N32 -tx1 /dev/urandom | tr -d ' \n')
grant_key=$(od -An -N32 -tx1 /dev/urandom | tr -d ' \n')
"$cdn" --publish "$content" --store "$store" --signing-key "$signing_key" > "$out/cdn-publish.log" 2>&1
publisher_key=$(sed -n 's/.*cdn: publisher key \([0-9a-f]\{64\}\).*/\1/p' "$out/cdn-publish.log" | tail -1)
test "${#publisher_key}" -eq 64 || { echo "FAIL: publisher did not report its signing key" >&2; tail -20 "$out/cdn-publish.log"; exit 1; }

driver_seconds=$((seconds + 15))
cdn_port=$((port_base + 1))
timeout $((driver_seconds + 45)) "$cdn" --store "$store" --grant-key "$grant_key" --port "$cdn_port" --seconds "$driver_seconds" > "$cdn_log" 2>&1 &
cdn_pid=$!

healthy=0
for _ in $(seq 1 100); do
	if ! kill -0 "$cdn_pid" 2>/dev/null; then
		echo "FAIL: CDN exited before it accepted requests" >&2
		tail -30 "$cdn_log" >&2
		exit 1
	fi
	if curl --fail --silent "http://127.0.0.1:$cdn_port/health" >/dev/null; then
		healthy=1
		break
	fi
	sleep 0.1
done
((healthy == 1)) || { echo "FAIL: CDN did not become healthy" >&2; tail -30 "$cdn_log" >&2; exit 1; }

server_args=(--game "$scene" --listen 0 --seconds "$driver_seconds" --tick-rate 30 --chatter --metrics-every 5)
for world in "${worlds[@]}"; do
	server_args+=(--remote-world "$world")
done
timeout $((driver_seconds + 30)) "$server" "${server_args[@]}" > "$server_log" 2>&1 &
server_pid=$!

ready=0
ready_deadline=$((SECONDS + 60))
while ((SECONDS < ready_deadline)); do
	if parse_ready_endpoints "$server_log" "${worlds[@]}" 2>/dev/null; then
		ready=1
		break
	else
		parse_status=$?
		if ((parse_status == 2)); then
			echo "FAIL: server emitted a malformed or duplicate host endpoint" >&2
			tail -40 "$server_log" >&2
			exit 1
		fi
	fi
	if ! kill -0 "$server_pid" 2>/dev/null; then
		echo "FAIL: hosted driver exited before all child worlds were ready" >&2
		tail -40 "$server_log" >&2
		exit 1
	fi
	sleep 0.2
done
((ready == 1)) || { echo "FAIL: timed out waiting for all eight child endpoints" >&2; tail -40 "$server_log" >&2; exit 1; }

printf 'stress: 8 hosted worlds, 25 clients each, %s seconds\n' "$seconds"
for endpoint in "${READY_ENDPOINTS[@]}"; do
	read -r _ world port <<< "$endpoint"
	[[ $world =~ ^stress-world-([0-7][0-7])$ ]] || {
		echo "FAIL: unexpected stress world name: $world" >&2
		exit 1
	}
	world_ordinal=$((10#${BASH_REMATCH[1]}))
	seed=$((135790 + world_ordinal))
	client_log="$out/${world}-clients.log"
	timeout $((seconds + 60)) "$loadtest" --address 127.0.0.1 --port "$port" --clients 25 --seconds "$seconds" --tick-rate 30 --input-every-ticks 1 --random-heading-seed "$seed" > "$client_log" 2>&1 &
	client_pids+=("$!")
done

# Start HTTP pressure after the 200 clients have begun their joins.
sleep 2
timeout $((seconds + 60)) "$loadtest" --cdn-only --cdn-store "$store" --cdn-publisher-key "$publisher_key" --cdn-grant-key "$grant_key" --cdn-address 127.0.0.1 --cdn-port "$cdn_port" --cdn-requests 25 --cdn-concurrency 8 > "$cdn_fetch_log" 2>&1 &
fetch_pid=$!

failed=0
for pid in "${client_pids[@]}"; do
	if ! wait "$pid"; then failed=1; fi
done
client_pids=()
if ! wait "$fetch_pid"; then failed=1; fi
fetch_pid=
if ((failed != 0)); then
	echo "FAIL: one or more client or CDN cohorts failed" >&2
	for world in "${worlds[@]}"; do tail -15 "$out/${world}-clients.log" >&2 || true; done
	tail -30 "$cdn_fetch_log" >&2 || true
	cat "$server_log" >&2
	exit 1
fi

for world in "${worlds[@]}"; do
	client_log="$out/${world}-clients.log"
	metric() {
		awk -v wanted="$1" '{ line=$0; sub(/^[[:space:]]+/, "", line); if (index(line, wanted) == 1) { rest=substr(line, length(wanted) + 1); if (rest ~ /^[[:space:]]/) { print $NF; exit } } }' "$client_log"
	}
	playing=$(metric "playing at the end")
	unique=$(metric "unique player IDs")
	moving=$(metric "moving characters")
	inputs=$(metric "inputs sent")
	[[ $playing == 25 && $unique == 25 && ${moving:-0} -gt 0 && ${inputs:-0} -gt 0 ]] || {
		echo "FAIL: $world did not prove 25 unique playing, moving clients" >&2
		tail -45 "$client_log" >&2
		exit 1
	}
	grep -q "host-metric .*world=$world " "$server_log" || {
		echo "FAIL: $world has no HostStatus tick samples" >&2
		tail -60 "$server_log" >&2
		exit 1
	}
done

# Let the driver and origin finish their bounded runs so both print final metrics.
server_status=0
wait "$server_pid" || server_status=$?
server_pid=
if ((server_status != 0)); then
	echo "FAIL: hosted driver did not exit cleanly" >&2
	tail -50 "$server_log" >&2
	exit 1
fi
cdn_status=0
wait "$cdn_pid" || cdn_status=$?
cdn_pid=
if ((cdn_status != 0)); then
	echo "FAIL: CDN origin did not exit cleanly" >&2
	tail -40 "$cdn_log" >&2
	exit 1
fi

cache_line=$(grep 'cdn: prepared cache hits ' "$cdn_log" | tail -1)
cache_hits=$(sed -n 's/.*hits \([0-9][0-9]*\), misses.*/\1/p' <<< "$cache_line")
cache_misses=$(sed -n 's/.*misses \([0-9][0-9]*\).*/\1/p' <<< "$cache_line")
[[ $cache_hits == 20 && $cache_misses == 6 ]] || {
	echo "FAIL: CDN prepared cache expected 20 hot hits and 6 cold misses, saw: $cache_line" >&2
	exit 1
}
queue_high_water=$(sed -n 's/^[[:space:]]*queue high-water: //p' "$cdn_fetch_log")
[[ $queue_high_water =~ ^[1-9][0-9]*$ ]] || {
	echo "FAIL: CDN fetch queue did not record a positive high-water mark, saw: ${queue_high_water:-none}" >&2
	tail -30 "$cdn_fetch_log" >&2
	exit 1
}
bus_line=$(grep -E 'world\.delivery\.staged\.messages.*· [1-9][0-9]* over [1-9][0-9]* call' "$server_log" | tail -1)
[[ -n $bus_line ]] || {
	echo "FAIL: hosted worlds did not stage any cross-world bus deliveries" >&2
	tail -60 "$server_log" >&2
	exit 1
}
read -r bus_active bus_accepted_peak bus_sent_peak bus_dropped_total < <(awk '
	/host-bus-window accepted=/ {
		accepted = sent = dropped = 0
		for (field = 1; field <= NF; field++) {
			if ($field ~ /^accepted=/) { split($field, value, "="); accepted = value[2] + 0 }
			if ($field ~ /^sent=/) { split($field, value, "="); sent = value[2] + 0 }
			if ($field ~ /^dropped=/) { split($field, value, "="); dropped = value[2] + 0 }
		}
		if (accepted > 0 && sent > 0) active = 1
		if (accepted > peakAccepted) peakAccepted = accepted
		if (sent > peakSent) peakSent = sent
		droppedTotal += dropped
	}
	END { printf "%d %d %d %d\n", active, peakAccepted, peakSent, droppedTotal }
' "$server_log")
[[ ${bus_active:-0} -eq 1 && ${bus_dropped_total:-1} -eq 0 ]] || {
	echo "FAIL: coordinator did not accept and send cross-world bus deliveries cleanly: active=${bus_active:-none}, peak=${bus_accepted_peak:-none}/${bus_sent_peak:-none}, dropped=${bus_dropped_total:-none}" >&2
	tail -60 "$server_log" >&2
	exit 1
}

{
	echo "preset release"
	echo "build $build"
	echo "commit $(git -C "$root" rev-parse HEAD)"
	echo "dirty $(test -n "$(git -C "$root" status --porcelain)" && echo yes || echo no)"
	echo "remote worlds 8"
	echo "clients per world 25"
	echo "total clients 200"
	echo "tick rate 30"
	echo "inputs per client tick 1"
	echo "movement seed range 135790-135797"
	echo "movement heading interval 30 inputs"
	echo "CDN requests 25 (20 hot, 5 cold), concurrency 8"
	echo "CDN source bundles 6, 16 MiB plus 64 KiB each"
	echo "seconds $seconds"
	echo "build contention $contended"
	if [[ $contended == yes ]]; then echo "timing classification functional-only"; fi
	echo "prediction corrections not measured (loadtest uses the authoritative Connector path)"
	echo "captured $(date -Is)"
} > "$out/meta.txt"

echo "integrated stress ok: $out"
if [[ $contended == yes ]]; then
	echo "  build contention: yes (functional only)"
else
	echo "  build contention: no"
fi
echo "  hosted endpoints: 8"
echo "  clients playing: 200 / 200"
echo "  observed bus window: accepted peak $bus_accepted_peak; sent peak $bus_sent_peak; dropped total $bus_dropped_total"
echo "  CDN cache: $cache_line"
echo "  CDN fetch: queue high-water $queue_high_water; $(grep 'latency ms p50/p95/p99:' "$cdn_fetch_log" | tr '\n' ';')"
echo "  server timing: $(grep -c 'host-metric ' "$server_log") per-world samples"
