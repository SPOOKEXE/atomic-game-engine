# Pixel Composer M0-M7 implementation progress

Snapshot date: 2026-10-03. This is a requirements and evidence inventory, not a completion claim. The implementation plan is [v026-pixel-composer.md](../to-delete/v026-pixel-composer.md). Status reflects files and recorded evidence visible at this snapshot. Native code or tests alone do not establish official executable parity.

## Ten-step work checklist

1. **Classify scope:** done for this inventory. M0-M7 requirements and release gates are enumerated below; implementation is not complete.
2. **Discover existing evidence:** partial. CodeGraph was queried before source discovery; the plan, matrices, source manifests, reference gate, validation ledger, and test/scene paths were inspected. This is not a complete per-node source audit.
3. **Specify acceptance:** partial. Node and workflow matrices exist. Node matrix has 995 rows: 2 `documented and implemented`, 979 `documented and blocked by named prerequisite`, and 14 `unknown`. All 20 workflow rows are `blocked`. See [node matrix](node-parity-matrix.csv) and [workflow matrix](product-workflow-matrix.csv).
4. **Localize architecture and invariants:** partial. `mono.engine/imagegraph` and its module guidance exist. The full dependency, layer, output-owner, and sink acceptance gates still need current joined evidence. See [imagegraph guidance](../../mono.engine/imagegraph/AGENTS.md), [architecture graph](../../mono.tools/architecture/expected_graph.json), and the architecture requirements below.
5. **Prototype the native core:** partial. Typed graph, evaluator, image operations, arrays, timeline, audio, mesh, and bounded resource code and focused suites exist. Presence is not complete node coverage or parity. See `../../mono.engine/imagegraph/` and the focused suite links below.
6. **Build adapters and stubs:** partial. Studio, headless runner, client, and render adapter slices exist. Required editor workflows, live graph scheduling, sinks, and product acceptance remain open. See `../../mono.studio/src/ImageComposer.cpp`, `../../mono.tools/imagegraph/`, `../../mono.client/src/ImageGraphRuntime.cpp`, and `../../mono.engine/render/`.
7. **Implement documented families:** incomplete. M2-M6 requirements remain open as itemized below. Matrix status cannot be promoted from source presence alone.
8. **Validate required suites and scenes:** incomplete. Build47 imagegraph core had 1,171 cases pass and 4 fail, with 5 failed assertions across `Source2DGridNodes.cpp` (mixed producer), `FlipDestroy.cpp` (direct budget), `OutputDiagnostics.cpp` (two PXC assertions), and `SourceRouting.cpp` (fresh-seek cache, one assertion). Build48 core passed all 1,175 cases and 1,808,028 assertions. Build49 then passed dev and release core suites, each with 1,187 cases and 1,808,130 assertions. Build49 I/O passed 162 cases/5,926 assertions; export passed 36/732; focused Studio passed 39/894; the Composer workflow filter passed 2/122; client CPU adapters passed 16/1,478; and adapter targets linked in 208 steps. These are bounded native gates. All seven named scene files exist, but manual and GPU acceptance remain pending. See the updated October 2 ledger, whose latest gate dates are October 3 in Darwin.
9. **Optimize and profile:** not evidenced. Release profiles for static 2D, animation, feedback, simulation, and 3D are still required.
10. **Review release gates:** incomplete. Native acceptance, matrix coverage, scenes, and PXC validation remain open. Licensed executable comparison is unavailable by the user's stated choice and remains a distinct open gate.

## Evidence baseline and parity distinction

The [reference gate](reference-gate.md), [source manifests](source-manifest-2026-09-30.json), [fixture manifest](supplied-fixture-manifest.json), [current node evidence](current-node-evidence-2026-09-30.json), [source pin](source-code-pin-2026-09-30.json), [September 30 native ledger](native-validation-2026-09-30.json), and [October 2 native ledger](native-validation-2026-10-02.json) are the evidence set. The retrieved documentation inventory records 3,014 URLs, 2,244 successful responses and 770 HTTP 404 responses. The current official candidate is Pixel Composer 1.22.0.0 stable, released September 28, 2026. The documentation does not expose a single site-wide product version. The official executable requires purchase; the user confirmed it is unavailable. No executable hash, runtime check, or matched capture exists.

