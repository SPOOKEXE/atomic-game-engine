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

### Frozen full-scene baseline

Frozen executable SHA-256: `49b1cb12470c7c5a1a2e7f118b9ac581afd151422542eb15fb43c3e15240835d`.
Command: `.cache/build/bench-o0/ten-round-baseline/bench_studio --suite studio.bench.replicationstress --samples 3 --warmup-samples 1`.
Two process runs without concurrent campaign builds or tests reported
150,800,639 and 148,162,277 ns/tick.
Each report is the minimum of three samples, each running eight ticks. The
engine uses `-O0`, the fixture translation unit uses `-O3`, and the workload
has 20,000 movers, 20,009 server poses and 20,010 client poses. Runtime logs
showed 23 workers and 12 physical cores pinned. The fixture calls `Start(1)`,
but the worker pool was already active, so that call did not reduce the pool.
Other user CPU workloads were active on the machine, so background load was
uncontrolled and unattributed throughout the campaign. Alternating paired
measurements reduce this noise but do not remove it. An exploratory
166,784,897 ns/tick run is excluded because compilation and source checking
were active concurrently. A paired before/after result is still required;
these baseline samples support no overall speed claim.

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
| 3 | Physics | **KEEP**, source commit `24c201af`, for the measured setup-phase gain only. Candidate: replace repeated whole-cache binary searches during manifold setup with one `lower_bound` per fixed worker range, advance a cursor through sorted body-pair runs, and restrict feature searches to each pair span while preserving speculative speed-max order. Command: `python3 .cache/build/dev/cell-size-audit/RunPhysicsRound.py 3 .cache/build/dev/cell-size-audit/round3-before .cache/build/dev/cell-size-audit/current-profile physics.solve-setup physics.solve`; three alternating samples, fixed 4,000-body pile, `-O0`, three workers, four million pairs, two ticks. Setup t0/t1 ms, before: 400.570/523.420, 424.404/527.870, 432.832/629.236; after: 324.950/359.270, 375.754/322.224, 418.128/352.179. Medians: 424.404/527.870 to 375.754/352.179 ms, 11.5%/33.3% lower. Total solve t0/t1 ms, before: 6,977.725/4,802.021, 7,194.268/4,878.341, 7,172.204/4,967.519; after: 7,133.738/4,817.710, 7,175.025/4,726.930, 7,348.112/4,740.523. Medians: 7,172.204/4,878.341 to 7,175.025/4,740.523 ms: cold flat, warm 2.8% lower and noisy. No broad speed claim. Focused `.cache/build/dev/tests/test_physics '[solver-cache-cursor],[solver-setup-parity]' --rng-seed 1 --order decl`: 20,490 assertions, 3 cases, including miss-then-hit. Canonical check `python3 .cache/build/dev/cell-size-audit/CheckPhysicsParity.py .cache/build/dev/cell-size-audit/round3-test-before .cache/build/dev/tests/test_physics`: 460 lines exactly match Round 1 SHA-256 `2b35f39b3ed4b3c2c502fa3dbc62841a0967ff8e58daf1c7b00f6d47c8a0248b`; each build passed 20,247 assertions across 2 cases. |
| 4 | Replication | **KEEP**, source commit `0e55f2fa`. `SignSlot` compares sorted `Hashed` against `Hashes`, then swaps buffers, removing the third `Next` allocation/copy while preserving trace reads from old `Hashed`. Frozen before binary SHA-256 `bfe721ba69fda6b747096f130d735b822b096b6df73e06694e4eee0178d5d86f`; before library SHA-256 `ac8340a5cf43995b5b80767389b444afd272428a9d92d15f78ce596997292883`; before probe SHA-256 `060c8de54f58b470666b74dcdbd4c1322679f6f31ec5f57128942c0ba1013490`. `bench-o0` build for `bench_replication` and dev `test_replication`; full `[replication]~[.sandbox]~[.soak]`: 317 cases, 228,775 assertions passed. Wire parity: 54,305 bytes, SHA-256 `17a7298822da35a6d17aa0f65cada3bbc65c042618c72ae4b065067477116bc4`, matching in two fresh baseline processes. Probe covers 80 signed rows, archetype churn, 10 generation reuses with old entity absence, visibility churn, seven recovery caps, multipart 192 chunks/500 budget, six refusals, four losses, 32 held ACKs, and convergence by tick 204. Trace parity: all 443 `(entity,before,after)` tuples exact, including 353 nonzero-before rows. Three alternating benchmark blocks, 3 samples and 1 warmup, engine `-O0`/fixture `-O3`, exact command `--suite engine.replication.bench.survey --benchmark '<name>'`: `Publish · 10k entities · 4 signed slots · 16-byte value`, before 287/287/283 ns/entity (spread 11/17/43), after 230/235/228 (67/70/70). `64-byte` exact benchmark, before 311/414/297 (102/16/30), after 243/246/316 (69/73/36); retain the noisy final block regression in the record. Source and independent reviews approved; formatting and diff checks passed. |
| 5 | Physics | **REJECT** the compact solver-body-state candidate; no source commit. It passed 47 cases/78,591 assertions and exact 460-line canonical parity, but did not improve the measured whole-solve workload and added retained memory. Command: `python3 .cache/build/dev/cell-size-audit/RunPhysicsRound.py 5 .cache/build/dev/cell-size-audit/round5-before .cache/build/dev/cell-size-audit/current-profile physics.solve-sweeps physics.solve physics.solve-state-copy-in physics.solve-state-copy-out`. Before/after profiles used `-O0`; exact fixture/build artifact hashes are recorded below. Sweeps t0/t1 ms, before: 5315.331/3717.949, 5387.923/3712.152, 5378.320/3781.691; candidate: 5899.306/4022.888, 5346.536/3768.013, 5389.632/3909.604. Median t0/t1: 5378.320/3717.949 to 5389.632/3909.604 ms. Total solve before: 6993.337/4674.012, 7167.769/4665.276, 7089.657/4771.154; candidate: 7816.945/5073.252, 6987.044/4746.879, 7140.396/5019.417. Median: 7089.657/4674.012 to 7140.396/5019.417 ms, 0.7%/7.4% slower. State copy-in candidate: 0.103/0.071, 0.134/0.065, 0.111/0.079 ms; copy-out: 0.044/0.043, 0.033/0.034, 0.037/0.030 ms. Body size is 128 bytes, compact state 64 bytes, both aligned to 64; 4,001 states at capacity add 256,064 live and retained bytes. Size/memory command: `.cache/build/dev/cell-size-audit/body-state-sizes`. Frozen profile before/candidate SHA-256: `41d4c7256db94bbfc474ffad353a99a8c5f6aa6294967df30c9fffa539279c67` / `6f207f325cd202cebb65d940a88e9d0786a59619b78c116c5f35439794b9c207`. Frozen test before/candidate SHA-256: `4ae1684d91c182d3741122f0fae354112705d798f0487696c624f0fcd599d28c` / `f3b8c5dae22a4eb9f5f25c50076554a2d8885f5b0f608b5efc2d696f00a24528`. Focused `.cache/build/dev/tests/test_physics '[solver-body-state],[solver],[solvergroups],[solver-setup-parity]' --rng-seed 1 --order decl`: 78,591 assertions, 47 cases. Canonical command `python3 .cache/build/dev/cell-size-audit/CheckPhysicsParity.py .cache/build/dev/cell-size-audit/round5-test-before .cache/build/dev/tests/test_physics`: 460 lines exactly match SHA-256 `2b35f39b3ed4b3c2c502fa3dbc62841a0967ff8e58daf1c7b00f6d47c8a0248b`; each build passed 20,247 assertions across 2 cases. All five candidate source/test files were restored exactly; physics diff is empty. |
| 6 | Replication | **REJECT** the monotonic outstanding-row cursor; cleanup commit `8a919017` removed its method and offer cursor, restoring `Authority` to the accepted Round 4 state. The only retained benchmark change is the sparse-control fixture with a corrected header. Candidate used per-component sorted row cursors initialized to `SIZE_MAX`, `lower_bound` on first access or regression, and linear advance otherwise, with no layout changes. Engine `-O0`/fixture `-O3`, same compiler flags. Before benchmark SHA-256 `45bb0b0c2dfe16ccd6f130792ad5f87de3cf26d28e3977ab17bb28db09dc5d25`; old bench library `3658774c5e620cc54ec42c9f3ad7b788008a091bed5de174e426b4259ab31116`; relinked sparse-control binary `ten-round-before6-controls` SHA-256 `f01f9e2483cd1afbef61e7f9ab05076d05bd75e38f1a2ca85e84cade25b51cb1`. Three alternating old/new, new/old, old/new blocks. Recovery rows, 512 entities/128 cap, ns/row: before 2265/2299/2268 (spread 295/39/39), after 2153/2232/2254 (24/28/135); median 2268 to 2232, 1.6% lower. Sparse control, 20k backlog/two distant changes, ns/call: before 5,713,888/5,030,917/4,787,560 (spread 550,495/1,207,527/1,508,653), after 4,871,757/4,942,811/5,043,931 (42,040/343,966/1,440,936); median 5,030,917 to 4,942,811, 1.75% lower with high spread. Publish, 10k entities/1 client/serial, ns/client: before 2,824,704/2,844,110/2,917,528 (spread 104,587/15,279/852,191), after 2,992,699/2,825,125/2,960,869 (39,434/38,462/949,033); median 2,844,110 to 2,960,869, 4.1% slower. Each benchmark used 5 samples and 2 warmups; exact suites were `engine.replication.bench.recovery-rows` and `engine.replication.bench.publish` with `Publish · 10k entities · 1 client · serial`. Full `[replication]`: 317 cases, 228,234 assertions passed. Exact 54,305-byte wire output unchanged, SHA-256 `17a7298822da35a6d17aa0f65cada3bbc65c042618c72ae4b065067477116bc4`; exercised six refusals, four losses, 32 held ACKs, 10 destroyed/absent entities, 80 rows and convergence at tick 204. Reviews found no correctness issue; measured publish regression drove rejection. |
| 7 | Physics | PLANNED. No candidate, timing or decision recorded. |
| 8 | Replication | **KEEP**, source commit `8101810b`. Skip source-attribute owner scans only when the attribute entity table is empty; skip predicted-local owner scans only when both local entity and revision tables are empty. Preserve empty-resource wire output, clears and `NextRevision` maximum. Root and independent source review passed. Before benchmark SHA-256 `f01f9e2483cd1afbef61e7f9ab05076d05bd75e38f1a2ca85e84cade25b51cb1`; candidate benchmark SHA-256 `0c878b9d80e813708cee0958411f10723b052aa81779f090eeb309e4771c9deb`; candidate replication library SHA-256 `510da07ba4e99e1ae983803a28b8cdf7471569724246255a0f1e591e32eacf56`; candidate attributes probe SHA-256 `c529b4a9774e5c267e58199182ded012a6265051404631f894f0b4009b2d10bb`. Three alternating old/new, new/old, old/new blocks, command `--suite engine.replication.bench.attributes --samples 5 --warmup-samples 2`, engine `-O0`, fixtures `-O3`, timings are whole-call ns. Empty select, 20k entities: before 4,965,527/4,773,838/4,788,435 (spread 1,230,102/232,867/128,571), after 190/130/130 (90/30/80); median 4,788,435 to 130 ns/call. Sparse four-owner select control, 20k: before 5,126,380/5,096,935/5,179,810 (1,619,342/120,696/68,449), after 5,075,304/4,982,550/5,184,869 (221,286/80,771/148,751); no demonstrated change beyond noise. Empty component write, 20k: before 1,294,321/1,308,448/1,312,626 (29,276/241,544/50,073), after 741/692/701 (170/120/181); median 1,308,448 to 701 ns/call. These are empty-table fast paths only and make no total-scene performance claim. Permanent benchmark job: `just replication-attributes-bench 5 bench-o0`. Build commands: `cmake --build --preset bench-o0 --target bench_replication -j8` and `cmake --build --preset dev --target test_replication test_ecs -j8`. New test `.cache/build/dev/tests/test_replication '[replication][attributes]'`: 40 assertions, 2 cases. Full replication suite: 228,274 assertions, 319 cases. ECS attribute suite: 708 assertions, 15 cases. Attribute-enabled wire parity: with predicted local attributes, 77,198 bytes, SHA-256 `7078e39a6a2a02c8b3cdfff6b02ddfe7c5a983ae3c6757170860538be46ba5a8`; without them, 77,146 bytes, SHA-256 `f80cb93b0753bdb65f82d0070e58dd6bc17b6e94d831f2bf6d1be556004001dc`. Baseline output matched in two fresh processes before exact comparison to candidate. Both probes: 80 rows, convergence tick 204, six refusals, four losses, 32 held ACKs, 10 destroyed/absent entities; source attributes set 3/27 and cleared 14/35; predicted-local value 91 retained. |
| 9 | Physics | PLANNED. No candidate, timing or decision recorded. |
| 10 | Replication | PLANNED. No candidate, timing or decision recorded. |

Completed rounds: **7 of 10**.

## Campaign checklist

Track each phase once for the campaign. Do not mark a phase complete because
its work is planned or because a related change exists already.

- [x] Classify the target paths and workloads.
- [x] Discover existing profiles, benchmark jobs and parity checks.
- [x] Specify baseline conditions and acceptance evidence.
- [x] Localize measured cost in the target paths.
- [ ] Prototype candidates as individual experiments.
- [ ] Check for required stubs and remove any that are no longer needed.
- [ ] Implement only candidates accepted by measured evidence.
- [ ] Validate `-O0` coverage, wire output and deterministic output.
- [ ] Optimise only against recorded measurements; retain rejects as decisions.
- [ ] Review the complete diff and run full CI, 20,000-mover runs, the full
  physics cases (920 assertions) and canonical-output checks.
