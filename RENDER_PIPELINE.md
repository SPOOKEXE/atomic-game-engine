# Render pipeline

## Status

This root-level document is the authority for how a frame becomes a graph of
nodes, how that graph compiles, what runs it, and which data stays resident on
the GPU. `mono.engine/render/AGENTS.md` names it as such. It is written against
the tree as it stands: the `graph` runtime at L9, the render backend at L12,
the Studio editor, and `docs/OPTIMISATIONS_RENDER.md` as the backlog feeding it.

- **§1 to §2** describe what exists today, by name, so nothing here invents
  a second vocabulary for something already in the tree.
- **§3 to §5** are the design for finishing the modularisation: one
  registration path, custom nodes, shader capabilities, compilation stages,
  data movement and quantisation, and the optimised default pipeline.
- **§6 to §8** cover profiling and introspection (GPU, render, heap, flame
  graph, with disable switches), the demo pipelines (anime, cartoony,
  drawing, anime screencap), and the staged migration.

Nothing in §3 onward is committed work. Each item says what exists to build
it on.

---

## 1. The frame as a graph, as implemented

### 1.1 The layers

| Module | Layer | Owns | May not |
|---|---|---|---|
| `mono.engine/graph` | L9 `shared` | node runtime: documents, graphs, schedules, plans, entity flow, culling arithmetic, diagnostics, profile grids | name any device type |
| `mono.engine/render` | L12 `client` | device: node backends, RHI objects, targets, shader compilation, the frame | decide what is drawn; that decision lives below |
| `mono.engine/resources` | L11 `client` | built-in GLSL, compiled by `glslc` at build time | be reached except through `resources::Shader(name)` |
| `mono.engine/msl` | L11 `client` | SPIR-V to Metal translation | become a second compiler inside `render` |
| `mono.studio/nodegraph` | standalone | canvas library: graph model, evaluation, layout, serialisation, inspectors | know anything about rendering |
| `mono.studio` | program | render pipeline panel and Demo Nodes panel, both over the one canvas | own engine state |

The split is load-bearing twice over. `graph` is testable on any machine
because it touches no device: every culling, schedule and document round-
trip test runs headless. And `graph` being `shared` means physics,
replication and audio can reuse the same runtime for their own node sets,
which is why it is not `client` tier.

### 1.2 The authoring chain

```
PipelineDocument            ordered edits, undoable, saveable ("renderpipeline 2")
   | graph::Build           replay edits into an empty graph
   v
graph::RenderGraph          nodes + typed resources
   | graph::Compile         declaration order is the order;
   v                        reads/writes are a check, not a derivation
graph::CompiledGraph        Shared / PerView / Final blocks
   | graph::CompileSchedule dependency waves, queues, async policy
   v
graph::ExecutionSchedule    waves of ScheduledNode {Queue, Async, Culling}
   | graph::PlanCommandBuffers
   v
PlannedCommandBuffers       Graphics / Compute / Transfer classes
   | render::GraphRunner    implements graph::NodeRunner over SDL
   v
SDL submits                 one thread records, buffers ordered transfer/compute/graphics
```

Facts each stage pins:

- **Declaration order is execution order.** A read-modify-write chain
  (`gbuffer`, then `deferred-lighting`, then `tonemap`, all touching colour)
  is a cycle if order is derived from dependencies, so the graph does not
  derive it. Resources turn mis-ordering into a named compile diagnostic
  instead of a frame lit by stale memory.
- **Nodes name resources, never other nodes.** Reads and writes are
  `ResourceId`s; the editor turns them into wires. An unconnected input is
  legal and means "empty", which makes a mis-wired filter draw nothing
  rather than everything.
- **Scopes are `Frame`, `World`, `View`.** The executor partitions on them:
  shared work runs once per distinct world before the views and once after,
  so four split-screen views of one world pay one shadow pass. A shared node
  declared between two per-view nodes is refused (`SharedBetweenViews`),
  never silently hoisted.
- **Parameters are text** (`Name key`, `string value`) because they cross a
  save file, a diff and somebody typing them. Parsing happens at the reader,
  which also owns what absence means.
- **Disabled nodes leave the compile**, so switching a pass off costs zero
  per frame rather than a branch per node per view.
- **Multi-writer resources are versioned by declaration order**, and the
  scheduler's waves follow resource edges rather than declaration position;
  `CompileRenderPipeline` repartitions the compiled blocks from the schedule
  by scope.

### 1.3 The resource model

`ResourceDesc`: interned `Name`; `ResourceKind`; `ResourceFormat`; size as
absolute `Width`/`Height` or `Divisor` relative to the view (zero follows
the view); `External`.

Seven kinds: `Colour`, `Depth`, `Texture`, `Storage`, `Buffer`, `Camera`,
`Entities`. `Camera` is a `Viewpoint` (frame, lens, projection, fitted flag)
and exists because "the camera" used to be ambient; an unwired camera input
falls back to the view camera. `Entities` is indices into the view draw
list where order is part of the value; it is how the graph says *which*
geometry a pass draws. Both flow along wires on the CPU:
`EntityFlow.hpp` holds named lists per view; `Cull.hpp` produces ascending
index lists so recorded frames replay; `CullAndBound` fuses bounds for the
shadow fit; `VisibleSurfaces` resolves pane-in-pane visibility iteratively;
`FitDirectionalLight` and `FitPortalLight` build light matrices from bounds
alone.

