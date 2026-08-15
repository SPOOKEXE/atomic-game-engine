#!/usr/bin/env bash
#
# Drags the studio's window through several hundred sizes and asks whether it
# is still alive afterwards.
#
# **What this catches, and why it cannot be a unit test.** The viewport panel
# shows *last frame's* scene texture - imgui records its draw lists before the
# renderer runs, so there is no other texture to show. On the frame the panel
# changes size, the order is: the interface records a bind of the old texture,
# `EnsureScene` notices the new size, and then those draw lists are replayed.
# Releasing the old texture in the middle of that hands SDL's Vulkan backend a
# freed `TextureContainer`, and it segfaults in `VULKAN_BindFragmentSamplers`
# with nothing on the stack from this repository above SDL.
#
# Reproducing it needs a real window, a real swapchain and a window manager
# delivering a size change per motion event. That is not something a headless
# test binary can do, which is why this is a script rather than a `TEST_CASE`
# - the same reason `just studio-smoke` is a recipe and not part of `just
# check`.
#
#   scripts/studio-resize-test.sh                    the dev build
#   scripts/studio-resize-test.sh .cache/build/release/studio/studio
#
# Exit 0 means it survived. Exit 1 means it died, and the signal is printed.

set -u

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
binary=${1:-$root/.cache/build/dev/studio/studio}

if [ ! -x "$binary" ]; then
    echo "no studio at $binary - build it first (just studio)" >&2
    exit 2
fi

if ! command -v xdotool > /dev/null; then
    echo "xdotool is not installed, and driving a window is the whole test" >&2
    exit 2
fi

if [ -z "${DISPLAY:-}" ]; then
    echo "no DISPLAY - this needs a real window and a real swapchain" >&2
    exit 2
fi

log=$(mktemp)
"$binary" --width 1200 --height 700 > "$log" 2>&1 &
pid=$!

# **Found by pid, then verified by name.** `xdotool search` defaults to
# `--any`, so `search --pid X --name Y` matches anything matching *either* -
# an earlier version of this script matched a terminal whose title happened to
# contain the repository path, resized that instead, and reported a pass.
window=""
for _ in $(seq 1 60); do
    kill -0 "$pid" 2>/dev/null || break
    for candidate in $(xdotool search --onlyvisible --pid "$pid" 2>/dev/null); do
        if [ "$(xdotool getwindowname "$candidate" 2>/dev/null)" = "atomic studio" ]; then
            window=$candidate
            break 2
        fi
    done
    sleep 0.25
done

if [ -z "$window" ]; then
    echo "FAIL: no studio window appeared" >&2
    sed -n '1,20p' "$log" >&2
    kill "$pid" 2>/dev/null
    rm -f "$log"
    exit 2
fi

if [ "$(xdotool getwindowpid "$window" 2>/dev/null)" != "$pid" ]; then
    echo "FAIL: that window belongs to another process - refusing to resize it" >&2
    kill "$pid" 2>/dev/null
    rm -f "$log"
    exit 2
fi

sleep 1.5

# A drag rather than a handful of jumps: the bug needs the size to change on a
# frame whose draw lists have already been recorded, so it wants many small
# changes rather than a few large ones.
for _ in 1 2 3; do
    for width in $(seq 700 17 1400); do
        kill -0 "$pid" 2>/dev/null || break 2
        xdotool windowsize "$window" "$width" $((width * 2 / 3)) 2>/dev/null
        sleep 0.02
    done
    for width in $(seq 1400 -23 700); do
        kill -0 "$pid" 2>/dev/null || break 2
        xdotool windowsize "$window" "$width" $((width * 3 / 4)) 2>/dev/null
        sleep 0.02
    done
done

sleep 0.5

if kill -0 "$pid" 2>/dev/null; then
    kill "$pid" 2>/dev/null
    wait "$pid" 2>/dev/null
    rm -f "$log"
    echo "studio ok - survived ~250 resizes with the viewport docked"
    exit 0
fi

wait "$pid" 2>/dev/null
status=$?
echo "FAIL: the studio died during a resize (exit $status)" >&2
tail -20 "$log" >&2
rm -f "$log"
exit 1
