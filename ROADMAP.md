
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

Planned in [v02v03v04.md](v02v03v04.md). Userland gets Roblox-style instancing, tweaked; the engine underneath is full ECS. The order below is the order the steps land in, and each one leaves the tree building and passing.

Standing discipline for v0.2 and v0.4, in that document's own section: preallocate and reuse by default, take the better data structure without ceremony but attach a number to an algorithm swap, and allow async only for work the tick cannot observe finishing.

- [x] jobs: an atomic pool claim so a nested or concurrent `For` runs inline instead of deadlocking; `Store::Owner` atomic, because a world is picked up by a different worker every tick
- [x] `core::ByteWriter` / `ByteReader` — explicit little-endian framing, used by messages now and by saves and the wire later
- [x] engine-owned storage: `TypeDescriptor`, `Column`, `ComponentSet`, `SparseSet`, the archetype graph and cached queries. flecs removed, `Store::Native()` deleted, and the public `Store` API unchanged so the existing suites are the acceptance criterion
- [x] `ecs::ChangeChannel` — per-row dirty bits held as a lazily-added `DirtyBits` column (never a tag, which would move the row on every write), plus a coarse counter a batch write still moves. `Observe<T>` opts a type in; `EachChanged<T>` walks only what moved
- [x] instance model: the class table with per-class prototype rows (so `Instance.new` is a column copy and `:Clone()` falls out of it), `:IsA`, property descriptors, and the `Parent`/`FirstChild`/`NextSibling` hierarchy. Organisational only — not a transform hierarchy, because `Transform` is world-space and nothing propagates
- [x] change signals fired at a phase boundary rather than on assignment — `Store::OnChanged<T>` and `FlushSignals`, called by `World::Tick` after the simulation phases. The dirty bits *are* the queue, so there is no second record of what moved; three writes in one tick signal once with the value it ended at, and the changed set is collected before anything fires so a listener may add, remove or destroy without walking a table that has moved underneath it. A flush refuses to re-enter, so a listener writing what it listens to is one signal at the next boundary rather than a loop. Changes clear at the *start* of a tick, not the end, so `PreRender` still sees what the tick did
- [x] world snapshot — `Store::Save`/`Load` through each `TypeDescriptor`. Components are recorded by **name**, so a snapshot crosses processes and builds; the entity directory is reproduced exactly, index and generation alike, so an `Entity` stored inside a component is still the same entity after a restore. Refuses rather than half-writes when a component has no serialisation, and leaves the store empty rather than half-restored on any failure
- [x] `mono.engine/world` at L4: `Universe`, `World`, `WorldId`, the directory and the control queue. Universe = overarching simulation, worlds = each subarea to simulate. Per-world tick rates, Active/Idle/Suspended/Faulted, soft-fault quarantine with a crash-loop cap, and `Enter` as the only — scoped — way to reach a store
- [x] World Entities — each world's root instance (`workspace`), created with the world so no system has to check for one; structural changes to the world list queue to the barrier rather than mutating it underneath a running batch
- [x] communication buses — worlds never address each other: MessagingService (pub/sub topics), MemoryStore (ephemeral shared map, sorted map, queue), DataStore (durable key/value, versioned, read-modify-write) and Teleport. Routing is hub-and-spoke, so there are N channels rather than N², and every ordering decision is made in one place. There is no cross-world entity reference at all, which is the Roblox model and deletes the type that would have broken rule 3
- [x] outbox and inbox — resources on the world's store (so a snapshot carries pending traffic), exactly one tick of latency, applied at the barrier in `(sender, sequence)` order. `From` is stamped by the driver, not the sender: a world does not get to say who it is, and every ordering decision depends on that field
- [x] multi-world parallel processing — worlds are the batch, dispatched longest-first so the tail stays short; `WorldParallel` and `WorldSerial` proven by test to produce identical results. Host pool sizing lands with `parallel/process`
- [x] Universe Data (shared data in Universe) — the bus backends and nothing else. Player data is a DataStore key, not a row in a world nobody owns. `Universe::Peek` is the documented escape hatch for consumers outside the simulation, and a system may never call it. DataStore's durable backing is in-memory for now and says so
- [x] asynchronous and synchronous methods to not block scripts — every bus call returns a `Ticket` and replies land next tick, which is the contract `:GetAsync()` already teaches. Per-world request budgets, reset at the barrier, so one world cannot starve a neighbour
- [x] recording and replay — `Recorder`/`Replayer` over a versioned format that is one snapshot plus retained input, plus `Universe::Save`/`Load`. Snapshots are byte-stable (every map is written in name order), so two of them can be diffed. Determinism is same-binary and cross-machine is documented as not promised
- [x] `mono.server --record` / `--replay` flags, proven end to end by a test that records a run, replays it and compares every entity
- [x] a CI job that runs a scene twice and diffs — `.github/workflows/ci.yml`, split by what each job needs installed rather than by what it checks, so the half that runs the determinism diff runs on a machine with no graphics stack at all and proves the tier split by existing. `just check` runs the same list locally in the same order, so "it passes here" and "it passes in CI" mean one thing. `just replay-check` now diffs as well as runs: recording a replay has to give back the recording it replayed, which needed `--replay` and `--record` together to stop being silently ignored. The workflow itself has not had a first run on GitHub — every recipe in it passes locally
- [x] `parallel/ipc` — `Channel`, framed bytes, bounded and non-blocking (a send that waited would stall a job worker and with it every world in the host). Local implementation; the caller cannot tell which transport it holds, which is the whole point
- [x] `parallel/process` — `Process` (spawn, poll-and-reap, request-stop, kill) over `posix_spawn`, plus `WorkersPerHost` so eight hosts on a 24-core machine do not each start 23 workers. A destroyed handle never leaves an orphan
- [x] soft-fault quarantine — a throwing system faults one world and its neighbours never notice; a hard fault takes the host and is documented as un-isolatable, because catching `SIGSEGV` and carrying on means carrying on with a suspect heap
- [x] supervisor — `PlanHosts` groups worlds into hosts (dedicated ones alone, shared ones packed, deterministic so a restart rebuilds the same grouping), plus heartbeat deadlines, restart, and a crash-loop cap. Time is passed in rather than read, so a five-second deadline is testable in a microsecond and no wall clock reaches anything reproducible
- [x] `mono.server --host` mode and a `Channel` over a socket, so a supervised host is a real process rather than an in-process one. `MakeProcessChannel` is the same `Channel` interface over a `socketpair` — framed, bounded, non-blocking, and close-on-exec on both ends so each side actually notices the other going away. `Process::Start` places one end at a fixed inherited slot, so a host needs no address, no retry and no timeout to find its driver. `HostLink` carries five signals and stamps the sender rather than trusting it. A host's universe is `Federated`: it holds worlds and no bus backend, collects and orders what they posted, and hands it up — two processes each answering the same DataStore key is the thing that cannot be allowed. Proved by four cases that spawn the real binary
- [x] multi-world rendering — a `ViewChannel` per world view: three slots and an atomic publish index rather than a lock, so a slow compositor drops frames instead of throttling a simulation. Carries an opaque payload at L4 — a draw list rather than pixels, because a host that publishes pixels needs a GPU and stops being `server` tier, and SDL3's GPU API exposes no shared texture handles. Drops are counted rather than hidden
- [x] `mono.server` moves onto `Universe` — one named world in a universe rather than a bare store, `Enter` as the only (scoped) way in, presentation run explicitly because `PreRender` on a headless server is where replication extraction lives
- [x] `mono.client` moves onto `Universe` — one named world, the universe owning the accumulator so the client no longer keeps a second copy of its own tick rate, and the renderer called from *inside* `Enter` so the camera and draw list stay where they were produced
- [x] the client composites N views through `ViewChannel` — `client::Compositor`, one channel per world. The renderer no longer reaches into a store at render time: it draws what the compositor took off the channels, so a frame at the display's rate and a world at its own no longer touch the same memory. `--worlds N` gives it something to composite; each view after the first is *placed* rather than overlaid, because two worlds' coordinates do not mean the same thing. A producer that stalled keeps its last frame and is counted stale — a world flickering out of existence for one frame is worse than a world one frame behind
- [x] the driver's side of the host link — `world::Driver`, one barrier over worlds here and worlds elsewhere: pump the links, ingest their traffic, tick, dispatch outward, poll. A world a host holds is registered in the driver's directory as `WorldState::Remote` from the moment it is planned, so a subscription or a teleport addressed to it has somewhere to go before its host has answered anything. **There is one router**, so a world's bus behaviour does not change by being moved out of the driver — proved by a differential test that runs the same script of publishes with every world local and then with half of them in a host, and requires the delivered set to match. `Envelope::From` stays stamped rather than trusted across the boundary: traffic is tagged with the host that handed it over, and a host claiming a world it does not hold is refused and counted. `mono.server --remote-world` makes it reachable end to end, with a real driver spawning real hosts, and the worker budget worked out once by the only process that knows how many there will be
- [x] reserve the replication seam — the server has authority and a client simulates its own replica, so v0.2 makes snapshot restore work into a *running* store rather than only an empty one, and a replica's bus handle refuses writes. `Store::Apply` merges into a live world under an `ApplyMode` — `Authoritative` destroys what the sender did not mention, `Overlay` leaves it — matching entities by index *and* generation so an entity the sender destroyed and recreated is a different entity here too, and reading a corrupt stream into a scratch store first so a bad packet cannot half-merge. The `Replica` resource refuses every bus write at the call rather than at review time, while the inbox still delivers. Building it surfaced one thing the wire protocol has to solve: two independently built stores allocate the same entity indices, so a replica cannot yet own a locally predicted entity — pinned by a test and carried in `ecs/docs/TODO.md` under v0.3. The wire protocol, interest management and lag compensation are a later version with their own plan, and are not started here
- [x] `mono.cdn` — the content origin scaffolded at `shared` tier, which is the strict choice: `MONO_TIER_ALLOWS_shared` is `shared` alone, so a presentation or hosting module on its link line fails the configure with the edge named rather than needing a build-option guard the way `mono.server` does. Library plus thin main, the `cdn` preset, `just cdn` / `just serve` / `just check-cdn-is-bare`, and the `expected_graph.json` row. Landed at v0.2 rather than v0.8 so the tier and the link row are settled before there is anything to move
- [x] `cdn::ContentRoot` — the boundary between a content name and the filesystem, with two checks that are not redundant: components before the disk is touched, which catches `..` and absolute names and cannot see a symlink, then containment of the resolved path against the canonical root, which catches the symlink. A symlink staying inside is served, because an atomically swapped `current` is the deployment pattern this is for. Profiling spans and `cdn.resolve.served` / `.refused`, so a refusal rate — somebody walking the origin — reads apart from content going missing
- [x] the CDN's own work was pulled forward from v0.8 rather than left for it — `Engine::assets` at L8 and the rest of `mono.cdn` are built and listed under that version. What is still open there is the wire hop and the client's barrier-applied completions, and both wait on something outside the CDN: `Engine::net`'s transports, and the world driver
- [x] `mono.engine/assets` at L8 `shared` — the second new engine module this version, after `world`. Content addressing, chunking, the hash tree, the manifest and the one signature over it; the format both halves of content delivery share, so the running game and the origin cannot acquire a dialect. Deliberately depends on no simulation module, which is what lets the origin link it alone. Full detail under v0.8
- [x] two vendors added — **BLAKE3** for content addressing and **Zstd** for delivery-group compression, both in `mono.build/MonoVendor.cmake` with an entry in each `THIRD_PARTY_NOTICES.md`. Neither is gated on a tier: portable code with no platform dependency, like asio and Crypto++. Zstd is dual-licensed and **we take BSD-3-Clause, not GPLv2** — the one choice in that file where reading the wrong text leads to a materially wrong conclusion about shipping the engine. Legacy 0.x frame support is off, as decoder surface parsing origin-supplied bytes that nothing here could have written
- [x] `just docs-check` restored to green — four dangling `[v02v03.md]` links in this file were failing the site pass, and because that pass runs first, the coverage pass behind it had never run at all. Exactly the cascade `docs/CODE_DOCUMENTING.md` describes happening once before: a check that has been failing for a while stops being read, and takes the real failures down with it. Fixing the links surfaced two genuine documentation gaps that had been invisible
- [x] the `cdn` preset wired into CI — `just preset=cdn test-all` on the bare headless runner, so the origin's suites are run rather than merely compiled, plus `test-architecture` on the `server` and `cdn` presets. **That last one is the only place `requires` in `expected_graph.json` is exercised at all**: the `full` job checks a preset with every option on, and there an entry whose `requires` is wrong looks exactly like one that is right
- [x] `mono.cdn` wired into the documentation build — it was absent from all three of docgen's hand-maintained input lists, so its pages were never generated. The include-roots list is globbed precisely so a new module cannot be missed; the two above it are literal, so it was
- [x] smart benchmarking — `benchrunner`, a thin main over `Tool::testrunner`'s own library, so discovery, the cascading signature and the skip cache are the *same code* rather than a second copy that drifts. A benchmark declares `TEST_SUITE_ID` like a test and answers `--mono-suites` like one; `mono_suites` was split out of `testmain` so a benchmark binary gets that without linking a test framework whose per-case overhead would land inside the numbers. Reports the **minimum** sample per iteration, not the mean — a benchmark is bounded below by the work and unbounded above by the machine's mood — with the spread beside it. `just bench` / `bench-all` / `bench-accept` against a `bench` preset that optimises, and its skip cache lives in the build directory because a signature covers sources and not compiler flags. A regression is reported and never enforced: a laptop on battery swings further than most real ones
- [x] benchmark the ECS, world and renderer areas — 24 measurements across `engine.ecs.bench.iteration`, `engine.world.bench.barrier` and `engine.render.bench.overlay`. They confirm two documented claims and produce one number nothing had: `EachParallel` is **5.5x slower than `Each` at 10k entities** (21.9 us against 4.0) and **3.0x faster at 500k** (67.7 us against 202), so the ~60k crossover the default grain was chosen for still holds after the storage rewrite; `EachBatchParallel` at 500k is 2.5x `Each`; a *quiet, empty* world costs about 0.4 us of barrier per tick measured serially, so 200 of them are 81 us before any simulates anything; and the debug panels cost 997 us at 4K against 60 us at 1080p — 16.7x the time for 4x the pixels, which is superlinear and the one figure here nobody had guessed
- [x] the harness reports its own noise — a run that reported `+52.1% slower` for code that had just been made faster is what forced this. Each figure now carries the spread of its own samples, and a delta smaller than that spread is labelled `(noise)` and left out of the faster/slower counts. The multi-world benchmarks were running at +-25% to +-128% because thread wake-up dominates fifty empty worlds, so serial variants were added beside them: those measure the barrier rather than the scheduler and hold +-5%. A tool that cannot say how much it does not know generates confident noise
- [x] act on those numbers, first pass — the per-world barrier cost. Three avoidable allocations per world per tick: `Fanout.assign(N, {})` destroyed and rebuilt every world's delivery vector, throwing away the capacity it had just grown; handing an inbox over with `SetResource` **copies**, and a resource column holding a non-trivial type destroys and re-copies on assignment, so every world's delivery buffer was freed and reallocated each barrier; and the per-tick budget reset was a hash lookup and a copy to move eight bytes. Now a resize-and-clear, a vector swap, and an in-place write. **59.9 us to 40.2 us, a third off**, on 50 worlds fanning 2450 deliveries a tick, at +-5-9% — measured against the committed version by reverting to it and back
- [_] the rest of those numbers — the overlay's superlinear scaling with area (997 us at 4K), and vectorised compute in the ECS iteration paths. Left open rather than guessed at: the baseline exists and the before-and-after discipline above is what the harness was built for