The node matrix snapshot is larger than the 990-row historical inventory recorded in the September 30 reference note. The current matrix contains 995 rows. The reference note also records 5 missing Armature Build pages and three 1.22 release-note nodes absent from retrieved navigation or the older catalogue: Array Cumulative, Path Redistribute, and Channel Swizzle. Therefore neither the historical 990 nor current 995 rows alone prove a complete 1.22 catalogue. Reconcile the matrix to current documentation and release notes; keep unavailable behavior unresolved.

Native fixtures establish the behavior they actually exercise, and native repeatability only. Source-derived expectations must retain their source provenance. The supplied five PXC projects and five PNG/GIF captures are fixture-specific evidence, not a public format specification or exact node-output oracle. Exact executable parity and undocumented behavior remain unknown until a licensed build and controlled same-input captures are available.

## Milestone requirements

Each item retains the requirement's full acceptance scope. Status is `partial` only where existing artifacts demonstrate a bounded slice. A test file or implementation file is evidence to inspect, not proof that the whole row passes.

### M0: inventory and reference harness

| # | Requirement | Status and current evidence |
|---|---|---|
| 0.1 | Archive official docs with retrieval date and hashes; record 1.22 stable and comparison fallbacks; reconcile every current docs row; build node and product-workflow matrices with fixtures. | **Incomplete.** [Dated source manifest](source-manifest-2026-09-30.json), [node matrix](node-parity-matrix.csv), [workflow matrix](product-workflow-matrix.csv), and [node evidence](current-node-evidence-2026-09-30.json) exist. Current counts are 995 node rows and 20 workflow rows. The reference gate identifies missing pages and the 1.22 release-note catalogue gap. Not every matrix row has a passing fixture or current-build reconciliation. |
| 0.2 | Characterize supplied GIF/PNG captures; keep capture runner ready; record licensed executable gate unavailable; retain exact-parity and undocumented-behavior limits. | **Partial.** [Visual notes](visual-reference-notes.md), [capture runner](capture-reference.sh), and [reference gate](reference-gate.md) exist. No licensed executable or controlled executable capture exists. |
| 0.3 | Characterize all five supplied projects without claiming their observed binary layout is public. | **Partial.** [Fixture manifest](supplied-fixture-manifest.json) and reference gate record the five samples and their observed structure. The PXC gate below remains open for general format claims and application compatibility. |
| 0.4 | Record graph layouts, parameters, ticks, images, and diagnostics for every reference fixture. | **Incomplete.** Supplied media hashes and frame facts are recorded in the fixture manifest. Complete paired native/reference fixture records for every project and capture are not evidenced; executable side is unavailable. |
| 0.5 | Gate later work on reviewed documentation coverage; preserve unknowns. | **Incomplete.** Current matrices retain blocked and unknown rows. The current documentation inventory and 1.22 catalogue reconciliation are not closed, and no row-level full coverage review is recorded. |

### M1: graph model, editor, and headless runner

