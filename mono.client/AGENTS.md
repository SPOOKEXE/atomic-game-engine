# mono.client - module invariants

The client program: a `client`-tier library and a thin main over it.

## What is here, and the test that decides it

The main, the session bootstrap, the window and swapchain owner, the event
loop, the content pump, and the presentation half of a world. Anything reusable
that does **not** need a device belongs under `mono.engine/` behind a tier.

**The test used to be "would a second program want it", and that test was
already false when it was written.** `mono.studio` links `Mono::client` and
includes five of its headers - `client/Scene.hpp`, `client/Replicated.hpp`,
`client/ContentDemand.hpp`, `client/EditableImages.hpp` and
`client/EditableMeshes.hpp` - and `mono.unified_tests` adds
`client/ContentLink.hpp` to that list. So a second program wants most of this
directory, and reading the old test literally would empty it.
`docs/ARCH_REVIEW.md` B and C4 are that finding.

**The real test is two questions, and the library/program split is where they
are answered.**

- *Does it need a device, a window or a swapchain?* Then it is `client` tier,
  and it is here unless it is big enough to be its own engine module -
  `Engine::render`, `Engine::ui` and `Engine::audio` are what that looks like.
  Nothing left in this directory is.
- *Is it the **program**'s decision or the **presentation**'s?* A default
  content origin, a command line, a frame budget, an exit code: the program's,
  and those live in `app/main.cpp` and on `Client`. Turning a world into a draw
  list, a `scene::EditableMesh` into a `render::MeshTable` entry, a `Sound` row
  into mixer voices: presentation, and a second program that draws a world wants
  every one of them.

**Two programs drawing the same world through two collectors is the failure
this arrangement exists to prevent**, and it has happened here: `Replicated.cpp`
and `Scene.cpp` each built a `scene::DrawInstance` by hand and drifted, which is
why both now go through `scene::MakeDrawInstance`. A studio that reimplemented
`CollectInstances` rather than linking it would be that failure one level up,
between two programs instead of two files.

So `Mono::client` is a library two programs link and one program is thin over,
and that is deliberate rather than an accident to be corrected. The section
below on single-player is the same property from the other end.

### `BuildMeshData` is `shared`-shaped and stays here anyway

`EditableMeshes.cpp`'s `BuildMeshData` is the clearest case `docs/ARCH_REVIEW.md`
C4 names: a pure `scene::EditableMesh` to `assets::MeshData` transform with no
device anywhere in it, sitting in a `client`-tier directory. Checked, and it is
true - the function names `scene` and `assets` and nothing else.

It stays, and the reason is where it would have to go. `scene` is L7 and
`assets` is L8, so `scene` may not host it; `assets` could, but `assets` is the
file formats and knows nothing about a component today, and giving it a `scene`
edge to host one function inverts a clean module. A module of its own is a row
in `expected_graph.json`, a layer, a tier, an `AGENTS.md` and a suite for
eighty-five lines with two callers, both of which link `Mono::client` already.

**The thing that would change this is a third caller that cannot link
`Mono::client`** - a server baking a script-built mesh, a tool importing one.
Until one exists this is machinery bought against a need nobody has, and the
honest place to record that is here rather than in a module nobody asked for.

## The library exists so single-player can happen

`mono.client` is a library plus a thin executable, and the split is not
cosmetic. Single-player links `Mono::server` into this process and hosts a
server over a loopback transport. That is impossible when the program is one
executable's worth of globbed sources - there is nothing to link.

When that edge is added it goes in `CMakeLists.txt` as an explicit
`ALLOW_TIER_ESCAPE Mono::server` with a comment saying why. It is the one edge
the tier rule allows by name rather than by rule, and naming it is what keeps
it a decision instead of a precedent.

## Do not include a server header

`mono.server/include/server/` is invisible here, and it stays that way.

