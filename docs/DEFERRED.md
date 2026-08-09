
# DEFERRED

## Format

Each section has the header `[_] D00000`.

The numerical is a counter that increments every item.

Insert the NEWEST items at the front **of `## Deferred Items`**, so older ones
are towards the back. The section is named because "the front" on its own is the
front of this *format example*, which is where D00025 through D00032 spent v0.9
and v0.10 — eight real entries, the newest of them, rendering as a code sample.
Nothing checks this: a fenced block is valid Markdown whatever is inside it.

If a deferred item no longer exists, say the related code was deleted, then mark with [DELETED] flag.

```
### [_] D00102

**The Assets Pipeline panel draws an empty document, and the blocker is a
dependency decision rather than the editor.**

`Editor::DrawAssetsPipeline` lays out a default-constructed `bake::Document`
every frame and says so on screen. The canvas, the layout and the document
format all exist and are tested — `bake::GraphDocument` records edits, replays
them and round trips. What is missing is that **a world does not carry one**.

- **The obvious fix is wrong, and the roadmap says so in writing.** The render
  half landed by giving `game` a dependency on `graph`, which is cheap: `graph`
  is arithmetic over names and links no device. `bake` is not that shape — it
  carries the PNG, JPEG, GIF, BMP, OBJ, GLTF and PMX decoders, so `game`
  depending on it to parse a *text document* pulls every image and model decoder
  into the save format, and therefore into `server`, which links `game` and has
  no reason to decode a JPEG.
- **Three ways out, and the roadmap ranks them.** (a) Split `bake::Graph`,
  `Document` and their format out of the decoder library into something `game`
  can link — "the honest fix and a real refactor". (b) Carry asset pipelines as
  opaque text the save format never parses, which costs the load-time validation
  the render half has. (c) Put them on the universe rather than the world, and
  load them only where `bake` is already linked.
- **Settled: (a), the module split.** The roadmap called it the honest fix and
  that is the call. (b) buys speed by giving up the load-time validation the
  render half has, which would make the two halves of one editor behave
  differently for no reason a user could see. (c) rests on a premise nobody has
  established — whether a bake chain shared by four worlds is any one world's
  property — so it trades a dependency question for an ownership question that
  is harder.

  **The shape:**

  ```
  Engine::bakegraph   TIER shared, no decoders
      Graph.hpp / GraphDocument.hpp / the text format

  Engine::bake    ->  DEPS Engine::bakegraph + the decoders
  Engine::game    ->  DEPS Engine::bakegraph        <- new, and cheap
  ```

  `server` links `game` and gets no JPEG decoder, which is the property the
  whole question was about.

  **What it touches**, in the order it wants doing: the new module and its
  CMakeLists; `bake` losing those files and gaining the dep; `nodeview` and
  `mono.studio` following the headers; `game::WriteWorldBody` / `ReadWorldBody`
  emitting and reading the block, the way the render half emits `<Pipelines>`;
  and `mono.tools/architecture/expected_graph.json`, which will fail on the new
  edge until it is told — as it is there to.

  **One thing to decide while doing it, not before:** what a world does when it
  loads a document naming a node kind this build does not have. The render half
  refuses the pipeline and keeps the world, with a warning; matching that is
  probably right and is a one-line argument once the code is in front of
  somebody.
- **The panel's own pointer was wrong**, which is how this entry came to exist.
  It cited `D00039`, which is about the physics module having no caller. Fixed
  to point here.

**Reopen trigger: none needed — it is v0.11 roadmap work and blocked on a
decision, not on effort.** The render half of the same line is done, so this is
the remaining half of "many node trees in one editor".

### [_] D00101

- item 1
- item 2
- item 3
```

and for deleted marked items;

```
### [DELETED] D00001

- item 1
- item 2
- item 3
```

## Deferred Items

### [_] D00103

**Per-pass GPU time cannot be measured, and the blocker is the vendored SDL
rather than anything in this repository.**

`ProfilePass::Elapsed` stays zero and the Pipeline Profile panel says **not
measured**, which is the honest state. This entry exists so that "somebody
should just fill that in" is answered once, in the place blocked work lives,
rather than sitting in `PIPELINE_TODO.md` as an open box inviting an attempt
that cannot succeed.

- **Verified against the vendored source, not assumed.** SDL 3.2.31 is what
  `mono.vendor/sdl` is pinned to. `SDL3/SDL_gpu.h` contains no timestamp query,
  no query pool and no `SDL_GPUQuery` of any kind, and the Vulkan backend
  contains zero references to `vkCmdWriteTimestamp` or `VkQueryPool`. There is
  no call to make.
- **Do not fill it with CPU time.** A submit-side number in a field labelled as
  the pass's cost is worse than a blank: somebody reads "0.4 ms" for the shadow
  pass, believes the GPU said it, and spends an afternoon optimising the wrong
  thing. The panel saying "not measured" is a feature.
- **The readable half already landed.** `SDL_PushGPUDebugGroup` names every node
  in a capture, so RenderDoc, Nsight and Xcode attribute every draw to its pass.
  What is missing is only the numbers.

**Two ways out, and both are the user's call rather than a code change.**

- **(a) Wait for upstream.** SDL adds timestamp queries to the GPU API, the
  submodule moves, and `Renderer` gains a few lines. No divergence, no cost, and
  no control over when.
- **(b) Fork the submodule.** `mono.vendor/sdl` is a git submodule pointing at
  `libsdl-org/SDL`, so this is not a patch — it is repointing the project at a
  fork of SDL that this repository maintains. Commits made inside the submodule
  without that are unreachable by anyone else who clones, because the parent
  records a SHA that exists on no remote. It also means per-backend code —
  `vkCmdWriteTimestamp` and a query pool for Vulkan,
  `ID3D12GraphicsCommandList::EndQuery` for D3D12 — in a module whose whole
  point is not being per-backend, and a rebase burden on every SDL update
  forever.

**Trigger:** an SDL release that ships timestamp queries, or a decision to
maintain a fork. Nothing in this repository moves it.


### [CLOSED] D00047

**Readbacks: the viewer node's image, channel histograms, and an overdraw view.**

`PIPELINE_NODES.md` stage 8, and the three faults its §1.5 cannot reach without
a path off the GPU.

- **Faults 3, 4 and 9 all need the same thing.** Is this alpha channel blank; is
  this whole target uniform; how many times was this pixel shaded. Each is a
  reduction over a rendered target, and none is answerable from a declaration —
  `PipelineDiagnostics::UnusedAlpha` gets as close as a declaration can, which
  is "nothing is *arranged* to read it".
- **The `viewer` node is already in the catalogue** and does nothing, because
  showing what is on a wire means reading a target back and putting it
  somewhere. That is the same machinery, and it is the cheapest first user of it.
- SDL_GPU has the download path; what is missing is the fence discipline, because
  a readback that waits is a stall and a readback that does not is a frame late.
  A frame late is fine for a debug view and is worth saying out loud.
- **The arithmetic half is built** — `engine.render.readback`, with no device in
  it. `render::Histogram` answers faults 3 and 4 (`Constant`, `Blank`,
  `ImageHistogram::Uniform`), and `render::PendingReadback` is the fence policy:
  one download in flight, never stall, and report the age *from the request*
  rather than from the fence. Eight cases, four mutations checked red.
- What is left is the device half — a transfer buffer that outlives the frame and
  `SDL_QueryGPUFence` polled on a later one; the `viewer` node, which is one blit
  and the cheapest first user of all this; and overdraw, which is the odd one out
  because it needs a pass that *counts* rather than shades — additive blend into
  an `R8`, no depth write, its own pipeline and shader.

### [_] D00046

**Per-pass GPU timestamps, which `SDL_GPU` cannot express.**

`PIPELINE_NODES.md` stage 7's remaining half. `ProfilePass::Elapsed` is the field
they land in; it reads zero and the profile panel shows that as *not measured*
rather than as free.

- **This entry used to say it was blocked on the executor.** It is not, any more:
  D00002 landed and `graph::Execute` submits the frame, so there is now a node to
  put a timestamp around. There is still no way to write one.
- **`SDL_GPU` has no timestamp query API.** Checked by reading
  `SDL_gpu.h` at the vendored 3.2.31 rather than by remembering: there are fences
  — `SDL_SubmitGPUCommandBufferAndAcquireFence`, `SDL_QueryGPUFence` — and those
  are whole-command-buffer granularity, which is one number for the frame. No
  query pool, no timestamp write, nothing per pass.
- So this is blocked on SDL rather than on us, which is a different kind of
  blocked: no amount of work here moves it. Either a release adds the API, or it
  needs a per-backend path behind `Renderer::Backend()` — Vulkan has
  `vkCmdWriteTimestamp`, D3D12 has `EndQuery` — which is real per-backend code in
  a module whose whole point is not being per-backend.
- **Do not fill `Elapsed` with CPU time in the meantime.** A submit-side number
  in a field labelled as the pass's cost is worse than a blank: somebody reads
  "0.4 ms" for the shadow pass, believes the GPU said it, and optimises the wrong
  thing.
- **The half that could be built, was.** `FrameRunner::Run` pushes an
  `SDL_PushGPUDebugGroup` named for each node, so RenderDoc, Nsight and Xcode
  attribute every draw to a node. One group spans `opaque` and `transparent`
  because they share a render pass. That is the readable-capture half of §7; the
  numbers half is what is stuck.
- The upload counters **are** built: `FrameResult::UploadedBytes` and `Uploads`
  count every copy into GPU memory, measured at the region rather than derived
  from a count, and the profile panel shows them.

### [CLOSED] D00045

**Closed at v0.11: `NodeScope` shipped with three values.** The open question the
last bullet leaves — three scopes or four — was answered as it predicted.
`Frame`, `World` and `View` exist; `Surface` does not, because nothing schedules
a per-surface block and a fourth value would have been a word in the type with no
block to run in. `RenderGraph::Execute` runs the `World` band once per distinct
world and `Renderer::FrameRunner` prepares that world's first view before it,
which is what gave `World` something real to attach to.

**`Node::PerView` is gone**, not widened: the field is `Scope` and `Compile`'s
partition turns on a predicate rather than a boolean.

**`PIPELINE_NODES.md` stage 5's remaining half, as it was written:**

- A boolean says per-view or not. What a frame needs is **once per frame, once
  per world, once per view, once per surface** — and the multi-view seam already
  needs three of those. A shadow map is per world and is currently spelled "not
  per view", which is true and is not what it means.
- **`Compile`'s partition is the consumer.** Frame and World fall into the
  shared blocks, View and Surface into the per-view one, which maps exactly onto
  what the boolean does today — so this is a widening rather than a redesign.
- **The size estimate was wrong and is corrected here.** "85 references across
  18 files" counted `Band::PerView` and `CompiledGraph::PerView`, which are
  different symbols entirely. The real edit sites are around twenty-five: one
  field on `Node`, one on `Edit`, one on `NodeKindSpec`, `Compile`'s partition,
  and the designated initialisers in `StandardGraph` and the suites. The
  catalogue's table can keep a private `bool` in its own local row struct and map
  it at registration, so its forty-five rows do not change.
- **What actually blocks it is a design question, not the size.** Of the four
  scopes, `Frame` and `View` are what the boolean already says, and `World` has
  something real to attach to — `RenderGraph::Execute` runs the shared block once
  per distinct world. **`Surface` has nothing.** Nothing runs a per-surface
  block; the surface pass loops inside a per-view one. So a four-value enum would
  ship one value that is a word with no behaviour, which is the shape rule 6 is
  about.
- So the open question is whether this lands as **three** scopes now — `Frame`,
  `World`, `View` — with `Surface` waiting for a block to run in, or as four with
  one of them inert. Three is almost certainly right and it is not a call to make
  in the last few minutes of a session.

### [CLOSED] D00044

**`PropertyDescriptor::Writes` has no consumer. It is a declared constraint the
build does not check and nothing reads.**

**Filed on a false premise and closed with the real one.** `Writes` *is*
consumed: `mono.tools/bindings` emits it as the `writes` array of every property
row in `manifest.json`, a checked-in file both declaration files are generated
beside. The search that filed this covered `mono.engine`, `mono.client`,
`mono.server` and `mono.studio` and did not cover `mono.tools`.

So the field is not dead — it is *published*, which makes a wrong one worse
than the entry claimed rather than harmless. What was actually wrong:

- **Four read-only properties declared a write set.**
  `Attachment.WorldCFrame` and `WorldPosition`, `Humanoid.Grounded` and
  `MeshPart.TrianglesCount` each wrote `Writes = property.Reads` beside
  `Writable = false`. The manifest said so too, in the file, for two versions —
  telling every script author that setting them moves storage they cannot even
  be given a value for. `gui::ResolvedField` had the right shape the whole
  time; `scene` was the outlier.
- **Fixed** by declaring the empty set, and the manifest regenerated: four rows,
  no other drift.
- **And made unrepeatable.** `bindings` now walks the whole class table before
  it writes or compares, and refuses two contradictions: a read-only property
  that declares writes, and a writable one that declares none. It runs under
  `just check`, so a descriptor that drifts fails the build. That check is in
  the bindings tool rather than a suite because it is the only binary that
  registers `scene`, `script`, `effects`, `gui` and the services together.
- **An empty `reads` set is deliberately not a fault.** `Players.LocalPlayer`
  has one and is right to: it projects a world resource, not a component on the
  row. The consequence — such a property can never fire `.Changed` — is a
  limitation its own comment states, not a contradiction.

### [CLOSED] D00043

**A derived property whose getter walks to another entity cannot signal
`.Changed` when that entity moves.**

Closed by making the resolve pass report its own write, which is a smaller fix
than the entry expected and does not need a cross-entity dependency at all.

- **The real cause was one layer down and worse than filed.**
  `ResolveAttachments` wrote `WorldFrame` through the reference `Store::Each`
  hands out — a direct memory write, which the store does not report. So
  `Attachment.WorldCFrame` and `WorldPosition` could not fire `.Changed` for
  *any* reason, including the attachment's own `CFrame` being set. The entry
  read it as a limitation of per-entity delivery; delivery was never reached.
- **The fix**: the pass gathers what actually moved, then writes those rows
  through `Store::GetMutable` — the call that reports a write. `ChangeQueue`
  already subscribes to `Attachment` through `Reads`, so the signal now arrives
  on the attachment's own row, which is where the per-entity filter wants it.
- **Only the rows that moved**, which is the half that makes it safe to run
  every frame in two phases: reporting unconditionally would advance the world's
  change counter for ever and falsify `physics`'s static broadphase gate and
  `gui`'s compile gate. `engine.scene.attachments` asserts both directions —
  the parent moving signals, and a second pass over a still world signals
  nothing and leaves `ChangeVersion` where it was.

### [CLOSED] D00042

**`Scheduler` says registration order within a phase means nothing. Four systems
in `PreRender` depend on it.**

Closed by declaring the contract the scheduler already keeps, rather than
building a dependency sort for something insertion order expresses exactly.

- `Scheduler::RunPhases` walks its vector in insertion order, and every host in
  the repository relies on it: a pass that derives something is registered ahead
  of the pass that reads it, and `client::InstallPresentation` says so in its
  own comments. A rule the code breaks everywhere is not a rule, it is a trap —
  it tells a reader the ordering they can see is accidental.
- **The alternative was a declared dependency between systems**, which would be
  a second mechanism saying what registration already says, and would have to be
  written out at every one of those call sites to mean the same thing.
- The header now states both halves, and `ecs.scheduler` holds them: systems in
  one phase run in the order they were added, over two ticks so a scheduler that
  rebuilt its list could not pass; and a phase boundary still outranks
  registration, so the new sentence cannot be read as "registration order is
  *the* contract".

