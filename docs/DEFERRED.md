
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

### [_] D00119

**`Player.CharacterAppearanceId` is absent, and it is absent because the thing
behind it is.** An appearance id is a content reference to an avatar the engine
has no notion of: `CharacterDesc` is three colours and six boxes, and there is no
catalogue, no bundle format and no loader that could resolve a number to a body.

**A property with nothing behind it reads as decided**, which is
`SoundService.cpp`'s rule and the reason it is named here rather than declared
and left returning zero. The same paragraph in that file lists eleven Roblox
members it does not have and what each would need first; this is the same
statement one class along.

**Reopen trigger: an avatar format** — the moment `MakeCharacter` can be handed
something other than three colours, the id is what names it.

**`Player.Team` shipped at v0.15 and is no longer part of this entry.** It was
held back on the stated ground that a team whose only effect is a coloured name
is a field rather than a feature, and the named reopen trigger was a
`SpawnLocation` class. That is what was built, in the order the parts depend on
each other:

- **`SpawnLocation` is a class**, deriving from `Part`, carrying `TeamColor`,
  `Neutral` and `Enabled`. `scene::FindSpawn` was a lookup for a part *named*
  `SpawnLocation` — the deliberate stop `Characters.hpp` recorded — and it now
  resolves the class **and** the name, because every scene in
  `mono.engine/examples` builds its pad as a block called `SpawnLocation` and a
  plain part wearing the name is read as an enabled, neutral spawn.
  `PartDesc::Class` is what keeps `MakePart` the only constructor for it.
- **`Teams` is a service and `Team` is an instance in it**, following
  `InstallServices` and the `ServiceComponent`/`ServiceScope` pattern.
  `Shared`, for `Players`' reason: a server decides who is on which side and a
  client asks which side it is on, so both halves need the list.
  `store.Protect()` covers it like every other fixture.
- **`Player.Team` decides where you appear.** `LoadCharacter` hands the player
  to `FindSpawn`, which prefers a pad of that team's colour, falls back to a
  neutral one, and never uses another team's — the first in tree order rather
  than a random one, because a respawn drawn from a random number is a recording
  that does not replay.

What is *still* absent is the `TeamCreate`-style permission rule the original
entry also named. Nothing in the engine asks who may edit what, so there is
nothing for it to attach to; it belongs with whatever brings collaborative
editing permissions rather than here.

### [_] D00116

**A client cannot ask its server to teleport it, so `TeleportService` is
server-only and says so.** The binding refuses on a replica — `Postbox::Teleport`
is an authority operation and `world/Postbox.hpp` has said so since v0.2 — and
that refusal is currently the whole of the story: there is no request channel a
`LocalScript` could use to ask for one.

**The shape it would take is already in the tree and is deliberately not
generalised yet.** `replication::MessageKind::User` carries `game::JoinNotice`
down and `Connector::Submit` carries `game::MoveInput` up; a client-initiated
teleport is a third message on the same two channels. What stops it being written
now is that a request is not an act — the host has to decide whether to honour
it, and "who may ask to be moved where" is a game's policy rather than an
engine's. A binding that queued the request and left the policy unwritten would
be an engine deciding it by default.

**Neither end of that channel has a caller, and that is nearer than the policy
question.** Checked at v0.15 rather than assumed, because the entry above reads
as though the only thing missing is a decision:

- **Nothing on a client can ask, because no client runs a script.**
  `client::BuildReplicatedWorld` installs a draw list, a camera and no VM —
  `replication/AGENTS.md` says `mono.client` runs no replicated scripts in those
  words — so `Client::DeliverGuiEvents` finds `RuntimeOf(Rendered)` null on a
  connected client and a press on a replicated `TextButton` reaches nothing.
  Single-player runs both roles and has no `Player` at all: `scene::AddPlayer`
  is called by `mono.server` and by `studio::PlayLink`, and by nothing in
  `mono.client`. The missing piece is a script running where the viewer is, and
  it is named in two other places already: `replication/AGENTS.md` calls the
  script half of a join deliberately absent for exactly this reason, and
  `examples/PlayerList.luau`'s TAB toggle is held open by it.
- **No host reads the up direction.** `Listener::OnUserMessage` has no caller
  outside `mono.studio`'s edit stream and the module's own tests, and
  `Server::ApplyInputs` decodes a `game::MoveInput`, then an `examples::Shot`,
  and counts everything else as `Dropped` — so a third message on the input
  channel arrives as a refusal statistic. A dedicated server also publishes
  `PrimaryWorld` and only that, so "which world is this client in" is not yet a
  question one server answers differently for two connections.

**What a client following a teleport looks like is already written down once**,
in `studio::Editor::FollowTeleports`, and whatever the shipped client eventually
does has to be that rather than a second copy of it: the destination is
*searched for by the player's name* rather than carried, because a handle may not
leave the world that minted it; `PlayLink::Stop` is the teardown and there is not
a second one; and `PlayLink::Missing` against `LOST_FRAMES` is the answer to the
frames where the player is in neither world, which is at least one because the
arrival is admitted at the destination's next barrier. The half both hosts would
share belongs in `game` beside `ApplyMoveInput`, for that function's stated
reason.

**Reopen trigger, restated: a script running in a client's replica.** The
original trigger was a game that wants a client to start a teleport, and it is
still true that every teleport this engine has a use for is decided by a server
script — a pad, a round ending, a matchmaker — and that all of those work. What
it did not say is that a game wanting the other kind could not have it: until a
client can run a line of Luau there is nobody to ask and nothing to ask with, and
the policy seam is the cheapest of the three parts rather than the blocking one.

