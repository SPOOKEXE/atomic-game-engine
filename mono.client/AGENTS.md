# mono.client — module invariants

The client program: a `client`-tier library and a thin main over it.

## This directory holds attachments, not engine

The main, the session bootstrap, the window and swapchain owner, the event
loop, and the demo scene. Anything reusable belongs under `mono.engine/` behind
a tier.

The test for "does this belong here" is whether a second program would want it.
The frame loop's *shape* would; `Client::Step` itself would not.

## The library exists so single-player can happen

`mono.client` is a library plus a thin executable, and the split is not
cosmetic. Single-player links `Mono::server` into this process and hosts a
server over a loopback transport. That is impossible when the program is one
executable's worth of globbed sources — there is nothing to link.

When that edge is added it goes in `CMakeLists.txt` as an explicit
`ALLOW_TIER_ESCAPE Mono::server` with a comment saying why. It is the one edge
the tier rule allows by name rather than by rule, and naming it is what keeps
it a decision instead of a precedent.

## Do not include a server header

`mono.server/include/server/` is invisible here, and it stays that way.

**The components are shared now, and the sharing is `mono.engine/scene` at
L7** — an engine module both programs link, not an include across two programs.
`Scene.hpp` — `Demo.hpp` as was — used to declare a `Transform`, a
`PreviousTransform`, a `Visual`, a
`SceneBounds` and an `ActiveCamera`, and `Replicated.hpp` used to declare the
server's two components a second time under the server's wire names so a
snapshot could resolve. All of that is gone. Both programs register the same
`scene` types under the same strings, so a snapshot and a delta cross with no
translation layer.

So **a component declared in this directory that means something a `scene`
component already means is the change to refuse.** `DrawList` is what is left,
and it is not a duplicate: it is what one world hands its compositor. `Spin` and
`Orbit` are gone with the C++ demo — a scripted scene writes `CFrame` directly
and needs neither.

`Replicated.hpp` survives its own reason for existing because it acquired a
better one — a replicated world still has to be *drawn*, nothing about drawing
it crosses a wire, and that is neither the demo's job nor the engine's.

## The world holds the world. This directory holds the program

The line is worth stating precisely, because it moved once already.

**In the store:** every component, the clock, the camera, the world bounds, the
draw list. Anything a system reads or writes. The camera is a *row* — a
`scene::Camera` and a `scene::Transform` on an entity, with the
`scene::ActiveCamera` resource naming which one is live — because a world may
hold several and exactly one is in charge. There is no `DemoScene` object
and there must not be one again — it held exactly that state as members and
handed systems a `this`, which put the half of the world the renderer reads
outside the affinity check, outside the profiler, and out of reach of a second
world. `mono.engine/ecs/AGENTS.md` has the full account.

**On `Client`:** the window, the swapchain, the frame budget, the panel scroll,
the parsed options. None of it is world state and none of it belongs in the
store. The test is whether a second world in this process would want its own —
a draw list yes, a window no.

## The demo scene is gone, and what replaced it

`Demo.hpp` and `Demo.cpp` were to go away when there was a game file to load a
scene from. **There is one now**: `mono.engine/examples/Rings.luau` builds the
ring scene through the same class table `Instance.new("Part")` resolves against,
and `--script` loads it. The C++ `BuildDemoWorld` is deleted and the files are
`Scene.hpp` and `Scene.cpp`, which is what they now hold — the client's own half
of loading a world, and no scene of their own.

**There is one path, and that is the point.** Keeping the C++ scene beside the
Luau one would have been two ways to do one job, which is the most expensive
kind of debt in a monorepo because both accumulate callers — and only one of
them exercises the bindings. `--script` with no argument falls back to the
example rather than to a second implementation.

What survives here is the client's half and nothing more:

- `DrawList`, which is what one world hands its compositor.
- `MoveCamera`, which is a placeholder and says so — a script can make and aim a
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
it. It is drawn: `BuildReplicatedWorld` installs a `DrawList` and one `PreRender`
system, the client presents it like any other world, and the compositor places
it beside the demo.