### [CLOSED] D00041

**The node canvas is a `gui` tree and the studio's panels are Dear ImGui. They
do not compose, and the mounting work has to answer this first.**

Closed by taking the second of the two options this entry laid out: the canvas
is drawn with an ImGui draw list, and `nodeview`'s `gui`-tree half is deleted.

- **The decision was forced by what "a full editor" needs, not by taste.** A
  read-only diagram could have gone either way. Adding a node, dragging a wire
  and refusing an incompatible drop all need per-frame input against per-frame
  geometry, and routing that through a retained tree rendered into an offscreen
  target — one per open editor — buys a widget set the panel does not otherwise
  use and costs a texture, a pass and a coordinate hop on every click.
- **What replaced it is testable in the same places.** `nodeview::Editor` holds
  the hit-test, the drop rule, the zoom-about-a-point arithmetic and the menu's
  search; `engine.nodeview.editor` asserts all of it. Only the drawing is in
  `mono.studio/src/Pipelines.cpp`, which is the part no test could have reached
  under either option.
- **Deleted rather than left dead**: `nodeview::Canvas`, `nodeview::Build`,
  `BuildAssets`, `CanvasStyle`, `Pick`, `PickAt` and `Click`, with their tests
  and the internal `Widgets.hpp`. Keeping a second, unused way to draw a node
  canvas is exactly the two-ways-to-do-one-job that `AGENTS.md` names as the
  most expensive debt in a monorepo, and it would have been the copy nobody
  noticed had rotted.
- **`CanvasState`'s pan survives** because the Assets Pipeline canvas is still a
  read-only diagram and still scrolls. It moves onto the same seam when that
  panel becomes an editor.

### [CLOSED] D00040

**`Node::PerView = false` is shared *per world*, and the graph has no idea what
a world is.**

**Closed at v0.11.** `Execute` takes one world identifier per view and runs the
shared block once per *distinct* world, with that world's views immediately
after it. `render::View::World` carries the number — an opaque `uint64_t` and
deliberately not a `world::WorldId`, since `render` is L12 and a world's
identifier is L4's; what the partition needs is only whether two views are of
the same world. The view-count overload is kept and means "every view in one
world", which is what a game and a single-panel editor both are.

Two views of one world run one shadow pass; two views of two worlds run two.
Views of one world need not be adjacent, because a studio's panels are in panel
order rather than world order. The grouping — a world's shared work, then that
world's views — is the load-bearing half: every world's shared block first would
have the second world's shadow pass overwrite the first's before the first's
views had sampled it.

Found while wiring `Renderer::Render` onto `graph::RenderGraph` — the §4.3
executor — and it is the thing that has to be settled before that work is worth
starting.

- **The shadow node is shared because every view of one world samples one map.**
  That is the claim v0.11 is built on and it is true, with the qualifier the
  graph does not carry: *of one world*. `Compile` partitions into shared and
  per-view and nothing in it names a world, so "shared" currently means "once
  per frame".
- **A frame may hold views of different worlds.** That is the roadmap line this
  version exists for, and the studio's second panel already *defaults* to a
  different world. Two such views need two shadow maps, fitted to two sets of
  bounds. Running one shared shadow pass for the frame would light one world's
  geometry with the other's sun fit.
- **It is not a bug today because nothing passes more than one view** (`D00038`),
  and a single-view frame has exactly one world in it. It becomes one on the
  first frame that does.
- **The renderer's shape says the same thing from the other side.** Everything
  the shadow pass needs — the union bound, the light matrix, the scene order —
  is derived inside the per-view setup from *that view's* draw list. Hoisting
  the pass without hoisting its inputs is not possible, and its inputs are only
  hoistable across views that share a draw list.
- **The likely answer is that a `View` names its world** — an opaque id the
  caller sets, not a pointer and not a `world::WorldId`, since `render` is L12
  and may not reach for one — and `Execute` runs the shared block once per
  distinct id rather than once per frame. That makes the partition mean what its
  comment already claims. The alternative is one compiled graph per world, which
  is cheaper to reason about and pays a compile per world rather than a
  partition per frame.
- **Promoted to a roadmap line at v0.11**, "the render pipeline is per world,
  not per process", because it is a change to what a pipeline *is* rather than a
  defect in one — `StandardGraph()` is a free function returning one frame for
  the whole process, and a universe holds several worlds.
- Until then `render/benchmarks/Frame.cpp` is the honest measure of what the
  partition is worth, and it says the shareable thing is the shadow *pass*
  rather than the shadow *fit*.

### [_] D00039

**No world this engine ships runs a physics tick. The module is complete,
tested, benchmarked, and connected to nothing.**

Found while starting v0.11 §4.6, which was scoped as "pre-emptive — nothing has
been reported slow, so begin with benchmarks and touch nothing a number has not
pointed at". The benchmarks were the right first move and they pointed
somewhere else.

- **`RegisterPhysicsSystems` is called from `physics/tests/` and nowhere else.**
  Checked across the whole repository, not inferred: no client, server, studio,
  `world` or `game` translation unit calls it.
- **`PreparePhysicsWorld` is the same story.** Every call outside the module's
  own tests and benchmarks is one of those tests. Without it a store has no
  `PhysicsWorld` resource, and `WorldResource.cpp` is explicit about what that
  means: *"a world with no `PhysicsWorld` produces no pairs, no contacts and no
  query answers at all"*.
- **Only `Engine::script` lists `Engine::physics`** in a `CMakeLists.txt`. The
  client and server reach it transitively.
- **Four production call sites use `physics::Raycast`** — the client's humanoid
  ground check, one server path, and the Luau and JS `Raycast` bindings. None
  of them is reached in the default scenes: a headless `studio --run play` and a
  60-tick server run each log **zero** occurrences of the every-call error that
  an unprepared world produces. So the queries are not silently returning
  nothing today; they are simply not being asked.
- **What this costs is that the whole simulation half is unexercised outside its
  own suites.** Integrate, broad phase, narrow phase and solver have tests and
  benchmarks and no consumer, so nothing about them is checked end to end and
  the numbers below describe a module rather than a frame.
- **What it means for optimisation work: do not.** The solver is by far the
  largest figure in the suite — 13.7 ms for 800 stacks of four, which would be
  82% of a 60 Hz frame — and the broad phase is 2.6 ms at sixteen thousand
  colliders. Both are honest measurements of code no world runs, so a
  percentage taken off either buys nothing until this entry is closed.
- **The order is wiring first, then measuring the real thing.** Whichever world
  gains a physics tick will have its own collider count, density and
  static/dynamic split, and every constant here — cell size included — should be
  re-measured against it rather than tuned now against a synthetic slab.

### [_] D00038

**`Renderer::Render` takes many views and nothing passes more than one.**

- v0.11 replaced the twelve-parameter `Render` with `std::span<const View>`, so a
  frame of four viewports is one command buffer, one swapchain acquisition and
  one present. `D00002` is closed by that. **The seam exists and is unexercised**
  — all three callers pass a span of one.
- **The studio is the caller that wants it and cannot have it yet.** It
  round-robins one panel per frame, so with two open each updates at half the
  rate. Converting the loop is not the hard part; the hard part is above it.
- **`Universe->Present` runs `PreRender`, and `aim-surface-cameras` lives
  there.** A panel's surface cameras are aimed from *that panel's* eye, into
  world state, immediately before its draw list is collected. Drawing two panels
  in one frame means aiming twice before rendering once, and the second aim
  overwrites the first — which `Editor.cpp` already records as the bug that made
  a mirror in one panel track the camera being flown in the other.
- **Two panels on two worlds is fine and is the interesting case.** `Present` is
  per world, the aim is per world, and the second panel already defaults to a
  *different* world — which is the roadmap's "handle multiple worlds in
  parallel" exactly. **Two panels on one world is the one that breaks**: it
  would present the same world twice in a frame and run its `PreRender` systems
  twice against one `frameSeconds`.
- So the conversion needs the same-world case answered first — present once per
  distinct world per frame, then aim and collect per panel — rather than a loop
  around what is there now.
- Until then the round-robin stays, and `render/benchmarks/Frame.cpp` says what
  it is buying: about 150 us of CPU record per viewport, 18% of a 300 fps frame
  at four panels.

### [CLOSED] D00037

**`BENCH`'s iteration count means two different things in two files, and a row
does not say which.**

**Closed at v0.11.** The report carries the unit. `BenchCase` grew a
`BenchUnit` — `Call` when the body loops `Iterations` times, `Item` when it runs
once over `Iterations` things — and the line is now
`bench<TAB>suite<TAB>ns<TAB>spread<TAB>samples<TAB>iterations<TAB>unit<TAB>name`.
`graph`'s rows read `call` and `scene`'s read `item`, so two figures four orders
of magnitude apart are no longer silently comparable.

- **A second macro rather than a parameter.** `BENCH_PER_ITEM` declares the
  normalising form; `BENCH` keeps its signature and its meaning, so no existing
  benchmark changed except `scene/benchmarks/Ordering.cpp`, which was already
  using the divisor that way and now says so.
- **The unit goes before the name, not after it.** The name is free text
  flattened onto one line, so anything past it cannot be found by counting tabs.
- **The baseline migration this entry warned about cost nothing**, because
  `.cache/bench-baseline.tsv` is git-ignored — there was no committed baseline
  to migrate. Checked rather than assumed; the runner re-measured all 401 rows
  against the new format.

- `BenchMain::Sample` calls the body **once** and divides the elapsed time by the
  declared `Iterations`. So the count is a divisor the author promises the body
  honours, and `Bench.hpp` states that contract: *"Runs the body `Iterations`
  times."*
- **`graph/benchmarks/Cull.cpp` keeps the promise** — it writes
  `for (pass = 0; pass < 200000; pass++)` inside the body — and its rows are
  nanoseconds per call.
- **`scene/benchmarks/Ordering.cpp` does not, deliberately.** Its bodies run once
  and pass the *instance count* as the divisor, so its rows are nanoseconds per
  **instance**: *"One iteration is one instance, so every row divides into a
  per-instance cost."* That is a reasonable thing to want and the header says so.
- **Nothing in the report distinguishes them.** Both emit the same six tab-
  separated columns, so `OrderScene · 10k instances = 1` and `Cull 1000, all
  visible = 17207` sit in one output looking comparable and are off by four
  orders of magnitude from each other's unit.
- Found while writing `render/benchmarks/Frame.cpp`, which made the third
  mistake available: a body that runs once while declaring 200, reporting a
  frame at 739 ns. It was caught only because `graph`'s table gave a number to
  contradict it — 5000 instances cannot record in less time than one of them
  culls.
- **Not fixed here because the fix is a decision, not a patch.** Either
  `BenchCase` grows a unit field the report carries, or the per-instance
  normalisation is spelled differently — a `PER_ITEM` variant of the macro — so
  the divisor always means the same thing. `bench-accept` compares rows against a
  stored baseline, so whichever is chosen has to migrate the baseline in the same
  commit.

### [CLOSED] D00036

**307 public entities carried no comment, and nothing had ever been able to say so.**

- `just docs-check` runs two Doxygen passes. The first is the site, and it fails
  the recipe on malformed comments and dangling links; the second is the
  **coverage** pass, `EXTRACT_ALL = NO`, whose whole job is to report a public
  entity nobody documented. **The second had never completed**, because the first
  had been red for at least two versions and the recipe stops at it.
- **Found by fixing the thing in front of it**, which is `D00035`. The moment
  `warnings.txt` reached zero, `gaps.txt` reported **307** — and this is the
  third time this repository has recorded that cascade: `ROADMAP.md` v0.2 for
  `docs-check` itself, `D00005` for `just preset=ci check`, and now one level in.
- **All 307 were genuinely public**, checked rather than assumed: the coverage
  pass leaves `EXTRACT_PRIVATE` off, so none was a private detail that slipped
  in.
- **About a third were unattached rather than unwritten**, and one setting closed
  them. `ecs::AttributeValue` has fifteen payload fields that are one idea —
  a per-field comment could only ever have said "the `float` case" — and
  `engine::gui` declares nineteen `Describe` overloads under one paragraph
  explicitly about all nineteen. Doxygen attaches a comment to the declaration
  beneath it, so the other fourteen and eighteen counted as gaps.
  `DISTRIBUTE_GROUP_DOC = YES` plus `//@{` markers documents each family once.
  **Not a lowered bar**: the author still writes the comment and still marks the
  group by hand. What it stops is documentation written to satisfy a check.
- **The rest was writing, and the useful ones were where a name hides a trap** —
  `Delta::Baseline` and why a lost datagram is survivable, `Structure`'s three
  lists and why `Forgotten` is never merged with `Destroyed`,
  `Statistics::Refused` against `Deferred` (the link saying no against this
  module saying later, which this file already records people confusing twice),
  and `Answer::PublicKey` being repeated rather than remembered, which is what
  makes the challenge stateless.
- **Two wrong turns, both recorded rather than tidied away.** A filter rule was
  added so `//@{` was *not* promoted to `///@{`, on the reasoning that a
  delimiter is not prose — wrong for this pipeline, since the promoted form is
  the one Doxygen groups on. Worse, while that rule was in place it produced the
  measurement behind a confident claim in this entry that namespace-level
  overloads **cannot** be grouped. They can. **A tool change made mid-
  investigation invalidated the measurement being taken through it**, because the
  filter was both the instrument and the subject. The rule was reverted and left
  out, having no user.
- **It also turned up three more orphaned doc blocks** of the kind `D00035`
  found two of — a new member's comment inserted *inside* an existing one, so
  `RequestShownContent` and `PublishManifestNames` were undocumented while their
  prose sat on `FitPartsToMesh`. All three were added by the v0.10 mesh work.
- **`just docs-check` exits 0 and says "every public entity is documented".**
  It is now a check that can fail for a real reason, which it has not been able
  to do for two versions.

### [CLOSED] D00035

**`just docs-check` was red, and the thing it was red about was not the thing worth finding.**

- Found by running it rather than by reading it: the recipe fails when
  `warnings.txt` is non-empty, and it held **19 lines**. One was a genuinely
  broken link — `README.md` pointing at `docs/CPP_LINKER.md` after that file
  moved to `docs/retired/` — and the other eighteen were Doxygen's Markdown
  disagreeing with prose it was handed.
- **The link half was larger than the one warning showed.** The same move left
  **26 stale `docs/<name>.md` references across 20 files**, almost all in source
  comments where nothing checks them — `docs-check` only resolves links
  reachable from the documented surface. Two more dangling links were found by
  sweeping every local Markdown link directly, including one *inside*
  `docs/retired/v07v08.md` that broke by being moved beside the siblings it
  names.
- **The count moved the wrong way first, and that was the tell.** 19 to start;
  fixing the mainpage link took it to **26**, because the site pass had been
  stopping at that link and the seven it then reported had been invisible behind
  it — including two live source defects, where a new member's doc block had
  been inserted *inside* an existing one, leaving `bake::Graph::AddWrite` and
  `render::Renderer::TextureHandle` undocumented while their prose sat on the
  wrong function.
- **The eighteen had one cause, and it was not the one this entry first named.**
  `JAVADOC_AUTOBRIEF` ends the brief at the first sentence-ending stop and does
  not care that the stop is inside emphasis. This repository's house style is a
  bold *sentence* — `**Twenty-eight and not thirty-two.**` — so the split lands
  between the `**` and its partner: the brief ends holding an unclosed emphasis
  and the detail starts with a stranded closer. Doxygen reports it **against the
  following comment block**, which is why the warning never points at the
  comment that caused it.