**The components are shared now, and the sharing is `mono.engine/scene` at
L7** - an engine module both programs link, not an include across two programs.
`Scene.hpp` - `Demo.hpp` as was - used to declare a `Transform`, a
`PreviousTransform`, a `Visual`, a
`SceneBounds` and an `ActiveCamera`, and `Replicated.hpp` used to declare the
server's two components a second time under the server's wire names so a
snapshot could resolve. All of that is gone. Both programs register the same
`scene` types under the same strings, so a snapshot and a delta cross with no
translation layer.

So **a component declared in this directory that means something a `scene`
component already means is the change to refuse.** `DrawList` is what is left,
and it is not a duplicate: it is what one world hands its compositor. `Spin` and
`Orbit` are gone with the C++ demo - a scripted scene writes `CFrame` directly
and needs neither.

`Replicated.hpp` survives its own reason for existing because it acquired a
better one - a replicated world still has to be *drawn*, nothing about drawing
it crosses a wire, and that is neither the demo's job nor the engine's.

## The world holds the world. This directory holds the program

The line is worth stating precisely, because it moved once already.

**In the store:** every component, the clock, the camera, the world bounds, the
draw list. Anything a system reads or writes. The camera is a *row* - a
`scene::Camera` and a `scene::Transform` on an entity, with the
`scene::ActiveCamera` resource naming which one is live - because a world may
hold several and exactly one is in charge. There is no `DemoScene` object
and there must not be one again - it held exactly that state as members and
handed systems a `this`, which put the half of the world the renderer reads
outside the affinity check, outside the profiler, and out of reach of a second
world. `mono.engine/ecs/AGENTS.md` has the full account.

**On `Client`:** the window, the swapchain, the frame budget, the panel scroll,
the parsed options. None of it is world state and none of it belongs in the
store. The test is whether a second world in this process would want its own -
a draw list yes, a window no.

## The demo scene is gone, and what replaced it

`Demo.hpp` and `Demo.cpp` were to go away when there was a game file to load a
scene from. **There is one now**: `mono.engine/examples/Rings.luau` builds the
ring scene through the same class table `Instance.new("Part")` resolves against,
and `--script` loads it. The C++ `BuildDemoWorld` is deleted and the files are
`Scene.hpp` and `Scene.cpp`, which is what they now hold - the client's own half
of loading a world, and no scene of their own.

**There is one path, and that is the point.** Keeping the C++ scene beside the
Luau one would have been two ways to do one job, which is the most expensive
kind of debt in a monorepo because both accumulate callers - and only one of
them exercises the bindings. `--script` with no argument falls back to the
example rather than to a second implementation.

What survives here is the client's half and nothing more:

- `DrawList`, which is what one world hands its compositor.
- `MoveCamera`, which is a placeholder and says so - a script can make and aim a
  camera at v0.6, so this exists only until an example does.
- `CollectInstances`, which turns simulation state into a draw list. Not a
  demo's job: every world a client draws needs it.
- `BuildScriptedWorld`, which is the loader plus the two systems above.

The two properties the demo scene was kept for are now the *script's* to hold,
and `Rings.luau` holds them: it is a function of accumulated simulated time
rather than of frame count, and it draws its randomness from a hash of an index
rather than from a stream. Both are stated in that file, beside the code.

## A replicated world is presented, never simulated

`--connect` adds a world the server owns, beside the demo rather than instead of
it. It is drawn: `BuildReplicatedWorld` installs a `DrawList` and the `PreRender`
systems, the client presents it like any other world, and the compositor places
it beside the demo.

Four things about it are deliberate and each hides a real failure:

- **Nothing there advances the world.** Everything in it arrived. A simulation
  system would be this process disagreeing with the authority once per tick, and
  the disagreement grows rather than corrects. **Running a script is not
  advancing the world**, which is why the VM below is not an exception to this:
  every write a script could make into a row is refused by `ecs::Store`.
