# render refactor plan

## 0. scope and review status

Full plan for review. Engine implementation starts after review. Nothing marked
planned below is a claim that the feature ships. Baseline inspected at
`b61c564c`; future implementation must recheck the relevant code and tests.

This document consolidates `RENDER_PIPELINE.md`,
`future-work/OPTIMISATIONS_RENDER.md`, and
`future-work/materials-and-shaders.md`. Their requirements belong here now.
[TORNADOSIM.md](TORNADOSIM.md) remains the gameplay demo plan; its render needs
are included here. The attached checklist and [ROADMAP.md](../ROADMAP.md)
v0.24 define the requested work, even where older notes called it future work.

Review covers the whole destination, the phase order, and the explicit policy
changes below. Completing this document does not complete the renderer.
Each implementation phase must leave a working engine and evidence for its gate.

### decisions that need review

| Decision | Proposed resolution | Consequence |
|---|---|---|
| Live compiler versus cooked shaders | Adopt the materials plan: cook published modules; Studio previews use the same compiler through jobs; shipped clients execute published modules only | This changes the current render module policy requiring runtime shaderc. Update that policy with human sign-off before removing the client compiler. This planning change does not silently override it. |
| Everything in nodes | All render work, including residency, uploads, simulation, captures, UI composition and presentation, has graph ownership and declared effects | Device helpers remain ordinary functions called by nodes. A helper is not a second scheduler. |
| GPU state versus ECS ownership | ECS owns authored values and simulation transforms; GPU owns derived visual state and caches | Per-instance capability flags, visual displacement, visibility and LOD choices are GPU-readable. There is no CPU mirror of GPU decisions in the frame path. |
| Parallel scenes | Collect all active scene/camera requests, prepare immutable slices in parallel, join, then record and submit on the render owner thread | Keep world affinity and deterministic tick barriers. Multiple cameras share world residency. |
| Ray/path tracing | Ship compute-based tracing nodes on the supported compute baseline; add native hardware paths only behind measured, probed support | A catalogue label, SSR effect, or fallback image does not count as a tracer. |
| Visual compositor and material authoring | Extend the existing Studio canvas and typed engine documents; material graphs compile through the shader cook | No second canvas runtime or special preview renderer. Older exclusions of visual shaders and tracing do not remove these requested features. |
| Quantization | Support both compact stored formats and explicit value rounding through editable-content policy components | Merely rounding float32 values is not a memory reduction. Native storage, arithmetic and sampling support are separate capabilities. |
| Portals | Finish same-world and cross-world visual/physical seams under explicit ownership and bounded recursion | Visual proxy geometry cannot become a second authoritative body. |

Protected `AGENTS.md` files are unchanged during planning. Historical mentions
of `RENDER_PIPELINE.md` in policy, old roadmap entries and source comments refer
to the predecessor of this document. Implementation phase P0 must reconcile
those references and outdated policy statements through the approved exception.

## 1. requirements and proof map

| ID | Requested result | Detailed plan | Required proof |
|---|---|---|---|
| R01 | Easy, thorough per-step image and projection tests | §5 | Real offscreen GPU captures, mathematical oracles, per-pass failure artifacts |
| R02 | Seamless portal light, physics, projection, clipping and crossing geometry; visible demo | §12, §19 | Automated crossing sequences, body ownership tests, inspected portal demo |
| R03 | Per-mesh, world-lighting and camera capability controls | §6 | Inheritance/mask tests and GPU branching without readback |
| R04 | Remove rendering outside nodes; residency and delta upload in same refactor | §3, §4, §20 | One executor, zero hidden frame work, actual zero-traffic reuse |
| R05 | Semi-real raytrace and path-trace nodes | §11 | Real intersections, secondary visibility, convergence and reset tests |
| R06 | Tracing example pipelines | §19 | Named hybrid and progressive pipelines, captures and profiles |
| R07 | Attach compute/post nodes to every visual item; active shader residency | §6, §8 | Coverage matrix for visual families, deterministic placement, inactive no-work checks |
| R08 | Dynamic AO, emissivity, mipmaps, conservative occlusion, sRGB, PBR, tessellation, environment compute, GI, displacement | §7, §9 to §14 | Feature-specific numeric, image and lifetime tests |
| R09 | Blender-like workflows and nodes for all features | §8, §19 | Material and compositor authoring, groups, previews, save/undo/cook round trips |
| R10 | Unity-like visual compositor system | §3, §8 | One typed authoring-to-runtime path with inspectable intermediate outputs |
| R11 | All active scenes, entity lists, resident update, batched cameras; parallel/vectorized prep | §4 | Serial/parallel parity and upload counts independent of camera count |
| R12 | Editable mesh/texture packing, quantization component, many 4K textures, atlas | §13, §14 | Actual byte reductions, decode error bounds, pressure and bleed tests |
| R13 | Different AA choices as nodes | §10 | None, MSAA, FXAA, SMAA, TAA and temporal upscale with honest tier support |
| R14 | Four authored LOD meshes, automatic decimation, projected-triangle/meshlet path | §13 | GPU view-specific selection, seam/error tests, stream fallback and no CPU round trip |
| R15 | Port all material/shader contracts | §7, §15 | Author, save, script, cook, publish, load, reload, fallback and retire |
| R16 | Preserve all optimization research and TornadoSim needs | §9 to §18, §22 | Every old subsection maps to an owned phase or measured candidate |

## 2. current foundation and gaps

Current code is evidence; old prose is context. Do not inherit obsolete numbers
or a checked box without inspecting its implementation and consumer.

| Area | Evidence inspected or located | How to use it |
|---|---|---|
| Graph authoring | `mono.engine/graph/src/PipelineDocument.cpp`, `PipelineCatalogue.cpp`, `NodeSchema.cpp`; Studio `RenderPipelineGraph.cpp` | Keep ordered documents, shared declarations, typed links and round-trip tests. |
| Node handlers | `mono.engine/render/src/nodes/{Upload,Geometry,Shading,Shadow,Portal,Mirror,Authored,Output}Nodes.cpp` | Existing family split is the migration seam. |
| Upload wrapper | `UploadNodes.cpp` registers CPU finish spans and `upload-instances`; CPU work currently happens in `ViewRecording::Begin` | Make scheduling control real execution, not only retrospective labels. |
| Instance packing | `InstancePacking.hpp::GpuInstance` has position, snorm16 quaternion, scale, colour, appearance, surface colour and emission | Current row is 48 bytes, not the 40 bytes in older optimization notes. Extend/version based on measured layout needs. |
| World instance residency | `InstanceResidency.hpp`, corresponding tests | Reuse stable slots, generations, dirty spans and acknowledgements. |
| View ordering | `IndexResidency.hpp` holds three in-flight versions and pending acknowledgement | Preserve per-view whitelists and failure retry; do not upload shared rows for each camera. |
| Batched renderer | `Renderer.hpp` and `Renderer.cpp` expose a span of `View` values | Product collection remains work. Studio `Editor::PresentWorld` still builds a round-robin candidate list. |
| Hidden mesh transfer | Batched `Renderer::Render` calls `State->Meshes.Flush()` before grouping worlds/views | Move this transfer into graph-owned residency work with the same batching behavior. |
| Presentation damage | `WorldPresentation.cpp::ScenePresentationSignaturesOf`, `PresentationDamage.hpp` | Keep objects, particles, environment and portals separate; UI and diagnostics have separate damage. |
| Editable content | `scene/EditableMesh.hpp`, existing revision consumers | Reuse world-owned authoring data, mutation revisions and collision rebuild boundaries. |
| Materials | Legacy AMT1 versions and seven-map material records described by the material plan | Verify each map's current consumers before conversion; translate old content into one schema. |
| Device tests | [RUNNING.md](../RUNNING.md) documents `[gpu]`, `--gpu-tests`, `Renderer::Initialise(nullptr)` and capture | Extend the real-device harness. Ordinary headless runs do not prove pixels. |
| Tracing | `PipelineCatalogue.cpp` contains a `raytrace` declaration; search found no corresponding tracer in inspected render node families | Inventory any previous implementation/history before porting. Backend and actual image proof remain required. |

The old render plan records registration, capability checks, custom native
handlers, profiling, compiler optimization/reflection, command-buffer grouping,
transient aliasing and tiered defaults as implemented. P0 must verify those
claims with current consumers and suites; this plan does not rerun them yet.

### baseline frame and data

The predecessor's default order is `world`, optional `shadow`, `camera`,
`last-frame`, `entities`, `cull-frustum`, `order-draw`, `upload-instances`,
optional `mirror-capture`, `portal-capture`, `portal-tonemap`, `gbuffer`,
`depth-linearise`, optional `ssao`, `deferred-lighting`, `sky`, `tonemap`,
`portal-overlay`, `mirror-overlay`, `transparent`, `present`, optional
`interface`, final `overlay`, final `output-image`. Preserve its reference
images during extraction; §10 and §12 then deliberately correct the colour
and portal composition order with new proofs.

Resident state includes mesh/index buffers, packed instances, draw-order streams,
source textures, seven material maps, shaders/pipelines, particle pools, retained
game-GUI geometry and glyph atlases, ImGui buffers, and shadow/surface/history
targets. Revisions gate editable content; signatures gate retained geometry;
dirty ranges gate instance/mesh data; overlay uploads use a dirty rectangle.
Lights/cameras upload small structured values; ribbons upload compact geometry.

No normal frame uploads a CPU-composed GUI or scene image. Full pixel uploads
are source textures or explicit editable images. Readback is for requested
captures, tests and completed timing queries, not per-frame visual decisions.

## 3. one graph from authoring to frame

### ownership and layers

| Owner | Responsibility | Boundary |
|---|---|---|
| `scene`, ECS and world | Authored components, simulation transforms, revisions, immutable published views | No device handles; cross-world values are copies with stable names |
| `graph`, L9 shared | Documents, node schemas, pure selection math, resource contracts, validation, schedule, lifetimes, diagnostics | No SDL, shaderc or device objects |
| `render`, L12 client | Device probes, resident caches, node backends, command recording, GPU allocation and retirement | No second world registry or gameplay state |
| `resources`, L11 client | Built-in shader names/source and staging | Consumers use `resources::Shader(name)` |
| `msl`, L11 client | Existing SPIR-V-to-MSL translation | One translation rule, entry and binding checks |
| `assets` and delivery | Bounded cooked containers, content identity, signed manifest and verified bytes | No material policy or shader compilation in CDN |
| `bake`, bake graph and tools | Foreign imports, decimation, packing, shader/material cooking and reports | No runtime engine dependency on source importers |
| Proposed shading libraries | Pure shader schemas/records separately from source compiler/optimizer/reflection worker | Determine legal layers before adding targets; runtime reader cannot pull compiler transitively |
| `mono.studio/nodegraph` | Existing generic canvas, layout, model and serialization | No engine render semantics in canvas |
| Studio/client | Active panels/cameras, frame requests, editor documents and authoring jobs | Engine systems stay under `mono.engine` |

The materials plan proposed one `Engine::shading` library. Split its reader and
compiler targets if needed: a shared runtime schema cannot link client-only MSL
or drag shaderc into the shipped reader. Add exact layers/edges to
`expected_graph.json`, test server-only configuration, and do not create
`mono.engine/renderer` or widen a tier escape.

### document and compiler contract

