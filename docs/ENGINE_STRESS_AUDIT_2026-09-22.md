# Engine stress audit, 2026-09-22

This audit covers every `mono.engine` module, the CDN, and real connected player movement. Source locations below are relative to the repository root. An opportunity is a change to test, not a measured speedup. Preserve each module's `AGENTS.md` invariants and use an A/B benchmark with replay or output parity before changing an algorithm.

## Method and limits

- Host: AMD Ryzen 9 9900X, 12 cores and 24 hardware threads, 123 GiB RAM. Benchmark samples used the optimized `bench` preset.
- The existing benchmark catalog has 63 suite files and 628 declared benchmark rows across the engine and CDN. An isolated, optimized `benchrunner --all --samples 2` completed 69 discovered suites and 763 measured rows across the repository. The new input suite was built and run separately because it was absent from the clean base commit. Twenty-two of the 31 engine modules have a benchmark suite after that addition. The missing nine are `bakegraph`, `control`, `datastore`, `examples`, `msl`, `resources`, `script`, `scriptjs`, and `ui`.
- Focused world, physics, and parallel runs completed with three samples. Asset and CDN suites completed with three samples while unrelated Ninja builds were active. Their figures indicate workload scale and cannot be used as comparison baselines.
- The main working tree had independent, uncommitted changes during this audit. Its release server build failed in `mono.engine/replication/include/engine/replication/Replica.hpp` before the 200-client test could start. The isolated load run below used commit `767dc4cde0562520ae2b01d6a968b5bf951c55e6` plus only the new loadtest harness patch. No independent changes were reverted or committed here.
- The full benchmark catalog completed, including render and Studio suite binaries, but no live graphical scene or GPU output was visually checked. A benchmark missing from a module is an explicit coverage gap.
- The isolated release server and full optimized benchmark targets built successfully. The loadtest suite passed 95 assertions in 20 cases and the input suite passed 115 assertions in 8 cases. The new four-row input benchmark built and completed three samples per row.

## Measurements

| Workload | Observed figure | Interpretation |
|---|---:|---|
| 2 worlds, 100,000 entities each | parallel 14.95 us; serial 15.48 us | Near the crossover, no useful win demonstrated. |
| 4 worlds, 100,000 entities each | parallel 32.02 us; serial 31.15 us | Dispatch lost on this fixture. |
| 200 quiet worlds | serial 103.12 us; suspended 10.29 us | World scheduling overhead is visible without active simulation. |
| 4,000 physics bodies, spread | 0.832 ms/tick | Sparse-contact reference. |
| 4,000 physics bodies, pile | 39.19 ms/tick | Dense contact solver is the clearest measured pressure point. |
| Broadphase, 4,000 bodies | sync and pairs 321.03 us; pairs only 274.00 us | Pair generation dominates this fixture. |
| Empty parallel dispatch, 128 ranges | 39.66 us | Sets a meaningful floor for tiny parallel tasks. |
| Asset same-size resize, 2048 by 2048 | 43.41 ms | Exploratory only; concurrent builds were not excluded. |
| CDN grouping, 50,000 loose assets | 40.78 ms | Exploratory only; sample spread was about 57%. |
| CDN admission, 1,024 bundle scopes | 28.21 us/request | Exploratory only; concurrent build contamination. |

The physics `16k woken every tick` row had extreme variance and is excluded from conclusions. No speedup claim is made from any candidate below.

### Connected random movement

`scripts/stress-test.sh .cache/build/release random-motion-200-clean 200 45 45318 Stress.luau 30 1 30` ran against the isolated `-O3` release server after our build and benchmark jobs completed. The seed was 1 and headings changed every 30 submitted input ticks. All 200 sessions were admitted and in Playing at the end, with none still streaming; the harness sent 196,218 inputs and applied 1,249,544 deltas. Admission p99 was 0.133 seconds and join p95 was 17.683 seconds. The server processed 557 ticks over 57.02 seconds: tick p50 117.042 ms, p95 150.539 ms, p99 153.824 ms, with 484 overruns at a 30 Hz target. The harness counted 162,980 lost packets. The server dropped 29,056 profiling scopes, so the flame graph is incomplete. Its largest recorded self-time scopes were `Authority::Interest` (32.42%), `Authority::RecoverRows` (28.78%), `Authority::Score` (23.01%), and `Authority::Refine` (5.76%); these shares describe retained scopes, including reported work, rather than complete frame time. No before/after speedup is claimed.

