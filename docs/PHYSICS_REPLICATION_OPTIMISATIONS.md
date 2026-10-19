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
| 1 | Physics | PLANNED. No candidate, timing or decision recorded. |
| 2 | Replication | PLANNED. No candidate, timing or decision recorded. |
| 3 | Physics | PLANNED. No candidate, timing or decision recorded. |
| 4 | Replication | PLANNED. No candidate, timing or decision recorded. |
| 5 | Physics | PLANNED. No candidate, timing or decision recorded. |
| 6 | Replication | PLANNED. No candidate, timing or decision recorded. |
| 7 | Physics | PLANNED. No candidate, timing or decision recorded. |
| 8 | Replication | PLANNED. No candidate, timing or decision recorded. |
| 9 | Physics | PLANNED. No candidate, timing or decision recorded. |
| 10 | Replication | PLANNED. No candidate, timing or decision recorded. |

Completed rounds: **0 of 10**.

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