## v0.3

Replication. The server has authority and a client simulates its own replica and syncs against it — decided while planning v0.2, which reserves the seam for it (see [v02v03v04.md](v02v03v04.md) §2.12) and builds nothing else. It sits here rather than later so that physics, scripting and rendering are all built against a replicated world instead of being retrofitted into one.

- [x] `mono.engine/net` at L11 `shared` — the connection lifecycle, framing and channels every one of `upstream/`, `downstream/`, `predict/` and `http/` sits on. `Link` is the state machine: Connecting → Connected → Disconnecting → Disconnected, handshake and idle timeouts, keep-alive, per-tick byte *and* packet budgets with the overflow visible in `ConnectionStats` rather than as a mystery stall. **Time is passed in, never read** — no wall clock in the subsystem whose failures are hardest to reproduce, and a timeout the suite states rather than waits for. `Packet` is the wire format, and it refuses a wrong magic, an unknown version, a channel byte outside the enum, and a length that runs past the buffer; a version mismatch is refused rather than negotiated downward. Sequence comparison is wrap-aware, because a 16-bit counter wraps every eighteen minutes at sixty packets a second and a plain `>` breaks every long match. Unreliable is the default and the stale rule applies to it alone — a late reliable packet is a resend that still has to be delivered
- [_] the transports themselves — a loopback and an asio UDP socket, both driving `Link`. §16.6 has single-player ride the loopback with real encoding, so neither is a path the other skips
- [_] reliability — the acknowledgement window is carried and recorded; nothing resends against it yet
- [_] transport encryption — X25519 agreement and ChaCha20-Poly1305. Crypto++ is already vendored for it
- [_] join by full snapshot, then per-tick deltas — the snapshot format v0.2 already produces, so there is one serialisation path and a client that falls too far behind is re-snapshotted rather than repaired
- [_] deltas from `ChangeChannel`'s per-row dirty bits — the third reader of the bits v0.2 builds for `.Changed` and render invalidation
- [_] client-side prediction of the local player only, with everything else interpolated authoritative state. `PreviousTransform` and the render `Alpha` already exist and are what a correction smooths against
- [_] reconciliation — apply authoritative state into a running store, which is the v0.2 capability this depends on. Note that this needs no cross-machine determinism: the client drifting is expected, correcting the drift is the mechanism
- [_] client input submission, and the server-side rule that a replica may not write to a bus
- [_] interest management — what a client is sent at all. Deferred within this version if the first worlds are small enough that everything fits; say so explicitly rather than silently

