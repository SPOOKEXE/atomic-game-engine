
# DEFERRED

Retired deferred items are in atomic-game-engine/docs/retired/DEFERRED.md.

## Format

Each section has the header `[_] D00000`.

The numerical is a counter that increments every item.

Insert the NEWEST items at the front **of `## Deferred Items`**, so older ones
are towards the back. The section is named because "the front" on its own is the
front of this *format example*, which is where D00025 through D00032 spent v0.9
and v0.10 — eight real entries, the newest of them, rendering as a code sample.
Nothing checks this: a fenced block is valid Markdown whatever is inside it.

**It happened again, and D00102 spent v0.11 to v0.13 in there.** Sixty lines of a
live decision, invisible as a sample. The example below is one dummy entry and
nothing else; anything with real content in it is in the wrong place.

**An item that is closed or no longer exists is removed, not marked.** It used to
be flagged `[DELETED]` and left in place, which grew a register where most of the
entries were about code that is not there — so the open ones, which are the point
of the file, were the minority. What a closed item decided belongs in
`ROADMAP.md` and in the commit that closed it, both of which survive; retired
entries are in `docs/retired/DEFERRED.md`.

```
### [_] D00101

- item 1
- item 2
- item 3
```

## Deferred Items

### [_] D00109

**Filed as `D00108` and renumbered, because that number was already taken.**
`docs/retired/DEFERRED.md` carries a `D00108` closed at v0.13 by
`studio::EditStream` — team create's shared-document model — and the counter is
supposed to increment past retired entries rather than reuse them. The number
was picked by reading the front of the live file, which is exactly the half of
the register that does not contain the used numbers. `ROADMAP.md`'s ownership
entry cited the wrong one for a version and now cites this.

**`replication::Prediction` still has no caller, and ownership was not the one
it was waiting for.** The plan that produced v0.13's ownership work assumed the
two would meet — build the upward state path, and prediction gets its consumer
on the way past. They do not meet, and the reason is worth writing down because
it looks like they should.

`Prediction.hpp` says what it is for in its first line: "the local player and
nothing else. Everything else is interpolated authoritative state." An entity a
client *owns* is neither of those. It is simulated by that client and never
corrected — there is no authoritative state arriving for it to reconcile
against, because the client is the authority for it. So ownership does not give
prediction a caller; it describes a third category that prediction deliberately
does not cover, which is exactly Roblox's arrangement.

**What is actually unwired is larger than prediction**, and finding it is the
useful part of this entry:

- **Nothing calls `Connector::Submit`.** No client in this repository has ever
  sent an input. The only reference is `replication/tests/Admission.cpp`
  asserting that submitting *before admission* is refused.
- **So `Prediction` never holds anything.** `Connector::Poll` calls
  `Reconcile(Replica_.Applied())` every tick, faithfully, against an empty
  buffer.
- **And the server's whole input path has no sender.** `Server::ApplyInputs`
  decodes an `examples::Shot`, rewinds the client's view with
  `Rewind::TickSeenBy`, hit-tests against the recorded history and recolours
  what was struck — a complete, careful, server-authoritative feature whose
  `examples::EncodeShot` has no caller anywhere in the tree.

That is `D00039`'s shape again: two halves, each finished, connected to nothing,
and neither of them looking unfinished from its own side.

**Replay is the caller's job and that is correct rather than missing.**
`Prediction::Pending()` hands back what to replay and `Connector` does not
replay it, because replaying an input means knowing what an input *means* — the
same line every other opaque payload in this module sits on. A caller has to
apply them. What that means is that the first consumer writes the whole third
step of the loop, and a consumer that forgets it gets the rubber-band the header
warns about with nothing reporting why.

**The natural consumer is the character controller**, which is `ROADMAP.md`
v0.14's "character controller + humanoid + character states". A local player
walking is precisely the case prediction is for, and a shot is precisely the
case `ApplyInputs` is for. Building either one before there is a character to
control would be inventing a client to exercise a server, which is how the
harness in D00018 came to disagree with the thing it was checking.

**Reopen trigger: the first client that controls something.** Whichever lands
first — a character or the shooting demo's other half — brings the input
encoding, and prediction's caller comes with it.

### [_] D00102

**The dependency decision is settled and done. What is left is that the panel it
was blocking no longer exists.**

- **The split shipped at v0.13 and stands on its own.** `Engine::bakegraph` is
  the node vocabulary and the document format at `shared`, linking `Engine::core`
  and nothing else; `bake` keeps every importer and the evaluator and depends on
  it. So `Engine::game` *can* now carry a pipeline in a place file without
  pulling the PNG, JPEG, GIF, BMP, OBJ, glTF and PMX readers into `server`, which
  is the whole property this entry was about. `expected_graph.json` carries the
  new edge, and a `bakegraph` suite proves the format is testable with no
  importer linked.