- **The wrong answer was held for an afternoon and is recorded rather than
  quietly dropped.** The first minimal reproduction kept the stop inside the
  bold while dashes, quotes and line wrapping were varied around it — so every
  variant failed and *wrapping* took the blame. That conclusion was written into
  this entry as confirmed, with a measurement beside it (355 multi-line bolds
  across 145 headers) that made it look substantiated. **Emphasis spanning a
  line break is completely fine.** Moving the stop out fixes a bold spanning
  three lines; leaving it in breaks one that fits on half of one. The lesson is
  the old one: varying everything except the cause proves the cause is
  everything else.
- **Fixed in the filter, in one character.** `docgen::Promote` moves a trailing
  stop from inside the emphasis to outside it — `**Sentence.**` becomes
  `**Sentence**.` — which preserves the line count that every source link on the
  generated page depends on, keeps `JAVADOC_AUTOBRIEF`, and leaves the house
  style alone. An unpaired `**` leaves its block untouched, and markers inside a
  code span are code. Seven cases in `docgen/tests/Filter.cpp`.
- **The last one was in Markdown rather than in a comment, and pages are not
  filtered.** A code span containing apostrophes —
  `` `Unknown type 'Enum.Material'` `` — left an unmatched `</tt>`, reported
  against a line four bullets further down. It resisted isolation because the
  line, the pair and the section all rendered clean on their own; what found it
  was neutralising each of that line's sixteen code spans in turn.
- **`DOT_GRAPH_MAX_NODES` is 64 now**, up from Doxygen's default of 50, which
  `studio::Editor` passes with 55 collaborators — and passing it draws no graph
  at all and warns, so the default gave the one class whose relationships are
  hardest to hold in a head the one page with no picture.
- **Closed with `warnings.txt` at zero, and what that revealed is `D00036`.**
  The coverage pass behind it had never run to completion and reports 307
  documentation gaps. That is a separate entry because it is separate work.

### [CLOSED] D00034

**One asset baked and did not publish, and nothing in the pipeline would say which.**

- v0.10's store re-baked to **1974 raw, 1973 baked**. The gap was real, was never
  identified, and was only visible by *subtracting two numbers printed by two
  different tools* — which is why it survived a whole version.
- **Found, and it is not where this entry looked.** The entry assumed a baker
  refusing something and throwing the reason away. It was neither the baker nor
  the publisher: `cdn::ImportFile` accepted a **zero-byte file**. Found by
  diffing the two folders by hash, which left exactly one — `af1349b9…`, BLAKE3's
  empty-input digest — and then reading the content log, which named
  `blender-dragon/.venv/.lock`: a Python virtualenv lock file swept along by the
  folder import of a model.
- **The diagnosis this entry made was right and the location was wrong, which is
  worth keeping.** "The reporting is the actual defect, not the missing asset"
  held exactly — but the missing report was three stages upstream of where it
  was looked for. `cdn::Publish` *already* names what it skips, including an
  empty file; the trouble is that by the time it says so the file has been in
  `raw/` for good, and `raw/` is the folder the counts are taken from.
- **Closed by refusing it where refusing is free.** `ImportFile` now rejects an
  empty file and names it: it can never bake and never publish, so accepting one
  is guaranteed to produce a store whose totals disagree. Nothing is written and
  nothing is logged, because a refusal that still left the file behind would
  move the silence rather than remove it.
- **Not deleted from this repository's own store**, which still holds the
  zero-byte file: that is somebody's content directory and not this change's to
  edit. The counts there stay 1974/1973 until it is removed by hand.
- Pinned by a case that imports an empty file, requires the refusal, and then
  requires `raw/` and the log to be empty. Demonstrated by mutation.

### [CLOSED] D00033

**A mesh had no cached thumbnail, so a picker row showed a glyph until it was hovered.**

- `PaintPreview` rendered the *hovered* row into the studio's one preview slot —
  built-ins included — and every other row showed a letter. The rendering worked;
  what was missing was retention.
- **The blocker was in `render` rather than in `mono.studio`, and that reading
  held.** A cached thumbnail means keeping a scene target past the frame, and the
  renderer exposed no copy: the studio could ask for a slot to be drawn and could
  not ask for the result to be kept.
- **Closed at v0.10 by `Renderer::CaptureSceneTexture`**, a device-to-device blit
  from a slot into a new texture, published into `render::TextureTable` through a
  new `Adopt`. Nothing goes through the host: reading a picture back to the CPU
  only to upload it again would be a round trip across the bus for bytes that
  never needed to leave the device.
- **`Adopt` transfers ownership, and that is the whole contract.** The table
  releases the texture on `Drop`, on replacement and on `Shutdown` exactly as it
  does for one it uploaded — so every refusal in `CaptureSceneTexture` has to
  leave the caller still owning it, and a full table returns `false` *before*
  releasing whatever was under the name rather than after.
- **The drawn rectangle is copied, not the allocation.** A scene target is
  rounded up to 64-pixel blocks with hysteresis, so most of it is border the pass
  never wrote — copying it whole would keep a picture with an unwritten margin
  down two edges and force every consumer to carry `SceneTextureExtent` beside
  the handle, which is the coupling this ends.
- **It lands in the existing thumbnail cache rather than beside it**, which is
  what makes eviction work without knowing captures exist: `PumpThumbnails` drops
  the least recently drawn by calling `DropTexture` on exactly the name a capture
  publishes under. A second cache would have been a second thing to evict, and
  the one nobody wrote a policy for. Captured once per asset, not once per frame
  — the preview turns, and the frozen angle a thumbnail wants is any of them.
- **What is tested is every refusal and none of the success.** A capture needs a
  device; standing a fake in front of that would test the fake. Three cases pin
  the paths that decide whether a caller has just been handed a texture it now
  owns — no device, an invalid name, a slot never drawn into. The drawn path is
  verified by running the editor.
- The prediction in this entry's old reopen trigger was half right: the material
  preview did arrive and did want the same mechanism, but it shipped *before*
  this using the live slot, so the trigger never fired on its own.

### [CLOSED] D00032

**Deleting a `Material` instance left the texture it last resolved on the part.**

- `scene::ResolveMaterials` walks `MaterialRef` rows and writes the resolved texture into the parent's `SurfaceAppearance::ColourMap`. A part that no longer has a `Material` child has no row, so nothing visits it and the last resolved name stays — the part goes on drawing a texture nothing in the tree names any more.
- **The three states that do work are the ones that matter day to day**, which is why this shipped rather than blocking: no material at all leaves an authored `BasePart.ColorMap` alone; a material naming an asset resolves to it; and a material set back to `None` *clears* the part, because the pass writes even when it resolves to nothing. Only the deletion is stale, and only until something writes the field again.
- **The obvious fix is the wrong trade by two orders of magnitude.** Visiting every part every tick to ask whether it still has a material child is a child scan per drawable per tick, on the loop `client::CollectInstances` exists to keep flat, to correct an editor-time action.
- ~~**What would close it is a destruction hook**, which is the shape `ecs::Store::DestroyEntity` already carries for attributes.~~ **Closed at v0.10, and not that way — the hook would have been both harder and weaker.**
- **Harder, because the shape this entry pointed at is not a hook.** `DropAttributes` is a *hard-coded call* inside `DestroyEntity`, not a registration anything can add to, and `scene` sits above `ecs` — so closing this as described meant first inventing a general destruction hook in the storage layer, for one caller.
- **Weaker, because destruction is only one of the ways this goes wrong.** A `Material` **reparented** to a different part destroys nothing, and leaves the part it left holding the old texture for ever; so does a `MaterialRef` removed from a living instance, and a material moved under something with no `SurfaceAppearance`. A hook on the row leaving catches none of those three. This entry named the symptom it had seen and then wrote the fix for exactly that symptom, which is the failure worth keeping: **the trigger was "deleted", the defect was "stopped being written".**
- **What shipped is a difference between two passes.** `ResolveMaterials` records the parents it wrote in `MaterialCatalogue::Resolved`, and the next pass clears any parent in that record it did not write again. It is O(materials), not O(parts) — so the trade this entry rejected is not paid: nothing walks the drawables, and the pass already had every entity it needs in hand.
- **Three things had to be right and each is pinned.** The set holds **handles rather than indices**, because a destroyed entity's index is reused immediately and an index alone would clear an unrelated part built in its place — `Store::Get` checks the generation and answers null. `ReadMaterialCatalogues` clears the set on load, or a handle kept across a directory replacement names whatever now sits at that index. And a world that has never had a material still acquires **no resource at all**, which is what `ColourMapOf` being the non-creating reader exists to protect.
- **Demonstrated by mutation.** With the clear removed, the deletion case and the reparent case both fail; the third case — no catalogue on an untouched world — passes either way, because it pins a guarantee rather than catching this bug.

### [_] D00031

**The editor does not know `Enum.Material`, because luau-lsp reads the definitions file and nothing registers the prefix for it.**

- `just typecheck` accepts `local m: Enum.Material` — `scriptcheck` registers `importedTypeBindings["Enum"]` itself. luau-lsp loads the same definitions file and does not, so an author writing the dotted form sees a red squiggle on a line that builds and passes.
- **The flat spelling still resolves everywhere**, so this is a cosmetic gap with a workaround rather than a broken surface: `Enum_Material` is what the declaration file declares and what the editor understands.
- **Three ways to close it, and none is obviously right yet.** Teach luau-lsp the prefix, which means a patch to a vendored tool and `mono.vendor/AGENTS.md` says a patch goes upstream or into a fork. Switch `luau-lsp.platform.type` to `roblox`, which makes the editor typecheck against Roblox's class tree rather than this engine's — worse than the squiggle. Or generate an `Enum.luau` module and have scripts `require` it, which works in both and costs a line at the top of every file.
- **Reopen trigger: somebody writing enum annotations often enough to be annoyed.** The engine's own scripts have three.

### [_] D00030

**A mutable property on a script *global* reads once and never again, because `luaL_sandbox` enables Luau's `safeenv`.**

- `UserInputService.MouseBehavior` is the first property in the engine that lives on a global rather than on an instance, and it does not work when it is read that way. `local UIS = game:GetService("UserInputService")` works; `UserInputService.MouseBehavior` returns whatever it was the first time any closure asked.
- **The mechanism, because it is not obvious and cost an hour.** `luaL_sandbox` freezes the global table and turns on `safeenv`, which lets the compiler emit `GETIMPORT` for a constant global followed by constant fields. `GETIMPORT` resolves the chain once per closure and caches the **value**. It does this whether the intermediate is a table or a userdata, so making the service a userdata does not fix it — that was tried, and the observation that settled it is that `__index` fires for the first read of a field and never for the second, with no raw key on the object to explain it.
- **The userdata is still right and is kept.** It is what makes every read *through a local* go to `__index`; a plain table would have been cached there too.
- **In practice it does not bite, which is why this is filed rather than fixed.** Every Roblox script begins `local UIS = game:GetService("UserInputService")`, and `game:GetService` is a method call that cannot be an import. The engine's own declaration files describe the property, the test uses the idiomatic form, and the comment in `InputServices.cpp` says so.
- **What closing it would take.** Either not sandboxing — which is not on the table, `LuauRuntime` freezes the globals so one script cannot change the language the next one runs in — or making the service a *function call* rather than a global, which changes the surface away from Roblox's. Neither is worth it for a property nobody reaches the broken way.
- **Reopen trigger: a second mutable global property.** One is an oddity with a workaround everybody already uses; two is a pattern, and at that point the surface should stop being globals.

### [CLOSED] D00029

**The light count was spelled in C++ and in GLSL and nothing checked that the two agreed.**

- `render::MAX_SCENE_LIGHTS` is 16 and `MAX_LIGHTS` in `shaders/opaque.frag` is 16, and the only thing keeping them equal is that somebody wrote both. `AGENTS.md` rule 6 is explicit that a constraint the build does not check is documentation, and this one is.
- **A test that reads the shader back does not work, and it was tried.** What `Paths::Shaders` stages is SPIR-V — the constant is folded away by then — so a suite comparing the staged file against the C++ value has nothing to compare. Reading the repository's `shaders/` directory from a test binary would work and would make the test depend on the source tree being present beside it, which no other suite here does.
- **What a mismatch costs, which is why this is filed rather than shrugged at.** It is not a validation error and not a crash: `LightUniforms` is sized by the C++ constant and the shader indexes by its own, so a shader with a smaller cap silently ignores the tail of the set and one with a larger cap reads past the buffer. Both look like "that lamp does not work".
- ~~**The fix is one line and it is in the build rather than in either file.**~~ **Closed at v0.10, and the entry's own prescription was right except for where the number lives.** `mono_add_library` takes `SHADER_DEFINES`, `_mono_add_shaders` turns each into a `-D` on the glslc command, and `opaque.frag` guards its value with `#ifndef MAX_LIGHTS` so it still compiles by hand.
- **The number stayed in C++ rather than moving into the build, which is the one place this entry's plan was changed.** Putting it in CMake and defining it into both languages was the obvious reading of "one home in the build", and it fails on a detail: `Renderer.hpp` is included by things that do not link `render` — `mono.tools/linecount/tests/Report.cpp` is one — so a compile definition would either break them or need a fallback default, which is the second spelling again. Instead the configure **reads** `MAX_SCENE_LIGHTS` out of the header with a regex and feeds glslc. C++ stays where a reader expects the constant, and the shader keeps no literal of its own.
- **Demonstrated by mutation rather than asserted.** The header set to 8, reconfigured: the command line becomes `-DMAX_LIGHTS=8` and the staged SPIR-V changes hash. Reverted, and it changes back. **There is no test, deliberately** — the disagreement is now unrepresentable rather than detectable, and a suite could not have read the number anyway, which is what the first attempt at this discovered.
- **Two sharp edges, both handled where they bite.** The regex is matched at configure time, so `CMAKE_CONFIGURE_DEPENDS` names the header — without it, editing the constant recompiles the C++ and leaves the shaders on the value read at the last configure, which is this bug arriving by a different door. And a failed match is a `FATAL_ERROR` naming the file, not a silent skip, because a quiet fallback would restore exactly the drift being deleted.
- **Reopen trigger: the next time a shader needs a constant C++ also holds.** The mechanism now exists, so that is a one-line `SHADER_DEFINES` entry rather than a build change.

### [CLOSED] D00028

**`Enum.Material` is a type in Luau after all. This entry was wrong and is corrected rather than deleted.**

- **What it claimed:** that Luau *cannot* express a dotted type name for a global, that a definitions file's inability to declare one was the language's inability, and that the only way out was a generated `Enum.luau` module and a require-path resolver.
- **The first half was right and the conclusion did not follow.** A definitions file genuinely cannot declare one: `loadDefinitionFile` writes `exportedTypeBindings[name]` and nothing else, and there is no `declare` syntax for a dotted name. The probe that produced "Unknown type 'Enum.Material'" was real.
- **What was missed is where the resolution happens.** Luau parses `Enum.Material` in a type position as a reference with a *prefix*, and resolves it through `Scope::lookupImportedType("Enum", "Material")` — the `importedTypeBindings` map. `require` populates that map (`ConstraintGenerator.cpp:1512`), and so may a **host**. Roblox is not using definition-file syntax; it is registering that map. So is luau-lsp's Roblox platform.
- **Closed by doing the same thing.** `mono.tools/scriptcheck` walks the extern types the generator emitted under the `Enum_` prefix and aliases each under the `Enum` prefix, before `freeze`. 35 enums, and `local m: Enum.Material` typechecks. The examples that carried `Enum_Material` in an annotation now carry `Enum.Material`.
- **The declaration file still uses the flat names, and that is ordering rather than compromise.** The aliases are built *from* the types the file created, so they cannot exist while it is being loaded — emitting the dotted form there made the file fail to load before a single script was checked. Both spellings name the same `TypeFun`.
- **What is still open is the editor, and it is filed as D00031** rather than left inside a closed entry.
- **The lesson worth keeping: "the file cannot say it" is not "the language cannot do it".** The first probe answered the question that was asked and the wrong question was asked.

