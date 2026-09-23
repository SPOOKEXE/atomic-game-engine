# v0.26 Pixel Composer implementation plan

## Goal

Deliver Pixel Composer feature parity as an engine-native graph product. It includes the complete authored graph
workflow, documented node catalogue, 2D and 3D generation, filters, transforms, data and math, animation,
audio-driven operations, VFX, rigid and fluid simulation, feedback, groups, arrays, expressions, I/O, export,
headless execution, PXC interchange, and first-class engine output bindings.

This is a large multi-milestone v0.26 program. A starter node set is not parity. No milestone claims parity until all
of its version-pinned inventory rows and reference fixtures pass.

## Evidence and parity baseline

The supplied files are five binary PXCX-prefix projects: Black-Hole_121092.pxc, Fire-Tornado_121092.pxc,
Glass-Block-Refraction_121092.pxc, Ornate-Trim_121092.pxc, and Spark-Bolt_121092.pxc. The supplied captures are
005_nodes.png, 008_3d.png, 001_effect_2.gif, 006_effects.gif, and 007_simulation.gif. They show graph and timeline
workflows, 3D nodes, VFX and simulation. Screenshot text appears to identify version 1.16.6.0; strings in project
files appear to identify 1.22.10.201. Both are observations, not verified format semantics.

Create and pin a source-of-truth inventory from the official
[Pixel Composer documentation](https://docs.pixel-composer.com/) and
[node documentation](https://docs.pixel-composer.com/nodes/). Record retrieval date, source product
version when exposed, node page URL, node id, ports, types, properties and defaults, dynamic inputs, animation,
arrays/groups, errors, required resources and documented edge behavior. Do not infer undocumented behavior from a
name, screenshot or binary string.

Every node gets a node-parity-matrix row with one status: documented and implemented, documented and blocked by named
prerequisite, undocumented but verified by approved fixture, unknown, or prohibited by engine policy. Every
implemented row gets a fixture containing graph, inputs, parameters, ticks, expected typed output and exact hash,
numeric tolerance, structural assertion or reviewed image comparison.

Maintain a separate product-workflow matrix. It covers project creation and settings, collections and assets, graph
canvas interaction, inspector and property editing, palette and gradient editing, 2D and 3D preview, timeline,
keyframes, UI interaction, batch export, headless CLI execution, and diagnostics/profiling. Each workflow row names
its acceptance path and reference evidence. Passing every node does not establish product parity if an author cannot
perform the corresponding workflow.

| Family | Required matrix coverage |
|---|---|
| Core graph | Typed ports, junctions, defaults, dynamic inputs, links, groups, subgraphs, arrays and errors |
| Import/export/I/O | Image, video, sequence, PXC, paths, headless, file, network and shell behavior |
| 2D | UV, generate, draw, pixel builder, transform, compose, filter and effect nodes |
| Data | Scalar, colour, vector, matrix, text, conversions, random, curves, math and expressions |
| Animation/audio | Timeline, keyframes, interpolation, playback modes, audio inputs and frame semantics |
| Stateful VFX | Feedback, particles, rigid, fluid and all simulation reset/state rules |
| 3D | Scene, cameras, lights, meshes, materials, ray marching, render and 3D outputs |
| Scripting | Expression, Lua and shader-language nodes with policy and resource boundaries |

## Architecture

Create mono.engine/imagegraph at shared L9 and add it to mono.tools/architecture/expected_graph.json. Future
implementation adds its AGENTS.md and focused tests with the module and architecture row. It owns the
canonical document, registry, schemas, compiler, deterministic device-independent evaluator, bounded state, typed
outputs and declarative sink bindings. It can depend only on lower engine layers such as assets, core, ecs, parallel
and needed world types, proven through the architecture check.

Render owns GPU execution, 3D work, shader and descriptor admission, GPU lifetime and owner-scoped dynamic texture
publication. Game and client own save/content integration and scheduling. Studio owns the graph canvas, inspector,
timeline and preview adapters. Imagegraph never links render, Studio nodegraph, a filesystem, network socket, shell,
or decoder directly.

Keep existing systems separate. Bakegraph and bake remain asset import/cook. Engine graph remains render-pipeline
planning. Studio nodegraph remains a Studio-only canvas. Effects remains the game particle-emitter system and its
future effect graph, as planned in [VFX system](future-work/vfx-system.md). Imagegraph VFX and simulation nodes may
generate images or their own graph state, but feed particle emitters through stable texture names and do not replace
the game effects emitter, motion, or draw systems. The module dependency plan follows
[the engine architecture](CODE_ARCH.md).

The canonical document stores format version, stable string node and port ids, durable text or UUID node instance
ids, values, links,
outputs, group/subgraph declarations, timeline/keyframes, layout and declared external dependencies. It never stores
pointers, process-local Name ids, renderer handles, compiled plans, workers, previews, caches or pixels.

Compilation rejects unknown/version-incompatible nodes, invalid properties, incompatible ports, duplicate wires,
illegal cycles, incomplete feedback definitions, invalid outputs, dimensions and limit excess. It produces one
immutable plan with stable order and diagnostics naming durable nodes and ports. Do not reuse bakegraph NodeKind.

## Types, time, cache and lifetime

Use explicit types: scalar, bool, enum, colour, vector, matrix, text, path, image, image-array, volume, mesh, scene,
material, audio frame, flipbook, cubemap, seed and simulation state. Match documented junction compatibility and use
explicit conversion nodes where the reference requires them. Do not add undocumented implicit casts, any payloads or
raw string property bags.

Published 2D output uses assets TextureData, including RGBA8, RGBA8_LINEAR, R8, mip and current flipbook metadata.
TextureData carries only atlas side, populated frame count and one frame rate. Effects playback is owned by the
consumer through FlipbookMode, and current effects layout accepts at most an 8 by 8 atlas with 64 cells.

The graph document and evaluator must preserve lossless animation and sequence timing, including more than 64 frames
and variable frame durations. An interim binding may refuse an unsupported particle target with a diagnostic, but M7
requires an extended texture/effects contract or sequence-playback path that preserves requested particle playback.
Never truncate frames, collapse variable durations to one rate, or silently repartition an atlas. A cubemap is six
named equal-size, equal-format faces. 3D output declares target, depth, samples, colour space, lifetime and readback
availability.

Every evaluation receives graph revision, seed, fixed tick index, fixed delta, parameter snapshot, selected output,
dimensions and format. It has no wall clock or hidden random state. Random samples derive from seed, node id,
coordinate, frame and declared sample index. Equivalent input produces equivalent output on a supported evaluator.

Static graphs run after signature changes. Animated and simulation graphs run at declared fixed rates with bounded
catch-up. A late result cannot overwrite a newer generation. Feedback, VFX, rigid and fluid state have explicit
previous/next buffers, seed, warmup, reset value, fixed step and reset triggers.

Visual client output may coalesce stale jobs. Simulation-visible output completes inside its owning tick with replayable
fixed-step rules. Replication carries graph name, parameters, seed and binding intent when required, never pixels or
GPU handles. File, network, shell, live audio, or other nondeterministic external input cannot drive replicated or
tick-visible output without a captured input record and replay contract.

Cache by document hash, compiler/node version, selected output, parameters, seed, relevant time/tick, dimensions,
format and external revisions. Invalidate only downstream closure. Budget nodes, links, pixels, arrays, volumes,
mesh data, intermediate bytes, simulation buffers, output bytes, work/tick, queued jobs, GPU residency and previews.
Preview has an independent smaller budget and cannot evict a live generation.

Three owners remain separate:

1. Asset or Studio session owns authored document data.
2. Client, cooker or Studio preview owns cancellable evaluator state and CPU cache.
3. Render owns uploaded GPU generations through their final possible draw.

Stale document, world, binding, output or tick results are rejected before upload. Workers never call renderer.
Publisher keeps the last good generation until a replacement is admitted, then retires it safely. Teardown, owner
loss, rebind and failed admission each retire one generation exactly once.

## Complete parity milestones

### M0: inventory and reference harness

1. Crawl and pin official documentation. Build node and product-workflow parity matrices with fixture manifests.
2. Establish a reference capture runner at pinned Pixel Composer versions where possible.
3. Characterise the five supplied projects without claiming their binary layout is public.
4. Record baseline graph layouts, parameters, ticks, images and diagnostics for all reference fixtures.
5. Gate later work on reviewed matrix coverage. Unknown remains unknown.

### M1: graph model, editor and headless runner

1. Implement versioned document, registry, typed schemas, links, groups, arrays, subgraphs, dynamic inputs,
   diagnostics and migrations.
2. Build Studio adapter using nodegraph for canvas, typed sockets, inspector, node search, preview, timeline,
   keyframes, undo and diagnostics navigation.
3. Build headless native runner with parameters, tick range, output selection, hashes and diagnostics. Headless 3D
   execution uses an offscreen render adapter or reference backend; core imagegraph remains device-independent
   scheduling plus CPU-pure nodes, while GPU and 3D execution stays in render.
4. Match core graph/property/group/array/error fixtures and canvas/inspector/timeline workflow fixtures before
   large node families.

The Studio dock uses true black background, white primary text, dense information layout, no decorative cards or
pills, and no continuously repainting animation. It contains graph canvas, typed sockets, inspector, preview,
timeline, asset and sink panel, diagnostics, cache/budget state and profiler panel.

### M2: full 2D and data parity

1. Implement every documented UV, generate, draw, pixel builder, transform, compose, filter and effect row.
2. Implement every scalar/vector/matrix/text/math/conversion/random/curve/expression row.
3. Match defaults, edge sampling, alpha, colour space, arrays and animated behavior node by node.
4. Cook stills, maps and flipbooks into normal engine texture assets.

### M3: timeline, animation, audio and feedback

1. Implement keyframes, interpolation, playback, fractional-frame policy and timeline behavior.
2. Implement audio nodes with captured-frame contracts and deterministic recorded-fixture playback.
3. Implement feedback and stateful image operations with explicit precision, warmup, fixed step and reset.
4. Match fixtures across start/end, wrap, seek, reset and dropped-update cases.

### M4: VFX, simulation and live engine bindings

1. Implement all documented VFX, rigid and fluid simulation rows after state and resource contracts are proved.
2. Feed generated texture names into existing effects systems and future effect graph work.
3. Add owner-scoped live texture registry, last-good fallback and every consumer binding.
4. Match fixed seed, fixed tick, reset and long-run drift fixtures.

### M5: complete 3D parity

1. Implement documented scenes, cameras, lights, meshes, materials, ray marching, render nodes and output nodes.
2. Define typed render capabilities and explicit resource lifetime for each 3D node.
3. Reuse cooked shaders and declared sampler/resource inputs. Add a validated named-sampler binding extension before
   arbitrary composer output may bind a cooked shader sampler. It enumerates allowed sampler names, expected
   dimensions, format, colour space and owner lifetime. HLSL and shader-language composer nodes compile during
   authoring or cook into validated binaries; shipped clients consume those binaries and carry no runtime compiler.
4. Compare controlled captures, output structures and diagnostics at fixed backend and resolution.

### M6: I/O, scripting, PXC and export parity

1. Implement native image, video, sequence and export rows through bounded host adapters.
2. Implement file, network and shell rows as explicit capability nodes with permissions, recorded input or runtime
   refusal. They are never ambient evaluator power.
3. Implement Lua/expression execution with bounded APIs and replayable inputs. Implement HLSL and shader-language
   nodes through authoring/cook compilation into validated binaries, with no shipped runtime compiler.
4. Finish native PXC read/write through the required format gate.

### M7: parity release gate

1. Every node-matrix and product-workflow-matrix row is implemented and passing. A policy prohibition is a release
   blocker until user accepts the corresponding parity exception.
2. Run native and reference fixtures, publish tolerances and reviewed visual-diff evidence.
3. Profile static 2D, animated, feedback, simulation and 3D graphs in release.
4. Run Studio and client scenes and preserve release evidence.

## Engine output integration

| Sink | Existing seam | Acceptance |
|---|---|---|
| Particle image | effects ParticleEmitter Texture | Generated still reaches standard particle collection and last-good output remains on error. |
| Particle flipbook | ParticleEmitter Flipbook, FlipbookFrames and FlipbookFramerate | Generated atlas plays declared cells at authored rate; M7 extends the consumer contract or uses sequence playback for lossless >64-frame or variable-duration output. |
| Beam and trail | Existing effects texture fields | Live name resolves through ordinary texture demand. |
| Material maps | scene MaterialMaps PBR names | Colour and linear map semantics validate and draw through normal material resolution. |
| GUI images | ImageLabel Image and ImageButton normal/hover/pressed picture properties | Every GUI state uses normal interface texture resolution. |
| Shader inputs | Existing material, interface and declared render sampler contracts | A cooked shader samples generated data only after validated named-sampler binding admission, with no runtime shader compilation. |
| Skybox | scene SkyboxTextures Front, Back, Left, Right, Up and Down | Six coherent names update or the prior sky remains. |
| Static content | TextureData and normal asset cook/content | Export/reload preserves pixels, colour-space, mip and flipbook facts. |

## PXC read/write parity gate

Current fact: supplied PXC files are binary and begin with PXCX. Import or writer compatibility is not established.
Native PXC read and write are explicit parity work and release gates.

1. Obtain an authoritative PXC format specification or a validated format description.
2. Verify PXCX framing, versions, compression, graph records, values, links, resources, timeline and export against
   controlled fixtures.
3. Write bounded parser and writer tests for valid, malformed, unknown-version and unsupported-node documents.
4. Round-trip every supplied PXC file through native read/write, compare graph structure, parameters, animation and
   render output against reference captures.
5. Verify native-generated PXC in Pixel Composer or an authoritative validator before claiming writer parity.

If this is infeasible or undocumented, the parity gate stays open. Do not silently replace it with a best-effort
translator or a native-only format claim.

## Tests and scenes

Generate focused tests from node and product-workflow matrix rows. Each public header has a focused suite. GPU/3D
tests use real renderer fixtures, never a mock renderer. Add these living scenes:

| Scene | Evidence |
|---|---|
| Composer-Particle-Flipbook.aworld | Fire and spark sheets drive particles, beams and trails with safe retirement. |
| Composer-Material-Maps.aworld | One graph feeds colour, normal-like and packed PBR maps. |
| Composer-Gui-And-Shader.aworld | ImageLabel/ImageButton states update while a cooked shader samples graph mask. |
| Composer-Skybox.aworld | Six outputs form one skybox generation and reject invalid face. |
| Composer-Feedback-And-Fluid.aworld | Fixed seed feedback, fluid and rigid reset/step behavior. |
| Composer-3D-Reference.aworld | Controlled scene, refraction, ray-march and camera fixtures. |
| Composer-Reference-Studies.aworld | Native studies of black hole, fire tornado, glass, trim and spark references. |

Completion requires architecture checks, all node and product-workflow matrix suites, native headless runner, static
cook/reload, PXC round-trip and render comparison on all five supplied files, release profiles for all graph classes,
and manual Studio/client inspection of every scene.

## Risks

| Risk | Control |
|---|---|
| Parity becomes a small starter set | M0 has node and product-workflow matrices, and M7 gates both at row level. |
| PXC behavior or format is undocumented | Keep PXC read/write gate open until verified. |
| Async work breaks tick ordering | Match result generation/tick and use synchronous fixed-step state when gameplay-visible. |
| Dynamic images leak memory | Separate document/evaluator/render ownership with budgets and retirement tests. |
| Each sink grows a private path | Publish ordinary owner-scoped names and use existing resolution. |
| Scripting or shader nodes bypass policy | Use explicit capabilities, bounded APIs and cooked shaders. |