- **It was cheaper than this entry assumed and the reason is worth recording.**
  `GraphDocument` needed only `Graph.hpp` and `core`; `Graph.hpp` needed
  `assets`, which is what a baked mesh *is* rather than a decoder. Only
  `Graph.cpp` — the evaluator — reaches an importer at all, and only six files in
  the repository included either header. `Build` is the one function needing both
  halves and stayed in `bake`. `IsBareNode` had to become public, because a
  closed list of parameterless kinds is exactly the thing that must not be
  copied.
- **Correction, and it is the same one D00038 needed.** The heading used to read
  "the Assets Pipeline panel draws an empty document". **There is no panel.**
  `Editor::DrawAssetsPipeline` is a `TODO(render-pipeline)` marker, as are the
  `WritePipelines`/`ReadPipelines` pair in `game/src/Game.cpp` and
  `client::InstallWorldPipelines` — all of it went out with the render-pipeline
  revert. Checked by grepping for the symbols rather than by remembering.
- **So the `<AssetPipelines>` block was deliberately not written.** A save-format
  section with no producer and no consumer is a feature that looks present and is
  not, which is the trade `D00017` and `D00008` both come down against — and the
  format break is cheap to take later precisely because the module split is the
  part that was expensive. When a panel exists, the block is a small change
  against a dependency graph that already permits it.
- **What is left is not this entry's any more.** It is the extended rendering
  pipeline in `ROADMAP.md`, behind a prototype project, and the asset half comes
  back with it.

**What it looked like before:**

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

### [_] D00106

**JavaScript and TypeScript have no breakpoints, and the obstacle is the VM
rather than this engine.**

The Luau half of `script::Debugger` works through `lua_callbacks(L)->debugstep`,
which Luau calls after every instruction while single-step mode is on — so a
breakpoint is a line comparison in a hook that only exists while one is armed.
QuickJS as vendored offers no equivalent:

- **`JS_SetInterruptHandler` is the whole of it**, and its callback takes a
  runtime and an opaque pointer. No line, no file, no frame — it exists to let a
  host abort a long-running script, which is what `RuntimeLimits::StepBudget`
  already uses it for.
- **There is no debugger API at all.** `js_debugger` appears zero times in
  `mono.vendor/quickjs`. Some quickjs forks carry one — the Ladybird and
  quickjs-debugger trees both add `js_debugger_*` with breakpoint and stack
  support — and this vendored quickjs-ng does not.
- **`JS_GetScriptOrModuleName` is not a substitute.** It answers which module a
  frame belongs to and nothing about where in it, so it cannot tell a line from
  the next one.

**Three ways out, and none is a small change.**

- **A vendor bump or a fork.** `mono.vendor/AGENTS.md` says a patch goes
  upstream or into a fork whose remote is recorded in `.gitmodules`, never into
  a file in this tree — so this is a submodule decision with a maintenance cost
  against a moving target, which is the same trade `D00019` records for Luau.
- **Source instrumentation.** Rewriting a plugin's JavaScript to call a hook at
  each statement would work in any VM and changes what runs, which makes every
  line number in a stack trace a translation and every measurement a lie about
  the program the author wrote.
- **A second VM for tooling.** Out of proportion to the feature.

**What exists in the meantime is stated rather than implied, and asking for the
missing thing is refused rather than ignored.** `BreakpointService` is installed
by the Luau binding alone, so a JavaScript plugin gets `undefined` from
`game.GetService("BreakpointService")` rather than an object that refuses
everything — and `Debugger::Add` answers `false` for a `.js`, `.mjs`, `.cjs`,
`.ts` or `.tsx` chunk whoever asked, naming this entry in the reason.

That refusal is the part worth keeping if the rest of this is ever built
differently. A breakpoint that sits in a list looking armed and never fires
reads as the debugger being broken rather than as the language not being
supported, and a person cannot tell those apart from the outside.

**The instrumentation option was considered and set aside**, and is recorded
here so it is not rediscovered as a new idea. Prefixing each statement line with
a hook call — without adding newlines, so every line number survives — would
give line breakpoints in any VM and needs no vendored change. What it costs is a
JavaScript lexer good enough to know which line boundaries are safe (not inside
a template literal, a string, a comment, or a regex, where regex-versus-division
is the hard case), and it changes what runs. It is the cheapest path that
touches no submodule, and it is more work than it first looks.

**And TypeScript needs a second thing regardless of the first.** The studio's
`tsc` invocation emits no `--sourceMap`, so the engine runs transpiled
JavaScript whose lines do not correspond to the `.ts`. Even a perfect VM
debugger would put breakpoints in generated code; the mapper is cheap and would
be conspicuous by its absence.

**Reopen trigger: a vendored QuickJS with a debugger API**, or the first
TypeScript plugin big enough that its author asks for one.

### [PARTIAL] D00104

**Rojo's file table: seven of the nine are built, and the two left are format
readers rather than mappings.**

**`.toml` closed at v0.13 and it went exactly as this entry predicted.** The
entry called it "the cheapest by a distance and the only one whose cost is a
submodule rather than a format reader", and that is what it cost: toml++
vendored — MIT, header-only, no dependencies — a conversion into `json`, and
`LuauModuleFor` reused unchanged, because Rojo maps `*.toml` and `*.json` to the
same `ModuleScript`. Nothing downstream of the parse is new.

