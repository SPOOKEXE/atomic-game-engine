"""Refresh the Pixel Composer node matrix from source and executor candidates.

Static matches identify files for review. They do not prove a node was executed
or that its controls and edge cases have fixtures. Licensed reference comparison
is tracked separately from native validation.

Usage: uv run scripts/pixel-composer/update-matrix.py [--check]
"""

import argparse
import csv
import io
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
MATRIX = REPO / "docs/pixel-composer-m0/node-parity-matrix.csv"
CATALOGUE = REPO / "mono.engine/imagegraph/src/SourceCatalogue.inc"
EXECUTORS = REPO / "mono.engine/imagegraph/src/nodes"
TESTS = REPO / "mono.engine/imagegraph/tests"
PIN = "b69eca232217360cf1502ef0223523d818606652"
IMPLEMENTED = "documented and implemented"
BLOCKED = "documented and blocked by named prerequisite"
REVIEW_PENDING = "native executor and test references found; full controls/edge-case fixture review pending"


def read_catalogue(path: Path) -> dict[str, dict[str, object]]:
    entries: dict[str, dict[str, object]] = {}
    current: dict[str, object] | None = None
    for line in path.read_text(encoding="utf-8").splitlines():
        fields = line.split("\t")
        if fields[0] == "N" and len(fields) == 6:
            current = {"type": fields[1], "file": fields[5], "inputs": []}
            entries[fields[2]] = current
        elif fields[0] == "I" and current is not None and len(fields) == 8:
            current["inputs"].append((fields[1], fields[5], fields[6]))
    return entries


def find_native_candidates(executor_dir: Path, test_dir: Path) -> dict[str, list[str]]:
    executors: set[str] = set()
    for source in executor_dir.glob("*.cpp"):
        executors.update(
            re.findall(
                r'\{\s*"(pc\.[a-z0-9_]+)"\s*,\s*[A-Za-z0-9_]+\s*(?:,\s*(?:true|false))?\s*\}',
                source.read_text(encoding="utf-8"),
            )
        )

    test_references: dict[str, set[str]] = {}
    for test in sorted(test_dir.glob("*.cpp")):
        for node_type in set(re.findall(r'"(pc\.[a-z0-9_]+)"', test.read_text(encoding="utf-8"))):
            test_references.setdefault(node_type, set()).add(test.name)

    candidates: dict[str, list[str]] = {}
    for node_type in sorted(executors & test_references.keys()):
        candidates[node_type] = [f"executor registry: {node_type}"]
        candidates[node_type].extend(
            f"test reference: mono.engine/imagegraph/tests/{name}"
            for name in sorted(test_references[node_type])
        )
    return candidates


def has_named_validation_evidence(row: dict[str, str]) -> bool:
    fixture = row.get("fixture", "").strip()
    if not fixture or fixture == "unrecorded":
        return False
    if re.fullmatch(r"native executor pc\.[a-z0-9_]+; exact fixtures in .+", fixture):
        return False
    return bool(
        re.search(r"(?:case|fixture|assertion|pixel|control|edge|input|output)", fixture, re.IGNORECASE)
        and re.search(r"(?:tests?/|test_imagegraph|cases?\b|assertions?\b)", fixture, re.IGNORECASE)
        and re.search(r"\bpassed\b", fixture, re.IGNORECASE)
    )


def refreshed_defaults(row: dict[str, str], entry: dict[str, object]) -> str:
    inputs = entry["inputs"]
    defaults = "; ".join(f"{identifier}={default or 'runtime'}" for identifier, _, default in inputs)
    generated = f"pinned source {PIN} {entry['file']}: {defaults}"
    old = row.get("defaults", "")
    if not old.strip():
        return generated

    input_ids = {identifier for identifier, _, _ in inputs}
    if old.startswith("pinned source "):
        _, separator, old_values = old.partition(": ")
        if separator:
            notes = [
                part for part in old_values.split("; ")
                if part and part.split("=", 1)[0] not in input_ids
            ]
            if notes:
                return f"{generated}; {'; '.join(notes)}"
            return generated
    return f"{generated}; {old.strip()}"


