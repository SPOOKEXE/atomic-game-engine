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
- Four offscreen Vulkan image fixtures now pass. The user explicitly requested
  portal visual checks, which are being added to that harness. Interactive Studio
  inspection remains a separate final verification step.
- No phase is complete yet. No renderer speedup is claimed.

## current implementation slices

- [x] P0: locate hidden frame work. `Renderer::Render` flushes meshes before
  graph execution; `ViewRecording::Begin` executes entity selection and packing;
  upload handlers report that completed work. Particle preparation, interface
  preparation, thumbnail capture and final capture/chrome remain migration work.
- [x] P0: add bounded signature benchmarks for 1,024/16,384 rows, 1/2/8/32
  cameras and unchanged/one-row edits. `bench_render` builds; all 16 cases run.
  These diagnostic runs had background work, heap hooks and Tracy enabled.
- [x] P1: independent CPU comparison of byte colour, float and integer-ID images
  with stride validation, per-pixel outliers, RMSE, non-finite rejection and
  mismatch bounds. `[imagecomparison]` passes 112 assertions in 8 cases.
- [x] P1 slice: real-device plane fixtures and failure artifacts. Four Vulkan
  cases pass, including lens/clipping, edited objects and multiple worlds/views.
  A final graph observer retains captured intermediate resources; this preserves
  ordinary aliasing elsewhere. Orthographic/oblique, skinning, PBR, temporal,
  portal and broader lifetime families remain required in their feature phases.
- [x] P2 slice: conservative `View` damage includes object inputs, rather than
  reusing stale resident rows after same-count edits. The GPU edit case reproduced
  this defect and passes after the fix; hosts can still supply precise damage.
- [x] P2 slice: frame setup runs once per resolved pipeline, with shared resources
  available to each world/view. Duplicate prefix reproduced in headless tests.
  Planner, mixed-writer, fallback aliases and idle-view cases pass headlessly.
  This closes scope bookkeeping only; actual hidden-work migration remains.
- [x] P3 slice: retain accepted shader words on failed edits, invalidate removed
  consumers, and key attempted sources by storage incarnation/entity/revision.
  Replacement, duplicate selection, cross-world contamination and snapshot tests
  pass. Snapshot application conservatively invalidates source caches, so unchanged
  shaders may recompile until the content-key cook cache exists.
- [x] P3 slice: bounded canonical cooked shader transport, reflected interface
  identity, corruption tests and sanitizer-backed parser fuzzing. Cooking,
  executable admission and delivery consumers remain required; this is a format
  unit, not a published shader pipeline.

## portal acceptance tasks requested by the user

- [ ] Compare direct and portal views with destination lighting, moving lights,
  shadows, exposure and camera changes, using aperture masks and numeric probes.
- [ ] Verify visual object crossing and real player characters whose
  `CameraSubject` is a `Humanoid`, in first-person and third-person views.
  Baseline has no public `CameraSubject` property and directly follows root parts.
  Per-camera subject storage, one root resolver and consumer migration are underway.
- [ ] Deliver the other world's images through bus messages and bounded jobs;
  test ownership, stale/out-of-order replies, unload/reload and process isolation.
  Baseline `AttachForeignSurfaces` copies draw rows from local stores; it does not
  demonstrate remote world image exchange.
- [ ] Walk both directions through cross-world portals with one authoritative
  body and continuous subject, camera, movement, animation and collision.
  Baseline `ImmersivePortals` uses a scripted proximity teleport and respawn;
  generic scene crossing and physics contact proxies skip cross-world seams.
- [ ] Reproduce and fix the existing non-Euclidean portal demo using the common
  engine path, then inspect deterministic and interactive crossing sequences.
- [ ] Sweep every side/azimuth/elevation, grazing and off-centre views, near-plane
  crossings and look-back. The aperture must show the correct other side without
  holes, inversion, stale pixels or a detached rectangular picture.

## validation record

| Check | Result |
|---|---|
| Original frame prefix over two worlds | Reproduced duplicate setup, test exit 42 |
| Graph correction after scope review | 8,838 assertions / 223 cases passed |
| Render correction, excluding GPU | 28,001 assertions / 370 cases passed |
| ECS storage identity and existing suites | 132,762 assertions / 438 cases passed |
| Scene shader identity and existing suites | 498,946 assertions / 506 cases passed |
| Client and Studio after shader consumer fix | 9,212 / 163 cases and 5,929 / 515 cases passed; camera migration not included yet |
| Cooked shader asset format | 6,992 assertions / 10 cases; all assets 30,380 / 230 cases passed |
| Cooked shader parser fuzzing | Clang 21, ASan/UBSan, 10,000 runs passed; routine maximum input 64 KiB |
| Image comparison, normal render binary | 112 assertions / 8 cases passed |
| Signature benchmark build and run | 16 cases passed; no baseline accepted |
| First GPU fixture run | 16 failed assertions: aliased capture lifetime and stale default object damage reproduced |
| Corrected GPU image fixtures | Vulkan, 380 assertions / 4 cases passed, unchanged tolerances |
| Full phase/build/backend acceptance | Incomplete; individual checks above do not establish it |