| # | Requirement | Status and current evidence |
|---|---|---|
| 1.1 | Implement versioned documents, registry, typed schemas, links, groups, arrays, subgraphs, dynamic inputs, diagnostics, and migrations. | **Partial.** Core evidence includes `../../mono.engine/imagegraph/include/engine/imagegraph/Document.hpp`, `../../mono.engine/imagegraph/src/Document.cpp`, `../../mono.engine/imagegraph/tests/Document.cpp`, `../../mono.engine/imagegraph/tests/GroupBoundary.cpp`, `../../mono.engine/imagegraph/tests/GroupReplay.cpp`, and array suites. Full documented graph parity and M1 fixtures remain open. |
| 1.2 | Build Studio adapter with nodegraph canvas, typed sockets, inspector, search, preview, timeline, keyframes, undo, and diagnostic navigation. | **Partial.** `../../mono.studio/src/ImageComposer.cpp`, `../../mono.studio/tests/ImageGraph.cpp`, `../../mono.studio/tests/TimelineDopesheet.cpp`, and `../../mono.studio/tests/TimelineKeyEditor.cpp` cover slices. Workflow matrix remains 20/20 blocked; the full interaction set and live GUI inspection are unverified. The build49 Composer workflow filter passed 2 cases/122 assertions covering bounded group editing with Dopesheet, undo, PXC and Lua dependencies, plus atomic refusal and history retention for an ambiguous opaque-key drag. |
| 1.3 | Build bounded headless runner for parameters, ticks, output selection, hashes, diagnostics, and headless 3D via render adapter/reference backend. | **Partial.** Runner code/tests are in `../../mono.tools/imagegraph/`; 3D adapter evidence is in `../../mono.client/src/ImageGraphTransform3DAdapter.cpp` and `../../mono.engine/render/tests/ImageGraphTransform3DGpu.cpp`. Existing runner export and Transform Image 3D slices do not cover the whole runner or 3D graph contract. |
| 1.4 | Pass core graph/property/group/array/error fixtures and canvas/inspector/timeline workflow fixtures before large node families. | **Incomplete.** Focused suites and historical runs are recorded in the native ledger. The workflow matrix remains blocked and M1 joined acceptance is not established by this snapshot. Array routing remains incomplete across processor nodes and non-image elements. |

### M2: full 2D and data parity

| # | Requirement | Status and current evidence |
|---|---|---|
| 2.1 | Implement every documented UV, generate, draw, pixel builder, transform, compose, filter, and effect node. | **Incomplete.** Many operation sources and focused tests exist under `../../mono.engine/imagegraph/src/` and `../../mono.engine/imagegraph/tests/`. Bounded Grain and Contrast Blur kernels were included in build49. Matrix coverage remains mostly blocked; no complete family acceptance is evidenced. |
| 2.2 | Implement every scalar, vector, matrix, text, math, conversion, random, curve, and expression row. | **Incomplete.** Bounded value/data slices and suites exist, including `StructuredValues.cpp`, `MatrixNodes.cpp`, `TextOps.cpp`, `CurveNodes.cpp`, and `VectorNodes.cpp` under `../../mono.engine/imagegraph/tests/`. Full catalogue coverage is open. |
| 2.3 | Match defaults, edge sampling, alpha, colour space, arrays, and animated behavior node by node. | **Incomplete.** Per-node fixtures and matrix columns exist, but most rows are blocked or unknown. Native behavior and executable parity must stay distinct. |
| 2.4 | Cook stills, maps, and flipbooks into normal engine texture assets. | **Partial.** Existing texture and flipbook asset paths and `../../mono.tools/assetc/tests/Sequence.cpp` cover slices. M7 records variable-duration and long-sequence limits; all documented outputs and consumers are not accepted. |

### M3: timeline, animation, audio, and feedback

| # | Requirement | Status and current evidence |
|---|---|---|
| 3.1 | Implement keyframes, interpolation, playback, fractional-frame policy, and timeline behavior. | **Partial.** Timeline versions, signed time, easing, scalar sine driver, Studio edits, and tests exist in `../../mono.engine/imagegraph/tests/TimelineV4.cpp`, `TimelineV5.cpp`, `TimelineV6.cpp`, `TimelineV8.cpp`, `TimelineSigned.cpp`, `TimelineDrivers.cpp`, and `../../mono.studio/tests/TimelineKeyEditor.cpp`. Fractional source-map semantics and several editor gestures remain unresolved. |
| 3.2 | Implement audio nodes with captured-frame contracts and deterministic recorded-fixture playback. | **Partial.** Bounded WAV import/export, planar recorded captures, multichannel Window/FFT/loudness, Bit/Second/Progress locations, signed seeks, and Studio playback controls have native implementations and focused suites. `TimelineAudioDriver.cpp` covers persisted native captured-audio keys with channel/metric/gain/bias and fractional seek replay; these are explicit engine extensions, not source audio key drivers. `NodeAudio.cpp`, `WavClip.cpp`, `WavExport.cpp`, `AudioCapture.cpp`, `AudioWindowPresentation.cpp`, Studio `WavPreview.cpp`/`AudioWindowPanel.cpp`, and imagegraphexport `GraphFileHost.cpp` cover native paths. Capture-device behavior, automatic File Watcher invalidation, exact source thumbnail/index coercion, and licensed executable comparison remain open. |
| 3.3 | Implement feedback and stateful image operations with explicit precision, warmup, fixed step, and reset. | **Incomplete.** No full feedback history acceptance is evidenced. Relevant bounded state and reset work must not be treated as completed feedback parity. |
| 3.4 | Match fixtures across start/end, wrap, seek, reset, and dropped-update cases. | **Incomplete.** Timeline fixtures cover selected cases, but the full node/state fixture set and licensed comparison do not exist. |

