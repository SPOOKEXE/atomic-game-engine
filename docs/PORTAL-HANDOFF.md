# Portal handoff

Updated 2026-09-07. User requested a commit and handoff only; no further code work.

- Objective remains product endpoint discovery and continuous, seamless player crossing.
- Concise status: [ROADMAP](../ROADMAP.md). Detailed design and evidence: [render plan](RENDER-REFACTOR.md) and [task list](RENDER-REFACTOR-TASKS.md).
- The Terrain BVH change, benchmark job and initial handoff were committed as `cbe7a6ed`. The user subsequently explicitly requested committing all remaining working-tree changes together, including earlier changes of uncertain ownership.

## Verified work

- Terrain BVH construction caches centroids, partitions medians and preserves canonical leaf order. Release-derived bench: 8,192 triangles, 984 to 858 microseconds. Full Terrain worker cost remains unmeasured.
- Topology recovery accepts an authenticated current renewal after cache expiry, while retaining request deadlines. Ready topology replies are consumed before camera route selection.
- Product camera matrix passes all 16 variants: 30/60 Hz, first/third person, explicit/automatic Humanoid subject, held/released movement. `topology-ready-camera-matrix.log`: 136,117 assertions.
- Opt-in render-stage probe copies images before later stages overwrite them. Raw pixels, BMPs, metadata and an HTML index are saved. `render-stage-probe-final-gpu.log`: 756 assertions in two cases.
- Probe and topology changes are included with their wider renderer/client dependencies in the full checkpoint. No fresh full-suite validation was run for that checkpoint.

## Remaining, in dependency order

- Finish authorized retained-world content and observer lifetime, including gameplay lease retirement after destination adoption. Current source departures renew while the old connection remains alive.
- Prepare and render complete foreign-world views using the current camera. Switching geometry alone mixes lighting, particles and UI from different worlds.
- Fix parallax/disocclusion: `[eye-current-camera]` still fails three moving-camera cases with flat retained images.
- Diagnose the original valid-image-handle black frame. The probe separately captured a missing-image black frame during a topology wait; these are not proven to have the same cause.
- Verify continuous bidirectional player crossing under delay, restart, refusal and lost acknowledgements, with Humanoid cameras and animated bodies.
- Complete visual checks for lighting, transparency, effects, UI, clipping and all portal angles, plus the non-Euclidean demo.
- Measure release CPU/GPU time, residency, cache behavior, transfer bytes and full Terrain collision workers.

## Resume safely

- Retained-source staging prototype is rolled back. Saved patch: `.cache/build/dev/tests/portal-retained-staging-evidence/prototype.patch`. Do not treat it as a working feature.
- Latest recorded client/test_client/test_render builds passed. No build or test was started during this handoff.
- Diagnostic logs and images live under `.cache/build/dev/tests/`. Keep `stage-probe-check/`, `portal-stage-evidence/missing-eye/` and `topology-ready-camera-evidence/`; bulk captures were cleaned.
- Enable snapshots with `ATOMIC_RENDER_PROBE_DIR`; bound them with `ATOMIC_RENDER_PROBE_FIRST`, `ATOMIC_RENDER_PROBE_LAST` and optionally `ATOMIC_RENDER_PROBE_VIEW`. Captures wait for GPU completion and can change timing or cause expiries. They do not establish timing-neutral reproduction.
- Run one build and one GPU test at a time; never relink a running test. Consult RUNNING.md and module AGENTS.md before implementation.
- No release publication, merge or history squash was performed as part of this handoff. Full seamless crossing remains unfinished.
