
# ROADMAP

# VERSIONS

## v0.0

- [_] repository setup; readme, license (mlp2.0), subfolders, build process, basic C++ main.cpp files for each subfolder.
- [_] vendors setup and build auto-downloads vendor folder (recursive clone submodules).
- [_] concise documentation
- [_] essential libraries (spdlog, catch2, asio, tracy, sdl, imgui, flecs)
- [_] smart tests (cascading signature hash, smart-tests.txt)
- [_] code format documents, code quality checklist, CONTRIBUTING.md, ai guides (AGENTS.md, CLAUDE.md points to AGENTS.md), ai guidelines (always read the ai guide, code quality and code format documents), ai skills and workflows.

## v0.1

- [_] cli argument parsing
- [_] entity-component system
- [_] basic graphic rendering demo
- [_] tracy flamegraph (F5 keybind with pixel art visualiser)
- [_] fps counter (F3 with pixel art text)
- [_] ecs test

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
- [_] basic rendering pipeline (color, shadows, basic fov cull, sequential for many worlds)
- [_] demo\_1world\_scene.luau (mirrors.luau, mirrors.ts)

## v0.6

- [_] extended rendering pipeline (handle multiple worlds in parallel, handling gpu traffic)
- [_] demo\_2world\_scene.luau (mirrors.luau, mirrors.ts, split-screen for two worlds running in parallel via multi-process)

## v0.7

- [_] ...

## v0.8

- [_] ...

## v0.9

- [_] ...

---