### M4: VFX, simulation, and live engine bindings

| # | Requirement | Status and current evidence |
|---|---|---|
| 4.1 | Implement every documented VFX, rigid, and fluid simulation row after proving state/resource contracts. | **Partial.** Native CPU simulation and typed source-payload slices exist in `../../mono.engine/imagegraph/src/nodes/SimulationNodes.cpp`, `SimulationReplay.cpp`, `FluidPayload.hpp`, `SdfPayload.cpp`, and `ParticlePayload.hpp`, with focused replay and payload tests. The full documented VFX, rigid, and fluid node families, device-resource contracts, reset/warmup cases, and long-run drift acceptance remain open. Game effects remain a separate system. |
| 4.2 | Feed generated texture names into existing effects systems and future effect graph work. | **Partial.** Client owner-scoped publication, scene/game reflected selectors, and particle/beam/trail consumer seams exist. Focused reflected-binding suites passed 6 cases/79 assertions in scene and 1 case/12 assertions in game; required scene-level and GPU acceptance remains open. |
| 4.3 | Add owner-scoped live texture registry, last-good fallback, and all 2D consumer bindings, including particles, beams/trails, and GUI images. | **Partial.** Client runtime/publisher, one-evaluation live source output routing, and particle/ribbon/UI binding slices exist in `../../mono.client/src/ImageGraphRuntime.cpp`, `../../mono.client/tests/ParticleFlipbooksRender.cpp`, `ImageGraphRibbonBindings.cpp`, and `ImageGraphConsumerRender.cpp`. Build47 CPU adapter tests passed 13 cases/405 assertions, with separate ordered live-array tests passing 2/10. Reflected scene/game selector properties also have focused native tests. The full sink matrix, last-good behavior for every consumer, and GPU/scene acceptance remain open. Material, shader, and skybox routes still need named acceptance; adapter routes may exist outside the graph-executor registry. |
| 4.4 | Match fixed seed, fixed tick, reset, and long-run drift fixtures. | **Partial.** Native random draws can be captured and replayed for the MK Sparkle slice, and a native `pc.mk_sparkle` executor is registered. The complete VFX/rigid/fluid fixed-tick, reset, drift fixtures and the separate licensed-reference comparison remain open. |

### M5: complete 3D parity

| # | Requirement | Status and current evidence |
|---|---|---|
| 5.1 | Implement documented scenes, cameras, lights, meshes, materials, ray marching, render nodes, and output nodes. | **Partial.** Typed mesh and Transform Image 3D slices remain bounded. Native source camera-set, light, terrain atlas, and SDF renderer slices now exist, and the build47 CPU-filtered renderer passed 25 cases/2,541 assertions. Full documented 3D coverage, GPU acceptance, and licensed-reference behavior remain open. |
| 5.2 | Define typed render capabilities and explicit resource lifetime for each 3D node. | **Partial.** Renderer-owned Transform Image 3D adapter/resident queue and GPU tests exist in `../../mono.engine/render/`. They do not establish lifetime/resource contracts for every 3D node. |
| 5.3 | Complete material-map, shader-input, and skybox sink bindings in the engine-output matrix. | **Incomplete.** No full sink acceptance or required named scene is present. See sink inventory below. |
| 5.4 | Reuse cooked shaders and declared sampler inputs; admit named samplers through validation; compile HLSL/shader nodes during authoring or cook; ship binaries without runtime compiler. | **Incomplete.** Existing shader cook/check paths are not evidence that Pixel Composer shader nodes, named-sampler admission, or full output bindings meet this contract. |
| 5.5 | Compare controlled captures, output structures, and diagnostics at fixed backend and resolution. | **Blocked / incomplete.** Vulkan native fixtures cover a Transform Image 3D slice. Official executable is unavailable, so same-input executable captures and exact parity remain unknown. |

