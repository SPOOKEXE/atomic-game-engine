# v0.26 Pixel Composer implementation plan

## Goal

Deliver Pixel Composer's documented features as an engine-native graph product. It includes the complete authored graph
workflow, documented node catalogue, 2D and 3D generation, filters, transforms, data and math, animation,
audio-driven operations, VFX, rigid and fluid simulation, feedback, groups, arrays, expressions, I/O, export,
headless execution, PXC interchange, and first-class engine output bindings.

This is a large multi-milestone v0.26 program. A starter node set is not completion. Track documented feature
completion separately from exact comparison with an official executable. The user supplied GIF and PNG captures and
directed development without a licensed reference executable, so exact executable comparison remains unverified.

## Evidence and parity baseline

The supplied files are five binary PXCX-prefix projects: Black-Hole_121092.pxc, Fire-Tornado_121092.pxc,
Glass-Block-Refraction_121092.pxc, Ornate-Trim_121092.pxc, and Spark-Bolt_121092.pxc. The supplied captures are
005_nodes.png, 008_3d.png, 001_effect_2.gif, 006_effects.gif, and 007_simulation.gif. They show graph and timeline
workflows, 3D nodes, VFX and simulation. Screenshot text appears to identify version 1.16.6.0; strings in project
files contain the string 1.22.10.201. All five samples have a PXCX wrapper, a compressed thumbnail, a metadata block,
and a compressed JSON graph. Their metadata also contains the integer 121092. These observations establish a
fixture-specific layout, not a public format specification. The string may be a schema or build id, not the
application version, and must not be treated as one without validation.