### [_] D00109

**Filed as `D00108` and renumbered, because that number was already taken.**
`docs/retired/DEFERRED.md` carries a `D00108` closed at v0.13 by
`studio::EditStream` — team create's shared-document model — and the counter is
supposed to increment past retired entries rather than reuse them. The number
was picked by reading the front of the live file, which is exactly the half of
the register that does not contain the used numbers. `ROADMAP.md`'s ownership
entry cited the wrong one for a version and now cites this.

**`replication::Prediction` holds inputs as of v0.15 and still has nothing that
replays them, and ownership was not the caller it was waiting for.** The plan
that produced v0.13's ownership work assumed the two would meet — build the
upward state path, and prediction gets its consumer on the way past. They do not
meet, and the reason is worth writing down because it looks like they should.

`Prediction.hpp` says what it is for in its first line: "the local player and
nothing else. Everything else is interpolated authoritative state." An entity a
client *owns* is neither of those. It is simulated by that client and never
corrected — there is no authoritative state arriving for it to reconcile
against, because the client is the authority for it. So ownership does not give
prediction a caller; it describes a third category that prediction deliberately
does not cover, which is exactly Roblox's arrangement.

**What is actually unwired is larger than prediction**, and finding it was the
useful part of this entry. **Two of the three bullets closed at v0.15 and the
third turned out to be a different thing**, so they are kept with what actually
happened written against each rather than deleted:

- ~~**Nothing calls `Connector::Submit`.**~~ **Closed.** `client::Client::
  SubmitMove` sends a `game::MoveInput` every tick and an `examples::Shot` on a
  click. No client in this repository had ever sent an input; two now do, from
  one function.
- ~~**So `Prediction` never holds anything.**~~ **Closed, and by construction
  rather than by a second change.** `Connector::Submit` records into
  `Prediction_` on the way past, so the buffer fills the moment anything
  submits, and `Reconcile(Replica_.Applied())` is finally running against
  something.
- ~~**And the server's whole input path has no sender.**~~ **Closed.**
  `examples::EncodeShot` has a caller. `mono.server/tests/Replication.cpp`
  stands two clients on a real socket, fires from the first, and asserts the
  colour arrives at the second — the shooter never says what it struck.

**What closing them found is the part worth keeping.** Three defects, none of
them in the code being connected, and every one invisible from either side:

- **`scene::InputState::LatchPresses` latched keys and not buttons**, so a click
  that began and ended between two ticks was lost about two times in three. The
  frame/tick argument was written out in full above `InputState::Pressed` and
  applied to one of the two input devices, because jump was the only thing a
  simulation had ever acted on.
- **The rewind history walked `Transform` *and* `Motion`**, and `physics` takes
  a row's `Motion` away when it puts the body to sleep. A player standing still
  was therefore unhittable, and it presented as an ordinary miss. It walks
  `RigidBody` now, which is the question that was meant.
- **A client's input tick goes stale in a quiet world.** A tick reaches a client
  only when something changed, so in a still scene its idea of the server's
  clock stops while the server's does not, and the rewind targeted a tick that
  had fallen out of the ring. Resolved at the present now, with the argument in
  `Server::ApplyInputs`.

**What is left is one call, and it is the one this entry was named for.**
Nothing reads `Connector::Unconfirmed()`. The buffer fills, it is acknowledged
and drained, and no caller ever replays what is in it.

**Replay is the caller's job and that is correct rather than missing.**
`Prediction::Pending()` hands back what to replay and `Connector` does not
replay it, because replaying an input means knowing what an input *means* — the
same line every other opaque payload in this module sits on. A caller has to
apply them. What that means is that the first consumer writes the whole third
step of the loop, and a consumer that forgets it gets the rubber-band the header
warns about with nothing reporting why.

**The trigger fired at v0.15 and only half of what this entry predicted
followed, which is the correction worth recording.** It said the input encoding
and prediction's caller would arrive together with the first client that
controls something. The client arrived; the encoding arrived; prediction's
caller did not — and the reason is a decision taken deliberately one module
along rather than an oversight here.

**`game/Play.hpp` is why, and it says so in its own second paragraph.** A client
sends the *intent* and never the result: "the alternative was for a client to
simulate its own character and submit the resulting `Transform` … it puts a
physics step on both ends, makes every disagreement a reconciliation problem,
and hands a client the ability to state where its body is." A client that does
not simulate its own character has nothing to reconcile — it has latency, which
is a different thing and is not what `Prediction` reduces. So the consumer this
entry is waiting for is not "a character controller"; it is **a client that
simulates its own character locally**, which is a design this engine has
considered and refused for the local player's movement.

**Reopen trigger, restated: a client that runs a simulation step of its own.**
Not a character controller — there is one, and it did not need this. What would
is something whose latency is intolerable at the tick rate and which the client
can therefore be allowed to run ahead on. Until then `Prediction` is a mechanism
whose design premise this engine does not currently share, and that is a better
description of it than "unwired".

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

### [_] D00030

**A mutable property on a script *global* reads once and never again, because `luaL_sandbox` enables Luau's `safeenv`.**