### [CLOSED] D00027

**The mirror flashed once per orbit, and it was a sign flip rather than a projection fault.**

- `scene::AimSurfaceCameras` sets `facing = distance >= 0 ? 1 : -1` and points the reflected camera along `unit * facing`. That is correct on either side of a pane — a face can be looked at from behind and the reflection belongs on that side — and it is discontinuous *at* the plane.
- **Measured, not inferred.** `scene/tests/SurfaceCameras.cpp` orbits the eye a full lap at 360 samples and records the worst single-step change. The field of view moves by 0.027 radians at worst; the camera's look vector moves by **exactly 2.0** at 1.588 radians, which is a 180° turn in one frame at precisely the plane crossing. Orbiting a pane centred on the origin crosses twice a lap.
- **Skipping the crossing was tried and does not work.** Returning early leaves the camera at its previous transform, so the flip lands a frame later instead of not happening; the discontinuity belongs to the sign, not to when it is evaluated. There is no continuous path between facing -Z and facing +Z.
- ~~**What would fix it.**~~ **Closed at v0.10, exactly as this entry predicted and for the reason it gave.** A pane seen edge-on subtends zero pixels, so its surface renders *nothing*: inside `EDGE_ON_MARGIN` of the plane the camera is left where it was and the pane is taken off its slot. The two orientations either side of the crossing are therefore never in consecutive **visible** frames, which is what removes the flash — the sign still flips, and nothing is shown while it does.
- **The measurement inverted.** Worst single-step turn over a 360-sample lap went from **exactly 2.0** — a 180 degree whip — to **exactly 0**, and the zero is not a tuned bound: within one side of the plane `facing` is constant, so the reflected camera's orientation does not change at all as the viewer orbits. Only its position does. Six samples of 360 come out blank, twice a lap.
- **The entry's note about the test was right and was worth writing down.** The bound asserted the bug (`<= 2.001`), so closing this failed it and forced the assertion to be rewritten in the same change — and rewriting it surfaced the real question, which is *what to measure*. Continuity is only asked of frames that draw; comparing across the blank band would be asserting continuity of a picture nobody was shown. A second assertion requires the band to actually be entered, because otherwise the test would pass by quietly no longer crossing the plane.
- **Two existing cases were pinning the degenerate arrangement without saying so, and only failing revealed it.** One put the viewer exactly in the glass and required a *clamped* frustum — a finite matrix for a view covering half a turn, which nobody can see. The other, "a rotated pane reflects along the way it actually faces", left the eye at +Z while the rotated normal was -X: the eye was level with the plane, the mirrored position was the eye itself, and its assertion passed for a reflection that was never computed. **A test that passes because nothing happened is the failure mode this closure actually found.**
- **What it does not cover, stated rather than left to be discovered.** The band is a distance and a viewer's motion is a speed, so somebody crossing the plane fast enough to step over the whole band between two frames still sees the flip. No width closes that for every speed.

### [CLOSED] D00026

**Closed in v0.10.** `bake` writes the runtime texture format from supported
source images, while the client and studio resolve image names through their
content tables and upload the resulting pixels. Missing or unresolved images
still use the visible fallback marker. Runtime code does not decode PNG files;
the parser remains in `bake`.

### [CLOSED] D00025

**`gui::Pick` tests an axis-aligned rectangle, so a rotated button clicks where it is not.**

- Split out of `D00023` rather than left inside it, because the two close in different modules: that one is a *backend* emitting rotated geometry, and this one is `gui`'s own hit test, which no backend can fix.
- `DrawCommand::Bounds` is the unrotated rectangle and `Rotation` sits beside it. `Pick` reads the first and ignores the second, so a rotated element draws in one place and answers a pointer in another.
- **A rotated button that draws in one place and clicks in another is a bug people file twice**, which is why it is written down separately from the drawing half.
- **Closed at v0.8, in the same change as `D00023`** — rotating the geometry without rotating the test would have made the mismatch visible rather than merely present.
- **The point is turned into the element's space, not the rectangle into the screen's.** A rotated rectangle is not a rectangle and testing one needs a polygon; rotating the *point* back by the same angle makes the test the axis-aligned one it already was, exactly.
- **The clip is deliberately still not rotated.** A scissor is axis-aligned on every backend there is, so an element rotated inside a clipped container is cut by an upright rectangle — which is what the painter does and what the hit test therefore has to agree with.

### [CLOSED] D00023

**A rotated `gui` element rotates its box and not its contents.**

- `ui::PaintGui` draws a rotated `Rectangle` or `Outline` as a convex quad and draws `Image` and `Text` upright at the rotated rectangle's centre. `Element::Rotation` and `Resolved::AbsoluteRotation` are correct and are carried on every command; what is missing is a backend that uses them for the other two kinds.
- **Why it stops there rather than being finished.** Rotating a glyph run means per-glyph quads, which imgui's `AddText` cannot emit — it walks the atlas and appends axis-aligned quads with no transform. Rotating a nine-slice means rotating each of the nine pieces individually with its own uv rectangle. Both are real work in the *backend*, and the backend that is going to matter is the batched quad pipeline `ROADMAP.md` schedules at L12, rather than this one.
- **The hit test rotates with neither.** `gui::Pick` tests `DrawCommand::Bounds` as an axis-aligned rectangle, so a rotated button is clickable on its unrotated box. Stated because a rotated button that draws in one place and clicks in another is a bug people file twice.
- **Closed at v0.8 by `render::InterfaceMesh`**, which is the pass this entry predicted: it emits its own vertices, so the rotation is applied to all four kinds in one place — `Push` turns each corner and every kind goes through `Push`.
- **The pivot is the element's centre, not each quad's**, which is the one thing about rotating text that is easy to get wrong and unmistakable when it is: a per-quad pivot spins every letter on the spot and leaves the run in a straight line. Pinned by a case that rotates a two-glyph label and asserts the run goes *down* rather than across.
- **`ui::PaintGui` is unchanged and still draws rotated contents upright**, which is now a difference between the two backends rather than a gap in both. It is the editor's, imgui cannot emit a transformed glyph run without a per-glyph path, and the backend that matters for a shipped game is the one that was fixed.
- The hit test moved with it — see `D00025`, closed in the same change.

### [CLOSED] D00022

**A `SurfaceGui` and a `BillboardGui` lay out against a canvas whose 3D half nothing supplies.**

- `gui::CanvasFor` gave a `SurfaceGui` its `CanvasSize` in pixels and ignored `SizingMode::PixelsPerStud`; it gave a `BillboardGui` the offset half of its `Size` and ignored the scale half. Everything under either laid out correctly *against that canvas* — what was missing was the number the canvas should have been.
- **Closed at v0.8 by `render::ResolveSpatialCanvases`**, which is the shape this entry predicted and the same split `AdornmentGeometry` already makes one dimension up: `gui` is L7 `shared` and links neither `scene` nor a camera, `render` links both, and the multiplication went to the module with both operands.
- **A component rather than `Surface::CanvasSize`, which is where the entry's own suggestion was wrong.** Writing the resolved pixels back into the authored field clobbers a saved property — toggle `Sizing` back to `FixedSize` and the canvas an author typed is gone, replaced by whatever the last pixels-per-stud frame computed. `gui::SpatialCanvas` is a derived component beside `Canvas` and `Resolved`: nothing authors it, nothing replicates it, and two hosts with different viewports are *supposed* to disagree about it.
- **Absence is the interface, and it does more work than the value.** A collector nothing can measure — a `SurfaceGui` on a `Folder`, a billboard in a world with no live camera, a headless test — simply has no component, and `CanvasFor` then uses the authored pixels, which is exactly the behaviour that was there before. So the feature is additive: no caller had to change to keep working, and the one that did not call it kept the old answer rather than a zero.
- **Stale ones are removed, which is the half that is cheap to skip and expensive to skip.** A resolved canvas nobody refreshes keeps working — it keeps the size of the last frame that could measure it — so a `SurfaceGui` switched back to `FixedSize`, or one whose adornee was deleted, would look right until somebody moved the part it is no longer attached to. Pinned by a case that resolves, changes the mode, and asserts the component is gone.
- **Both hosts call it, and the studio calls it per panel.** A viewport panel *is* a canvas with its own camera, so two panels looking at one world from two distances give one billboard two sizes — which is correct, and is the reason this is resolved at the draw site rather than once for the world.
- Nine cases in `render/tests/SpatialCanvas.cpp`, including one that runs the whole seam: `render` writes the component, `gui::Layout` reads it, and a frame inside a pixels-per-stud surface lands on the resolved rectangle. Neither module can assert that alone.

### [PARTIAL] D00021

**`AutomaticSize` is declared, saved, bound and read by nothing.** The container half closed at v0.8; the text half is still open and is the harder one.

- `Element::Automatic` was a real property with a real enum that reached the layout pass and got ignored. An author setting `AutomaticSize = "Y"` got the size their `UDim2` resolved to.
- **Why it is declared anyway, which is the part that needs defending.** The roadmap's rule is that a class registered with nothing behind it is worse than no class — "a feature that looks present and is not". A *property* is a weaker case than a class and the trade came out the other way: the field crosses a save file and both bindings, so adding it later is a format change, and adding it now costs a byte. What is not acceptable is leaving it undocumented, which is what this entry is.
- **The container half is done**, and it is the second phase this entry predicted: `gui::ContentExtent` measures a node's children against a basis with the growing axes zeroed, and `Measure` adds the padding back and then constrains. A stack sums along its fill axis and takes the maximum across it, a grid counts its lines, and a container with neither grows to the far edge of the furthest child. Nine cases in `gui/tests/Layout.cpp` carry it.
- **The circularity got Roblox's answer, stated rather than inherited.** A child sized `{1, 0}` inside a parent sized by its content is asking to be as wide as the thing whose width it is deciding, and there is no fixed point. The growing axes resolve to zero for everything inside, so such a child measures as empty and then fills the grown parent when it is placed. Pinned by a case that asserts *both* halves, because either alone is satisfied by a different bug.
- **The extent is accumulated, not collected.** A sum, a maximum and a count are all single-pass, so an automatically sized container allocates nothing per frame — which is what keeps this off the layout benchmark for every tree that does not use it. The cost it does add is real and bounded: measuring is linear in the subtree and placing measures again, so a fully automatic tree of *N* nodes and depth *d* is O(N·d) rather than O(N). Only elements that set the property pay it.
- **Text is a separate and harder half, and the reason sharpened.** The obvious reading — "it needs real glyph metrics, and `render::GlyphAtlas` now has them" — is wrong, and wrong in a way worth writing down. `gui` is L7 and `render` is L12, so the metrics would have to arrive through an injected measurer; and the moment they do, a client with an atlas lays a tree out differently from a headless server, a studio and a test that do not. `Layout.hpp`'s stated invariant is not that the measurement is exact but that **there is one answer**. So this half needs metrics that are *shared* — a font table below L7, or the glyph advances themselves as engine data — and not merely metrics that exist.
- **Meanwhile a labelled element refuses to grow rather than approximating**, which is the branch that keeps the failure loud: growing to the estimate makes a box the text spills out of, and measuring the (absent) children of a `TextLabel` would collapse it to nothing.
- **Reopen trigger for what is left: the first author who sets `AutomaticSize` on a `TextLabel` and files it as a bug.** That is now a documented refusal rather than a silent no-op, so it will arrive as a question rather than as a mystery.

### [CLOSED] D00020

**`gui` text is a `core::Name`, and `core::Name` never releases a string.**

- `Label::Text`, `Picture::Image` and `Entry::PlaceholderText` all interned. That is what made each a `PropertyType::Name` — saved as text, sent as text, readable from both bindings, editable in the properties panel — with no new property type and no new wire form. `ecs::InstanceName` makes the same trade for the same reasons and still does.
- **The cost, stated rather than discovered.** Text that changes every frame interned a new string every frame, forever, plus the process-wide registry's mutex on every write. `label.Text = tostring(score)` at 60 Hz was an unbounded leak and a lock in the frame loop.
- **Closed at v0.8 by `ecs::PropertyType::String`**, which is exactly the shape this entry predicted and very nearly the size: the type, a case in `game::Values` at six sites, a widget in the properties panel, a case in each binding, two in the control surface and three in the manifest generator. Sixteen sites, and the compiler found every one of them — which is the dividend of `PropertyType` being a closed list that everything switches over exhaustively.
- **`Label::Text` and `Entry::PlaceholderText` are `std::string`; `Picture::Image` is still a `core::Name`, and that split is the actual decision.** An asset id is one of the bounded set of things a game shipped, so interning it buys an integer comparison everywhere downstream. A score is computed. The rule is short enough to apply without thinking about it: **a value the game picks from a set is a `Name`; a value the game computes is a `String`.** Both halves are pinned — one case asserts a thousand distinct labels move `core::Name::Count()` by zero, the other asserts a fresh image name still moves it.
- **The storage change cost nothing that was not already paid.** `Label` and `Entry` stop being trivially copyable, so they need written serialisers — which they already had, because a `core::Name`'s id is process-local and could never have been memcpy'd to a file either. `ecs::Column`'s non-trivial path has existed since v0.2 and this is its first user.
- **The one genuinely sharp edge, and it bit in a test before it could bite anywhere else.** Every binding reads a property into a shared `alignas(16) unsigned char bytes[...]` buffer and lets the descriptor fill it. A `String` getter *assigns* rather than filling bytes, and assigning a `std::string` into uninitialised storage is undefined behaviour — so all four callers take an explicit branch before the buffer, and the `PushValue`/`ToJs` switches refuse the type loudly rather than handling it. `gui/tests/Compile.cpp` walks *every* declared property and segfaulted on the first run, which is the failure arriving where it is cheap.
- **Nothing an author can see changed.** `engine.d.luau` and `engine.d.ts` are byte-identical across the change: both said `string` before and after, because whether the engine interns text is a storage concern and a type position should not leak one. A game file is unchanged too — `TypeTag` writes `string` for both and the schema check in `Game.cpp` now compares tags rather than enum members, so moving a property between the two is not a format break.
- **Measured, since it is a per-frame path.** `DrawCommand::Text` is owned as well, which makes a draw command non-trivial — but the compiled list is rebuilt only when the tree's hash moves, and `Compiled::Rebuild · 1k elements` went 136 → 139 ns per element, inside the run-to-run noise. The hash now folds every byte of a label rather than an interned id, which is the one real cost and is the one that has to be paid: a score going from 19 to 91 has to be seen to have changed.
- **`examples/Interface.luau` keeps its ten-hertz clock**, and its comment is unchanged because it never cited this: the throttle is about the compiled-list cache doing something measurable, and that reason survives the leak going away.

### [_] D00019

**The engine's Luau is held at the revision the editor tool can consume so that
the editor and the type check agree. The current engine revision is Luau 0.732.**

