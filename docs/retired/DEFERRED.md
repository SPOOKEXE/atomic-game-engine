
# DEFERRED

### [x] D00108

**Closed at v0.13 by `studio::EditStream`.** What follows is the entry as it
was written, kept because the reasoning is what the answer was built against.

The identity turned out to be an instance path rather than a new field in the
document format: two logs both issue `1` for their first instance, so an
`EditId` cannot cross, and a path is the one name two editors already share.
Ordering comes from the host relaying, which is also what keeps paths resolving
to the same instance everywhere. What the entry says about locking still holds
and is why there is none.

**Team create finds people and cannot yet let them edit together.**

`studio::TeamCreate` is the session layer and it is complete: an editor
announces itself at `Purpose::Studio`, sees the others on the subnet or through
a rendezvous point, and hands over a session id and a key to invite somebody
with. What it does not have is anywhere for two editors to put a change.

**Why the obvious answer is the wrong one.** `replication::Authority` already
orders changes to a world and streams them to clients, and pointing two editors
at it looks like a morning's work. It is not the same problem. An authority has
one writer and many readers; a shared document has many writers, and the
question it has to answer — what happens when two people move the same part in
the same beat — has no answer in a model built around a server that is right by
definition. Bolting one on would produce an editor where the last packet wins
and somebody's work disappears without a message.

**Why it is not fixed by locking.** Per-instance locks are the cheap version and
they fail in the ordinary case rather than the rare one: two people laying out
one model touch the same parts constantly, and a lock that has to be waited for
turns collaboration into taking turns.

**What closing it takes.** A change model with a total order that neither editor
owns, an undo stack per person that survives somebody else's edit landing in the
middle of it, and a policy for the conflicts the order does not resolve. That is
a design, not a patch, and it should be written down before any of it is typed.

Until then: the panel says "sessions only" in the window rather than in a
comment, because a person looking at a list of editors they cannot collaborate
with should be told why by the thing they are looking at.

### [CLOSED] D00105

**A plugin can change the world and cannot add a button, and the missing piece
is a channel rather than a function.**

**Closed at v0.12 by building the channel.** `script::HostSurface` is the seam —
one virtual taking a name and a `HostValue` list — and `script::HostValue` is a
value tree rather than `ScriptValue` widened, for the reason this entry
predicted: an instance handle means something inside one process and nothing on
a bus, so the two types stay apart. A Luau function passed as an argument
becomes a `HostCallback`, which is what makes a button's handler possible, and
`Runtime::Invoke` is the other direction. `mono.studio/src/PluginSurface.cpp` is
the editor's implementation and `engine.script.host` covers the crossing.

Two things the entry got right and one it did not. The value tree and the
callback were both needed, as predicted. What it called "the honest options" —
a polled queue or a registry-ref dispatch — turned out to be one option: the ref
lives in the module behind `Invoke`, so the host holds an id and polls nothing.

The original text follows.

`studio::PluginHost` runs a plugin as an ordinary `script::Runtime` against the
world an author is editing, so everything a game script can reach it can reach —
`Instance`, `workspace`, and since v0.12 `World`, the ECS underneath. The
selection needed no new surface at all: it is published as `studio.Selected`, a
described component, so a plugin queries for it and writes it back.

What that model cannot express is anything about the *editor* rather than about
the world: a toolbar button, a menu item, a docked panel, running a registered
command.

- **The obstacle is `script/AGENTS.md`'s first rule.** No `lua_State` appears in
  a public header, so the studio cannot install a global of its own into a
  plugin's VM the way `LuauEcs.cpp` installs `World` — that file is inside the
  module and `mono.studio` is not.
- **The shape that fits is a host-call seam, and the codec is already the hard
  half.** `script::ScriptValue` is the language-neutral tree both runtimes
  marshal through, with a deterministic encoding and a sorted map order; a
  `HostSurface` interface taking a name and a `ScriptValue` and answering one
  would give a host an arbitrary API without either side naming the other's
  types. What it costs is making `ScriptValue` public, which is a real widening
  of `script`'s surface and should be decided rather than slipped in.
- **A button also needs a callback going the other way**, which is the part the
  value tree does not answer: a handler lives in the plugin's VM and the press
  happens in the editor's frame. The honest options are a polled queue — the
  plugin asks "was I clicked" on its heartbeat — or a registry-ref dispatch
  inside the module, and the first is buildable today on top of the seam while
  the second is not.
- **Until then the absence is stated rather than discovered.**
  `studio/Plugins.hpp` says there is no toolbar API and why, which is the thing
  that keeps somebody from looking for one.

**Reopen trigger: the first plugin that wants to be invoked rather than to
run every frame.** A tool that aligns a selection is one — it should happen when
somebody asks, not sixty times a second.

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

### [CLOSED] D00006

**Closed in v0.9.** The stateless challenge prevents an unknown peer from
consuming a client slot, payloads use ChaCha20-Poly1305 with a monotone wire
counter, and the handshake is bound to a server identity with an Ed25519
signature. v0.10 added client identification and the server-side admission
policy. The default remains the weaker unauthenticated mode for compatibility;
`--identity-key` and `--server-key` opt into identity pinning.

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