- `UserInputService.MouseBehavior` is the first property in the engine that lives on a global rather than on an instance, and it does not work when it is read that way. `local UIS = game:GetService("UserInputService")` works; `UserInputService.MouseBehavior` returns whatever it was the first time any closure asked.
- **The mechanism, because it is not obvious and cost an hour.** `luaL_sandbox` freezes the global table and turns on `safeenv`, which lets the compiler emit `GETIMPORT` for a constant global followed by constant fields. `GETIMPORT` resolves the chain once per closure and caches the **value**. It does this whether the intermediate is a table or a userdata, so making the service a userdata does not fix it — that was tried, and the observation that settled it is that `__index` fires for the first read of a field and never for the second, with no raw key on the object to explain it.
- **The userdata is still right and is kept.** It is what makes every read *through a local* go to `__index`; a plain table would have been cached there too.
- **In practice it does not bite, which is why this is filed rather than fixed.** Every Roblox script begins `local UIS = game:GetService("UserInputService")`, and `game:GetService` is a method call that cannot be an import. The engine's own declaration files describe the property, the test uses the idiomatic form, and the comment in `UserInputService.cpp` says so.
- **What closing it would take.** Either not sandboxing — which is not on the table, `LuauRuntime` freezes the globals so one script cannot change the language the next one runs in — or making the service a *function call* rather than a global, which changes the surface away from Roblox's. Neither is worth it for a property nobody reaches the broken way.
- **This is Luau's alone, and v0.16 proved it rather than assumed it.** `UserInputService` and `SoundService` are bound by both languages now, and the JavaScript half is a plain object with a `JS_DefinePropertyGetSet` accessor per name — an accessor is not an import, so there is no chain to cache and nothing to defeat. `engine.script.scriptcall` reads, writes and reads again in *both* VMs and asserts the second read moved, which is what turns "JavaScript does not have this problem" into something the build holds.
- **The reopen trigger fired at v0.16 and the answer is still globals.** The count went from one mutable global property to three — `MouseBehavior`, `MouseDeltaSensitivity` and `SoundService.Volume` — with seven read-only ones beside them, so this is a pattern rather than an oddity. What the trigger asked was whether the surface should stop being globals; it should not, because the shape it would move to is not Roblox's and every script already writes `local UIS = game:GetService(...)`. What the growth *did* buy is that the workaround is no longer a comment in one file: `ServiceProperty` is a list both VMs walk, so the rule lives on the type and a tenth property costs a row.
- **Reopen trigger: a property somebody reaches the broken way in real code.** The count is no longer the question — three did not change the answer and ten will not either. What would is an authored script, an example or a panel that reads one off the bare global and gets a stale value, because that is the only form of this bug anybody can be bitten by.

### [_] D00019

**The engine's Luau is held at the revision the editor tool can consume so that
the editor and the type check agree. The current engine revision is Luau 0.731,
and 0.732 is the ceiling it is held under rather than where it is.**

- `mono.vendor/luau` is pinned to commit `f8ca77ac`, which is `Sync to upstream/release/731` and which `git describe` reports as **0.731**. 0.732 is the first revision luau-lsp cannot build against — it removed the `ConstraintSolver::reportError` overloads that `src/platform/roblox/RobloxLuauExt.cpp` calls — so it is the number that bounds this entry, not the number either tree is on. `mono.vendor/luau-lsp/luau` must be pinned to the same commit when that optional submodule is checked out. `mono.tools/scriptcheck` links the first and gates `just typecheck`; the language server in an editor uses the second. Two Luaus would mean an author reading diagnostics from a language the engine does not run, which is worse than no editor support because it looks authoritative.
- **The exact upstream ceiling belongs to luau-lsp.** Its nested Luau must remain buildable against the language-server sources. Do not bump the engine submodule alone: the sync check is the contract, and a failed `just luau-lsp` is preferable to silently giving authors diagnostics for another language revision.
- **Checked, not written down.** `just luau-lsp` compares the two `HEAD`s and refuses to build when they differ, naming both. Verified by mutation: bumping `mono.vendor/luau` alone makes the recipe fail with the two SHAs printed. Without that, the drift is invisible — the engine keeps passing every check it has, and only an editor is wrong.
- **What the choice actually costs, so a later reader can weigh it.** The engine follows the editor's compatible revision rather than independently following upstream. The trade is only defensible while the gap stays small; a long-lived gap would invert it, and the answer then is the fork below rather than a wider gap.
- **The fork is the way out and was declined at v0.7 on purpose.** Pointing luau-lsp at `mono.vendor/luau` needs sixteen mechanical call-site changes, and `mono.vendor/AGENTS.md` says a patch goes upstream or into a fork whose remote is recorded in `.gitmodules` — never into a file in this tree. That is a fork to maintain against a moving target, for a developer tool.
- **The third option arrived at v0.15 and this entry did not take it.** `docs/retired/DEFERRED.md` D00031 needed a change inside luau-lsp too, and what it used is a `.patch` under `mono.vendor/patches/` that `just luau-lsp` applies after cloning — no fork, no remote, no push. So "a patch goes upstream or into a fork" is no longer the whole of the rule, and the sixteen call sites above are now *mechanically* available at that price. They are still not worth it: D00031's patch is one hunk in a function that has not moved in two years, where sixteen hunks across a file upstream edits every release is a rebase every bump — which is a fork's cost with a fork's ceremony removed rather than a cheaper thing. The reopen trigger below is unchanged; what changed is that the way out is now measured in hunks rather than in whether a mechanism exists.
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

**(b) Group signatures — an audit layer, and the most interesting of the three. DONE.** Wire version 7, `replication/Audit.hpp`, `MessageKind::GroupSignatures` and `MessageKind::Disputed`.