An earlier run with the same seed overlapped a benchmark build, so its tick percentiles are excluded from the reference above. The Playing, input, and delta counters establish that the replay exercised 200 active character sessions. The clean replay had no known concurrent build at start, but one run is still insufficient for a regression threshold.

## Optimization opportunities by module

### `core`

1. Batch or shard metric writes after measuring lock contention; `mono.engine/core/src/Metrics.cpp:166` takes a process-wide lock on each count or observation.
2. Index registered metric rows by interned ID if cardinality grows; three lookup paths scan vectors in `Metrics.cpp:59`.
3. Reuse percentile scratch or select only requested ranks; `Metrics.cpp:143` copies and sorts samples on a report read.
4. Reuse per-name frame-graph percentile scratch; `mono.engine/core/src/FrameGraph.cpp:1016` and `:1130` build and sort sample vectors.
5. Test a thread-local cache for repeated literal `Name` construction; `mono.engine/core/src/Name.cpp:78` hashes under a shared registry lock. Keep string identity stable.

### `parallel`

The module policy says the worker pool is never destroyed (`mono.engine/parallel/AGENTS.md:221`), but `Jobs::Stop` resets its owned pool and `Pool::~Pool` exists (`mono.engine/parallel/src/Jobs.cpp:289`, `:347`). The current shutdown joins workers first. This is a policy and implementation mismatch to resolve before changing the pool lifecycle.

1. Reduce mutex traffic in the dispatch handshake; each woken worker takes `Pool::Guard` on entry and exit at `mono.engine/parallel/src/Jobs.cpp:237`, and dispatch waits for retirement at `:436`. The 128-empty-range row took 39.66 us. Preserve slot lifetime and lost-wakeup protection.
2. Wake only workers assigned a pinned task; `Jobs.cpp:581` uses `notify_all` for `ForWorkers`, so unassigned workers also enter the guard path. Keep exact worker placement.
3. Precompute each pinned worker's task span or index list; `Jobs.cpp:186` makes every worker scan all tasks and skip those assigned elsewhere. Preserve per-worker order.
4. Recycle bounded local-channel frame buffers; `mono.engine/parallel/src/Channel.cpp:74` allocates a vector per send, and `:88` copies then destroys it on receive. Keep copy semantics and bounded backpressure.
5. Maintain complete-frame counts and bytes incrementally; `mono.engine/parallel/src/SocketChannel.cpp:92` reparses inbound frames on each `Pending` read. Preserve partial-frame and closed-channel behavior.

### `ecs`

1. Give registered systems direct timing slots; `mono.engine/ecs/src/Scheduler.cpp:277` linearly searches prior timing rows on each system completion.
2. Retain per-wave timing scratch; `Scheduler.cpp:329` constructs a vector on each tick.
3. Cache canonical query terms for repeated call sites; `mono.engine/ecs/src/Store.cpp:593` copies and sorts terms, and `:522` resolves positions.
4. Measure `ArchetypeEdges` linear lookup under many component combinations, then test a deterministic sorted edge index if it crosses over; `mono.engine/ecs/src/ArchetypeEdges.cpp:5`.
5. Reuse snapshot sorting buffers when frequent replication snapshots justify it; `mono.engine/ecs/src/Snapshot.cpp:78` gathers and sorts resources and names.

### `world`

1. Tune the world parallel floor against the measured 2- and 4-world crossover; `mono.engine/world/src/Universe.cpp:870` selects serial or parallel execution.
2. Balance lanes by retained measured world cost rather than world count if asymmetric workloads confirm a gain; `Universe.cpp:96` currently distributes counts. Keep deterministic assignment.
3. Recompute lane maps only when pool or world membership changes; `Universe.cpp:790` refreshes every tick and `:96` scans and allocates scratch.
4. Retain bus merge scratch and merge ordered sender outboxes without re-sorting all messages; `mono.engine/world/src/BusRouter.cpp:427`, `:472`, `:510`. Preserve sender and sequence order.
5. Add a name-to-world index inside the directory while keeping string names on boundaries; `BusRouter.cpp:21` linearly scans worlds for routes.

### `collision`

1. Test a local-neighbour weld key that avoids 27 hash lookups per point; `mono.engine/collision/src/ConvexHull.cpp:176`. Preserve weld tolerance and point order.
2. Group coplanar live facets with an index instead of scanning all later facets for each seed; `ConvexHull.cpp:366`.
3. Index reverse edges deterministically in `EmitFaces`; `ConvexHull.cpp:397` uses linear search.
4. Index directed horizon edges during each Quickhull expansion; `ConvexHull.cpp:579` scans visible facets per edge.
5. Benchmark large triangle soups with alternative deterministic BVH split and leaf policies; `mono.engine/collision/src/TriangleMesh.cpp:125`. The existing centroid cache and `nth_element` should remain the baseline.