Formats are the set a frame is actually described in: `R8`, `RG8`, `RGBA8`,
`RGBA8_SRGB`, `RGB10A2`, `RG11B10F`, `R16F`, `RG16F`, `RGBA16F`, `R32F`,
`RG32F`, `D24S8`, `D32F`, plus block-compressed arrival formats (`BC1_SRGB`,
`BC3`, `BC5`, `BC7_SRGB`). Kind and format are orthogonal: kind says whether
a pass may render into a thing, format says what a pixel holds. Kind
mismatch refuses; format mismatch warns as a **lossy wire**
(`IsLossy` compares bits per pixel and channels).

External resources (swapchain, portal history, last-frame) may not alias,
may be read before any pass writes them, and a write is producing the frame.
Everything else is transient and owned by the graph.

### 1.4 The catalogue and the editor

`graph::NodeCatalogue` registers what a node kind *is*: ports (`PortSpec`
with kind, format, required flag, summary), presentational category
(`Draw`/`Composite`/`Interface`/`Output`), `Source` flag, `Scope`,
`DefaultShader`. Wire rules live here: `PortsCompatible` is asymmetric (a
`Texture` input accepts `Colour`, `Depth` or `Storage`; a sampled texture is
not renderable into), `WhyIncompatible` returns the words, format checks
need the catalogue so unknown kinds skip them.

Sixty-three kinds are registered today by `RegisterRenderNodeKinds()`:
captures (`shadow`, `mirror-capture`, `portal-capture`), the deferred path
(`gbuffer`, `depth-linearise`, `hzb`, `ssao`, `deferred-lighting`, `sky`),
effects (`bloom`, `dof`, `motion-blur`, `ssr`, `volumetrics`, `taa`,
`smaa-edges/smaa-blend/smaa-resolve`, `sharpen`, `grade`, `exposure`),
combinators (`mix`, `blur`, `separate`, `combine`, `scale`, `reduce-chain`,
`upscale`, `blit`, `clear`), entity flow (`world`, `camera`,
`light-camera`, `entities`, `cull-frustum`, `cull-distance`, `filter-tag`,
`order-draw`, `upload-instances`), authored (`raster`, `dispatch`), and
sinks (`output-image`, `overlay`, `interface`, `present`, `viewer`,
`capture`). Repeatable kinds: `cull-frustum`, `cull-distance`,
`filter-tag`, `order-draw`, `raster`, `dispatch`, `viewer`, `capture`.
Catalogue entries name default fullscreen shaders; only four of those files
exist under `resources/shaders/` yet (`depth-linearise.frag`,
`deferred-lighting.frag`, `ssao.frag`, `tonemap.frag`), so most kinds fall
to the node's own `shader` parameter at runtime and warn if absent.

Studio maps this onto the canvas
(`studio::RegisterRenderPipelineNodeTypes`, `LoadRenderPipelineGraph`,
`SaveRenderPipelineGraph`): one node type per catalogue kind under id
`render.pass.<kind>`, wire types `render.image` / `render.buffer` /
`render.camera` / `render.entities`, hidden `__render.*` widget keys
carrying bindings and parameters through the canvas document, save-time link
validation against port declarations, and a final proof by `Build` +
`CompileSchedule` before reporting success. The canvas evaluator never
executes these graphs; the engine does. Documents are line-format text,
exactly round-trippable, carrying `Move` edits for positions that `Build`
ignores; a `PipelineSet` carries named pipelines and rides in the world
document as ECS component `"graph.PipelineSet"`.

### 1.5 Diagnostics before dollars

`graph::Diagnose` finds ten fault shapes from the declaration alone and
reports, never refuses: `DeadResource`, `WastedWrite`, `DeadNode`,
`Disconnected`, `UnwrittenRead`, `FormatOverspend`, `LossyWire` (hint),
`OutOfOrder`, `UnusedAlpha`, `SamplesOwnTarget`. Every row names its node.
The three faults needing pixel readback (constant channel, uniform target,
shading-count overspend) are deliberately absent until readback exists.

`graph::ProfilePipeline` derives the pass-by-resource viewer grid from the
graph plus compiled order, never maintained separately: passes as columns in
execution order, resources as rows with `FirstWrite`/`LastRead` lifetimes
(half-open at start, closed at end; external always alive), access cells,
and `PeakBytes` versus `TotalBytes`, whose difference is exactly what memory
aliasing would recover. `Elapsed` and `Wall` stay zero until measurement
exists; see §6.

### 1.6 The backend

`render::GraphRunner` implements `graph::NodeRunner`. Handlers register into
a `NodeTable` keyed by kind, one file per family under `src/nodes/`. Every
node run wraps in `ENGINE_PROFILE_DYNAMIC_STABLE("graph node", <node name>,
Render)`: the authored name separates two `raster` nodes in a flame graph.

Acceptance is structural, pinned by `mono.client/tests/WorldPipelines.cpp`.
`Renderer::SetPipeline(name, graph)` compiles, then checks every *enabled*
kind has a backend (`BackendNodes()`), builtin kinds appear at most once
except repeatable ones, scopes match the backend's, `queue` parameters
match, and `culling=occlusion` requires an enabled `gbuffer` to seed the
pyramid. Any failure refuses the whole pipeline with logged reason and
offender, and install falls through to `"Default PBR"`, which is why a saved
world holding a `raytrace` node still gets a frame.

Resources resolve in `ScenePasses.cpp`: well-known names bind renderer-owned
textures (`shadow`, mirror banks, portal pools, PBR slots `albedo`,
`normal`, `material`, `emissive`, `linear-depth`, `occlusion`, `lit`,
`sky-lit`), `window` binds the swapchain, everything writable comes from
`EnsureGraphTarget` keyed `(pipeline, resource, owner)` with owner = view
slot, world id or 0 by scope, which is what makes per-view and per-world
targets distinct allocations of one descriptor.