## v0.4

Planned in [v02v03v04.md](v02v03v04.md) — written when this was v0.3, before replication took that slot.

- [_] `mono.engine/scene` at L7 — Basic Components: Transform, PreviousTransform, Bounds, Visual, Collider, RigidBody, Surface, Motion, Camera, QuickHash. Deletes the duplicated component definitions in `mono.client/Demo.hpp` and `mono.server/Simulation.cpp`; the C++ test scene itself stays until v0.5/v0.6 give it a game file to load
- [_] `scene::DrawInstance` — the draw-list payload v0.2's `ViewChannel` carries, at `shared` tier because a `server`-tier host writes it and a `client`-tier consumer reads it. Replaces `render::Instance` as the thing a world publishes; `mono.server` gains `scene` and `world` in its link row
- [_] `core/types`: AABB, Ray, RayHit — the value types v0.4 gives a consumer
- [_] `mono.engine/spatial` at L6 — uniform hash grid, and the optimised spatial query: raycast, overlap and shapecast with layer masks
- [_] basic physics collider — box, sphere and cylinder shapes, `Collider`/`RigidBody`/`Surface`, and the `SurfaceTable` resource the narrow phase reads once
- [_] basic physics pipeline — integrate, broadphase with sorted pairs, six exact analytic narrow-phase pairs, a serial sequential-impulse solver, contact events
- [_] Part — a class rather than a component: `{Transform, Bounds, Visual, Collider, Surface}`, with `PartDesc` and `MakePart` as the one place that decides what a part is
- [_] Camera — the `Camera` component, plus the `ActiveCamera` resource holding the live one and its resolved matrices

