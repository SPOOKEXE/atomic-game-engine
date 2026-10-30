#!/usr/bin/env bash
set -euo pipefail

source_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)
tmp=$(mktemp -d)
trap 'rm -rf -- "$tmp"' EXIT
root="$tmp/checkout with spaces"
mkdir -p "$root/scripts" "$root/.cache/build/dev" "$root/.cache/build/bench" "$root/.cache/build/foreign" "$tmp/outside"
cp "$source_root/scripts/clean-build.sh" "$root/scripts/clean-build.sh"
for preset in dev bench; do
    printf 'CMAKE_HOME_DIRECTORY:INTERNAL=%s\n' "$root" > "$root/.cache/build/$preset/CMakeCache.txt"
    printf 'data\n' > "$root/.cache/build/$preset/data"
done
printf 'CMAKE_HOME_DIRECTORY:INTERNAL=%s\n' "$tmp/foreign" > "$root/.cache/build/foreign/CMakeCache.txt"
printf 'keep\n' > "$tmp/outside/data"
ln -s "$tmp/outside" "$root/.cache/build/link"
mkdir -p "$root/.cache/build/cache-link"
ln -s "$root/.cache/build/bench/CMakeCache.txt" "$root/.cache/build/cache-link/CMakeCache.txt"
clean="$root/scripts/clean-build.sh"

bash "$clean" dev | grep -q reclaimable
[[ -f "$root/.cache/build/dev/data" ]]
for bad in '../outside' '.' 'dev/..' '' 'dev;touch evil'; do
    if bash "$clean" "$bad" yes >/dev/null 2>&1; then
        echo "Accepted invalid preset: $bad" >&2
        exit 1
    fi
done
for bad in link cache-link foreign missing; do
    if bash "$clean" "$bad" yes >/dev/null 2>&1; then
        echo "Accepted unsafe build: $bad" >&2
        exit 1
    fi
done
if bash "$clean" dev definitely >/dev/null 2>&1; then
    echo 'Accepted invalid apply argument' >&2
    exit 1
fi
bash "$clean" dev yes | grep -q reclaimed
[[ ! -e "$root/.cache/build/dev" && -f "$root/.cache/build/bench/data" && -f "$tmp/outside/data" ]]

mv "$root/.cache/build" "$root/.cache/real-build"
ln -s "$root/.cache/real-build" "$root/.cache/build"
if bash "$clean" bench yes >/dev/null 2>&1; then
    echo 'Accepted symlinked build root' >&2
    exit 1
fi
[[ -f "$root/.cache/real-build/bench/data" ]]
echo 'clean-build tests passed'