Create and pin a source-of-truth inventory from the official
[Pixel Composer documentation](https://docs.pixel-composer.com/) and
[node documentation](https://docs.pixel-composer.com/nodes/). Record retrieval date, source product
version when exposed, node page URL, node id, ports, types, properties and defaults, dynamic inputs, animation,
arrays/groups, errors, required resources and documented edge behavior. Do not infer undocumented behavior from a
name, screenshot or binary string.

At M0, identify the latest official Pixel Composer build and record its version, release channel, release date and
platform. If a licensed executable becomes available, also record its hash and run controlled captures. The current
candidate is
[1.21.10 beta, dated September 13, 2026](https://makham.itch.io/pixel-composer/devlog/1662052/12110-beta);
record whether the official build can be acquired and run for reference captures. Use
[1.21.0 stable, dated April 28, 2026](https://makham.itch.io/pixel-composer/devlog/1502094/1210-stable) as a
comparison fallback if the beta is not reproducible. A fallback does not permit omitting features in newer
documentation: the inventory must reconcile every current documentation row to the pinned build, marking build
availability or behavior as unresolved until verified. Archive the exact documentation snapshot used for the
inventory with its retrieval date and content hashes. Supplied project version strings remain unidentified unless
validated against the application. The supplied media provides visual workflow evidence, not exact node outputs or
hidden defaults.

The creator's public [Pixel Composer source](https://github.com/Ttanasart-pt/Pixel-Composer) is also available
under its [MIT license](https://github.com/Ttanasart-pt/Pixel-Composer/blob/main/LICENSE). The implementation
inventory uses commit `b69eca232217360cf1502ef0223523d818606652` (committed September 23, 2026) as a
read-only source reference for controls and file structure that the documentation omits. The local checkout is
outside this repository. This commit may differ from the supplied project files and archived documentation, so
source-derived behavior needs an explicit provenance note in each affected matrix row. It does not replace the
licensed executable capture gate.

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
| External formats and devices | Aseprite, Krita, ORA and GameMaker imports; MIDI and Spout inputs/outputs |
| Project and diagnostics | Project settings, tilesets, cache controls, debug tools, migration and diagnostics workflows |
| 2D | UV, generate, draw, pixel builder, transform, compose, filter and effect nodes |
| Data | Scalar, colour, vector, matrix, text, conversions, random, curves, math and expressions |
| Animation/audio | Timeline, keyframes, interpolation, playback modes, audio inputs and frame semantics |
| Stateful VFX | Feedback, particles, smoke, FLIP, strand and Verlet simulation, rigid and fluid state/reset rules |
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

The authored graph is immutable content: its canonical document stores format version, stable string node and port
ids, durable text or UUID node instance ids, authored values, links, outputs, group/subgraph declarations,
timeline/keyframes, layout and declared external dependencies. Live parameters and output bindings shared between
systems are ECS-owned state, identified across boundaries by stable names. Evaluator and render caches are derived
only; they are never authoritative state. The document never stores pointers, process-local Name ids, renderer
handles, compiled plans, workers, previews, caches or pixels.

Compilation rejects unknown/version-incompatible nodes, invalid properties, incompatible ports, duplicate wires,
illegal cycles, incomplete feedback definitions, invalid outputs, dimensions and limit excess. It produces one
immutable plan with stable order and diagnostics naming durable nodes and ports. Do not reuse bakegraph NodeKind.

## Types, time, cache and lifetime

Use explicit types: scalar, bool, enum, colour, vector, matrix, text, path, image, image-array, volume, mesh, scene,
material, audio frame, flipbook, cubemap, seed and simulation state. Match documented junction compatibility and use
explicit conversion nodes where the reference requires them. Do not add undocumented implicit casts, any payloads or
raw string property bags.

Published 2D output uses assets TextureData, including RGBA8, RGBA8_LINEAR, R8, mip and flipbook metadata.
TextureData v5 can carry exact per-frame durations as well as atlas side and populated frame count. Effects playback
is owned by the consumer through FlipbookMode. The implemented fixed-rate and variable-duration particle paths
accept up to a 16 by 16 atlas with 256 populated cells.

The graph document and evaluator must preserve lossless animation and sequence timing, including more than 64 frames
and variable frame durations. An interim binding may refuse an unsupported particle target with a diagnostic, but M7
requires an extended texture/effects contract or sequence-playback path that preserves requested particle playback.
Never truncate frames, collapse variable durations to one rate, or silently repartition an atlas. A cubemap is six
named equal-size, equal-format faces. 3D output declares target, depth, samples, colour space, lifetime and readback
availability.

Every evaluation receives graph revision, seed, fixed tick index, fixed delta, parameter snapshot, selected output,
dimensions and format. It has no wall clock or hidden random state. Random samples derive from seed, node id,
coordinate, frame and declared sample index. Equivalent input produces equivalent CPU output on a supported
evaluator. Tick-visible output is deterministic under the declared fixed-step and recorded-input contract.
GPU-rendered visual output may vary by backend, driver or device; its parity uses a pinned backend/device
configuration and reviewed image tolerances rather than claiming bit-exact cross-backend determinism.

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

1. Archive the retrieved official documentation pages with retrieval date and content hashes. Record 1.21.10 beta as
   the current official build candidate and 1.21.0 stable as a comparison fallback. Reconcile every current
   documentation row against available build evidence; no current row may be omitted because it was added after a
   fallback. Build node and product-workflow matrices with fixtures.
2. Characterise the supplied GIF and PNG captures as visual references. Keep a reference capture runner ready for a
   licensed executable, but record its gate as unavailable by the user's choice. This does not block documented
   feature development. It does block claims of exact executable parity and undocumented behavior.
3. Characterise the five supplied projects without claiming their binary layout is public.
4. Record baseline graph layouts, parameters, ticks, images and diagnostics for all reference fixtures.
5. Gate later work on reviewed documentation inventory coverage. Unknown remains unknown.

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

Current native array support preserves nested image shape, applies Spread Array one level, selects indexed elements
with Clamp, Loop and Ping Pong overflow, and rejects nested or multi-image output at a single-image sink. Height
Blend executes bounded image arrays in Loop, Hold, Expand and Expand Inverse order. Other processor nodes still need
the same array route, and non-image elements remain unsupported. M1 array parity remains open until those routes and
controlled input/output fixtures pass.
The pinned `node_processor.gml` uses a reversed suffix stride for Expand Inverse; for unequal two-input lengths,
that formula repeats some index pairs, while the documentation says all combinations. Record both expected orders
and resolve the discrepancy against a controlled executable before claiming exact parity.

The Studio dock uses true black background, white primary text, dense information layout, no decorative cards or
pills, and no continuously repainting animation. It contains graph canvas, typed sockets, inspector, preview,
timeline, asset and sink panel, diagnostics, cache/budget state and profiler panel.

### M2: full 2D and data parity

Imagegraph v3 introduced bounded Gradient, Area, Curve, Vector4 and Path2D values with typed sockets, defaults,
junctions and keyframes. Earlier imagegraph documents migrate without changing their authored values. Studio seeds
Posterize with a typed Colour palette and edits ordered RGBA8 swatches, capped at 32 entries; palette data round trips
through the native document. Gradient and Shape image execution and reference pixel comparison are still separate
acceptance work. File path strings remain text, distinct from Path2D graph paths.

1. Implement every documented UV, generate, draw, pixel builder, transform, compose, filter and effect row.
2. Implement every scalar/vector/matrix/text/math/conversion/random/curve/expression row.
3. Match defaults, edge sampling, alpha, colour space, arrays and animated behavior node by node.
4. Cook stills, maps and flipbooks into normal engine texture assets.

### M3: timeline, animation, audio and feedback

1. Implement keyframes, interpolation, playback, fractional-frame policy and timeline behavior.
2. Implement audio nodes with captured-frame contracts and deterministic recorded-fixture playback.
3. Implement feedback and stateful image operations with explicit precision, warmup, fixed step and reset.
4. Match fixtures across start/end, wrap, seek, reset and dropped-update cases.

The native imagegraph v4 slice preserves v1 through v3 fixed-tick keyframe results and adds bounded timeline
settings, per-property hold/loop/ping/wrap schedules, and authored incoming/outgoing linear, cut and Bezier easing.
New side easing executes for finite scalar, Vector2, Vector4 and RGBA8 colour values. The pure playback helper
models begin, stop, loop, pingpong and capped fixed-tick catch-up. Imagegraph v5 persists finite positive FPS and
adds a fractional request in `[0,1)`; integer requests preserve existing evaluation results. Older timeline files
migrate to 30 FPS, and the core accepts any finite positive rate with a finite positive frame duration.

Studio exposes play, pause, integer stepping, saved frame range, playback mode, per-property end and loop-tail
controls, and fractional frame seeking. Preview evaluation and its bounded cache include the fractional position.
FPS, range and mode are stored together in a v5 timeline, with a 30 FPS editor default before a timeline is saved.
Playback itself still advances in integer ticks with up to eight catch-up steps per update. Imagegraph v6 adds a
scalar Sine keyframe driver, authored in Studio with Frequency, Amplitude, Phase and Smooth controls. The pinned
[`KeyDriver_Sine` source](https://github.com/Ttanasart-pt/Pixel-Composer/blob/b69eca232217360cf1502ef0223523d818606652/scripts/node_keyframe_driver/node_keyframe_driver.gml)
defaults to frequency 4, amplitude 1, phase 0 and smooth 0. It adds
`sin((phase + time * frequency / frames_total) * 2π) * amplitude` to the interpolated scalar. A positive Smooth
value fades the offset at the ends of the active key interval with the source's smoothstep envelope. The native
four-frame fixture sets frequency 1 and amplitude 0.25, so frame 1 yields exactly a +0.25 scalar offset; a separate
fractional test covers the source defaults. v1-v5 documents migrate to v6 without adding drivers or changing key
values. This behavior is source-derived and native-tested, not executable-reference verified. Additive keys,
audio-driven animation and feedback history remain open M3 work. Legacy cubic keys are retained but report
`UnsupportedExecution`; source-side easing is the supported editable path. The pinned source's range-start pingpong
behavior and dropped-update stepping are recorded in focused native and Studio fixtures; executable reference
comparison remains unavailable.

The limited audio path is now `image.audio_recording` to `image.audio_volume`. A bounded `audio-capture 1` file
stores recorded mono sample arrays by source ID and exact tick. Each line is `frame "mono" 0 2 1 -1`; IDs are
1 to 255 ASCII letters, digits, underscores or hyphens. `audio-capture 2` adds a positive sample rate after the
tick, and v1 remains the exact encoding for captures without that metadata. The file is capped at 4096 frame records,
4096 samples per frame, 262144 aggregate samples and 8 MiB. Audio Volume follows the pinned
[`Node_Audio_Loudness` source](https://github.com/Ttanasart-pt/Pixel-Composer/blob/b69eca232217360cf1502ef0223523d818606652/scripts/node_audio_loudness/node_audio_loudness.gml):
`sqrt(sum(sample²)/N)` followed by `10*log10(RMS)`, with empty input returning 0. The runner accepts
`--audio-capture` and `--value`; Studio loads or clears a capture and previews scalar outputs. Missing source or
tick is a diagnostic. Nonempty silence returns `InvalidValue` because the source formula produces negative
infinity, which the native scalar contract cannot represent. This is source-derived and covered by deterministic
replay fixtures, not checked against a licensed executable. The native `image.audio_window` slice takes a bounded
scalar array with Width, static sample-index Location, Start/Middle/End cursor placement and positive integral Step;
it returns the sampled array in deterministic order. `image.audio_recording` also emits typed mono audio from a v2
capture. With that input and declared timeline FPS, Match Timeline uses the source formula
`tick / frames_per_second * sample_rate`; it otherwise returns a named diagnostic. Static seconds and progress
locations, multichannel audio, WAV import, audio-driven keyframes, live device capture and feedback history remain
open.

### M4: VFX, simulation and live engine bindings

1. Implement all documented VFX, rigid and fluid simulation rows after state and resource contracts are proved.
2. Feed generated texture names into existing effects systems and future effect graph work.
3. Add owner-scoped live texture registry, last-good fallback and all 2D consumer bindings, including particles,
   beams/trails and GUI images. Material maps, shader inputs, skyboxes and other 3D consumer bindings are completed
   and gated in M5.
4. Match fixed seed, fixed tick, reset and long-run drift fixtures.

### M5: complete 3D parity

1. Implement documented scenes, cameras, lights, meshes, materials, ray marching, render nodes and output nodes.
2. Define typed render capabilities and explicit resource lifetime for each 3D node.
3. Complete material-map, shader-input and skybox sink bindings in the engine-output matrix.
4. Reuse cooked shaders and declared sampler/resource inputs. Add a validated named-sampler binding extension before
   arbitrary composer output may bind a cooked shader sampler. It enumerates allowed sampler names, expected
   dimensions, format, colour space and owner lifetime. HLSL and shader-language composer nodes compile during
   authoring or cook into validated binaries; shipped clients consume those binaries and carry no runtime compiler.
5. Compare controlled captures, output structures and diagnostics at fixed backend and resolution.

#### First bounded node: Transform Image 3D

The documented [`Node_3D_Transform_Image`](https://docs.pixel-composer.com/nodes/_index/node_3d_transform_image.html)
is a candidate for the first render-owned 3D operation. Its inputs are `Position: vec3 [0,0,0]`, `Anchor: vec3
[0,0,0]`, `Rotation: quaternion [0,0,0,1]`, `Scale: vec3 [1,1,1]`, required `Surface: surface`, optional
`Back Surface: surface` (falls back to `Surface`), `Texture Tiling: vec2 [1,1]`, `Projection: enum {Perspective,
Orthographic}` (default Orthographic), `Fov: float 45`, `View Range: vec2 [0.001,10]`, and `Depth Range: vec2
[0,1]`. It returns `Mesh: d3mesh`, `Rendered: surface`, and `Depth: surface`; `Mesh` is hidden in the output UI
by default, and the two image outputs use the input `Surface` dimensions. The source creates a fixed plane and
returns no output for a missing front surface.

The pinned [node source](https://github.com/Ttanasart-pt/Pixel-Composer/blob/b69eca232217360cf1502ef0223523d818606652/scripts/node_3d_transform_image/node_3d_transform_image.gml),
[base transform](https://github.com/Ttanasart-pt/Pixel-Composer/blob/b69eca232217360cf1502ef0223523d818606652/scripts/__node_3d_object/__node_3d_object.gml),
and [depth shader](https://github.com/Ttanasart-pt/Pixel-Composer/blob/b69eca232217360cf1502ef0223523d818606652/shaders/sh_d3d_3d_transform/sh_d3d_3d_transform.fsh)
back these defaults and behavior. The shader computes `zNdc = gl_Position.z / gl_Position.w` and writes grayscale
depth as `1 - ((zNdc - DepthRange.x) / (DepthRange.y - DepthRange.x))`, with alpha 1. It does not clamp the value
or guard equal range endpoints; backend NDC behavior and invalid-range policy still need native fixture decisions.

The device-free imagegraph schema now has pure `Vector3`, `Quaternion`, `Enum` and opaque `Mesh` types. Render owns
the SDL GPU implementation. `ImageGraphTransform3D` uploads front and optional back RGBA8 inputs, creates the fixed
plane and graphics pipeline, renders RGBA8 color and encoded-depth targets with a D32 depth target, submits one
fenced command buffer, reads all three results, and releases every resource. Its public adapter exposes typed CPU
surfaces, the fixed plane mesh, rendered bytes, encoded depth and float depth without SDL types. A preflight limit
accounts for five textures and five full-size transfer buffers, including the fallback back surface, before GPU
allocation.

The first native Vulkan fixtures cover a distinct 2x2 back surface, fallback front surface, orthographic and
perspective projection, nonzero anchor with asymmetric rotation and scale, encoded and D32 depth, repeated
submission, invalid input before allocation, and resource tracking before and after cleanup. The client adds an
explicit headless export route that evaluates upstream CPU image nodes and calls the render adapter. Its authored
2x2 graph produces exact repeated RGBA `[11,22,33,255]`, publishes the result through the ordinary owner-scoped live
image publisher, and checks the typed mesh result. The live `ImageGraphRuntime` refuses this node with a named
headless-scheduler diagnostic, so it never performs a synchronous GPU wait or readback in a frame and preserves its
last good published generation.

Remaining work is a render-frame scheduler that keeps Transform Image 3D outputs resident and supports animated
controls without synchronous readback. The current imagegraph host-output model selects images only, so an authored
mesh consumer still needs a typed graph result route. Controlled reference Pixel Composer captures are also still
required for executable parity, including exact rasterization, culling, filtering and depth behavior across backends.

### M6: I/O, scripting, PXC and export parity

1. Implement native image, video, sequence and export rows through bounded host adapters.
2. Implement file, network and shell rows as explicit capability nodes with permissions, recorded input or runtime
   refusal. They are never ambient evaluator power.
3. Implement Lua/expression execution with bounded APIs and replayable inputs. Implement HLSL and shader-language
   nodes through authoring/cook compilation into validated binaries, with no shipped runtime compiler.
4. Finish native PXC read/write through the required format gate.

The native headless runner now has a bounded PNG frame-bundle export path. `imagegraph --input source.graph
--output-id final --frames 0:2 --bundle output-directory` evaluates explicit ticks and publishes one new
directory containing numbered straight-alpha RGBA8 PNGs and a deterministic manifest of tick, dimensions and
pixel hash. A failed later frame removes the staging directory; an existing destination is preserved. The runner
keeps the 4,096-frame and 512 MiB range bounds. Focused runner tests passed 105 assertions in 17 cases, and the
bundle, frame-range and repeat CLI tests passed. This completes a native PNG sequence export slice. Pixel Composer's
`Node_Export`, other formats, animation export behavior and executable comparison remain open.

### M7: parity release gate

1. Every documented node-matrix, product-workflow-matrix and engine-output sink row is implemented and passing against
   the pinned documentation snapshot and native fixtures. A policy prohibition is a release blocker until the user
   accepts the corresponding feature exception.
2. Run native fixtures and reviewed visual studies against the supplied media. Keep executable comparison as a
   separate open gate until a licensed reference build and controlled same-input captures exist.
3. Profile static 2D, animated, feedback, simulation and 3D graphs in release.
4. Run Studio and client scenes and preserve release evidence.

The implemented M7 animation slice bakes unequal-delay GIFs to v5 `.atex` without averaging delays, while
fixed-rate assets retain their v4 byte layout and v1 through v4 readers remain supported. CPU tests cover
particle loop, ping-pong, one-shot and explicit rate playback. A headless Vulkan test bakes a three-frame GIF,
reads the asset, and checks particle cell IDs and sampled colours through wrap; another reaches frame 64 of a
65-frame fixed-rate atlas. Exact timing for authored imagegraph sequences, variable-duration playback outside
particle and named texture consumers, and atlases beyond 256 cells remain open. These tests do not establish
full Pixel Composer node or executable parity.

The separate `.aseq` asset contract now retains 257 to 4096 flat, equal-size RGBA8 frames in authored order,
with an individual positive duration for each frame and a 256 MiB decoded pixel limit. Assetc emits `.aseq`
for GIFs over 256 frames, preserving their normalized hundredth-second delays, and for imagegraph ImageArray
outputs over 256 frames when an explicit `--flipbook-fps` is supplied. A 257-frame GIF and ImageArray both
round-trip through the cooked format in CPU tests. Short GIFs and arrays keep the existing `.atex` atlas route.
The `.aseq` payload is not yet a particle or other render consumer; sequence residency and sampling remain open.

## Engine output integration

| Sink | Existing seam | Acceptance |
|---|---|---|
| Particle image | effects ParticleEmitter Texture | Generated still reaches standard particle collection and last-good output remains on error. |
| Particle flipbook | ParticleEmitter Flipbook, FlipbookFrames and FlipbookFramerate | Generated atlas plays declared cells at fixed or per-frame timing through 256 cells. Larger sequences still require a separate playback path. |
| Beam and trail | Existing effects texture fields | Live name resolves through ordinary texture demand. |
| Material maps | scene MaterialMaps PBR names | Colour and linear map semantics validate and draw through normal material resolution. |
| GUI images | ImageLabel Image and ImageButton normal/hover/pressed picture properties | Every GUI state uses normal interface texture resolution. |
| Shader inputs | Existing material, interface and declared render sampler contracts | A cooked shader samples generated data only after validated named-sampler binding admission, with no runtime shader compilation. |
| Skybox | scene SkyboxTextures Front, Back, Left, Right, Up and Down | Six coherent names update or the prior sky remains. |
| Static content | TextureData and normal asset cook/content | Export/reload preserves pixels, colour-space, mip and flipbook facts. |

## PXC read/write parity gate

Current fact: all five supplied PXC files use PXCX framing, a zlib thumbnail, a metadata block, and a zlib JSON
graph. Their graph records contain 67, 32, 40, 27, and 44 nodes, with link references that resolve within each
file. The bounded native reader accepts all five, and the native writer reproduces unchanged archives byte for
byte. Synthetic tests cover recomposed metadata, thumbnail and graph streams. Import coverage beyond these
samples and compatibility of recomposed files with Pixel Composer are not established.
The optional native acceptance case, enabled with `PXCX_EXTERNAL_FIXTURE_DIR`, passed all five supplied projects
through `ReadPxcx` and byte-exact `WritePxcx` with 55 assertions. The normal suite does not require those external
files.
Native PXC read and write are explicit parity work and release gates.

1. Obtain an authoritative PXC format specification or a validated format description.
2. Verify PXCX framing, versions, compression, graph records, values, links, resources, timeline and export against
   controlled fixtures.
3. Write bounded parser and writer tests for valid, malformed, unknown-version and unsupported-node documents, and add
   a PXC parser fuzz target with bounded input size and execution time.
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

Documented feature completion requires architecture checks, all node and product-workflow matrix suites, native
headless runner, static cook/reload, PXC round-trip and render studies on all five supplied files, release profiles
for all graph classes, and manual Studio/client inspection of every scene. Exact executable parity remains unverified
without the official build; native fixture hashes establish native repeatability only.

## Risks

| Risk | Control |
|---|---|
| Parity becomes a small starter set | M0 has node and product-workflow matrices, and M7 gates both at row level. |
| PXC behavior or format is undocumented | Keep PXC read/write gate open until verified. |
| Async work breaks tick ordering | Match result generation/tick and use synchronous fixed-step state when gameplay-visible. |
| Dynamic images leak memory | Separate document/evaluator/render ownership with budgets and retirement tests. |
| Each sink grows a private path | Publish ordinary owner-scoped names and use existing resolution. |
| Scripting or shader nodes bypass policy | Use explicit capabilities, bounded APIs and cooked shaders. |