### M6: I/O, scripting, PXC, and export parity

| # | Requirement | Status and current evidence |
|---|---|---|
| 6.1 | Implement native image, video, sequence, and export rows through bounded host adapters. | **Partial.** Native image codec/export routes have 16 fixed-input checks with independent decoders. Bounded granted raster and GIF still-frame hosts and ordered live-array loading have focused native routes; build49 export passed 36 cases/732 assertions, and live-array coverage passed 2/10. Bounded WAV input and Studio WAV publication also have focused tests. Pixel Composer `Node_Export`, video, full sequence rendering, remaining format behavior, and licensed-reference parity remain open. |
| 6.2 | Implement file, network, and shell rows as explicit capability nodes with permissions, recorded input, or runtime refusal. Never grant ambient evaluator power. | **Partial.** Explicit file grants and bounded process grants now route through `../../mono.engine/imagegraphexport/src/GraphFileHost.cpp` and `GraphCommandHost.cpp`; process execution binds the script, literal arguments, and child working directory. Focused host tests exist. A complete capability inventory and network, shell, refusal, and policy acceptance across all rows remain open. |
| 6.3 | Implement Lua/expression execution with bounded APIs and replayable inputs; compile HLSL/shader nodes at authoring/cook into validated binaries; no shipped runtime compiler. | **Partial.** A bounded Composer Lua host and bounded authored input-expression compile/evaluation paths have native tests in `../../mono.engine/scriptluau/tests/ComposerLua.cpp` and `../../mono.engine/imagegraph/tests/PcxExpression.cpp`. Full Lua API/replay coverage and Pixel Composer HLSL/shader authoring, cook validation, and runtime-compiler policy remain open. |
| 6.4 | Finish native PXC read/write through the required format gate. | **Partial, release gate open.** Native PXCX import and byte-exact unchanged round trips are recorded for the five supplied samples. Bounded edits now have dedicated coverage for structure, groups, source inputs, keys, and authored regions in `../../mono.engine/imagegraphio/tests/PxcxStructureEdit.cpp`, `PxcxGroups.cpp`, `PxcxEdit.cpp`, and `PxcxKeyStructure.cpp`; Studio source-group/key history is separately covered. Authoritative format evidence, every-version/domain validation, full supplied-project semantic/render comparison, fuzz acceptance, and Pixel Composer validation remain open. See PXC acceptance below. |

### M7: parity release gate

| # | Requirement | Status and current evidence |
|---|---|---|
| 7.1 | Pass every documented node, product workflow, and engine-output sink row against the pinned documentation snapshot and native fixtures; policy prohibitions block release until accepted as exceptions. | **Incomplete.** Current matrix totals are 2/995 nodes implemented, 979 blocked, 14 unknown, and 0/20 workflows passing. Sink acceptance is incomplete. |
| 7.2 | Run native fixtures and reviewed visual studies against supplied media; keep executable comparison separate and open until licensed build and matched captures exist. | **Partial.** Native `noise_aniso` implementation is present; `mk_sparkle` has a registered native executor and captured random draws. Those facts do not close project-level render studies or the licensed executable comparison. `grid` and `path_shape` still have unresolved node behavior. |
| 7.3 | Profile static 2D, animated, feedback, simulation, and 3D graphs in release. | **Partial, release gate open.** One `just imagegraph-source-family-bench` job completed for native headless microfamilies and array structure/edit. No timing values were retained, and this does not cover the five required graph classes. Complete release profiles for static 2D, animated, feedback, simulation, and 3D remain open. |
| 7.4 | Run Studio and client scenes and preserve release evidence. | **Incomplete.** All seven named scene files are present, but manual Studio/client inspection, GPU acceptance, and release evidence remain open. |