- **Nor is dead-reckoning a body that the buffer has run out of ticks for**, and
  `CollectReplicated` is where the second exception that is not one lives.
  `SnapshotBuffer::DeadReckonSeconds` says how long the clock has been unable to
  interpolate; this file decides who gets that time spent on them - a row with a
  `scene::Motion` and no `scene::NetworkOwner`, because the first is a function
  the authority already sent and the second says somebody else already simulates
  it. The result is a pose in a `DrawInstance` and nothing else, which is why
  this advances no world: no system runs, no phase is added, no component is
  written, and `physics::Advanced` is the whole of what is called.
  `replication/AGENTS.md` carries the amended invariant and `D00015(c)` is the
  decision. **Bounded twice** - by `InterpolationSettings::ExtrapolateSeconds`
  in time, and by `RECKON_HALF_EXTENTS` of the body's own size in distance,
  because nothing here runs a broad phase to ask what it is about to pass
  through.
- **It is interpolated, and not by a `PreviousTransform`.** The demo
  interpolates between `PreviousTransform` and `Transform` because it owns both
  ends of its own tick. A replica owns neither, so the two states worth
  interpolating between are two *received* ticks - held in
  `replication::SnapshotBuffer`, which decides where between them the world is
  drawn. `Replicated.cpp` only asks. **Nothing interpolated reaches a
  component**: the pose goes into a `DrawInstance` and nowhere else, because a
  render-rate quantity written to a `Transform` would make the world this
  process replicates depend on the frame rate of whoever was watching it.
  `D00010`.
- **It has no camera of its own and is looked at through the demo's.** A camera
  is an entity, and an entity minted in a replica collides exactly with one the
  authority minted - the collision `Store::SetAdoptOnly` refuses. A local row in
  a replicated world is safe once the predicted-entity index range exists, and
  not before.

  **This is why a replicated world can be fully drawn and still not visible,
  and it has cost somebody an afternoon.** The composited camera is world zero's
  and is placed from *that* world's `WorldBounds`: the demo scene is about
  twenty-four metres across, so the camera orbits roughly ten to twenty-four
  metres out with a far plane of three times that. The server's placeholder
  world is a hundred and twenty-eight metres across of one-metre cubes, offset
  along X by `--view-spacing`. Most of it is past the far plane and the rest is
  sub-pixel. `--view-spacing 0` overlays the two and brings it into view, which
  is a workaround rather than the fix; the fix is the camera above.

  The F4 panel and the `replica:` line at exit exist so this is one reading
  rather than an afternoon: rows arrived, rows were drawn, and the scene is
  still empty means it is being drawn and not being looked at.

Its view channel is tracked at the join rather than at connect, because a
channel allocates its slots once and the only size this process has is what
actually arrived.

## A replica runs the client's own scripts, and only those

`BuildReplicatedWorld` opens a VM through `game::StartWorldScripts` with
`script::HostRole::OfClient`. Four rules, and none of them is checked by the
build:

- **Which scripts run is a class rule and a container rule.** The class half is
  Roblox's and `script::ScriptsIn` has always had it: a `Script` is the
  server's. The container half is `script::ClientScriptsIn` and is what a
  *replica* needs on top - a `LocalScript` runs when it is under this viewer's
  own `Player` or under `ReplicatedFirst`, so somebody else's player and the
  `StarterPlayerScripts` template are excluded by where they are. A single-player
  host is deliberately not filtered that way; it owns the world it is in.
- **The scripts arrive after the VM.** Every other host starts a world's scripts
  in one pass over a world it has already built. This one is empty when its
  runtime opens, so `replica-scripts` asks each tick what has arrived and
  `Runtime::RunNewScripts` starts each instance exactly once.
- **The refusals are `ecs::Store`'s and this directory adds none.** A client
  script cannot write a property - `Store::SetProperty` refuses in an adopt-only
  store - and cannot mint an instance, and both refusals reach the author as a
  raised error rather than a silent no-op. What it may write is what is not a
  row the authority owns: an attribute, a world resource, and the client-only
  surfaces the engine hands it.
- **The store is adopt-only before the VM opens.** `Client::BeginConnecting`
  says so rather than leaving it to the first `Connector::Poll`, because opening
  a VM and installing `GuiService` both ask the store whether minting is legal,
  and an authoritative index taken in that window is one the server is also
  handing out.

## The interface belongs to the world the player is standing in

`Client::InterfaceWorld` answers the replica once the join has completed and the
drawn world otherwise, and everything in `Client::Draw`'s interface block -
layout, compile, the router, `gui::Type`, `DeliverGuiEvents` - uses it.