Keep `PipelineDocument` ordered edits and the existing `renderpipeline 2` reader.
A versioned extension must preserve old inputs through one conversion reader.
Names, parameter keys, feature keys, resource names and node kinds serialize as
strings; document positions are authoring metadata, not execution hashes.

The chain remains document -> `Build` -> `RenderGraph` -> `Compile` ->
`CompiledGraph` -> `CompileSchedule` -> command-buffer plan -> backend executor.
Compile on edits/installation, not per view per frame. Publication lowers
authoring conveniences to cooked module and resource references.

Declaration order defines effects and each resource version. Reads/writes
validate that order; dependency waves expose independent work without changing
visible composition. Multi-writer resources use versioned writes; history reads
refer to the prior successful frame explicitly, never an implicit cycle.

Scopes remain `Frame`, `World`, `View`; Final is an execution partition, not a
fourth authored scope. Reject world/shared work interleaved inside a view block
when it cannot retain its semantics. A world node runs once per distinct world
and matching resource key, not once per camera that happens to see that world.

Unconnected optional entity inputs mean empty; missing required ports fail
validation. Camera fallback to the current view remains explicit in schema.
Disabled nodes leave the schedule; unused pure nodes can be removed by backward
reachability, but simulation, uploads, capture and present declare side effects
and survive if their effects are demanded.

### resource contracts

Preserve `Colour`, `Depth`, `Texture`, `Storage`, `Buffer`, `Camera`, `Entities`.
Extend descriptors with access, sample count, array/depth extent, mip range,
colour/alpha space, buffer stride, lifetime, owner and history generation.
Volume density, light lists, acceleration structures and visibility masks need
typed schemas over these resources, not unexplained buffer numbers.

Keep existing formats: R8, RG8, RGBA8, RGBA8_SRGB, RGB10A2, RG11B10F,
R16F/RG16F/RGBA16F, R32F/RG32F, D24S8/D32F and BC arrival formats.
Add formats only with capability, byte accounting, sampler/storage legality,
conversion and backend tests. Resource kind and pixel format remain separate.

Texture inputs may accept sampled colour/depth/storage outputs when usage allows;
a sampled asset is not automatically a render target. Validate sample type,
integer/float/depth meaning, dimensions and access, not only channel count.
Implicit narrowing reports `LossyWire`; explicit conversion names its target
and suppresses only the narrowing it deliberately performs.

Aliasing uses strict nonoverlapping lifetimes and exact compatible descriptors,
scope and owner. External assets, swapchain and retained history do not alias.
Derive RAW/WAR/WAW and alias transitions; do not recycle targets while pending
GPU work, another view or an exported capture still references them.

### registration and backend contract

One `NodeKindSpec` declares kind, label, summary, category, scope, queue,
parameters, ports, defaults, requirements, fallback and backend support.
Init-only idempotent registration preserves pointer stability; custom names
are namespaced strings. Registry drives Studio widgets, compile checks and
backend acceptance, including headless CPU-only kinds.

Keep authored `raster`/`dispatch`, reusable canvas groups and native kinds.
Groups flatten through one compiler with source-node mapping for errors.
Native handlers need a small command interface for attachments, bindings,
draw/dispatch, copies, barriers and timestamps; SDL stays in its adapter.
Do not promise a second backend from an interface alone.

Installation validates every enabled node, reflected bindings, formats,
capabilities, scope, queue and fallback before admission. Unknown kinds survive
authoring round trips visibly unresolved; runtime refuses with node and reason.
Install replacements atomically; preserve the last accepted pipeline on failure.
Release targets, caches, history, capture receipts and custom handler state by
pipeline/world lifecycle after in-flight use ends.

Command-buffer fusion merges legal adjacent submission units, not shader source.
Every node keeps its name, resource effects and timing attribution. SDL's unified
queue can serialize transfer, compute and graphics; scheduling eligibility does
not prove actual GPU overlap.

## 4. resident worlds and parallel frame preparation

### execution chain

```text
owner-barrier world snapshots + active output requests
  -> collect distinct worlds and all demanded cameras
  -> parallel immutable presentation extraction, per-world output slices
  -> join and canonical merge
  -> residency-plan / upload-deltas                 [world]
  -> particle-step / material-resolve / visual-derive [world]
  -> camera setup / conservative cull / LOD / indirect lists [view]
  -> shared and view shading / portal capture / post / composition
  -> output / capture / present
  -> successful-write acknowledgement and frame retirement
```

The product collector includes active game cameras, visible Studio viewports,
requested asset/material previews, surface/portal/mirror cameras and explicit
offscreen outputs. Hidden/inactive docks request nothing; paused worlds may
still need a redraw after edits. Minimized windows do not acquire a swapchain,
but an explicit offscreen capture remains a valid request.

Each request has world identity/generation, view identity/generation, camera,
output extent, pipeline, quality, source signatures, history identity and reason
for activation. Deduplicate world work; do not deduplicate distinct cameras or
eye-specific history merely because their current matrices match.

Capture world-owned immutable data at an owner barrier or through existing
presentation channels. Workers never call `Universe::Enter` on foreign threads,
mutate ECS, issue SDL calls or hold borrowed state across owner mutation.
Each worker writes a disjoint pre-sized slice; join and merge in stable
world/entity/view order before the render thread records.

Batch related columns and compact hot records; use prefix sums for output offsets
and vectorizable cull/pack loops. Reuse scratch capacity. Measure serial versus
parallel grain in release at 0, 1, small and large worlds/camera counts; write
the crossover beside the implementation rather than assuming parallel wins.

### stable residency and delta nodes

World resident identity includes world generation; entity slot identity includes
entity generation. Deletion invalidates membership and retires referenced ranges
after fences; reuse cannot display a previous entity's visual state.
World destruction releases particle pools, instance tables, history and bindings.

Keep authored transforms and compact input records separate from GPU-produced
visual fields. Proposed GPU rows/sidecar columns carry capability bits, material
slot, visual effect range, bounds metadata and deformation state. Layout is
private, versioned, aligned, byte-tested and decoded by matching shaders.

View-specific visibility, selected LOD, motion history and indirect counts live
in GPU per-view buffers keyed by stable slots. A single mutable LOD field in a
world row would race cameras and shadows. Shared rows carry LOD policy/level
references; each view stores its own result beside its occlusion output.

Residency planning computes dirty ranges from existing mutation/revision feeds.
Upload nodes coalesce only changed spans and update each in-flight version that
needs them. CPU authored/staging inputs are allowed; a continuously synchronized
CPU copy of GPU visibility, displacement or particle state is not.

Publish acknowledged revisions only after the required upload/render succeeds.
Failed acquire/submit keeps damage pending. Capacity growth copies old resident
data on device where supported; measure separately from ordinary delta bytes.
One camera turning must not repack/reupload every shared instance.

### retained presentation

Object, particle, environment and portal source changes invalidate scene pixels;
scene changes cascade to game composition, host composition and final image.
Game/host GUI changes begin at their own layer, never invalidate sibling source
geometry. An absent layer is `n/a`, not a cache hit.

Keep retained game-GUI vertices, glyph atlas and ImGui draw geometry. Diagnostics
use their own refresh deadline and dirty rectangle; an updating counter cannot
force scene, texture or material cache misses. A cache hit owes zero work for
that layer: no upload, no command buffer and no transient allocation.

Time-varying nodes declare time as input. A static sky may cache; moving clouds,
particle simulation, jittered temporal convergence and a visible video cannot
hide behind unchanged entity signatures. Freeze capture time explicitly.
Portal/history baselines advance only when those outputs were actually written.

Tests cover scene reorder, entity deletion/reuse, empty lists, failed submit,
world unload/reload, camera create/remove, same-world many-view reuse and foreign
portal views. Check exact transfer ranges and traffic, not only a hit counter.

## 5. render correctness harness

### fixtures and oracles

Extend existing `[gpu]` tests using a real offscreen device. Each case declares
scene/world state, camera matrices, dimensions, graph, device tier, random seed,
tick/sample count, exposure and requested intermediate resources. A capture
manifest records backend/GPU/driver, build revision, shader hashes and settings.

Provide one proposed `just render-check` entry with suite/filter/backend options,
wrapping the existing runner rather than inventing another test registry.
It must fail if a requested device cannot initialize. Ordinary CPU tests remain
device-free; missing hardware is reported as unverified, never image success.

Use three independent oracles: analytic geometry/pixel probes, CPU reference
math, and reviewed image baselines. A screenshot copied from broken output is
not an oracle. Use full-image errors plus region masks and explicit pixel
assertions, so a tiny missing portal is not hidden by a mostly black image.

| Test family | Inputs and observables | Failure it must catch |
|---|---|---|
| Projection | Axis triad, checker grid, known clip-space points, perspective/orthographic, oblique and mirrored cameras | Wrong handedness, upside-down image, half-pixel offset, aspect or near/far error |
| Depth | Ordered planes, near crossing, sky, linear-depth and HZB captures | Wrong depth convention, reduction direction or occlusion epsilon |
| Geometry | Winding, nonuniform/negative/zero scales, skinning, quantized mesh, displaced mesh | Culling, normal transform, bounds and decode disagreement |
| Resources | Read/write chain, alias pressure, viewport resize, history, pipeline replacement | Stale pixels, feedback, wrong owner, read-before-write and premature reuse |
| Colour | Linear ramps, known sRGB swatches, alpha wedges, HDR emitter and tonemap | Double conversion, gamma blending, clipped emission or premultiplication error |
| PBR | Roughness/metalness grid, each map, normal orientation, fixed light and environment | Broken BRDF, channel packing, missing map or inconsistent techniques |
| Selection | Per-object mask, per-camera feature override, multiple worlds | State leakage, widened cull set or incorrect capability merge |
| Temporal | Fixed seeded sequence, moving object/camera, resize/cut/portal crossing | Ghosting, stale history, wrong velocity or jitter reuse |
| Portals | Matched direct-view/through-portal geometry and scripted crossing | Seam holes, double body, clipping/light discontinuity |
| Lifetime | Repeated edit/reinstall/unload, failed admission, held frame | Leaks, invalid handles, loss of last valid image |

Pin the existing zero-to-one clip depth and Y-up convention. Derive frusta from
the exact matrix used for rendering; do not negate projection Y independently
of SDL viewport handling. Include odd and non-square sizes, 1x1 targets,
zero-area outputs and near-plane-straddling boxes.

Capture linear colour, depth, normal, material, velocity, entity ID, visibility,
LOD and final colour independently. Debug readbacks are opt-in and asynchronous;
fence completion, timeout and failure are explicit. Pad/read row strides and
format conversions correctly before comparing.

Comparison policies are fixture-specific: exact integer IDs and canonical bytes;
bounded world/pixel error for projection and packing; absolute/relative linear
colour tolerance; bounded outlier count and RMSE for raster images. Stochastic
tracers use fixed seeds, samples, variance/confidence criteria and convergence
checks. Set thresholds from expected precision and baseline repeatability,
not by widening until a failure passes.

On failure write input manifest, expected/actual/difference images, magnified
regions, numeric probes, graph/resource versions, validation errors and capture
receipts under the build output. Baseline changes are explicit reviewed edits,
never automatically accepted by CI. Test selection can be narrow locally;
release gates cover every supported backend and capability tier.

### verification tools and commands