### `spatial`

1. Use a cheap hierarchy precheck to avoid parallel classification followed by serial fallback on a newly promoted hash-grid scene; `mono.engine/spatial/src/HashGrid.cpp:239`.
2. Compare full dynamic-BVH rebuild sort strategies under churn; `mono.engine/spatial/src/DynamicBvh.cpp:288`.
3. Measure dense pair sort and dedup; `DynamicBvh.cpp:509` and `:570` sort candidate pairs.
4. Reuse chunk membership order when topology is stable; `mono.engine/spatial/src/ChunkMap.cpp:88` sorts placements.
5. Test retained cell keys to avoid duplicate hashing in grid histogram and fill; `HashGrid.cpp:627` and `:690`. Keep deterministic query results and read-only queries.

### `scene`

1. Cache skeleton slot ordering and scratch across stable rigs; `mono.engine/scene/src/Skinning.cpp:60` sorts joints and builds vectors.
2. Measure visibility fingerprint scans and tree walks on large static hierarchies, then gate them by complete ECS change revisions if valid; `mono.engine/scene/src/Visibility.cpp:79`, `:180`, `:298`.
3. Skip transparent re-sort only when camera and transform stamps prove the order unchanged; `mono.engine/scene/src/DrawInstance.cpp:209`.
4. Reuse per-seam rig-fit scratch for portal scenes; `mono.engine/scene/src/SurfaceCameras.cpp:2258` and `:2511`.
5. Cache surface slot and aim order until authored inputs change; `SurfaceCameras.cpp:1722` and `:2057`. Preserve deterministic visibility.

### `gui`

1. Reuse collector handle scratch across layout calls; `mono.engine/gui/src/Layout.cpp:2262` creates it each frame.
2. Cache collector order until display order or structure changes; `Layout.cpp:2265` sorts it again.
3. Reuse table row and cell scratch rather than allocate nested vectors each layout; `Layout.cpp:1547`.
4. Cache stable child ordering within list, grid, and page containers; `Layout.cpp:1094` and `:1535` sort during placement.
5. Test generation stamps against the full `Resolved` flag sweep at `Layout.cpp:2240`, but retain exact orphan and detach behavior.

### `assets`

1. Add a no-op or copy fast path for same-size `ResizeImage` after pixel parity checks; `mono.engine/assets/src/Resample.cpp:39` currently resamples every pixel, and the exploratory 2048-square case took 43.41 ms.
2. Avoid the full base texture copy when building mip chains; `Resample.cpp:134` copies the initial `TextureData`.
3. Batch manifest construction rather than repeated sorted inserts and root-index shifts; `mono.engine/assets/src/Manifest.cpp:139` and `:288`.
4. Stream verified chunks into the final asset buffer instead of holding per-chunk and whole-asset copies at once; `mono.engine/assets/src/ChunkStore.cpp:152` and `:190`.
5. Size an import buffer from the checked file length and bulk-read instead of per-byte stream iteration; `mono.engine/assets/src/LocalStore.cpp:146`.

### `physics`

1. Target deterministic pair generation first: the 4,000-body pair stage took 274 of 321 us; test retained output or less pair sorting at `mono.engine/physics/src/BroadPhase.cpp:336`.
2. Reduce repeated parallel dispatch for dense contacts when tasks fall under the measured dispatch floor; `mono.engine/physics/src/NarrowPhase.cpp:346` and `Solve.cpp:1489`, `:1597`, `:1626`.
3. Reuse a dense or open-address body-index table rather than clear and refill an `unordered_map` each tick; `Solve.cpp:617`.
4. Gate stable solver topology validation by an exact revision instead of rebuild-and-compare; `Solve.cpp:882`. Preserve manifold order and replay.
5. Route authored moving static geometry to the kinematic index where semantics permit; `mono.engine/physics/src/SyncBroadphase.cpp:420` rebuilds static motion much more expensively than the kinematic fixture.

### `effects`

