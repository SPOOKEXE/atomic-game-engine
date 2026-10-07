# Capture the fixed demo set in three independent five-second processes each.
demo-bench label="current":
    cmake --preset profile -DMONO_BUILD_BENCH=ON
    cmake --build --preset profile --target client server benchrunner
    bash scripts/demos/version-impact.sh --label "{{label}}" --output ".cache/build/profile/version-impact/$(date -u +%Y%m%dT%H%M%SZ)-$$"

# Build and recollect both revisions on the same machine, then enforce ceilings.
demo-impact baseline="v0.24.1" document="docs/VERSION_IMPACT.md":
    bash scripts/demos/compare-version-impact.sh "{{baseline}}" "{{document}}"

# Compare an existing collection to the exact baseline embedded in the document.
demo-impact-report current baseline="docs/VERSION_IMPACT.md" document="docs/VERSION_IMPACT.md":
    .cache/build/profile/tools/benchrunner --demo-compare "{{baseline}}" "{{current}}" --demo-document "{{document}}"

# Keep parser and aggregation tests in the ordinary headless check process.
demo-impact-check:
    cmake --build --preset {{preset}} --target test_core test_benchrunner
    ./{{build}}/tests/test_core '[benchmarkreport]'
    ./{{build}}/tests/test_benchrunner '[benchrunner]'

# Full correctness checks followed by measured performance regression checks.
regression-check baseline="v0.24.1": check (demo-impact baseline)
