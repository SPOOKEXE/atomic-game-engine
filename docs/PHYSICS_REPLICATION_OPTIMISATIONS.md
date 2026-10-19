# Physics and replication optimisation campaign

Starting point: `bb50866547f0d0c1189f177e6da07e5e1a506f39`.

This ledger tracks ten distinct, measured experiments: five physics rounds and
five replication rounds. A round is an executed experiment, not a plan, code
change or commit. Each round records a profile, a hypothesis, a candidate, a
measurement, exact parity evidence and a keep-or-reject decision. No round is
complete until all six are recorded. A faster result is not assumed, and a
candidate may be rejected or show no gain.

## Baseline

The frozen `bench-o0` baseline build is complete. The engine libraries use
effective `-O0`; benchmark driver translation units use effective `-O3`.
Before and after measurements must use matched settings. The profile below is
observed on a GCC dev `-O0` build with a 4,000-body pile, three workers and
two ticks; canonical capture is included in the full-frame timings. It is
separate from the mixed-driver `bench-o0` build and does not count as a
completed round.

Source: `.cache/build/dev/cell-size-audit/current-profile 3`, stdout only.
Command: `.cache/build/dev/cell-size-audit/current-profile 3`.

| Profile measurement | t0 | t1 |
|---|---:|---:|
| Pair count | 1,360,179 | 944,902 |
| Full frames, including canonical capture (ms) | 9,793 | 7,034 |
| Sweeps | 5,630.649 | 3,927.677 |
| Setup | 343.170 | 611.511 |
| Pair order | 226.430 | 149.860 |
| Owners | 172.068 | 107.231 |
| Compact | 109.323 | 34.259 |

These are observed profile values, not a controlled before/after benchmark.
Record the frozen build's runtime settings, workload counts and canonical
outputs with campaign measurements.

## Campaign constraints

- Keep existing entity, tick, input, fault and assertion coverage at `-O0`.
- Preserve wire output and deterministic output exactly for every candidate.
- Use existing benchmark jobs where possible. Any new permanent benchmark
  requires a `just` job; keep benchmark output under build directories.
- Do not write raw benchmark files.
- Final evidence requires full CI, real runs with 20,000 movers, both complete
  physics cases (920 assertions), and canonical outputs.

## Rounds

Record the profile and hypothesis, candidate, workload and build settings,
before/after measurements, exact parity evidence, and decision in each row.
Include the command or existing job used so the result can be reproduced.

| Round | Area | Profile, hypothesis, candidate, measurement, exact parity, decision |
|---:|---|---|
| 1 | Physics | **KEEP**, source committed as `3a1e0265`. Candidate: fuse the six digit counts for each A/B member, skip constant digits, and stable-sort all 64 bits by B then A while carrying attached sources. Workload: real 4,000-body pile, `-O0`, three workers, two ticks. The profile command runs `Python subprocess.check_output([path, '3'])` sequentially on `round1-before` and `current-profile`, printing `physics.pair-order`. Three old t0/t1 samples: 252.074/142.128, 269.852/149.537, 230.410/142.846 ms; candidate: 106.578/61.293, 101.662/59.309, 113.319/60.794 ms. Medians: 252.074/142.846 to 106.578/60.794 ms, reductions of 57.7%/57.4%. For one million full-width xorshift pair IDs at `-O0`, `.cache/build/dev/cell-size-audit/pair-sort-tradeoff`, alternating old/current helpers with matching target flags: old 200.878/198.463/285.180 ms, candidate 160.734/157.775/129.815 ms; median 200.878 to 157.775 ms, 21.5% lower. Canonical parity used `test_physics '[solver-setup-parity]' --rng-seed 1 --order decl` with `ATOMIC_SOLVER_PARITY_DUMP=1`; compare the 460 `# physics-parity` lines in memory. SHA-256 (joined with newlines, no trailing newline): `2b35f39b3ed4b3c2c502fa3dbc62841a0967ff8e58daf1c7b00f6d47c8a0248b`. Both cases: 20,247 assertions each build. Final targeted command `.cache/build/dev/tests/test_physics '[pair-sort],[broadphase]' --rng-seed 1 --order decl`: 26,355 assertions across 24 cases. Oracle covers equal-A distinct full-width B IDs including upper generation bits, duplicate full pairs with distinct sources, and a real singleton. Production is unchanged from the canonical and timed build. |
| 2 | Replication | **KEEP**, source commit `3fd735ab`; source review passed exit 0 with 2,613 files scanned and no open findings in ecs-copy/world-pointer/name-id/public-header checks. Candidate: bulk `WriteRaw`/`ReadRaw` for entity arrays only on native little-endian systems, guarded by trivial/standard-layout `Entity`, 8-byte size and `Id` at offset zero; scalar fallback and bounds remain. Build: `cmake --build --preset bench-o0 --target bench_replication -j6`, engine `-O0`, fixture TU `-O3`. Test `.cache/build/dev/tests/test_replication '[replication][protocol]'`: 175,141 assertions, 4 cases. Scalar-reference wire tests cover 0/1/257/4096 varied full-width handles, byte truncation with sentinel preservation, and over-limit refusal before read. Benchmark compares frozen `ten-round-baseline/bench_replication` SHA-256 `554a1b3078d4415c184211d1c7a48a357eeb34427562ff3890f5008aebc76bd9` against `bench/bench_replication`, protocol suite, exact benchmark, 3 samples plus 1 warmup, in three fresh-process blocks ordered old/new, new/old, old/new. `WriteMessage delta`, 10k entities/1 component: old 115/111/108 ns per entity, candidate 0/0/0; spread 20/0/1 vs 0/0/0. `ReadMessage delta`: old 20/14/19, candidate 5/5/5; spread 7/6/7 vs 6/6/6. `WriteMessage structure`, 1k created entities: old 116/119/153, candidate 0/0/0; spread 8/4/3 vs 0/0/0. Report label is `call` with row-count divisor, so values are interpreted per entity. Zero is below 1 ns/entity, not zero work: whole-batch write is below 10 μs for 10k rows and below 1 μs for 1k rows. Scalar-reference byte parity is direct evidence for this changed codec helper, which is used by Delta, Structure and Audit. |
| 3 | Physics | PLANNED. No candidate, timing or decision recorded. |
| 4 | Replication | PLANNED. No candidate, timing or decision recorded. |
| 5 | Physics | PLANNED. No candidate, timing or decision recorded. |
| 6 | Replication | PLANNED. No candidate, timing or decision recorded. |
| 7 | Physics | PLANNED. No candidate, timing or decision recorded. |
| 8 | Replication | PLANNED. No candidate, timing or decision recorded. |
| 9 | Physics | PLANNED. No candidate, timing or decision recorded. |
| 10 | Replication | PLANNED. No candidate, timing or decision recorded. |

Completed rounds: **2 of 10**.

## Campaign checklist

Track each phase once for the campaign. Do not mark a phase complete because
its work is planned or because a related change exists already.

- [ ] Classify the target paths and workloads.
- [ ] Discover existing profiles, benchmark jobs and parity checks.
- [ ] Specify baseline conditions and acceptance evidence.
- [ ] Localize measured cost in the target paths.
- [ ] Prototype candidates as individual experiments.
- [ ] Check for required stubs and remove any that are no longer needed.
- [ ] Implement only candidates accepted by measured evidence.
- [ ] Validate `-O0` coverage, wire output and deterministic output.
- [ ] Optimise only against recorded measurements; retain rejects as decisions.
- [ ] Review the complete diff and run full CI, 20,000-mover runs, the full
  physics cases (920 assertions) and canonical-output checks.