The one thing the entry did not predict is what a TOML date becomes. There is no
JSON type and no Luau one, so it arrives as its TOML spelling in a string —
dropping it would make a key silently vanish and a table of parts would invent an
interface this engine then owes an author.

What follows is the entry as it was, less the `.toml` row.

`studio::SyncRojoProject` builds `rojo.space/docs/v7/sync-details` except for
three rows. What it builds now, beyond the scripts it always did:

- **`.meta.json` and `init.meta.json`** — properties patched onto whatever the
  file of that stem produced, including a script. `game::WriteProperty` does the
  write and `ReadPropertyJson` accepts both spellings Rojo has used, the bare
  value and the named-part object. **A class change is the one part not
  honoured**: a class is the archetype an entity was created in, and `Store`
  offers no way to move a live row between class trees, so `className` in an
  `init.meta.json` is reported as a property the instance does not have.
- **`.model.json`** — the class, its properties and its children, sharing the
  patch above because Rojo documents one property syntax for both.
- **`.json`** — a `ModuleScript` whose source is generated rather than parsed at
  run time. Every key is bracketed, because a JSON key may be anything and
  `{ foo-bar = 1 }` is a syntax error; numbers go through
  `game::FormatNumber`, because `std::to_string` is `%f` and writes 1e-8 as
  "0.000000".
- **`.txt` and `.csv`** — a `StringValue` and a `LocalizationTable`, both over
  the new `scene::TextContent` and both under a `ValueBase` so `:IsA` answers
  the question a script would ask. **A `LocalizationTable` holds its CSV and
  resolves nothing**: translation lookup is a service with a locale and a
  fallback chain, and none of that is a file mapping.
- **`.project.json` under a `$path`** — followed, with a cycle check that is the
  part which had to exist before the recursion did.

**What is left, and why each is a vendor decision before it is a feature:**

- **`.rbxmx` needs an XML parser and `mono.vendor` has none.** The engine
  vendors JSON and nothing else that reads a markup tree. Roblox's XML model
  format is also not simply "XML" — it is `Item`/`Properties` elements with typed
  children and a referent table, so the parser is the smaller half.
- ~~**`.toml` needs a TOML parser, and `mono.vendor` has none.** This one is
  otherwise free: the JSON path already emits the Luau, so a TOML document
  parsed into the same tree would reuse `LuauModuleFor` unchanged.~~ **Done at
  v0.13**, and "otherwise free" was accurate.
- **`.rbxm` is Roblox's binary model** — LZ4-framed chunks, interned strings and
  a referent table. That is a format reader and it belongs beside the other model
  decoders in `bake` rather than in an editor, which is also where `.rbxmx`
  would go once something could parse it.

Each of the three is reported by name and by what Rojo says it is, so a gap
reads as a gap rather than as an unrecognised file — `studio.rojosync` asserts
both halves, that the three are named and that the six are silent.

**Reopen trigger: a project that carries one of the two.** Both are format
readers now that the cheap one is gone, and `.rbxmx` needs an XML parser before
it needs anything else.

### [_] D00103

**Per-pass GPU time is not measured. The Vulkan path that measured it was
removed with the rest of the render pipeline.**

`render/src/VulkanTimestamps.{hpp,cpp}` existed and worked: it reached into SDL's
Vulkan backend, created a query pool, marked each pass and read the results back
a frame later. It went out with the revert because it was wired into the pass
executor that no longer exists.

**Everything it established is still true and worth reusing.**

- SDL 3.2.31 — what `mono.vendor/sdl` is pinned to — has no timestamp query, no
  query pool and no `SDL_GPUQuery`, exposes no native handle, and its Vulkan
  backend has zero references to `vkCmdWriteTimestamp` or `VkQueryPool`. There is
  no supported call to make.
- `SDL_Vulkan_GetVkGetInstanceProcAddr` **is** public SDL, so the entry points can
  be loaded rather than linked — no Vulkan SDK and no linked Vulkan library. The
  Khronos headers come from SDL's own copy.
- What is not public is the `VkDevice` and the `VkCommandBuffer`. Those came from
  mirroring the first fields of `VulkanCommandBuffer` and `VulkanRenderer` and
  casting the opaque pointers the renderer already holds — pinned to one SDL
  version, guarded by a plausibility check that gave up rather than reading a
  wild pointer, and gated on `SDL_GetGPUDeviceDriver` being `vulkan` before any
  cast, because a D3D12 command buffer read through the Vulkan mirror is a crash
  rather than a wrong number.
- Marks belong at the **bottom of pipe**, and the read must not block: waiting on
  a timestamp serialises the CPU against the GPU in order to report how fast the
  GPU is.
- **Never fill the field with CPU time.** A submit-side number labelled as a
  pass's cost is worse than a blank — somebody reads "0.4 ms" for the shadow
  pass, believes the GPU said it, and optimises the wrong thing.