The default graph is `graph::DefaultPbrDocument()` ("Default PBR"):
`world`, `shadow*`, `camera`, `last-frame`, `entities`, `cull-frustum`,
`order-draw`, `upload-instances`, `mirror-capture*`, `portal-capture`,
`portal-tonemap`, `gbuffer`, `depth-linearise`, `ssao*`,
`deferred-lighting`, `sky`, `tonemap`, `portal-overlay`, `mirror-overlay`,
`transparent`, `present`, `interface*`, `overlay`, `output-image` (`*`
optional). It already runs deferred shading with the HZB two-phase occlusion
inside `gbuffer` and SSAO as an optional insert.

### 1.7 Exact default execution order

The table below is the shortest authoritative reading order for the shipped
default. Rows remain in declaration order. An asterisk means the node is
optional in the document, not that the executor may reorder it.

| Order | Node | Scope | What it does |
|---:|---|---|---|
| 1 | `world` | World | Selects the world and opens its shared inputs. |
| 2 | `shadow*` | World | Captures shadow casters before any view shades them. |
| 3 | `camera` | View | Publishes the view camera. |
| 4 | `last-frame` | View | Makes retained history available to later nodes. |
| 5 | `entities` | View | Produces the stable world draw list. |
| 6 | `cull-frustum` | View | Filters that list against the view frustum. |
| 7 | `order-draw` | View | Orders visible entities into deterministic draw runs. |
| 8 | `upload-instances` | View | Rewrites only dirty resident instance and index ranges. |
| 9 | `mirror-capture*` | View | Renders reflected views into persistent mirror targets. |
| 10 | `portal-capture` | View | Renders visible linked views into portal targets. |
| 11 | `portal-tonemap` | View | Converts portal HDR captures for composition. |
| 12 | `gbuffer` | View | Draws opaque and masked geometry and seeds HZB occlusion. |
| 13 | `depth-linearise` | View | Converts device depth for screen-space consumers. |
| 14 | `ssao*` | View | Computes ambient occlusion when enabled. |
| 15 | `deferred-lighting` | View | Shades the G-buffer into HDR colour. |
| 16 | `sky` | View | Composites the environment behind scene geometry. |
| 17 | `tonemap` | View | Maps HDR scene colour to the presentation range. |
| 18 | `portal-overlay` | View | Composites portal surfaces over the main view. |
| 19 | `mirror-overlay` | View | Composites mirror surfaces over the main view. |
| 20 | `transparent` | View | Draws blended geometry after opaque composition. |
| 21 | `present` | View | Resolves the scene image for presentation. |
| 22 | `interface*` | View | Draws retained game-interface geometry. |
| 23 | `overlay` | Final | Applies the dirty diagnostic-overlay region. |
| 24 | `output-image` | Final | Publishes the final image to its requested sink. |

`graph::Compile` preserves this order. `CompileSchedule` may place independent
work in the same dependency wave, but the current SDL backend records on one
thread and submits transfer, compute, and graphics command buffers in the
planned order. A wave is therefore eligibility for overlap, not a claim that
this backend executed it concurrently.

### 1.8 GPU residency and transfer policy

The renderer does not upload a completed scene or interface image. It keeps
reusable resources resident, uploads changed data, and rasterises on the GPU.
The only full images crossing to the device are source textures or explicitly
edited image content.

| Data | GPU state | Update gate | Current transfer policy |
|---|---|---|---|
| Mesh vertices and indices | Resident shared buffers | Dirty vertex/index spans in `MeshTable` | Coalesced changed ranges only. |
| Draw instances | Resident 48-byte rows | Chunk and row comparison in `InstanceResidency` | Dirty chunks and rows only. |
| Draw-order indices | Resident index stream | Versioned `IndexResidency` ranges | Dirty index ranges only. |
| Static and streamed textures | Resident `TextureTable` entries | Content name, delivery, replacement, or animated-sheet cell | Upload once, then bind by slot; animation changes only the selected cell state. |
| Editable meshes and images | Resident named resources | Per-object `Revision` | Refresh only revisions not already uploaded. |
| Authored shader modules and variants | Resident compiled modules/pipelines | `ShaderSource::Revision`, format, and variant key | Recompile and replace only the changed shader or variant. |
| Game-interface mesh and glyph atlas | Resident vertex/index buffers and atlas texture | Compiled-list signature, atlas change, or capacity growth | Reuse matching geometry; upload changed geometry or atlas content only. |
| ImGui interface | Backend-owned vertex/index buffers plus resident textures | `DrawGeometrySignature` or texture-status change | Upload draw vertices and indices when the signature changes. No CPU-rasterised GUI image is sent. |
| Particles | Resident pool, emitter parameters, curves, and live instances | Layout, resident-parameter, and simulation revisions | Rebuild layout only when layout changes; update resident values and run simulation in place. |
| Beam, trail, decal, and texture ribbons | Frame geometry over resident sampled textures | Authored/effect revision and visible-run contents | Upload compact vertices and runs, not a composed image. |
| Lights, cameras, and per-pass uniforms | Per-frame small buffers | Current view and frame signature | Upload compact structured values because the camera and simulation may move each frame. |
| Shadow, portal, mirror, history, and graph targets | Resident render targets | Descriptor, owner, extent, pipeline reinstall, or explicit release | Render into existing targets; never round-trip their pixels through the CPU. |
| Diagnostic overlay | Resident texture | `Overlay::UploadRegion` dirty rectangle | Upload only the changed rectangle, including the previous showing region when clearing. |
| GPU timing and captures | Normally nonresident on CPU | Explicit profiling collection or capture node | Read back only completed timestamp slots or requested captures. |

