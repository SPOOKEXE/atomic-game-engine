
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

**Rojo's file table: six of the nine landed at v0.12, and the three left each
need a dependency this repository does not vendor.**

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
- **`.toml` needs a TOML parser, and `mono.vendor` has none.** This one is
  otherwise free: the JSON path already emits the Luau, so a TOML document
  parsed into the same tree would reuse `LuauModuleFor` unchanged.
- **`.rbxm` is Roblox's binary model** — LZ4-framed chunks, interned strings and
  a referent table. That is a format reader and it belongs beside the other model
  decoders in `bake` rather than in an editor, which is also where `.rbxmx`
  would go once something could parse it.

Each of the three is reported by name and by what Rojo says it is, so a gap
reads as a gap rather than as an unrecognised file — `studio.rojosync` asserts
both halves, that the three are named and that the six are silent.

**Reopen trigger: a project that carries one of the three.** `.toml` is the
cheapest by a distance and the only one whose cost is a submodule rather than a
format reader.

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

### [_] D00001

- ~~`--script PATH` is accepted and warns.~~ **Closed at v0.5**, and it was the oldest thing in this entry — accepted and ignored since v0.1. Two VMs are vendored and linked, the file extension picks between them, and the flag loads a scene: `--script` on the client, `--game` on the server (ignored since v0.3), `--scene` on the unified harness. `mono.engine/examples/Rings.luau` and `Rings.js` build the same world through the same bindings, and the unified harness reads 512 entities on the server and 512 on the client from either.
- ~~`core/types` has `Vector3`, `Color3` and `CFrame` only.~~ **Closed at v0.4.** `AABB`, `Ray` and `RayHit` landed with the consumers this bullet was waiting for — `spatial`'s queries and `physics`'s narrow phase. Nothing else was added, deliberately: `Vector2` was considered and refused because §3.4 gates it on "the overlay or editor needs it" and neither does, and the culling operations an `AABB` invites (`Inverted`, `Grown`, `Contains(AABB)`) have no caller until v0.6's frustum cull.
- ~~`Column`, `ComponentSet`, `SparseSet` and `ChangeChannel` are not in `ecs` yet.~~ **Closed at v0.2** by the storage rewrite, and reopened and closed again at v0.4 by chunking. Recorded here rather than deleted because this bullet is why the entry was still `[_]` after the other half of it had shipped.
- macOS builds compile SPIR-V but not MSL; the cross-compile step is wired in CMake and untested. Linux/Vulkan is the verified path. **Still open, and still the least examined line in this file** — it is the only item here with no trigger, because nobody has a Mac to trip it.
- **Correction at v0.6, to the second bullet's reasoning rather than to its verdict.** "`Vector2` was considered and refused because §3.4 gates it on 'the overlay or editor needs it' and neither does" — **`Vector2` shipped at v0.6, and for neither of those reasons.** `UDim2` and `Rect` are made of it, and both arrived with the datatype vocabulary a script surface owes an author. The gate was right and the list of things that could open it was short by one, which is the useful half: a gate phrased as "who needs it" only names the consumers somebody had thought of. The other half of that sentence closed exactly as written — the `AABB` operations got their caller in `graph::Cull`, and `Frustum::Intersects` is the positive-vertex test that wanted an `AABB` rather than eight points.

**Three of four bullets are now closed and the entry stays `[_]` for macOS alone.** The paragraph that used to stand here said "two of four", which was true when it was written at v0.4 and stopped being true at v0.5 when `--script` closed — recorded rather than silently re-counted, for the reason D00004's drifting figure is recorded. `v02v03v04.md` predicted the v0.4 edit and said it belonged "with the next pass over `docs/DEFERRED.md`, not here".

### [DELETED] D00107

**Closed by doing it, and by doing the half this entry warned would be skipped.**
Kept rather than deleted, because the entry's own refusal — that a timer here
"trades a visible wrong picture for an invisible one" — is what shaped the
answer, and that is worth being able to point at.

- **`render::ChooseTexture` is the rule**, a free function so a suite can state
  it without a device: found, or named-and-expected, or named-and-not. The
  middle case draws the default material, so a scene load now looks like
  untextured parts becoming textured instead of a purple shimmer.
- **The renderer is told rather than asking**, because what is in flight belongs
  to the content pump and `render` must not reach up into it.
  `Renderer::ExpectTexture` on the request, `StopExpectingTexture` when it
  finishes; `TextureTable::Add` clears the mark itself, so no host can leave an
  arrival marked.
- **The failure half is the one the entry said not to skip, and it needed a new
  call.** A request that succeeds carries its name in the `Asset`; one that fails
  answers nothing at all, so `delivery::AssetClient::NameOf` was added — one
  virtual on an interface with one implementation. Both hosts read the name
  *before* `Take`, because a take is what destroys the record, and both unmark
  above every `continue` so no branch can forget. Two cases pin exactly that: a
  failed request still names what it was for, and a taken one no longer does.
- **No timer, and the entry was right that this was the temptation.** A grace
  period hides a genuinely missing texture for as long as it hides a streaming
  one, and with a byte budget in the path there is no N right for both a small
  scene and a large one.
- **The demo the entry did not ask for and should have.** `MeshGrid.luau` and
  `Meshes.luau` each gained one part naming a sheet nobody published, so both
  scenes now show all three answers at once — and the timing distinction is
  visible without reading a log. Verified by capturing the meshes world at 120
  frames, mid-load: the imports draw white and only the deliberate one is purple.

**What it looked like before:**

**A streaming texture and a texture that will never arrive look identical to
the renderer.**

`TextureTable` knows what it holds. It does not know what is in flight, and the
colour slot resolves a name that is not registered to `MissingTexture` — the
purple checkerboard — with no way to ask whether something is on its way. So a
sheet still crossing the network wears the marker for the frames it takes to
land, which on a scene load is a purple shimmer across every imported model as
their submesh textures arrive a step behind the geometry they belong to.

The gap is real rather than theoretical: the intake loop requests a mesh's own
sheets *while decoding the mesh*, so the mesh becomes resident at least one
frame before any of them can, and `delivery::IntakeBudget` may spread the sheets
over several more.

**Why it is not fixed with a timer here.** A grace period — "draw the default
for the first N frames after the name is first asked for" — hides a genuinely
missing texture for exactly as long as it hides a streaming one, and with a byte
budget in the path there is no N that is right for both a small scene and a
large one. It trades a visible wrong picture for an invisible one.

**What closing it takes.** The content pump knows what it has outstanding, and
that is the fact the renderer is missing. Roughly: the host marks a name as
expected when it issues a request for it and unmarks it when the asset arrives
*or the request fails*, and the colour slot draws the default for an expected
name and the marker for an unexpected one. The marker then means "nothing is
coming for this", which is the only meaning that is useful.

The cost is the bookkeeping, and the failure half is where it sits: the intake
loops hold `RequestId`s and a failed `Take` yields no name, so unmarking on
failure needs a request-to-name map in both the studio and the client. Skipping
that half is worse than not doing it at all — a misspelled texture name is
requested, misses, and would stay "expected" for ever, which is precisely the
case the marker exists for.

Until then: `render/AGENTS.md` records the limitation beside the three-way
split, and a texture that is briefly purple during a load is not a bug report.