**Trigger:** a new pass executor to hang the marks off. The file is recoverable
from git history on `v0.11` or the local branch `renderer-before-revert`.

### [_] D00046

**Per-pass GPU timestamps, which `SDL_GPU` cannot express.**

**Correction at v0.13, and it is the largest one in this file: every symbol and
every document this entry named has been deleted.** Checked by grepping for each
one rather than by remembering, the way `D00038` and `D00103` were.

- **`ProfilePass::Elapsed` does not exist.** The only occurrence of `ProfilePass`
  anywhere in the tree is the sentence below that names it. So the field the
  timestamps "land in" is not there to land in.
- **`PIPELINE_NODES.md` does not exist**, so "stage 7's remaining half" points at
  nothing a reader can open. The staging it refers to is `ROADMAP.md`'s extended
  rendering pipeline now.
- **`graph::Execute` is gone, and with it the node to hang a mark off.** The
  bullet below saying this entry is no longer blocked on the executor was true
  when it was written and stopped being true at the render-pipeline revert. It is
  blocked on the executor again.
- **`FrameRunner::Run` and its `SDL_PushGPUDebugGroup` calls are gone.** There are
  no GPU debug groups anywhere in this repository, so the "readable-capture half
  that could be built, was" describes work that is no longer in the tree.
- **`FrameResult::UploadedBytes` and `Uploads` are gone.** `FrameResult` carries
  `Presented`, `DrawCalls`, `Triangles`, `SurfaceInstances`, `SurfacePasses`,
  `RibbonVertices`, `Particles`, `Culled` and `Passes`, and none of them counts a
  copy into GPU memory. The one surviving `UploadedBytes` is `TextureTable`'s own
  private counter, which is a different number about a different thing.

**This entry and `D00103` are now one item seen from two sides**, and the split
is worth keeping only because the two halves are blocked on different things.
This one is the *portable* question — SDL exposes no way to write a timestamp,
so no amount of work here moves it. `D00103` is the *Vulkan* answer that existed,
worked, and was reverted, and is recoverable from git. **Whoever builds the pass
executor should read both and close both**; building one without the other
produces a number on Vulkan and a blank everywhere else with nothing saying why.

What it said before, with the deleted names left in place so the correction above
is checkable:

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

**Reopen trigger, which this entry never had: a pass executor to hang a mark off,
same as `D00103`'s** — or an SDL release with a timestamp query, which would make
this the portable answer and `D00103` a fallback rather than the only path.
Written down because an entry with no trigger is one nobody can decide is due,
and this one has been carried since v0.4 on an argument alone.

### [_] D00038

**`Renderer::Render` draws one view, and the studio round-robins its panels
through it — so two viewports each update at half the rate.**

- **Correction at v0.13, and it changes what this entry is blocked on.** The
  first bullet used to read "v0.11 replaced the twelve-parameter `Render` with
  `std::span<const View>` … the seam exists and is unexercised". **It does not
  exist.** `render::View` and the span went out with the render-pipeline revert,
  exactly as `D00103` records for `VulkanTimestamps`, and `Render` is a
  twelve-parameter call taking one `cameraFrame`, one `camera` and one
  `targetSlot`. Checked by grepping for the type rather than by remembering.
  Recorded rather than quietly rewritten, for the reason `D00004`'s drifting
  figure is: a reader following this entry would have gone looking for a span to
  loop over and found nothing.
- **So the cost moved from "convert a loop" to "re-establish the seam".**
  `Renderer::Render` owns the swapchain acquisition and the present, so one call
  is one frame and the round-robin is not a choice the studio is making — it is
  the only shape the API allows. Closing this needs "draw a view into a target"
  separated from "present the frame", which is the reverted pipeline's shape and
  is `ROADMAP.md`'s extended rendering pipeline, behind a prototype project.
- **What v0.13 did fix is the half that was a bug rather than a limitation.**
  Each viewport owns its surface textures — `Impl::SurfaceBank` per slot — so a
  panel showing a mirror no longer composites another panel's reflection, and
  the aim-overwrites-aim failure below is contained to the frame rather than
  crossing panels. The rate is still halved; the picture is no longer wrong.
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

### [_] D00031

**The editor does not know `Enum.Material`, because luau-lsp reads the definitions file and nothing registers the prefix for it.**