- The server hashes groups of replicated state and sends the hashes; the client hashes its own copy and reports mismatches; the server sends the true state on a later tick. **Anti-entropy over the replicated world.**
- **It is complementary to deltas rather than a second way to do one job**, which is the question `docs/CODE_QUALITY.md` asks and the one this has to answer. Deltas are the fast path — what moved. Signatures are the audit — what disagrees. The audit is what makes the delta path's optimism safe, and it catches **generically** the whole class of bug this version chased one cause at a time: the lost creation, the stranded value, the stale forget, the tick that never completed.
- **The open question is answered per-client over a rotating slice, and the reason is that the other answer was not this entry's to give.** A cell hash is shareable only if a client sees a whole cell or none of it, which turns interest management from per-entity into per-cell — a real architectural change, and a larger one than a bandwidth entry authorises. The rotating slice is what bounds the per-client cost anyway: `AuditSettings` is `EntitiesPerGroup` 16, `GroupsPerAudit` 2, `EveryTicks` 8, so one audit is 32 entities in one datagram once every eight ticks and a world twice the size is swept half as often rather than costing twice as much. **The cadence argument was the strongest part of the proposal and it is what shipped.**
- **Membership is on the wire, and that single decision is what made the rest fall out.** The audit lists the entities it hashed rather than letting the receiver derive them from a group number, which lets the sender leave three sets out without the receiver knowing anything about them: anything still `Unconfirmed` (the delta path is already correcting it, so the server is merely ahead), anything the client *owns* (v0.13 ownership makes the client's copy the newer one), and anything carrying a `SuppressWhenTagged` tag (the far side derives that row, so the two ends are meant to disagree). It also means nothing about interest management, suppression or ownership had to cross.
- **What is hashed is the value a replica holds, on both ends, and assuming the quantiser is idempotent would have been wrong.** The authority puts its own value through the same encode-and-decode `BeginSnapshot` already puts a join through, so the two ends compute one expression over one buffer. Measured on the real smallest-three rotation: **1666 of 2 million** uniformly random orientations re-encode to different bytes, because the recovered component can come back below one of the three that were sent and the next encode drops a different one. That is a false mismatch reported for ever, every sweep, on one part in twelve hundred. `assets::HashTree` is the hash — tagged interiors, leaf count sealed into the root, which is what makes a *missing* entity a different digest rather than a matching prefix.
- **The rate limit is enforced by the server and none of it is read off the message**, which is what this entry called part of the security argument rather than a tuning knob. An answer must name the audit this server issued, on the tick it issued it, with labels that are groups this server hashed and strictly ascending so one cannot be named twice — and an audit may be answered once. The most a client claiming everything mismatches can buy is the repair of exactly the slice the server had already chosen to look at. An audit the link refused is struck off by `Unsent`, because a question that was never asked may not be answered.
- **The repair is the recovery walk and not a resend.** A disputed group puts its entities back into `Unconfirmed`, which is the same seeding an entity coming into view already gets — no second path, no new message, no structural churn. What it cannot reach is a client holding an entity the server has no record of sending; the bound on that is the one this module already had, `ResnapshotAfterTicks`, reached because a delta naming a row the client does not hold never lets `Applied` move. `Statistics::Disputed` says whether any of it is happening.
- **Off by default, on in `mono.server`, and that is the one thing that could not simply be the default.** `replication/AGENTS.md` says a quiet world sends nothing, and anti-entropy is precisely the thing that must speak on a world at rest — a value stranded on a still world is what no delta would ever report. Both cannot hold, so the host decides. `studio.playlink` and `engine.replication.stream` each pin the quiet-world property and are untouched.
- **Ten cases, ten mutations, every one red.** The two that matter are a replica deliberately diverged on a still world — corrupt a row on the client, and it comes back — and a client answering with every group label there is, which is refused and counted. The others pin the three exclusions, the round trip, the once-only answer and the refused audit; the mutation that removes the slice-range check does not merely fail, it reads past the recorded slice, which is what that check stands in front of.

**(c) The client simulating physics from the quantised state — last, and only with an invariant amended on purpose.**

- `replication/AGENTS.md` currently forbids it: *"Prediction is the local player and nothing else. Predicting a second entity means predicting what another player will do, which is wrong more often than it is right and is visible as rubber-banding when it is wrong."*
- **That rule was written about players, and this proposal is about objects.** Predicting an input-driven agent is guessing at a human; dead-reckoning a ballistic crate is evaluating a known function. They deserve different rules and the invariant does not currently distinguish them. **Amend it deliberately or not at all** — quietly reading it narrowly is how an invariant stops being one.
- **The hazard is error growth, and it is different in kind from (a)'s.** Interpolating between two quantised poses keeps the error inside the quantisation step. *Integrating* from a quantised velocity accumulates it linearly with elapsed time, so the bound is a function of how long since the last correction rather than of the grid.
- **It collides head-on with `D00010`'s decision**, which was that a dry buffer **stops rather than extrapolates**, on the stated grounds that guessing forward is "a freeze plus a lie" — the snap arrives when the next tick disagrees with the guess. Reconciling those is the actual design question and it is answerable: a physics-driven object has a *right* answer to extrapolate toward and a player does not.
- ~~Needs `physics` on the client for the entities it extrapolates, which today it does not link.~~ **It links it, and has since v0.7. What it does not do is call it**, and the linker is what makes the difference visible. Measured on the `release` preset: `client` carries 51 `engine::physics::` symbols and **8 of `libengine_physics.a`'s 15 members**, arriving through `Engine::script` beneath `Engine::game` and `Engine::examples` rather than because anybody asked for physics. **Which eight is the useful half.** `Shapes`, `ShapeRay`, `ShapeSupport`, `Query`, `ContactPairs`, `FaceManifold`, `PhysicsWorld` and `WorldResource` are in — the *query* half, because a script raycasts. `BroadPhase`, `NarrowPhase`, `IntegrateMotion`, `Solve`, `SyncBroadphase` and `Pipeline` are **out**, dropped because nothing under `mono.client/` calls `RegisterPhysicsSystems`. So (c) costs a caller and a tick order, not a dependency edge — **the same link-line-versus-call-graph distinction `D00004` had to make twice** and conflated for two versions before it did.
- **v0.13's network ownership is a second way to get a client integrating, and it is deliberately not this one.** An owned body is simulated by its owner *authoritatively* — the client's answer is the one that crosses the wire, and there is nothing arriving for it to be reconciled against. (c) is the opposite arrangement: the server stays right by definition and the client integrates a guess between corrections. **They must not both apply to one entity.** A body extrapolated under (c) that also carries a `scene::NetworkOwner` would be simulated twice with one of the two wrong, and the wrong one is whichever the local machine happens not to own. Whatever ships for (c) states which set it walks, and `NetworkOwner` is the cheap way to say it: extrapolate what nobody owns.

**Sequencing, and it falls out of the above rather than being chosen:** (a) is self-contained, needs no invariant changed, and makes (b) sound. (b) had one open question and it is answered above. (c) is what is left, and it needs a rule rewritten and a bound nobody has measured — `replication/AGENTS.md`'s prediction invariant, amended deliberately or not at all, which is the repository owner's decision and not an implementer's. **Reopen trigger for (a): the first world whose delta does not fit at the current budget** — which the priority work made survivable rather than fatal, so it is now a bandwidth question rather than a correctness one.

### [_] D00014

**Congestion control shipped at v0.15 without QUIC, which takes this entry's
first and heaviest argument away from it.** The rest of the argument is intact
and is what the entry now is. Recorded rather than closed, and recorded rather
than deleted, because the reason the first argument could be answered separately
is itself the finding: *congestion control is a property of the send rate, and
the send rate is ours whatever carries the bytes.*

- **The algorithm is Copa** — Arun and Balakrishnan, *Copa: Practical Delay-Based Congestion Control for the Internet*, NSDI 2018 — a delay-based window steering toward a standing queue of `1/delta` packets at the bottleneck, spelled `CongestionSettings::TargetQueuePackets` because that number *is* the packets of queue it settles at. **A loss-based AIMD window in the NewReno lineage was rejected and the reason is not a preference**: a loss-based controller finds the bottleneck by *filling its buffer*, which is the mechanism and not a side effect, so on a home router with a hundred milliseconds of buffer it adds a hundred milliseconds to every input a player sends. Vegas was the other delay-based candidate and Copa is strictly better: same equilibrium argument, plus an answer for the case Vegas is famous for losing. BBR trades better than either and wants per-packet delivery-rate sampling and a pacing engine, neither of which this transport has.
- **Against a TCP download on the same bottleneck it stops being polite, and only for as long as it has to.** A pure delay-based controller is starved — it backs off as the queue grows, the loss-based flow does not back off until the queue overflows, and the delay-based share converges toward nothing. Copa's competitive mode is implemented: when the window is reduced round trip after round trip and the queueing delay does not follow it down, the queue is not this flow's, and `TargetQueuePackets` then moves AIMD-style — one packet added per round trip, halved on a loss — which is the law the neighbour is playing. When the queue comes back down the mode ends and the target returns to two packets. **Latency is given up only while somebody else is taking it anyway.**
- **The mode-switch predicate is a restatement of Copa's and it was measured being wrong first.** The paper asks whether the queue is ever nearly empty, which holds because its per-acknowledgement window oscillates hard enough to empty it. A window steered once a tick settles at its target instead and empties nothing, so the paper's form read *every* ordinary path as contested — the controller latched into competitive mode on a solo 250 kB/s path and ratcheted its own standing queue from 9 ms to 190 ms. Both the absolute form and the fraction-of-recent-range form did that. The response test does not.
- **`LinkSettings::BytesPerTick` survives as a hard ceiling with the controller underneath it.** Kept rather than deleted because the two answer different questions: a game may legitimately refuse to spend more than N on one player on a path that would carry ten times that, and a hundred players on one host is a hundred of these — the operator's bill is not a function of what the path can take. What it stopped being is a *rate*. `PacketsPerTick` is left a fixed cap for a different reason: per-packet cost is a property of the two endpoints rather than of the path between them, so there is nothing on the wire for a controller to measure it against.
- **`SendsOverBudget` did not stop meaning what it meant, which this entry predicted it would have to.** The prediction assumed one counter for both refusals. There are two: `SendsOverBudget` is a number somebody configured being enforced and `SendsOverAllowance` is the path refusing, and the distinction is that a caller answers the first by changing the number and cannot answer the second by changing anything. `render`'s debug panel documents the first meaning against `D00007` and is untouched, and the panel and its header therefore do **not** have to move with this after all.
- **No second acknowledgement path was added and none may be.** Both signals come out of what already crosses the wire. The delay is `ReliableSender`'s RFC 6298 estimate arriving at `Link::RecordRoundTrip`, which `replication::Session` has called on every inbound packet since v0.9 — the estimator grew the variance it never had, because a controller told only the mean cannot tell a wireless link's fifteen-millisecond swing from fifteen milliseconds of queue. The loss is holes in `PacketHeader::Acknowledge` and `AcknowledgeBits`, the fields `ReliableReceiver::Acknowledging` already stamps on every outgoing packet whatever its channel: `ReliableSender` reads them to retire payloads and `Link::ObserveAcknowledgement` reads them to find out whether the path dropped something. One acknowledgement, two questions.
- **The honest limitation is that both signals are about the reliable channel.** Unreliable loss on the way out is reported by nothing, and a direction whose reliable stream is quiet offers no samples at all — which on a server publishing a still world is a real gap, since deltas are unreliable and structure messages are occasional. The controller is built so that no sample is never a *stall*: the slow-start ramp falls back on an assumed round trip and the queueing delay reads as zero rather than unknown. Closing the gap properly wants per-packet delivery feedback, which is a second ack path, which is QUIC — see below.
- **Determinism was answered before it was written and not after.** `net` reads no clock and nothing here is random, so the controller's whole state is a function of the sequence of calls it was handed — the same property the idle timeout has had since v0.3. `just determinism` and `just replay-check` are unaffected twice over: by construction, and because `mono.server` does not call `ServeClients` on the replay path at all, so no recording has ever contained network state. Both pass byte-identical.
- **Cold start is RFC 6928's initial window, once.** Ten datagrams on the opening tick, because that is what an initial *window* is and there is no feedback yet to pace against; every tick after it is paced at the window over the round trip. The controller is then clocked by acknowledgements rather than by a timer — the doubling, Copa's velocity parameter and the mode switch all wait for the far side to acknowledge what was outstanding when the period opened, which is one round trip measured rather than assumed, and is what makes one implementation behave on a loopback and on a satellite.
- **Twenty-nine mutations, twenty-nine red.** Four of them needed a test written that did not exist, and the two worth naming are the ones a review would not have found. Measuring the round-trip variance *after* moving the mean rather than before survives every directional assertion, because the wrong order is still in the right direction — it is pinned as arithmetic. And a tick's length measured against the last time anybody named a time, rather than against the last `Advance`, reads every tick as a stall and clamps the allowance to sixty bytes: the packets that arrived earlier in the tick named the same instant. That one was found by `replication`'s suite and now has a `net` case of its own.

**What is left of the QUIC argument, which is most of it.**

- Per-stream loss recovery without head-of-line blocking across streams, which is the shape this module arrived at by hand — structure reliable, values not.
- TLS 1.3, which subsumes the engine's X25519/HKDF/ChaCha20-Poly1305 and the server-identity binding closed in `D00006`.
- Connection migration and 0-RTT resumption, neither of which we would build.
- **A delivery signal for unreliable traffic**, which is new to the argument and which v0.15 could not give itself. QUIC's ACK frames acknowledge packets rather than payloads, so DATAGRAM frames are acknowledged too — the controller would see the whole outbound stream instead of the reliable slice of it, and that is the one thing the hand-rolled version structurally cannot have without inventing a second ack path.
- **It still passes the second-consumer test.** The game link is one; `ROADMAP.md`'s cdn wire streaming is the other, blocked on `net` growing an `http/` sub-area because a content origin serves bulk bytes over request/response rather than over a game datagram channel with a per-tick budget. **HTTP/3 is QUIC.** One dependency answers both.
- The clock question is settled and stays settled: `ngtcp2` takes an explicit `ngtcp2_tstamp` on every entry point, which is the only shape compatible with *time is passed in, never read*.

**Everything the v0.13 scoping wrote out still stands, minus one line.** The
library is `ngtcp2`, because nothing under its `lib/` references a TLS stack —
one MIT submodule, no Perl and no Go, which is the only shape that keeps a fresh
clone needing CMake, Ninja and a C++ compiler and nothing else. `picoquic` was
ruled out for hard-requiring picotls by `find_package` or `FetchContent`;
**wolfSSL is GPLv2 or commercial**, which is a licence problem against MPL-2.0
and not a preference; BoringSSL needs Go *and* Perl; quictls and LibreSSL need
Perl. The crypto is a callback table, which is the good news and the trap:
`net::Cipher` cannot serve those callbacks as it stands, and the three mismatches
are structural — QUIC owns the nonce where `Sealer` holds it privately and only
moves it forward, header protection is a raw ChaCha20 keystream this engine does
not expose, and AES-128-GCM is mandatory for Initial packets and Retry integrity
whatever suite is negotiated. The TLS backend is the one open decision and the
three answers differ in what they buy: a minimal in-tree TLS 1.3 over `D00006`'s
primitives with RFC 7250 raw public keys, `D00006`'s exchange carried inside
CRYPTO frames (smallest, and it serves neither HTTP/3 nor the cdn argument), or
quictls beside ngtcp2 (interoperable, and it costs the fresh-clone property).
The whole of what has to land is unchanged — the vendor and its
`MonoVendor.cmake` target, a `THIRD_PARTY_NOTICES.md` line, the TLS answer, a
crypto seam for the three mismatches, connection ids and transport parameters and
Retry and stateless-reset tokens (which subsume `Cookie` and must keep its rule
that an unanswered challenge costs zero bytes), the expiry timer driven off the
tick through `ngtcp2_conn_get_expiry`/`handle_expiry`, the channel model mapped
onto DATAGRAM frames and streams, the deletions with their suites and benchmarks,
then the rewiring of `replication::Session`, `Listener`, `Connector`,
`mono.server`, `mono.client`, `mono.studio`, `mono.unified_server_client` and
`mono.network`'s discovery, then `expected_graph.json` and the tier check, then
the suites. **The one line that comes off the list is `ConnectionStats` and
`render`'s panel**, which v0.15 has already sorted out by adding a counter rather
than redefining one.

**Staging is not a preference here.** Two overlapping reliability stacks is worse
than either, so the order is: land the QUIC session beside the old one and prove
it, rewire, and only then delete — with every commit green, rather than a sweep
that leaves the tree with no working link. **`net::CongestionControl` is on the
delete list when that happens**, since ngtcp2 carries Reno, Cubic and BBRv2, and
it is a hundred and eighty lines rather than a project.

**Reopen trigger, replaced because the old one fired and was answered by
something other than this entry.** It read "whichever comes first of cdn wire
streaming or the first deployment over a path that is not loopback or a LAN", and
the second half is now covered: a real path is paced by a real controller, and
`ConnectionStats` counts refusals against what the path would have carried as well
as against our own cap. **What is left is cdn wire streaming**, which is a second
consumer that request/response over TCP would also serve — so the trigger is
sharpened to the point where one dependency is cheaper than two protocols: *the
first time `http/` needs something TCP does not give it*, or *the first
measurement showing head-of-line blocking inside the reliable channel costing a
player something visible*. Either is a thing that can be observed rather than a
version number, which is what the v0.7 correction to this entry was about.

### [_] D00008

- **The single-player `ALLOW_TIER_ESCAPE` in `mono.client/CMakeLists.txt`.** It is written out in a comment there and deliberately not declared: `DEPS ... Mono::server` plus `ALLOW_TIER_ESCAPE Mono::server`, the one edge the tier rule has to permit by name rather than by rule, so that a `client`-tier program may link a `server`-tier library.
- v0.3's roadmap listed declaring it as part of wiring the two programs together. **The wiring turned out not to need it, and that is the finding rather than an excuse.** `--connect` talks to a server in another process over a UDP socket, which is precisely the arrangement where the client links no server code at all. Declaring it now would add an escape with no user — which is what the comment itself says not to do, and what somebody would eventually reach for to do something unrelated.
- **The escape now has a first user, and it is not the one this entry was written for.** `mono.unified_server_client` — a diagnostic product that runs both halves of replication in one process with `net` cut out of the middle — declares `ALLOW_TIER_ESCAPE Mono::server`, because it genuinely needs the client's draw seam and the server's world in one binary. **That does not close this item**: single-player is still undeclared in `mono.client/CMakeLists.txt` and still wants a game file first. What it does do is settle a question this entry could only speculate about — the escape works, the tier check names the edge, and the mechanism is no longer untried. When single-player arrives it is copying a line that has a working precedent rather than writing the first one.
- **The prerequisite landed at v0.7 and the trigger still did not fire, which is the useful part.** `mono.engine/game` exists and `mono.client --game FILE.agame` plays a game file single-player under `HostRole::OfBoth` — the exact line this entry named as the real prerequisite. It was declared **without the escape**: `mono.client` gained `Engine::game`, not `Mono::server`, because playing a game file needs the format and a VM and not a hosted server. So the entry's phrasing survives a second attempt to close it, and the reason is the same one v0.3 found — *hosting a server in your own process* is a narrower thing than it sounds, and twice now the feature that looked like it has not been it.
- **A second declared user arrived and it is a product rather than a diagnostic.** `mono.studio` declares `ALLOW_TIER_ESCAPE Mono::server`: an editor genuinely runs both halves, and `expected_graph.json` is where the fact is visible. With `mono.unified_server_client` that is two users, neither of them this entry's, and the mechanism is now ordinary rather than untried.
- **Reopen trigger, unchanged and now twice unmet: a client linking server code to host a server in its own process.** Restated against the link line rather than against the feature, because the feature has now shipped twice without needing it. When it does arrive the edge is two lines and the comment already says which two.
- Worth keeping straight, because the two are easy to confuse: the escape is about *linking*, not about connecting. A single-player client that spawned `mono.server` as a child process and connected to it over loopback would need no escape either, and is a legitimate third option to weigh at that point — it costs a process and buys the same crash isolation `parallel/process` already argues for.

### [_] D00001

- ~~`--script PATH` is accepted and warns.~~ **Closed at v0.5**, and it was the oldest thing in this entry — accepted and ignored since v0.1. Two VMs are vendored and linked, the file extension picks between them, and the flag loads a scene: `--script` on the client, `--game` on the server (ignored since v0.3), `--scene` on the unified harness. `mono.engine/examples/Rings.luau` and `Rings.js` build the same world through the same bindings, and the unified harness reads 512 entities on the server and 512 on the client from either.
- ~~`core/types` has `Vector3`, `Color3` and `CFrame` only.~~ **Closed at v0.4.** `AABB`, `Ray` and `RayHit` landed with the consumers this bullet was waiting for — `spatial`'s queries and `physics`'s narrow phase. Nothing else was added, deliberately: `Vector2` was considered and refused because §3.4 gates it on "the overlay or editor needs it" and neither does, and the culling operations an `AABB` invites (`Inverted`, `Grown`, `Contains(AABB)`) have no caller until v0.6's frustum cull.
- ~~`Column`, `ComponentSet`, `SparseSet` and `ChangeChannel` are not in `ecs` yet.~~ **Closed at v0.2** by the storage rewrite, and reopened and closed again at v0.4 by chunking. Recorded here rather than deleted because this bullet is why the entry was still `[_]` after the other half of it had shipped.
- ~~macOS builds compile SPIR-V but not MSL; the cross-compile step is wired in CMake and untested.~~ **Examined at v0.15, and the half of that sentence that mattered was false.** There is no cross-compile step. Nothing in the root `CMakeLists.txt`, `mono.build/MonoLibrary.cmake` or any module's CMake names MSL, SPIRV-Cross or `shadercross`, and `mono.vendor/` holds no translator: `shaderc` pins glslang, SPIRV-Tools and SPIRV-Headers in its `DEPS` and carries no `spvc`. "Wired and untested" is a worse state to record than "absent", because it reads as a step somebody has only to run. This is what the entry meant by the least examined line in the file, and it is why the correction is the first thing here rather than the last.
- **The second thing the examination found is that the item is in two places rather than one.** `mono.engine/render` links `shaderc` into the shipped client and `render::ShaderCompiler` compiles runtime-authored GLSL to SPIR-V while the engine runs, for every `ShaderScript` a `graph` pass names. So a build-time translation would cover the 49 built-in modules and leave every user-authored shader broken on the platform. macOS needs SPIR-V to MSL *in the binary*, which is a vendored library and a runtime code path, not a CMake line.
- **What runs and is checked on Linux now: `just shader-check`, in `just check`.** `mono.tools/shadercheck` reflects every `.spv` the build produces and holds it to the resource contract `SDL_CreateGPUShader` documents. One entry point per module, named what the renderer asks for, at the stage the filename claims; every resource carrying an explicit `layout(set, binding)`; every resource in the descriptor set SDL names for its stage; bindings contiguous within a set, because every other shader format numbers them by counting and a gap shifts everything after it; and every SPIR-V capability on an allowlist of what MSL can express, `Float64` being the one that compiles happily for Vulkan and cannot be translated at all. 49 modules pass. **This is the part of the item that never needed a Mac**: MSL, DXIL and DXBC all derive their bindings from those sets, so wrong sets make every translation wrong, and the sets are readable here.
- **What it deliberately does not claim.** It translates nothing, and it proves nothing about a Metal device. `MetalIndices` derives the `[[texture(n)]]` and `[[buffer(n)]]` each resource would land on and the tool prints it, because that assignment is a property of the SPIR-V and can therefore be read from a machine with no Metal, but a derivation is not an observation. Asserting more than that would be the same untested claim this bullet was corrected for, arriving a second time.
- **A macOS client now fails at the configure with the reason.** Previously it configured, compiled, linked, staged, started and died inside `SDL_CreateGPUDevice`, because `render::Renderer` asks for `SDL_GPU_SHADERFORMAT_SPIRV` and Metal offers `MSL` and `METALLIB` and never that. The root `CMakeLists.txt` says so at the second the configure knows it. `--preset server` and `--preset cdn` are untouched and are the presets that should build on macOS today.
- **What is left, in the order it has to happen.** A vendored SPIRV-Cross, pinned like its neighbours. The MSL entry point, which is `main0` and not `main`, because MSL reserves `main`, so `Renderer::LoadShader`, `InterfacePass` and the constant in `shadercheck`'s `Contract.cpp` move together. A shader format chosen at runtime rather than the literal `SDL_GPU_SHADERFORMAT_SPIRV` at three sites. `render::ShaderCompiler` gaining a second half, so a `ShaderScript` still works. Then the things only the machine can answer: whether the argument-buffer default matches what SDL's Metal backend binds, and the depth range, which is the classic difference and is silent.
- **Reopen trigger, and it replaces "nobody has a Mac" with something that can actually fire: anybody configures a client preset on Darwin.** The `message(FATAL_ERROR)` above is the trigger, made of the same material as the item rather than left to somebody's memory of this file. Two things follow from that phrasing. It fires for the first person who tries rather than for the first person who owns the hardware, which is the population that matters. And it cannot fire by accident on Linux, so it costs the verified path nothing.
- **The honest guess at what breaks first, recorded now so the guess can be graded later.** Not the translation. `SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, ...)` returning null before a single shader is read, because the format request is a literal in `Renderer.cpp` and has nothing to do with what is on disk. Second, the `main` against `main0` entry point, which fails per shader with a message that names the shader and not the reason. Third, the runtime compiler, which fails only when somebody writes a `ShaderScript` and therefore looks like a content bug.
- **Correction at v0.6, to the second bullet's reasoning rather than to its verdict.** "`Vector2` was considered and refused because §3.4 gates it on 'the overlay or editor needs it' and neither does" — **`Vector2` shipped at v0.6, and for neither of those reasons.** `UDim2` and `Rect` are made of it, and both arrived with the datatype vocabulary a script surface owes an author. The gate was right and the list of things that could open it was short by one, which is the useful half: a gate phrased as "who needs it" only names the consumers somebody had thought of. The other half of that sentence closed exactly as written — the `AABB` operations got their caller in `graph::Cull`, and `Frustum::Intersects` is the positive-vertex test that wanted an `AABB` rather than eight points.

**Three of four bullets are now closed and the entry stays `[_]` for macOS alone.** The paragraph that used to stand here said "two of four", which was true when it was written at v0.4 and stopped being true at v0.5 when `--script` closed — recorded rather than silently re-counted, for the reason D00004's drifting figure is recorded. `v02v03v04.md` predicted the v0.4 edit and said it belonged "with the next pass over `docs/DEFERRED.md`, not here". **The count is still of the original four**, which is why it did not move at v0.15 when the macOS bullet grew from one line to eight: the eight are one item examined, not seven new ones, and the item is still open.