## v0.5

The engine has been full ECS since v0.2 and userland instancing is a façade over it — see [v02v03v04.md](v02v03v04.md). The test scene is still C++ at this point; v0.5 makes the façade reachable from script and v0.6 is where the scene actually moves.

- [_] bindings manifest for luau/typescript — generated from v0.2's `TypeDescriptor` property lists and class table, so there is no second source of truth for what a class is or what a property costs
- [_] plans for luau and typescript multi-threading and multi-processing systems (with locks, synchronise, etc, also plan integration with hytale setup for worlds) — over what v0.2 already established: the driver barrier, worlds-as-batch, `Ticket`/`Await`, `parallel/ipc` and `parallel/process`. The userland `thread` datatype is `parallel/threads/` and is a different contract from `Jobs`, because it has to survive a script yielding

## v0.6

Where the C++ test scene becomes a script. v0.4 rebuilt it on `scene`'s classes precisely so this is a port rather than a rewrite, and `Demo.hpp`/`Demo.cpp` go away here — `mono.client/AGENTS.md` has always said they die when there is a game file to load a scene from.

- [_] luau and typescript — `Instance.new`, properties, `.Changed`, the hierarchy and `:IsA` bind to v0.2's class table, the same one C++ calls. A calling convention, not a second mechanism
- [_] camera
- [_] surface camera (for mirrors, use previous frame as visual) — a view on v0.2's `ViewChannel` whose producer is the local world and whose consumer is a texture rather than the swapchain. The one-frame staleness is the latency that design already assumed
- [_] basic rendering pipeline (color, shadows, basic fov cull, sequential for many worlds) — the graph of `RENDER_PIPELINE.md`, its stages 1 to 7. Needs `mono.engine/graph/` at L9 first, and that needs v0.2's `ChangeChannel`
- [_] demo\_1world\_scene.luau (mirrors.luau, mirrors.ts) — the port of the C++ test scene, and the point where `D00001`'s `--script PATH` stops warning and starts loading
- [_] the demo dying is `D00004`'s trigger: ask whether anything still needs `core::Random`, and either close the item or say what renewed it