A single-player client is its own host. `EnsureLocalPlayer` establishes its
local player before world scripts start, so a top-level
`Players.LocalPlayer` read is valid. A scripted scene that authors a
`StarterGui` template has it copied after startup; one that authors its live
interactive interface directly under `PlayerGui` keeps those exact instances
and their runtime signal connections. Never reset an empty `StarterGui` over a
live `PlayerGui`, because that deletes the interface and replaces it with
nothing.

A connected client draws its local scene *and* the server's, and a person's
`PlayerGui` is a subtree of their own `Player`, which is a row in the replica.
Compiling the drawn world and delivering the press there is what made every
button in a replicated world silent: the router picked the right element,
produced the right event, and handed it to a VM that was not the button's.

**The pointer belongs to it too, and that is the same rule and a second bug.**
`UserInputService.MouseBehavior` and `MouseIconEnabled` are `scene::InputState`
fields, so there is one of each *per world*, and there is one window. Until
v0.19 `Client::WriteInput` mirrored both onto a member as it passed - and it is
called once per simulated world per frame, plus once for the replica - so the
pointer obeyed whichever world was entered last. Reproduced with `--worlds 2`
and a script that asks for `LockCenter` in one world and `Default` in the other:
the window took world 1's answer while the player stood in world 0. Root
`AGENTS.md` rule 2, exactly: the ECS owns the fact, a flat copy cannot represent
"per world", so it cannot be right.

There is no copy now. `Client::PumpEvents` reads both out of
`InterfaceWorld()`'s store at the point the window call is made. What survives
on `Client` is `AppliedPointerMode` and `AppliedPointerIcon`, which are the
record of a system call and not a copy of anything: no store holds what
`SDL_SetWindowRelativeMouseMode` was last told, and the compare against it is
what keeps a window-manager round trip off every frame. The `pointer:` log line
names the world that asked, because a client obeying a world the player is not
standing in is what this used to do and nothing said so.

## Simulation and rendering tick separately

`Client::Step` advances a `FixedTimestep` by the frame time and runs the
simulation phases that many times - usually zero on a fast machine, several
after a stall - then the `PreRender` phase once. `RENDER_PIPELINE.md` §14.

Three rules follow. The first two used to be conventions and are now structural:

- **A simulation system never sees the frame delta.** It cannot: no delta is
  passed to a system at all. `Store.AdvanceTick(Timestep.Delta())` records the
  fixed step on the world's clock as `Delta`, and `Store.SetFrame(delta, alpha)`
  records the frame's as `FrameDelta`. Reaching the wrong one now means naming
  a different field rather than accepting a differently-named argument.
- **The camera runs on simulated time, not wall time.** It is placed by a
  system in the Simulation phase from `Time().Elapsed`, which advances a fixed
  step per tick. Using wall time would move the camera at one speed while
  everything it looks at moved at another.
- **`ClearTimings` is called once per frame, not per tick.** `RunPhases`
  accumulates, so a system that ran three times shows its total for the frame
  rather than the last of three rows.

**Rendering interpolates, and is therefore up to one tick behind.** That is
inherent - you can only draw between two states you already have - and it is
what buys smooth motion at any frame rate. At alpha 0 the drawn position is
exactly the previous tick; at 1 it is the current one.

`PreviousTransform` is captured in `PreSimulation`, before anything moves. Capture
it later and you interpolate from a place nothing was ever at.

## Panels read last frame

`Client::Step` draws the debug panels from the frame graph's *published* frame,
which is the previous one - this frame has not finished being measured. That is
correct and intended. Do not "fix" it by calling `EndFrame` before the panels
are drawn; the render pass would then be missing from every graph, which is the
part you most want to see.

## An unchanged final image never reaches the swapchain

The client owns one `PresentationDamageTracker` because it owns one final
presentation. Object, particle, environment and portal signatures invalidate
the scene source independently; the game interface is signed separately and is
compiled only from `PlayerGui`. They meet at the game composition. There is no
Studio composition in this program.