1. Tune particle age and spawn dispatch floors by live particle count, not block count alone; `mono.engine/effects/src/ParticleSystem.cpp:1390`, `:1542`, `:1773` carry unmeasured thresholds.
2. Keep active block indices if large scenes contain many empty allocated blocks; the spawn planner scans every block at `ParticleSystem.cpp:1685`.
3. Measure frequent enable toggles and replace linear retiring-block erase if hot; `ParticleSystem.cpp:1075`.
4. Separate camera-independent ribbon geometry from eye-facing beam and trail rebuilds if static visuals dominate; `mono.engine/effects/src/Ribbon.cpp:320`.
5. Reserve portal-projected ribbon output from run counts; `Ribbon.cpp:475` grows a fresh result on large crossings.

### `graph`

1. Cache per-instance world bounds by transform and bounds revision across multi-view culling; `mono.engine/graph/src/Cull.cpp:13` recomputes them.
2. Maintain pane-to-instance membership for repeated surface views; `Cull.cpp:108` scans the full draw list per view.
3. Tune shadow cull dispatch threshold and chunk size at 1, 2, 4, and 8 views; `mono.engine/graph/src/Shadow.cpp:94`.
4. Reuse ping-pong scratch rather than copy a full source for an in-place entity node; `mono.engine/graph/src/EntityFlow.cpp:279`.
5. Cache dependency order until graph mutation; `mono.engine/graph/src/Execution.cpp:199` rebuilds incoming and outgoing vectors.

### `game`

1. Index inspected archive entries by path for repeated `FindEntry`; `mono.engine/game/src/Project.cpp:710` scans linearly.
2. Reuse bounded filename scratch across ZIP entries; `Project.cpp:607` allocates per entry.
3. Verify each distinct content chunk once when assets share hashes; `Project.cpp:1289` reads each asset chunk separately.
4. Sort archive entry indices or pointers for extraction instead of copying whole records; `Project.cpp:1578`.
5. Accept a strided vertex view or batch extraction to remove an extra full mesh-position copy before hull bake; `mono.engine/game/src/CollisionContent.cpp:27` and `:97`.

### `bake`

1. Avoid copying the entire mesh or texture payload before every graph node. Use immutable shared input and copy only for mutation, or move a sole consumer; `mono.engine/bake/src/Graph.cpp:183` and `:382`. Preserve graph fan-out.
2. Queue ready nodes rather than scan all nodes repeatedly in large authored graphs; `Graph.cpp:465`. This is low priority while graphs remain small.
3. Index glTF embedded texture and name dedup within an import; `mono.engine/bake/src/Gltf.cpp:825` uses repeated searches per submesh.
4. Reserve validated total PNG IDAT length or stream into inflate to avoid repeated vector growth; `mono.engine/bake/src/Png.cpp:190` and `:279`. Preserve CRC and exact-length refusal.
5. Test an incremental active-edge table for SVG rasterization; `mono.engine/bake/src/Svg.cpp:1288` rebuilds and sorts crossings for each subpixel row. Require pixel-identical output.

### `bakegraph`

1. Use `lower_bound` for `PipelineSet::Find`, whose order is sorted but lookup is linear; `mono.engine/bakegraph/src/Document.cpp:452`.
2. Combine replacement and insertion lookup into one `lower_bound` in `PipelineSet::Set`; `Document.cpp:432`.
3. Use the same sorted lookup for `Remove`; `Document.cpp:464` scans and then shifts vectors.
4. Write paired names and documents directly instead of calling `Find` for every name; `Document.cpp:481` is quadratic in pipeline count.
5. Batch parsed pipeline insertion and sort once on `Read` if large documents justify it; `Document.cpp:494` flushes through repeated `Set`. Preserve document order and format.

### `delivery`

1. Index request IDs with stable storage; `mono.engine/delivery/src/Client.cpp:237` linearly scans requests and erases from a vector for state, take, and cancel.
2. Track queued bundle roots rather than scan all jobs for each new request; `Client.cpp:622`.
3. Maintain bundle waiter counts or an index rather than repeatedly scan all requests; `Client.cpp:820` and `:884`.
4. Deliver by root lookup and share or move one immutable verified payload instead of nested request-by-delivered matching and per-waiter copies; `Client.cpp:900`.
5. Slice all members sequentially through one manifest-owned iterator rather than call `SliceOf` for each member; `Client.cpp:849` currently creates a quadratic bundle split. Keep one authoritative layout definition.

### `net`

1. Reuse bounded buffers for reliable QUIC framed messages; `mono.engine/net/src/quic/Connection.cpp:1218` constructs a vector per send.
2. Reuse bounded buffers for unreliable frames; `Connection.cpp:1257` follows the same allocation path.
3. Keep a next-unsent chunk cursor rather than restart each packet scan from the base; `Connection.cpp:1349`.
4. Cache the converted ASIO endpoint per stable peer instead of calling `ToAsio` for each UDP send; `mono.engine/net/src/UdpTransport.cpp:162`.
5. Measure a borrowed receive span or batch API against scratch-to-output datagram copies; `UdpTransport.cpp:194`. Keep packet lifetime and authentication explicit.