- `mono.vendor/luau` is pinned to commit `f8ca77ac` (Luau **0.732**), and `mono.vendor/luau-lsp/luau` must be pinned to the same commit when that optional submodule is checked out. `mono.tools/scriptcheck` links the first and gates `just typecheck`; the language server in an editor uses the second. Two Luaus would mean an author reading diagnostics from a language the engine does not run, which is worse than no editor support because it looks authoritative.
- **The exact upstream ceiling belongs to luau-lsp.** Its nested Luau must remain buildable against the language-server sources. Do not bump the engine submodule alone: the sync check is the contract, and a failed `just luau-lsp` is preferable to silently giving authors diagnostics for another language revision.
- **Checked, not written down.** `just luau-lsp` compares the two `HEAD`s and refuses to build when they differ, naming both. Verified by mutation: bumping `mono.vendor/luau` alone makes the recipe fail with the two SHAs printed. Without that, the drift is invisible — the engine keeps passing every check it has, and only an editor is wrong.
- **What the choice actually costs, so a later reader can weigh it.** The engine follows the editor's compatible revision rather than independently following upstream. The trade is only defensible while the gap stays small; a long-lived gap would invert it, and the answer then is the fork below rather than a wider gap.
- **The fork is the way out and was declined at v0.7 on purpose.** Pointing luau-lsp at `mono.vendor/luau` needs sixteen mechanical call-site changes, and `mono.vendor/AGENTS.md` says a patch goes upstream or into a fork whose remote is recorded in `.gitmodules` — never into a file in this tree. That is a fork to maintain against a moving target, for a developer tool.
- **Reopen trigger: luau-lsp syncs to a later Luau revision.** Bump both submodules together, run `just luau-lsp` — which refuses if only one moved — then run `just check`.

### [_] D00018

**What a game replicates is written out three times, and the third copy was added at v0.7 by the change that noticed it.** Filed rather than fixed, because there is nowhere correct to put it yet and the obstacle is a layer rather than an opinion.

- The table pairs a component name with a `replication::ChangeDetection`: `scene.Transform` and `scene.Motion` observed, `scene.Bounds` and `scene.Visual` signed. It appears in `mono.server/src/Server.cpp`, `mono.unified_server_client/src/Harness.cpp` and `mono.studio/src/PlayLink.cpp`. **All three agree today**, which is the only reason this is an entry and not a bug.
- **The pairing is not arbitrary and that is what makes drift expensive.** A `Transform` is written every tick by a system, so the dirty bits already know and hashing it would be a pass over the world to learn what was free. `Bounds` and `Visual` are written once by a script and then never, so observing them buys a dirty column paid for every tick and read never — and *not* signing them is the bug v0.7 fixed, where a part recoloured by a script kept its old colour on every client for ever. **Getting one entry wrong in one program is silent in both directions**: the wrong detector sends nothing and reports nothing.
- **Why it is not simply hoisted.** `scene` owns the components and already pairs each with its wire form in `Registration.cpp`, which is exactly the right shape — but `scene` is L7 and `replication` is L12, so `scene` cannot name `ChangeDetection` without inverting the layer rule. Putting the table in `replication` instead makes a module that must not know what a component *is* name four of them, which is the property that keeps `net` and `replication` separable at all.
- **The real answer is the one `mono.server` already wrote down and then did not get**: *"These four are the placeholder scene; a game file names its own at v0.5."* A `<Replicated>` section in the game document is a per-game declaration read by whoever loads it, which deletes all three copies rather than moving them — and it is the only version of this that also lets a game replicate a component `scene` does not own.
- **Reopen trigger: a fourth copy, or the first component whose detector differs between two of the three.** The second is the one that bites without warning — the copies are in three programs, so nothing in the build compares them, and the symptom is a value that crosses in the studio and not on a server.

### [PARTIAL] D00017

**The hosting half of L12 — orchestration — has been a `TODO(v0.2+)` in `mono.engine/CMakeLists.txt` since v0.0 and is not scheduled by any version.** Converted from a marker to an entry at v0.6, because a `TODO` naming a version that shipped three releases ago is the exact failure `docs/retired/v05.md` already records once.

- L12 is the tier that touches a device. `input` and `render` are its client half and are guarded by `MONO_BUILD_CLIENT`, so a headless build configures neither — which is what `just check-server-is-headless` proves by linking. **Orchestration is the mirror image**: a `[server]` module about processes, placement and lifetime, behind a `MONO_BUILD_SERVER` guard, and the comment in `CMakeLists.txt` says so in one line already.
- **What it does not have is a caller.** `mono.server` hosts one world in one process. `--worlds N` runs several in `parallel/process`, which is a *harness* rather than a hosting layer — it starts what a benchmark asked for and answers nothing about who starts a world in production, where it goes, or what happens when it dies. Building the module before something asks those questions produces a guess with a `MONO_BUILD_SERVER` guard on it.
- **Why this is not simply deleted.** The guard structure is the load-bearing part and it is already correct: the client half proves the pattern works, and the symmetry is what stops somebody putting a server-tier device module inside the `MONO_BUILD_CLIENT` block because that is where the other L12 modules live. The line is worth keeping; the version on it was not.
- **v0.7 changed what this entry is about, and it is no longer "nobody asks the questions".** The prediction above was right in the letter and wrong in the consequence: the studio does host its worlds in its own process, which is indeed the case orchestration is least needed for — and then it **answered two of the questions anyway**. `Editor::UpdateWorldLifecycle` decides when a world stops (idle at `IdleCloseSeconds`, 300 s by default), when it starts again (something is sitting in its inbox, which is reliable precisely because a suspended world is the one world whose inbox nothing drains), and three exceptions that are not obvious and were each arrived at by being wrong first: never the last world, never a world outside a scoped run, and being *looked at* counts as occupancy.
- **`mono.server` has none of it.** `--game FILE.agame` loads every world in the file and ticks all of them forever; there is no `SetState` and no `Suspended` anywhere under `mono.server/src`. So the lifecycle policy exists exactly once in this repository, and it is in the editor.
- **That makes the risk a second copy rather than a missing module, which is a different and cheaper thing to act on.** This repository's most expensive recurring bug is one policy written twice — `CapturePreviousTransforms` was five lines in `examples` that the studio needed too, `ReadSource` exists so a source cache cannot be consulted from one entry point and not another, and there is deliberately **one** bus router so a world's behaviour does not change by being hosted elsewhere. A server that grows its own idle policy makes a world that closes on one host and not the other, with nothing reporting it.
- ~~**So the narrow action is available before the module is**~~ **— done at v0.10, and only that half.** `engine::world::DecideLifecycle` is the policy, in `mono.engine/world` at L4 `server`, which both programs already link. `mono.studio` calls it; placement, which genuinely has no caller, is untouched and still waits for a deployment.
- **The split that made it hoistable is decision versus gathering.** Whether somebody is *looking* at a world is a question only an editor can answer, and whether a world is inside a scoped run is a `WorldRun` concept meaning nothing to a server — so those stay in `mono.studio` and arrive as facts in `LifecycleInputs`. What moved is the part that must not differ between hosts: the thresholds, the order the tests are applied in, and the three refusals.
- **The dividend that arrived first was not the one this entry argues for.** The case against a second copy is right, and `mono.server` still has no caller — so nothing has been de-duplicated yet. What changed immediately is that the policy became **testable**: every branch of it was previously reachable only by opening the studio and waiting five minutes, and there are now eight cases, including the two that were pure comment before — a `Faulted` world belongs to the supervisor, and occupancy cannot wake a suspended world because nothing can occupy a world that is not running.
- **One real ordering bug came out of the move.** Routing the studio through the shared decision put the idle-clock lookup ahead of the suspended-world case, so a suspended world with a teleport waiting would have been delayed a frame while an entry it has no use for was created for it. The clock is now looked up only for an `Active` world, which is also the honest statement of what an idle clock is for.
- **`mono.server` is deliberately not wired up.** It loads every world in a game file and ticks all of them forever, and giving it suspension is a behaviour change to a program whose output `just determinism` and `just replay-check` compare byte for byte. The policy being reachable is what this entry asked for; using it is a decision with its own consequences.
- **Reopen trigger, split in two because the halves are no longer due together.** *Lifetime* — when a world starts and stops — **is hoisted and this half is closed**; what is left of it is a caller in `mono.server`, which is a decision about server behaviour rather than about where the code lives. *Placement* — which host a world runs on, and what happens when it dies — is unchanged: more than one world hosted by something that is not a test harness and not a single-process editor. That is a deployment.

### [CLOSED] D00016

**A frame is described in two places as of v0.6 and nothing checks that the two agree.** Filed the moment the second description appeared, rather than after they disagreed.

**Closed at v0.11 by deleting the second description rather than checking it**, which is what the reopen trigger below said to wait for — and it went exactly the way that note warned it could not go otherwise.

- `render::PassOrder()` **reads its names out of `graph::StandardGraph()`**. It was a hand-written array of the same six; it is now a view of the one description. Drift is not possible rather than detectable, which is `AGENTS.md` rule 6 applied to the thing rule 6 was quoted about.
- **`graph::Pipeline` and `StandardPipeline` are deleted, along with their tests.** `RenderGraph` supersedes them entirely — the same declaration-ordered stages with the same read-before-write check, plus resources, the shared/per-view partition and an executor. Keeping the flat list beside it would have been the third description this entry explicitly forbids. "Delete the thing you replaced."
- **The comparison test is kept and repurposed.** It can no longer catch two lists disagreeing, because there is one; what it now checks is that the derivation lines up with the `Pass` enum, which is still written by hand. Mutation-verified: swapping `overlay` and `interface` in `StandardGraph` fails `tests/Passes.cpp` at both the count and the position.
- **The hole that remains is the one this entry always named.** A pass added by writing `SDL_BeginGPURenderPass` inline rather than entering through `PassRecorder` is still invisible to all of this. Closing *that* is the executor — `D00002`'s remainder — not another list.

- `Renderer::Render` submits a shadow pass, a surface pass, an opaque pass, a transparent pass and an overlay pass, in that order, from a function that knows all five by name. `graph::StandardPipeline` is the same five as data, and `Pipeline::Validate` checks the one property that goes wrong at this size — a stage reading a target no earlier stage wrote. **Neither knows the other exists.** `render` links `graph` for `Cull`, `Frustum` and `FitDirectionalLight`; it never reads a `Pipeline`.
- **This is D00002's "`render` must not grow a hand-rolled pass list" being overrun, and the overrun is not the interesting part.** The passes had nowhere else to live — the graph runtime describes and does not execute. What is worth filing is that the mitigation is a sentence: `mono.engine/render/AGENTS.md` now says *"if you add a pass here, add its stage there in the same change"*, which is **rule 6 out loud** — a rule the build does not check is documentation.
- **The check is built.** `render::Pass` and `render::PassOrder()` name the five stages in submission order; `mono.engine/render/tests/Passes.cpp` compares that list against `StandardPipeline`'s stage names, in order, with no device — the comparison is arithmetic over `core::Name`, so it runs anywhere. **Demonstrated by mutation rather than asserted**: a sixth stage added to `StandardPipeline` and nowhere else fails with *"Renderer submits 5 passes and StandardPipeline declares 6 stages"*. `just check` runs `test-all`, so it is every run and not only a changed-suite run.
- **The submission order is checked at runtime, because that half is the one a headless test cannot see.** `PassRecorder` walks `PassOrder` as the frame is built and refuses to go backwards — skips are the normal case and allowed, since every pass here is conditional and `Stage::Optional` already says so. It logs rather than aborts: a renderer that kills the process over its own bookkeeping is worse than the bug it found. `FrameResult::Passes` carries the bits out, which answers a question the draw-call count cannot — running `Mirrors-1-world` with and without `--stats` is 5 draw calls and 4, and only the bitmask says the missing one is the overlay pass rather than a geometry draw.
- **Two holes, named rather than implied.** A pass added by writing `SDL_BeginGPURenderPass` inline instead of entering through `PassRecorder` is still invisible — the convention narrowed from "remember to edit another module" to "enter passes through the recorder", which is greppable, but it is still a convention. And the smart-test signature is a hash over each suite's *header* closure, so a body-only edit to `StandardPipeline` re-runs nothing under bare `just test`; the renderer side is caught either way, because adding a `Pass` member edits `Renderer.hpp`, which is in the closure. **The asymmetry favours the direction this entry was written about.**
- **What is deliberately not done: running the frame from the list.** That is the render-node system, and a small version of it now would be the second executor `mono.engine/render/AGENTS.md` says is worse than one hand-rolled list. What is checked is the description, not the execution.
- **The trigger fired at v0.7, the check held, and the thing beside it did not — which is more useful than a clean pass would have been.** The studio needed its panels drawn inside the frame, so `Pass::Interface` is a real sixth pass, and it went into `render::PassOrder` *and* `graph::StandardPipeline` in the same change because the count-and-order comparison fails otherwise. That comparison is generic and it worked: **evidence rather than reassurance**, and the first time one of these has been demonstrated by a change somebody actually wanted instead of by a mutation.
- **What did not hold was the test next to it, and the distinction is the lesson.** `tests/Passes.cpp` also spells out each pass's *name* by hand, and `interface` was never added — so the one pass this entry's trigger is about was the one pass whose name nothing checked. The file's own header stayed at "five passes" too. **A generic assertion survived the change and both hand-maintained lists beside it rotted**, in the same file, written by the same argument. Fixed at v0.7, with the count deliberately removed from the prose rather than corrected: a number in a comment is the thing that goes stale, and this file exists to stop exactly that one level up.
- **What it did not do is close the entry, and the distinction matters more now than when it was filed.** Keeping two descriptions in step is not the same as having one. v0.8's `gui` rendering attaches a **seventh** pass at exactly this seam, and the cost of the convention is paid again per pass, forever, by whoever remembers.
- **Reopen trigger, now fired: a sixth pass shipped at v0.7 and v0.8 has started.** Both halves of the original trigger are met. **This should not be actioned on its own** — running the frame from the list is `D00002`, and a runtime *deletes* the second description rather than checking it, so doing this entry separately buys a third copy of a mitigation that is already a sentence in an `AGENTS.md` and a comparison in a test. Merge it into that work or leave it as the standing check; do not build a third thing here.

### [_] D00015

**Three proposals for replication bandwidth, recorded together because they interact and separately because they are not equally ready.** Written before any code, in the shape `v02v03v04.md` used: the open questions are the point, not the plan.

**(a) Lossy quantisation on the wire — DONE.** Wire version 4.

