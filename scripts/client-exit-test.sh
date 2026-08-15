#!/usr/bin/env bash
#
# Runs the client to a frame budget and asks whether the process actually ends.
#
# **What this catches, and why it cannot be a `TEST_CASE`.** The failure is a
# hang, and a hang is the one outcome a test framework cannot report: Catch2
# has no way to fail a case that never returns, so the suite stops rather than
# says anything. Only an external timeout turns "never finished" into a
# failure, which is what makes this a script.
#
# The bug it was written for: `Client::Shutdown` released the GPU device while
# `InterfacePass` still held a raw pointer to it, so `~InterfacePass` released
# its atlas through a freed device and blocked forever on a mutex that no
# longer existed. `Run()` had already drawn its frames, printed its statistics
# and returned 0 - the log of a hung run and of a healthy one are identical up
# to the last line, which is why nobody reading output would spot it.
#
# **Both modes, because the gap between them is what hid it.** Teardown used to
# be guarded by `if (Window)`, so a headless run skipped the device entirely
# and `just client-smoke` walked an order the shipped windowed run never takes.
# Headless passing is now evidence about the windowed path because they are one
# path; this checks both anyway, since that is the property being relied on.
#
#   scripts/client-exit-test.sh                     the dev build
#   scripts/client-exit-test.sh .cache/build/release/client/client
#
# The windowed half is skipped without a DISPLAY rather than failed. Exit 0
# means every mode that could run ended on its own.

set -u

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
binary=${1:-$root/.cache/build/dev/client/client}

if [ ! -x "$binary" ]; then
    echo "no client at $binary - build it first (just build client)" >&2
    exit 2
fi

if ! command -v timeout > /dev/null; then
    echo "coreutils timeout is missing, and bounding the run is the whole test" >&2
    exit 2
fi

frames=20

# Generous against a slow machine and still far under a hang. A run that draws
# twenty frames and tears down takes under a second here; anything past this is
# not slowness.
limit=60

log=$(mktemp)
trap 'rm -f "$log"' EXIT

# Runs one mode and reports. `$1` names it, the rest are extra flags.
run_mode() {
    local name=$1
    shift

    timeout "$limit" "$binary" --frames "$frames" --width 640 --height 360 "$@" \
        > "$log" 2>&1
    local status=$?

    if [ "$status" -eq 124 ]; then
        echo "FAIL: the $name client drew its $frames frames and never exited" >&2
        echo "      (SIGTERM after ${limit}s; teardown is where it is stuck)" >&2
        tail -6 "$log" >&2
        return 1
    fi

    if [ "$status" -ne 0 ]; then
        echo "FAIL: the $name client exited $status" >&2
        tail -20 "$log" >&2
        return 1
    fi

    # **Exiting is not enough on its own.** A run that failed to start would
    # also end promptly, and would say nothing about teardown - so the budget
    # line is what proves the process got as far as having something to tear
    # down.
    if ! grep -q "frame budget of $frames reached" "$log"; then
        echo "FAIL: the $name client exited without reaching its frame budget" >&2
        tail -20 "$log" >&2
        return 1
    fi

    echo "  $name ok - $frames frames drawn and the process ended on its own"
    return 0
}

failed=0

run_mode headless --headless || failed=1

if [ -n "${DISPLAY:-}" ] || [ -n "${WAYLAND_DISPLAY:-}" ]; then
    # **The mode the bug actually shipped in.** A window means a swapchain,
    # which means the interface pass has an atlas to release, which is the
    # resource the freed device was released through.
    run_mode windowed || failed=1
else
    echo "  windowed skipped - no DISPLAY or WAYLAND_DISPLAY"
fi

if [ "$failed" -ne 0 ]; then
    exit 1
fi

echo "client ok - every mode that could run ended on its own"
