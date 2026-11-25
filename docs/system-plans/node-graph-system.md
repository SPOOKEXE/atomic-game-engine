# Arbitrary node graph system plan

## Status

Implemented, 2026-11-25: phases 0 to 5 and migration rows 1 to 6 and 8 to 11.

- Row 11: `CompileEngineGraph` orders stages through an ang Tick graph.
  Execution stays a plain loop with ang hooks, timeline and report through
  `EngineObserver`, because an ang Runner cost 9.6 to 18.3 us per 64 nodes per
  view against 0.2 to 1.4 us.
- Row 7 (`render` GraphRunner) is not done, by decision: it would add a second
  per-node layer to the same frame path.

This plan adds one general node graph library (ANG) and moves the
engine's existing node systems onto it. The chain is:

```
ANG (model, compile, run, observe) -> engine hosts -> simulation / renderer / scripting / Studio
```

It also fixes two live defects that the shared view will own afterwards:

- the Studio render pipeline graph cannot be selected, dragged or panned;
- the runtime GUI node canvas demo cannot drag nodes, drag wires or pan.

Those fixes ship first (phase 0) and do not wait for the library.

## Goals

1. One model for typed ports, node definitions, links and type compatibility.
2. One compiler for order, waves, cycles and limits (`graphcore` already does
   most of this and becomes the kernel).
3. One runner with three execution modes: synchronous, parallel inside a tick,
   and asynchronous across frames.
4. Hooks before and after each node and before and after each run.
5. Per-node and per-run timing that feeds the existing `FrameGraph`, plus a
   graph-local timeline a view can draw.
6. Per-node and per-run memory accounting with budgets.
7. A built-in `NodeGraph` script library in Luau and JS that can define node
   types, build graphs, run them and draw them.
8. One headless view model, `NodeGraphView`, with theme presets and two
   painters (Dear ImGui for Studio, `gui` DrawList for games). Scripts can
   restyle it or paint their own.
9. Existing systems migrate one at a time, and the build stays green after
   each step.

## Non-goals

- No visual scripting language for gameplay logic in this plan. A script can
  build one on top of ANG, but the engine ships no control-flow node set.
- No change to on-disk formats in the first migration step of any system. Each
  system keeps its reader and writer and adapts to the ANG model in memory.
  A shared document format comes later and only where two formats are already
  near copies (`graph::PipelineDocument` and `bakegraph::Document`).
- No undo inside ANG. Hosts own undo, as `mono.studio` does now. ANG emits
  edit records a host can store.
- No multi-user editing of graphs.

## Current state

Nine node-graph systems exist. They share `graphcore` for ordering in most
cases, and duplicate almost everything else.

| System | Layer | Node kind id | Evaluator | Notes |
|---|---|---|---|---|
| `graphcore` | L2 shared | text | none | schema, port compare, cycle refuse, waves, limits |
| `graph` EngineGraph | L9 shared | fixed stage enum | waves via runner | per-tick, Concurrent or Deterministic |
| `graph` NodeSchema/NodeRuntime | L9 shared | `core::Name` | sync or dispatcher with `CompleteAsync` | dirty-closure cache, bypass |
| `graph` RenderGraph/Pipeline | L9 shared | enums | list, not resolver | `PipelineProfile` time and bytes |
| `computegraph` | L3 shared | named decls | render host | hard byte limits |
| `bakegraph` | L9 shared | on-disk enum | in `bake` | text doc with edit log |
| `imagegraph` | L9 shared | text | CPU, deterministic | JSON, `Fingerprint`, retained bytes |
| `procedural` | L3 shared | `StableText` | script jobs | 32-byte `Digest` |
| `audio` | L12 client | enum | serial | summing order is fixed |
| `gui` NodeCanvas | L7 shared | `core::Name` | none | ECS entities, own compat and cycle check |
| `mono.studio/nodegraph` | none, client | text | own `std::thread` pool, never blocks | `std::any` payloads, `Graph::Hash`, ImGui canvas |
| `render` GraphRunner | L12 client | `std::function` table | per frame | `NodeProfileHooks` |

Duplication to remove:

- four port compatibility checks with two wildcard spellings (`any`,
  `data.ANY`): `graphcore::ComparePortTypes`, `graph::NodeTypeCompatibility`,
  `gui::NodeCanvasTypesCompatible`, `nodegraph::DataTypes::CanConnect`;
- three cycle and input-limit checks (graphcore, gui, nodegraph);
- two data-flow evaluators with dirty caching (`graph::NodeRuntime`,
  `nodegraph::Evaluator`);
- four content identity schemes (`Graph::Hash`, `Fingerprint`, `Digest`, dirty
  flags);
- three profiling shapes (`PipelineProfile`, `NodeProfileHooks`, `RunReport`);
- bypass mapping in three places.

## Where it lives

```
L2  graphcore   (exists) schema, port compare, compile, waves, limits
L3  ang         model, registry, runner, hooks, observe   links: core, parallel, graphcore
L3  angview     headless view model: layout, hit test, interaction, theme   links: core, ang
L7  gui         NodeCanvas becomes an angview host + DrawList painter
L9  script      NodeGraph library (neutral ScriptCall rows)
L12 ui          ImGui painter for angview
    studio      render pipeline, image composer, asset pipeline editors
```

`ang` and `angview` are `shared` so a headless server and tests can build,
run and hit-test graphs with no GPU and no ImGui. Nothing in either includes
`imgui.h`.

`graphcore` stays the pure compile kernel. ANG adds the runtime half on top
instead of growing `graphcore`, so modules that only need ordering (audio)
keep linking only `graphcore`.

`mono.studio/nodegraph` is retired at the end of phase 5. Its `AGENTS.md`
rule "depends on nothing of ours" was right for an editor widget library. It
stops being right once the same model must run in a server, a game and a
script. The retirement diff must update that file and `expected_graph.json`
together, and remove `just check-one-node-graph` or point it at `ang`.

## Model

### Types

```cpp
namespace engine::ang {

// Durable text id. Never an ordinal (root AGENTS.md rule 4).
struct TypeId { std::string Text; };

struct DataType {
    TypeId Id;
    std::string Label;
    Colour Wire;                              // view hint, no ImGui type
    std::vector<TypeId> ConvertsTo;           // implicit conversions
    std::size_t (*Bytes)(const Value&) = nullptr;   // memory accounting
    PreviewFn Preview = nullptr;              // picture belongs to the wire
};

}
```

Rules carried from the existing libraries:

- `any` is the one wildcard, spelled once. `data.ANY` reads as `any` on load.
- An unregistered type connects to nothing.
- Compatibility is `graphcore::ComparePortTypes` plus `ConvertsTo`. A link
  through a conversion records the conversion so the runner inserts it.
- Generic ports: a port can be typed `T` with a constraint list. The first
  link resolves `T` for every port on that node that names `T`. This is the
  only generic feature. No type-level computation.

### Values

`Value` is a small tagged union for scalars (bool, int64, double, vector2/3/4,
colour, string, `core::Name`) and an owning `Blob` for anything else, with
`TypeId` and a byte count. It replaces `std::any` (nodegraph),
`NodeOpaqueValue` (graph) and the scalar token set. Hosts register a
`TypeId` and store their own object behind `Blob`. Scripts see scalars as
native values and blobs as opaque userdata.

### Node definitions

```cpp
struct PortDef {
    std::string Name;          // durable within the node type
    TypeId Type;
    Direction Dir;
    bool Optional = false;
    uint16_t MaximumLinks = 1; // 0 = unlimited, inputs default to 1
    Value Default;             // used when unlinked
};

struct NodeDef {
    std::string Id;            // durable, e.g. "image.blur"
    uint32_t Version = 1;
    std::string Category, Label;
    std::vector<PortDef> Inputs, Outputs;
    std::vector<ParamDef> Params;          // widget hints, no widget code
    ExecMode Mode = ExecMode::Sync;        // Sync, Parallel, Async
    Purity Pure = Purity::Pure;            // Pure results may be cached
    EvaluateFn Evaluate;                   // native
    MigrateFn Migrate = nullptr;           // Version n-1 -> n
    std::optional<BypassMapping> Bypass;
};
```

`NodeDef` lowers to `graphcore::NodeSchema` for compile. A host registers
definitions into a `Registry` it owns. There is no global registry; Studio,
the client and each script context hold their own, so a script cannot change
engine node types.