- **`scene::Transform` is 28 bytes and crosses in 10; `scene::Motion` is 24 and crosses in 12.** Position is three fixed-point axes and rotation is smallest-three — the largest component dropped, three sent at ten bits each, a two-bit index, exactly one 32-bit word. **Measured 25 entity values a datagram becoming 50**, against the real 1159-byte limit with `ChunkBytes` asked for above what can ever fit. The predicted 2.5x was optimistic by exactly the eight-byte entity handle, which does not shrink: the ceiling is 36/18 and the measurement lands on it. `just unified` at 64 entities is 6 messages and 4685 bytes a tick becoming 3 and 2612.
- **The largest message did not move, and that is the answer rather than a disappointment.** The packer fills to `ChunkBytes` whatever the stride is, so what changed is how many entities are in a datagram and not how big one is. At 2000 entities the largest message *rose* twelve bytes, because the budget is filled more completely.
- **The seam is a second pair of hooks on `ecs::TypeDescriptor`, not a codec over `Write`, and the difference was demonstrated rather than argued.** `Save` and `Load` are what a recording is made of. With the codec installed over `Write` instead, **`just determinism` and `just replay-check` both still passed** — they were comparing one lossy file against another. **That is a limit of both recipes worth knowing on its own**: they prove two runs agree, not that either is right. The mutation is killed by one case and by nothing else in the tree, which is why `TypeDescriptor::Wire` is its own slot and why `ecs/AGENTS.md` now carries the convention the build cannot check.
- **A wire form is installed by the registration that names the type**, which is what makes the two ends agree without either being told. The alternative considered — a table `replication` keeps by component name — makes agreement a discipline repeated in three programs and every test, and forgetting one is a receiver reading ten bytes as twenty-eight.
- **The snapshot path and the delta path were two places and are now one decision.** `BeginSnapshot` puts every value with a wire form *through* it before copying into the scratch store, so a joining client is given what the far side would have decoded. Without it a client's world depended on when it joined — which never shows as a failure and always shows as drift between two clients.
- **The grid is stated as the world's extent divided into steps, and the error is a bound in metres.** ±64 m in 32767 steps each way: 1.953 mm apart, **0.977 mm per axis anywhere in the world including both walls**, 1.69 mm on a 3D distance. Rotation is **0.0042 rad (0.24°)**, derived and then measured at 0.00408 over 400k orientations. Velocity is coarser on purpose and the justification is a test rather than a sentence: 3.9 mm/s over one 60 Hz tick is 65 µm, fifteen times under the position grid. 32767 of 32768 codes, so `+HalfExtent` is exactly representable — `Bounce` pins entities there, so the far wall is the common case and not an edge one.
- **Outside the extent an entity is clamped, on decode as well as on encode.** A clamped entity piles up against a wall somebody can see; a wrapped one is at the far side of the world and indistinguishable from a teleport the server meant. The decode clamp is not belt and braces: the encoder never emits -32768, so a trusting decoder would put a peer's entity outside the world this module states everything is inside. `WireCoversWorld` is the check for a world larger than the grid and it is a `static_assert` **where the world's size is authored**, because the encoder sees one component and not a world.
- **Both ends decode identically by construction, which is what (b) needs.** Every scale is a whole number over a power of two and a decode is one correctly-rounded division, so the value a client holds is one the server can predict bit for bit. Encoding is done in `double` — a float multiply near 32768 rounds to the wrong code and pushed the worst case 0.4% past half a step, which would have meant a bound with an apology in it.
- **Twelve mutations, twelve killed, one needed a new test.** Measuring the message-fit check against `sizeof` rather than the wire size survived the first sweep: it silently refuses a component that would have fitted, and nothing built a component large in a store and small on a wire.
- **Not done: the join snapshot is not itself smaller.** It carries the *decoded* value so that snapshot and delta deliver identical bytes, but it is still written by `Store::Save` at full width. Narrowing it means giving `Save` a lossy mode, which is the exact thing this design refuses — and the join is a one-off spread across ticks where the delta is every tick.

**(b) Group signatures — an audit layer, and the most interesting of the three.**

- The server hashes groups of replicated state and sends the hashes; the client hashes its own copy and reports mismatches; the server sends the true state on a later tick. **Anti-entropy over the replicated world.**
- **It is complementary to deltas rather than a second way to do one job**, which is the question `docs/CODE_QUALITY.md` asks and the one this has to answer. Deltas are the fast path — what moved. Signatures are the audit — what disagrees. The audit is what makes the delta path's optimism safe, and it would catch **generically** the whole class of bug this version chased one cause at a time: the lost creation, the stranded value, the stale forget, the tick that never completed.
- **The open question is whose hash it is, and it decides the cost.** A per-client hash cannot be shared, so the work scales with clients times entities. A hash over a **spatial cell** is computed once and shared by every client that can see it — far cheaper — but only holds if a client sees a whole cell or none of it, which turns interest management from per-entity into per-cell. That is a real change and possibly a good one; `spatial::HashGrid` already exists, and `assets::HashTree` is a working Merkle implementation with tagged interiors and the leaf count sealed into the root.
- **The client's reply is upstream traffic from a peer, and `replication/AGENTS.md` says every field of an inbound message is hostile in both directions.** A client claiming everything mismatches is request amplification. The rate limit is therefore not a tuning knob but part of the security argument, and it has to be **enforced by the server** rather than trusted from the client.
- The cadence argument is the strongest part of the proposal and should be written into whatever ships: **anything genuinely moving is already corrected by ordinary deltas, so the audit only ever catches *stale* divergence — which by definition is not urgent.** A rotating slice of groups per tick bounds the hashing cost, the wire cost and the repair cost at once, and it is the same rotation the priority work already uses.

**(c) The client simulating physics from the quantised state — last, and only with an invariant amended on purpose.**

- `replication/AGENTS.md` currently forbids it: *"Prediction is the local player and nothing else. Predicting a second entity means predicting what another player will do, which is wrong more often than it is right and is visible as rubber-banding when it is wrong."*
- **That rule was written about players, and this proposal is about objects.** Predicting an input-driven agent is guessing at a human; dead-reckoning a ballistic crate is evaluating a known function. They deserve different rules and the invariant does not currently distinguish them. **Amend it deliberately or not at all** — quietly reading it narrowly is how an invariant stops being one.
- **The hazard is error growth, and it is different in kind from (a)'s.** Interpolating between two quantised poses keeps the error inside the quantisation step. *Integrating* from a quantised velocity accumulates it linearly with elapsed time, so the bound is a function of how long since the last correction rather than of the grid.
- **It collides head-on with `D00010`'s decision**, which was that a dry buffer **stops rather than extrapolates**, on the stated grounds that guessing forward is "a freeze plus a lie" — the snap arrives when the next tick disagrees with the guess. Reconciling those is the actual design question and it is answerable: a physics-driven object has a *right* answer to extrapolate toward and a player does not.
- Needs `physics` on the client for the entities it extrapolates, which today it does not link.

**Sequencing, and it falls out of the above rather than being chosen:** (a) is self-contained, needs no invariant changed, and makes (b) sound. (b) is a design with one open question. (c) needs a rule rewritten and a bound nobody has measured. **Reopen trigger for (a): the first world whose delta does not fit at the current budget** — which the priority work made survivable rather than fatal, so it is now a bandwidth question rather than a correctness one.

### [_] D00014

- **QUIC underneath `net::Transport`, replacing the hand-rolled reliability, handshake and framing.** Raised as a direction rather than a complaint: what is built works and is tested, and the argument for QUIC is not that ours is wrong but that a great deal of it is a worse version of something standardised.
- **What it would buy, in the order the arguments actually weigh.** Congestion control, which **this engine has none of** — `LinkSettings` has `BytesPerTick` and `PacketsPerTick`, and a *fixed cap is not congestion control*: it does not back off when the path is congested and it does not open up when it is not, so on a real internet path it is either wasting the link or contributing to a collapse it cannot detect. Then per-stream loss recovery without head-of-line blocking across streams, which is exactly the shape this module arrived at by hand — structure reliable, values not. Then TLS 1.3, which subsumes the engine's X25519/HKDF/ChaCha20-Poly1305 and server-identity binding, now closed in D00006. Then connection migration and 0-RTT resumption, neither of which we would build.
- **It passes this repository's second-consumer test, which is the standard that justifies work here.** The game link is one. `ROADMAP.md`'s cdn wire streaming is the other: it is blocked on `net` growing an `http/` sub-area, because a content origin serves bulk bytes over request/response rather than over a game datagram channel with a per-tick budget. **HTTP/3 is QUIC.** One dependency answers both, and the alternative is hand-rolling a second protocol beside the first.
- **The seam already exists and was built for exactly this.** `net::Transport` is the interface a caller cannot see through, `Endpoint` is this engine's own value type precisely so that no public header names a socket or an `error_code`, and `replication` at L12 names entities and components and hands bytes down. So QUIC is **a `Transport` implementation plus the deletion of the reliability layer**, not a rewrite of `replication`. That is the cheap part and it is worth saying, because it makes the expensive parts legible.
- **The blocking obstacle is the clock, and it is this module's central invariant rather than a detail.** `net/AGENTS.md`: *"Every call that could care about now takes it as an argument. There is no `Clock` member and there must not be."* Every QUIC stack runs loss detection, pacing and congestion control off timers of its own. Some can be driven entirely from a caller-supplied time — `picoquic` and `ngtcp2` take an explicit timestamp on every entry point — and others cannot without fighting them. **That choice is the whole feasibility question**, and it has to be settled before a library is picked, not after: get it wrong and `just determinism` and `just replay-check` stop meaning what they say, which is the one thing this repository checks rather than claims.
- **Three more costs, none fatal and all real.** A QUIC library needs a TLS stack — BoringSSL or quictls — which is a large addition to `mono.vendor` and threatens the property `MONO_VENDORED_GLSLC` exists to protect, that a fresh clone needs CMake, Ninja and a C++ compiler and nothing else. **Unreliable datagrams are an extension (RFC 9221), not core QUIC**, and this engine is unreliable-first with reliability opted into *by message kind inside `Session`* — a library with weak DATAGRAM support would push the game link toward reliable-everything, which is precisely the failure `ChannelFor` exists to prevent and which v0.3 already wrote down as how one lost packet becomes a visible stall. And head-of-line blocking still applies *within* a stream, so the stream layout is a design decision rather than a free win.
- **What gets deleted, and it should be deleted rather than left beside it.** `Handshake`, `Cipher`, `Cookie`, `ReliableSender`/`ReliableReceiver`, most of `Link`'s state machine, and the packet framing. That is most of v0.3's `net` and most of this session's `D00006`. Sunk cost is not an argument for keeping it, but **two overlapping reliability stacks is a worse outcome than either**, so this lands as a replacement or not at all.
- **Correction at v0.7: this entry's trigger named a version rather than a thing, and the version moved.** It said "v0.8's cdn wire streaming"; the scripted interface took v0.8 and cdn wire streaming is **v0.9's**. Nothing about the argument changes — the trigger was always the streaming, not the number — but for a version it read as due when it was not, which is the same drift `D00004`'s figure and `D00001`'s "two of four" are recorded for. Stated against the work from here on.
- **Reopen trigger: whichever comes first of cdn wire streaming — v0.9 as this is written — or the first deployment over a path that is not loopback or a LAN.** The second is the one that bites without warning — the absence of congestion control is invisible until it is a stall nobody can reproduce, and `ConnectionStats` counts refusals against our own fixed budget, not against what the path would have carried.

### [DELETED] D00013

**Closed by doing it, and the entry's own reopen trigger is what settled it** — `replication/tests/Loss.cpp`'s *"a value lost from a tick that took several messages is not repaired"* started failing, and was rewritten to assert the repair with the same world and the same nominated datagram.

- **The wire change is three bytes and the version is 3.** `Delta::Part` and `Delta::Final` — a position and a marker — between the baseline and the component count. They come out of the packer's existing 64-byte overhead allowance rather than out of `MAXIMUM_MESSAGE_BYTES`, so no budget moved; a delta's real header is 23 bytes and is now 26. **The module has had five bugs from messages that did not fit, so this is now measured rather than argued**: `engine.replication.stream` builds at the capped `ChunkBytes` and asserts every message against `net::Packet::MAXIMUM_MESSAGE_BYTES`, and a mutation removing the overhead from the cap kills it.
- **"All parts" means the parts the sender emitted, and getting that wrong would have broken the budget path rather than the loss path.** The marker is written by `Authority::Pack` after packing has finished, on the last part that actually went. A tick the priority rotation trimmed is therefore *complete*: what was held back was never part of that tick, it keeps its unconfirmed entry and comes back later. A marker meaning "nothing else changed" would leave every tick of a world larger than its link unacknowledged. `just unified --entities 2000` is byte-identical before and after.
- **The give-up rule is that there is nothing to give up on, and that is the finding.** An incomplete tick is abandoned the moment a newer one arrives — never waited for, because the unreliable channel does not resend. It is sound because every value the missing part carried is still unconfirmed, so the next tick re-offers it and acknowledging *that* tick confirms it. **The bound is one tick of acknowledgement per lost part**, asserted as `Applied == lostAt + 1` rather than described. The bound on the case where no tick ever completes is `ResnapshotAfterTicks`, this module's existing answer to a client that cannot be caught up by deltas.
- **The two alternatives, and what they cost.** Bounding the wait and then acknowledging anyway reinstates this entry at a lower rate — the ack retires the missing part's values, and **a bound on how often a bug happens is not a fix**. Escalating to a re-snapshot spends the whole visible world to repair one colour, on a client that is one tick behind rather than adrift.
- **A part number is a position, not an arrival order.** The receiver keeps a set of positions and walks it to the final index. A count of arrivals passes a case where every part is delivered twice and fails only when one is missing *and* another is duplicated, which is the case that was added.
- **A refusal by the transport was the regression this nearly shipped, and an existing case caught it.** With `PacketsPerTick` below `MessagesPerTick` the link refuses the tail of every tick, so no tick could ever complete and the client was re-snapshotted every 120 ticks for ever. `Authority::Unsent` now rolls `Client::Streamed` back for a tick the transport cut short: **a client cannot acknowledge a tick it holds only some of, so that tick must not be what its silence is measured against.** Same argument as the quiet world and the held-back budget, one layer down.
- **The snapshot buffer needed no change and that is the point.** `client::RecordReplicatedTick` is fed `Replica::Applied`, which now skips an incomplete tick — so no pose is ever taken from a store holding one datagram's rows beside another's previous values.
- **Eleven mutations, eleven killed, and one needed a new test.** Removing the part-record reset on a re-snapshot survived the first sweep — its only effect is miscounting `Statistics::Incomplete` after a rejoin, which is the number an operator reads to tell a lossy link from a broken one. The sequence is real rather than contrived: a streaming snapshot sends no delta at all, so the last delta before a rejoin is often an incomplete one nothing supersedes.

### [DELETED] D00012

**Closed by doing it.** The crossover was re-measured at `-O3` for `Each`, `EachBatch` and `IntegrateMotion`; `Jobs`' two constants are unchanged and their justifications are not; `physics::INTEGRATE_GRAIN` moved from 512 to 1024. Kept rather than deleted, because *why nothing changed* is the finding.