When every layer hits, `Client::Draw` returns before `WaitForFrame`, command
buffer acquisition and swapchain acquisition. A swapchain image is not durable
cache storage, so the final-image hit means no presentation at all, not copying
the previous image into a new swapchain image. If a frame could not be acquired,
do not commit the candidate signatures and do not count a successful write.

Record cache decisions alongside `FrameResult` upload, command-buffer and GPU
heap statistics. A supposedly unchanged frame that still submits is a
presentation regression. A high source hit rate that still uploads resident
rows is a residency regression. They are separate failures and the diagnostics
must keep them distinguishable.

## The content pump is the largest thing here that nothing described

`Client::PumpContent` is 323 lines and had no account in this file at all until
v0.19. It is not the biggest function in the directory - `Client::Step` is 1,177
lines and is described three sections down - but it is the one with rules a
reader cannot recover from the code in one sitting.

Four steps, in this order and for reasons that are each a failure somebody had:

- **Ask once, when the catalogue is ready.** `OfferPublishedContent` hands every
  world the manifest's mesh *names* - a few hundred strings - because a scene
  can only name what it can discover, and a script's own catalogue holds what
  has already been asked for. Names and never content: asking by kind fetched
  6.9 GB through one synchronous function on the frame the editor opened, which
  is the failure `client/ContentDemand.hpp` carries in full.
- **Collect demand only when a content-bearing component changed.**
  `CollectWantedContent` is several walks of a store and used to run on every
  world on every frame to produce, in the steady state, an empty list. The gate
  folds the component revisions and matching counts for the types that can name
  content. A simulation tick or a transform write is not content demand. Counts
  are part of the key because removing the last reference must be observable
  even though no surviving row carries the removal.
- **Pump delivery.** `AssetClient::Pump` resolves, verifies and decompresses
  **synchronously**, because the fetch path may not have a thread: a completion
  that landed at a moment scheduling chose would be a desync. That is why the
  step above must not over-ask.
- **Take completions against a byte budget.** `delivery::IntakeBudget` is what
  stops the frame that notices a room full of new models from taking a third of
  a second. A deferred arrival is *held*, never dropped, so the budget is a
  delay and not a loss.

**Two things are asked for at the moment they become readable rather than by
the walk**, and both would otherwise never be fetched at all: a mesh's own
sheets, whose names live inside the mesh file and are read on arrival, and a
material's colour map, which `scene::ResolveMaterials` writes into a
`SurfaceAppearance` field the next scan already reads.

Demand is deduplicated by interned content name before delivery. Ten thousand
emitters naming one particle sheet issue one request and `render::TextureTable`
owns one GPU texture for that name; per-emitter texture copies are a residency
bug. Rebuilding the delivery client clears both the request memo and the world
revision baselines, so a newly published catalogue can resolve the same name to
new content without reviving per-frame scans.

**Where it runs is load-bearing.** Before the simulation and outside every pass:
content becoming visible mid-tick is rule 5's desync, and content registering
mid-frame is an upload into a buffer a render pass may be reading.

## `client::Action` is this program's diagnostics, and it lives here now

`client/Actions.hpp` was `engine/input/Actions.hpp` until v0.19.
`docs/ARCH_REVIEW.md` C6 was right about it: all twelve members are this
program's own intents - ten profiler and HUD panels, a wireframe toggle and
`Quit` - and no target but this one ever named them. An engine module at L12
whose job is input has no business enumerating `WriteProfilerSnapshot`.

**The key table came with it, and the invariant it carried still holds.**
`input/AGENTS.md` said "nothing outside this module names a key"; what that
protects is that a binding is *one table* rather than branches spread over a
frame loop. There are exactly two places an `SDLK_` may appear:
`input::KeyOf`, which answers what a script sees, and `BINDINGS` in
`src/Actions.cpp`, which answers what this program does. **Do not add a third**,
and in particular do not compare `event.key.key` anywhere in `Client.cpp`.

Adding a behaviour that needs a key means adding an `Action` first, and every
`Action` needs a name and a binding - `tests/Actions.cpp` asserts both exist for
every member, so forgetting the table is a test failure rather than a feature
nobody can discover.