`Version` and `Migrate` close the gap `mono.studio/nodegraph` names under
"no node type versioning". An unknown node type still loads as a node with no
evaluation and reports `Skipped`.

### Graph document

`Graph` holds nodes (stable `NodeId` text, def id, version, params, position,
optional group), links (from node/port to node/port) and groups/comments.
Edits go through `Graph::Apply(Edit)` and return an `EditRecord` with the
inverse, so a host can build undo without ANG owning it.

`Graph::Hash` is kept per node and covers def id, version, params and
upstream hashes. Nothing time or pointer based goes in. This replaces
`Graph::Hash`, `Fingerprint` and `Digest` for cache keys. Systems whose
digest is on disk (`procedural`) keep it as their saved identity and use the
ANG hash only for runtime caching.

Subgraphs: a node can reference another graph by id with declared input and
output ports. Compile flattens them. Recursion is refused at compile.

## Compile

`ang::Compile(graph, registry, limits)` calls `graphcore` for order and waves,
then adds:

- conversion nodes for converting links;
- generic type resolution;
- per-node execution mode and the wave each node lands in;
- a dirty map so a param edit recompiles only the closure below it.

A refused compile keeps the previous `Plan` live and returns bounded
diagnostics, as `graphcore` does now.

## Runner

### Three modes and the tick rule

Root `AGENTS.md` rule 5: work inside a tick may be parallel, work across
ticks may not. Simulation must stay deterministic. The runner encodes that as
a graph-level domain chosen by the host, not by node authors:

| Domain | Allowed node modes | Who uses it |
|---|---|---|
| `Tick` | Sync, Parallel | EngineGraph, simulation, server, replicated script graphs |
| `Frame` | Sync, Parallel, Async (GPU) | renderer, audio, client presentation |
| `Tool` | Sync, Parallel, Async (CPU workers) | Studio editors, bake, image composer, local script tools |

- Sync runs on the calling thread in order.
- Parallel runs a wave's independent nodes through `Jobs::For` and joins
  before the wave ends. Nested dispatch is already serial in `parallel`, so a
  parallel node that itself calls `Jobs::For` is safe.
- Async hands the node to a worker and returns. `Run` never blocks. A node
  whose input is pending waits for the next `Run`. This is the
  `nodegraph::Evaluator` model and only exists in `Tool` and `Frame`.

Compile refuses an Async node in a `Tick` graph with a diagnostic naming the
node. That is the line that keeps cross-tick work out of simulation.

Async workers: ANG owns one bounded `AsyncPool` (worker count from config,
default physical cores minus pinned job workers). It is not `Jobs`, because
`Jobs` work must finish inside the tick. Cancellation is cooperative through
`RunContext::Cancelled()`. A param edit cancels in-flight work for the dirty
closure.

### Run API

```cpp
Runner runner{plan, Domain::Tool};
runner.Hooks().PreRun  = ...;
runner.Hooks().PreNode = ...;
RunReport report = runner.Run(inputs);   // non-blocking in Tool/Frame
const Value* out = runner.Output("blur1", "image");
```

`RunReport` keeps `Ran`, `Cached`, `Skipped`, `Waiting`, `Failed` as
separate counts, plus per-node `NodeStatus` and note, as nodegraph does now.
Errors go to status and report, never only to a log.

### Hooks

```cpp
struct Hooks {
    std::function<void(const RunInfo&)>            PreRun, PostRun;
    std::function<void(const NodeInfo&)>           PreCompute;
    std::function<void(const NodeInfo&, Result&)>  PostCompute;
    std::function<void(const NodeInfo&)>           PreNode, PostNode;
};
```

- `PreNode`/`PostNode` fire for every node visit, including cache hits and
  skips. `PreCompute`/`PostCompute` fire only when `Evaluate` actually runs.
  That split is the difference between "the graph walked here" and "work
  happened here".
- Hooks fire on the thread that ran the node. A hook used by a Parallel or
  Async node must be thread safe; script hooks are therefore queued and
  delivered on the script thread after the wave or on the next `Run`.