### `replication`

1. Maintain created, forgotten, and destroyed membership incrementally per client; `mono.engine/replication/src/Authority.cpp:1993` scans full visible and known sets each publication.
2. Retain visibility results when hierarchy and ownership revisions prove no change; `Authority.cpp:1960` selects visibility per client after `mono.server/src/Server.cpp:2239` surveys the full world.
3. Serialize candidate component values only after byte-budget selection where exact wire priority permits it; `Authority.cpp:1734`, `:1865`, and `:2060` currently serialize candidates that may not fit.
4. Compare regular delta transport with separate per-player motion messages; `mono.server/src/Server.cpp:3248` emits a dedicated correction path. Preserve prediction and authority semantics.
5. Tune join snapshot capture and stream quotas against actual link budget; `mono.engine/replication/include/engine/replication/Authority.hpp:50` and `Authority.cpp:2275` bound captures and chunks per tick.

### `script`

1. Index profiler tree nodes by parent, source, function, and line; `mono.engine/script/src/Runtime.cpp:82` scans up to 4,096 nodes per sample.
2. Map active VM threads to stable slots rather than search the active list on begin, sample, end, and binding events; `Runtime.cpp:54`, `:138`, and `:200`.
3. Index native binding profile rows by name within a node; `Runtime.cpp:194` scans a leaf's bindings on each call.
4. Bulk-merge started script IDs or use hash membership plus stable order on initial load; `Runtime.cpp:319` and `:370` insert into a sorted vector repeatedly.
5. Skip the full Lua source-container walk when a complete structural and source generation says nothing changed; `mono.engine/script/src/SourceCache.cpp:265` currently mirrors every beat.

### `scriptjs`

1. Capture stable property descriptors in getter and setter closures instead of converting a name and resolving the descriptor on every access; `mono.engine/scriptjs/src/JsBindings.cpp:440`, `:476`, and `:554`. Validate schema lifetime.
2. Cache parsed source maps by path and file identity during repeated error reporting; `mono.engine/scriptjs/src/SourceMap.cpp:164` and `:260` load them per frame.
3. Cache editor completion surface enumeration until globals change; `mono.engine/scriptjs/src/JavaScriptRuntime.cpp:681` walks globals and members each request.
4. Benchmark borrowed host string conversion at the property-write boundary to remove duplicate string construction; `JsBindings.cpp:497`.
5. Hoist the QuickJS runtime pointer outside the bounded microtask drain; `JavaScriptRuntime.cpp:369` retrieves it each iteration. This is a low-impact candidate.

### `scriptluau`

1. Cache class and field descriptors for instance index and newindex with schema invalidation; `mono.engine/scriptluau/src/LuauInstances.cpp:315` and `:407` scan class lists.
2. Cache compiled ModuleScript bytecode by source hash and compile options across worlds, while keeping evaluated return values per runtime; `mono.engine/scriptluau/src/LuauRuntime.cpp:845` and `:940` compile anew.
3. Keep an active-module set beside the ordered loading stack for cycle checks; `LuauRuntime.cpp:853` and `:907` search linearly.
4. Cache completion surfaces until global writes invalidate them; `LuauRuntime.cpp:1302` reconstructs names and members per request.
5. Reuse teleport and settings event scratch after each pump while preserving reentrant queue isolation and ordering; `LuauRuntime.cpp:1093`, `:1160`, and `:1225` swap temporary vectors.

### `input`

1. Iterate changed keys instead of all `KeyboardCount` keys in Luau and JavaScript input pumps; `mono.engine/script/src/LuauInput.cpp:379` and `JsInput.cpp:350` scan full state each frame.
2. Iterate changed controller slots and buttons rather than all eight slots for one changed axis; `LuauInput.cpp:402` and `JsInput.cpp:370`.
3. Index listeners by input property while retaining safe mutation during callbacks; `LuauInput.cpp:186` and `mono.engine/script/src/Signals.cpp:96` scan or copy connections.
4. Index actions by bound key while preserving priority and mid-callback bind semantics; `mono.engine/script/src/Actions.cpp:167` scans each action and its keys per edge.
5. Measure per-world controller-state copies and frame rolling, then update changed slots only where world count makes it material; `mono.client/src/Client.cpp:2172` and `mono.engine/input/src/Translate.cpp:274`.