Two frequently confused interface paths are intentionally separate. Game GUI
is compiled into retained geometry by `InterfacePass`. ImGui produces CPU draw
lists, and the SDL GPU backend turns those lists into device vertex and index
buffers. Sending a full GUI image would add a large pixel upload, discard GPU
clipping and texture composition, and usually cost more PCIe bandwidth. The
current signature gate already avoids re-uploading unchanged ImGui geometry.

Always-changing diagnostics are isolated from scene and interface signatures.
Statistics labels and flame-graph samples may refresh on their own display
cadence without forcing object, environment, game-interface, or host-interface
cache misses. `PresentationDamage` and the panel refresh deadline are the two
gates to preserve when adding another live counter.

`SurfaceAppearance` follows the same retained path. Colour, normal, roughness,
occlusion, height, metalness, and emissive maps resolve and stream separately,
then bind as seven resident texture slots. Opaque and masked materials enter the
G-buffer; transparent and overlay materials enter the later forward pass.
Metalness is consumed by both lighting paths, and masked shadows use the same
alpha cutoff as the visible material. Surface colour, emission, resampling, and
alpha state are saved and replicated as components, then packed into each
48-byte instance row. Roblox `Content`-object aliases are deliberately absent:
the engine has no `Content` value type beneath such aliases, so the string
content-name properties are the supported boundary.

---

## 2. Invariants the conversion must not break

Each of these was bought with a bug:

1. **`graph` names no device type.** Decisions go in `graph`; pixels go in
   `render`.
2. **Declaration order is the order.** Reads/writes validate; they never
   reorder. Which composite sits above which is authored.
3. **A wrong "hidden" is a hole; a wrong "visible" is a draw call.** Culling
   biases toward drawing everywhere.
4. **Clip convention is Vulkan's** (`GLM_FORCE_DEPTH_ZERO_TO_ONE`, pinned in
   `core`) **and Y is up** (SDL submits negative-height viewports). Do not
   negate `projection[1][1]`.
5. **One thread records.** `Render` aborts off-thread. There is no rebind
   seam, deliberately.
6. **Shadow and surface work draws the world; screen work draws the culled
   set.** Offscreen casters still shadow; the instance buffer's two ranges
   encode this.
7. **A shader failing at runtime is a diagnostic string, never fatal.**
   Built-ins failing at build time are build failures, never runtime ones.
8. **SPIR-V where the device offers both** (MoltenVK takes SPIR-V); MSL gets
   entry point `main0` because Metal reserves `main`. Format, staged file
   and entry point move together in `ShaderBinaryFor`.
9. **No SDL type outside the sanctioned headers, no shaderc type anywhere
   public.** `MeshTable.hpp` and `TextureTable.hpp` are the two named
   exceptions.
10. **An enabled kind with no backend refuses the pipeline loudly at
    install.** Never a silent skip mid-frame.

---

## 3. Finishing the modular node system

Registration is currently split-brained: the catalogue declares sixty-three
kinds, `BackendNodes()` declares thirty-one executor entries with their own
scope/queue copy, and the studio re-derives per-kind editor widgets by hand.
Three sources of one truth drift the first time someone adds a kind to two
of the three. Closing that is stage one of the conversion.

### 3.1 One registration path

Make `graph::NodeKindSpec` the unit and hang everything else off it:

```cpp
struct NodeKindSpec {                 // extended in place
    // existing: Kind, Label, Summary, Category, Inputs, Outputs,
    //           Source, Scope, DefaultShader
    ParamSchema     Params;           // 3.2: typed knobs, drives editor widgets
    Requirements    Needs;            // 3.3: device/shader requirements
};
```

`render` keeps its handler table but derives acceptance from the registry:
`BackendNodes()` becomes a view over `NodeCatalogue::All()` filtered to kinds
that installed a handler. The studio's per-kind widget switch reads
`Params` instead of its hand-written cases. One registration fills all three
consumers.

Rules that keep it safe:

- Registration stays init-only; the catalogue's pointer-stability contract
  (valid until next `Register`) extends to handlers unchanged.
- A kind with no handler is still declarable: CPU-only kinds (`world`,
  `camera`, `entities`, `cull-frustum`, ...) run headlessly through
  `RunEntityNode`, which is why graph tests need no device. Acceptance only
  demands backends for *enabled GPU* kinds in an installed pipeline.
- `RegisterRenderNodeKinds()` remains idempotent; custom registrations below
  must be too.

### 3.2 Custom nodes

Three mechanisms already exist and stay:

- **Authored nodes**: `raster` and `dispatch` take a `source` GLSL parameter
  or a `shader` staged-file name. Reads bind as samplers in slot order (set
  2), storage writes bind as read-write textures, uniforms arrive as
  `GraphPassUniforms` (set 3, binding 0). Compilation is demand-driven,
  cached keyed `(pipeline, node, format, samplers/local sizes)`, wrapped in
  `ENGINE_PROFILE_CAT("compile graph pipeline")` so a hitch is visible as
  itself.
- **Canvas templates**: `nodegraph::Graph::Template` folds a subgraph into a
  placed `custom.node`, saved inside the document. Authoring reuse only; the
  engine still sees the unfolded nodes.
- **Repeatable kinds**: eight kinds may appear many times, which is how a
  custom multi-pass effect is composed today from stock nodes plus authored
  shaders.

Missing is the **native custom kind**: a game or plugin registering a real
kind with real backend behaviour. Design:

```cpp
// once, before any Renderer exists, from game/plugin init:
engine::graph::RegisterNodeKind({
    .Declared = {
        .Kind   = Name("game.dither"),
        .Inputs = {{Name("colour"), ResourceKind::Texture, ResourceFormat::RGBA16F}},
        .Outputs= {{Name("out"),    ResourceKind::Colour,  ResourceFormat::RGBA8}},
        .Scope  = NodeScope::View,
        .Params = {{Name("levels"), WidgetKind::Number, {2, 32}, /*default*/ 8}},
    },
    .Needs  = Requirements{},          // 3.3
});
// then, after Renderer::Initialise:
renderer->InstallNodeHandler(Name("game.dither"),
    [state](const graph::RunContext &ctx, ViewRecording &rec) -> bool {
        // rec.GraphTexture(resource, ctx, make) resolves bindings;
        // OpenScenePass/Fullscreen record into the frame's command buffer;
        // return false abandons the frame, exactly like built-in handlers.
    });
```

Constraints, each inherited rather than new:

- **Names are strings and namespaced.** A kind id crossing a save file is
  `"game.dither"`, never an ordinal. Unregistered kinds survive load drawn
  broken (canvas) and refuse install loudly (engine), which is the same
  two-sided honesty `WorldPipelines.cpp` already pins for `raytrace`.
- **Handlers are render-tier objects.** The registration seam lives on
  `Renderer`; `graph` never learns about them, so headless tests keep
  working and physics/audio can register their own handler tables against
  their own consumers.
- **Parameters stay text at rest**, typed only in `Params` for editor
  widgets; the reader parses, the document stores.
- **A custom kind is one entry in every diagnostic** automatically: dead
  nodes, lossy wires and overspent formats apply to it because diagnostics
  read the declaration, not a builtin list.

### 3.3 ShaderCapabilities

Today capability knowledge is folklore spread across code: `AddShaderVariant`
hard-codes `opaque.frag`'s binding counts (10 samplers, 3 uniform buffers),
compute existence gates `hzb` to a no-op, `VulkanTimestamps::Probe` silently
disables GPU timing, `msl::Translate` decides the shader form, SDL answers
format questions. Nothing names these as a set, so a pipeline author cannot
ask "will this run here" before installing. Design one probe and one
vocabulary:

```cpp
namespace engine::render {

struct DeviceCaps {
    // probed once in Initialise(), immutable afterwards:
    bool HasCompute;              // compute pipelines created OK
    bool HasStorageTextures;      // read-write texture bindings
    bool HasIndirectDraws;        // indirect argument buffers
    bool HasTimestamps;          // VulkanTimestamps::Probe succeeded
    bool UnifiedQueue;           // SDL today: always true
    bool PrefersMSL;             // !SPIR-V path => msl translation needed
    uint32_t MaxSamplersPerDraw; // what AddShaderVariant assumes (10)
    uint32_t MaxColourTargets;   // gbuffer writes four
    std::span<const ResourceFormat> ColourFormats; // RGBA16F etc. creatable
};

struct Requirements {              // declared per kind in NodeKindSpec
    bool Compute = false;
    bool StorageTextures = false;
    bool IndirectDraws = false;
    bool TimestampsUseful = false; // not required; enables timing when present
    std::span<const ResourceFormat> NeedsFormats; // e.g. RGBA16F for HDR chain
    const char *FallbackKind;      // optional substitute kind name
};

// checked at SetPipeline time, alongside backend acceptance:
enum class CapabilityStatus : uint8_t { Ok, MissingCompute, MissingStorage,
                                        MissingFormat, ... };
CapabilityStatus CheckCapabilities(const DeviceCaps &, span<const Requirements>,
                                   core::Name &offender);
}
```

Behaviour rules:

- **Refusal stays whole-pipeline and loud**, exactly like missing backends:
  install falls through to the default rather than half-running. The reason
  string names the requirement ("needs compute for hzb").
- **`FallbackKind` is substitution, never silent degradation**: the compiled
  graph literally contains the substitute node, so captures, profiles and
  diagnostics describe what actually ran.
- **The editor asks first.** Studio renders a requirements row per kind from
  `Needs`, greying kinds the connected renderer cannot run, so an author
  discovers the gap while wiring instead of at install.
- **Shader-level caps ride the same struct.** Authored fragment shaders may
  assume exactly `MaxSamplersPerDraw` samplers and three uniform buffers,
  matching what `AddShaderVariant` and `GraphRasterFor` build today; the
  number becomes data instead of a coincidence between two files.
- **Tier selection uses caps** (§3.6): the default pipeline is chosen per
  device by probing, not by a settings enum.

### 3.4 Compilation stages and caches

The full chain is already staged; what is missing is naming the stages as a
contract and giving each its own cache key:

| Stage | Input | Output | Cache key | Invalidate on |
|---|---|---|---|---|
| parse | document text | `PipelineDocument` | none | file change |
| build | document | `RenderGraph` | none | edit |
| compile | graph | `CompiledGraph` blocks | none | edit |
| schedule | graph | waves | none | edit |
| plan | schedule | command buffer classes | none | edit |
| accept | compiled+schedule+backends+caps | installed pipeline | name | reinstall |
| targets | descriptors | SDL textures | `(pipeline, resource, owner)` | `ReleaseGraphState(name)` |
| pipelines | shaders+formats | SDL pipelines | `(pipeline, node, format, samplers, locals)` | eviction above |
| modules | GLSL text | SPIR-V (+MSL) | script name + `Revision` integer | revision bump |