- `just typecheck` accepts `local m: Enum.Material` — `scriptcheck` registers `importedTypeBindings["Enum"]` itself. luau-lsp loads the same definitions file and does not, so an author writing the dotted form sees a red squiggle on a line that builds and passes.
- **The flat spelling still resolves everywhere**, so this is a cosmetic gap with a workaround rather than a broken surface: `Enum_Material` is what the declaration file declares and what the editor understands.
- **Three ways to close it, and none is obviously right yet.** Teach luau-lsp the prefix, which means a patch to a vendored tool and `mono.vendor/AGENTS.md` says a patch goes upstream or into a fork. Switch `luau-lsp.platform.type` to `roblox`, which makes the editor typecheck against Roblox's class tree rather than this engine's — worse than the squiggle. Or generate an `Enum.luau` module and have scripts `require` it, which works in both and costs a line at the top of every file.
- **Re-examined at v0.13 and deliberately left as it is, because all three ways out cost more than the problem.** Patching luau-lsp is a fork to maintain against a moving target for a cosmetic squiggle. Switching to the `roblox` platform typechecks against the wrong class tree. And generating an `Enum.luau` to require has a hazard the entry did not name: `local Enum = require(...)` **shadows the runtime `Enum` global**, so every value use — `Enum.Material.Plastic` — would then resolve through the module rather than the engine, and the fix for the annotation would break the thing the annotation is about. `scriptcheck` reports 35 enums reachable as `Enum.<Name>` in a type position, so the build is not what is wrong; one editor is.
- **Reopen trigger: somebody writing enum annotations often enough to be annoyed.** The engine's own scripts have three.

### [_] D00030

**A mutable property on a script *global* reads once and never again, because `luaL_sandbox` enables Luau's `safeenv`.**

- `UserInputService.MouseBehavior` is the first property in the engine that lives on a global rather than on an instance, and it does not work when it is read that way. `local UIS = game:GetService("UserInputService")` works; `UserInputService.MouseBehavior` returns whatever it was the first time any closure asked.
- **The mechanism, because it is not obvious and cost an hour.** `luaL_sandbox` freezes the global table and turns on `safeenv`, which lets the compiler emit `GETIMPORT` for a constant global followed by constant fields. `GETIMPORT` resolves the chain once per closure and caches the **value**. It does this whether the intermediate is a table or a userdata, so making the service a userdata does not fix it — that was tried, and the observation that settled it is that `__index` fires for the first read of a field and never for the second, with no raw key on the object to explain it.
- **The userdata is still right and is kept.** It is what makes every read *through a local* go to `__index`; a plain table would have been cached there too.
- **In practice it does not bite, which is why this is filed rather than fixed.** Every Roblox script begins `local UIS = game:GetService("UserInputService")`, and `game:GetService` is a method call that cannot be an import. The engine's own declaration files describe the property, the test uses the idiomatic form, and the comment in `InputServices.cpp` says so.
- **What closing it would take.** Either not sandboxing — which is not on the table, `LuauRuntime` freezes the globals so one script cannot change the language the next one runs in — or making the service a *function call* rather than a global, which changes the surface away from Roblox's. Neither is worth it for a property nobody reaches the broken way.
- **Reopen trigger: a second mutable global property.** One is an oddity with a workaround everybody already uses; two is a pattern, and at that point the surface should stop being globals.

### [_] D00019

**The engine's Luau is held at the revision the editor tool can consume so that
the editor and the type check agree. The current engine revision is Luau 0.732.**