### `audio`

1. Keep command scheduling bounded and allocation-free on the device callback; `mono.engine/audio/src/Mixer.cpp:357` drains and stable-sorts due commands. Test fixed-capacity stable ordering under bursts.
2. Replace full node-order comparison per segment with an exact graph revision; `Mixer.cpp:43`, `:405`, and `:433` compare order even when routing is unchanged.
3. Compile node and input slot indices on graph changes; `Mixer.cpp:170`, `:250`, and `:320` resolve nodes and slots during each segment.
4. Hoist channel mapping and use bounded direct spans in the player loop; `Mixer.cpp:217` asks `SampleBuffer::Frame` and `Format` per output frame.
5. Combine preclip peak measurement and final clamp into one output pass; `Mixer.cpp:438` and `:449` scan samples twice. Preserve peak-before-clipping semantics.

### `render`

1. Use a frame-local entity-ID set for viewport duplicate detection; `mono.engine/render/src/ViewportFrames.cpp:74` scans prior entries per command.
2. Reuse viewport frame vectors and descendant rows where dependency signatures prove validity; `ViewportFrames.cpp:64` and `:109` allocate and rescan.
3. Replace the steady-frame full dirty-flag clear with a generation or dirty-slot list if upload profiles show it matters; `mono.engine/render/src/InstanceResidency.cpp:23` walks every entry.
4. Measure bitmap or incrementally ordered dirty ranges against full dirty-slot sort; `InstanceResidency.cpp:285` sorts all changed slots before uploads.
5. Index pending portal destinations by authenticated correlation and endpoints; `mono.engine/render/src/PortalTopologyHost.cpp:226` scans destinations per reply. Keep deadline and origin checks.

### `ui`

1. Compare full draw-geometry signature cost against saved upload cost; `mono.engine/ui/src/Interface.cpp:43` and `:331` hash all vertices, indices, and commands each frame. Test an exact incremental signature before changing it.
2. Retain rich-text per-piece widths across measure and draw walks; `mono.engine/ui/src/GuiPainter.cpp:416` and `:470` measure text twice.
3. Reuse or reserve rich-text piece scratch; `GuiPainter.cpp:391` allocates per draw command.
4. Precompute folded directory sort keys once per entry; `mono.engine/ui/src/Browse.cpp:115` lowercases both names during each comparison.
5. Cache visible modal path text and index selected paths in large file prompts; `mono.engine/ui/src/Prompts.cpp:310` constructs path strings and searches chosen entries per row.

### `control`

1. Cache serialized tool, resource, and prompt lists and invalidate on registration changes; `mono.engine/control/src/Surface.cpp:258` rebuilds discovery JSON per request.
2. Index tool names and resource URIs to stable rows while keeping the registry as the source of truth; `Surface.cpp:313`, `:392`, and `:445` search linearly.
3. Benchmark compact tool payload serialization for large answers; `Surface.cpp:52` and `:419` pretty-print a payload string that is serialized again in outer JSON.
4. Reuse bounded request storage or parse a bounded stream view; `mono.engine/control/src/Server.cpp:119` copies each incoming line. Keep one-client, caller-thread ordering.
5. Cache a validated staged-resource manifest if startup scans dominate; `mono.engine/control/src/Resources.cpp:120` recursively finds and sorts `AGENTS.md` files.

### `datastore`

1. Keep an adapter-owned SQLite connection and prepared load/save statements for repeated barrier operations; `mono.engine/datastore/src/Sqlite.cpp:116` and `:195` reopen and prepare each call.
2. Create the directory and schema once, with safe recovery after external database replacement; `Sqlite.cpp:188` repeats both on every save.
3. Measure a lifetime-safe `SQLITE_STATIC` bind against a full `SQLITE_TRANSIENT` image copy; `Sqlite.cpp:218` binds the complete encoded snapshot before a synchronous step.
4. Use the SQLite read-only open result to distinguish missing files instead of a separate filesystem metadata lookup; `Sqlite.cpp:107`.
5. Schedule multiple HTTP provider operations at the host barrier and pump them together if use cases justify it; `mono.engine/datastore/src/Http.cpp:138` synchronously polls one request and `:167` limits outstanding work to one. Never call a provider inside a world tick.

### Modules with fewer credible runtime candidates