## Other plan acceptance obligations

### PXC read/write parity gate

All five requirements below remain open or partial. Evidence is described in [reference gate](reference-gate.md), [fixture manifest](supplied-fixture-manifest.json), and the ledger's `pxc_edit_transaction_io_phase` record.

1. Obtain authoritative PXC specification or validated format description. **Open.** Current layout observations are limited to five files.
2. Verify PXCX framing, versions, compression, graph records, values, links, resources, timeline, and export against controlled fixtures. **Partial.** Five supplied samples and synthetic stream tests cover limited structure; dedicated tests now exercise bounded group and structural edit records. Broad version/domain validation and paired semantic/render comparison remain absent.
3. Add bounded parser/writer tests for valid, malformed, unknown-version, and unsupported-node documents, plus bounded parser fuzzing. **Partial.** Reader/writer tests cover the current supported paths and bounded structural edits; `just bake-pxcx-fuzz` exists, but the full rejection matrix and an accepted fuzz run are not recorded.
4. Round-trip every supplied PXC and compare graph structure, parameters, animation, and render output against reference captures. **Partial.** Native read/write covers the five supplied samples and unchanged archives, with bounded structural/group/key edits covered separately. Full paired semantic and rendered-output comparison is not evidenced.
5. Verify native-generated PXC in Pixel Composer or an authoritative validator before claiming writer parity. **Blocked.** No licensed application or validator result exists.

### Required living scenes

All seven named `.aworld` files are present under `../../mono.engine/examples/assets/worlds/`. Their authored presence does not establish manual or GPU acceptance; every scene remains open for those checks.

| Required scene | Evidence it must preserve | Status |
|---|---|---|
| `Composer-Particle-Flipbook.aworld` | Fire and spark sheets drive particles, beams, and trails with safe retirement. | File present; manual/GPU acceptance open. |
| `Composer-Material-Maps.aworld` | One graph feeds colour, normal-like, and packed PBR maps. | File present; manual/GPU acceptance open. |
| `Composer-Gui-And-Shader.aworld` | ImageLabel/ImageButton states update while a cooked shader samples graph mask. | File present; manual/GPU acceptance open. |
| `Composer-Skybox.aworld` | Six outputs form one skybox generation and reject an invalid face. | File present; manual/GPU acceptance open. |
| `Composer-Feedback-And-Fluid.aworld` | Fixed-seed feedback, fluid, and rigid reset/step behavior. | File present; manual/GPU acceptance open. |
| `Composer-3D-Reference.aworld` | Controlled scene, refraction, ray-march, and camera fixtures. | File present; manual/GPU acceptance open. |
| `Composer-Reference-Studies.aworld` | Native studies of black hole, fire tornado, glass, trim, and spark references. | File present; manual/GPU acceptance open. |

### Engine-output sinks

Every row needs named native acceptance and safe last-good/owner behavior as specified. Existing consumers are seams, not acceptance.

| Sink | Required acceptance | Status |
|---|---|---|
| Particle image | Generated still reaches standard particle collection; last-good output survives error. | Partial seam; scene acceptance open. |
| Particle flipbook | Declared cells play at fixed or per-frame timing through 256 cells; larger sequences use a separate path. | Partial: `.atex` particle timing is tested through 256 cells. `.aseq` has CPU sequence coverage and a renderer integration slice; GPU acceptance and larger-sequence behavior remain open. |
| Beam and trail | Live name resolves through ordinary texture demand. | Partial binding seam; no full scene acceptance. |
| Material maps | Colour and linear semantics validate and draw through normal material resolution. | Open. |
| GUI images | ImageLabel/ImageButton normal, hover, and pressed states resolve normally. | Partial: reflected scene/game selectors pass focused binding tests, but normal/hover/pressed rendering and full sink-scene acceptance remain open. |
| Shader inputs | Cooked shader samples generated data after validated named-sampler admission, with no runtime compilation. | Open. |
| Skybox | Six coherent names update atomically or prior sky remains. | Open. |
| Static content | TextureData and normal cook/content preserve pixels, colour space, mip, and flipbook facts. | Partial: native image codecs have 16 independent-decoder checks and asset/cook paths exist; end-to-end content acceptance remains open. |