def preserved_prerequisite(prerequisite: str, node_type: str) -> str:
    generated_catalogue_note = (
        f"catalogue type {node_type} loads, saves, imports from PXC and edits in Studio"
    )
    boilerplate = {
        generated_catalogue_note.lower(),
        "native executor and exact fixture pending",
        "native executor and named test validation evidence pending",
        "licensed executable comparison unavailable",
    }
    boilerplate.update(part.strip().lower() for part in REVIEW_PENDING.split(";"))
    details = [
        part.strip() for part in prerequisite.split(";")
        if part.strip() and part.strip().lower() not in boilerplate
        and "licensed executable comparison unavailable" not in part.lower()
    ]
    return "; ".join(details)


def preserved_fixture(fixture: str) -> str:
    value = fixture.strip()
    if value == "unrecorded" or not value:
        return ""
    if re.fullmatch(r"native executor pc\.[a-z0-9_]+; exact fixtures in .+", value):
        return ""
    if value == "no matched native executor and named test references":
        return ""
    return value


def refresh_rows(
    rows: list[dict[str, str]],
    entries: dict[str, dict[str, object]],
    candidates: dict[str, list[str]],
) -> tuple[list[dict[str, str]], int]:
    for row in rows:
        row.setdefault("native_evidence_candidate", "")
        entry = entries.get(row["node_id"])
        if not entry:
            continue

        row["defaults"] = refreshed_defaults(row, entry)

        node_type = entry["type"]
        row["native_evidence_candidate"] = "; ".join(candidates.get(node_type, []))
        status = row.get("status", "")
        prerequisite = row.get("prerequisite", "")
        if "prohibited by engine policy" in f"{status} {prerequisite}".lower():
            continue
        if row["status"] == "unknown":
            continue
        candidate_exists = bool(row["native_evidence_candidate"])
        if row["status"] == IMPLEMENTED and candidate_exists and has_named_validation_evidence(row):
            row["prerequisite"] = preserved_prerequisite(prerequisite, node_type)
            continue

        row["status"] = BLOCKED
        previous_details = preserved_prerequisite(prerequisite, node_type)
        fixture_details = preserved_fixture(row.get("fixture", ""))
        if candidate_exists:
            row["fixture"] = fixture_details or REVIEW_PENDING
            row["prerequisite"] = "; ".join(part for part in (previous_details, REVIEW_PENDING) if part)
        else:
            row["fixture"] = fixture_details or "no matched native executor and named test references"
            missing = "native executor and named test validation evidence pending"
            row["prerequisite"] = "; ".join(part for part in (previous_details, missing) if part)
    return rows, sum(row["status"] == IMPLEMENTED for row in rows)


def serialize_rows(rows: list[dict[str, str]], fieldnames: list[str]) -> bytes:
    if "native_evidence_candidate" not in fieldnames:
        fieldnames.append("native_evidence_candidate")
    output = io.StringIO(newline="")
    writer = csv.DictWriter(output, fieldnames=fieldnames)
    writer.writeheader()
    writer.writerows(rows)
    return output.getvalue().encode("utf-8")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="fail if the matrix would change; do not write it")
    args = parser.parse_args(argv)

    with MATRIX.open(encoding="utf-8", newline="") as handle:
        reader = csv.DictReader(handle)
        rows = list(reader)
        fieldnames = list(reader.fieldnames or [])
    rows, implemented = refresh_rows(rows, read_catalogue(CATALOGUE), find_native_candidates(EXECUTORS, TESTS))
    generated = serialize_rows(rows, fieldnames)
    current = MATRIX.read_bytes()
    if args.check:
        if generated != current:
            print("pixel composer matrix is out of date", file=sys.stderr)
            return 1
        print("pixel composer matrix is current")
        return 0

    MATRIX.write_bytes(generated)
    print(f"{implemented} rows retain named native validation evidence")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