## v0.7

- [_] extended rendering pipeline (handle multiple worlds in parallel, handling gpu traffic) — `RENDER_PIPELINE.md` stages 8 to 12, including the HDR and G-buffer prerequisite its §17 opens with
- [_] demo\_2world\_scene.luau (mirrors.luau, mirrors.ts, split-screen for two worlds running in parallel via multi-process) — two `ViewChannel` producers and one compositor, which v0.2 already built. This demo is what proves the cross-process view path end to end

## v0.8

- [x] local filesystem content delivery network (cdn) — `mono.cdn` and `cdn::ContentRoot` landed early, at v0.2
- [x] cdn content addressing — `Engine::assets` at L8 `shared`: `ContentHash` (BLAKE3-256, checked against the published vectors rather than against itself), `Chunker` (gear rolling hash, FastCDC normalised two-mask cut, table generated from a stated seed so 256 frozen constants are reviewable as an algorithm), `HashTree` (Merkle with a tagged interior and the leaf count sealed into the root, so neither a subtree nor a duplicated node can pass as a whole tree), `Signature` (Ed25519 over a domain-separated message, one signature at the root and none below it) and `Manifest` (the four-level tree, canonical order, byte-stable output, and a reader that recomputes every root rather than believing it). BLAKE3 vendored. `CDN.md` §2
- [x] cdn grants — `assets::Grant`: an HMAC-SHA256 token naming bundle roots, a session, an absolute expiry and a byte budget, plus `cdn::Gate` as the origin's whole authorisation surface. The MAC is checked before any field is acted on and compared in constant time; forged, expired and out-of-scope are counted apart so an alarm is not buried in ordinary events; a grant names content hashes and has no field a path could occupy; `nowSeconds` is passed in so neither end holds a clock of its own to drift. `CDN.md` §4
- [x] cdn group assembly — `cdn::Grouper`: self-sufficiency first (an affinity is never split, even past the ceiling, because half a scene makes nothing appear — and the oversized group is counted rather than hidden), then a size mix that falls out of largest-first packing rather than needing a rule of its own, then priority. Deterministic, because two origins that group the same content differently cache different bundles and nothing reports that they stopped sharing. `CDN.md` §5
- [x] cdn group compression — `cdn::GroupCodec` and `cdn::Dictionary` over vendored Zstd: per group and never per file or per manifest, chunks left uncompressed at rest so dedup works on them, dictionaries trained per build and content-addressed so they version like everything else. The content checksum is turned on because Zstd leaves it off and a flipped byte otherwise decompresses cleanly to the wrong content; a decompression buffer is sized from the signed manifest and never from the frame, which is what stops a kilobyte declaring a gigabyte. Zstd vendored under BSD-3-Clause, legacy frame support off as attack surface we cannot need. `CDN.md` §5
- [x] cdn prepared-group cache and cancellation — `cdn::PreparedCache`, keyed by bundle root **and** dictionary hash because a group compressed against one dictionary is a different artefact from the same group compressed against another. LRU, bounded, thread-safe, and frames handed out as shared ownership so an eviction cannot free bytes underneath a client still streaming them. Cancellation is load-bearing: a cancelled request costs no compression, and one cancelled mid-preparation has its result discarded rather than delivered to nobody
- [x] cdn as a cache server — `CDNSettings` is the whole of an origin's setup, and CDN.md §6's three sources are field combinations rather than three programs: local store (`AllowUpstream` off), cache server (local first, forward on a miss, keep what came back) and pure proxy (`LocalFirst` off, `CacheUpstream` off). Local content is checked before anything external, which is what makes a hit cost no network at all. What an upstream returns is verified against the **signed** manifest before it is cached or served — a length check, honestly not chunk-level, because the chunk layout inside a group is not designed yet; the client verifies end to end regardless, so this is defence in depth. Upstream attempts are bounded, and an upstream that is down is counted apart from one serving wrong content
- [_] cdn wire streaming — parallel group streams over a real connection. Needs `Engine::net`'s transport: `Link` and the framing exist, the loopback and UDP sockets do not. `CDN.md` §5
- [x] cdn async pipeline — `cdn::Origin`: submit, pump, take, cancel. A publication is immutable and publishing is an **atomic swap**, so a request already admitted is served against what it was admitted against whatever is published underneath — pinned by a test that swaps mid-request. Concurrency is per request; `Jobs::For` appears in exactly one place, compressing a known set of groups, because that is CPU work with a known end and the pipeline around it waits on a filesystem. Cache lookups happen before the fan-out so a hit costs no compression. A bundle with no payload is refused rather than served empty. `CDN.md` §3
- [_] the client half of the async pipeline — a fetch issued from a world lives inside a tick, so its completion applies at the barrier; a chunk visible mid-tick is a desync. Belongs to `Engine::assets` and the world driver rather than to `mono.cdn`. `CDN.md` §3
- [_] mesh importing, baking and rendering pipeline
- [_] texture importing, baking and rendering
- [_] importing node pipeline (input nodes, processing nodes, export nodes)
- [_] auto export to assets folder
- [_] MeshPart + SurfaceAppearance (but as components) + Tagging => Render pipeline capabilities for filtering tagged objects for redirected pipeline work
- [_] put infront mirrors to see if it works with texture rendering
- [_] scripts that create MeshPart and set mesh properties (surfaceappearance equivalent but as components).

## v0.9

- [_] CurrentCamera
- [_] basic character controls
- [_] basic controls (enable/disable/shift-lock)
- [_] basic camera and controls (zoom, pan, control camera via script)
- [_] UserInputService and ContextActionService

## v0.10

- [_] ...

## v0.11

- [_] ...

## v0.12

---