Existing documented commands are `just test`, `just test-all`,
`just test --gpu-tests`, `just shader-check`, `just test-architecture`,
`just typecheck`, `just docs-check` and `just check-server-is-headless`.
`just edit --headless --frames 12 --run play --capture shot.bmp` is an existing
offscreen Studio capture path; it still uses a real GPU.

New render fixture and benchmark recipes are proposed, not commands available
today. Add them with the harness, document arguments in RUNNING, and keep all
benchmark outputs under the configured build tree. Do not create loose benchmark
report files in the repository.

During implementation, headless math tests run first. At final verification ask
for live Studio/browser/compute approval as the repository instructions require;
record any declined GPU/interactive checks. This document-only task runs none.

## 6. capabilities and attached effects

### separate support, policy and state

`DeviceCaps` describes backend support and limits; reflected
`ShaderCapabilities` describes a module's stage, resources, formats, workgroups,
minimum buffer bytes and requirements. Authored render policy is a third thing,
not another driver capability record.

Define world-lighting defaults, camera overrides and per-instance visual policy.
Use tri-state inherit/enable/disable fields for authoring, with stable names at
save/VM boundaries and packed bits in GPU rows. Resolve effective enablement as
authored policy constrained by pass support, device support and selected tier.
Neither a camera nor an instance can enable an unsupported device feature.

Keep explicit default capability documents: Tier A uses deferred HDR, compute
HZB and optional AO; Tier B removes compute-only work and uses conservative CPU
selection plus supported raster effects; Tier C uses supported LDR/forward
formats while retaining bounded portal/mirror captures. Validate actual device
limits rather than treating tiers as a total hardware ranking. Numeric quality
uses parameters/divisors; fallback substitutions revalidate the whole document.

Specify each feature's merge rule instead of one arbitrary bitwise OR. Examples:
cast/receive shadows, AO participation, lighting channels, emissive contribution,
reflection/refraction visibility, motion vectors, tracing visibility, culling
eligibility, two-sidedness, displacement and post selection.

Camera-local visibility and quality apply only to that view. World shadow or GI
work shares results only if participating cameras request compatible inputs;
otherwise key distinct shared resources or make the work per-view. Publish the
resolved policy and refusal reason in Studio without reading back every row.

### one attachment model

An attachment contains stable graph/module reference, named entry/technique,
execution slot, selection mask, typed parameters, order, enabled state and
revision. Store authored attachments in ECS; pack active ranges/references into
GPU resident instance/emitter/environment/UI records. Shared modules compile
and reside once; instances change data, not shader variants.

| Visual consumer | Allowed effect stages | Selection and bounds |
|---|---|---|
| Parts, mesh parts, editable/skinned meshes | Material shading, visual deformation compute, selected post | Stable instance/submesh ID; declared displacement envelope |
| Decals and surface textures | Material and selected composite | Surface mask and UV contract |
| Particles, beams and trails | Resident simulation/field compute, shading, selected post | Emitter/ribbon IDs; finite capacity and conservative bounds |
| Terrain/chunk output | Material, displacement and tile compute | Chunk/meshlet IDs and per-view LOD |
| Skybox, fog, atmosphere and clouds | Environment-generation compute, volume/render composite | World/camera scope and explicit output resources |
| Portal/mirror/surface camera | Capture graph and surface composite | View lineage, aperture mask, recursion and history identity |
| Game UI, text and image widgets | Declared UI material/composite | UI mask, premultiplied alpha, native-resolution text |
| Camera and global lighting | Fullscreen compute/post, lighting and GI graph | Explicit view/world scope and channel policy |

Effects on selected objects need an ID/stencil/mask input. A fullscreen post
cannot infer its owning object from attachment metadata. Define whether its
neighbourhood may sample outside the mask and how it composites at edges.
Group identical programs into batches; never issue one fullscreen pass per item.

Declare writes and bounded dispatch domain, time dependence, history use,
resource needs and invalidation. Compute deformation writes visual streams;
physics retains original geometry unless the author separately edits physical
geometry through its existing API. Recompute visual bounds or use a verified
conservative envelope before culling.

Active demand drives shader, texture, sampler and pipeline residency. Hidden
attachments produce no preview work and no new loads; in-flight resources retire
safely and caches may retain bounded reusable entries. Activation is deterministic
from scene/view policy, never from profiling counters.

## 7. materials and shader publication

### immutable definitions and sparse instances

Keep distinct stable identities for material asset, material instance, shader
module family, texture asset and sampler preset. Layout and interface signatures
describe compatibility; they are not authoring names. Asset rename is an explicit
publication move with old-reference handling, not a silent new identity.

A versioned material definition holds shader module and techniques, bounded
fallback references, canonical sorted declarations/defaults, texture roles,
samplers, static features, alpha/cutoff/two-sided/shadow policy, dependencies,
layout signature and optional labels/groups. Labels do not affect device layout.
Legacy AMT1 versions one through four and seven-map `.amat` records translate
into standard PBR parameters through one reader.

Techniques have stable names for opaque, alpha-test, transparent, shadow,
depth-only and velocity consumption, plus declared tracing techniques. A pass
asks for a technique; the material resolves it or its fallback. Keep default
white plastic for an unassigned part, a distinct missing-content marker for a
bad reference, and consistent alpha cutoff in visible and shadow passes.

A world material instance holds one published parent and sparse typed overrides,
plus authored revision. Published instance chains may have a small bounded depth;
resolve once, reject cycles/missing parents/unknown overrides/type mismatches,
flatten and cache by all parent roots plus override bytes/revision. No per-draw
inheritance walk and no copy of the immutable definition into every entity.

Parameters are bool, signed/unsigned integer, finite float, float2/3/4, explicitly
linear RGB/RGBA, declared matrices, typed texture reference, normalized sampler
and closed enum token. Names/counts/default/override bytes are bounded; numeric
ranges and steps are declared. Canonical little-endian encoding, unique names
and refusal of unknown active types are required.

Reflection supplies uniform offsets, sizes, alignment and descriptor bindings.
Validate its signature against the selected module before allocation; render
packs directly from canonical scene values. Large runtime arrays are separate
graph storage resources with their own schema/limits, not material parameters.

### texture and sampler contracts

Each binding declares semantic role, dimension (2D/array/cube/volume), channels,
sample type, colour space, swizzle, optionality, missing fallback, editable-image
permission and mip policy. ORM channel packing is explicit. Normalize normal
map handedness at cook where possible; do not infer it from filenames.

Samplers specify min/mag/mip filters, wrap, anisotropy, comparison and LOD range.
Normalize and deduplicate the complete descriptor per device, with a hard cap.
Linear and nearest become built-in presets. Unsupported settings follow declared
fallbacks with a reason, not a silent materially different clamp.

Editable-image revisions replace only the affected texture. Generated
`editable-image://` and `editable-mesh://` names remain local; publication bakes
ordinary assets and updates references transactionally. Pixel edits must not
compile shaders or invalidate unrelated uniform rows.

### modules, interfaces and variants

GLSL remains initial source. Metadata declares stage/entry, approved-root includes,
language version, static feature switches, specialization values, vertex or
fullscreen contract, fragment outputs, resources/access, material parameters,
requirements and fallback. Reject path escape, absolute/network includes,
symlink escape and excess include depth/expanded bytes; map diagnostics back to
original file and line.

Keep material source fragment-only until a versioned public vertex/deformation
contract exists. Vertex work added for tessellation/deformation uses that contract,
not private `GpuInstance` layout. Fragment, vertex and compute modules all enter
the same publication path.

Cooked bundles carry bounded variants, sorted feature keys, stage/entry,
optimized SPIR-V (initial existing target environment), supported MSL payload,
reflection, minimum binding sizes, capability requirements, optimization reports,
compiler/translator/ABI/container versions, dependency roots and payload hashes.
Prefer SPIR-V when the device supports both; carry backend form, file and entry
together. MSL uses `main0` where required and the existing SDL binding rules.

Cook only variants demanded by materials/pipeline profiles and their fallbacks,
not the Cartesian product. Static layout/feature changes may compile; numeric
values, colours, samplers and textures remain data by default. Bound variants,
features, total bytes and device pipelines; report which asset demanded each.
Duplicate feature keys producing different bytes are deterministic-cook errors.

Validate stage, set/binding, kind, access, dimension/format, local size and minimum
uniform/storage layout against the graph/material contract. Reflection instruction
mix (arithmetic, texture, memory, control) is a static estimate, not GPU cycles,
occupancy or runtime-sized resource bytes.

### cook and trust chain

Build an explicit sorted dependency DAG across material parents, textures, shader
includes/fallbacks, mesh submaterials, pipeline modules and capability profiles.
Reject cycles before work. Stages: discover -> bound/parse -> resolve -> demand
variants -> compile -> SPIR-V validate -> optimize -> reflect/match -> MSL translate
-> independent shadercheck -> serialize -> ordinary manifest -> sign root once.

Reuse existing BLAKE3-256 identity, deterministic chunks and signed manifest.
CDN serves bytes, holds no publisher signing key and interprets no shader policy.
Append new AssetKinds without renumbering existing kinds; unknown future kinds
retain the existing Unknown rule. Containers check magic/version, count, offsets,
overflow, strings, order and total bytes before allocation; no partial success.

Independent compile jobs receive immutable inputs and separate output slots.
Gather/read first, fork bounded jobs, join, then publish sorted results. Compiler
workers need cancellation, time/memory/output/process limits and atomic writes.
Studio authoring jobs may span frames outside simulation; world tick work may not.

Incremental keys include normalized source/include roots, stage/entry/features,
compiler/optimizer versions and flags, SPIR-V target, MSL translator/options,
interface/policy/ABI and capability profile. Verify cached outputs. Material
default edits avoid shader recompilation; texture pixel changes avoid unrelated
container or module rebuilds. Failed revisions cache diagnostics, never artifacts
that publication can mistake for valid output.

Preserve explicit optimization reports: constant folding/specialization
propagation/simplification/dead-result cleanup, then local/module common-subexpression
and redundancy elimination, cleanup and ID compaction. Record before/after
instruction counts and whether each stage found work; preserve the entry-point
interface and validate optimized output. Cache these results with the module,
not in a separate Studio compiler path.

Before migration, authored graph samplers follow read-slot order (set 2), pass
uniforms use the current GraphPassUniforms contract (set 3, binding 0), and compute
dispatch distinguishes explicit groups from cover-target dimensions. Freeze
these fixtures and translate them into named reflected interfaces; do not change
binding layout silently while replacing inline source with cooked references.

### runtime admission and caches

Selection order: requested technique/feature -> compatible cooked variants ->
preferred backend form -> authored technique/node fallback -> material fallback
-> visible missing marker plus bounded diagnostic. Chains are cycle/depth checked
at cook and load. Equal caps yield equal selection regardless of hash iteration.

Cache immutable parsed records by content root and reader version. Manifest swaps
create a resolution generation; old in-flight frames keep their consistent roots.
GPU keys include definition/parent roots, canonical overrides, technique/features,
layout/interface signatures, texture roots/editable revisions, sampler descriptors,
device identity/backend and packing ABI.

Shader keys add module root, stage/entry and variant; graphics pipelines add vertex
layout, formats, depth/blend/raster state, sample count and specializations;
compute keys include valid local sizes. Test each key field by varying it alone.
Do not rebuild shader objects merely because a texture-dependent material key changed.

