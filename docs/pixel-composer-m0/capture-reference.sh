#!/usr/bin/env bash
set -euo pipefail

if (( $# < 4 )); then
    echo "usage: $0 PIXEL_COMPOSER PROJECT.pxc EXPECTED_OUTPUT CAPTURE_DIR [CLI_ARGS...]" >&2
    exit 2
fi

composer=$(realpath "$1")
project=$(realpath "$2")
output=$(realpath -m "$3")
capture=$(realpath -m "$4")
shift 4

if [[ ! -f "$composer" || ! -f "$project" ]]; then
    echo "executable and project must be existing files" >&2
    exit 2
fi
if [[ -e "$output" ]]; then
    echo "expected output already exists; use a fresh fixture destination" >&2
    exit 2
fi
if [[ -e "$capture" ]]; then
    echo "capture directory already exists; use a fresh destination" >&2
    exit 2
fi

mkdir -p "$capture"
printf 'field\tvalue\n' > "$capture/manifest.tsv"
printf 'executable_sha256\t%s\n' "$(sha256sum "$composer" | cut -d ' ' -f 1)" >> "$capture/manifest.tsv"
printf 'project_sha256\t%s\n' "$(sha256sum "$project" | cut -d ' ' -f 1)" >> "$capture/manifest.tsv"
printf 'platform\t%s\n' "$(uname -srm)" >> "$capture/manifest.tsv"
printf 'captured_utc\t%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" >> "$capture/manifest.tsv"
printf 'project\t%s\n' "$project" >> "$capture/manifest.tsv"
printf 'output\t%s\n' "$output" >> "$capture/manifest.tsv"

# Pixel Composer documents the project path followed by -h and optional globals.
# Run from the project directory so relative input assets resolve as authored.
set +e
( cd "$(dirname "$project")" && timeout 120s "$composer" "$project" -h "$@" ) > "$capture/stdout.txt" 2> "$capture/stderr.txt"
result=$?
set -e
printf 'exit_code\t%s\n' "$result" >> "$capture/manifest.tsv"
if (( result != 0 )); then
    echo "reference process failed with exit code $result; see $capture" >&2
    exit "$result"
fi
if [[ ! -f "$output" ]]; then
    echo "reference process created no expected output; see $capture" >&2
    exit 1
fi
printf 'output_sha256\t%s\n' "$(sha256sum "$output" | cut -d ' ' -f 1)" >> "$capture/manifest.tsv"
printf 'output_bytes\t%s\n' "$(wc -c < "$output")" >> "$capture/manifest.tsv"
echo "$capture"
