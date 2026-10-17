#!/usr/bin/env bash
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
tmp=$(mktemp -d)
trap 'status=$?; if ((status != 0)) && [[ -f $tmp/coordinator.log ]]; then cat "$tmp/coordinator.log" >&2; fi; rm -rf "$tmp"' EXIT
build="$tmp/release"
fake="$tmp/fake-bin"
mkdir -p "$build/server" "$build/tools" "$build/cdn" "$build/assets/examples/scripts" "$fake"
touch "$build/assets/examples/scripts/Stress.luau"

cat > "$fake/cdn" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' "$*" >> "$FAKE_CDN_ARGS_LOG"
for ((index = 1; index <= $#; index++)); do
	if [[ ${!index} == --publish ]]; then
		printf 'cdn: publisher key %064d\n' 1
		exit 0
	fi
done
python3 -c 'import time; time.sleep(3)'
echo 'cdn: prepared cache hits 20, misses 6'
EOF

cat > "$fake/server" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' "$*" > "$FAKE_SERVER_ARGS_LOG"
for index in $(seq 7 -1 0); do
	world=$(printf 'stress-world-%02d' "$index")
	port=$((52000 + index))
	printf '[fake] host-ready host=host.shared.%s world=%s port=%s\n' "$index" "$world" "$port"
	printf '[fake] host-metric host=host.shared.%s world=%s tick=120 tick-ms=0.25\n' "$index" "$world"
done
echo 'host-bus-window accepted=80 sent=560 dropped=0'
echo 'host-bus-window accepted=0 sent=0 dropped=0'
echo 'world.delivery.staged.messages · 8 over 8 call(s)'
python3 -c 'import time; time.sleep(3)'
EOF

cat > "$fake/loadtest" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' "$*" >> "$FAKE_LOADTEST_ARGS_LOG"
for argument in "$@"; do
	if [[ $argument == --cdn-only ]]; then
		echo 'cdn fetch cohort'
		echo '  queue high-water: 3'
		echo '  hot latency ms p50/p95/p99: 1.000 / 1.000 / 1.000'
		echo '  cold latency ms p50/p95/p99: 2.000 / 2.000 / 2.000'
		exit 0
	fi
done
cat <<'REPORT'
  playing at the end         25
  unique player IDs          25
  moving characters          25
  inputs sent                 25
REPORT
EOF

cat > "$fake/timeout" <<'EOF'
#!/usr/bin/env bash
shift
exec "$@"
EOF

cat > "$fake/curl" <<'EOF'
#!/usr/bin/env bash
exit 0
EOF

cat > "$fake/head" <<'EOF'
#!/usr/bin/env bash
# The fake CDN does not inspect fixture payloads, so keep coordinator tests tiny.
exit 0
EOF

chmod +x "$fake"/* "$root/scripts/integrated-stress-test.sh"
ln -s "$fake/server" "$build/server/server"
ln -s "$fake/loadtest" "$build/tools/loadtest"
ln -s "$fake/cdn" "$build/cdn/cdn"
FAKE_CDN_ARGS_LOG="$tmp/cdn-args.log" \
FAKE_SERVER_ARGS_LOG="$tmp/server-args.log" \
FAKE_LOADTEST_ARGS_LOG="$tmp/loadtest-args.log" \
INTEGRATED_STRESS_CONTENDED=yes \
PATH="$fake:$PATH" "$root/scripts/integrated-stress-test.sh" "$build" 10 45200 > "$tmp/coordinator.log" 2>&1

grep -q 'integrated stress ok:' "$tmp/coordinator.log"
grep -q 'hosted endpoints: 8' "$tmp/coordinator.log"
grep -q 'clients playing: 200 / 200' "$tmp/coordinator.log"
grep -q 'CDN fetch: queue high-water 3;' "$tmp/coordinator.log"
grep -q 'observed bus window: accepted peak 80; sent peak 560; dropped total 0' "$tmp/coordinator.log"
run_dir=$(sed -n 's/^integrated stress ok: //p' "$tmp/coordinator.log")
[[ -f $run_dir/meta.txt ]]
[[ $(find "$run_dir" -name 'stress-world-*-clients.log' -type f | wc -l) -eq 8 ]]
[[ $(find "$run_dir/content" -name '*.agame' -type f | wc -l) -eq 6 ]]
grep -q '^total clients 200$' "$run_dir/meta.txt"
grep -q '^build contention yes$' "$run_dir/meta.txt"
grep -q '^timing classification functional-only$' "$run_dir/meta.txt"
grep -q '^prediction corrections not measured (loadtest uses the authoritative Connector path)$' "$run_dir/meta.txt"
[[ $(grep -o -- '--remote-world' "$tmp/server-args.log" | wc -l) -eq 8 ]]
[[ $(grep -o -- '--remote-world [^ ]*' "$tmp/server-args.log" | sort -u | wc -l) -eq 8 ]]
grep -q -- '--chatter' "$tmp/server-args.log"
grep -q -- '--seconds 25' "$tmp/server-args.log"
[[ $(grep -c -- '--seconds 25' "$tmp/cdn-args.log") -eq 1 ]]
[[ $(grep -v -- '--cdn-only' "$tmp/loadtest-args.log" | wc -l) -eq 8 ]]
[[ $(grep -c -- '--clients 25' "$tmp/loadtest-args.log") -eq 8 ]]
[[ $(awk '/--cdn-only/ { for (i = 1; i < NF; i++) if ($i == "--cdn-requests") print $(i + 1) }' "$tmp/loadtest-args.log") == 25 ]]
[[ $(awk '/--cdn-only/ { for (i = 1; i < NF; i++) if ($i == "--cdn-concurrency") print $(i + 1) }' "$tmp/loadtest-args.log") == 8 ]]
[[ $(awk '{ for (i = 1; i < NF; i++) if ($i == "--random-heading-seed") print $(i + 1) }' "$tmp/loadtest-args.log" | sort -u | wc -l) -eq 8 ]]
[[ $(awk '{ for (i = 1; i < NF; i++) if ($i == "--port") print $(i + 1) }' "$tmp/loadtest-args.log" | sort -u | wc -l) -eq 8 ]]
awk '/--cdn-only/ { next } { port = 0; seed = 0; for (i = 1; i < NF; i++) { if ($i == "--port") port = $(i + 1); if ($i == "--random-heading-seed") seed = $(i + 1) } if (seed != 135790 + port - 52000) exit 1; count++ } END { if (count != 8) exit 1 }' "$tmp/loadtest-args.log"

echo 'integrated stress fake coordinator test passed'