Separate budgets for parsed material/shader bytes, textures, uniform slabs,
observable shader bytes, pipeline/sampler counts, pending uploads and deferred
release. Evict least-recently-used unreferenced entries; retain in-flight data
until retirement. Oversized artifacts refuse before emptying unrelated caches.
Reason metrics are bounded names, not asset-derived metric keys.

### Studio authoring and scripts

Material editor widgets derive from declarations. Preview standard meshes, lights,
backgrounds, exposure and capability tiers through the real pipeline. Shader editor
shows line errors, reflected interface, optimization deltas, variants, fallbacks
and active-versus-attempted revision. Undo/redo and debounced/manual preview submit
the same cook request; hidden tabs submit no preview work.

Requests carry immutable sources, dependency roots, profiles and revision.
Worker returns owned bytes/errors/timing/request identity; discard stale results.
Admit successful results on the render owner at a frame boundary and retire old
objects after GPU completion. Invalid source retains the last accepted preview.

Luau and JavaScript use the same scene methods for get/set/clear overrides,
typed declaration queries, texture assignment, sparse cloning, revisions and
diagnostic selection. Validate type/range/enum/dimension/authority before mutation;
invalid calls change nothing. Generate VM/docs metadata from one schema; generic
parameter names stay data. Cross-VM fixtures compare values and errors.

Studio/trusted authoring may edit ShaderScript source and request cook tickets.
Packaged games select published modules and bounded feature values; they cannot
submit source, arbitrary SPIR-V/MSL, includes or descriptor layouts. Studio saves
may preserve source; package cooking must resolve every runtime demand first.

## 8. compositor and visual shader workbench

Use the existing canvas, `render.pass.<kind>` types and typed wire families.
Keep resource bindings and hidden metadata losslessly through load/save; final
save validation runs Build/CompileSchedule and interface checks. Engine executes
the compiled graph, never the canvas evaluator.