- **The 17.6% on `EachParallel · 10k` was never the job system, and believing it was is the confident wrong answer this entry invited.** 10k rows is below `DEFAULT_GRAIN * MINIMUM_GRAINS`, so the floor did exactly its job and that row times the **inline** path — `engine.ecs.parallel` now carries a case requiring `Participants == 1` there, twenty-five times running, so nobody re-derives it. What moved is the optimiser. That body writes one float of a twelve-byte row, and `-O3` vectorises a stride-12 read-modify-write into shuffles slower than the scalar loop `-O2` emitted. Proved by rebuilding the same translation unit at `-O2`: `Each · 10k` (three adds) **4.04 us to 1.76**, `EachParallel · 10k` (one add) **2.12 to 2.44**, and a new serial control with the *same* one-field body **2.13 to 2.40**. **The serial and parallel one-field rows move together and neither moves with `Each`.** The 2.12 us reproduces the accepted `-O2` baseline of 2,110 ns to three figures. Two rows that were never a fair pair, too — `Each · 10k` does three adds and `EachParallel · 10k` does one — so `Each · one field · 10k entities` now sits beside them as the row to read against.
- **The crossover for the cheapest body is ~262,144 rows, not 32,768 and not the 60-80k this file has carried since v0.1.** Measured at `-O3` on 24 threads, three float adds per row, `Each` against `EachParallel`: 8k **1.45 us / 25.9**, 32k **5.54 / 31.5**, 128k **23.3 / 36.1**, 256k **49.1 / 48.6**, 500k 1.33x faster batched. `EachBatch` gives the same crossover. **So the floor of 32,768 permits a measured 5.7x loss**, and the ceiling past the crossover is 1.3x rather than 3.5x — at 500k both paths stream twelve megabytes and the limit is bandwidth.
- **The other half of the ratio had never been measured on its own, and now is.** `engine.parallel.bench.dispatch` is new and times an empty `For`: **48 ns** for the decision not to dispatch, **31 us** dispatched to 23 workers, **2.3 us** dispatched to one, ~95 ns per further range. **The handover is linear in the pool, not in the work** — every worker decrements `Batch::Outstanding` under `Pool::Guard` whether it took a range or not, so a batch waits for 23 threads to take one mutex in turn. That serialised join, not the `notify_all`, is what a short span cannot repay, and it is the thing to attack if the crossover ever has to come down. That is a rewrite of the join, not a change to a constant.
- **A grain constant is a row count and the thing it is trying to express is a duration, which is why one default cannot serve two bodies.** The two crossovers are 262,144 rows and 8,000 rows — 32x apart. As serial work they are 49 us and 29 us — one handover either way. Rows differ by the row cost; microseconds do not.
- **So both `Jobs` constants stay, and the reason is that neither can move.** `MINIMUM_GRAINS` multiplies *every* caller's floor including the one that measured: physics passes 1024 and wants its floor at 8192, and 64 would put it at 65,536 and give back a measured 1.8x. `DEFAULT_GRAIN` would need to be 32,768 to put the floor where the cheap body wants it, which would break the only long-lived caller that takes the default — `mono.client/src/Replicated.cpp` writes a `CFrame`, two vectors and two ids per row and is worth dispatching an order of magnitude sooner. One constant, two jobs, opposite directions. The justification at both now says so with the numbers, replacing one that described a machine that no longer exists.
- **The second data point asked for, and it came back as another stale constant.** `INTEGRATE_GRAIN`'s own comment claimed a crossover at ~4096 rows; at `-O3` it is **~8000**, so 512 was dispatching a six-thousand-row world into a 1.27x loss. **1024 now**, floor at 8192, on the measurement — and better at every count above it too, by 9-18%, because a range costs ~95 ns to hand out whatever is in it. Same failure as this entry's, one version later, in the constant that was *supposed* to be the careful one.
- Worth keeping, because it is the shape of the problem rather than an instance of it: **`Jobs::For` already separates the two questions and `Store` does not.** `For` takes `minimum` beside `grain`; `Store::EachParallel` and `EachBatchParallel` expose only `grain`, so an ECS caller can move its floor only by distorting its range size. physics did exactly that and landed right by coincidence of the coupling. Not plumbed through, because that is a public parameter with no caller and `D00008` is the entry about adding one of those.
- **Every number here was taken with about three of twenty-four hardware threads busy with someone else's process.** Serial rows are min-of-61 and reproduce to ~3%; parallel rows carry spreads from ±85% to ±860% and their minima wobble ~25% between runs. Nothing above turns on less than 1.5x. The baselines were deliberately **not** accepted.

### [DELETED] D00011

**Closed by doing what the entry said to do first.** The reopen trigger was "the first packet loss on a link that is not loopback" and the advice was that building a link that drops was worth more than the fix. It was: the lossy transport found four more bugs than this one, **two of which needed no packet loss at all** and were therefore live in the shipped code.

- **`net::LossyTransport`** — a wrapper over any `Transport` that discards, duplicates and reorders arrivals under the caller's control. Deterministic by construction: no clock, no `std::random_device`, and whether arrival *n* is lost is a pure function of *n* and a stated seed, so a failure is reported as a seed and reproduced from it. Loss is applied on the way *in*, so `Send` never has to invent or hide a status the transport underneath would have given. `DropNext` rather than a percentage is what made these cases deterministic instead of flaky.
- **The fix is not the protocol change this entry proposed, and the entry's own argument is why.** Acknowledging structure entity by entity is a second acknowledgement channel beside `net::ReliableSender`, which is already a per-message acknowledgement channel with a window, a resend timer and a bound. What shipped instead: creations and destroys left `Delta` and joined forgets in a `Structure` message, which `Session::ChannelFor` puts on the reliable channel — **where the forget already was, for exactly this reasoning. The asymmetry was the bug.** Wire version 2.
- **Extending v0.3's unconfirmed-entry mechanism to known-set edits was tried on paper and does not close it**, which is worth recording because it is the obvious cheap answer. It retires against `Applied`, and `Applied` names a tick rather than a message: a tick split across several messages is acknowledged by the client on the strength of the ones that arrived, so an edit in the lost one is retired unconfirmed and the hole is as permanent as before. Making it sound needs per-part completeness on the wire, which is the protocol change arrived at sideways.
- **Two things the reliable channel alone did not cover, both found by the new transport.** A structural message is not judged by its tick on arrival — it is resent six ticks later into a world that moved on, and refusing it as stale is how a destroy never happens. And a delta naming a row the client does not hold yet no longer advances `Applied`, which is what `Protocol.hpp` always claimed it meant: without it the creation arrived reliably and the entity held none of its components.
- **What the sweep found besides this, and the two that shame the existing suites.** A forget was discarded as stale whenever its tick also carried a delta — **no packet loss required**, and it survived because every existing forget case happened to sit on a tick where nothing else moved. And an entity entering a client's interest arrived with **none of its components**, because a delta comes from the dirty bits and an entity coming into view has not moved, so nothing was ever sent for it until it changed — which for anything stationary is never. Both fixed. The join chunk stream, the handshake retransmission and the priority rotation were all put under seeded loss for the first time and all held.
- **What was left is closed at `D00013`**: a value lost from a tick that took several messages, fixed by numbering the parts.

### [DELETED] D00010

**Closed by doing it.** Kept rather than deleted, because the entry's own refusal — "it cannot simply be given a `PreviousTransform`" — is what shaped the answer, and that is worth being able to point at.

- **`replication::SnapshotBuffer`, beside the tick agreement and not in the client**, exactly where this entry put it. It holds **per-entity pose history rather than snapshots of the world**: one ring of `HistoryTicks` poses per entity, one `uint64_t` and one `CFrame` each. A whole snapshot per tick was the obvious reading and is wrong at scale — it copies every component of every entity sixteen times over to interpolate a transform.
- **The delay is two ticks and the reasoning is at the constant.** What it buys is `DelayTicks - 1` tick periods of lateness, because one of them is the gap between two on-time arrivals: at zero there is nothing to interpolate between, at one the first slightly-late packet is a stall, at two there is one tick period of slack, and above about four everything that is not the local player is drawn far enough in the past that a player starts leading their aim. **A starting point rather than a measurement**, and the header says so and says to lower it only with one.
- **The dry buffer stops rather than extrapolating**, which this entry said was the case that decides whether the feature is good or annoying. On screen the world freezes at the last pose the server actually described and resumes from where it stopped; guessing forward is a freeze *plus a lie*, because the snap arrives when the next tick disagrees with the guess. A gap wider than the delay is walked back at five percent rather than teleported; past eight ticks it jumps once and counts it.
- **Prediction was the interaction this entry warned about, and it is structural.** `Record` refuses the nominated entity and the whole `CreatePredicted` index range *before* it does the tick accounting, so a caller cannot delay the local player by forgetting something. Found by a mutation showing only `Sample`'s half was tested — the buffer kept sixteen poses per predicted entity that nothing could ever read, and every test stayed green.
- **The tick rate had to be measured, which nobody predicted.** Nothing on the wire carries the authority's rate and the two programs do not share a default: `server --listen` paces at 30 and `client` at 60. A configured rate is therefore wrong by a factor of two in the most ordinary setup there is, which is not a drift a five percent correction absorbs. `MeasuredTickRate` is the ticks and the seconds the caller passed in, divided — a real run against a 30 Hz server reads 30.2.
- **Verified against a real `--listen` server rather than by reasoning**: 513 entities replicated, 512 drawn, **80% of poses interpolated rather than held**.
- Two things left undone and named rather than hidden. `SnapshotBuffer::Forget` exists and is tested but **no caller wires the forget list to it** — today nothing forgets, so the case cannot arise, but the day interest management starts forgetting, an entity that leaves view and returns within sixteen ticks will be interpolated across the gap. And the join transient is real and bounded: the rate is wrong by 2x for the first few ticks, so the clock stalls about once per tick and then refills over ~40. Every clean fix broke either the dry-buffer case or the resync case; **the one that actually removes it is putting the authority's tick rate on the wire**, which is a protocol change.
- **What remains is not this entry's.** The replicated world is drawn correctly and is still hard to *see*, because the composited camera is the demo world's — placed from a 24-metre scene's bounds, looking at a 128-metre one, so most of it is past the far plane and the rest is sub-pixel. `--view-spacing 0` overlays them and brings it into view. That is `mono.client/AGENTS.md`'s second gap, needs the predicted-entity promotion policy, and is recorded there with the numbers.

### [DELETED] D00009

**Closed the same day it was opened, by doing it.** Kept rather than deleted, because the entry's own stated test is what settled it and that is worth being able to point at.

- The finding: `release` and `bench` compiled at `-O2`, and the ECS iteration control ran 100k rows in 38.74 us at `-O2` against **16.61 us at `-O3`**. Found sideways, while disproving v0.4's vectorisable-layout item — measured at `-O2` alone, the packed and padded layouts look the same and that item reads as merely unhelpful rather than backwards.
- The objection was floating point: `-O3` vectorises and inlines more aggressively, and this repository diffs two runs byte for byte. **This entry named the measurement that would settle it — `just determinism` and `just replay-check` at `-O3` — and both are byte-identical.** GCC enables neither `-ffast-math` nor `-funsafe-math-optimizations` at any `-O` level, so IEEE semantics never moved. The whole suite was also built and run optimised, which nothing in the presets otherwise does: `release` has `MONO_BUILD_TESTS` off, so **the shipping optimisation level had never had the tests run against it at all**, and raising the level is exactly what surfaces latent undefined behaviour. 104 suites, 18 `ctest` targets, all pass.
- Both places moved, and the second one is the one that would have rotted: first-party targets now state `-O3` rather than inheriting `RelWithDebInfo`'s `-O2`, and `mono_add_benchmarks` pinned `-O2` of its own. Those two had agreed by coincidence, not by construction, so the benchmark binaries would have gone on reporting the old number for the thing that ships. `MonoLibrary.cmake` now says to change them in one commit.
- **See `ROADMAP.md` v0.4 for the measured outcome**, which is not uniform: serial row iteration roughly halves, and a handful of structural and query-planning paths get 4-12% worse. And see `D00012`, which is the new question this opened.

### [_] D00008

- **The single-player `ALLOW_TIER_ESCAPE` in `mono.client/CMakeLists.txt`.** It is written out in a comment there and deliberately not declared: `DEPS ... Mono::server` plus `ALLOW_TIER_ESCAPE Mono::server`, the one edge the tier rule has to permit by name rather than by rule, so that a `client`-tier program may link a `server`-tier library.
- v0.3's roadmap listed declaring it as part of wiring the two programs together. **The wiring turned out not to need it, and that is the finding rather than an excuse.** `--connect` talks to a server in another process over a UDP socket, which is precisely the arrangement where the client links no server code at all. Declaring it now would add an escape with no user — which is what the comment itself says not to do, and what somebody would eventually reach for to do something unrelated.
- **The escape now has a first user, and it is not the one this entry was written for.** `mono.unified_server_client` — a diagnostic product that runs both halves of replication in one process with `net` cut out of the middle — declares `ALLOW_TIER_ESCAPE Mono::server`, because it genuinely needs the client's draw seam and the server's world in one binary. **That does not close this item**: single-player is still undeclared in `mono.client/CMakeLists.txt` and still wants a game file first. What it does do is settle a question this entry could only speculate about — the escape works, the tier check names the edge, and the mechanism is no longer untried. When single-player arrives it is copying a line that has a working precedent rather than writing the first one.
- **The prerequisite landed at v0.7 and the trigger still did not fire, which is the useful part.** `mono.engine/game` exists and `mono.client --game FILE.agame` plays a game file single-player under `HostRole::OfBoth` — the exact line this entry named as the real prerequisite. It was declared **without the escape**: `mono.client` gained `Engine::game`, not `Mono::server`, because playing a game file needs the format and a VM and not a hosted server. So the entry's phrasing survives a second attempt to close it, and the reason is the same one v0.3 found — *hosting a server in your own process* is a narrower thing than it sounds, and twice now the feature that looked like it has not been it.
- **A second declared user arrived and it is a product rather than a diagnostic.** `mono.studio` declares `ALLOW_TIER_ESCAPE Mono::server`: an editor genuinely runs both halves, and `expected_graph.json` is where the fact is visible. With `mono.unified_server_client` that is two users, neither of them this entry's, and the mechanism is now ordinary rather than untried.
- **Reopen trigger, unchanged and now twice unmet: a client linking server code to host a server in its own process.** Restated against the link line rather than against the feature, because the feature has now shipped twice without needing it. When it does arrive the edge is two lines and the comment already says which two.
- Worth keeping straight, because the two are easy to confuse: the escape is about *linking*, not about connecting. A single-player client that spawned `mono.server` as a child process and connected to it over loopback would need no escape either, and is a legitimate third option to weigh at that point — it costs a process and buys the same crash isolation `parallel/process` already argues for.

### [_] D00007

**The bandwidth half closed at v0.4. Lag compensation is untouched. They were filed together and should not have been — one had a trigger that could fire and the other has a trigger that cannot yet.**

- ~~Priority under a bandwidth cap.~~ **Closed, and the reopen trigger fired exactly as this entry wrote it.** `SendsOverBudget` came off zero in a real cross-process run: a 2000-entity world's tick was ~137 messages against a 64-packet budget, so 73 were dropped every tick with the tail chosen by position in a vector — the precise failure this entry predicted, found because the number it named as the signal was the number that moved. What shipped is what this entry asked for: a score per entity per client supplied by the game (this module carries named components and cannot know which one is a position, the same argument `SetInterest` already makes), **a rotation that outranks the score rather than being weighted against it**, and an explicit per-client answer with the reasoning in the header — the budget belongs to a link and there is one link per connection, so a per-server cap would have to be divided before it could be enforced, and that division *is* a per-client cap. The starvation bound is `StarvationTicks + ceil(n/k)` and is asserted by a test rather than argued for. Ordering costs nothing when there is no pressure: rows go out in dirty-bit order and are only re-packed by score if that did not fit.
- Worth keeping from the closure, because it was nearly missed: **the item was found by a bug, not by a measurement anybody set out to take.** The refusals were being blamed on load and on a wall-clock deadline for four separate investigations. The entry's own advice — "`ConnectionStats` already counts the refusals; read it before concluding a component is not replicating" — was right, and nobody read it. A counter that is not looked at is not a mitigation.
- **Still open: lag compensation** — rewinding the server to what a client saw when it fired. It needs a server-side history buffer of past ticks that `replication` deliberately does not keep, and a policy for how far back it will honour, which is a game-design decision about fairness rather than an engine one. **Reopen trigger: the first hitscan weapon**, which cannot be built without it. v0.4 brought the physics and the `Part` that trigger was implicitly waiting on, so the blocker is now the game rather than the engine.

- **Lag compensation** — rewinding the server to what a client saw when it fired. It needs a server-side history buffer of past ticks that `replication` deliberately does not keep, and a policy for how far back it will honour, which is a game-design decision about fairness rather than an engine one. **Reopen trigger: the first hitscan weapon**, which cannot be built without it and which nothing before v0.4's physics can express.

### [CLOSED] D00006

