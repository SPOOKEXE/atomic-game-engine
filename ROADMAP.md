
# ROADMAP

## Editing

Do NOT place `deferred` items in this file. Place them in `docs/DEFERRED.md`. If a TODO item is not FULLY completed, split the TODO item, keeping the concise short dash point list with block infront and split it as another item under the same version.

For example, if we complete A and B but not C and D:
```
### v0.5
- [x] do: A1, A3, A5
- [_] do: A, B, C, D
- [x] do: G, H, I
- [x] do: K, K2, K3
```

becomes:
```
### v0.5
- [x] do: A1, A3, A5
- [x] do: A, B
- [x] do: G, H, I
- [x] do: K, K2, K3
- [_] do: C, D
- [_] deferred `D0001` for later.
```

or defer to another version.

## VERSIONS

### v0.0

- [x] repository setup; readme, license (mlp2.0), subfolders, build process, basic C++ main.cpp files for each subfolder.
- [x] repository setup: mono.server, mono.tools, per-folder tests/, tier checking at configure time, four presets (dev, release, server, ci)
- [x] vendors setup and build auto-downloads vendor folder (recursive clone submodules).
- [x] vendors setup: shaderc builds glslc from source, so no system shader compiler is a prerequisite and the SPIR-V is reproducible
- [x] concise documentation
- [x] concise documentation: RUNNING.md, SECURITY.md, THIRD_PARTY_NOTICES.md, docs/DEFERRED.md, and an AGENTS.md in every mono.X folder carrying that folder's invariants
- [x] essential libraries (spdlog, catch2, asio, tracy, sdl, imgui, flecs)
- [x] smart tests (cascading signature hash, smart-tests.txt)
- [x] code format documents, code quality checklist, CONTRIBUTING.md, ai guides (AGENTS.md, CLAUDE.md points to AGENTS.md), ai guidelines (always read the ai guide, code quality and code format documents)
- [x] ai skills and workflows — `.claude/skills/` (grug, ste-writing) and `.claude/commands/` (`/run-checklist`, `/new-module`, `/pr-analysis`), checked in so every contributor gets the same ones
- [x] essential libraries 2 (asio, imgui)

Deferred:
- D00001

### v0.1

- [x] cli argument parsing — `core::Arguments`, declared rather than scanned for, so `--help` is generated and an unknown flag is an error
- [x] entity-component system — `ecs::Store` and `ecs::Scheduler` over flecs, with a thread-affinity check that aborts
- [x] basic graphic rendering demo — SDL3 GPU, instanced, one draw call for the whole scene
- [x] tracy flamegraph (F5 keybind with pixel art visualiser) — four views: flamegraph, categories, systems, counters
- [x] fps counter (F3 with pixel art text) — 3x5 bitmap font, min/avg/max over a 20 s window, plus jitter
- [x] ecs test — plus unit suites for every other module, an integration suite and an architecture test
- [x] mono.server — `server`-tier library and thin main, fixed tick, headless. The binary contains no renderer and `just check-server-is-headless` proves it
- [x] tests live in each mono.X folder; tooling is C++ and CMake, so no scripting runtime is a prerequisite
- [x] `Store::EachParallel` — parallel within a tick. 3.5x at 500k entities, and slower than serial below ~60k, which is why the default grain is 4096
- [x] `core::Name` — a stable string and an interned dense id. The mechanism behind every registry that has to survive a rename
- [x] runtime shader compilation — `render::ShaderCompiler` over vendored libshaderc, the compiler `RENDER_PIPELINE.md` §11.9.1 calls the one honest blocker on a live shader swap
- [x] simulation and rendering tick separately — `core::FixedTimestep`, `Scheduler::RunPhases`, interpolated rendering and both rates on F3. `RENDER_PIPELINE.md` §14's measurement holds: 60.0 ticks/s while rendering at 2400 fps

Deferred:
- D00002

## v0.2

Planned in [v02v03.md](v02v03.md). Userland gets Roblox-style instancing, tweaked; the engine underneath is full ECS. The order below is the order the steps land in, and each one leaves the tree building and passing.

- [_] jobs: an atomic pool claim so a nested or concurrent `For` runs inline instead of deadlocking; `Store::Owner` atomic, because a world is picked up by a different worker every tick
- [_] `core::ByteWriter` / `ByteReader` — explicit little-endian framing, used by messages now and by saves and the wire later
- [_] engine-owned storage: `TypeDescriptor`, `Column`, `ComponentSet`, `SparseSet`, the archetype graph and cached queries. flecs removed, `Store::Native()` deleted, and the public `Store` API unchanged so the existing suites are the acceptance criterion
- [_] `ecs::ChangeChannel` — version stamps per column and per row range
- [_] instance model: the class table, `:IsA`, property descriptors, the `Parent`/`FirstChild`/`NextSibling` hierarchy, and change signals fired at a phase boundary rather than on assignment
- [_] world snapshot — serialise and restore a world through its `TypeDescriptor`s
- [_] `mono.engine/world` at L4: `Universe`, `World`, `WorldId`, `Address`, the directory and the control queue. Universe = overarching simulation, worlds = each subarea to simulate
- [_] World Entities — each world's root instance, and entity lifetime across the barrier
- [_] mailboxes and router — outbox and inbox, exactly one tick of latency, inbox sorted by `(sender, sequence)` so a replay cannot diverge
- [_] multi-world parallel processing — worlds are the batch; `WorldParallel` and `WorldSerial` are a tuning knob that changes no result
- [_] Universe Data (shared data in Universe) — the shared store on the driver barrier, with `Universe::Exclusive` as a documented escape hatch for consumers outside the simulation
- [_] asynchronous and synchronous methods to not block scripts — a `Ticket` inside a tick, `Await` only outside one
- [_] `parallel/ipc` — `Channel`, framed bytes, local implementation, the router never learning which it has
- [_] `parallel/process` — `WorldHost` child process, supervisor, heartbeat, restart from snapshot. The argument is crash isolation, not speed
- [_] multi-world rendering — a `ViewChannel` per world view: three slots and an atomic publish index rather than a lock, so a slow compositor drops frames instead of throttling a simulation. Carries a draw list rather than pixels, because a host that publishes pixels needs a GPU and stops being `server` tier, and SDL3's GPU API exposes no shared texture handles. Local and cross-process are the same call site with a different region
- [_] server and client move onto `Universe`; the client composites N views, and a local world is a view whose producer is in this process