Three things about it are deliberate and each hides a real failure:

- **Only a `PreRender` system.** Everything in that world arrived. A simulation
  system there would be this process disagreeing with the authority once per
  tick, and the disagreement grows rather than corrects.
- **It is interpolated, and not by a `PreviousTransform`.** The demo
  interpolates between `PreviousTransform` and `Transform` because it owns both
  ends of its own tick. A replica owns neither, so the two states worth
  interpolating between are two *received* ticks — held in
  `replication::SnapshotBuffer`, which decides where between them the world is
  drawn. `Replicated.cpp` only asks. **Nothing interpolated reaches a
  component**: the pose goes into a `DrawInstance` and nowhere else, because a
  render-rate quantity written to a `Transform` would make the world this
  process replicates depend on the frame rate of whoever was watching it.
  `D00010`.
- **It has no camera of its own and is looked at through the demo's.** A camera
  is an entity, and an entity minted in a replica collides exactly with one the
  authority minted — the collision `Store::SetAdoptOnly` refuses. A local row in
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

## Simulation and rendering tick separately

`Client::Step` advances a `FixedTimestep` by the frame time and runs the
simulation phases that many times — usually zero on a fast machine, several
after a stall — then the `PreRender` phase once. `RENDER_PIPELINE.md` §14.

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
inherent — you can only draw between two states you already have — and it is
what buys smooth motion at any frame rate. At alpha 0 the drawn position is
exactly the previous tick; at 1 it is the current one.

`PreviousTransform` is captured in `PreSimulation`, before anything moves. Capture
it later and you interpolate from a place nothing was ever at.

## Panels read last frame

`Client::Step` draws the debug panels from the frame graph's *published* frame,
which is the previous one — this frame has not finished being measured. That is
correct and intended. Do not "fix" it by calling `EndFrame` before the panels
are drawn; the render pass would then be missing from every graph, which is the
part you most want to see.

## Order in the frame

`Actions::BeginFrame` clears the edge-triggered state and runs *before* the
event pump, not after. Clearing afterwards discards actions fired during the
frame before anything reads them.

`PumpSounds` runs **after the tick and the replica's apply, and before
presentation** — so a `Sound` a script started this frame is heard this frame
rather than next, and the state it reads has stopped moving.

## Sound is a seam, and it holds the state neither side can

`scene::Sound` is rows in a world and `engine::audio` is nodes in a graph.
Neither knows the other exists, and `Sounds.hpp` is the only thing that knows
both: it owns the mapping from an entity to the nodes standing in for it,
because the world must not hold node ids and the mixer must not hold entities.

**One `SoundStage` per world.** Node ids are minted per mixer and an entity is
only unique inside its own store, so one stage across two worlds collides on
both counts.

**Post only what changed.** The command queue is bounded and a full one drops
rather than blocks — right, because the consumer has a deadline — so a pass that
reposted its whole state every frame would fill it with no-ops and start
dropping the commands that were real changes. That is what the last-posted
values on `Voice` are for, and they are the values the *mixer* was told rather
than the values the world holds.

**Decode and resample once, at delivery.** The graph must never resample on the
device thread, and a buffer converted per voice would pay for it again for every
part playing one footstep. `DecodeAudio` picks its decoder from the **bytes**
rather than from the manifest's name, because a name is what a publisher typed
and the content is what arrived — and a decoder handed the wrong format produces
noise at full volume rather than nothing.

**Nothing distinguishes a replicated `Sound` from a locally created one**, and
that is why "under Workspace, in sync for everyone" and "made by a LocalScript,
heard by that player alone" need no audio rule at all. They are the same rows;
which client has them is replication's answer, already given.

**A start is scheduled against the sample clock**, never applied at the top of
whichever block it lands in. `audio/AGENTS.md` names that as the one place
"close enough to the frame" is wrong, and a `Play` posted without a deadline is
exactly the convenience it warns will be reached for.