- `mono.vendor/luau` is pinned to commit `f8ca77ac` (Luau **0.732**), and `mono.vendor/luau-lsp/luau` must be pinned to the same commit when that optional submodule is checked out. `mono.tools/scriptcheck` links the first and gates `just typecheck`; the language server in an editor uses the second. Two Luaus would mean an author reading diagnostics from a language the engine does not run, which is worse than no editor support because it looks authoritative.
- **The exact upstream ceiling belongs to luau-lsp.** Its nested Luau must remain buildable against the language-server sources. Do not bump the engine submodule alone: the sync check is the contract, and a failed `just luau-lsp` is preferable to silently giving authors diagnostics for another language revision.
- **Checked, not written down.** `just luau-lsp` compares the two `HEAD`s and refuses to build when they differ, naming both. Verified by mutation: bumping `mono.vendor/luau` alone makes the recipe fail with the two SHAs printed. Without that, the drift is invisible — the engine keeps passing every check it has, and only an editor is wrong.
- **What the choice actually costs, so a later reader can weigh it.** The engine follows the editor's compatible revision rather than independently following upstream. The trade is only defensible while the gap stays small; a long-lived gap would invert it, and the answer then is the fork below rather than a wider gap.
- **The fork is the way out and was declined at v0.7 on purpose.** Pointing luau-lsp at `mono.vendor/luau` needs sixteen mechanical call-site changes, and `mono.vendor/AGENTS.md` says a patch goes upstream or into a fork whose remote is recorded in `.gitmodules` — never into a file in this tree. That is a fork to maintain against a moving target, for a developer tool.
- **Checked at v0.13 and the trigger has not fired.** Upstream luau-lsp at `53f4238` pins Luau `f8ca77acdcb50241e3da21af663f8ef97b4b5ce4`, which is byte for byte the commit `mono.vendor/luau` is on. **There is no gap to close**: this engine is already at the editor's ceiling rather than lagging behind it, which is the state this entry describes as defensible. Worth recording because "held at the revision the editor can consume" reads as a compromise, and right now it costs nothing at all.
- **Reopen trigger: luau-lsp syncs to a later Luau revision.** Bump both submodules together, run `just luau-lsp` — which refuses if only one moved — then run `just check`.

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
- ~~**`mono.server` is deliberately not wired up.**~~ **Wired at v0.13, and the decision this bullet describes is what shaped how.** `--idle-close` turns lifetime management on and its absence is the behaviour this program had before — so the two byte-comparing recipes are unaffected *by construction* rather than by their runs happening to be shorter than five minutes. Both still pass byte-identical. None of the policy is repeated: what the server supplies is occupancy, which for it is a player standing in the world, where the studio also counts the active scene and a viewport looking at it.
- **Two things came out of the wiring that were not this entry's and are worth recording here anyway, because a second caller is what found them.** The first is that **`LastWorld` could not do its job**: the refusal is "a universe with every world suspended is a game that has stopped without saying so", and the only caller derived it from `Universe::Count()`, which that function documents as including suspended worlds. The count never drops, so N idle worlds suspend one after another, each the last only after the others had gone. `Universe::CountInState` is the fact the refusal is about and both hosts now use it. **That is this entry's own argument arriving from the other direction** — it warns about one policy written twice, and what actually happened is one policy read wrongly by its only reader, with nothing to compare it against until there were two.
- **The second is that an empty world is not always an idle one.** NPCs on a route, a shop restocking, a round counting down. So the timeout became one of three answers — `world::IdleSleep` — with `Never` for a 24/7 world spelled as an enum member rather than as a very large number, and a ten-minute ceiling the decision clamps to rather than trusting a host to remember. And `scene::AwakeWorld` is the half a host cannot work out for itself: a script attaches a claim to the entity that needs the world running, so the claim dies with the entity instead of outliving whatever set it.
- **Reopen trigger. *Lifetime* is closed at v0.13** — the policy is hoisted, both hosts call it, and the server's caller is behind a flag whose absence is the old behaviour. *Placement* — which host a world runs on, and what happens when it dies — is unchanged and is the whole of what this entry is now: more than one world hosted by something that is not a test harness and not a single-process editor. That is a deployment.

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
- ~~Needs `physics` on the client for the entities it extrapolates, which today it does not link.~~ **It links it, and has since v0.7. What it does not do is call it**, and the linker is what makes the difference visible. Measured on the `release` preset: `client` carries 51 `engine::physics::` symbols and **8 of `libengine_physics.a`'s 15 members**, arriving through `Engine::script` beneath `Engine::game` and `Engine::examples` rather than because anybody asked for physics. **Which eight is the useful half.** `Shapes`, `ShapeRay`, `ShapeSupport`, `Query`, `ContactPairs`, `FaceManifold`, `PhysicsWorld` and `WorldResource` are in — the *query* half, because a script raycasts. `BroadPhase`, `NarrowPhase`, `IntegrateMotion`, `Solve`, `SyncBroadphase` and `Pipeline` are **out**, dropped because nothing under `mono.client/` calls `RegisterPhysicsSystems`. So (c) costs a caller and a tick order, not a dependency edge — **the same link-line-versus-call-graph distinction `D00004` had to make twice** and conflated for two versions before it did.
- **v0.13's network ownership is a second way to get a client integrating, and it is deliberately not this one.** An owned body is simulated by its owner *authoritatively* — the client's answer is the one that crosses the wire, and there is nothing arriving for it to be reconciled against. (c) is the opposite arrangement: the server stays right by definition and the client integrates a guess between corrections. **They must not both apply to one entity.** A body extrapolated under (c) that also carries a `scene::NetworkOwner` would be simulated twice with one of the two wrong, and the wrong one is whichever the local machine happens not to own. Whatever ships for (c) states which set it walks, and `NetworkOwner` is the cheap way to say it: extrapolate what nobody owns.

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

**Scoped at v0.13 and deliberately not started.** The library question this entry
left open is now answered and the cost is written out, because the useful thing
to know before starting is how much has to be true at once. **This is not a
change; it is a project.** Nothing below is speculative — every claim was checked
against the upstream trees rather than remembered.

**The library is `ngtcp2`, and the reason is that its core needs no TLS at all.**
Nothing under `ngtcp2/lib/` references OpenSSL, wolfSSL, GnuTLS or picotls — the
backends are separate `ngtcp2_crypto_*` helper libraries behind `ENABLE_*`
options, and so is every `find_package` in its top-level CMake. So the core is
**one MIT submodule with no Perl and no Go**, which is the only shape that keeps
the property this entry already names: a fresh clone needs CMake, Ninja and a C++
compiler and nothing else. It also settles the clock question by construction —
every entry point takes an explicit `ngtcp2_tstamp` — and it carries Reno, Cubic
and BBRv2, which is this entry's first argument, and RFC 9221 DATAGRAM, which is
its third.

**What was ruled out, so it is not re-evaluated from scratch.** `picoquic` hard
-requires picotls through `find_package(PTLS REQUIRED)` or a configure-time
`FetchContent`, which is a third-party dependency arriving by download rather
than by submodule, and defaults `WITH_OPENSSL` on. **wolfSSL is GPLv2 or
commercial**, which is a licence problem against MPL-2.0 and not a preference.
BoringSSL needs Go *and* Perl; quictls and LibreSSL need Perl.