Unity separates pipeline configuration assets from execution and exposes visual
compositor image, value, selection and organization nodes. Use that separation
and discoverable palette as references, while retaining this engine's scopes and
ordered resource versions. [SRP fundamentals](https://docs.unity3d.com/Manual/scriptable-render-pipeline-introduction.html),
[visual compositor nodes](https://docs.unity3d.com/Packages/com.unity.visual-compositor@0.27/manual/nodes.html).

Build Blender-like authoring conveniences over the same contracts: reusable
subgraphs with typed exposed sockets, material-node groups, reroutes, frames and
comments, searchable palette, duplicate/paste, undo/redo, node mute/bypass with
defined type behavior, channel inspection and a selected-output viewer.
No engine execution depends on canvas coordinates or folder layout.

| Node family | Required nodes and contracts |
|---|---|
| Inputs | World, camera, entity selection, light camera, texture/constant, history, scene depth/normal/velocity/ID |
| Selection | Frustum/distance/tag/channel filters, set union/intersection/difference, deterministic draw order |
| Preparation | Residency plan, delta upload, material resolve, particle step, deformation, bounds, LOD, indirect build |
| Capture/draw | Shadow/cascade/cube, depth, G-buffer, forward opaque/masked/transparent, portal/mirror, authored raster |
| Compute/light | Dispatch, light clusters, HZB/cull, AO, GI, environment generation, hybrid raytrace, path trace |
| Image math | Add/subtract/multiply/divide, min/max, mix/over, masks, threshold, clamp, separate/combine, explicit conversion |
| Spatial filters | Blur/bilateral, dilate/erode, edges, scale/crop/transform, reduce chain, mip generation, upscale |
| Post | Exposure/histogram, bloom, tone/grade/LUT, DOF, motion blur, SSR, AA, sharpen, palette, hatch |
| Outputs | Image, viewer, one-shot/sequence capture, game interface, host interface, diagnostic overlay, present |
| Material graph | Typed constants/parameters, texture sampling/swizzle, math, normal mapping, PBR/unlit/toon/emission closures |

Material graphs lower to bounded shader source and cook through §7, not to
independent draw-pass callbacks. Compositor nodes manipulate declared images,
buffers and selections. Keep these domains visibly distinct while sharing UI
primitives and parameter schemas.

Each shipped kind needs a working backend/default shader, schema, capability
requirements, fallback, lifetime/signature policy, test fixture and node help.
A catalogue entry alone is not completion. Preview requested outputs only;
retain unchanged previews and stop work when hidden. Show pending/error state
without an always-repainting spinner.

Inspector tools show input/output dimensions, colour/alpha space, resource version,
owner, lifetime, allocation alias, selected shader/tier, timings and capture.
Report missing writes, dead resources/nodes, wasted writes, disconnected nodes,
out-of-order effects, overspent formats, unused alpha and feedback. Pixel-dependent
constant-channel/uniform-target/shading-count diagnoses require explicit readback
or instrumentation; do not infer them from graph topology.

## 9. lighting, shadows and visibility

### PBR and indirect light

Standard PBR has linear base colour, metalness, roughness, tangent-space normal,
occlusion, height and emissive inputs with explicit defaults. Define a shared
BRDF contract for deferred, forward, shadow/depth alpha, velocity and tracer
techniques. Roughness remapping, normal normalization, tangent handedness,
two-sided behavior and emission units cannot differ silently between passes.

Use a documented microfacet specular model, diffuse energy allocation and bounded
roughness. Test reciprocity/finite output where applicable, a white-furnace
energy fixture, metal/dielectric endpoints, grazing angles and nonuniform scale.
HDR emissive radiance cannot remain limited by the legacy packed colour/strength
range; supply a versioned material value or HDR sidecar and test bright emission.

Dynamic AO gets a node taking depth, normals, camera and optional motion history.
Provide radius/bias/quality, bilateral filtering and depth-aware upsample. Apply
AO to intended indirect terms, not as an unexplained dark multiplier on emission
and all direct light. Bake AO remains a separate material input.

GI nodes expose irradiance/radiance, confidence, update policy and history.
Provide environment/probe lighting as baseline, a measured screen-space indirect
option with offscreen fallback, and trace-derived indirect light from §11.
Do not call SSAO or SSR full GI. Emissive GI must reach nearby receivers, not
only bloom around the emitter's own pixels.

### shadow work, all preserved candidates

| Work | Algorithm and initial experiment settings | Gate and tradeoff |
|---|---|---|
| Cascades | 1 to 4 directional levels; exponential far bounds `firstFar * pow(maxDistance/firstFar, i/(N-1))`, with N=1 handled separately; compare practical log/uniform blends; initial 10m/150m/2048 settings are tunable | Fit each camera slice in light space; expanded caster frusta retain offscreen casters; test split transitions and light rotation |
| Stable fit | Rotation-only light basis; sphere/body-or-far-diagonal extent, quantized scale and snapped light-space centre | Never assume an integer extent makes world texels powers of two. Prove stationary geometry has stable shadow pixels during camera motion |
| Cascade blend | Overlap band, initial 0.2 fraction; fade to lit beyond maximum | No visible boundaries; sample derivative-sensitive comparison operations safely before divergent weighting |
| Filter menu | Hardware 2x2 PCF; normalized bilinear-weighted 9-tap Gaussian; 8 rotated spiral taps; optional PCSS blocker search plus filter | Explicit comparison sampler support, temporal seed and light size; quality/per-pass timing capture |
| PCSS correctness | Compute blocker/receiver separation in declared light-space distance; derive penumbra under that convention | Do not copy the old sign-sensitive depth formula across normal/reverse depth; receiver/blocker fixtures pin contact and growing penumbra |
| Bias | Raster slope bias plus receiver-normal offset proportional to world texel width per cascade/resolution | Test acne, contact gaps and peter-panning separately; fit enough depth for casters behind the eye |
| Point shadows | Cube/6-layer array, 90-degree faces, near/far and depth convention explicit; tangent-basis filter directions | Cap active lights/faces and refresh work; test face seams and near singularities; reverse-Z is a capability/contract change, not an isolated shader edit |
| Update budget | Near dynamic levels every frame; stagger coarse refresh, bounded age, footprint-based invalidation | Publish last successfully rendered centres, not desired centres; moving sun/caster invalidation and failure retry |
| Arrays/atlas | Start fixed layers; optional shelf/bin-packed tiles with padding and shared viewport/scissor/sample/clear rectangle | Atlas only after measured layer waste; border filtering and tile reuse tests |

World-shared shadows can share scene/caster data, but camera-fitted cascades
need a camera-set key or per-view fit. Budget and cache keys include light state,
caster revisions, material alpha, visual deformation and portal lighting state.

### clustered lighting

Implement a `light-cluster` node used by deferred and transparent/forward passes.
Initial 16x9x24 grid and logarithmic depth slices are test settings, not fixed
quality. Store bounded per-cluster offsets/counts plus flat point/spot index lists;
validate packed-bit capacity before writing. Keep distant-light handling explicit
when cluster far distance is below camera far.

Frustum-cull lights first. Begin conservative sphere/AABB and cone/plane tests,
then compare tighter Z/Y/X sphere slice refinement and optional convex-volume
raster assignment. Tight assignment earns adoption only if reduced shaded lights
repay extra ALU. Overflow must trigger a correct slow path or declared fallback,
with debug visualization and counters; never silently drop lighting.

### bounding boxes before finer culling

Shared resident bounds feed per-view frustum/distance/box rejection. Invalid,
near-plane-straddling, newly spawned, teleported or unknown bounds stay visible.
Visual displacement and skinning expand/recompute bounds before culling.

Keep the two-phase HZB path: conservative early occluders -> depth -> farthest
depth reduction under the chosen depth convention -> remaining box tests ->
compacted visible/indirect output. Temporal visibility is a hint, never permission
to erase a newly revealed object. Bounded counters and prefix sums cannot overflow.

Harden odd/non-power-of-two reductions, per-candidate mip choice, footprint
sampling and epsilon, camera cuts, resized targets and occluder deletion. Compare
SPDs or other reduction kernels against exact conservative CPU pyramids before
adoption. Validate shadow-view occlusion separately using light-view depth.
Record candidates, early/late survivors, fallback count and final drawn count.

GPU indirect work bins by compatible pipeline/material binding/index type and
pass; reset counters, cull, compact, generate arguments and draw. Stable output
is required where order affects blending/replay captures. Native multidraw/count
support is probed; fallback can issue bounded individual indirect draws or use
CPU-built arguments without pretending the unsupported feature ran.

Preserve CPU software occluders as a measured candidate: curated conservative
proxies, front-to-back SIMD masked rasterization, tiled depth/coverage (8x4 as an
experiment), interleaved queries and disjoint screen-region jobs. It belongs in
a graph CPU preparation node and must match the safe visibility direction.
Use only where CPU headroom and scene structure justify it.

## 10. colour, post and antialiasing

### one colour and temporal contract

Decode sRGB colour textures at sampling; normals, roughness, depth and masks stay
linear data. Lighting, emission, transparency, portal/mirror captures and bloom
compose in linear HDR. Tone map the completed scene once, then encode for output.
UI composition states whether it occurs in linear display space or a documented
presentation path; text remains native resolution.

The old default tone-mapped captures before overlay and drew transparency after
scene tonemap. Extraction preserves baseline first; replace that ordering with
HDR-compatible surface/transparent composition and explicit exposure ownership.
Per-camera effects can differ, but a physical portal cannot silently apply both
its destination camera's tonemap and the viewing camera's tonemap.

Resource descriptors carry straight/premultiplied alpha. Blend, over, blur,
resize and texture edges respect it; conversion is explicit. Colour-space and
normal-map mip generation use semantic filters, not one byte-space average.

Velocity uses previous/current transforms, deformation and camera matrices with
defined jitter treatment. Temporal resources key world/view generation, camera,
extent, projection, pipeline, shader/material epoch and portal lineage. Reset
after cuts, resize, new content or incompatible policy; disocclusion uses
depth/normal/ID rejection, neighbourhood clamps and a reactive mask.

### AA menu

| Choice | Required work | Proof/fallback |
|---|---|---|
| None | Direct resolve with no history | Reference image for other modes |
| MSAA | Supported sample-count targets, matching raster state, explicit colour/depth resolve and deferred edge treatment | Probe sample/format support; reject unsupported combinations; exercise alpha-test and thin geometry |
| FXAA | Luma/edge fullscreen pass after appropriate scene conversion | Edge fixtures, text excluded from blur; cheap spatial fallback |
| SMAA | Edge, blend-weight and resolve nodes with required lookup resources | Test diagonal/subpixel edges and actual three-pass resources |
| TAA | Jitter, velocities, depth/history, rejection/clamp and accumulation | Static convergence, camera/object motion, thin foliage, emissive reactive mask and cuts |
| Temporal upscale | Low-resolution scene plus high-resolution history/reconstruction and optional sharpen | Independent project-sized node group; dynamic-resolution/cut tests; UI at output resolution |

Mode selection replaces a typed subgraph. No mode can reuse history with another
mode's interpretation. Capability profiles explicitly choose spatial fallback;
quality does not fork an untracked hardcoded renderer.

### post nodes and measured cost

Bloom uses down/up pyramids, optional 13-tap downsample and first-level Karis
weighting, tent upsample and HDR additive composition. Specify minimum mip extent
and maximum chain length from quality; the old note's 512-pixel hint is an
experiment, not a universal smallest-level rule. Verify energy and bright emitters.

Exposure consumes a bounded histogram/reduction with explicit adaptation clock.
Grade/tonemap can use a baked 3D LUT with declared domain, interpolation, gamut
and exposure placement. Compare LUT output with analytic curves and record the
error; dynamic exposure cannot be hidden in a stale baked LUT.

DOF consumes linear depth and lens parameters; motion blur consumes velocities;
SSR consumes depth/normal/roughness/HDR and falls back for offscreen misses.
Half/quarter-resolution AO, fog and bloom use depth-aware or appropriate
upsampling. Every enabled node has a complete shader, input contract and image
test; a placeholder catalogue default is a failed implementation gate.

## 11. tracing and global illumination nodes

### port boundary

First inventory the prior semi-real raytrace/path-trace code and fixtures in the
working tree and available history. Port useful algorithms into node handlers
and shared material contracts; retire their old orchestration. If there is no
usable prior implementation, build the missing node rather than calling a named
catalogue entry a port. Record provenance and any retained licences.

Define semi-real raytrace concretely as hybrid rendering: raster primary depth,
normal and material plus real secondary ray intersections for reflections,
shadows and bounded indirect lighting. SSR may be a fast candidate/miss fallback,
but rays must hit valid offscreen geometry in the demo.

Compute tracing provides a portable baseline using a bounded acceleration
structure over resident geometry. Mesh-local static BVHs can be cooked; instance
acceleration updates/refits on transform changes; editable/topology changes
rebuild only affected structures. GPU visual deformation must update traced
geometry/bounds or declare an explicit nonparticipating fallback.

Nodes: `trace-geometry`, `trace-acceleration`, `ray-generate`, `trace-hybrid`,
`trace-path`, `trace-accumulate`, `trace-denoise`, `trace-composite`. Names are
proposed schema IDs. Separate world geometry work from per-view ray/history work;
do not rebuild or reupload the same world BVH per camera.

Hardware ray queries/pipelines require backend implementation, capability probes,
shader forms and parity tests. Compute/no-compute fallback is visible in the
selected pipeline report. No vendor-only requirement silently becomes baseline.

### path transport contract

Progressive path tracing samples camera rays, intersects actual triangles,
evaluates the same material parameters, samples lights and BSDFs with explicit
PDFs, accumulates throughput/radiance and terminates with a bounded bounce cap
and controlled Russian roulette. Include emission, environment, alpha policy,
shadow visibility and diffuse/specular transport; state whether transmission is
supported for each technique. Use multiple importance sampling for direct-light
and BSDF sampling to reduce variance. [Path-tracer reference](https://pbr-book.org/4ed/Light_Transport_I_Surface_Reflection/A_Better_Path_Tracer).

Use robust origin offsets, conservative traversal, finite-value guards, bounded
stack/queue sizes and overflow fallbacks. Seeds depend on view/pixel/sample and
purpose, not scheduling order. Separate accumulation count, moments and denoised
display; denoising cannot overwrite the raw convergence evidence.

Reset history on camera/projection/extent changes, scene/material/light revisions,
deformation/topology changes and relevant portal updates. A frozen interactive
preview may continue samples; a hidden preview stops. Resource/iteration budgets
are declared settings, not a driver timeout assumption.

Ray masks and per-instance capability bits live in GPU rows. Portal rays transform
origin/direction and carry bounded lineage; physical transport, lighting and
recursion rules agree with §12. If a material/volume cannot be traced, report its
technique fallback instead of silently disappearing.

Proof includes ray-box/triangle degeneracies, BVH versus brute-force intersections,
offscreen reflection, shadow visibility, mirror/diffuse/emissive scenes, fixed-seed
repeatability, monotonic statistical convergence and invalidation after one edit.
CPU references test tiny scenes; real GPU output proves shader traversal and packing.

## 12. portals with one seam contract

### transform, projection and geometry

Define a rigid seam transform from entrance frame, the engine's facing-turn
convention, and destination frame. One pure function supplies camera, points,
directions, normals, velocity and ray mapping. Test forward/inverse round trips;
mirror handedness is a separate operation. Initially reject non-rigid/unequal
scale pairings unless scale physics and projection are explicitly designed.

Resolve portal cameras through scene camera math. Clip to the destination plane
with the correct zero-to-one oblique projection or explicit clipping contract;
clip the entrance aperture using a common polygon/mask. Test edge-on views,
camera on the plane, near-plane crossings, nested panes, backfaces and mirrored
winding. The surface must not show geometry from behind its exit wall.

A body straddling the seam draws clipped source and transformed destination
proxies from the same stable entity identity and authoritative transform.
Both halves share material/skinning/deformation, normal basis, lighting inputs,
shadow policy and motion history. Do not duplicate uncut geometry or rebuild
physical ownership merely to show its far half.

Recursion is a bounded view graph with stable world/view lineage. Render deepest
required captures before consumers; cycles use the last valid history with a
visible diagnostic when exhausted. Bound depth, total views, pixels, memory and
per-frame work, and invalidate history when either portal or destination changes.
Missing/deleted destinations preserve the established mirror fallback where
applicable, with an explicit status rather than undefined memory.

### light transport and composition

Portal and mirror captures use the same PBR/material/colour pipeline as the scene.
Carry the destination environment and light state through the view contract;
avoid tinting the physical portal by a default grey pane or double exposure.
Composition uses proper depth/aperture coverage and HDR colour before tonemap.

Cross-seam lighting needs a generic portal-light transport node, not just a camera
picture. Clip transported light influence to aperture geometry, transform light
direction/position, account for both-side shadow blockers and cap light-path
recursion. Trace paths follow the same seam mapping. A light crossing the portal
must not double its energy or vanish at the plane.

Test source and destination light configurations, offscreen casters, alpha-test
casters, emissive objects, moving lights, shadow bias and exposure. Keep a direct
unfolded reference scene to compare with the equivalent portal scene.

### physics and world ownership

Detect swept crossing against the aperture, not only a sign flip at frame end.
Map position/orientation, linear/angular velocity and remaining movement through
the seam at a deterministic world barrier. Prevent immediate reentry using
oriented crossing state with a finite separation rule; a time-only cooldown must
not trap slow bodies or miss fast crossings.

During overlap, collision queries against the far side transform into the other
space and return owned contacts/constraints to the authoritative solver. Proxies
are query shapes, not independent simulated bodies. Define contact normal,
impulse, inertia, gravity-frame and constraint behavior across the seam; chains,
ragdolls and compound bodies require whole-island or explicitly constrained
transfer, never silently broken joints.

Same-world portals retain one entity/body owner. Cross-world transit is a named,
versioned transfer payload, accepted once at coordinated barriers. No ECS handles
or pointers cross worlds; requests include stable identity/generation and tick.
Destination absence or rejected transfer retains valid source ownership and
collision. Acknowledgement prevents duplicate removal or double spawn.

Portal physics math belongs in scene/collision/physics/world at legal layers,
not render. GPU-only visual displacement changes neither collision nor transit
authority. Multiplayer tests cover ownership transfer, rollback/replay inputs,
stale destination, simultaneous bodies and high-speed tunnelling.

### visible proof scene

Build `PortalSeams` example: contrasting lit rooms, matching floor grid, an oblique
and moving portal pair, a portal loop, a mirror, a skinned/rigid object crossing,
a rolling body, a fast projectile, a long body straddling and a cross-world pair.
Show entry/exit planes, body owner, velocity vectors and source/destination
clipped bounds on demand. Provide deterministic camera/crossing scripts, pause,
single-step and direct-versus-portal split view for agent inspection.

Gate is inspected image sequences plus numeric ownership/contact tests. A single
still image cannot prove seamless motion, physics or temporal history.

## 13. editable packing, quantization and geometry detail

### explicit policy components

Add scene-owned quantization policy components for editable mesh and editable
texture/image data. Use one shared format vocabulary with component-specific
channels. Fields include enabled state, target attributes/channels, encoding,
range/scale/offset, rounding mode, saturation/refusal policy, quality/error limit
and revision. Format names serialize as strings and drive both VMs and Studio.

Distinguish three operations: editing canonical authored values, rounding those
values to a declared lattice, and storing/uploading a compact representation.
An explicit destructive rounding edit changes authored values and revision;
presentation-only packing preserves authored values and updates visual residency.
Collision rebuild follows actual mesh edits, never camera-dependent quantization
or displacement. Undo/redo restores values and policy in one transaction.

| Requested family | Representation contract | GPU path |
|---|---|---|
| float16 | IEEE binary16, declared finite/overflow/subnormal rules | Native where supported; packed uint decode otherwise |
| unsigned float16 | Explicit nonnegative range or named unsigned encoding; not an assumed IEEE type | Defined encode/decode and error test before exposure |
| float8 / unsigned float8 | Named exponent/mantissa layout, e.g. explicit E4M3/E5M2 semantics, sign/nonnegative policy, finite limits | Packed storage with shader decode on baseline; no assumed native texture format |
| int16 / uint16 | Signed two's-complement or unsigned; raw integer versus snorm/unorm is explicit | Typed integer fetch or normalized/decode path |
| int8 / uint8 | Same distinction, bounded channels/ranges | Probe storage/arithmetic/sample support separately |
| int4 / uint4 | Defined signed/unsigned nibble order, padding and stride | Pack into byte/word; decode explicitly |
| bool | Canonical zero/one value; bit-packed storage with explicit word layout | Mask/shift decode; no C++ bool ABI on wire |

Signed/unsigned labels cannot be aliases with different undocumented ranges.
Round-to-nearest ties policy is deterministic; reject NaN/infinity in authored
finite types; define zero, negative zero and saturation. Test min/max, ties,
odd counts, endian, alignment and every code for small formats.

Storage support does not imply shader arithmetic support, and arithmetic support
does not imply a filterable image format. Probe the actual backend surface;
FP16/int8 feature support itself is optional on relevant Vulkan versions.
[Khronos feature contract](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_shader_float16_int8.html).

For bounded normalized quantization use scale/offset and a recorded max error;
the half-step error holds only within range without clipping. Positions use
mesh-relative bounds; normals/tangents use octahedral encoding with handedness;
UVs preserve authored tiling via separate bounds/scale. Degenerate extents decode
without division. CPU reference and GLSL/MSL decoders must agree.

### mesh allocation and bake pipeline

Keep the existing shared vertex/index buffer model, add slab suballocation only
when arrivals/growth cause measured transfer waste. Use bounded free ranges/bin
classes, coalescing, stage/commit batches, geometric growth/high-water retention,
deferred reuse and an oversized-mesh policy. O(1) claims need an actual allocator
proof; do not copy them from research notes.

Use u16 indices when all referenced indices fit; u32 otherwise. Validate count
and overflow, including 65,535/65,536 boundaries. Quantize then weld on compatible
attributes, preserving UV/normal/material seams and tangent handedness. Reorder
triangles for post-transform cache, optimize fetch order and remap vertices.

Editable mesh changes repack/upload dirty attribute/index spans where the format
allows. A changed quantization bounding box can invalidate a whole stream; count
that full repack honestly. Immutable bake and runtime editing share codecs,
while the cook stays off the shipped runtime path.

### four LOD modes and tessellation

1. Four authored mesh levels: stable references, projected size/error thresholds,
   per-level bounds and residency. Missing finer content selects a resident
   coarser level; it does not erase the object.
2. Automatic decimation: deterministic edge collapse with normal/UV/material
   constraints and attribute weights. Share vertices/index ranges where useful,
   allow separate coarse streams when needed; store measured geometric error.
3. Smart triangle reduction: select by projected geometric error and triangle
   area, with silhouette/boundary preservation. Tiny triangle count alone is not
   permission to remove important silhouettes or thin walls.
4. Meshlet/virtual geometry: cook 64 to 128 triangle clusters as initial test
   sizes, hierarchy/DAG, sphere/cone bounds and error. GPU chooses a crack-safe
   cut, culls and streams required pages. Hardware raster for larger clusters,
   optional compute visibility raster for tiny triangles, then material resolve.

All selections run in GPU per-view output beside culling, using stable instance
slots. Store policy and level references in world rows. Add hysteresis (0.85
down-threshold ratio as an experiment), camera-specific history and separately
tuned shadow LOD. CPU selection remains a test/no-compute fallback only.

Meshlet work is a required staged prototype under this request, not silently
discarded as long-horizon. Bound page tables/requests, provide resident coarse
coverage, stitch transitions and handle edits. Probe 64-bit atomics or use a
proved alternative before a packed atomic visibility implementation; capability
failure selects the four-level/decimated graph with a visible reason.

Tessellation is a node with bounded factors, crack-compatible edge rules and
camera/error policy. Use native tessellation only where the backend exposes it;
compute subdivision or baked subdivisions provide supported fallback. Visual
displacement samples declared maps, expands bounds and updates normals, shadows,
velocity and traced geometry. Physics still sees authored collision geometry.

Proof covers watertight seams, negative scales, degenerate triangles, UV repeats,
alpha edges, topology edits, LOD oscillation, cameras choosing different levels,
portal views, streaming pressure and conservative bounds. Report source/packed
bytes, triangle/vertex fetch counts and decode cost, not a guessed percentage win.

## 14. textures, mip streaming and atlas proof

Keep source, cooked, compressed and device representations distinct. Evaluate
ETC1S/UASTC interchange in a bounded standard container and target transcode to
BCn/BC7, ASTC, ETC2 or RGBA fallback according to actual backend support.
Compare cook-per-target storage against runtime transcode cost; no runtime
recompression hidden in the render thread. UI/masks can request lossless formats.

Mip chains preserve colour space, alpha coverage/premultiplication and normal
semantics. Flipbooks filter cells independently and stop when a frame reaches
one pixel; unevenly divided cells refuse an unsafe chain. Procedural/editable
producers use the same rule; dirty regions expand appropriately at each mip.

Streaming starts with usable coarse levels, promotes by declared screen-footprint
policy, and uses hysteresis/cooldown. Apply byte budgets to resident, staging,
pending and retired allocations, not texture count. Do not evict current/in-flight
resources; demote unneeded fine mips before whole least-recently-used resources.
An oversized request fails without thrashing every other texture.

SDL may require a new allocation for a different resident mip range. Account for
temporary overlap and retirement, update descriptors atomically, and ensure
missing upper levels are never sampled. Optional GPU feedback is asynchronous
streaming demand; LOD visibility decisions remain GPU-resident without a blocking
CPU readback. A conservative CPU footprint fallback may overrequest content.

Staging uses bounded persistent rings, aligned ranges and completion fences.
Recycle only retired ranges; cap bytes per frame and defer finer mips. Critical
base data and diagnostic markers have explicit priority; no unconditional full
buffer cycling/reupload on each arrival. Track queue age and refusal reasons.

### atlas experiment

Build a graph-addressable atlas/array allocator with stable logical handles and
indirection to tile/layer, UV rectangle, padding and mip availability. Test fixed
arrays before variable tiles. Group compatible formats, samplers and colour
spaces; repeat/wrap textures may require array layers rather than atlas tiles.

Allocate gutters sufficient for filtered mip footprints; generate per-tile mips
with edge dilation. Clamp sampling within safe rectangles; alpha and normal
tiles need semantic padding. Repacking updates indirection transactionally and
retires old allocations only after references complete. Editable dirty rectangles
must not touch neighbours; rotated tiles require matching UV/normal conventions.

### many 4K textures

Create deterministic fixtures at 1, 8, 32, 128 and a pressure-selected higher count
of 4096x4096 textures. A base RGBA8 4K image is 64 MiB; its full chain approaches
85.33 MiB. Eight full chains already exceed the existing 512 MiB content budget,
so test streaming/refusal instead of promising all images fit.

Compare RGBA8, semantically valid compact channels, block compression and packed
editable formats with/without atlas. Measure actual device/staging/retired bytes,
upload traffic, resident mip distribution, visible error, edit latency and frame
time. Sweep device budgets rather than allocating until driver failure.

Tests include all tiles visible, mostly hidden, repeated materials, camera motion,
wrap/anisotropy, distant mips, alpha borders, shader sampling, one-pixel edits,
odd packed dimensions, repack, eviction and device recreation. Flat steady live
bytes plus bounded churn and correct images are the gate.

## 15. save, replication, safety and migration

Save authored names, parent references, sparse tagged overrides, declared feature
keys, attachment order, quantization policy and Studio-only source. Save revisions
only where reload semantics need them. Do not save resolved catalogues, GPU
handles/slots, capability decisions, cache timestamps or duplicated module bytes.

Material/attachment deltas carry entity identity in the owning protocol, stable
slot/parameter name, base/next revision, bounded value and authority. Base mismatch
requests a bounded current snapshot; duplicate/stale deltas are idempotent.
Large textures/modules/material definitions use signed delivery, not replication.

Server validates approved publication/schema names and values without compiling
shaders or linking render. Client-local visual overrides remain nonauthoritative;
gameplay cannot rely on local LOD, displaced vertices or visual particles.
Cross-VM setters, save readers and network readers use one canonical validation.

Bound source/include expansion, names/errors, parameter/default/override bytes,
inheritance/fallback depth, variants, module words/instruction estimates,
descriptor ranges, local invocations, buffer/target extents, per-frame changes,
uploads and pipeline creation. Reject malformed arithmetic before allocation or
device calls. Validate SPIR-V and permitted capabilities independently.

Signed bytes establish publisher identity, not shader correctness or a guarantee
against GPU hangs. Validate available structure, restrict resources and dispatch,
and isolate authoring workers; never claim static instruction counts bound
arbitrary loop execution. No raw device addresses, host pointers or undeclared
resources in authored contracts.

Worker cancellation/failure cannot publish half an artifact. Escape source/path
diagnostics in UI, cap excerpts and unique-name diagnostic/cache growth. Fuzz
containers, reflection and translation under process limits; soak failed cooks,
publication swaps, resource pressure and delayed retirement.

Migration is one-way conversion into canonical runtime structures. Freeze old
material/world/ShaderScript/graph fixtures; load cooked modules alongside the
existing source path temporarily; migrate material shader names into techniques,
package inline raster/dispatch source, then remove the source compiler path after
approved policy and package gates. Studio retains authoring source and cook jobs.

Built-ins enter the same validation/reflection/container path. Preserve any
needed legacy readers for the declared support window, but remove duplicate
resolvers, hand-written parameter tables and old shader binding conventions.
Release target/staging checks prove no shaderc front end, source includes or
source-only runtime demands ship after the migration gate.

## 16. particles, environment and TornadoSim

### retained particle foundations

Preserve per-world pools and per-emitter fixed capacity blocks; live-prefix swap
retirement and free-range reuse. Separate mutable simulation, compact draw rows
and per-emitter shared state. The predecessor records 28-byte particle draws and
16-sample curves; verify current layouts before optimizing them.

Keep stateless seeded emission from emitter identity, monotonic spawn counter and
purpose tag; never seed from a recycled slot. Curves update on authored revision,
transforms/forces on their own revision. GPU owns visual stepping when active;
CPU reference/fallback defines matched semantics without running a duplicate
live particle system. Spawn, integrate, retire and draw become explicit nodes.

| Preserved candidate | Plan and gate |
|---|---|
| Blend/depth order | Retain blend/orientation partitions and unsorted additive particles; add bounded far-to-near depth buckets for blended groups; test overlapping smoke and bucket seams |
| Soft particles | Sample opaque linear depth, apply configurable depth-difference fade; opt-in for smoke, not mandatory for tiny sparks; measure overdraw/tile GPU cost |
| Significance/LOD | Compact emitter rows ranked by projected importance and effect class; rate/lifetime/size/pool caps, distance activation and fade; reduce fill footprint as well as count |
| Curl fields | Generic sampled/procedural vector field with strength-zero bypass; finite-difference curl of a defined vector potential, optional low-rate interpolated updates |
| Curves | Allow bounded per-emitter sample quality for sharp authored curves; do not enlarge every emitter's table |
| Trails | Preserve simulation-tick fixed ring and render-derived ribbon; authored-only save/replication restores empty history; no render-rate trail sampling |

Field effects declare their units, clock, interpolation and bounds. Approximate
curl may have discretization divergence; test the actual field rather than
claiming perfect incompressibility. Field textures and parameters reside on GPU.

### environment nodes

Make existing SkyboxCompute, AtmosphereProcedural and CloudCompute paths explicit
graph producers. Add generic fog/volume resources, density generation, lighting,
integration, reprojection/upsample and composite nodes. World generation can be
shared; view integration/depth/temporal history belongs to each camera.

Define sky cubemap face orientation, atmosphere units, density/extinction/albedo,
phase function, step count, light injection/shadow inputs and height bounds.
Test analytic empty/constant-density limits, Beer-Lambert transmittance, horizon,
camera-in-volume, depth intersection, lighting changes and temporal disocclusion.
Bound volume dimensions, ray steps, history and update rate; no unlimited compute.

### TornadoSim obligations

Retain the pure Luau first slice in [TORNADOSIM.md](TORNADOSIM.md). It is an arcade
analytical field, not a fluid-solver port. One `SampleField` provides softened
maximum-wind ring, radial inflow, central lift, upper flow and deterministic
turbulence; EF-style presets and lifecycle use tables and the script clock.

`RunService.Heartbeat`, Part/Model/CFrame/Vector3/math.noise drive bounded kinematic
debris, ground bounce, tree bend and script-controlled building break groups.
ParticleEmitter disc/cylinder, tangential/radial forces, drag/noise and curves
make dust base, condensation funnel, rain shaft and cloud deck on the GPU.
Beam, PointLight, Trail and GUI supply lightning and hazard readout.

`TornadoSim.luau` contains gameplay state, moving vortex, plain, structures,
camera, preset controls and camera-sampled hazard. First runnable slice needs no
external assets; later wind/thunder audio uses published content. No per-particle
CPU mirror, tornado-specific native component, or claim of rigid debris physics.

Render gaps proved by the scene feed generic nodes: field-sampling force when
emitters cannot follow the field, volume rendering for dense cloud self-shadow,
visual deformation for vegetation, and lightning/environment composition.
Physics velocity/impulse or reusable break groups are separate owner-layer work
only when kinematic debris is insufficient.

The source lab's 1M to 50M custom particles and sparse cloud density octree remain
explicit scale experiments, not requirements for the first script or promised
capacity. Profile the real scene first, then bounded pool/volume variants under
memory/fill limits. Procedural spatial audio remains an audio/content dependency.

Proof: both VM metadata where relevant, Luau typecheck, headless script advance,
inspected funnel/rain/debris/tree/lightning motion, correct hazard at camera and
release profile stating debris, emitters, live particles and volume work.

## 17. chunked-world render input candidates

These requirements from the optimization notes remain owned work, with scene,
terrain, bake, world and physics plans providing the non-render systems.
They must not become private renderer terrain state.

| Candidate | Preserved algorithm/constraints | Verification |
|---|---|---|
| Padded binary greedy meshing | Stage `(n+2)^3` voxel shell from immutable neighbour borders; bit masks per face, maximal run/row merge, optional 64-bit packed quad; reusable scratch | No hidden boundary faces or missing neighbour access; packed coordinate/extent overflow tests; fixed-width constraints explicit |
| AO-aware quads | Per vertex side1/side2/corner: both sides occupied gives zero, else `3 - sum`; 2-bit value; diagonal selected from opposite-corner sums | Document exact vertex order/diagonal rule and test asymmetric cases, rather than relying on a vague larger-sum instruction |
| Chunk scheduler | Coordinate-sorted derived readiness for generate/mesh/upload; revision/ticket validation, interior then border reconciliation | ECS owns state; no private dirty registry or structural tag churn; repeated/missed signals converge |
| Ticketed generation | Immutable request with graph/seed/coordinate/LOD/outputs; dedup full signature, bounded owned results, current ticket/revision check | Completion order alone cannot change simulation. Deterministic tick admission must wait/join or record an explicit admission decision; ticket sorting alone is insufficient |
| Streaming | Load radius inside unload radius, e.g. 3 versus 5 chunks; distance/frustum priorities; async byte/work and sync finalization budgets | Priorities affect visual arrival only; cancel pending eviction on reentry, memory bounded, physics admission deterministic |
| Region files | Candidate 32x32 columns, floor division for negatives, location/timestamp table, 4 KiB sectors, bounded independently compressed chunks and oversize sidecars | Transactional write/flush/recovery needed; synchronous open alone is not crash safety. Test torn writes, reuse and malicious offsets |
| Chunk phase DAG | Cached dependency order, per-tick readiness counters, conflict bitsets, bounded completion queue, fork/join within tick | Compare serial/parallel output; use simple phased Jobs::For below measured crossover |

Greedy merge preserves material/alpha/AO seams. Border edits invalidate adjacent
derived meshes; LOD borders stitch or use justified skirts. No claim of a
hundredfold speedup or fixed performance factor survives without local evidence.

## 18. profiling and optimization gates

Every meaningful node CPU scope uses existing ENGINE_PROFILE instrumentation,
feeding Tracy, FrameGraph and heap tags. Worker durations report after join,
preserving producer hierarchy. Waiting is Idle; dropped/unmarked work is visible.
Never add a second timing-only scope system.

GPU timestamps bracket real node invocations and carry frame/world/view/node
generation plus query identity. Collect only completed slots, once; late/out-of-
order results attach to their producer, not a fabricated current-frame position.
Use GPU category and `gpu ` prefix under current root profiling rules, correcting
the predecessor's suggestion to categorize GPU work as ordinary Render spans.

Existing timestamp notes specify 128 marks/four slots; verify actual backend
limits before assigning. Prioritize shadow/surface captures, geometry/lighting,
then post; show dropped marks. CPU-only nodes require no GPU marks. Vulkan-only
timing availability is not a claim of Metal timing support.

Offer Off/CPU/Full runtime tiers and per-node `profile=false`, with rows labelled
unmeasured. Compiled-out support and runtime disable are distinct: a runtime
toggle cannot remove compiled macros. Sampling every N frames records age and
holds last valid data; disabling abandons pending queries without blocking.

| Signal | Required detail |
|---|---|
| Frame time | CPU busy/self/idle, GPU producer duration, critical dependencies, submitted buffers and actual overlap evidence |
| Node work | Draw/dispatch/copy counts, submitted triangles, shaded/selected counts where observable, resolution/sample count |
| Culling/LOD | Candidate funnel, overflow/fallback, per-level counts, selected error, page/mesh requests |
| Lighting | Shadow tile texels used/allocated, cache ages, cascade updates, cluster count histogram and overflow |
| Traffic | Exact instance/index/material/texture/mesh/particle upload bytes and operations at transfer boundaries |
| Residency | Live/peak/cumulative allocated/released logical GPU payload, creation counts, staging and deferred bytes |
| CPU heap | Live blocks/bytes, total allocation and profiler overhead; one-second sampler, heap-report and slope/fit soak |
| Caches | Hits/writes/misses/refusals/evictions by bounded reason, absent layers excluded, zero traffic on actual hits |
| Authoring | Queue/cook/compile/reflect/translate/check/admit/upload/first-visible latency, variant demand and cache hits |

Studio profile grid uses pass columns, resource rows, read/write/lifetime cells,
logical/physical alias IDs, declared/peak/allocated bytes and measured GPU time.
In-game overlay reads the same data; capture receipts pair images with graph,
profile, settings and measurement age. Explicit profile capture can export text
under build artifacts; this is not an unsolicited benchmark document.

Measure shipped cost in release. Heap hooks compile out there, so use a named
diagnostic preset for heap cost and state the difference. Reports include GPU,
driver/backend, preset, scene/publication, world/view counts, material/variant
counts, resolution, quality, warm/cold cache and visible workloads.

Experiments compare one and many cameras, 1/1,000/high material counts, small and
dense geometry, static/moving portals, transparent smoke, 4K pressure, streaming
bursts, world churn and hot reload. Bound measurement windows, warmups and seeds;
show distribution/spikes rather than only averages. Do not infer byte savings
from entity counts or physical overlap from scheduling waves.

Preserve candidate priorities after correctness/residency foundations: stable
cascaded shadows, clustered lights with tight assignment, HZB funnel hardening,
texture compression/streaming/staging, mesh compaction/order, particle significance.
Bindless material slabs are a later measured step: descriptor arrays plus
material slab/slot in GPU data, dummy bindings, normalized samplers and limited
variants. Probe descriptor indexing/update behavior and actual limits; retain
bounded per-material binds on unsupported backends.

## 19. example pipelines and review scenes

Every example is an authored, saved PipelineDocument/PipelineSet plus a runnable
scene in `mono.engine/examples`, loaded through the same Studio/runtime path.
No demonstration may call a hidden renderer outside the graph. Each has fixed
capture inputs, expected tier behavior and a release profiling recipe.

| Example | Graph shape and parameters | What review must see |
|---|---|---|
| Standard PBR lab | Shared residency/shadows -> per-view cull/LOD/G-buffer -> AO/GI/light/sky -> surfaces/transparency -> post/output | Seven maps alone/together, metal/roughness sweep, HDR emission, lighting channels |
| Anime | Lit scene -> palette (initial 4 bands, dither off) -> depth/normal edges (1px) -> masked ink mix -> authored ramp/output | Stable hard bands and fine outlines; optional wide threshold/blur glare, not compulsory bloom |
| Cartoony | Lit scene -> 2 to 3 bands -> 2px edges/dilate -> saturation grade -> output | Broad outlines and flat colours; AO-only lighting profile as cheap-tier experiment |
| Drawing | Depth/normal edges + lit luminance -> hatch (scale/45-degree orientation/contrast) -> desaturate -> paper multiply -> output | Crisp silhouettes, multiple hatch densities and no needless HDR intermediates after conversion |
| Anime screencap | HDR lighting -> soft bloom -> grade -> light sharpen -> letterbox/vignette -> output conversion | Retained static composition; explicit tonemap placement prevents double conversion |
| Hybrid raytrace | Raster primary -> resident acceleration -> secondary ray reflection/shadow/GI -> denoise/composite -> output | Offscreen object reflected and occluding; comparison with raster/SSR fallback |
| Progressive path trace | Acceleration -> camera rays -> path integration -> accumulate/moments -> optional denoise -> tonemap | Diffuse/specular/emissive transport, sample count, convergence and immediate reset after edit |
| PortalSeams | Linked captures, portal-light transport, split geometry, physical crossing, recursive composition | The full sequence in §12, including cross-world ownership |
| AA comparison | Same scene/camera, selectable None/MSAA/FXAA/SMAA/TAA/upscale graph | Thin edges, foliage, moving emissive geometry, cuts and native UI |
| Multi-world camera wall | Distinct worlds and repeated cameras per world, asset preview and portal views | All active cameras update in one frame; shared rows uploaded once |
| Editable packing lab | Quantization policy -> pack/upload -> atlas/mips -> draw/inspect | Error heatmaps, real byte counts, one-pixel edits and pressure fallback |
| Geometry detail lab | Four meshes/decimation/triangle-error/meshlet modes plus tessellation/displacement | Different view LODs, watertight transitions and unchanged physics mesh |
| Attached effects lab | Same cooked module on mesh/decal/particle/ribbon/UI/environment/surface view | Correct masks/order, shared shader residency and hidden no-work behavior |
| TornadoSim | Generic weather/particle/field/volume nodes driven by the Luau scene | Moving funnel, rain, tree bend, debris, lightning and meaningful hazard readout |

Do not label unsupported Tier B/C tests passed because a default pipeline rendered
something. Assert the selected fallback graph and reason. New `palette`, `edges`
and `hatch` kinds require actual shaders; paper, ink and letterbox operations use
ordinary typed composite nodes rather than abusing diagnostic overlay state.

## 20. implementation order and deletion gates

Phase names are proposed work units, not version promises. Implement all requested
features through these phases; measured candidates retain explicit acceptance
criteria. A failed prerequisite does not justify marking the rest complete.

| Phase | Dependencies | Concrete changes | Completion gate |
|---|---|---|---|
| P0: baseline/contracts | Plan review | Read affected module policies; verify old done claims; inventory hidden rendering and shader consumers; settle reviewed compiler policy; freeze graph/material/portal/image fixtures; reconcile policy references | Current behavior classified by evidence, ownership/format/clip contracts written, baseline failures named |
| P1: image harness | P0 | Extend real-device fixtures, per-pass capture, analytic probes, comparison policy, failure bundles and `just render-check` | Deliberate projection/depth/colour/resource faults fail for the right reason; missing device cannot pass |
| P2: graph-owned frame/residency | P0, P1 | Move preparation/uploads/simulation/capture/UI into real node execution; explicit side effects/resources; retain stable world/view slots; parallel collector and batched submission | Serial/parallel same output; all active views update; static worlds have zero duplicate uploads; delete old frame orchestrator |
| P3: shader cook and interfaces | P0, P1, graph contracts | Separate runtime schemas from compiler targets; cook bundles/variants, reflect contracts, validate/translate, signed delivery and complete keys | Packaged authored shader works through cooked loading; supported backends pass bindings/images; policy prerequisites satisfied |
| P4: materials and attachment data | P2, P3 | Typed material definitions/instances, samplers, capability policy, per-item attachments, VM/save/replication integration and resident rows | Legacy content uses one resolver; per-item changes update data only; every visual family has tested attachment semantics |
| P5: compositor and Studio authoring | P3, P4 | Typed palette/groups/material graph lowering, inspectors, async cook previews, undo/redo and hidden-work gates | Save/load/cook/run round trip; stale/failed cook cannot replace valid preview; no UI-thread compile hitch |
| P6: lighting/colour/post/AA | P1 to P4 | PBR contracts, HDR order, dynamic AO/GI baseline, stable shadows/clusters, post shaders, velocity/history and AA subgraphs | Numeric energy/colour and motion fixtures pass; every mode has actual backend or explicit tier fallback |
| P7: packing/streaming/geometry | P2 to P4 | Quantization components/codecs, editable dirty updates, mip/texture pressure/atlas, four LOD modes, decimation, meshlets, tessellation/displacement | Bounded memory and error; no culling holes; per-camera GPU LOD; physical mesh unaffected by visual state |
| P8: portal completion | P1, P2, P4, P6; physics/world work | Shared seam math, clipping/proxies, light transport, HDR captures, physical overlap and world transfer | Numeric ownership/contact and inspected crossing sequence; no missing geometry or doubled lighting/body |
| P9: tracing | P3, P4, P6, geometry contract; P8 for portal rays | Port/build acceleration and hybrid/path nodes, accumulation/reset, denoise and capability fallback | Offscreen intersection and statistical convergence; no fake tracer label; portal-ray behavior consistent |
| P10: weather/effects and scene producers | P2, P4, P6 | Explicit particle/environment nodes, generic field/volume work proven by TornadoSim; retained chunk render-input candidates | Script and visual gates pass; bounded particle/volume cost; no duplicate CPU simulation |
| P11: examples and portability | Relevant feature phases | All §19 documents/scenes, native handler command adapter, all backend/tier image runs, active-camera and pressure profiling | Actual SPIR-V/MSL images, documented limits, no implicit unsupported features |
| P12: remove migration paths | All functional gates | Remove shipped source compilation/translation after approval, duplicate layouts/resolvers/old pass lists; finish checks, fuzz/soak and documentation | One supported path per responsibility, packaged cooked-only content, bounded retirement and complete evidence |

P6/P7/P8/P9 contain substantial work and can be split into smaller reviewed
changes while preserving their gates. Shader cook work must not delay fixing
existing image defects that P1 can reproduce safely; temporary compatibility
adapters have named removal in P12, not permanent second owners.

### deletion ledger

| Existing path | Replacement | Delete when |
|---|---|---|
| Product round-robin/one-view rendering orchestration | Active requests plus batch executor | Same-frame all-view output and lifecycle tests pass |
| `Begin` doing hidden CPU node work then reporting spans | Scheduled preparation nodes | Disabling/reordering nodes changes actual work with valid contracts |
| Upload outside resource declarations | World residency and delta nodes | Dirty-range, in-flight acknowledgement and zero-traffic tests pass |
| Hidden shadow/portal/particle/environment/post calls | Registered families with explicit resources/side effects | Image and failure-lifecycle parity exists for each family |
| Hardcoded material parameter/binding switches | Canonical declarations/reflection and one resolver | Legacy fixtures plus both VMs and all techniques pass |
| Inline/source-only packaged nodes and runtime ShaderScript compile | Cooked module references; Studio worker authoring | Reviewed policy updated, packaging/link/staging checks prove source-free client |
| Runtime authored SPIR-V-to-MSL translation | Published checked MSL payload | Supported MSL backend runs packaged content |
| CPU mirror of selected LOD/visibility/visual particles | GPU world inputs plus view outputs | Device/reference parity and explicit no-compute fallback exist |
| Temporary conversion shims | Supported-version canonical reader | Declared support window ends; migration fixtures retained for supported input |

## 21. final implementation acceptance

Completion requires evidence for every R01 to R16 item and each phase gate.
Documentation, catalogue entries, green unrelated tests and screenshots of one
frame cannot substitute for behavior across the named scope.

- One graph controls all render work and explains why each resource exists.
- Active world/camera collection is complete; preparation is safely parallel and
  vectorizable; record/submit remains owner-thread; cameras share residency.
- Per-instance capabilities, visual deformation, culling and LOD use GPU state
  with no per-frame decision readback; physics/authored state remains canonical.
- Every visual family supports declared attached shader/effect stages with masks,
  active residency, bounded resources and correct camera/world policy.
- Material/shader authoring, save, VM bindings, cooking, publication, load,
  fallback, hot reload and retirement all use the canonical contracts.
- PBR, colour/alpha, lighting, AO/GI, shadows, environment, post and AA have real
  image and numeric proof on the supported tier/backend matrix.
- Hybrid and progressive tracing intersect real geometry and demonstrate their
  transport, convergence and invalidation rather than falling back invisibly.
- Portals pass projection, clipping, light transport, geometry overlap, physics
  ownership and cross-world transfer tests plus an inspected moving demo.
- Editable quantization covers every requested format family; storage bytes and
  error are measured; 4K streaming/atlas pressure is bounded and visually correct.
- Authored four-level LOD, automatic decimation, projected-error/meshlet work,
  tessellation and visual-only displacement have explicit supported paths.
- Every example in §19 loads through normal authoring/cook/runtime and has a
  fixture/profile entry. TornadoSim retains honest first-slice limits.
- Release/dev/server builds, appropriate full test gates, `[gpu]` backend tests,
  shader/architecture/source/binding/component/type checks and relevant fuzz/soak
  runs pass. Report unexecuted hardware checks as missing evidence.
- Release profiling names workload and settings, shows real traffic/bytes and
  regression comparisons; caches and retirement remain bounded after churn ends.
- Replaced paths are deleted under the ledger; docs/policy and generated metadata
  match the final code. No unrelated roadmap items get marked done.

This planning deliverable is complete when the source migration map below covers
every old section, all attached checklist items have implementation and proof
requirements, source documents are removed only after porting, and references
lead here. Engine gates above remain uncompleted until implementation review.

## 22. source migration map

The following map records coverage, not inherited implementation status. Numeric
research starting points are experiments; claimed speedups, old fixed row sizes
and backend assumptions are not promises. Technical corrections in this plan
override stale research prose while retaining the problem and proposed approach.

### former render pipeline document

| Former sections | Destination |
|---|---|
| Status; 1.1 layers; 1.2 authoring chain | §0, §2, §3 |
| 1.3 resources; 1.4 catalogue/editor | §3, §8 |
| 1.5 diagnostics/profile; 1.6 backend | §3, §8, §18 |
| 1.7 exact default order; 1.8 residency/transfer | §2, §4, §10 |
| 2 invariants | §3, §4, §5, §12, §15 |
| 3.1 registration; 3.2 custom nodes | §3, §8 |
| 3.3 DeviceCaps/ShaderCapabilities | §6, §7 |
| 3.4 stages/optimization/caches | §3, §7, §18 |
| 3.5 data movement/quantization | §3, §4, §13, §14 |
| 3.6 optimized Tier A/B/C default | §6, §10, §19, §20 |
| 4 shader authoring | §0 policy conflict, §7, §8, §15 |
| 5.1 Anime; 5.2 Cartoony; 5.3 Drawing; 5.4 Anime screencap | §19 |
| 6.1 existing signals; 6.2 GPU time; 6.3 introspection; 6.4 switches; 6.5 heap | §18 |
| 7 completed and remaining modularity stages | §2 verification requirement, §3, §7, §20 |

### former optimization document

| Former sections/candidates | Destination |
|---|---|
| Cascaded exponential shadows; stable frusta | §9 shadow table |
| Filter menu; depth/normal bias; point shadow cubes | §9 shadow table |
| Shadow update scheduling; atlas/tile reuse | §9 shadow table |
| Forward+ clusters; tight light assignment | §9 clustered lighting |
| Two-phase Hi-Z hardening; CPU software occluders; indirect batch sets | §9 visibility |
| Bindless material slabs/pipeline counts | §18, §7 |
| Buffer slab allocation; instance/vertex quantization | §4, §13 |
| Smallest indices; triangle order/shared-vertex LOD; meshlets; LOD refinements | §13 |
| Universal compressed interchange; flipbook mips; mip residency/eviction; staging rings | §14 |
| Particle blocks; split arrays/curve tables; seeded emission; GPU step/CPU reference | §16 |
| Blend partition/depth buckets; soft particles; significance; curl fields; trail recording | §16 |
| Padded greedy meshing; baked AO/triangulation; derived chunk scheduler | §17 |
| Ticketed generation; hysteresis streaming; region files; chunk phase DAG | §17 |
| Transient aliasing/barriers; async compute overlap; post-chain cost | §3, §10, §18 |
| Profiling contract and six-item priority ranking | §18 |

### former materials and shaders document

| Former sections | Destination |
|---|---|
| Status/product goal/current foundation | §0, §2, §7 |
| Non-negotiable rules/ownership/layer cuts | §3, §7, §15 |
| Stable identities/definitions/instances/types/device packing | §7 |
| Texture declarations/samplers/editable textures | §7, §13, §14 |
| Source model/cooked bundles/interface matching/variants | §7 |
| Device/shader caps and selection order | §6, §7 |
| Dependency graph/cook stages/incremental keys/containers | §7 |
| Material/shader editors/hot reload/shipping boundary | §7, §8, §15 |
| CPU caches/material residency/shader pipeline keys/budgets | §7, §14, §18 |
| Graph material techniques/authored shaders/preview visibility | §6, §7, §8 |
| Runtime API/authoring API/binding generation | §7, §15 |
| Save/replication/hostile content limits | §15 |
| Migration and six work phases | §20 |
| Headless/integration/image/fuzz/soak tests | §5, §7, §15, §21 |
| Profiling/budgets | §18 |
| Non-goals/completion | §0 reconciles newly requested visual graphs/tracing/packing; §15 preserves trust and bounded interfaces; §21 |

### TornadoSim and attachment

All TornadoSim source-scope rows, existing engine doors, first demo slice,
first-pass exclusions, proof gates and evidence-triggered engine doors are in
§16. Its gameplay plan remains linked rather than deleted. R01 to R14 correspond
in order to the fourteen attached checklist lines; R15/R16 cover consolidation.