### Required checks and release evidence

`RUNNING.md` defines the available recipes. The plan's acceptance requires all matrix suites, architecture checks, headless runner, static cook/reload, PXC round trip, render studies for all five supplied projects, release profiles for all graph classes, and manual Studio/client inspection of every required scene. None is waived by a narrow suite pass.

Relevant named checks available in this checkout:

- `just test-architecture` and `just source-check` for declared architecture and source rules.
- `just test engine.imagegraph`, `just test engine.imagegraphio`, `just test engine.render`, `just test client`, `just test studio`, and `just test tools.imagegraph.runner` for focused suite families. Use `just test-list` to confirm suite selectors before running.
- `just test-all` for the complete suite set; `just render-check` for GPU renderer tests.
- `just preset=release build`, plus `just bench` or the relevant named benchmark recipe for measured release evidence.
- `just edit` / `just run` for manual Studio/client scene inspection, recorded with the corresponding scene and backend.

The [October 2 native validation ledger](native-validation-2026-10-02.json) records retained gates; its newest results were executed October 3 in Darwin. Build47 had 4 failing cases and 5 failed assertions across `Source2DGridNodes.cpp` (mixed producer), `FlipDestroy.cpp` (direct budget), `OutputDiagnostics.cpp` (two PXC assertions), and `SourceRouting.cpp` (fresh-seek cache, one assertion). Build48 passed all 1,175 core cases and 1,808,028 assertions. Build49 passed dev and release core suites, each with 1,187 cases and 1,808,130 assertions. Build49 I/O passed 162/5,926, export 36/732, focused Studio 39/894, Composer workflow 2/122, and client CPU adapters 16/1,478. Adapter targets linked in 208 steps. Earlier headless scene, example, and CPU renderer results remain narrow gates, not full acceptance. Commits `28b3e64b` and `b980f56f` add bounded raster input and one-evaluation live source-output routing. Commits `cae0626b`, `c908f9fc`, and `c57e05ca` add native camera/SDF renderer slices, authored Composer scene fixtures, and corrected audio evidence. Commits `b307cc14`, `a3cd1af5`, `f7309522`, `e9b07cbb`, `d39a540c`, and `ef769cea` add bounded Grain/Contrast Blur kernels, a FLIP advance guard, opaque PXC key safeguards, a headless group editing workflow, granted GIF still-frame input, and camera-set/light/terrain atlas slices. The Composer workflow test covers a bounded native path and a refusal case; ambiguous moved opaque-key source compatibility remains open. A helper linked to the build49 archive reports 406 registered `pc.*` names, 405 overlapping the 886-entry source catalogue, 481 catalogue nodes without a registered executor, and one registered-only name, `pc.global_scope`; [the exact list and archive hash](../../.cache/build/dev/evidence/pixel-composer-2026-10-02/executor-inventory/registered-vs-catalogue-build49.json) provide capability counts, not parity. Registration is not evidence of behavior, and some adapter routes, including 3D output routes, may be outside the registry. The licensed executable is unavailable, GPU acceptance is pending, and exact Pixel Composer parity remains unverified. No project tests or builds were run while updating this ledger and progress summary; the temporary capability helper was linked and run against the build49 archive.

## Current completion decision

**M0-M7 documented feature completion: incomplete.** **Exact official executable parity: unavailable and unverified.** Continue from the open requirements above. Preserve the matrix's blocked and unknown statuses until their named acceptance evidence is reviewed; do not infer completion from an implementation file, test name, source-derived expectation, or a native-only round trip.
