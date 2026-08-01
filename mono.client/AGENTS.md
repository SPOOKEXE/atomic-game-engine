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

`mono.server/include/server/` is invisible here, and `server/Simulation.hpp`
duplicating the idea of a position component is deliberate. When components
become shared they become `scene` at L7 — an engine module both programs link —
not an include across two programs.

## The world holds the world. This directory holds the program

The line is worth stating precisely, because it moved once already.

**In the store:** every component, the clock, the camera, the scene bounds, the
draw list. Anything a system reads or writes. There is no `DemoScene` object
and there must not be one again — it held exactly that state as members and
handed systems a `this`, which put the half of the world the renderer reads
outside the affinity check, outside the profiler, and out of reach of a second
world. `mono.engine/ecs/AGENTS.md` has the full account.

**On `Client`:** the window, the swapchain, the frame budget, the panel scroll,
the parsed options. None of it is world state and none of it belongs in the
store. The test is whether a second world in this process would want its own —
a draw list yes, a window no.

## The demo scene is a placeholder with a real job

`Demo.hpp` and `Demo.cpp` go away when there is a game file to load a scene
from. Until then they are the only thing exercising the renderer, so two
properties are worth keeping:

- **It is a function of elapsed time, not of frame count.** Orbits are derived
  from accumulated time rather than integrated per frame, so two runs at
  different frame rates put the same cube in the same place. Without that, a
  frame-time comparison between two runs compares two different scenes.
- **It is deterministic.** `engine::core::Random` rather than `std::mt19937`,
  because a seeded standard generator may differ between standard libraries —
  `std::uniform_real_distribution` is not specified, so the same seed gives
  different numbers on different ones. `Random` is SHA-256 underneath and
  indexed rather than streamed: `Random::Float(index, salt)` is a pure function,
  so spawning one entity gives the value it would have had in a loop from zero.
  This was a mixer written out here and copied into `mono.server`; both are gone.

The scene's radius band is fixed rather than growing with the entity count. If
it grew, the camera would pull back to fit it and every cube would shrink —
`--entities 20000` would look *less* like a 3D scene than 500 does.

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
