#!/usr/bin/env bash
# Repeat the deterministic process transfer under baseline and impaired traffic.
# Handoff duration is measured in 60 Hz simulation ticks from transfer start.

set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$here/../.." && pwd)
build="$root/.cache/build/release-tests"
out=${OUT:-$root/.cache/portal-profile/handoff}
test_program="$build/tests/test_script"

if [ ! -x "$test_program" ]; then
	echo "no prebuilt release-tests script suite at $test_program" >&2
	exit 1
fi
mkdir -p "$out"
PORTAL_PROFILE_REPEATS=100 "$test_program" '[script][portal-transfer][process]' --reporter compact > "$out/test.log" 2>&1

python3 - "$out" <<'PY'
import json
import math
import re
import sys
from pathlib import Path

out = Path(sys.argv[1])
pattern = re.compile(
    r"portal process impairment rtt_ms=(\d+) jitter_ms=(\d+) loss_percent=(\d+) "
    r"sample=(\d+) committed_at_tick=(\d+) dropped=(\d+) duplicated=(\d+) reordered=(\d+)"
)
groups = {}
for match in pattern.finditer((out / "test.log").read_text()):
    rtt, jitter, loss, sample, committed, dropped, duplicated, reordered = map(int, match.groups())
    groups.setdefault((rtt, jitter, loss), []).append({
        "sample": sample,
        "handoff_ticks": committed - 5,
        "dropped": dropped,
        "duplicated": duplicated,
        "reordered": reordered,
    })
if set(groups) != {(0, 0, 0), (150, 30, 5)}:
    raise SystemExit(f"unexpected handoff profiles: {sorted(groups)}")
report = []
for (rtt, jitter, loss), samples in sorted(groups.items()):
    if len(samples) != 100 or {entry["sample"] for entry in samples} != set(range(100)):
        raise SystemExit(f"profile {(rtt, jitter, loss)} lacks 100 distinct committed samples")
    durations = sorted(entry["handoff_ticks"] for entry in samples)
    percentile = lambda fraction: durations[math.ceil(fraction * len(durations)) - 1]
    report.append({
        "rtt_ms": rtt,
        "jitter_ms": jitter,
        "loss_percent": loss,
        "samples": len(samples),
        "handoff_ticks_p50": percentile(0.50),
        "handoff_ticks_p95": percentile(0.95),
        "handoff_ticks_p99": percentile(0.99),
        "handoff_ticks_max": durations[-1],
        "handoff_ms_p95_at_60hz": percentile(0.95) * 1000 / 60,
        "handoff_ms_p99_at_60hz": percentile(0.99) * 1000 / 60,
        "faults_observed": {
            "dropped": sum(entry["dropped"] for entry in samples),
            "duplicated": sum(entry["duplicated"] for entry in samples),
            "reordered": sum(entry["reordered"] for entry in samples),
        },
    })
(out / "handoff-profile.json").write_text(json.dumps(report, indent=2) + "\n")
print(json.dumps(report, indent=2))
PY