**The crypto is a callback table, which is the good news and the trap.** ngtcp2
asks the application for `encrypt`, `decrypt`, `hp_mask`, `update_key`,
`client_initial`, `recv_crypto_data` and `rand`. That makes the TLS backend a
*later, contained* decision rather than a foundational one. It also means
**`net::Cipher` cannot serve those callbacks as it stands**, and the three
mismatches are structural rather than plumbing:

- **QUIC owns the nonce and `Cipher` owns it privately.** RFC 9001 derives the
  nonce by XORing the packet number into a static IV, so ngtcp2 supplies the full
  twelve bytes on every call. `Sealer` takes a four-byte prefix and holds a
  private counter that only moves forward, which is the invariant `Cipher.hpp`
  says may not be weakened "to make plumbing convenient". This is exactly that
  request, and the answer is a *second* type rather than a loosened `Sealer`.
- **Header protection is a primitive this engine does not have.** It is a raw
  ChaCha20 keystream block masking five bytes, not an AEAD, and `Cipher` exposes
  no keystream and no constructor from raw key material by design.
- **AES-128-GCM is mandatory whatever cipher suite is negotiated.** Initial
  packets are keyed by HKDF from the destination connection id (RFC 9001 §5.2)
  and Retry integrity is AES-128-GCM under a fixed key (§5.8). Crypto++ has it;
  `net` does not expose it, and there is no version of QUIC that skips it.

**The TLS backend is the one open decision, and the three answers differ in what
they buy rather than in effort alone.** (i) A minimal real TLS 1.3 in-tree over
the primitives D00006 already vendored, with RFC 7250 raw public keys so no X.509
parser is needed — standards-compliant on the wire, no new dependency, and about
two thousand lines of security-critical code this repository would own. (ii)
D00006's existing exchange carried inside QUIC's CRYPTO frames — smallest and
lowest risk, and a private variant: no HTTP/3, no Wireshark decode, and **the
cdn second-consumer argument above is not served**, which is half of why this
entry exists. (iii) quictls beside ngtcp2 — fully interoperable, and it costs the
fresh-clone property and adds a very large vendor tree.

**The whole of what has to land, because the cost is the count and not any one
item.** The vendor and its `MonoVendor.cmake` target with everything but `lib/`
disabled, and a `THIRD_PARTY_NOTICES.md` line. The TLS answer above. A crypto
seam for the three mismatches. Connection ids, transport parameters, Retry and
stateless-reset tokens — which subsume `Cookie` and have to keep its rule that an
unanswered challenge costs zero bytes. The expiry timer driven off the tick's
`nowSeconds` through `ngtcp2_conn_get_expiry`/`handle_expiry`, since a QUIC stack
that arms its own timer breaks `just determinism` and `just replay-check` in a
way that shows as neither passing nor failing but as two runs disagreeing. The
channel model mapped onto QUIC — unreliable to DATAGRAM frames, reliable to
streams, and the stream layout is a design decision because head-of-line blocking
still applies within one. Then the deletions this entry already lists, with their
suites and benchmarks. Then the rewiring: `replication::Session`, `Listener`,
`Connector`, `mono.server`, `mono.client`, `mono.studio`,
`mono.unified_server_client`, and `mono.network`'s discovery. Then
`ConnectionStats`, where **`SendsOverBudget` stops meaning what it means today** —
a fixed cap refusing is not a congestion controller pacing, and `render`'s debug
panel documents that distinction against `D00007`, so the panel and its header
move with this. Then `expected_graph.json` and the tier check. Then the suites,
most of which currently test things that would no longer exist.

**Staging is not a preference here.** This entry already says two overlapping
reliability stacks is worse than either, so the order is: land the QUIC session
beside the old one and prove it, rewire, and only then delete — with every commit
green, rather than a sweep that leaves the tree with no working link.

### [_] D00008