Two properties to preserve and state outright:

- **Everything above the accept line is pure and testable headless.** That
  is the payoff of the L9/L12 split and the reason bugs are found by tests
  named `Frustum.cpp` rather than screenshots.
- **Eviction is per pipeline.** `ReleaseGraphState(name)` / `ReleaseAllGraphState()`
  drop targets, raster and compute caches and capture receipts together;
  reinstall releases old state after `WaitForFrame`. Custom handlers get the
  same lifecycle hook so they cannot leak across reinstalls.

Hot-swap contract (already true, worth writing down): editing a
`ShaderScript` bumps `Revision`; `ShaderLibrary::Refresh` recompiles on the
next frame; `VariantFor(shader)` swaps the bound pipeline mid-run; failure
yields the previous module plus a diagnostic, never a black screen.

### 3.5 Moving data, and quantising it

Data moves five ways today; the conversion makes each one explicit:

1. **Entity lists on wires** (`ResourceKind::Entities`): CPU index vectors,
   order-bearing, fused cull-and-bound available, ascending output for
   replay determinism. Never leave the process as pointers; if a host
   publishes draw lists cross-process, the payload serialises indices.
2. **Viewpoints on wires** (`ResourceKind::Camera`): fitted light cameras,
   reflected mirror cameras, warped portal cameras; unwired falls back.
3. **Images between passes**: transient targets owned by the graph,
   resolved by well-known name to renderer-owned pools where a pass family
   needs continuity (shadow, portals, PBR slots).
4. **Uploads**: `upload-instances` records instance/texture transfers inside
   a transfer-class command buffer planned ahead of graphics; staging is
   accounted through the tracked `gpu::CreateBuffer/CreateTexture`
   wrappers, so byte counters exist per resource class.
5. **Readback**: `viewer` (preview slot) and `capture` (file, once/every-
   frame) are ordinary sinks reading any resource, which is also the seam a
   profiling readback would use (§6).

Quantisation exists in three layers and gains a fourth:

- **Formats are quantisation declarations.** `RGB10A2` for normals,
  `RG11B10F` HDR without alpha, `R8` AO: choosing a format is choosing bits,
  and `LossyWire` diagnostics fire where a producer spends more than the
  consumer reads. New pipelines should declare the narrowest format that
  survives, per `docs/OPTIMISATIONS_RENDER.md`.
- **Instance packing is done and pinned**: snorm16 quaternion, packed
  colour, 48-byte rows, decode mirrored in GLSL, byte-for-byte tested.
  Vertex quantisation (octahedral normals, box-relative positions) is the
  OPTIMISATIONS_RENDER candidate that lands beside this doc's stage plan.
- **Block-compressed arrival formats** flow through `ResourceFormat` so an
  upload is describable in the profile grid.
- **New: explicit conversion nodes.** Where a wire is lossy by design
  (HDR lit image into LDR grade input), authors insert `blit` with an
  explicit target format rather than relying on implicit narrowing at bind
  time. Rule: **implicit narrowing warns (`LossyWire`), explicit conversion
  nodes are silent.** This keeps "why did my banding change" answerable by
  reading the graph.

### 3.6 The optimised default

The default document should be the best-measured arrangement, selected per
device, not a fixed list. Concretely:

- **DefaultPbrDocument stays the shape** (deferred, HZB occlusion, SSAO
  insert); the OPTIMISATIONS_RENDER candidates land inside it as enabled-by-
  default optional nodes once measured: cascaded shadow maps behind a
  `csm` capture node, clustered lighting behind a `light-cluster` dispatch,
  mip streaming behind residency bookkeeping.
- **Tiers by capability, chosen at install:**
  - Tier A (full caps): default document as written above.
  - Tier B (no compute): `hzb` drops out via `FallbackKind`, occlusion
    falls to CPU-chosen early phase only, SSAO drops, everything else holds.
  - Tier C (no float colour formats): tonemap chain collapses to forward
    opaque + tonemap direct to swapchain; portal/mirror captures persist
    (RGBA8), HDR intermediates go away.
  Tiers are documents too, diffable in the editor, so "what did Tier B skip"
  is a visual answer.
- **Quality scaling does not fork the graph**: `Divisor`s and node
  parameters carry quality (bloom at half res, SSAO quarter), so one
  document serves all machines and a capture states which tier ran.

---

## 4. The shader authoring model

Four shader sources meet in one resolver (`ShaderLibrary::Resolve`), in this
order:

1. A world `ShaderScript` whose name matches: GLSL in `scene::ShaderSource`
   (`Code` + `Revision`, revision-bumped only through the setter), compiled
   at runtime by `ShaderCompiler` (libshaderc, pimpl'd, failure always
   carries a non-empty error string).
2. A built-in staged at build time by `glslc` from `resources/shaders/`,
   read as SPIR-V regardless of device form.
3. Otherwise a loud diagnostic naming the misspelling, like the missing-
   texture marker.

Consumption paths, all live today:

- **Material shaders** (`toon`, `unlit` built-ins and any `ShaderScript` of
  matching name) bind per draw through `DrawSlots`: `VariantFor(shader)`
  returns null for unknown names so a typo draws with the engine shader
  rather than vanishing; a variant join ends a run alongside mesh/texture/
  seam changes because a bind is per-draw cost. `AddShaderVariant` builds
  opaque and blended pipelines together or neither, translating SPIR-V to
  MSL when the device needs it.
- **Node fullscreen/compute shaders**: catalogue `DefaultShader` names a
  staged file; the node's `shader` parameter overrides it; `source` GLSL on
  `raster`/`dispatch` compiles on demand. Samplers bind in read-slot order;
  uniforms are one block; compute declares local sizes via parameters with
  `dispatch.mode` choosing groups versus cover-target.