- `examples`: `mono.engine/examples/src/Shooting.cpp:68` scans targets per shot; filter candidates through an existing broadphase and retain deterministic ties. `Shooting.cpp:16` creates an extra shot-packet copy. `Scene.cpp:46` and `:69` compute orbit and spin transforms per entity per tick. These three need a large demo-scene profile. The remaining code is example setup, not reusable engine runtime.
- `scripthost`: `mono.engine/scripthost/src/Runtime.cpp:22` may register effect components on each runtime construction, worth measuring only if it is not already guarded. The rest is a small factory and wrappers; its module policy explicitly keeps scripting work elsewhere.
- `msl`: `mono.engine/msl/src/Translate.cpp:52` constructs and sorts several resource-slot vectors per translation. A content-hash reuse cache may help repeated translation at `:157`, provided shader identity and options are exact. This platform-specific module was not executed on this Linux host.
- `resources`: `mono.engine/resources/src/Shaders.cpp:10` only joins and checks a staged shader path. Shader compilation and caching are owned by `render`, which already caches shader modules in `mono.engine/render/src/ShaderLibrary.cpp:157`. There is no credible hot path in this module.

Five independent speed opportunities do not exist in these four thin modules without inventing work or moving it across an explicit module boundary. They still count in the module coverage audit; none is presented as stress-tested.

## CDN and connected characters

The CDN is a separate program, outside `mono.engine`, and was explicitly requested:

1. Coalesce simultaneous cold requests for the same bundle before resolve and compression; `mono.cdn/src/Origin.cpp:273` checks cache before a batch, then `:291` can compress duplicate misses independently.
2. Prepare cold bundles asynchronously with bounded work so one compression does not stall the serial HTTP pump; `mono.cdn/src/Service.cpp:534` and `mono.engine/net/src/http/Server.cpp:97`.
3. Index request IDs and maintain a ready queue; `Origin.cpp:209`, `:251`, and `:389` repeatedly scan and erase a request vector.
4. Reduce full payload copying from prepared frame to response and then socket outbox; `Service.cpp:615` and `mono.engine/net/src/http/Server.cpp:312`.
5. Use narrower grants or a trusted verified-grant reuse boundary when a client requests many bundles; `mono.engine/assets/src/Grant.cpp:124` parses and authenticates every scoped root. Authentication must stay on every untrusted token.

The player-character workload spans server, scene, game, replication, and net. Its five leading experiments are:

1. Batch movement writes under one world entry per inbox; `mono.server/src/Server.cpp:2314` and `:2342` enter per input.
2. Use an incremental per-client visibility membership set instead of full scans each publication; `mono.engine/replication/src/Authority.cpp:1993`.
3. Skip world-wide visibility survey when structure and ownership are unchanged; `mono.server/src/Server.cpp:2239`.
4. Defer component serialization until candidates are selected under the byte budget; `Authority.cpp:1734` and `:2060`.
5. Measure combined correction and delta transport against the dedicated player-motion message; `Server.cpp:3248`.

The loadtest harness originally gave each client a fixed radial heading. The audit adds an opt-in deterministic seed and heading interval so clients turn independently and runs are repeatable. `Stress.luau` permits 512 players. `ReplicationStress.luau` is a separate 20,000-moving-part workload with a much smaller player cap. The run must report how many clients reached Playing; a requested connection count is not proof that hundreds of characters moved.

## Follow-up optimization pass

All 31 `mono.engine` modules were scanned again for a small, local improvement. The first changes were selected in assets, audio, bakegraph, core, and ui. The other modules either have a larger measured bottleneck listed above, have concurrent edits in the shared tree, or have too little runtime work for a credible local speedup. In particular, the existing `ForWorkers` million-row benchmark submits only 64 tasks, so changing its per-worker task scan needs a targeted dispatch measurement first.

| Change | Optimized preset observation | Verification |
|---|---|---|
| Assets, 2048 by 2048 same-size resize | 30.64 ms baseline, 0.39 ms after, on an isolated revision | Resample suite: 67 assertions in 11 cases. |
| Audio, one voice output mix | 2,225 ns baseline, 1,973 ns after | Mixer suite: 2,276 assertions in 29 cases. The 16 to 512 voice rows showed little or inconsistent difference. |
| Bakegraph, 4,096 pipeline lookup | 377 ns linear control, 268 ns binary control, 266 ns live lookup | Bakegraph suite: 8,403 assertions in 27 cases. Linear lookup was faster through 2,048 entries. |
| Core frame snapshot | One sort per sample distribution instead of repeated copies and sorts | FrameGraph suite: 294 assertions in 61 cases. No direct speed measurement yet. |
| UI directory browse | Fold each name once before sorting | Browse suite: 36 assertions in 10 cases. No direct speed measurement yet. |
| Replication priority and refinement, 2,000 entities, four changed rows, 32 clients | 2.774 ms per client baseline, 2.340 ms after, nine samples each | Replication suite: 22,810 assertions in 276 cases. Fixture asserts score and refinement hooks run before timing. |