- `PostCompute` may replace the result (debug overrides, value capture for
  the view's wire inspector).
- Empty hooks cost one branch. Measure in `release` and record the number in
  a comment next to the check.

`render::NodeProfileHooks` and `graph` EngineNode `Profile` become ANG hooks.

### Timing

Every node visit records begin, end, thread and outcome into a per-run
`RunTimeline` (bounded ring, last N runs). Feeds:

- `FrameGraph`: Sync and Parallel nodes use the existing profile scopes; work
  on workers reports after the join with `ReportNamed`, as
  `AGENTS.md:315-360` requires. Async nodes report on the `Run` that
  collects them.
- `angview`: draws per-node time on the node and a timeline strip under the
  canvas.
- Studio: `RenderPipelineProfile.cpp` reads `RunTimeline` instead of its own
  shape.

### Memory

Each node output records bytes from `DataType::Bytes`. The runner tracks:

- per node: current, peak, cached;
- per run: live, peak, retained cache total;
- per graph: budget. Over budget, the cache evicts least recently used pure
  outputs first; if still over, the run fails the next allocating node with a
  `Budget` status rather than growing.

Allocation inside `Evaluate` is tagged with `ENGINE_HEAP_SCOPE` under the
node id, so `HeapProfile` shows it. `computegraph`'s hard caps and
`imagegraph`'s retained bytes become ANG budgets with the same numbers.

## View: `angview` and `NodeGraphView`

### One interaction model

Both reported UI bugs are the same missing piece: the interaction state
machine lives in the painter (ImGui canvas) or nowhere (gui NodeCanvas).
`angview` holds it headless:

- inputs: pointer down/move/up per button, wheel, keys, modifiers, in view
  space;
- state: pan, zoom, selection, hover, active drag (node, wire, box select,
  pan), pending wire with type-checked target highlight;
- outputs: edits (`Graph::Apply`), redraw flag, a `DrawCommands` list in
  graph-neutral shapes (rect, rounded rect, bezier, text, image handle).

Default bindings: left-drag node moves it, left-drag from a port pulls a wire,
drop on a compatible port links, drop on empty space opens the add-node
search filtered by type, left-drag empty space box-selects, middle-drag or
Alt+left-drag or space+left-drag pans, wheel zooms about the cursor, F frames
selection, Delete removes. Touch: one finger behaves as left, two fingers pan
and pinch zoom.

Hit testing and layout are in `angview` so tests can drive a drag with no
renderer, which is the test the demo never had.

### Painters

- `ui`: ImGui painter. One `InvisibleButton` over the canvas, forwards input,
  paints `DrawCommands` into the window `ImDrawList`. Replaces
  `nodegraph::Canvas`.
- `gui`: `NodeCanvas` becomes an `angview` host. The existing ECS entities
  (canvas, node, port, link) stay as the saved and replicated form; the
  canvas component owns an `angview::View`, and `gui` `Input.cpp` routes
  pointer events for a `NodeCanvas` into it. `NodeCanvasTypesCompatible` and
  the gui cycle check are deleted in favour of ANG.

### Themes

`Theme` is plain data: background, grid, node body/header/border per state,
port shape per type, wire width and style (bezier, straight, orthogonal),
font scale, timing overlay colours. Presets ship as data:

- `Dark` (true black background, white text; default)
- `HighContrast`
- `Blueprint`
- `Minimal` (no headers, no grid)

Studio maps its `ui::Theme` palette onto the ANG theme so the editor follows
the user's palette. `gui` maps `UITheme` tokens. Scripts can pick a preset,
override fields, or set `View.Paint` to their own function that receives the
`DrawCommands` and the graph and draws with `gui` primitives. Theme changes
are not animated; nothing in the view repaints unless state changed.

## Script library: `NodeGraph`

Exposed in both Luau and JS through neutral `ScriptCall` rows in `script`,
installed by `scriptluau` and `scriptjs`, typed in `engine.d.luau`,
`engine.d.ts` and `api-dump.json`.

```lua
local NG = NodeGraph

local reg = NG.Registry.new()
reg:DefineType("number", { Wire = Color3.new(1, 1, 1) })
reg:DefineNode({
    Id = "math.add",
    Inputs  = { { Name = "a", Type = "number" }, { Name = "b", Type = "number" } },
    Outputs = { { Name = "sum", Type = "number" } },
    Evaluate = function(ctx, i) return { sum = i.a + i.b } end,
})

local g = NG.Graph.new(reg)
local a = g:AddNode("math.add", { b = 2 })
local b = g:AddNode("math.add")
g:Link(a, "sum", b, "a")

local runner = NG.Runner.new(g, "Tick")
runner.PreNode:Connect(function(info) end)
local report = runner:Run({ [a] = { a = 1 } })
print(runner:Output(b, "sum"), report.Ran, report.Milliseconds)

local view = NG.View.new(g, { Theme = "Blueprint" })
view.Parent = screenGui   -- a NodeCanvas under PlayerGui
```

Rules:

- Script-defined `Evaluate` functions run on the script thread only, so they
  are Sync in every domain. Parallel and Async are available to scripts only
  through native node types the host registered.
- A script graph in `Tick` domain on the server is deterministic and may be
  replicated as a document. Script `Evaluate` inside it must obey the
  existing `ReadOnlyDepth` rules: a `Pure` script node runs read-only, the
  same guard `ForEachParallel` uses.
- `runner:RunAsync()` in `Tool` or `Frame` returns a promise in JS and yields
  in Luau, using the existing `call.Suspended()` path.
- Engine graphs (image graph, procedural recipe, render pipeline on the
  client) are reachable as `NG.Graph` objects with the host's registry, read
  only unless the host marks the graph editable. This replaces the JSON-text
  `ImageGraphSetDocument` path as the primary authoring route; the JSON path
  stays as an import.
- Limits: node count, link count, value bytes and run time per call are
  bounded per script context and refused with a script error.

## Migration of existing systems

Each step deletes the old code it replaces in the same change. No adapters
left behind "for later".

| Order | System | Change |
|---|---|---|
| 1 | `gui` NodeCanvas | host `angview`, delete own compat and cycle checks |
| 2 | `mono.studio/nodegraph` render pipeline editor | ImGui painter + `angview`; `RenderPipelineGraph.cpp` registers `NodeDef`s |
| 3 | image composer + plugin dock graph | same as 2; `PluginImageGraph.cpp` uses the shared painter |
| 4 | `imagegraph` evaluator | node kinds become `NodeDef`s; `Fingerprint` becomes ANG hash; JSON reader kept |
| 5 | retire `mono.studio/nodegraph` | delete module, update `expected_graph.json` and checks |
| 6 | `graph` NodeSchema/NodeRuntime | replaced by ANG runner; `NodeTypeCompatibility` deleted |
| 7 | `render` GraphRunner | `NodeTable` becomes registry; `NodeProfileHooks` become ANG hooks; `Frame` domain |
| 8 | `procedural` recipes | `NodeDef`s, `Tick` domain; `Digest` kept as saved identity |
| 9 | `bakegraph` + `graph::PipelineDocument` | one shared text document reader; on-disk `NodeKind` ordinals map to text ids at load |
| 10 | `computegraph` | kernels as `NodeDef`s; caps as budgets |
| 11 | EngineGraph | stages as nodes in a `Tick` graph; resource lifetimes as port types |

`audio` stays on `graphcore` only. Its serial summing order is a fixed
requirement and it gains nothing from the runner. It may adopt `angview` for
a Studio mixer view later without changing its runtime.

## Phases

### Phase 0: fix the two UI defects now

Studio render pipeline (`mono.studio/src/RenderPipeline.cpp:68-71`,
`RenderPipelineSession.cpp:22-58`):

1. Confirm the cause: `LoadRenderPipeline` runs every frame when the active
   world is invalid or the load fails, because the trigger is
   `Nodes().empty()`. Each reload calls `Select(NO_NODE)` and `Fit()`, which
   drops any drag and resets the view.
2. Reload only when the active world id changes or the user asks. Store the
   last load result and do not retry a failed load every frame; show the
   error in the panel instead.
3. If input is still dead after that, add hover and active-id diagnostics
   around `nodegraph/src/Editor.cpp:1590-1632` and check the plugin dock
   host and `PaletteOpen`/`MenuOpen` state.
4. Test: load a pipeline into a headless canvas state, run two frames, assert
   selection and view survive.

GUI node canvas demo (`mono.engine/gui/src/Input.cpp`,
`examples/assets/scripts/NodeCanvas.luau`,
`DemoCameras/NodeCanvas.client.luau`):

1. In `Input.cpp`, route pointer down/move/up over a `NodeCanvas`: drag on a
   node moves `Position` (divided by zoom), drag on empty space changes
   `Pan`, drag from a port draws a pending wire and links on release with
   the existing `NodeLinkResult` checks.
2. Keep click-then-click connect as a fallback.
3. Replace the `WaitForChild(..., 10)` asserts in the client demo with
   waits that do not kill the script.
4. Test in `examples/tests`: drive pointer events through `Router` and
   assert node moved, pan changed, link created.

This engine-side drag code is the seed of `angview` and moves there in
phase 2 without changing behaviour.

### Phase 1: `ang` model and compile

- Module scaffold through the `new-module` skill: `mono.engine/ang`, L3
  shared, links `core`, `parallel`, `graphcore`.
- `TypeId`, `DataType`, `Value`, `PortDef`, `NodeDef`, `Registry`, `Graph`,
  `Edit`/`EditRecord`, hash, `Compile` with conversions and generics.
- Tests with a fixture registry of its own (same rule as nodegraph: never
  import a consumer's node set).

### Phase 2: runner, hooks, timing, memory

- `Runner` with `Tick`/`Frame`/`Tool` domains and Sync/Parallel/Async modes.
- `AsyncPool` with cancellation.
- Hooks, `RunTimeline`, `FrameGraph` reporting, byte accounting, budgets and
  eviction.
- Tests: wave parallel result equals serial result; Async in `Tick` refused at
  compile; hash settles (no per-frame recompute); budget eviction order;
  hook order and pre/post pairing.
- A release benchmark for empty-hook overhead and Parallel crossover, number
  written next to the code.

### Phase 3: `angview` and painters

- Headless view: layout, hit test, interaction state machine, `DrawCommands`,
  theme data and four presets.
- ImGui painter in `ui`. `gui` painter and `NodeCanvas` host in `gui`; move
  the phase 0 drag code in.
- Tests drive drags, wires, box select, pan and zoom headlessly.

### Phase 4: script library

- `NodeGraph.Registry`, `Graph`, `Runner`, `View` in `script` as neutral
  rows, installed in Luau and JS, typings and api dump updated.
- Script hooks delivered on the script thread; `RunAsync` via suspension.
- Example place: a script-built graph drawn by `NodeGraphView` with a preset
  theme and a script-painted variant, replacing the current NodeCanvas demo.
- Tests in `scripthost` for both languages: define, build, link refusal on
  type mismatch, run, hooks, limits, read-only guard.

### Phase 5: Studio editors move and `nodegraph` retires

- Render pipeline, image composer and plugin dock graph on ANG + ImGui
  painter. Studio undo stores `EditRecord`s.
- Render pipeline view shows per-pass time and bytes from `RunTimeline`.
- Delete `mono.studio/nodegraph`, its check, and its graph row.

### Phase 6+: engine systems

Migration table rows 4 and 6 to 11, one per change, each with its old
evaluator, compat check or profile shape deleted.

## Testing

Per module, kept small and real:

- `ang`: compile, compat, conversions, generics, modes, hooks, budgets, hash.
- `angview`: interaction by synthetic input.
- `script`: one suite per language for the public surface.
- Studio: render pipeline reload regression from phase 0.
- One integration test per migrated system that runs a real saved graph
  through ANG and compares output with the pre-migration golden.

No per-node smoke tests for every registered type.

## Risks

- Threading. Async across frames is the one mode that can break determinism.
  The domain rule refuses it at compile in `Tick` graphs; do not add an
  override flag.
- Hook cost on hot graphs (render, EngineGraph). Measure before migrating
  rows 7 and 11; if empty hooks show up, compile them out per plan.
- `Value` blob copies. Outputs are moved into the cache and borrowed by
  readers; a copy on read is a bug.
- Scope. Each migration row is its own change. If a row stalls, the engine
  still ships with the systems already moved.

## Open questions

- Should `angview` own a minimap and a node search palette, or leave both to
  hosts? Default in this plan: search in `angview`, minimap later.
- Should a script graph in `Tick` domain replicate as a document by default,
  or only when parented under a replicated instance? Default: only when
  parented, same as other instances.
