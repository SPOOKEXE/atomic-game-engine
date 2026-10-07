#!/usr/bin/env bash
# Recollect the reference and candidate on one host before enforcing ceilings.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
baseline_ref=${1:-origin/v0.24.1}
document=${2:-"$root/docs/VERSION_IMPACT.md"}
reference="$root/.cache/build/version-impact-reference"
candidate_build="$root/.cache/build/profile"
if [[ ! -d "$reference/.git" && ! -f "$reference/.git" ]]; then
	git -C "$root" worktree add --detach "$reference" "$baseline_ref"
fi
[[ $(git -C "$reference" rev-parse HEAD) == $(git -C "$root" rev-parse "$baseline_ref") ]] || {
	echo "reference worktree differs from $baseline_ref; remove it with git worktree remove before retrying" >&2
	exit 2
}
git -C "$reference" submodule update --init --recursive --jobs 8
if [[ ! -f "$reference/mono.vendor/shaderc/third_party/glslang/CMakeLists.txt" ]]; then
	(cd "$reference" && python3 mono.vendor/shaderc/utils/git-sync-deps)
fi
for source_root in "$reference" "$root"; do
	(cd "$source_root" && cmake --preset profile -DMONO_BUILD_BENCH=ON && cmake --build --preset profile --target client server benchrunner -j "${BUILD_JOBS:-8}")
done
stamp=$(date -u +%Y%m%dT%H%M%SZ)-$$
reference_output="$reference/.cache/build/profile/version-impact/$stamp"
candidate_output="$candidate_build/version-impact/$stamp"
# Use the candidate's workload list for both: additions are explicit unavailable
# rows in the old revision, rather than disappearing from coverage.
bash "$root/scripts/demos/version-impact.sh" --root "$reference" --build "$reference/.cache/build/profile" --output "$reference_output" --label "$baseline_ref" --workloads "$root/scripts/demos/version-impact-workloads.tsv" --allow-added-missing
bash "$root/scripts/demos/version-impact.sh" --root "$root" --build "$candidate_build" --output "$candidate_output" --label "$(git -C "$root" branch --show-current)"
"$candidate_build/tools/benchrunner" --demo-compare "$reference_output" "$candidate_output" --demo-document "$document"