The assets and audio comparisons used the same isolated source revision, build preset, and benchmark fixture before and after each edit. The bakegraph controls isolate search cost and do not return the same pointer type as the public API. The replication comparison used the same fixture and optimized preset with a source-only A/B swap. These numbers establish a fixture-level gain, not a whole-frame gain. Replication interest and other dense-contact physics work remain substantial targets that need parity checks and A/B measurements.

The stable-contact solver cache was tested and rejected. With the final cache implementation and a corrected churn fixture, seven-sample release A/B measured stable dense contacts at 1.243 ms baseline versus 1.210 ms cached, with spreads of 0.245 and 0.177 ms. Bridge churn measured 2.077 ms baseline versus 2.135 ms cached, with spreads of 0.353 and 0.374 ms. The overlapping variation and churn regression do not support carrying the extra topology state. The cache patch remains isolated and is not included in this pass.

### Continued pass

The committed release state at `7acb651f` ran 200 randomly turning clients for 45 seconds. All 200 reached Playing; tick p50 was 100.388 ms and p95 was 121.743 ms, with 567 overruns in 642 ticks. The test sent 205,548 inputs and applied 1,653,336 deltas. The profile dropped 36,984 scopes, so its percentages describe recorded self time only: interest 36.17%, recovery 24.69%, score 22.26%, and refinement 6.24%. This is a new reference, not an A/B attribution to one prior change.

An all-loose CDN grouping path reserves its exact cluster count and skips the affinity map. In two alternating optimized-preset comparisons, 50,000 loose assets took 9.21 ms baseline versus 8.06 ms with the path in the longer 11-sample round. Mixed-affinity rows stayed near their prior times. The focused CDN suite passed 56 assertions in 14 cases.

Two further candidates were tested and left out. Skipping zero-friction tangent work for speculative contacts passed its solver tests, but the Solve-inclusive 4,096-pair benchmark did not improve consistently across sequential 11-sample runs. Removing a duplicate liveness check from replication recovery passed its focused tests, but the dedicated recovery row was slower in two comparisons. The seven-sample sequential round measured 446.9 microseconds baseline versus 475.3 microseconds with the check removed. Neither patch is part of this pass.

The first interest-filter experiment combined the server's two sorted visibility lists but kept the per-entity predicate. Two 200-player release runs had tick p95 values of 156.40 and 117.97 ms, against baseline runs of 121.74 and 129.63 ms. Interest time per frame stayed similar. This experiment was rejected.

The accepted interest change passes the sorted replicated candidates to a batch selector once per client. The server builds one sorted visibility-exception list per publish and merge-walks it with those candidates, validating a client's player slot once per batch. The legacy predicate path keeps its original survey and selection work. In the same isolated `release` worktree, two 200-player random-motion runs with the batch path reached Playing for all 200 clients and had tick p95 values of 79.97 and 81.54 ms. A fresh baseline rebuild in that worktree reached all 200 and had tick p95 of 105.47 ms; the two earlier baselines were 121.74 and 129.63 ms. The three baseline and two batch runs used the same seed, 45-second load, 30 Hz input, and 30-tick heading changes. Recorded `Authority::Interest` self time was 369,271 ms across 689 baseline frames, versus 1,272 ms across 1,396 batch frames and 1,142 ms across 1,262 batch frames. These are summed worker spans, not elapsed wall time. The profiler dropped 43,496 baseline scopes and 96,118 and 85,218 batch scopes, so the captures are incomplete. The end-to-end tick percentiles give the stronger evidence of a repeatable gain. The final replication suite passed 22,847 assertions in 281 cases, including batch/legacy interest parity and serial/parallel publishing. The server replication suite passed 201 assertions in 14 cases with the batch path.

The extended server test exposed a separate existing visibility-consumer gap: both the legacy and batch hooks emit `Structure::Forgotten` when a public player child moves into a private container, but `Replica` deliberately retains forgotten entities and no client consumer currently removes them from the traversable store. The optimization preserves this protocol behavior; the dynamic-reparent assertion was not included in its passing test suite. A client-side forgotten-row policy needs its own design and verification.
