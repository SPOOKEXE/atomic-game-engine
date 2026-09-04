# render refactor execution

Implementation of [RENDER-REFACTOR.md](RENDER-REFACTOR.md), all R01-R17 and P0-P12.
The implementation request authorizes the planned destination. A checked phase
requires its functional and optimization evidence, not merely code or a build.

## phase tasks

- [ ] P0: audit current contracts, policies, hidden work and shader consumers;
  record baseline tests and release costs. In progress.
- [ ] P1: per-step image/projection harness, independent oracles, bounded
  captures, failure artifacts and `just render-check`.
- [ ] P2: graph-owned preparation, residency, uploads, simulation, capture and
  composition; shared worlds and parallel active-view collection.
- [ ] P3: shader runtime schema, cook/compiler split, reflection, variants,
  published bundles, backend payloads and complete cache keys.
- [ ] P4: material definitions/instances, capability flags, effect attachments,
  resident records, VM bindings, save and replication.
- [ ] P5: compositor/material authoring, incremental canvas, cook previews,
  groups, undo/redo and hidden-work gates.
- [ ] P6: lighting, PBR, colour, AO/GI, shadows, post, velocity/history and all
  requested AA choices with numeric and image proof.
- [ ] P7: editable packing/quantization, mip/atlas streaming, four authored LODs,
  decimation, meshlets, tessellation and visual displacement.
- [ ] P8: portal projection, clipping, light transport, physical overlap,
  cross-world ownership and inspected moving demo.
- [ ] P9: real hybrid/progressive tracing, acceleration, transport, accumulation,
  reset, denoise, portal rays and bounded GPU queues.
- [ ] P10: particle/environment nodes, TornadoSim and retained scene-producer
  requirements with deterministic authoritative simulation.
- [ ] P11: all requested examples, backend/tier verification, full-frame
  benchmarks, pressure tests and latency/quality comparisons.
- [ ] P12: delete replaced paths, enforce cooked packaging, finish build/test/
  fuzz/soak gates, reconcile documentation and audit R01-R17.

## workflow for the current phase

- [x] classify: engine-wide implementation, preserving the full plan.
- [ ] discover-core: inspect graph execution, device capture and shader loading.
- [ ] specify: map baseline and image contracts to concrete tests.
- [ ] localize: name production seams and test commands before editing.
- [ ] prototype: only unresolved device/format behavior needs a probe.
- [ ] stubs: review data shapes and function logic before filling bodies.
- [ ] implement: build in dependency order without placeholder success.
- [ ] validate: headless first, then device evidence at final verification.
- [ ] optimize: release measurements with quality and memory held explicit.
- [ ] review: reconcile code, tests, policies and residual gates.

## evidence and current boundaries

- Starting revision: `4daba0b6`; clean worktree before implementation.
- The source plan contains 23 sections, 17 requirements and 13 phases.
- `render/AGENTS.md` still requires live client shader compilation and exempts
  host chrome/window clear from graph ownership. P0 must reconcile these with
  the explicitly requested implementation and verified current code.
- Device/Studio verification has not run for this implementation. Repository
  policy requires approval at final verification; ordinary headless tests can
  proceed now.
- No phase is complete yet. No renderer speedup is claimed.