- **The single-player `ALLOW_TIER_ESCAPE` in `mono.client/CMakeLists.txt`.** It is written out in a comment there and deliberately not declared: `DEPS ... Mono::server` plus `ALLOW_TIER_ESCAPE Mono::server`, the one edge the tier rule has to permit by name rather than by rule, so that a `client`-tier program may link a `server`-tier library.
- v0.3's roadmap listed declaring it as part of wiring the two programs together. **The wiring turned out not to need it, and that is the finding rather than an excuse.** `--connect` talks to a server in another process over a UDP socket, which is precisely the arrangement where the client links no server code at all. Declaring it now would add an escape with no user — which is what the comment itself says not to do, and what somebody would eventually reach for to do something unrelated.
- **The escape now has a first user, and it is not the one this entry was written for.** `mono.unified_server_client` — a diagnostic product that runs both halves of replication in one process with `net` cut out of the middle — declares `ALLOW_TIER_ESCAPE Mono::server`, because it genuinely needs the client's draw seam and the server's world in one binary. **That does not close this item**: single-player is still undeclared in `mono.client/CMakeLists.txt` and still wants a game file first. What it does do is settle a question this entry could only speculate about — the escape works, the tier check names the edge, and the mechanism is no longer untried. When single-player arrives it is copying a line that has a working precedent rather than writing the first one.
- **The prerequisite landed at v0.7 and the trigger still did not fire, which is the useful part.** `mono.engine/game` exists and `mono.client --game FILE.agame` plays a game file single-player under `HostRole::OfBoth` — the exact line this entry named as the real prerequisite. It was declared **without the escape**: `mono.client` gained `Engine::game`, not `Mono::server`, because playing a game file needs the format and a VM and not a hosted server. So the entry's phrasing survives a second attempt to close it, and the reason is the same one v0.3 found — *hosting a server in your own process* is a narrower thing than it sounds, and twice now the feature that looked like it has not been it.
- **A second declared user arrived and it is a product rather than a diagnostic.** `mono.studio` declares `ALLOW_TIER_ESCAPE Mono::server`: an editor genuinely runs both halves, and `expected_graph.json` is where the fact is visible. With `mono.unified_server_client` that is two users, neither of them this entry's, and the mechanism is now ordinary rather than untried.
- **Reopen trigger, unchanged and now twice unmet: a client linking server code to host a server in its own process.** Restated against the link line rather than against the feature, because the feature has now shipped twice without needing it. When it does arrive the edge is two lines and the comment already says which two.
- Worth keeping straight, because the two are easy to confuse: the escape is about *linking*, not about connecting. A single-player client that spawned `mono.server` as a child process and connected to it over loopback would need no escape either, and is a legitimate third option to weigh at that point — it costs a process and buys the same crash isolation `parallel/process` already argues for.

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
- **Re-examined at v0.13 and the swap is deliberately not done.** Everything above says it costs the shipped programs nothing, and the other half of the trade has not been written down until now: **`Random.new(seed)` is bound into both script VMs**, so changing what is behind the interface changes every seeded stream every game has. That is a real behaviour change for a saving this entry has already measured at zero. The interface stays and so does what is behind it, until the trigger below actually fires.
- **Reopen trigger, re-phrased: a program that calls `core::Random` and links neither `net` nor `assets`.** There is still not one. The nearest miss is `mono.tools/bindings`, which has the link line and not the call.

### [_] D00001

- ~~`--script PATH` is accepted and warns.~~ **Closed at v0.5**, and it was the oldest thing in this entry — accepted and ignored since v0.1. Two VMs are vendored and linked, the file extension picks between them, and the flag loads a scene: `--script` on the client, `--game` on the server (ignored since v0.3), `--scene` on the unified harness. `mono.engine/examples/Rings.luau` and `Rings.js` build the same world through the same bindings, and the unified harness reads 512 entities on the server and 512 on the client from either.
- ~~`core/types` has `Vector3`, `Color3` and `CFrame` only.~~ **Closed at v0.4.** `AABB`, `Ray` and `RayHit` landed with the consumers this bullet was waiting for — `spatial`'s queries and `physics`'s narrow phase. Nothing else was added, deliberately: `Vector2` was considered and refused because §3.4 gates it on "the overlay or editor needs it" and neither does, and the culling operations an `AABB` invites (`Inverted`, `Grown`, `Contains(AABB)`) have no caller until v0.6's frustum cull.
- ~~`Column`, `ComponentSet`, `SparseSet` and `ChangeChannel` are not in `ecs` yet.~~ **Closed at v0.2** by the storage rewrite, and reopened and closed again at v0.4 by chunking. Recorded here rather than deleted because this bullet is why the entry was still `[_]` after the other half of it had shipped.
- macOS builds compile SPIR-V but not MSL; the cross-compile step is wired in CMake and untested. Linux/Vulkan is the verified path. **Still open, and still the least examined line in this file** — it is the only item here with no trigger, because nobody has a Mac to trip it.
- **Correction at v0.6, to the second bullet's reasoning rather than to its verdict.** "`Vector2` was considered and refused because §3.4 gates it on 'the overlay or editor needs it' and neither does" — **`Vector2` shipped at v0.6, and for neither of those reasons.** `UDim2` and `Rect` are made of it, and both arrived with the datatype vocabulary a script surface owes an author. The gate was right and the list of things that could open it was short by one, which is the useful half: a gate phrased as "who needs it" only names the consumers somebody had thought of. The other half of that sentence closed exactly as written — the `AABB` operations got their caller in `graph::Cull`, and `Frustum::Intersects` is the positive-vertex test that wanted an `AABB` rather than eight points.

**Three of four bullets are now closed and the entry stays `[_]` for macOS alone.** The paragraph that used to stand here said "two of four", which was true when it was written at v0.4 and stopped being true at v0.5 when `--script` closed — recorded rather than silently re-counted, for the reason D00004's drifting figure is recorded. `v02v03v04.md` predicted the v0.4 edit and said it belonged "with the next pass over `docs/DEFERRED.md`, not here".