Rules that stay fixed:

- Fragment-only for authored material shaders: a vertex stage would author
  against the renderer's private instance layout.
- Runtime compile failures return to the author with name and line; engine
  keeps running. Built-in compile failures break the build.
- No shaderc type public; `ShaderCompiler.hpp` is strings in, words out.
- `PostProcessing::Shader` already lets an authored fragment replace the
  tonemap node's program, which is the precedent §5's demo pipelines lean on.

What ShaderCapabilities adds here (§3.3): the binding-count assumptions,
format availability and compute existence stop being implicit and become
probed data the editor can show next to every kind.

---

## 5. Demo pipelines

Each is a `PipelineDocument` over existing kinds plus a small number of new
fullscreen kinds, installable as named entries in the world's `PipelineSet`
and switchable at runtime (`SetPipeline`). All four keep the deferred spine
(`world ... gbuffer, depth-linearise, deferred-lighting`) and differ after
lighting. New kinds proposed: `edges` (depth+normal edge field), `hatch`
(luminance cross-hatch), `palette` (n-band posterise/ramp). Each is one
fullscreen raster pass reusing the `raster` machinery, registered with ports
and default shaders like any stock kind.

### 5.1 Anime

Cel shading with hard terminator and inked outlines.

```
deferred-lighting -> quantise -> edges -> mix(outline) -> tonemap -> output-image
```

| Node | Params | Notes |
|---|---|---|
| `palette` | bands=4, dither=off | luminance posterise of lit image; hard bands are the cel look |
| `edges` | mode=depth+normal, width=1, threshold=med | writes R8 edge field at full res |
| `mix` | amount=1.0, mask=edge field | multiplies ink over colour |
| `tonemap` | curve=ramp (authored) | banded highlight rolloff instead of filmic |

Materials: `toon` built-in already exists for forward-lit props; the
document path above gives the same language to deferred scenes. Optional:
`bloom` disabled deliberately; anime glare uses `blur` + `screen-mix` on a
thresholded highlight instead (softer, wider than bloom).

### 5.2 Cartoony

Flat colour, thick outline, punchy saturation.

```
deferred-lighting -> palette(bands=2..3) -> edges(width=2) -> grade(sat+) -> output-image
```

Differences from anime: fewer bands, wider softer edge (dilate the edge
field one pixel before mix), no ramp curve, `grade` lifts saturation and
clips blacks. Shadow map optional: cartoony reads fine with AO only, which
makes this the cheapest demo pipeline and a good mobile tier test.

### 5.3 Drawing

Pencil-sketch rendering of the lit scene.

```
gbuffer -> depth-linearise -> edges -> hatch -> overlay(paper) -> output-image
```

| Node | Params | Notes |
|---|---|---|
| `hatch` | scale, angle=45, contrast | maps luminance to line density; two rotated fields blended for midtones |
| `blit` | target RGBA8 | paper texture multiply, sRGB |

Edges carry most of the drawing: depth edges dark and thin, normal edges
lighter and wider, mixed multiplicatively before hatching so silhouettes
stay crisp. Colour drains to near-monochrome via `grade(desat=0.9)`; the
paper multiply rides `overlay`'s existing composite rather than a new node.
This pipeline exercises `Divisor` economics honestly: nothing needs HDR.

### 5.4 Anime screencap

The "keyframe" look: graded, gently bloomed, letterboxed.

```
... deferred-lighting -> bloom(soft) -> grade(film LUT-ish) -> sharpen(light)
    -> overlay(letterbox+vignette) -> tonemap -> output-image
```

Keeps full HDR chain (unlike the others), adds `overlay` composition with
authored bars/vignette drawn as an interface-style image, light unsharp mask
after grade so compression-friendly frames stay crisp. This is the pipeline
that proves the graph's retained-image caching: a static camera holds the
composed result until signatures move.

All four documents ship as data in `mono.engine/examples` (or resources),
loadable through the same `LoadRenderPipeline` studio entry, each with one
headless acceptance assertion (builds, schedules, installs or refuses with
the expected reason on Tier B/C devices).

---

## 6. Profiling and introspection

The renderer must answer "what did this frame cost, where, and why did it
change" without attaching anything, and the instrumentation must be
switchable off without editing code.

### 6.1 What exists

| Signal | Mechanism | Notes |
|---|---|---|
| CPU flame graph | every node run wrapped in `ENGINE_PROFILE_DYNAMIC_STABLE("graph node", <name>, Render)` | feeds Tracy, in-game `FrameGraph`, heap tags; authored node name separates instances |
| GPU per-pass time | `VulkanTimestamps`: 128 marks, 4 slots, nonblocking `Collect`; failed probe disables timing entirely | results arrive late and out of order; reported exactly once |
| Reported spans | workers report completed durations via `FrameGraph::Report`/`ReportNamed`/`ReportedScope` | producer-attributed, never spliced into the frame thread |
| GPU memory | tracked wrappers in `gpu::` (`CreateBuffer`/`CreateTexture`...) with `MemoryStatistics` | logical payload: live, peak, cumulative allocated/released, creation counts |
| CPU heap | every `ENGINE_PROFILE` scope opens a heap tag; `ENGINE_HEAP_SCOPE` for allocation-only boundaries; `--heap-report`, `just heap-soak` | hooks compile out of shipped release |
| Static grid | `graph::ProfilePipeline` passes x resources, lifetimes, Peak vs Total bytes | `Elapsed`/`Wall` fields exist awaiting timestamps |
| Cache health | `PresentationCacheProfile` hit/write counters, Frame Graph panel rows | hit means zero uploads and zero transient allocations |

