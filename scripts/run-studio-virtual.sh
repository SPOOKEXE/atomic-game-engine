#!/usr/bin/env bash
#
# Runs the complete windowed Studio on a private X display with disposable
# configuration. It keeps automated mouse, docking and resize work away from a
# person's desktop while still exercising SDL's real window and swapchain path.
#
#   scripts/run-studio-virtual.sh --frames 120 --run server
#   ATOMIC_VIRTUAL_SCREEN=2560x1440x24 scripts/run-studio-virtual.sh

set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)

if ! command -v xvfb-run > /dev/null; then
    echo "xvfb-run is not installed; install Xvfb to run Studio on a private display." >&2
    exit 2
fi

virtual_config=$(mktemp -d "${TMPDIR:-/tmp}/atomic-studio-virtual.XXXXXX")
cleanup() {
    rm -rf -- "$virtual_config"
}
trap cleanup EXIT

screen=${ATOMIC_VIRTUAL_SCREEN:-1920x1080x24}

xvfb-run -a -s "-screen 0 $screen -nolisten tcp" \
    "$here/run-studio.sh" --config-root "$virtual_config" "$@"
