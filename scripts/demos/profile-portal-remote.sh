#!/usr/bin/env bash
# Profile the product's process-hosted destination image handoff at 30 and 60 Hz.
# The existing GPU test asserts the remote image and crossing before this script
# accepts its sidecars as profiling evidence.

set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$here/../.." && pwd)
build="$root/.cache/build/release-tests"
out=${OUT:-$root/.cache/portal-profile/remote}
test_program="$build/tests/test_client"

if [ ! -x "$test_program" ]; then
	echo "no prebuilt release-tests client suite at $test_program" >&2
	exit 1
fi
mkdir -p "$out"
test_status=${TEST_STATUS:-0}
if [ "${EXTRACT_ONLY:-0}" != 1 ]; then
	if PORTAL_PROFILE_SNAPSHOT=1 "$test_program" '[portal-product-image-handoff]' --reporter compact > "$out/test.log" 2>&1; then
		test_status=0
	else
		test_status=$?
	fi
fi

python3 - "$build/tests" "$out" "$test_status" <<'PY'
import gzip
import json
import shutil
import sys
from pathlib import Path

base, out = map(Path, sys.argv[1:3])
test_status = int(sys.argv[3])
summary = []
for tick_rate in (30, 60):
    sequence = base / f"portal-client-walk-{tick_rate}-third-explicit-held-clear-image-frames"
    frames = sorted(
        (path for path in sequence.glob("[0-9]*.json") if path.stem.isdigit()),
        key=lambda path: int(path.stem),
    )
    if len(frames) != 600:
        raise SystemExit(f"{sequence}: expected 600 sidecars, found {len(frames)}")
    sidecars = [json.loads(path.read_text()) for path in frames]
    import_uploads = [sample.get("portal_import", {}).get("uploaded_bytes", 0) for sample in sidecars]
    decoded = [sample.get("portal_inbox", {}).get("decoded_bytes", 0) for sample in sidecars]
    pending = [sample.get("portal_inbox", {}).get("pending_count", 0) for sample in sidecars]
    pending_capacity = [sample.get("portal_inbox", {}).get("pending_capacity", 0) for sample in sidecars]
    stale = [sample.get("portal_inbox", {}).get("stale_rejections", 0) for sample in sidecars]
    ages = [
        capture["accepted_age_ms"]
        for sample in sidecars
        for portal in sample.get("portal_views", [])
        if portal.get("external")
        for capture in [portal.get("capture")]
        if isinstance(capture, dict) and "accepted_age_ms" in capture
    ]
    if not max(import_uploads) or not max(decoded) or not ages:
        raise SystemExit(f"{sequence}: remote import, decode or image-age evidence absent")
    snapshot = sequence / "frame-graph-snapshot.txt"
    if not snapshot.is_file():
        raise SystemExit(f"{sequence}: frame graph snapshot absent")
    shutil.copy2(snapshot, out / f"frame-graph-{tick_rate}hz.txt")
    with gzip.open(out / f"sidecars-{tick_rate}hz.jsonl.gz", "wt") as archive:
        for sample in sidecars:
            archive.write(json.dumps(sample, separators=(",", ":")) + "\n")
    visibility = sorted(
        sequence.glob("[0-9]*.visibility.json"),
        key=lambda path: int(path.name.split(".", 1)[0]),
    )
    if visibility:
        with gzip.open(out / f"visibility-{tick_rate}hz.jsonl.gz", "wt") as archive:
            for path in visibility:
                archive.write(path.read_text().strip() + "\n")
    renderer_frames = sorted(
        sequence.glob("[0-9]*.renderer.json"),
        key=lambda path: int(path.name.split(".", 1)[0]),
    )
    if renderer_frames:
        with gzip.open(out / f"renderer-frames-{tick_rate}hz.jsonl.gz", "wt") as archive:
            for path in renderer_frames:
                archive.write(path.read_text().strip() + "\n")
    summary.append({
        "world_tick_rate_hz": tick_rate,
        "functional_test_passed": test_status == 0,
        "frames": len(sidecars),
        "remote_capture_samples": len(ages),
        "import_uploaded_bytes_max": max(import_uploads),
        "decoded_image_bytes_max": max(decoded),
        "inbox_pending_count_max": max(pending),
        "inbox_pending_capacity_max": max(pending_capacity),
        "inbox_stale_rejections_max": max(stale),
        "accepted_image_age_ms_max": max(ages),
        "frame_graph_snapshot": str(out / f"frame-graph-{tick_rate}hz.txt"),
    })
(out / "remote-profile.json").write_text(json.dumps(summary, indent=2) + "\n")
print(json.dumps(summary, indent=2))
PY
exit "$test_status"