## v0.3

Planned in [v02v03.md](v02v03.md).

- [_] `mono.engine/scene` at L7 — Basic Components: Transform, PreviousTransform, Bounds, Visual, Collider, RigidBody, Surface, Motion, Camera, QuickHash. Deletes the duplicated component definitions in `mono.client/Demo.hpp` and `mono.server/Simulation.cpp`; the C++ test scene itself stays until v0.4/v0.5 give it a game file to load
- [_] `scene::DrawInstance` — the draw-list payload v0.2's `ViewChannel` carries, at `shared` tier because a `server`-tier host writes it and a `client`-tier consumer reads it. Replaces `render::Instance` as the thing a world publishes; `mono.server` gains `scene` and `world` in its link row
- [_] `core/types`: AABB, Ray, RayHit — the value types v0.3 gives a consumer
- [_] `mono.engine/spatial` at L6 — uniform hash grid, and the optimised spatial query: raycast, overlap and shapecast with layer masks
- [_] basic physics collider — box, sphere and cylinder shapes, `Collider`/`RigidBody`/`Surface`, and the `SurfaceTable` resource the narrow phase reads once
- [_] basic physics pipeline — integrate, broadphase with sorted pairs, six exact analytic narrow-phase pairs, a serial sequential-impulse solver, contact events
- [_] Part — a class rather than a component: `{Transform, Bounds, Visual, Collider, Surface}`, with `PartDesc` and `MakePart` as the one place that decides what a part is
- [_] Camera — the `Camera` component, plus the `ActiveCamera` resource holding the live one and its resolved matrices

## v0.4

The engine has been full ECS since v0.2 and userland instancing is a façade over it — see [v02v03.md](v02v03.md). The test scene is still C++ at this point; v0.4 makes the façade reachable from script and v0.5 is where the scene actually moves.

- [_] bindings manifest for luau/typescript — generated from v0.2's `TypeDescriptor` property lists and class table, so there is no second source of truth for what a class is or what a property costs
- [_] plans for luau and typescript multi-threading and multi-processing systems (with locks, synchronise, etc, also plan integration with hytale setup for worlds) — over what v0.2 already established: the driver barrier, worlds-as-batch, `Ticket`/`Await`, `parallel/ipc` and `parallel/process`. The userland `thread` datatype is `parallel/threads/` and is a different contract from `Jobs`, because it has to survive a script yielding

## v0.5

Where the C++ test scene becomes a script. v0.3 rebuilt it on `scene`'s classes precisely so this is a port rather than a rewrite, and `Demo.hpp`/`Demo.cpp` go away here — `mono.client/AGENTS.md` has always said they die when there is a game file to load a scene from.

- [_] luau and typescript — `Instance.new`, properties, `.Changed`, the hierarchy and `:IsA` bind to v0.2's class table, the same one C++ calls. A calling convention, not a second mechanism
- [_] camera
- [_] surface camera (for mirrors, use previous frame as visual) — a view on v0.2's `ViewChannel` whose producer is the local world and whose consumer is a texture rather than the swapchain. The one-frame staleness is the latency that design already assumed
- [_] basic rendering pipeline (color, shadows, basic fov cull, sequential for many worlds) — the graph of `RENDER_PIPELINE.md`, its stages 1 to 7. Needs `mono.engine/graph/` at L9 first, and that needs v0.2's `ChangeChannel`
- [_] demo\_1world\_scene.luau (mirrors.luau, mirrors.ts) — the port of the C++ test scene, and the point where `D00001`'s `--script PATH` stops warning and starts loading
- [_] the demo dying is `D00004`'s trigger: ask whether anything still needs `core::Random`, and either close the item or say what renewed it

## v0.6

- [_] extended rendering pipeline (handle multiple worlds in parallel, handling gpu traffic) — `RENDER_PIPELINE.md` stages 8 to 12, including the HDR and G-buffer prerequisite its §17 opens with
- [_] demo\_2world\_scene.luau (mirrors.luau, mirrors.ts, split-screen for two worlds running in parallel via multi-process) — two `ViewChannel` producers and one compositor, which v0.2 already built. This demo is what proves the cross-process view path end to end

## v0.7

- [_] local filesystem content delivery network (cdn) in engine
- [_] mesh importing, baking and rendering pipeline
- [_] texture importing, baking and rendering
- [_] importing node pipeline (input nodes, processing nodes, export nodes)
- [_] auto export to assets folder
- [_] MeshPart + SurfaceAppearance (but as components) + Tagging => Render pipeline capabilities for filtering tagged objects for redirected pipeline work
- [_] put infront mirrors to see if it works with texture rendering
- [_] scripts that create MeshPart and set mesh properties (surfaceappearance equivalent but as components).

## v0.8

- [_] CurrentCamera
- [_] basic character controls
- [_] basic controls (enable/disable/shift-lock)
- [_] basic camera and controls (zoom, pan, control camera via script)
- [_] UserInputService and ContextActionService

## v0.9

- [_] ...

## v0.10

- [_] ...

## v0.11

---


