
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

- [_] cli benchmarking (like smart tests but we benchmark all systems in the code instead to find bottlenecks). utilises tracy.
- [_] hytale ECS setup. Universe = overarching simulation, worlds = each subarea to simulate. Multi-world with parallel processing, lock mechanism for global stores like player data and such in Universe, asynchronous and synchronous methods to not block scripts. plan ahead now.

## v0.3

- [_] Basic Components: Transform, Bounds, Visual, Collider, RigidBody, Surface, Motion, Camera, QuickHash
- [_] Part
- [_] Camera
- [_] World Entities
- [_] Universe Data (shared data in Universe)

## v0.4

- [_] bindings manifest for luau/typescript
- [_] plans for luau and typescript multi-threading and multi-processing systems (with locks, synchronise, etc, also plan integration with hytale setup for worlds)

## v0.5

- [_] luau and typescript
- [_] camera
- [_] surface camera (for mirrors, use previous frame as visual)
- [_] basic rendering pipeline (color, shadows, basic fov cull, sequential for many worlds) — the graph of `RENDER_PIPELINE.md`, its stages 1 to 7. Needs `mono.engine/graph/` at L9 first, and that needs v0.2's `ChangeChannel`
- [_] demo\_1world\_scene.luau (mirrors.luau, mirrors.ts)

## v0.6

- [_] extended rendering pipeline (handle multiple worlds in parallel, handling gpu traffic) — `RENDER_PIPELINE.md` stages 8 to 12, including the HDR and G-buffer prerequisite its §17 opens with
- [_] demo\_2world\_scene.luau (mirrors.luau, mirrors.ts, split-screen for two worlds running in parallel via multi-process)

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
- [_] basic physics collider
- [_] basic physics
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


