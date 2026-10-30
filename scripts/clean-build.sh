#!/usr/bin/env bash
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
preset=${1:-}
apply=${2:-no}
if [[ $# -gt 2 || ! $preset =~ ^[A-Za-z0-9_-]+$ || ( $apply != no && $apply != yes ) ]]; then
    echo "Use 'just clean-build <preset>' to preview or 'just clean-build <preset> yes' to delete." >&2
    exit 2
fi

# Refuse redirects at every path component before checking ownership.
for component in "$root/.cache" "$root/.cache/build" "$root/.cache/build/$preset"; do
    if [[ -L $component ]]; then
        echo "Refusing symlink: $component" >&2
        exit 1
    fi
done
dir="$root/.cache/build/$preset"
cache="$dir/CMakeCache.txt"
if [[ ! -d $dir || ! -f $cache || -L $cache ]] || ! grep -qxF "CMAKE_HOME_DIRECTORY:INTERNAL=$root" "$cache"; then
    echo "Not a build from this checkout: $dir" >&2
    exit 1
fi

kib=$(du -skx -- "$dir" | cut -f1)
if [[ $apply == yes ]]; then
    case $(uname -s) in
        Linux) rm -rf --one-file-system -- "$dir" ;;
        Darwin|FreeBSD|NetBSD) rm -rfx -- "$dir" ;;
        *) echo 'Cannot safely restrict cleanup to one filesystem on this platform.' >&2; exit 1 ;;
    esac
    result=reclaimed
else
    result=reclaimable
fi
printf '%s: %d MiB %s\n' ".cache/build/$preset" "$((kib / 1024))" "$result"