**Closed in v0.9.** The stateless challenge prevents an unknown peer from
consuming a client slot, payloads use ChaCha20-Poly1305 with a monotone wire
counter, and the handshake is bound to a server identity with an Ed25519
signature. v0.10 added client identification and the server-side admission
policy. The default remains the weaker unauthenticated mode for compatibility;
`--identity-key` and `--server-key` opt into identity pinning.

### [_] D00005

- **`.github/workflows/ci.yml` is deferred by decision, not by effort.** What was never committed is the file that makes a machine other than this one run the checks, and it is deliberately not going to be: a workflow on GitHub fires jobs, and this repository does not want jobs firing.
- **Correction, made at v0.4: this entry used to say "the checks it would run are written and pass", and that was half false for as long as it was written down.** It was true of `just check`, which defaults to the `dev` preset. It was false of `just preset=ci check` — the configuration this very entry names as "what the pipeline actually enforces" — which **did not compile at all**, because `ci` makes every warning fatal and two of them were live: a `-Wmissing-field-initializers` in `core::Arguments` and a `-Wdangling-reference` at five sites in `world`'s suites. Both are now fixed and the preset passes end to end. **The lesson is the one this file already records about `just docs-check` in v0.2** — a check nobody can run stops being read, and then stops being true, and the sentence claiming it passes ages into a false one. If a recipe is named here as the standard, something has to run it.
- So the guarantee today is **local and manual**: `just check` before a push, run by a person who remembers to. That is honest rather than green — it is the same guarantee the repository has had all along, now written down instead of implied by a roadmap line that read as pending work.
- **What is actually lost is the second machine, not the checks.** Two things only a different box can prove. The tier split: `just check-server-is-headless` and `just check-cdn-is-bare` currently pass on a machine that *has* a graphics stack, so they prove the binary does not link one — but a job on a box with no graphics stack at all would prove it by building there and succeeding. And the fresh-clone case: a check that quietly depends on something in this working tree passes here forever and fails for the first person who clones.
- The split a workflow should take, if one is ever wanted, is by **what each job needs installed** rather than by what it checks — that is what makes the headless job's environment the proof. `just check`'s list is the job list, in the same order, or "it passes here" and "it passes in CI" stop meaning one thing.
- **Reopen trigger, restated at v0.7 and now the only one: the repository's owner asks for it.** This previously read "a second contributor, or a pipeline that is not GitHub's", which is a condition a *reader* could decide had been met — and a deferral whose trigger somebody else can judge is an invitation rather than a decision. It is a decision. Nothing here is to grow a workflow file, a `workflow_dispatch` stub or an action of any kind until it is asked for by name, and the reason is not the objection to jobs firing: **the engine is not finished enough to be worth gating.** A pipeline that goes red on a half-built subsystem trains everybody to ignore red.
- **What this rules out, so a later reader does not relitigate it.** Not a `workflow_dispatch`-only workflow, not a "tests and typecheck, no build" job, not a lint-only job. The narrower forms were considered at v0.7 and are the same answer: the constraint is that GitHub builds nothing and runs nothing for this repository yet. The one thing worth knowing when the answer changes is that **the C++ suites cannot run without compiling** — `just test-all` builds the engine to produce the test binaries — so a genuinely build-free job could only ever run `just typecheck`'s TypeScript half and `format-check`. The Luau half would need either `scriptcheck` compiled or a downloaded `luau-lsp` binary, which has the `analyze --definitions` mode `mono.tools/scriptcheck` reimplements.
- Removed from `ROADMAP.md` v0.2 rather than left unticked. The two items that once claimed CI existed were corrected before this was deferred, and both now read accurately: v0.2's recipe item says the recipes "exist and pass locally", and the determinism item describes `just check` as the local chain. Nothing left in the roadmap asserts a pipeline.

### [_] D00004

- ~~`Engine::core` links Crypto++ for `engine::core::Random`, and everything links `core` — so a SHA-256 implementation is in the client and the server alike.~~ **Measured per program, and removing it would save the client, the server and the cdn nothing at all.** Since this was written, `net` gained X25519, HKDF-SHA256, ChaCha20-Poly1305 and an HMAC-SHA256 admission cookie, and `assets` gained Ed25519, HMAC-SHA256 and BLAKE3. Both programs that carry Crypto++ link `net`, and `net` is what puts it there.
- **The measurement, `release` preset, per program.** client and server each carry **6,102 `CryptoPP::` symbols and 43 of the archive's 173 members**; `cdn` carries **zero of both**. Every one of those 43 is first-caused by `libengine_net.a` — `Cipher.cpp.o`, `Handshake.cpp.o`, `Cookie.cpp.o` — and **not one by `core`**. Linked alone, `core::Random` pulls **36** members, and those 36 are a strict subset of the 43; net's extra seven are the ChaCha, Poly1305 and curve25519 objects. Confirmed a second way, by relinking both programs against a `core` whose `Random` is a plain integer mixer: **43 members and 6,102 symbols, unchanged**, with 1.2 KB of text and 91 KB of debug info the only difference.
- **`cdn` is zero today for a reason that is not this entry's, and it will stop being zero for a reason that is not this entry's either.** `mono.cdn/app/main.cpp` is a 59-line stub that mounts a `ContentRoot`; it never constructs an `Origin`, so one of `libcdn_lib.a`'s six members is linked and neither `assets` nor `blake3` contributes anything. Force the whole of `cdn_lib` in — what wiring `Origin` into `main` will do — and it pulls **36 members and 5,647 symbols, first-caused by `libengine_assets.a(Grant.cpp.o)`**, because `cdn::Gate` opens grants and a grant is an HMAC. `core::Random` is not pulled into `cdn` in either case; nothing under `mono.cdn/` calls it.
- **The 9,479 figure this entry carried does not reproduce and has not for some time.** `core::Random` alone is **5,647** symbols now and the shipped client and server are **6,102**. The `36 of 173` half reproduces exactly. Recorded rather than quietly corrected, because a number drifting inside a deferred entry is the failure D00005 was reopened for.
- **What the dependency is actually worth, for the program that does not exist:** a `main` calling only `Random::Float` is **1.55 MB of text and 5,647 symbols** against **1.6 KB and none** with a plain mixer. Enormous in isolation; zero in everything that ships. Any future program that links `core` and neither `net` nor `assets` pays the whole 36.
- **So the v0.6 question survives but it is a smaller one, and it is no longer about size.** "Does anything still need `Random` when the demo dies" is now a question about one file and about `THIRD_PARTY_NOTICES.md`, not about what is in the binary. The cheaper option stands and is now the *only* argument for it: keep the interface, put a small specified integer mixer behind it. The interface was designed for that swap.
- **What `Random` is for is unchanged and is why this is not simply deleted.** A value identical on every machine, which `std::mt19937` plus `std::uniform_real_distribution` cannot promise. That determinism is also exactly why it must never produce a cryptographic key, and why the answer is never "use a system source".
- **The old trigger fired at v0.6, and the question it was holding is answered.** The C++ demo died — `BuildDemoWorld` is deleted, `Demo.hpp`/`Demo.cpp` are gone — so "does anything still need `Random` when the demo dies" can finally be asked. **It does.** `Random.new(seed)` is bound into both script VMs, drawn as a counter over `core::Random::Float` so a stream is indexed rather than stateful. That is the first consumer that exists because somebody wanted the numbers rather than because a demo needed some, and it is *userland* — which settles the direction: the interface stays. It settles nothing about what is behind it, so the small specified integer mixer is still the cheaper option and still costs the shipped programs nothing either way.
- **The narrowed trigger fired too, and the measurement says the trigger was phrased wrong.** `mono.tools/bindings` arrived at v0.6 linking `core`, `ecs`, `scene` and `script` — and `script` reaches `world`, `physics`, `spatial` and `parallel` and **neither `net` nor `assets`**. That is precisely the shape this bullet named. It pays **zero**: 16,593 symbols in the binary, **0 `CryptoPP::`, 0 `core::Random`**, against the client's 10,307 and 3 in the same preset. Nothing was pulled because static archives link per object — the tool's `main` calls `ScriptClass()` and never reaches the datatype bindings, so `LuauDatatypes.cpp.o` is not in the binary and `Random.cpp.o` is not pulled behind it.
- **So the trigger is a link-line property and the cost is a call-graph property, and this entry conflated them for two versions.** Restated: **a program that *calls* `Random` while linking neither `net` nor `assets` pays the whole 36.** Linking `core` is not enough and never was — every measurement in this entry is consistent with that and none of them said it.
- **Reopen trigger, re-phrased: a program that calls `core::Random` and links neither `net` nor `assets`.** There is still not one. The nearest miss is `mono.tools/bindings`, which has the link line and not the call.

### [DELETED] D00003

- **Closed at v0.2 by the storage rewrite.** Every iteration path now goes through one cached `QueryPlan` per term list, topped up rather than rebuilt as tables appear, so nothing builds a query per call. The flecs-shaped problem below no longer exists — there is no `flecs::query` to be typed or untyped about.
- `Each` and `EachParallel` still build a query per call. `CountMatching` now caches its query and a typed cache for the iteration paths is the same idea, but it needs a per-store map of typed `flecs::query<Ts...>` rather than the one untyped kind, so it is a bigger change than the count was.
- Not urgent and not measured. Both iteration paths cost what they always cost — this is a saving, not a regression to fix — and the number to have before doing it is what query construction is as a fraction of a tick at a realistic entity count.
- Likely moot at v0.2, when `Column`/`ComponentSet` replace flecs as the storage and the query object stops being flecs's to build.
- Resources are per-world with no ordering guarantee against each other, which is fine while they are written by one system each. When two systems write one resource, that ordering is a phase question, not a resource question.

### [CLOSED] D00002

**Closed at v0.11 by the executor landing.** `Renderer::Render` runs
`graph::Execute` over a compiled `graph::RenderGraph`; the six pass bodies are
handlers a `PassTable` finds by kind. `render::PassOrder` reads its names out of
the graph and the hand-written `Pass` enum beside it is deleted, so the frame is
described once. `Renderer::SetPipeline` takes an authored graph, which is the
line this entry's last bullet was really about — editing a pipeline changes a
frame now and not only a document.

**What did *not* close with it, recorded so nobody looks for it here:** the graph
still describes what runs and does not yet *allocate* what it runs over —
`Impl::TextureFor` maps the standard frame's resource names onto textures the
renderer owns, and a name it does not know gets an invalid answer rather than a
new target. Transient allocation, aliasing and pass culling — RDG's derived half,
`PIPELINE_NODES.md` §1.1 — are all still absent. That is the next entry somebody
should file rather than a bullet here.

**Three of the four bullets below were already false by the end of v0.6 and were corrected in place rather than left to be read.**

- The graph renderer of `RENDER_PIPELINE.md`. ~~The current `render` module is stage 0 of its twelve — one instanced opaque pass and one overlay pass, standing in for stage 1's skeleton.~~ **Five passes as of v0.6** — shadow, surface, opaque, transparent, overlay — over a frustum-culled draw list. Still stage 1's skeleton rather than the graph, and the thing that makes it a skeleton is unchanged: **the order is a function body**. More passes is not more architecture.
- ~~Its stage 2 needs `ecs::ChangeChannel` for per-node cache invalidation, and its §4.2 needs `ecs::Column`/`ComponentSet` to store nodes as rows. Both wait for v0.2, which is D00001.~~ **Both closed** — the storage rewrite at v0.2, chunking at v0.4. **Nothing in `ecs` blocks the graph any more**, which means this entry has been waiting on nobody but itself since v0.4.
- ~~The graph runtime itself is `mono.engine/graph/` at L9 and does not exist.~~ **It exists as of v0.6, at exactly the tier and layer this line named** — and what landed there is the *description*, not the runtime. `Frustum`, `Cull`, `Shadow`, and a `Pipeline` of `Stage` records that `Validate` checks for a stage reading what nothing wrote. What is absent is the whole of §4.2 and §12: no nodes, no handles, no capabilities, no cache, no compiler, no executor. `Renderer` calls `graph::Cull` and `graph::FitDirectionalLight` as **functions**; it does not run a graph. The directory existing is the least interesting half of this bullet closing.
- ~~`render` must not grow a hand-rolled pass list before it does.~~ **It grew one anyway, and that is an overrun rather than a revision.** v0.6's roadmap asked for shadows and a render-to-texture surface, the graph runtime executes nothing, so the passes had nowhere else to live. The part that is not defensible is that the two descriptions of a frame are now unchecked against each other — filed as **D00016**, which is where the argument about what to do belongs.
- §12.3's remaining additions to F5 — per-stage cache hit/miss, and the undemanded capability of a dead node — **unchanged, and now for the right reason**: they need the cache and the executor, not the directory. The tick rate on F3 and the tick/render split of §14 are done.
- **Reopen trigger, stated for the first time: v0.8's extended pipeline.** "Handle multiple worlds in parallel" is the first roadmap line that cannot be met by making the function longer — per-world pass sharing is exactly the decision `Stage::PerView` was written to record and that nothing yet reads, and HDR plus a G-buffer change what every later stage reads, which is the change a hand-rolled list makes by editing five call sites and a graph makes by editing a list.

### [_] D00001

- ~~`--script PATH` is accepted and warns.~~ **Closed at v0.5**, and it was the oldest thing in this entry — accepted and ignored since v0.1. Two VMs are vendored and linked, the file extension picks between them, and the flag loads a scene: `--script` on the client, `--game` on the server (ignored since v0.3), `--scene` on the unified harness. `mono.engine/examples/Rings.luau` and `Rings.js` build the same world through the same bindings, and the unified harness reads 512 entities on the server and 512 on the client from either.
- ~~`core/types` has `Vector3`, `Color3` and `CFrame` only.~~ **Closed at v0.4.** `AABB`, `Ray` and `RayHit` landed with the consumers this bullet was waiting for — `spatial`'s queries and `physics`'s narrow phase. Nothing else was added, deliberately: `Vector2` was considered and refused because §3.4 gates it on "the overlay or editor needs it" and neither does, and the culling operations an `AABB` invites (`Inverted`, `Grown`, `Contains(AABB)`) have no caller until v0.6's frustum cull.
- ~~`Column`, `ComponentSet`, `SparseSet` and `ChangeChannel` are not in `ecs` yet.~~ **Closed at v0.2** by the storage rewrite, and reopened and closed again at v0.4 by chunking. Recorded here rather than deleted because this bullet is why the entry was still `[_]` after the other half of it had shipped.
- macOS builds compile SPIR-V but not MSL; the cross-compile step is wired in CMake and untested. Linux/Vulkan is the verified path. **Still open, and still the least examined line in this file** — it is the only item here with no trigger, because nobody has a Mac to trip it.
- **Correction at v0.6, to the second bullet's reasoning rather than to its verdict.** "`Vector2` was considered and refused because §3.4 gates it on 'the overlay or editor needs it' and neither does" — **`Vector2` shipped at v0.6, and for neither of those reasons.** `UDim2` and `Rect` are made of it, and both arrived with the datatype vocabulary a script surface owes an author. The gate was right and the list of things that could open it was short by one, which is the useful half: a gate phrased as "who needs it" only names the consumers somebody had thought of. The other half of that sentence closed exactly as written — the `AABB` operations got their caller in `graph::Cull`, and `Frustum::Intersects` is the positive-vertex test that wanted an `AABB` rather than eight points.

**Three of four bullets are now closed and the entry stays `[_]` for macOS alone.** The paragraph that used to stand here said "two of four", which was true when it was written at v0.4 and stopped being true at v0.5 when `--script` closed — recorded rather than silently re-counted, for the reason D00004's drifting figure is recorded. `v02v03v04.md` predicted the v0.4 edit and said it belonged "with the next pass over `docs/DEFERRED.md`, not here".
