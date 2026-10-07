#!/usr/bin/env bash
# Fresh processes keep each of the three five-second samples independent.
# Raw observations stay beneath the build; the document is generated separately.
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
build="$root/.cache/build/profile"
output=""
label=""
workloads="$root/scripts/demos/version-impact-workloads.tsv"
allow_added_missing=false
while (($#)); do
	case "$1" in
		--root) root=$(cd -- "$2" && pwd); shift 2 ;;
		--build) build=$2; shift 2 ;;
		--output) output=$2; shift 2 ;;
		--label) label=$2; shift 2 ;;
		--workloads) workloads=$2; shift 2 ;;
		--allow-added-missing) allow_added_missing=true; shift ;;
		*) echo "unknown argument: $1" >&2; exit 2 ;;
	esac
done
build=$(cd -- "$build" && pwd)
output=${output:-"$build/version-impact/current"}
mkdir -p -- "$output"
output=$(cd -- "$output" && pwd)
case "$output/" in "$build/"*) ;; *) echo "reports must be beneath the build directory" >&2; exit 2 ;; esac
test -x "$build/client/client"
test -x "$build/server/server"
test -f "$workloads"
test -x /usr/bin/time || { echo "GNU time is required for peak process RSS" >&2; exit 2; }
test ! -e "$output/manifest.json" || { echo "choose a fresh output directory: $output" >&2; exit 2; }
revision=$(git -C "$root" rev-parse HEAD)
label=${label:-$(git -C "$root" branch --show-current)}
label=${label:-$(git -C "$root" rev-parse --short HEAD)}
dirty=false
test -z "$(git -C "$root" status --porcelain --untracked-files=normal)" || dirty=true
source_diff=$( { git -C "$root" diff HEAD -- . ':!docs/VERSION_IMPACT.md'; while IFS= read -r -d '' changed_path; do sha256sum "$root/$changed_path"; done < <(git -C "$root" ls-files --others --exclude-standard -z); } | sha256sum | cut -d' ' -f1)
client_binary=$(sha256sum "$build/client/client" | cut -d' ' -f1)
server_binary=$(sha256sum "$build/server/server" | cut -d' ' -f1)
os=$(uname -srvmo)
cpu=$(sed -n 's/^model name[[:space:]]*: //p' /proc/cpuinfo | head -1)
compiler=$(c++ --version)
gpu="unavailable"
if command -v nvidia-smi >/dev/null; then gpu=$(nvidia-smi --query-gpu=name,driver_version --format=csv,noheader); elif command -v lspci >/dev/null; then gpu=$(lspci -nn | sed -n '/VGA\|3D controller/p'); fi
machine=$(printf '%s\n' "$os" "$cpu" "$gpu" "$compiler" "${VK_DRIVER_FILES:-default}" "${VK_ICD_FILENAMES:-default}" | sha256sum | cut -d' ' -f1)
json_string() {
	local value=$1
	value=${value//\\/\\\\}; value=${value//\"/\\\"}
	value=${value//$'\n'/\\n}; value=${value//$'\r'/\\r}; value=${value//$'\t'/\\t}
	printf '"%s"' "$value"
}
server_pid=""
viewer_pid=""
cleanup() {
	if [[ -n "$viewer_pid" ]]; then
		kill "$viewer_pid" 2>/dev/null || true
		wait "$viewer_pid" 2>/dev/null || true
		viewer_pid=""
	fi
	if [[ -n "$server_pid" ]]; then
		kill "$server_pid" 2>/dev/null || true
		wait "$server_pid" 2>/dev/null || true
		server_pid=""
	fi
}
trap cleanup EXIT
manifest="$output/manifest.partial.json"
{
	printf '{"schema":1,"source_revision":"%s","source_dirty":%s,"source_diff_sha256":"%s","client_binary_sha256":"%s","server_binary_sha256":"%s","label":' "$revision" "$dirty" "$source_diff" "$client_binary" "$server_binary"
	json_string "$label"
	printf ',"machine":{"fingerprint":"%s","cpu":' "$machine"
	json_string "$cpu"; printf ',"gpu":'; json_string "$gpu"; printf ',"os":'; json_string "$os"; printf ',"compiler":'; json_string "$compiler"
	printf '},"settings":{"runs":3,"seconds":5,"preset":"profile","width":960,"height":540,"compute":"serial","max_fps":0,"replica_viewer_max_fps":60},"workloads":['
} > "$manifest"
separator=""
while read -r name kind asset availability; do
	[[ -z "$name" || "$name" == \#* ]] && continue
	[[ "$name" =~ ^[A-Za-z0-9_-]+$ ]] || { echo "invalid workload name" >&2; exit 2; }
	asset_kind=scripts
	[[ "$kind" == client-world ]] && asset_kind=worlds
	path="$build/assets/examples/$asset_kind/$asset"
	[[ -f "$path" ]] || path="$build/client/assets/examples/$asset_kind/$asset"
	status=measured
	if [[ ! -f "$path" ]]; then
		[[ "$availability" == added && "$allow_added_missing" == true ]] || { echo "missing required demo: $asset" >&2; exit 1; }
		status=unavailable
	fi
	printf '%s{"name":"%s","kind":"%s","status":"%s"' "$separator" "$name" "$kind" "$status" >> "$manifest"
	separator=,
	if [[ "$status" == unavailable ]]; then printf ',"reports":[]}' >> "$manifest"; continue; fi
	hash=$(sha256sum "$path" | cut -d' ' -f1)
	printf ',"asset_sha256":"%s","reports":[' "$hash" >> "$manifest"
	mkdir -p "$output/$name"
	for run in 1 2 3; do
		[[ $(sha256sum "$build/client/client" | cut -d' ' -f1) == "$client_binary" && $(sha256sum "$build/server/server" | cut -d' ' -f1) == "$server_binary" ]] || { echo "binaries changed during collection; choose fresh output and rerun" >&2; exit 1; }
		printf '%s sample %s/3, five seconds\n' "$name" "$run"
		report="$output/$name/run-$run.json"
		log="$output/$name/run-$run.log"
		measure=(/usr/bin/time -o "$report.rss" -f '%M')
		common=(--headless --frames 1000000000 --uncapped --max-fps 0 --width 960 --height 540 --force-serial-compute --profile-seconds 5 --benchmark-report "$report")
		case "$kind" in
			server-replica)
				timeout --foreground --kill-after=10s 180s "${measure[@]}" "$build/server/server" --game "$path" --listen 0 --transport datagram --force-serial-compute --benchmark-seconds 5 --benchmark-report "$report" --benchmark-wait-for-client > "$log" 2>&1 &
				server_pid=$!
				port=""
				for ((attempt=0; attempt<600; attempt++)); do
					kill -0 "$server_pid" 2>/dev/null || { cat "$log" >&2; exit 1; }
					port=$(sed -nE 's/.*replication listening on .*:([0-9]+) over.*/\1/p' "$log" | head -1)
					[[ -n "$port" ]] && break
					sleep 0.1
				 done
				[[ -n "$port" ]] || { echo "server readiness timeout" >&2; exit 1; }
				timeout --foreground --kill-after=10s 180s "$build/client/client" --headless --frames 1000000000 --uncapped --max-fps 60 --width 960 --height 540 --force-serial-compute --script "$path" --connect "127.0.0.1:$port" > "$output/$name/viewer-$run.log" 2>&1 &
				viewer_pid=$!
				wait "$server_pid"
				server_pid=""
				if ! kill -0 "$viewer_pid" 2>/dev/null; then
					wait "$viewer_pid" || true
					viewer_pid=""
					cat "$output/$name/viewer-$run.log" >&2
					echo "server viewer exited before measurement completed" >&2
					exit 1
				fi
				grep -q 'joined: [1-9][0-9]* entities' "$output/$name/viewer-$run.log" || { echo "server viewer did not join" >&2; exit 1; }
				cleanup ;;
			server)
				timeout --foreground --kill-after=10s 180s "${measure[@]}" "$build/server/server" --game "$path" --force-serial-compute --benchmark-seconds 5 --benchmark-report "$report" > "$log" 2>&1 ;;
			replica)
				server_log="$output/$name/server-$run.log"
				"$build/server/server" --game "$path" --listen 0 --transport datagram --force-serial-compute > "$server_log" 2>&1 &
				server_pid=$!
				port=""
				for ((attempt=0; attempt<600; attempt++)); do
					kill -0 "$server_pid" 2>/dev/null || { cat "$server_log" >&2; exit 1; }
					port=$(sed -nE 's/.*replication listening on .*:([0-9]+) over.*/\1/p' "$server_log" | head -1)
					[[ -n "$port" ]] && break
					sleep 0.1
				 done
				[[ -n "$port" ]] || { echo "server readiness timeout" >&2; exit 1; }
				timeout --foreground --kill-after=10s 180s "${measure[@]}" "$build/client/client" "${common[@]}" --script "$path" --connect "127.0.0.1:$port" --benchmark-wait-for-join > "$log" 2>&1
				grep -q 'joined: [1-9][0-9]* entities' "$log" || { echo "replication demo did not join" >&2; exit 1; }
				cleanup ;;
			client-world)
				timeout --foreground --kill-after=10s 180s "${measure[@]}" "$build/client/client" "${common[@]}" --game "$path" > "$log" 2>&1 ;;
			client)
				extra=()
				if [[ "$name" == RenderFeatures ]]; then
					pipeline="$build/assets/examples/pipelines/RenderFeatures.pipeline"
					[[ -f "$pipeline" ]] || pipeline="$build/client/assets/examples/pipelines/RenderFeatures.pipeline"
					test -f "$pipeline"
					extra=(--render-pipeline "$pipeline")
				fi
				timeout --foreground --kill-after=10s 180s "${measure[@]}" "$build/client/client" "${common[@]}" --script "$path" "${extra[@]}" > "$log" 2>&1 ;;
			*) echo "unknown workload kind: $kind" >&2; exit 2 ;;
		esac
		test -s "$report" || { cat "$log" >&2; echo "no benchmark report: $name" >&2; exit 1; }
		[[ "$run" == 1 ]] || printf ',' >> "$manifest"
		printf '"%s/run-%s.json"' "$name" "$run" >> "$manifest"
	done
	printf ']}' >> "$manifest"
done < "$workloads"
printf ']}\n' >> "$manifest"
mv "$manifest" "$output/manifest.json"
printf 'complete reports: %s\n' "$output"