### 6.2 Closing the loop: per-node GPU time into the grid

`ProfilePass::Elapsed` fills from `VulkanTimestamps`:

- Each backend handler that brackets real GPU work marks the command buffer
  around its passes (the marks already exist for frame-level buckets);
  `GraphRunner` assigns mark pairs per node invocation and records
  `(slot, from, to)` beside the run context.
- `Collect` resolves completed slots once per frame; resolved times land in
  three places: the in-game Frame Graph as reported spans under category
  Render (producer-attributed, may overlap, never subtracted from parents),
  `PipelinePass::Elapsed` for the Studio grid column, and the running
  averages the diagnostics use.
- Mark budget: 128 total. Priority order when a frame would exceed it:
  capture nodes (shadow, portals, mirrors), gbuffer/lighting, post chain,
  entity nodes (CPU-timed anyway). Dropped marks are counted and shown, not
  hidden: a partial timing column must not read as complete.
- Late-and-out-of-order is preserved: a timestamp lands on the node whose
  command range produced it, not at wall-clock position; the grid sorts by
  execution order while the flame graph keeps producer attribution.

### 6.3 The introspective surfaces

Three views, one data source:

1. **Studio Render Pipeline panel gains Timing and Memory columns** on the
   profile grid: GPU ms per node (EMA plus last-frame spike), draw calls,
   triangles, and per-target bytes from the tracked wrappers. Rows sort by
   self cost; clicking a row highlights the node in the canvas.
2. **In-game overlay** stays the zero-dependency view: frame graph with
   node spans, GPU buckets, cache-hit rate per viewport, upload bytes per
   frame. Works headless-windowed on any machine, nothing attached.
3. **Capture receipts** extend to profiles: `capture` already writes files;
   add `capture.mode=once` on a `profile-grid` pseudo-sink writing the
   static grid plus resolved timings as text beside the image, so a bug
   report carries its own measurements.

### 6.4 Disable switches

Profiling has three tiers and two escape hatches:

| Tier | What runs | Cost shape |
|---|---|---|
| Off | no timestamp marks acquired; `ENGINE_PROFILE` macros compiled to no-ops (ship release); heap hooks out | zero steady-state; flame graph absent by construction, not empty |
| CPU | spans only; no GPU queries | one scope pair per node; what dev preset measures anyway |
| Full | spans + timestamp marks + memory counters | mark write per bracketed pass; Collect amortised per frame |

- **Runtime switch**, not rebuild: `Renderer::SetProfiling(tier)` between
  frames. Dropping to Off abandons pending slots cleanly (`Abandon`), never
  waits.
- **Per-node opt-out**: node parameter `profile=false` skips the span and
  marks for hot tiny nodes (entity filters) where overhead exceeds value;
  the grid shows such rows as unmeasured rather than zero, keeping absence
  honest.
- **Sampling mode** for steady state: resolve timings every Nth frame
  (default 4) and hold last values; halts the driver round-trip cost during
  soak tests while keeping numbers visible.
- **House rules unchanged**: idle waits marked Idle, not missing; metrics
  read to report, never steer behaviour mid-frame; published numbers name
  preset, backend, scene and settings; a hit rate climbing while upload
  bytes grow is a bug, not a win.

### 6.5 Heap discipline for the graph specifically

- `EnsureGraphTarget` allocations and releases are the natural
  `ENGINE_HEAP_SCOPE` sites (allocation-heavy, rarely worth timing).
- Per-frame transient churn reports as cumulative allocated versus flat
  live bytes; a growing live slope across steady frames is the leak signal
  `heap-soak` fits.
- GPU-side: live versus peak versus cumulative from `gpu::MemoryStatistics`;
  presentation-cache hits must show zero uploads and zero new targets or
  the cache is lying.

---

## 7. Migration stages

Independently shippable, each leaving the tree green:

1. **One registration path** (§3.1): fold scope/queue metadata and
   requirements into the catalogue; derive `BackendNodes()`; studio widgets
   read `Params`. Tests: existing acceptance suite must pass untouched;
   add one asserting registry and backend agree on every kind.
2. **DeviceCaps probe + CheckCapabilities** (§3.3): probe in Initialise;
   wire refusal messages; studio requirements column. Headless tests pin
   the checker; the probe itself is client-tested by running the client.
3. **Custom native kinds** (§3.2): `RegisterNodeKind` +
   `Renderer::InstallNodeHandler`; lifecycle hook on reinstall; one demo
   custom kind in examples with its own suite.
4. **Per-node profiling** (§6.2): mark assignment in GraphRunner, grid
   column, tier switch. Assert dropped-mark accounting in the Frame Graph
   panel rows.
5. **Conversion nodes + narrowing rule** (§3.5): explicit `blit` format
   targeting; `LossyWire` demoted to hint when an explicit conversion sits
   between producer and consumer.
6. **Tiered defaults** (§3.6): Tier B/C documents; capability-driven pick
   at install; WorldPipelines extension asserting fall-through reasons.
7. **Demo pipelines** (§5): new kinds (`palette`, `edges`, `hatch`),
   example documents, acceptance assertions.
8. **OPTIMISATIONS_RENDER candidates** land behind the same optional-node
   pattern afterwards, measured in release each time, in the priority order
   that document already states.

Stages 1 to 4 unblock everything else; none of them changes what a frame
looks like, which is the point: the conversion is mechanical first,
expressive second, fast third.