## Order in the frame

`Actions::BeginFrame` clears the edge-triggered state and runs *before* the
event pump, not after. Clearing afterwards discards actions fired during the
frame before anything reads them.

`PumpSounds` runs **after the tick and the replica's apply, and before
presentation** - so a `Sound` a script started this frame is heard this frame
rather than next, and the state it reads has stopped moving.

## The keyboard reaches the interface through the world, not through a member

Three steps, all in `Client::Draw`'s interface block, and each is a hop that
was missing once:

- **`SDL_StartTextInput` is asked for only while `gui::FocusedTextBox` answers
  something**, and compared before it is called. It is not a subscription - it
  raises an on-screen keyboard on a phone and opens an input method's window on
  a desktop - so a client that started it once and left it on would put a
  keyboard over the game. Headless has no window and therefore never calls it.
- **`Translator::TypedText()` goes straight to `gui::Type`**, which writes
  `Label::Text` in the store. Nothing here keeps the string: `input/AGENTS.md`
  says why it never reached `scene::InputState`, and this is the one hop it
  makes instead.
- **Typing is applied before `Router::Update`.** The characters were produced by
  a keyboard aimed at whatever held the focus when they arrived; routing first
  would post them into the box the person is only now clicking on.

`--type TEXT` synthesises the SDL event so that the second and third steps are
checkable with no keyboard attached. It needs `--click NAME` to have focused a
box first, and it cannot check the first step at all - the log line the toggle
writes is what says that call was made.

## Sound is a seam, and it holds the state neither side can

`scene::Sound` is rows in a world and `engine::audio` is nodes in a graph.
Neither knows the other exists, and `Sounds.hpp` is the only thing that knows
both: it owns the mapping from an entity to the nodes standing in for it,
because the world must not hold node ids and the mixer must not hold entities.

**One `SoundStage` per world.** Node ids are minted per mixer and an entity is
only unique inside its own store, so one stage across two worlds collides on
both counts.

**Post only what changed.** The command queue is bounded and a full one drops
rather than blocks - right, because the consumer has a deadline - so a pass that
reposted its whole state every frame would fill it with no-ops and start
dropping the commands that were real changes. That is what the last-posted
values on `Voice` are for, and they are the values the *mixer* was told rather
than the values the world holds.

**And a drop is repaired, never recorded as landed.** `CommandQueue::Post`
returns whether the command was queued and all fifteen call sites in
`Sounds.cpp` discarded it until v0.19, which turned one transient full queue
into three different permanent faults: a dropped `Open` burst is a voice that is
silent for the life of the world with every side of the system reporting it as
correct, a dropped `SetGain` recorded as sent is a fader stuck at the old level
for ever, and a dropped `RemoveNode` is a node nothing can ever ask for again.
`audio/Commands.hpp` names the three classes - coalescable, repairable,
terminal - and states what each owes a refusal. Here that means: a last-posted
value is written only after `Post` returned true; a voice is reserved with
`CommandQueue::Free` and built all at once or not at all; and a teardown whose
row has gone is held in `SoundStage::PendingCloses()` and retried, because
nothing in the world will ever ask for it a second time.

**Decode and resample once, at delivery.** The graph must never resample on the
device thread, and a buffer converted per voice would pay for it again for every
part playing one footstep. `DecodeAudio` picks its decoder from the **bytes**
rather than from the manifest's name, because a name is what a publisher typed
and the content is what arrived - and a decoder handed the wrong format produces
noise at full volume rather than nothing.

**Nothing distinguishes a replicated `Sound` from a locally created one**, and
that is why "under Workspace, in sync for everyone" and "made by a LocalScript,
heard by that player alone" need no audio rule at all. They are the same rows;
which client has them is replication's answer, already given.

**A start is scheduled against the sample clock**, never applied at the top of
whichever block it lands in. `audio/AGENTS.md` names that as the one place
"close enough to the frame" is wrong, and a `Play` posted without a deadline is
exactly the convenience it warns will be reached for.
