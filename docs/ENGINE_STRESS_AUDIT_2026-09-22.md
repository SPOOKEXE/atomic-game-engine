# Engine stress audit, 2026-09-22

This audit covers every `mono.engine` module, the CDN, and real connected player movement. Source locations below are relative to the repository root. An opportunity is a change to test, not a measured speedup. Preserve each module's `AGENTS.md` invariants and use an A/B benchmark with replay or output parity before changing an algorithm.

## Method and limits

- Host: AMD Ryzen 9 9900X, 12 cores and 24 hardware threads, 123 GiB RAM. Benchmark samples used the optimized `bench` preset.
- At the initial audit, the benchmark catalog had 63 suite files and 628 declared benchmark rows across the engine and CDN. An isolated, optimized `benchrunner --all --samples 2` completed 69 discovered suites and 763 measured rows across the repository. The new input suite was built and run separately because it was absent from the clean base commit. Twenty-two of the 31 engine modules had a benchmark suite after that addition. The missing nine at that point were `bakegraph`, `control`, `datastore`, `examples`, `msl`, `resources`, `script`, `scriptjs`, and `ui`; see the 2026-09-24 coverage update below for the current inventory.
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

1. Precompute source row and column ranges for each mip level, then compare them with the current integer-bound calculations. Keep exact pixel bytes for odd dimensions.
2. Batch manifest asset construction rather than repeated sorted inserts and root-index shifts; `mono.engine/assets/src/Manifest.cpp:139` and `:288`.
3. Stream verified chunks into the final asset buffer instead of holding per-chunk and whole-asset copies at once; `mono.engine/assets/src/ChunkStore.cpp:152` and `:190`.
4. Size an import buffer from the checked file length and bulk-read instead of per-byte stream iteration; `mono.engine/assets/src/LocalStore.cpp:146`.
5. Write the manifest signature and encoded body without first copying the body into a second full-file vector; `mono.engine/assets/src/ChunkStore.cpp:234`.

The same-size `ResizeImage` path is already implemented and measured. The optimized preset row moved from 30.64 ms to 0.39 ms on an isolated revision, with pixel parity covered by the Resample suite. A separate `BuildMipChain` copy candidate targeted each generated mip buffer: it copies the previous level's `Pixels` into `image.Mips`, not the full base `TextureData`. An 11-sample direct A/B under concurrent host builds was noisy and did not support keeping the change. Old versus new minima and ranges were 1.204/0.199 ms versus 1.285/0.459 ms at 512, 3.894/2.218 ms versus 5.608/1.705 ms at 1024, and 24.058/3.689 ms versus 20.512/4.982 ms at 2048. No mip-chain speedup is claimed, and the copy change was discarded.

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
| Assets, same-size resize at 2048 by 2048 | 30.64 ms baseline, 0.39 ms after, on an isolated revision | Resample suite: 67 assertions in 11 cases. |
| Audio, one voice output mix | 2,225 ns baseline, 1,973 ns after | Mixer suite: 2,276 assertions in 29 cases. The 16 to 512 voice rows showed little or inconsistent difference. |
| Bakegraph, 4,096 pipeline lookup | 377 ns linear control, 268 ns binary control, 266 ns live lookup | Bakegraph suite: 8,403 assertions in 27 cases. Linear lookup was faster through 2,048 entries. |
| Core frame snapshot | One sort per sample distribution instead of repeated copies and sorts | FrameGraph suite: 294 assertions in 61 cases. No direct speed measurement yet. |
| UI directory browse | Fold each name once before sorting | Browse suite: 36 assertions in 10 cases. No direct speed measurement yet. |
| Replication priority and refinement, 2,000 entities, four changed rows, 32 clients | 2.774 ms per client baseline, 2.340 ms after, nine samples each | Replication suite: 22,810 assertions in 276 cases. Fixture asserts score and refinement hooks run before timing. |

The assets and audio comparisons used the same isolated source revision, build preset, and benchmark fixture before and after each edit. The bakegraph controls isolate search cost and do not return the same pointer type as the public API. The replication comparison used the same fixture and optimized preset with a source-only A/B swap. These numbers establish a fixture-level gain, not a whole-frame gain. Replication interest and other dense-contact physics work remain substantial targets that need parity checks and A/B measurements.

The stable-contact solver cache was tested and rejected. With the final cache implementation and a corrected churn fixture, seven-sample release A/B measured stable dense contacts at 1.243 ms baseline versus 1.210 ms cached, with spreads of 0.245 and 0.177 ms. Bridge churn measured 2.077 ms baseline versus 2.135 ms cached, with spreads of 0.353 and 0.374 ms. The overlapping variation and churn regression do not support carrying the extra topology state. The cache patch remains isolated and is not included in this pass.

### Large lighting scene, 2026-09-24

The `release-tests` Vulkan `volume-light-stress 60` gate passed its authored scene,
volume resolver, light selection, and GPU volume cases. The 60-frame selection
measurements were 0.0263 ms per frame for 256 point lights, 0.0241 ms for 256
spot lights, 0.0540 ms for the mixed set, and 0.0073 ms for fog volume selection.
The GPU volume fixture reported 0.455 ms per frame. Its small render target and
sixteen-volume cap make that figure a separate workload from the full scene.

The `release` Vulkan client ran `LightingStress.luau` for 240 headless frames at
1280 by 720. It reported 176.5 average presented FPS and 62 simulation ticks;
the latter reached only 10.4 Hz during the run. A five-second frame-graph
capture recorded a 37.49 ms mean `gpu fog` span from 31 completed GPU samples.
The CPU frame median was 3.383 ms. The first render also spent 4.536 seconds in
shadow setup, which dominates the capture mean and must be separated from
steady-state cost. These figures establish fog shading as the measured large
scene pressure point, not light selection.

Caching each volume's camera-ray interval per pixel was tested as a shader
candidate, then rejected. A second five-second `release` Vulkan capture on the
same scene measured a 37.73 ms mean `gpu fog` span from 27 completed samples,
against 37.49 ms before. The small difference does not establish a gain. The
shader was restored. Further work needs a visual parity comparison and a
measured reduction in shadowed density sampling or active pixel work.

A temporary authored-scene experiment changed each volume from 16 to 4 shadow
steps without changing engine code. The `release` Vulkan profile measured 13.93
ms mean `gpu fog` over 60 completed samples, and a repeat of the original
16-step scene measured 39.29 ms over 27 samples. At simulation tick 20, the
1280 by 720 BMP captures had identical RGB pixels. A subsequent comparison of
all 29 shared simulation ticks in the two 60-frame captures found identical RGB
pixels at each tick. Visual inspection showed that the dense, overlapping fog
obscured the receiver field in those captures, so this equality does not prove
that four shadow steps preserve visible shadow detail. The checked-in stress
scene still uses 16 shadow steps. In a temporary visibility variant, the first
16 volumes used density 0.005 and the other 240 used density 0.03. Receivers
were visible, yet all five shared ticks still had identical RGB pixels with 16
and four shadow steps.
This is explained by the scene's 18.4 clock time: `LightingOf` sets the
directional term to zero below the horizon, while `volume.frag` still traces
the shadow rays. This identified an exact zero-contribution shader path to
remove and measure with the same scene.

That shader branch is now implemented. On the same `release` Vulkan client,
1280 by 720 scene, and dedicated Xvfb display, a ten-second baseline capture
recorded `gpu fog` mean 38.414 ms across 27 completed samples. The branch
recorded 4.785 ms across 186 completed samples, about an eightfold reduction
for this zero-directional-light scene. Both runs include a cold setup pause;
the reported fog spans are completed device timestamps, not the capture-wide
CPU frame mean. The checked-in night scene had identical RGB pixels on all
three shared simulation ticks in five-frame before and after sequences. A
temporary noon variant with visible receivers and nonzero direct light had
identical RGB pixels on all five shared ticks. These comparisons check that
the zero branch preserves the captured image and that the nonzero branch still
takes the original calculation; they do not cover every renderer backend.
After the shader change, `just volume-light-stress 60` passed its five focused
scene, selection, and Vulkan GPU suites, including 167 assertions in the GPU
volume case. `just shader-check` also passed the SPIR-V and MSL contract check.

### Continued pass

The committed release state at `7acb651f` ran 200 randomly turning clients for 45 seconds. All 200 reached Playing; tick p50 was 100.388 ms and p95 was 121.743 ms, with 567 overruns in 642 ticks. The test sent 205,548 inputs and applied 1,653,336 deltas. The profile dropped 36,984 scopes, so its percentages describe recorded self time only: interest 36.17%, recovery 24.69%, score 22.26%, and refinement 6.24%. This is a new reference, not an A/B attribution to one prior change.

An all-loose CDN grouping path reserves its exact cluster count and skips the affinity map. In two alternating optimized-preset comparisons, 50,000 loose assets took 9.21 ms baseline versus 8.06 ms with the path in the longer 11-sample round. Mixed-affinity rows stayed near their prior times. The focused CDN suite passed 56 assertions in 14 cases.

Two further candidates were tested and left out. Skipping zero-friction tangent work for speculative contacts passed its solver tests, but the Solve-inclusive 4,096-pair benchmark did not improve consistently across sequential 11-sample runs. Removing a duplicate liveness check from replication recovery passed its focused tests, but the dedicated recovery row was slower in two comparisons. The seven-sample sequential round measured 446.9 microseconds baseline versus 475.3 microseconds with the check removed. Neither patch is part of this pass.

The first interest-filter experiment combined the server's two sorted visibility lists but kept the per-entity predicate. Two 200-player release runs had tick p95 values of 156.40 and 117.97 ms, against baseline runs of 121.74 and 129.63 ms. Interest time per frame stayed similar. This experiment was rejected.

The accepted interest change passes the sorted replicated candidates to a batch selector once per client. The server builds one sorted visibility-exception list per publish and merge-walks it with those candidates, validating a client's player slot once per batch. The legacy predicate path keeps its original survey and selection work. In the same isolated `release` worktree, two 200-player random-motion runs with the batch path reached Playing for all 200 clients and had tick p95 values of 79.97 and 81.54 ms. A fresh baseline rebuild in that worktree reached all 200 and had tick p95 of 105.47 ms; the two earlier baselines were 121.74 and 129.63 ms. The three baseline and two batch runs used the same seed, 45-second load, 30 Hz input, and 30-tick heading changes. Recorded `Authority::Interest` self time was 369,271 ms across 689 baseline frames, versus 1,272 ms across 1,396 batch frames and 1,142 ms across 1,262 batch frames. These are summed worker spans, not elapsed wall time. The profiler dropped 43,496 baseline scopes and 96,118 and 85,218 batch scopes, so the captures are incomplete. The end-to-end tick percentiles give the stronger evidence of a repeatable gain. The final replication suite passed 22,847 assertions in 281 cases, including batch/legacy interest parity and serial/parallel publishing. The server replication suite passed 201 assertions in 14 cases with the batch path.

The extended server test exposed a separate existing visibility-consumer gap: both the legacy and batch hooks emit `Structure::Forgotten` when a public player child moves into a private container, but `Replica` deliberately retains forgotten entities and no client consumer currently removes them from the traversable store. The optimization preserves this protocol behavior; the dynamic-reparent assertion was not included in its passing test suite. A client-side forgotten-row policy needs its own design and verification.

### Remaining-module coverage closure, 2026-09-24

The earlier thin-module note was incomplete. The following candidates complete the
five-opportunity inventory for every engine module. They are hypotheses to measure,
not claims of an existing bottleneck. `bakegraph`, `control`, `datastore`, `script`,
`scriptjs`, and `ui` already have five candidates above. `bakegraph` also has an
optimized-preset 4,096-pipeline lookup measurement in the follow-up table. The
other modules below have no dedicated benchmark row, so no speed figure is claimed.

A fresh `just preset=bench bakegraph-pipeline-set-bench 5` run completed after
this review. At 4,096 pipelines it reported 385 ns per call for the linear
control, 266 ns for the text binary control, and 267 ns for
`PipelineSet::Find`, with spreads of 13 ns, 8 ns, and 21 ns. This confirms the
earlier lookup measurement on the current checkout. It does not measure parsing,
writing, or graph execution.

#### `examples`

1. Feed `NearestHit` a deterministic broadphase candidate span for large target sets; `mono.engine/examples/src/Shooting.cpp:72` tests every target per shot. Keep its equal-distance behavior and invalid-target refusal.
2. Replace the temporary `ByteWriter` result copy in `EncodeShot` with a fixed-size shot payload only after preserving the exact 36-byte wire encoding; `Shooting.cpp:16` through `:28` constructs a vector from another byte range.
3. Measure combined sine and cosine evaluation for large orbit scenes; `mono.engine/examples/src/Scene.cpp:54` through `:61` evaluates both for every orbit each tick. Preserve the deterministic clock and transform result.
4. Build the scene-library child-name index once while mounting, instead of calling `FindFirstChild` for each sorted directory; `Scene.cpp:120` through `:126`. The duplicate-name check must keep using the ECS tree as its authority.
5. Cache a directory listing only behind an explicit filesystem-generation or mtime check; `mono.engine/examples/src/DemosLoader.cpp:57` through `:78` walks and sorts on every list request. Startup discovery is likely the only credible workload.

#### `msl`

1. Reserve each resource-slot vector from the corresponding SPIRV-Cross group sizes before `Collect`; `mono.engine/msl/src/Translate.cpp:56` through `:81` grows five separate vectors.
2. Reserve the final texture and buffer vectors before appending storage slots; `Translate.cpp:56` through `:73` can otherwise reallocate and copy earlier slots.
3. Measure one stable partition followed by one sort against the current per-kind sorts, while retaining the required type order and set-binding order; `Translate.cpp:38` through `:73`.
4. Cache a successful translation by complete SPIR-V bytes and all translation options when repeated runtime compilation is demonstrated; `Translate.cpp:156`. The cache key must include every binding-affecting option and not outlive changed staged shader bytes.
5. Avoid constructing trace diagnostics unless `msl` trace logging is enabled, if profiling shows formatting material at shader-load time; `Translate.cpp:91` through `:151` reports every binding. Preserve the first separate-sampler warning.

#### `resources`

1. Replace the heap-backed suffix temporary with a `string_view` if path construction profiles show repeated calls matter; `mono.engine/resources/src/Shaders.cpp:14` currently creates `std::string` for either fixed suffix.
2. Build the final filename with one reserved string before converting to `std::filesystem::path`, if the platform path implementation makes the current nested construction allocate; `Shaders.cpp:25` through `:26` creates both a filename string and a path.
3. Measure caching the staged module root after process initialization; `Shaders.cpp:25` calls `core::Paths::Shaders` for every lookup. The cache is valid only if the process treats asset roots as immutable.
4. Make the trace argument lazy if disabled-log profiling shows `staged.string()` allocation is visible; `Shaders.cpp:31` converts every resolved path to text. Keep trace output identical when enabled.
5. Cache exact `(name, form)` path results at shader-library ownership only if repeated lookup dominates a measured reload workload. `Shader` must remain a pure path constructor and must not acquire global cache state or file-existence policy.

These five `resources` candidates all concern path construction. This module has no
device work, compilation, or file I/O, and the audit found no evidence that it is a
frame-time pressure point.

### Lighting stress release repeat, 2026-09-24

The current worktree passed `just volume-light-stress 120` after rebuilding the
release test targets. Its authored-scene, volume resolver, and renderer checks
passed 1,023 assertions. The 120-frame CPU measurements were 0.0220 ms per frame
for 256 point lights, 0.0251 ms for 256 spot lights, 0.0491 ms for their mixed
selection, and 0.00386 ms for 256 fog volumes. The Vulkan sixteen-volume fixture
measured 0.330 ms end to end per frame. These bounded fixtures show that local
light and volume selection are not the large-scene bottleneck.

A separate current `release` client capture used the checked-in `LightingStress`
scene at 1280 by 720, headless and uncapped, for 600 presented frames. It recorded
146 completed `gpu fog` timestamps: 4.655 ms mean, 4.562 ms p50, and 6.442 ms p99.
The earlier zero-directional-light after-capture recorded 4.785 ms mean over 186
timestamps. The two captures use different durations and a worktree containing
concurrent renderer edits, so the 2.7% difference is a repeatability check rather
than a speedup attribution. The new capture also contains one-time shadow setup
work, with a 533.138 ms maximum, and must not be summarized by its 4.039 ms
whole-run frame mean.

No additional lighting change was made in this pass. The concrete before-and-after
result remains the existing zero-directional-light fog branch, from 38.414 ms to
4.785 ms on its controlled capture. The new release repeat supports retaining that
result while keeping shadowed direct-light sampling and visible-image parity as the
next optimization gate.

### Broadphase pair-sort candidate, 2026-09-24

The optimized `bench` preset measured `Pairs only · 4000 colliders, 4m cells` at
281.54 us ±9% for the existing 11-bit radix digit. A bounded 16-bit digit
experiment reduces each 64-bit key from six passes to four. In alternating
11-sample runs on the same row, with concurrent builds on the host, the 11-bit
control measured 322.85 us ±29% and 327.75 us ±30%; the 16-bit candidate measured
651.18 us ±5% and 444.45 us ±14%. The second candidate spread overlaps the
controls, and neither round demonstrates a stable gain. The 16-bit change was
rejected and the 11-bit implementation remains. These fixture timings do not
claim a whole-engine gain.

The retained implementation's 24 physics suites were green, including broadphase
ordering (22 cases, 99 assertions) and contact event coverage (4 cases, 14
assertions). The optimized server produced byte-identical recordings across two
200-tick runs with 512 entities, and replay reproduced all 120 barriers in the
256-entity fixture. The next ranked physics candidate is reducing repeated
parallel dispatch for dense contacts below the measured 39.66 us empty-dispatch
floor. Any such path must preserve pair, manifold, and event order and pass the
same determinism and replay checks.

### Missing engine stress suites, 2026-09-24

The initial nine-module list is historical. At that inventory point, benchmark
suites covered 30 of 31 `mono.engine` modules. The two new modules added during
this goal bring the current total to 33, with suites in 32. `bakegraph` has
`engine.bakegraph.bench.pipeline-set`; `control` has
`engine.control.bench.mcp-control`. The following targeted suites close six
runtime gaps. All six optimized binaries built and ran with three samples. The
bench preset reconfigured after a glob mismatch and completed a 1,562-step
incremental build at `-j2`. A Barotrauma process was active during the suite
runs; the build also overlapped a portal product test and other host work.
Retain these values as functional evidence only, not as performance baselines.
The report's nanosecond and spread fields are normalized by each row's
iteration count; spread is the slowest sample minus the fastest sample.

| Module | Suite and workload | Coverage limit |
|---|---|---|
| `datastore` | `engine.datastore.bench.sqlite-snapshot`: replace and load a 1,024-entry SQLite snapshot with 256-byte values. | Measures local durable storage; does not model HTTP latency or remote provider contention. The temporary database is under `.cache/build/bench/benchmark-data/` and is removed at exit. |
| `examples` | `engine.examples.bench.motion`: full scheduler ticks over 128, 1,024, and 4,096 `Part` entities with the example orbit and spin systems installed. | Measures the C++ motion systems; does not time loading a staged authored scene or its VM startup. |
| `msl` | `engine.msl.bench.translate`: repeated SPIR-V to MSL translation using the four-resource fragment fixture shared with the unit suite. | Measures CPU translation on this host; there is no Metal compiler or device here to execute the result. |
| `script` | `engine.script.bench.source-mirror`: unchanged `MirrorSourcePrograms` passes over 64, 512, and 2,048 cached scripts. | Measures the neutral script source mirror; VM execution is covered separately. |
| `scriptjs` | `engine.scriptjs.bench.property-access`: QuickJS bound `Position` read and write cycles at 64, 256, and 1,024 operations. | Includes the short `Run` wrapper evaluation around a function compiled during setup; does not measure source-map parsing or sustained promise-job drains. |
| `ui` | `engine.ui.bench.headless-interface`: a 96-control ImGui frame through layout, draw-list generation, and geometry signature hashing. | Exercises the CPU frame path only; it omits backend upload, GPU draw, and presentation. |

The values below are the report's normalized nanoseconds and spread, with the
row unit and sample count shown. All rows used three samples.

| Suite | Row | ns | spread ns | unit |
|---|---|---:|---:|---|
| datastore | SQLite atomic snapshot replace, 1,024 entries x 256 bytes | 591,731 | 11,863 | call |
| datastore | SQLite snapshot load, 1,024 entries x 256 bytes | 207,650 | 11,682 | call |
| examples | example motion tick, 128 orbiting and spinning Parts | 32 | 0 | item |
| examples | example motion tick, 1,024 orbiting and spinning Parts | 29 | 0 | item |
| examples | example motion tick, 4,096 orbiting and spinning Parts | 28 | 8 | item |
| msl | SPIR-V translation of four-resource fragment | 1,849 | 31 | call |
| script | unchanged source mirror tick, 64 cached scripts | 51 | 0 | item |
| script | unchanged source mirror tick, 512 cached scripts | 50 | 2 | item |
| script | unchanged source mirror tick, 2,048 cached scripts | 50 | 0 | item |
| scriptjs | QuickJS bound `Position` read and write, 64 cycles | 625 | 40 | item |
| scriptjs | QuickJS bound `Position` read and write, 256 cycles | 438 | 58 | item |
| scriptjs | QuickJS bound `Position` read and write, 1,024 cycles | 401 | 17 | item |
| ui | headless UI frame and geometry signature hash, 96 controls | 457 | 2 | item |

The UI run reported that no preset fonts were staged under
`.cache/build/bench/bench/fonts` and used ImGui's built-in font. All six suite
processes exited successfully and emitted their expected rows.

`resources` remains the only engine module without a benchmark suite. Its runtime
work is path construction, while shader compilation and file access belong to
consumers. Its module invariant describes no device work or I/O, and the audit
found no frame-time evidence that would make a synthetic path benchmark useful.
The image graph CPU workloads added during this pass are measured below.

### Script source mirror path reuse, 2026-09-24

The ordinary mirror walk already receives each `LuaSourceContainer` from its
ECS query. It now reads that row's Luau path directly, while still consulting
the language selector and fetching `JavaScriptSourceContainer` for scripts
running JavaScript. This removes one redundant component lookup per Luau
script. The sourcecache test verifies that switching from `.luau` to `.ts`
updates the mirrored path and text while the source-cache generation stays
unchanged.

The optimized `bench` preset ran the existing source-mirror suite before and
after the change with 11 samples each. The report values are the fastest
normalized nanoseconds per item, and spread is slowest minus fastest:

| Cached scripts | Before ns/item (spread) | After ns/item (spread) |
|---:|---:|---:|
| 64 | 50 (0) | 36 (0) |
| 512 | 49 (0) | 34 (0) |
| 2,048 | 49 (1) | 34 (0) |

The paired run overlapped unrelated compiles, and Barotrauma was active during
the candidate run. Timing evidence is inconclusive and does not support a
performance claim. The focused sourcecache suite passed 50 assertions in 13
cases.

### New image graph module opportunities, 2026-09-24

`imagegraph` and `imagegraphio` were added after the earlier 31-module
inventory. Neither has a benchmark suite yet. The following source-based
opportunities are hypotheses to test, not claims of a measured bottleneck.

#### `imagegraph`

1. Measure validating and compiling the supplied plan on every evaluation; `mono.engine/imagegraph/src/Document.cpp:2684-87` and `:3875-82` rebuild and compare it. Any reuse must still reject a plan for a changed document.
2. Retain exact node indices and upstream adjacency in the compiled plan instead of rebuilding them for every output evaluation; `Document.cpp:2701-19` and `:3892-3909` build the node map and adjacency before walking reachability. Preserve output-specific reachable nodes.
3. Index effective links and resolved defaults by destination node and port; `Document.cpp:2740-44`, `:2780-84`, and `:2817-29` linearly scan the full lists while resolving each input. Keep missing-port and duplicate-input diagnostics unchanged.
4. Avoid deep-copying the entire document for each animated evaluation by applying evaluated keyframes through an exact overlay or touched-node copy; `Document.cpp:3911-23` builds tracks and then copies the document. Preserve source immutability and keyframe order.
5. Split Gaussian blur interior pixels from border pixels to avoid per-sample bounds checks for in-range taps; `mono.engine/imagegraph/src/PixelOpsBlur.hpp:55-80` checks each offset for every pixel. Preserve edge normalization, alpha handling, and gamma output byte-for-byte.

#### `imagegraphio`

1. Index links by source and destination before testing representability; `mono.engine/imagegraphio/src/PxcxImport.cpp:371-75` scans every link for each node at `:415`. Keep unsupported sockets opaque with the same diagnostics.
2. Index project-output links by destination instead of scanning all links for each output node; `PxcxImport.cpp:458-65` nests the output-node walk over the complete link list. Preserve the first matching input-zero link.
3. Defer building the opaque node type until a native mapping fails; `PxcxImport.cpp:413-29` first creates an opaque type, then successful node mappings replace it. Keep opaque source nodes byte-preserving.
4. Bind each node's input array and current-value records once during mapping; `PxcxImport.cpp:47-63` repeats `inputs`, `r`, and `d` lookups through helpers called for each authored control. Retain rejection of animation and linked controls.
5. Reserve the gradient key vector from the already checked JSON key count before appending; `PxcxImport.cpp:265-81` knows the count but grows `gradient.Keys` incrementally. Preserve increasing-time validation and exact order.
6. Measure repeated reference-preview requests before caching the thumbnail hash; `PxcxImport.cpp:14-21` recalculates FNV over all 256 by 256 RGBA bytes per call. Keep the preview span tied to `Source` and verify the cached hash against the source bytes.

### SQLite blob binding experiment, 2026-09-24

The save path binds the encoded image with `SQLITE_STATIC`. SQLite requires
that this buffer stay alive until the statement is finalized ([binding
contract](https://www.sqlite.org/c3ref/bind_blob.html)). `image` is declared
before the database and statement objects, so reverse destruction order
finalizes the statement before destroying the byte vector on success and error
returns. This meets SQLite's documented binding lifetime. The focused success
and atomic-replacement cases passed 11 assertions in 2 cases before the
failure-path test was added. The trigger-based rollback case checks that a
failed step preserves the prior snapshot. The focused SQLite suite now passes
18 assertions in 3 cases, including the failed replacement path.

The optimized `bench` preset ran two interleaved 11-sample rounds with the
existing `engine.datastore.bench.sqlite-snapshot` suite. Each pair used the
same 1,024-entry by 256-byte workload:

| Round | Bind lifetime | Save us (spread) | Load us (spread) |
|---|---|---:|---:|
| 1 | `SQLITE_TRANSIENT` | 552.78 (17%) | 198.09 (7%) |
| 1 | `SQLITE_STATIC` | 513.17 (36%) | 197.49 (9%) |
| 2 | `SQLITE_TRANSIENT` | 588.63 (7%) | 207.87 (11%) |
| 2 | `SQLITE_STATIC` | 610.29 (25%) | 213.26 (34%) |

The pre-run process check found no build, benchmark runner, or Barotrauma
process. The save candidate was faster in the first pair and slower in the
second, with broad sample spreads. These measurements do not support a speed
claim. The lifetime and rollback gates pass, but no optimization gain is
claimed.

### Image graph CPU benchmark fixtures, 2026-09-24

The image graph modules now have bounded CPU suites. Their preflight checks run
before measured iterations and compare repeated results. Both builds and Just
jobs passed. GDA was compiling on the host during the runs, so these numbers are
functional workload evidence, not quiet baselines.

| Module | Workload | Min ns/call | Spread ns | Samples | Coverage limit |
|---|---|---:|---:|---:|---|
| `imagegraph` | Evaluate a 256 by 256, three-octave simplex output | 59,788,331 | 449,814 | 5 | One synthetic noise node with a precompiled plan; does not cover Studio editing, PXCX import, or multi-node image pipelines. |
| `imagegraphio` | Project a synthetic chain of 256 opaque nodes, one mapped solid node, and 257 links while retaining parsed source bytes | 1,141,856 | 14,547 | 5 | Measures in-memory projection from a parsed archive; excludes file access, archive decoding, real Pixel Composer projects, and native pixel evaluation. |

The imagegraph unity build passed with a GCC `-Wmaybe-uninitialized` warning
in `Document.cpp:2482-2505` while checking optional source and target port
types. An explicit `if (!source || !target) return` precedes the reported
dereferences. This appears to be an optional-flow false-positive candidate;
the source was left unchanged during the benchmark work.

### Replication optimization parity gates, 2026-09-24

Two narrow replication changes now have focused parity gates. These tests show
that the changes preserve the exercised output; matched release measurements
are pending because host contention prevents a reliable A/B run.

The fixed-width recovery deferral gate, `test_replication
'[replication][priority][recovery]'`, passed 62 assertions in one focused case.
Under a constrained byte allowance it compares the exact outgoing packet bytes
and priority/budget order across two `Pack` passes. It also covers a refused
`Unsent` row and retry after the source changes. The fixture records six writer
calls for four transmitted rows across the two passes, matching the existing
flush decision. This is a parity and work-count bound, not a measured runtime
gain.

The class-id lookup gate passed `test_scene '[scene][services]'` with 236
assertions in 15 cases and `test_server '[server][replication]'` with 201
assertions in 14 cases. The scene fixture compares the helper's ordered
replicated result with the prior per-candidate lookup path before and after
subtree reparenting. The server hook resolves and captures the class id when
priority is configured, avoiding a registry lookup for each candidate. If
configuration has no valid id, it retains the prior lookup path. This gate
does not establish a connected-player performance gain.

No matched 200-client release rerun has been completed after these changes.
The connected run earlier in this audit is a baseline only and must not be
attributed to either change.

### Next independent engine-system candidate

The colored-dispatch probe did not establish a one-task trigger. The
`ConnectedLattice(64)` fixture selected the color schedule but had no wave with
at most 64 groups, so the proposed minimum-task change was not made.

The next measurement should use the existing 4,000-body pile in
`mono.engine/physics/benchmarks/Stepping.cpp`. It places 1m boxes in a 3m span
with a 4m grid cell and measured 39.19 ms per tick, but the audit has no
candidate-pair count or per-stage breakdown for this fixture. Compare 4m, 2m,
and 1m cells while recording pair counts and `SyncBroadphase`, `BroadPhase`,
`NarrowPhase`, `Solve`, and whole-tick costs. Before considering a cell-size
change, require identical ordered pairs and manifolds, contact events, and
final body state across the settings, and retain the stacked and scattered
rows as controls. This is a proposed measurement only; production behavior and
the prior broadphase default remain unchanged.

### Matched 200-client recovery deferral rerun, 2026-09-30

The fixed-width serialization deferral is rejected. It reduced the recorded
recovery phase slightly, but increased packing and whole-tick cost in both
compute modes. The runtime now encodes offered values in `BuildComponents`
once and lets both packing passes reuse those bytes. The deferred row flag,
packing-time store lookup, intermediate writer, and late source-buffer append
have been removed. The exact packet-order and current-value retry fixture is
retained; its writer-count check covers one eager encode per offered row.

Both frozen servers were built with `release-tests`: GCC, first-party `-O3`,
Tracy enabled, heap hooks and assertions disabled. The baseline differed from
the candidate only by replacing the `BuildComponents` deferral predicate with
`false`; all other static libraries matched by SHA256. Both variants used the
same loadtest executable and staged `Stress.luau`, QUIC, 200 clients, a
45-second harness, a 57-second server, 30 Hz ticks and input, random-heading
seed 1, heading changes every 30 ticks, and 30-tick profile windows. Serial
runs passed `--force-serial-compute`; the parallel runs used the default.
The binary copies, source variants, cache settings, common-library hashes,
and machine-readable results remain under
`.cache/build/release-tests/stress-ab/`. Each capture's metadata records
server, harness, and scene hashes. The host was a Ryzen 9 9900X with 24 logical
CPUs. Build jobs were stopped and the documentation retrieval process was
suspended for the timing window; ordinary desktop processes remained running.

| Compute | Variant | Ticks | Tick p50 / p95 / p99 ms | Overruns | Recovery p95 ms | Pack p95 ms | Dropped scopes |
|---|---|---:|---|---:|---:|---:|---:|
| Serial | Eager baseline | 552 | 107.861 / 138.521 / 143.048 | 478 | 26.004 | 5.521 | 0 |
| Serial | Deferred candidate | 497 | 134.712 / 150.296 / 156.272 | 421 | 25.045 | 14.055 | 0 |
| Parallel | Eager baseline | 1,312 | 20.720 / 77.869 / 99.158 | 452 | 554.356 | 7.134 | 88,224 |
| Parallel | Deferred candidate | 1,260 | 22.288 / 82.770 / 98.761 | 486 | 518.313 | 183.209 | 83,302 |

Every run admitted all 200 sessions and ended with all 200 Playing, none
streaming or timed out. Serial baseline/candidate sent 201,362/201,444 inputs
and applied 1,341,358/1,215,106 deltas; parallel baseline/candidate sent
197,849/198,272 inputs and applied 2,648,461/2,248,700 deltas. The random seed
fixes submitted headings, but a slower server completes different ticks and
receives different acknowledgements. These are matched configurations with
real network feedback, not identical internal execution traces. One pair per
compute mode establishes no broad regression threshold or precise speed ratio.

Recovery and packing figures are complete lane timing histograms from
`Authority::ReportPhases`, summed across publishing lanes per sample. They
measure producer work, not owner-thread elapsed time. The serial captures
folded all 552/497 frames with zero dropped scopes and retain whole-run and
windowed flame graphs. Parallel flame graphs are explicitly incomplete;
`FrameGraph` refuses off-owner live scopes as well as bounded overflow. Their
retained-scope percentages must not be used as complete frame-time shares.
The serial total also includes reported work, so its summed folded self time
must not be equated with wall time.

The measured packing increase is a phase result. Source inspection confirms
that deferral added a second component lookup and an intermediate encoded
buffer append before copying into the packet. `Pack` can run twice after
priority selection, so the intermediate cache also supported reuse on the
second pass. This audit does not attribute the measured increase to one
lookup, copy, allocator, or lock without finer evidence.

Raw captures use labels `recoverrows-{baseline,candidate}-{serial,parallel}-20260930`
in `.cache/stress/`, with server and client logs, folded stacks, whole-run
SVG/text, window snapshots, and averaged SVG/text. The first candidate serial
wrapper exited 2 after the server and harness had completed and the whole-run
SVG was written: editing the executing shell script changed its read offset
and caused an EOF error. Its complete raw profile and final session report
were retained, and its averaged output was regenerated directly. Subsequent
runs used an immutable script copy and exited 0.

The stress wrapper now clears the current label's old folded artifact before
launch and rejects a nonzero server exit. A bounded fake server stayed alive
past the two-second readiness check, then exited 7 while its fake harness
exited 0. The wrapper exited 1 with `FAIL: the server exited 7`, removed the
precreated stale folded file, and printed no success. Shell syntax and diff
checks pass.

After removing the deferred path, a fresh `release-tests` build of the server,
loadtest, and replication test binary passed. The exact packet/retry gate
passed 62 assertions in one case; priority, recovery, and parallel publishing
checks passed 274 assertions in 27 cases. The complete replication suite
passed 22,928 assertions in 284 cases, including variable-width serialization,
loss, QUIC/datagram, and serial/parallel publication coverage. The three changed
C++ files pass `clang-format-21 --dry-run --Werror`; the scoped diff check
passes. No additional post-removal timing is claimed beyond the frozen eager
baseline above.


## 2026-09-30: 4000-body cell-size sweep with physical-output parity

The new `just physics-cell-size-sweep` job checks parity in `release-tests`
and runs the `bench` preset suite `engine.physics.bench.cell-sizes`. Its
default and maximum measured sample count is five. The fixture forces compute
serial, advances at 60 Hz, settles for ten ticks, then executes the runner's
eight warmup calls and five measured calls. Calls 9 through 13 correspond to
scene ticks 19 through 23. The deterministic pile, stacks and scattered scenes
share their builder with the original stepping benchmarks. Actual scenes have
4000 dynamic boxes and one anchored floor. Each adjacent baseline has zero
dynamic boxes and the same floor, tick pipeline and profile settings.

The exact parity gate passed **552 assertions in one case**, comparing all
three cell sizes on every one of 23 ticks for all three layouts. It compares
ordered candidate pairs, manifold fields and contact points, ordered contact
events, transforms, motions, rigid-body values and Simulated presence.
Private solver warm-start caches and broadphase topology are excluded. This is
physical-output parity for the stated trajectory, not every private world
field. The recipe rejects samples outside 1 through 5 rather than extending
beyond that verified measurement window.

Timing session **6324 exited 0**. All **234** per-call profile records were
captured, including warmups and baselines. Every record had **zero dropped
spans** and all four required phase scopes. Floor records contained 48 spans;
actual pile and scattered records contained 51; actual stacks contained 39.
The suite fails on drops or missing required phases. Owner elapsed encloses
BeginFrame, the tick pipeline and EndFrame. Phase figures are inclusive
ENGINE_PROFILE durations; their medians need not sum to the owner median.
The BENCH report includes fixture lookup and measurement bookkeeping, a
different boundary from owner elapsed.

The user authorized measuring with games running. A preparation snapshot
showed Barotrauma PID 62502 at 13.8% lifetime CPU after replacing earlier game
PID 53010. During measurement, Overwatch.exe PID 73622 was also present at
290% lifetime CPU, Barotrauma at 14.0%, Steam PID 50850 at 4.8%, and Discord PID
4608 at 16.9%. These are process lifetime CPU snapshots, not interval averages
or measured contention shares. Other agents held builds for the timed interval.
No user process was changed. This is one **background-loaded** sequential
sweep, not an uncontended performance claim.

All table entries are milliseconds, medians of the five measured calls.
Incremental is actual-scene owner median minus its adjacent floor-only owner
median. It estimates observed added engine cost in this run. It does not
subtract game CPU or recover an uncontended engine time.

| Scene | Cell m | Actual backend | Owner elapsed | Floor baseline | Incremental | Sync | Broad | Narrow | Solve |
| --- | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Pile | 4 | grid | 86.823 | 0.008 | 86.815 | 0.322 | 13.945 | 9.466 | 62.183 |
| Pile | 2 | grid | 80.264 | 0.005 | 80.259 | 0.365 | 7.515 | 9.478 | 61.983 |
| Pile | 1 | grid | 81.363 | 0.005 | 81.358 | 0.434 | 7.729 | 10.300 | 62.074 |
| Stacks | 4 | tree | 10.024 | 0.005 | 10.019 | 0.261 | 0.174 | 0.278 | 8.931 |
| Stacks | 2 | tree | 10.087 | 0.005 | 10.082 | 0.271 | 0.179 | 0.306 | 8.972 |
| Stacks | 1 | tree | 9.989 | 0.005 | 9.984 | 0.266 | 0.116 | 0.299 | 8.932 |
| Scattered | 4 | tree | 1.258 | 0.005 | 1.253 | 0.243 | 0.147 | 0.586 | 0.003 |
| Scattered | 2 | tree | 1.259 | 0.005 | 1.254 | 0.241 | 0.149 | 0.597 | 0.003 |
| Scattered | 1 | tree | 1.210 | 0.005 | 1.205 | 0.242 | 0.090 | 0.599 | 0.003 |

For every pile cell size, measured pair and manifold counts were 33801, 31441,
29609, 28344 and 26908 in order. Event counts were 38138, 35563, 33457, 31951 and
30592. Thus the pile is still evolving across the measured window. Stacks
retained 4580 pairs, manifolds and events per call; scattered retained 4000
candidate pairs and zero manifolds/events. Baselines had zero pairs, manifolds
and events, while still executing the floor tick.

The pile used the grid throughout measured calls. Stacks and scattered used
the dynamic tree, so their differences do not establish a grid-cell tuning
benefit. The 2 m pile broadphase median was 7.515 ms versus 13.945 ms at 4 m,
while solve remained approximately 62 ms and dominated the tick. The 1 m cell
did not improve the observed whole-tick median over 2 m. This supports
repeating 2 m with matched trajectories; one sequential run under variable
background load does not justify changing the production default.
**No production physics setting changed.**

Profile records and derived medians are retained under
`.cache/build/bench/cell-size-profiles/2026-09-30.json`. This artifact contains
profile data only. No raw benchmark report was written to a file. After this
measurement, benchmark flag restoration was made scope-bound so failures
restore serial/profile settings; that robustness edit is not represented as
a second measured run. The final bench_physics rebuild (session 67411,
CCACHE_DIR=/tmp/atomic-physics-ccache, -j2) passed both compile/link jobs. CMake
confirmed the bench preset uses -O3. The final benchmark binary SHA256 is
98cd09aa8230b02b977da77ac4715aed8ab9407dfb714da090b4422f7ad51032;
no timing is claimed for that later binary. The release-tests parity rerun
(session 80633) passed 552 assertions in one case. An initial rerun command
used the nonexistent test/ directory and exited 127; the corrected documented
tests/test_physics command supplied the passing result. Formatting and scoped
diff checks pass. The parent reviewed the audit; Git metadata is read-only,
so these changes remain uncommitted.


### Next measurement proposal, not executed

Repeat the 4 m and 2 m pile comparison with counterbalanced order, alternating
4/2 and 2/4 across fresh process runs. Each arm must rebuild the identical
seeded world, settle ten ticks, warm eight calls and measure exactly scene ticks
19 through 23. Compare the same tick between arms, because pair counts and
solver cost are falling through this trajectory; a later evolving world is
not a valid comparison with an earlier one. Retain adjacent floor-only
baselines, per-call phase durations, counts, active backend and drop checks.
The current suite has fixed 4/2/1 ordering, so reverse ordering requires a
reviewed harness or suite option before a counterbalanced result is claimed.

Keep the 23-tick exact physical-output gate and replay stacks/scattered
controls. A longer or settled trajectory needs its own matching parity window.
Record background process transitions, keep other builds out of each pair and
report observed paired differences without subtracting background process CPU.
Production grid defaults remain unchanged until repeated evidence includes
representative scenes and confirms a whole-tick benefit without control
regressions. This plan has not produced additional timing evidence.


## Imagegraph source-family profiling

The existing `engine.imagegraph.bench.evaluation` workload evaluates one
256x256 three-octave simplex output. New source-family fixtures now exercise
captured stereo audio through source Audio Window and FFT, palette Gradient
through 1024 colour samples, and eight 256x256 Solid outputs through an invert
processor batch. Inputs are authored and captured in memory, with stable FNV
fingerprints. Preflight checks use analytical expected bins, colours and pixels
and require deterministic replay. The first coherent fixture run failed the
analytical FFT magnitude check. The catalogue default is None (enum 0);
source inspection showed that Audio Window clamps its end to packets minus
one and excludes that end packet. A 1024-sample capture therefore supplied
1023 window samples. The fixture now supplies 1025 samples per channel and
explicitly authors None preprocessing, preserving its intended 1024-sample
window and unchanged analytical DC expectation. A subsequent coherent full
core run passed 332 of 333 cases and failed this source-family fixture during
compilation of its Solid/filter route. The legacy value.array output schema
is Array, while pc.invert's input is Image; the link guard lacks the matching
surface-array exception. The passing batched pc.solid-to-pc.invert fixture uses
Image declarations at both ends and does not cover this legacy Array link.
The fixture route and expected outputs were preserved while the production
owner repaired the compiler. Fixture failures now include family, durable node and port identifiers.
After the verified surface-array compiler repair, the later coherent dev
core gate passed 40,468 assertions in 334 cases, including this analytical
fixture. The optimized profiling result appears below. These are engine
correctness fixtures, not licensed-native parity evidence.

The new `imagegraph-source-family-bench` recipe caps measured samples at five
and runs the focused analytical fixture first. The profiling benchmark records
each evaluation frame's span hierarchy, parent, order, inclusive time, self
time, idle time and reported flag. It fails on missing required evaluation and
family scopes or dropped frame/heap spans. The last five of thirteen recorded
calls are measured after eight runner warmups. Verification, retained-record
copies and reporting occur outside the owner frame but inside the broader
BENCH call boundary. No raw benchmark report is written to a file.

Bounded NodeContext/processor scopes feed existing Tracy, FrameGraph and heap
attribution. Fresh image allocation counters record actual pixel vector sizes
and allocation operations. Value publication counters describe retained typed
payload size and output operations, not actual allocator commitment. Heap
reports separately expose allocated-byte/block deltas, live bytes/blocks,
process peaks and profiler overhead, including instrumentation in the measured
interval. A missing compiled-in heap profiler is identified explicitly.
These logical payload counters do not replace allocation evidence.

After those coherent dev gates passed, the authorized Just benchmark job
(session 8600) failed during its release-tests preflight build, before any
measurement. The release unity translation unit exposed unqualified
GradientKey references resolving to detail::GradientKey instead of the public
imagegraph::GradientKey. The production owner qualified all type uses,
including allocation-budget sizeof expressions. After qualification and source
freeze, the same job passed as described below. No performance result is claimed
from the failed job.

### Final optimized profiling result

The exact `just imagegraph-source-family-bench 5` job completed with exit 0 in
session **58626**, with `CCACHE_DIR=/tmp/atomic-joined-ccache` and
`CMAKE_BUILD_PARALLEL_LEVEL=2`. Its fresh release-tests build completed 51 jobs;
the analytical source-family preflight passed **6 assertions in one case**.
The bench build completed ten jobs. The benchmark binary SHA256 is
`0daedf8c44853cf6b7f847fda673d6df8ba5479e279c6c45ed143583643d8c01`.
The `bench` preset used RelWithDebInfo, first-party **-O3**, heap hooks **ON**,
Tracy **ON**, and unity batch size 8. Release-tests used -O3 with heap hooks OFF.
This is an optimized diagnostic CPU profile with heap overhead, not a claim
for the ordinary shipped build with heap hooks disabled. The binary was built
from the frozen pre-v8 wave. Source-file hashes were not captured at
measurement; later working-tree source hashes are not a manifest of this
binary. The recorded binary hash and authored input fingerprints identify the
actual measurement. No later v8 source behavior is included in these numbers.

Each family executed thirteen recorded calls: eight runner warmups and five
measured calls, numbered 9 through 13. All **39 frames** passed the required
phase and zero frame/heap-drop gates. Their **689 spans** preserved valid
parent/depth hierarchy. Audio had six spans per call, gradient five, and Solid
batch 42. Recorded spans had no reported producer work and zero idle time.
Other agents held builds during measurement. Per-game background CPU attribution
was unavailable in the restricted process view; no uncontended or machine-load
corrected performance claim is made for this run. The earlier physics load
interval is not reused as a measurement of this later interval.

Owner elapsed encloses BeginFrame, synchronous evaluation and EndFrame.
Correctness verification, retained-record copies and output reporting are
outside that frame, but inside the broader BENCH call boundary. Allocated bytes
and blocks below are actual process heap deltas around the evaluation frame,
including instrumentation, not inferred from scene or output counts. They were
identical across all five measured calls in each family.

| Workload | Owner median ms | Owner min to max ms | Allocated bytes/call | Allocated blocks/call |
| --- | ---: | ---: | ---: | ---: |
| Captured stereo 1024-sample Window to FFT | 0.191799 | 0.191288 to 0.194093 | 979167 | 147 |
| Palette Gradient to 1024 colour samples | 0.053851 | 0.053790 to 0.053901 | 256398 | 100 |
| Eight 256x256 Solid images to invert batch | 10.967432 | 10.848128 to 11.091214 | 6338271 | 453 |

Median scope figures below are inclusive/self milliseconds, summed by bounded
name within each frame before taking the median. These are nested scopes;
do not add inclusive figures to estimate elapsed cost.

| Workload | Evaluate inclusive/self | Processor inclusive/self | Executor inclusive/self | Image allocation inclusive/self |
| --- | ---: | ---: | ---: | ---: |
| Audio | 0.188032 / 0.034875 | 0.152966 / 0.026090 | audio 0.126897 / 0.126897 | no image output |
| Gradient | 0.050084 / 0.020568 | 0.029516 / 0.007173 | gradient 0.022341 / 0.022341 | no image output |
| Solid/filter batch | 10.965788 / 3.481802 | 7.409454 / 0.005370 | generate 3.083944 / 2.972693; filter 4.333773 / 4.314989 | 0.064923 / 0.064923 |

Solid/filter created **16 actual pixel vectors**, totaling **4194304 payload
bytes** per call at NewImage. The legacy array's image copies and other
allocations are covered by heap attribution; they are not counted as NewImage
operations. Audio published three typed values totaling **246208 payload
bytes**; Gradient published two totaling **82112 payload bytes** per call.
Typed payload counters include the declared value representation and are not
allocator commitments. Executor counts were three, two and sixteen respectively.

At each family's final measured call, process residency and cumulative process
peak were as follows. They include fixtures and bounded retained profiling
metadata from earlier calls, and are not independent per-family heaps.

| Workload | Live bytes | Live blocks | Process peak bytes | Profiler overhead bytes |
| --- | ---: | ---: | ---: | ---: |
| Audio | 18477078 | 5546 | 18835702 | 177472 |
| Gradient | 18666251 | 5677 | 18835702 | 181664 |
| Solid/filter batch | 20879307 | 6275 | 25075449 | 200800 |

Retained metadata explains why process residency need not remain flat across
these calls. This short bounded profile does not establish a leak or a long-run
residency slope. Full per-tag heap records retain parent/depth, allocated bytes
and blocks, live bytes and blocks, and cumulative tag peaks separately.

Fixed input/output FNV fingerprints stayed constant across all recorded calls:

| Workload | Input FNV | Output FNV |
| --- | ---: | ---: |
| Audio | 9208630484314759798 | 4576047086240908149 |
| Gradient | 11164665964581922855 | 12683341020544811813 |
| Solid/filter batch | 18412692067533507951 | 17186047831471956773 |

The normal profile-event capture is
`.cache/build/bench/source-family-profiles/2026-09-30.profile.txt`: **1027 lines**,
consisting only of 39 frame summaries, 689 span events, 117 operation/byte
counter events and 182 heap attribution events. Its whitelist was checked,
with zero unexpected lines. BENCH statistical rows, build/test logs and
whole-process stdout are excluded. Those reports remained on stdout; no raw
benchmark report file was written. This provides actual timing and allocation
evidence for these engine fixtures. It does not promote any family to licensed
native visual or workflow parity.

## Interval machine-load follow-up with the final physics binary

During shared-header review, with native builds held, the parent granted
an interval for load sampling. Session **90526 exited 0**, running the immutable final `bench_physics`
binary, SHA256
`98cd09aa8230b02b977da77ac4715aed8ab9407dfb714da090b4422f7ad51032`,
with `--suite engine.physics.bench.cell-sizes --samples 1`. No source or binary
was rebuilt or modified for this run. This later result supersedes the earlier
statement that no timing had been taken for that final binary; it is a separate
one-sample follow-up, not another five-sample median sweep.

The bounded sampler `/tmp/atomic-sample-benchmark-load.py` sampled `/proc/stat`
and visible per-process `/proc/PID/stat` counters at approximately one-second
intervals. It ran a 15-second leading background-only window, the complete
benchmark process, then a 15-second trailing background-only window. It uses
monotonic elapsed time and SC_CLK_TCK (100 ticks/second), excludes idle, iowait
and steal from busy CPU, and excludes guest fields already included in
user/nice. Busy cores are observed CPU seconds divided by wall seconds.
The sampler and its overhead are included. Output remained on stdout; no raw
report file was created.

| Interval | Seconds | Machine busy cores, time weighted | Observed one-second range |
| --- | ---: | ---: | ---: |
| Leading background only | 15.000 | 0.561 | 0.310 to 1.469 |
| Benchmark active | 41.988 | 1.503 | 1.179 to 2.389 |
| Trailing background only | 15.000 | 0.292 | 0.190 to 0.860 |

The weighted adjacent background mean was **0.426 busy cores**. Active minus
that adjacent mean was **1.076 observed machine busy cores**. The benchmark's
complete child rusage was **41.739 CPU seconds**, averaging **0.994 busy cores**
over launch through final observation. These describe separate observations,
not a correction applied to engine timings. The leading and trailing loads
differed, so their mean does not establish the background work present during
each measured tick or the contention cost.

The current restricted execution environment uses a process namespace:
the sampler was namespace PID 2 and the benchmark PID 3. Host game and Steam
processes were not available for interval attribution through this process
view. Their CPU is **unavailable**, not zero. The aggregate `/proc/stat` view
reported 24 logical CPUs. Visible process deltas observed 40.810 benchmark CPU
seconds; the final process exit prevented a final per-process counter read.
Child rusage supplied the complete 41.739-second CPU result instead.
The sampler reports new, vanished and inaccessible processes explicitly rather
than inventing their missing deltas. This follow-up does not establish whether
the earlier named games continued at their earlier load, and cannot claim an
uncontended machine. No user process was changed.

All **162** per-call physics records had zero dropped spans; the required
phase gate passed. There were eight runner warmup calls and one measured call
per row. The measured call is scene tick **19**, within the existing 23-tick
exact physical-output gate. Candidate counts match the corresponding tick of
the earlier run: pile 33801 pairs/manifolds and 38138 events; stacks 4580 of
each; scattered 4000 candidate pairs and no manifolds/events. Baselines had
zero dynamic bodies and one floor, with owner elapsed from 0.004979 to
0.006362 ms. No additional sample count, median or precision is inferred.

Observed actual-scene durations in milliseconds:

| Scene | Cell m | Owner elapsed | Sync | Broad | Narrow | Solve |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Pile | 4 | 98.180 | 0.315 | 15.357 | 10.684 | 70.856 |
| Pile | 2 | 90.534 | 0.354 | 8.349 | 10.785 | 70.035 |
| Pile | 1 | 93.098 | 0.348 | 8.279 | 10.951 | 72.429 |
| Stacks | 4 | 10.084 | 0.271 | 0.173 | 0.294 | 8.993 |
| Stacks | 2 | 10.171 | 0.283 | 0.176 | 0.299 | 9.061 |
| Stacks | 1 | 10.243 | 0.269 | 0.111 | 0.294 | 9.208 |
| Scattered | 4 | 1.237 | 0.238 | 0.146 | 0.582 | 0.003 |
| Scattered | 2 | 1.313 | 0.317 | 0.146 | 0.581 | 0.003 |
| Scattered | 1 | 1.191 | 0.237 | 0.088 | 0.598 | 0.003 |

This one-sample follow-up again makes 2 m a plausible pile candidate, while
preserving the evolving trajectory and unchanged outputs. It does not replace
the counterbalanced repeat proposal or justify a production default change.
The source-family profiling workloads still await joined correctness and
their separate benchmark build.


## Counterbalanced physics follow-up

The joined optimized validation and both fresh-process timing runs completed on September 30. The production default is unchanged. Results and their aggregate background-load label are recorded below.

`just physics-cell-size-counterbalance 5` uses the existing deterministic 4000-body gravity-free pile and floor-only fixture. Two benchmark rows own independent worlds. Each call runs floor4/pile4/floor2/pile2 in the forward row and floor2/pile2/floor4/pile4 in the reverse row. Both rows settle each world for ten ticks, run eight benchmark warmups and measure five ticks, so physical comparisons refer to scene ticks 19 through 23 in both orders. The job launches the same frozen benchmark binary twice in fresh processes. One sample is allowed for diagnosis, but five are required for the intended comparison.

Each arm retains the owner elapsed frame, all bounded timing spans and their parent/depth relationships, the four required physics phases, exact pair/manifold/event counts and backend selection. Missing phases, reported worker spans, invalid parent relationships or dropped frame/heap scopes fail the run. The same call compares ordered pairs, active manifold points, events and the physical body fields captured by `CellSizeParity.hpp` between spacings. This is output parity, not a claim that every private solver cache is captured.

After all four owner frames in a call, a Spatial-owned non-shipping fixture adapter inspects the actual dynamic, static and continuous grids and returns bounded diagnostic values. Physics fixtures do not name private grid layout symbols. They record each level's proxy count, entry memberships, unique occupied coordinate cells, maximum memberships per cell, bucket count, occupied buckets and maximum bucket entries, plus residual proxies. Coordinate cells and hashed buckets are different quantities. Logical live/retained bytes come from each grid and the world memory groups. Per-owner process heap readings report allocated bytes/blocks, live bytes/blocks, global peak bytes and profiler overhead separately; those totals are not independent world heaps. Sorting diagnostic scratch and allocating parity snapshots are outside the owner frames and inside the broader BENCH call, so BENCH timing must not be substituted for owner elapsed tick cost.

The existing spatial hierarchy, clamped-bound and exact residual candidate tests are parameterized for 1m, 2m and 4m cells. A distinct physics parity fixture applies gravity and fast downward motion against promoted and hierarchy-exhausting floors, at the origin and a finite 600,000m x offset. It checks floor query admission, bounded finite output positions, broadphase/contact admission and exact ordered outputs over 23 ticks. The coordinate clamp is 262,144 cells, corresponding to base-grid saturation near 1,048,576m at 4m spacing and 524,288m at 2m. These are indexing bounds, not safe-world-size guarantees; float resolution and narrow-phase geometry still constrain useful coordinates.

Results must retain the aggregate background-load label. Report matched per-tick 2m-minus-4m differences separately by order and fresh-process repeat, with each arm's adjacent floor-only cost. Floor subtraction is observed incremental engine work and does not remove game contention. Dense-pile results alone do not justify a production default change; stacked/scattered controls and representative large-body workloads remain necessary for that decision.

## Audio playback prerequisite: disabled event path measurement

The joined `dev` build completed in session 21058. With the SDL dummy backend,
the complete audio suite passed **145,056 assertions in 89 cases**. The separate
client seek fixture passed **34 assertions in one case**, exercising queued
fractional seeking, actual observation capture, script validation and control
JSON serialization with durable source names. This does not verify physical
audio output or establish a sound retirement fence.

The existing optimized `engine.audio.bench.mixing` suite was run with five samples
and eight warmups in four fresh processes: preserved/candidate, then
candidate/preserved. Sessions 28067, 26060, 16565 and 59455 exited 0. Builds and
other owned runtime tests were stopped during these runs. Host background
process attribution remained unavailable; the earlier physics load baseline
was not reused as a correction. No raw benchmark report was written to a file.
The preserved executable SHA256 was
`aa738aa973200feeec020eb3fd0a607aeba17577e716f7c488fe11f752cd13ad`;
the rebuilt `bench` executable SHA256 was
`1401ff77c86907b5ee7399c305c9128f523f36cd3a44b9933d0d5f1aaa345246`.
Playback events are disabled in these existing fixtures. Numbers below are
the runner's minimum sample per call, not medians or confidence intervals.

| Existing workload | Preserved, first pair ns | Candidate, first pair ns | Candidate, reverse pair ns | Preserved, reverse pair ns |
| --- | ---: | ---: | ---: | ---: |
| Render, 64 voices, no commands | 101,580 | 100,387 | 101,655 | 101,077 |
| Render, 64 voices, 8 commands | 137,678 | 134,926 | 135,194 | 135,535 |
| Render, 64 voices, 64 commands | 403,419 | 390,871 | 391,053 | 392,848 |
| Render, 512 voices | 964,238 | 950,765 | 947,136 | 950,608 |
| CommandQueue::Post, available ring | 5 | 6 | 6 | 5 |
| CommandQueue::Post, full ring | 4 | 4 | 4 | 4 |

The render rows remain close in both orders. These pairs do not establish a
stable speed improvement. The available-ring command posting row increased
from 5 to 6 ns in both orders, consistent with copying the enlarged command;
the cause was not separately isolated. Enabled event delivery is covered by
functional tests, including overflow and concurrent polling, but its timing
was not measured by this disabled-event suite.


### Residual-floor CCD failure and repair

The first dev run of the new falling-floor gate failed at tick zero for a million-metre half-extent floor at the origin with fast downward motion: body centre y was -1.051489115m, below the required 0.48m bound. The enclosing O0 parity run was stopped with status 130 after observing that failure, so it did not complete the full stress suite. Existing spatial tests passed 1129 assertions in 33 cases.

An isolated diagnostic linked against the existing dev physics library confirmed the exact sweep error. A unit box at y=3 above the floor top at y=0 has a 2.5m vertical gap. With 4096m floor half-extent, GJK returned 2.5m and the motion sweep fraction 0.499727577. With million-metre floor half-extent, it returned 4.000245094m with witness points near the two shape centres, and fraction 0.799661934, placing the body centre near -1.000489235m before the continuous bite and solver. At y=-1.051489115m, GJK still reported separation while the exact box SAT reported touching. The static candidate was admitted; the residual index did not drop the floor. GDB tracing was unavailable because the sandbox refused ptrace.

Translation-only box pairs now use one allocation-free swept SAT helper shared by the static and relative-motion sweep entry points. It intersects overlap intervals on six face axes and nine edge cross axes. Double normalization and projection/interval arithmetic retain finite nonzero cross axes, including nearly parallel axes that can matter for long boxes. The contact skin remains conservative, the normal points from the fixed box toward the mover, and the closing speed is the relative linear speed along that normal. Initial supporting tangent and separating placement remains permitted. Angular motion retains the existing advancement path. The witness is constructed at the unpadded contact time within the submitted interval; skin-only empty face clips use a nearby fixed-box surface point rather than a distant support corner. This repairs the reproduced box sweep, not the general GJK distance API or every large-coordinate geometry case.

Isolated objects compiled from the replacement source and linked against the existing dev libraries passed ConvexQuery (350 assertions, 32 cases), Continuous (75 assertions, 17 cases) and the formerly failing falling-floor fixture (2304 assertions, 1 case). These are focused replacement-object checks, not a new coherent build or a completed 4000-body parity run. The subsequent coherent `release-tests` full physics suite passed 63,127 assertions in 318 cases, including the falling-floor and cell-spacing parity fixtures. The full Spatial suite passed 1,942 assertions in 110 cases. The optimized counterbalanced profiling job then completed both fresh-process repeats with five measured samples per arm. Its executable SHA256 was `354c9a656526c82cbac0b0b7b51121402e95bf58acea389f14c055e686d564e7`, distinct from the earlier cell-size sweep binary.


### Counterbalanced run background load

Builds and owned runtime tests were held during the two fresh-process counterbalance repeats. Aggregate `/proc/stat` sampling reported 24 logical CPUs. The 5.000137-second leading baseline averaged 0.803978 busy CPU cores; the 5.000149-second trailing baseline averaged 0.797976. Their duration-weighted mean was 0.800977 cores. During the 112.876135-second active window, aggregate busy load averaged 2.062349 cores. Subtracting the surrounding baseline yields approximately 1.261372 cores of incremental observed machine load.

Complete child rusage recorded 112.500927 CPU seconds across 112.876891 seconds from launch to final observation, equivalent to 0.996669 cores. Visible process interval deltas missed final exits and are explicitly incomplete. The difference between aggregate subtraction and child CPU accounting demonstrates background variation during the active window; subtraction does not isolate benchmark CPU precisely or remove contention from tick timings. Individual game and Steam process counters were inaccessible in this process namespace. No user process was changed.


### Coherent CCD timing check

The existing optimized `engine.physics.bench.continuous` suite ran with five samples in four fresh processes: preserved/repaired, then repaired/preserved. All exited zero. Builds and owned runtime tests were held. The preserved executable SHA256 was `98cd09aa8230b02b977da77ac4715aed8ab9407dfb714da090b4422f7ad51032`; repaired was `354c9a656526c82cbac0b0b7b51121402e95bf58acea389f14c055e686d564e7`. Aggregate background remained uncontrolled; the separate surrounding counterbalance baseline cannot correct these timings. Values are runner minimum sample ns per BENCH call, not owner-frame medians or uncertainty estimates.

| Existing workload | Preserved, first pair ns | Repaired, first pair ns | Repaired, reverse pair ns | Preserved, reverse pair ns |
| --- | ---: | ---: | ---: | ---: |
| 4096 crossing dynamic pairs | 173,029 | 165,977 | 167,922 | 178,671 |
| 4096 rotational impacts | 2,734,975 | 2,774,485 | 2,778,806 | 2,720,975 |
| 8192 still bodies and one fast pair | 485,486 | 485,586 | 484,770 | 489,514 |
| Frozen cascade resweeps | 59,524 | 54,715 | 54,800 | 58,762 |
| Duplicate swept-grid gather | 4,630,424 | 4,364,332 | 4,551,323 | 4,393,499 |
| Ordered swept-grid gather | 4,192,767 | 4,372,835 | 4,291,430 | 4,366,576 |
| Ordered swept-grid shape gather | 12,279,852 | 12,105,185 | 12,504,071 | 12,678,461 |

The translation-only crossing and cascade minima were lower in both orders. Rotational impact minima were approximately 1.4 to 2.1 percent higher, although that angular path was unchanged. The gather controls varied in both directions. These runs check the repair's observed shipped-build cost; they do not establish an isolated speed improvement or bit-exact equivalence with the known incorrect old sweep. No raw benchmark report was saved.


### Copied playback outcome validation and final audio timing

The subsequent outcome fix adds copied dispatch status to internal observations and prevents a refused seek or other command from being published as an applied version-1 event. If any event is refused, the existing durable format marks the complete event history missing while retaining source state and waveform. The coherent SDL-dummy audio suite passed 145,095 assertions in 90 cases; the full client audio data-factory suite passed 239 assertions in 17 cases. The focused seek/refused-capture cases passed 78 assertions in two cases. These checks do not verify a physical output device.

The final optimized audio executable SHA256 was `dd79c00ec10c9f3d10edf540e44f5c411260643ec1949ebb627d8f044f6bed42`. The same preserved executable above was compared in four fresh processes, preserved/final then final/preserved, with five samples. Builds were held. A separate metadata extractor unittest run briefly overlapped the last preserved run (reported unittest elapsed 0.009 seconds); this last window was therefore not completely exclusive of owned tests. Background process load was not isolated or subtracted. Each value is the existing runner's minimum ns per BENCH call with copied playback events disabled.

| Existing workload | Preserved, first pair ns | Final, first pair ns | Final, reverse pair ns | Preserved, reverse pair ns |
| --- | ---: | ---: | ---: | ---: |
| Render, 64 voices, no commands | 101,419 | 101,476 | 100,506 | 102,292 |
| Render, 64 voices, 8 commands | 139,536 | 135,221 | 135,279 | 135,939 |
| Render, 64 voices, 64 commands | 408,510 | 397,390 | 400,280 | 403,872 |
| Render, 512 voices | 991,766 | 956,506 | 955,129 | 983,998 |
| CommandQueue::Post, available ring | 5 | 6 | 6 | 5 |
| CommandQueue::Post, full ring | 4 | 4 | 4 | 4 |

The final render minima remain close to the earlier candidate. Available-ring posting again measured 6 ns versus preserved 5 ns; no isolated cause or enabled-event timing conclusion is inferred. Raw benchmark reports were not written to files.


### Counterbalanced cell-spacing results

The two fresh processes retained 104 owner frames and 5,148 spans each, for 208 frames and 10,296 spans total. All frame and heap dropped counts were zero; required phase and hierarchy checks passed. The tree backend flag remained zero. All ordered pair, manifold, event and captured physical-body comparisons passed between 4 m and 2 m. Measured samples are evolving scene ticks 19 through 23 after ten settling ticks and eight warmups. The twenty matched measured tick pairs are not twenty independent run trials.

Durations below are milliseconds. Owner columns and adjacent floor columns are per-arm medians. Delta columns are medians of matched per-tick differences, not subtraction of separate medians. Floor-adjusted delta compares each pile owner tick after subtracting its same-call floor-only arm. That subtraction does not remove background contention. Diagnostics and physical snapshots run after all four owner frames and are included in the broader BENCH boundary, which is not used for this owner-tick comparison.

| Fresh process | Arm order | Pile 4 m owner | Pile 2 m owner | Paired 2-minus-4 | Floor 4 m owner | Floor 2 m owner | Floor-adjusted paired delta | Broadphase paired delta |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 4 then 2 | 85.497818 | 79.495323 | -5.898697 | 0.041849 | 0.030117 | -5.893006 | -5.666638 |
| 1 | 2 then 4 | 85.595657 | 79.633827 | -6.253983 | 0.030909 | 0.042250 | -6.263040 | -5.826550 |
| 2 | 4 then 2 | 86.047836 | 80.390732 | -5.657104 | 0.042640 | 0.032210 | -5.645222 | -5.706183 |
| 2 | 2 then 4 | 86.039818 | 80.269218 | -5.989334 | 0.032441 | 0.041819 | -5.997840 | -5.754105 |

Every matched owner difference favored 2 m, ranging from -7.141395 to -4.868401 ms. Most of the paired median difference came from broadphase. Narrowphase differences changed sign between orders, so no reliable narrowphase gain is established. This is a dense gravity-free overlapping pile with two fresh-process repetitions; it does not justify changing the production default or establish a result for stacked, scattered or representative large-body scenes.

At measured tick 23, each pile has 4,000 dynamic base-level proxies and zero residual proxies. The common static floor has one promoted level-4 proxy, eight memberships and eight occupied coordinate cells. The continuous grid is empty. Actual dynamic-grid diagnostics:

| Quantity at tick 23 | 4 m | 2 m |
| --- | ---: | ---: |
| Entry memberships | 7,316 | 12,566 |
| Unique occupied coordinate cells | 65 | 449 |
| Maximum memberships per coordinate cell | 320 | 79 |
| Hash buckets | 8,192 | 16,384 |
| Nonempty hash buckets | 47 | 330 |
| Maximum entries per hash bucket | 447 | 209 |
| Logical grid live bytes | 421,828 | 538,596 |
| Logical grid retained bytes | 470,516 | 578,308 |

Coordinate occupancy and hash-bucket occupancy are separate measured quantities. The 2 m grid retained 107,792 additional bytes in this tick while lowering bucket congestion and broadphase cost. Neither quantity establishes a general optimum.

The paired fixtures have substantial retained memory. At tick 23, a 4 m pile world reports 56,712,813 logical live bytes and 2,696,651,261 retained bytes; 2 m reports 56,829,581 live and 2,696,759,053 retained. Solver storage retains 1,954,539,925 bytes and persistent storage 585,695,200 bytes per pile world. The four independently retained pile worlds sum to 10,786,820,628 retained bytes, compared with approximately 10,808,953,262 process heap live bytes. These process heap readings include both benchmark rows and are not separate world heaps or RSS. World memory groups use vector capacities; source inspection is consistent with high-water storage from initial overlapping contacts, but exact per-vector allocation attribution was not captured. Diagnostic scratch is outside these world groups. This single evolving run does not prove a leak.

Only the bounded profile/occupancy/memory evidence was retained under `.cache/build/bench/cell-size-profiles/2026-09-30-counterbalance-1.profile.txt` and `2026-09-30-counterbalance-2.profile.txt`. They contain no raw BENCH report rows.


### SDL copied-event opt-in repair

An isolated harness using the actual SDL dummy callback reproduced a device-specific omission: `OpenDevice` with `PlaybackEvents=true` rendered 1,024 frames but produced zero copied events. The NullDevice constructor already enabled events; the SDL device ignored this setting. The SDL open path now enables the bounded copied-event ring before stream creation and the first callback resume.

A coherent audio-only dev rebuild completed in session 15957. With `SDL_AUDIODRIVER=dummy`, the complete audio suite passed 145,139 assertions in 91 cases (gate 788914), including enabled and disabled settings through actual headless SDL callbacks. The executable SHA256 was `d0b66034d1f16a01ad8b7252416ea0e1678a8f9cc6d5ab2452bd7edc86e709ac`. This is a later audio-only gate, separate from the earlier joined freeze and optimized timing binaries. No physical device was selected. A possible partial pause/lock failure remains a host ownership risk without a live reproduction; preview retirement must retain sound ownership and latch a failed pause rather than assume a retry establishes the callback barrier.


### Manifold-wide solver setup experiment

The candidate in `mono.engine/physics/src/Solve.cpp` computes the contact tangent basis and shared correction mass once per nonempty manifold. Pair-wide speculative closing history is resolved lazily on the first non-speculative contact. Empty and all-speculative manifolds do not gain a cache lookup. Contact-specific lever arms, angular responses, closing-speed arithmetic, bias, restitution, feature-specific warm-start lookup, contact ordering and sweep ordering remain in their existing order. The cache is read-only throughout setup. This experiment does not change the production cell-size default or solver scheduling.

The baseline includes the same canonical-output fixtures as the candidate, with the setup implementation preserved. Optimized `bench` executable SHA256 values are:

| Build | Executable SHA256 | `Solve.cpp` SHA256 |
| --- | --- | --- |
| Baseline | `48e568ef36a76164d5145618655554c7c12581beb50a27e1e955c3ab6a34b3f4` | `eaee11f289e851248ebd3ba28972a61cc10aeb65a8b339c5b4117d532c4b078b` |
| Candidate | `a9b0c7b495b1b890eef52430b666a40cb9584fe97a0a4d582a13f29df9658cd2` | `757ec7718530a471804eb18c6d49a50f09b9be859c1cdb295ad5d0feced0f294` |

The preserved baseline test executable SHA256 is `a584a6a2f22496627d581275922c9d3b868ac2770922d7b9d51cfd0feb8de3dd`. The dev baseline full physics suite completed in session 69348 with 81,438 assertions in 319 cases. The fixture-only baseline mixed-contact gate passed 18,311 assertions in one case before the production edit.

Cross-binary correctness was compared using complete canonical hexadecimal word payloads captured and compared in memory, rather than pair counts or hash equality alone. Session 68c48e completed successfully with 460 equal records across pile, stacked, scattered and fast layouts over 23 ticks. These records include ordered public pairs, manifold/contact fields, events, captured physical-body fields and the warm-start impulse cache. Their canonical hex payload occupies 9,251,200 bytes; its SHA256 is `abf8b8c7408674a4b397d0f2e284d7f804f062790ae512827a0578d419300544`.

The optimized full 4,000-body comparison in session 29872 also completed successfully: all 32 canonical records matched across eight counterbalance arms at final measured scene tick 23. The records contain complete ordered public pairs, manifold/contact fields, events and captured physical-body fields for both cell sizes and arm orders, including floor-only arms. Canonical hex payload occupies 69,616,768 bytes, SHA256 `793846e500af9ee898994dc22778d03d1e615e4b91c58f105f8b01c8cfbd43ea`. These final-tick records do not include private warm-start caches or every intermediate 4,000-body tick. Existing same-binary cross-cell comparisons remain enabled. Neither gate claims every internal field is covered.

#### Counterbalanced setup timing

Session 89056 completed four fresh optimized processes in baseline/candidate/candidate/baseline order, with canonical stdout export disabled. Each process retained 104 owner frames and 5,148 spans; all frame and heap dropped counts were zero and the required phase and hierarchy gates passed. The four processes therefore captured 416 frames and 20,592 spans. Owned builds and tests were held during this interval. Execution was forced serial, with the tree backend disabled; this does not establish pooled-worker performance.

Each source pair compares corresponding evolving scene ticks 19 through 23, after ten settling ticks and eight runner warmups, across both cell sizes and both arm orders. Each pair supplies twenty matched 4,000-body tick differences. These forty differences are not forty independent whole-run trials. Values below are medians of candidate-minus-baseline matched differences in milliseconds, not subtraction of separate medians.

| Source order | Matched pile ticks | Owner median difference | Owner difference range | Setup median difference | Setup difference range | Solve median difference |
| --- | ---: | ---: | --- | ---: | --- | ---: |
| Baseline then candidate | 20 | -2.2641185 | -3.772782 to -1.198547 | -2.1881725 | -2.747063 to -1.625887 | -2.1181845 |
| Candidate then baseline | 20 | -1.4394875 | -2.315270 to -0.705094 | -1.9796595 | -2.360359 to -1.697237 | -1.7805400 |

Every matched owner and setup difference favored the candidate. This establishes an observed benefit for the exercised serial dense-pile workload, with exact output checks at the stated boundaries. It does not establish a universal scene or parallel-solver gain. Owner elapsed encloses the tick and frame begin/end; occupancy, memory and snapshot diagnostics run afterward, inside the broader BENCH boundary. Raw benchmark report rows were not written to files.

#### Interval load and acceptance limits

Adjacent background intervals averaged 0.2399822477 and 0.2766444101 busy cores, with a duration-weighted mean of 0.2583133883. The active 222.357532954-second interval averaged 1.3718446861 busy cores; its observed excess above the adjacent baseline was 1.1135312978 cores. Complete benchmark child CPU averaged 0.9967270866 cores. This subtraction describes aggregate machine load; it does not attribute background processes, remove contention or correct individual tick timings.

The candidate full physics suite in session 51905 completed successfully with 81,438 assertions in 319 cases. The separate optimized solver controls also completed as recorded below. Acceptance remains qualified by workload: cross-binary parity and a measured serial dense-pile benefit are established, while broader controls do not establish a universal gain.


#### Default-scheduling solver controls

Session 36288 completed successfully with four fresh optimized processes in baseline/candidate/candidate/baseline order, each running all 22 registered rows of `engine.physics.bench.solver` with five samples and eight warmups. The baseline and candidate executable hashes remain those recorded above. This control run used default scheduling, without forcing serial compute. No worker profile capture was collected, so complete worker-span attribution is not claimed. Owned builds, tests and compilers were held during the 138.975233104-second interval.

Across the 22 existing rows, candidate minima were lower in both source pairs for seven rows, higher in both for three, and changed sign between pairs for twelve. Several differences are comparable with the within-process sample spreads. Representative controls below are the existing runner's minimum normalized nanoseconds per iteration, followed by the slowest-minus-fastest spread in parentheses. They are not medians or matched evolving-tick differences.

| Existing workload | Baseline, first pair ns (spread) | Candidate, first pair ns (spread) | Candidate, reverse pair ns (spread) | Baseline, reverse pair ns (spread) |
| --- | ---: | ---: | ---: | ---: |
| Stable topology, dense contacts | 1,209,050 (97,632) | 1,200,564 (25,314) | 1,278,067 (276,348) | 1,224,053 (455,949) |
| 256 independent stacks of 4 | 570,934 (36,390) | 581,731 (46,643) | 580,021 (20,597) | 558,040 (14,516) |
| Independent stacks with bridge churn | 2,092,867 (97,030) | 2,081,921 (401,613) | 2,066,860 (91,607) | 2,046,681 (72,439) |
| Pipeline topology churn, 128-body add/remove cycle | 1,823,975 (109,424) | 1,880,540 (119,166) | 1,804,729 (109,423) | 1,788,426 (56,564) |

The four-body independent-stack minima were approximately 1.9 percent and 3.9 percent higher with the candidate, with comparable sample spreads. Topology-churn minima were also higher in both pairs. Stable and bridge-churn results changed direction. These controls qualify the observed dense serial-pipeline benefit rather than demonstrating a general solver improvement or excluding regressions in other scenes. Raw benchmark reports were not written to files.

Leading and trailing background intervals averaged 0.513320467640 and 0.213328027296 busy cores, with a duration-weighted mean of 0.363324261792. The active interval averaged 5.445286783104 busy cores, exceeding that adjacent baseline by 5.081962521311; complete benchmark child CPU averaged 5.393581814963 cores. These are observed aggregate-load figures, not corrected solver timings or per-worker costs. The changing adjacent load further limits causal interpretation of small control differences.


### World lane-refresh scratch experiment

The existing `engine.world.bench.barrier` workloads now have an opt-in heap diagnostic selected by `ATOMIC_WORLD_LANE_HEAP=1`. A fixed `Universe::RefreshLanes` profile scope feeds the existing timing and heap instrumentation. The diagnostic reads actual allocation counters for this scope beneath Tick's `schedule worlds` parent, excluding construction and presentation paths. It brackets the existing workload ticks and prints after their joins. Reads and printing belong to the broader opt-in BENCH cost. The diagnostic uses neither a second timer nor per-tick heap sampling, resets no counters, bounds output to 1,024 calls and refuses missing heap hooks, missing attribution or new dropped scopes. Raw benchmark output was not written to files.

Baseline diagnostic session 13657 completed successfully with one sample and eight warmups per row: 162 calls across all 18 existing workloads. The allocation comparison excludes each row's first call and analyzes 144 subsequent calls, comprising seven remaining warmups and one measured sample per row. With 12 pinned workers, every analyzed lane refresh allocated 96 bytes in one block per tick. The allocation was temporary, with zero scope live bytes afterward. Candidate diagnostic session 14044 repeated the same 162 calls and showed zero allocated bytes and blocks per refresh in the corresponding 144 post-first calls. Both runs reported zero dropped heap scopes. These executables use preset `bench`, first-party `-O3` and compiled heap hooks. Their timing results are not claims about a heap-hook-free shipped `release` build.

The candidate retains a private `LaneLoads` vector per Universe and resizes and clears its contents before each nonzero-lane refresh. Both registry scans, local/remote/null filtering, existing lane preservation and least-loaded-lane assignment arithmetic remain in their existing order. This is reusable driver-owned scratch, not a cached copy of registry state. The zero-lane early return remains unchanged. Eliminating per-tick allocation trades temporary allocation churn for retained vector capacity. Tick-path heap counters do not measure the creation-path allocation or the total retained buffer residency, so zero Tick live bytes must not be described as zero memory cost.

| Optimized `bench_world` build | Executable SHA256 |
| --- | --- |
| Diagnostic baseline, preserved at `/tmp/atomic-world-lane-before-scratch` | `32a83ebecc669e94c127cb3c388015432d83b188c12a7542f6d112d4825a39fb` |
| Scratch candidate | `2fd5958444c57edc4f7df9b81698d8b0e9b0b09ddf86c40700530c7021d58018` |

The existing benchmark job can select this suite with `just bench --filter engine.world.bench.barrier --all --samples 5`. Diagnostic collection uses the same suite with the environment opt-in. Timing comparisons leave that opt-in unset.

#### Correctness gates and unresolved world failures

Optimized benchmark build 48359 and dev world-test build 9310 completed successfully. The focused new lane-refresh case passed 245 assertions in one case. It keeps one Universe alive across pool sizes 2, 4, 1, zero and 2, exercises a null registry slot becoming remote and then local, and checks slot reuse, exact analytical counters, serial/parallel output equality and matching tick/presentation thread ownership over fifteen ticks per execution mode. Formatting and isolated syntax checks passed before the builds.

The complete world suite in session 59694 exited 42: 281 of 285 cases passed, with 33,768 of 33,776 assertions passing. Compact rerun 90980 reproduced the same failures. The observed failing boundaries were:

- `TickExchangeHost.cpp:207` and `:222`: `SendTickExchange` returned false and nested exchange work failed.
- `TickExchangeHost.cpp:523` and `:435`: readiness checks returned false, including four sections at line 435.
- The child at line 139 obtained a null result from `AdoptInheritedChannel`.
- `PresentationBus.cpp:405`: `SendPresentation` returned false.

These failures have no established cause in this experiment. They cannot all be classified as networking failures, and the passing focused lane case does not replace the failed full-module gate. A separate root probe confirmed that creating/binding both UDP and TCP loopback sockets currently raises `PermissionError(1, Operation not permitted)`. This independently blocks a new 200-client RecoverRows/Tracy loopback capture under the current sandbox permissions; it does not prove the cause of every world assertion above. Architecture gate 50658 passed, covering 52 modules, six programs, 36 layered targets and six fixtures. The scratch reuse is accepted as a compatible allocation improvement, qualified by the matched baseline isolation below. The full module remains failing and its failure causes are unproved; no universal speed gain or fully green world suite is claimed.

#### Counterbalanced timing and load limits

Session 48903 completed four fresh optimized processes in baseline/candidate/candidate/baseline order, each running all 18 Barrier rows with five samples and eight warmups. All rows completed. Candidate minima were lower in both source pairs for five rows, higher in both for five, and changed sign for eight. The existing runner reports minimum normalized nanoseconds per iteration; these are not medians or independent per-tick paired samples. Other rows were mixed and their sample spreads were often large, so no universal barrier or scheduling gain is established.

| Existing workload | Baseline, first pair ns | Candidate, first pair ns | Candidate, reverse pair ns | Baseline, reverse pair ns | First / reverse candidate change |
| --- | ---: | ---: | ---: | ---: | --- |
| Tick, 100 worlds of 200 entities | 74,477 | 52,490 | 52,639 | 74,963 | -29.5219% / -29.7800% |

This row showed a repeatable observed benefit in both source orders. The strongest established mechanism across all rows is removal of the measured allocation churn. Timing and heap diagnostics are separate runs; the diagnostics do not establish complete worker-span profiling.

For the baseline diagnostic interval, adjacent background readings averaged 0.4466236893 and 0.2033154094 busy cores; active load averaged 1.1461627371 cores, with an observed excess above the adjacent baseline of 0.8211931878 cores. For the 5.855953669-second ABBA interval, leading and trailing background readings averaged 0.7466135138 and 0.6899549218 busy cores; active load averaged 1.6359419049 cores, with observed excess 0.9176576871 cores. These aggregate CPU subtractions describe machine load only. They neither remove contention nor correct reported benchmark wall times. The visible process namespace exposes Codex processes rather than reliable host process identities, so current game-process presence or named-process load is not established.


#### Original-path baseline isolation of world failures

A dev `test_world` baseline was rebuilt in session 1808 using the pre-scratch diagnostic `Universe.cpp` SHA256 `90addadd64406b25911ecad0b0e96eeaba15029fe71516af7f5235a8f94b9694` and `Universe.hpp` SHA256 `930e7991a2de1604aa9410c18e4ace667a31ecdf78667ca912c5b3b93be8a034`. It used the same new lane-refresh test fixture as the candidate, SHA256 `a9939bb9737f9548e71c9f66d76606b8ac8fd6afcb844eadd93786eef804f9c6`. Thus the runtime comparison changes scratch reuse while retaining the diagnostic scope and test coverage. Both production source files were restored afterward, and the candidate rebuild completed successfully.

| Dev `test_world` build | Executable SHA256 | Preserved executable |
| --- | --- | --- |
| Pre-scratch baseline with the new fixture | `56145e7d747e56660d5898937bdc0e17aa02b50c9e5e27996c043ce44ada2813` | `/tmp/atomic-world-lane-baseline-test_world` |
| Restored scratch candidate | `936c2c009e526ca139fd1e603064eab4cffcc630742b14e7e2fc6b3c25db09ad` | `/tmp/atomic-world-lane-candidate-test_world` and `/tmp/atomic-world-lane-restored-candidate-test_world` |

The first attempt ran the copied baseline from `/tmp`, which changed the self-child executable location and produced a different `driver.Start` failure at line 427. That attempt is discarded as matched runtime evidence. Corrected session 12844 temporarily substituted the baseline at the original `.cache/build/dev/tests/test_world` executable path, ran the full compact suite, and restored the candidate executable in cleanup.

Session 12844 exited 42 with exactly the same totals as candidate compact rerun 90980: 285 cases, 281 passing and four failing; 33,776 assertions, 33,768 passing and eight failing. It reproduced the same `AdoptInheritedChannel` null result at child line 139, readiness failures at lines 523 and 435, `SendTickExchange` and nested-work failures at lines 207 and 222, and `PresentationBus.cpp:405` send failure. These four case failures therefore reproduce without scratch reuse, against the same tests at the original executable path. The new focused analytical/serial-parallel parity fixture passes in both full runs; no introduced failing case was observed in this comparison. This isolates these failures from the scratch change without explaining their cause or turning the failed full-module gate into a pass.


### Rejected Metrics name-resolution hoist

The candidate moved `Name(name)` construction before the sink mutex in `Metrics::Count` and `Metrics::CountTime`, while preserving sink initialization order and keeping counter lookup, value accumulation and sample increments under the existing mutex. It introduced no index, cache, shards or public API. The intended mechanism was a shorter sink critical section by resolving the name through its separately synchronized registry first. The measured tradeoff below rejects this production change; the original `Metrics.cpp` implementation has been restored exactly to SHA256 `352101249547202ea045e88f920d830052254aef4ce7c18bc6dbe68c3b2a4223` from its true preimage. The meaningful concurrency/lifecycle test remains.

Baseline session 65065 completed the existing `engine.core.bench.instrumentation` suite with five samples and eight warmups. It showed minimum normalized Count costs of 27 ns with one thread, 100 ns with four threads and 316 ns with eight threads, with respective spreads of 0, 140 and 33 ns. This ladder includes thread creation and joining, name resolution, lookup and accumulation, so it does not attribute all cost to the sink mutex. The initial four-thread spread particularly limits interpretation of a single baseline minimum.

| Optimized `bench_core` build | Executable SHA256 |
| --- | --- |
| Baseline, preserved at `/tmp/atomic-metrics-before-name-hoist` | `a5541cac17d1623d874163a5ae04f439b94e872a66ce4d698ac42e137ecfd32f` |
| Rejected hoist candidate | `d2f90fa290f2adb8b77253ba051ea78f1a966c585479e36ff8447437d1f3c0be` |

Session 46557 completed four fresh optimized processes in baseline/candidate/candidate/baseline order, running all 26 existing instrumentation rows with five samples and eight warmups: 104 completed rows in total. Preset `bench` uses first-party `-O3` and compiled heap hooks; these are not heap-hook-free shipped-release measurements. Representative minima below are normalized nanoseconds per iteration, not medians. Existing contended rows include thread spawning and joining in both builds. Raw benchmark reports were not written to files.

| Existing workload | Baseline, first pair ns | Candidate, first pair ns | Candidate, reverse pair ns | Baseline, reverse pair ns |
| --- | ---: | ---: | ---: | ---: |
| Count, 64 names | 22 | 22 | 21 | 22 |
| Count, one name | 17 | 17 | 17 | 17 |
| CountTime | 20 | 22 | 22 | 22 |
| ScopedCount | 52 | 53 | 51 | 52 |
| Count contended, one thread | 28 | 27 | 29 | 28 |
| Count contended, four threads | 128 | 178 | 238 | 150 |
| Count contended, eight threads | 331 | 255 | 232 | 319 |
| Count contended, eight threads sharing one name | 168 | 158 | 161 | 166 |

Four-thread minima increased by 39.0625 percent and 58.6667 percent in the two source pairs. Eight-thread minima decreased by approximately 22.96 percent and 27.27 percent, and the shared-name eight-thread row also decreased in both pairs. One-thread results changed direction, and single-thread counter rows showed little or mixed change. The consistent observed four-thread slowdown makes the hoist an unsuitable general replacement despite its eight-thread benefit. This is a workload-dependent rejection, not a universal speed claim or a measured mutex-wait attribution.

The candidate Metrics gate in session 46dd15 passed 2,233 assertions across 17 cases. Full core gate fdebe6 passed 48,484 assertions in 358 cases. After restoring production code and retaining the fixture, dev build 79bd27 completed successfully and full core gate 5bf9db passed 48,489 assertions in the same 358 cases. Assertion counts vary with the bounded drainage polling; case counts and exact conservation checks remain stable. The added bounded fixture starts four writers on fresh count/time names, performs a controlled midpoint Snapshot and Drain, conserves exact per-name values and samples across a single bounded drain consumer and final joined drain, verifies snapshot copies remain unchanged, and reuses names after Drain/Clear with first-writer `IsTime` behavior preserved. These gates establish correctness for the exercised boundaries but do not rescue the measured four-thread tradeoff. The rejected executable is preserved at `/tmp/atomic-metrics-rejected-name-hoist`. The rejected implementation and retained test draft are preserved in `/tmp/atomic-metrics-name-hoist.patch`; that patch includes the rejected production hunk and must not be reapplied wholesale as an accepted change.

Leading and trailing background readings averaged 0.6898786275829174 and 0.6399192489090978 busy cores. The active 18.66483591999713-second interval averaged 2.0723461039675697 cores, with observed excess above the adjacent baseline of 1.4074471657215621 cores. These aggregate CPU differences describe machine load, not corrected benchmark wall times or specific mutex contention. The process namespace does not establish named host-game presence or attribution. No additional implementation is accepted from this experiment.

Git staging was revalidated in session 5e7d85 and failed because `.git/index.lock` is on a read-only filesystem. No commit was made.


The final optimized core benchmark rebuilds in sessions 9792 and e45891 completed successfully. The restored `bench_core` SHA256 is exactly the original baseline `a5541cac17d1623d874163a5ae04f439b94e872a66ce4d698ac42e137ecfd32f`, independently confirming executable restoration. The restored dev core-test executable, including the retained parity fixture, has SHA256 `dac7871395f0d1ffe16f7ffa6db0e1e8a0c60bfc98182969c8998864f9692188`. The retained test-only patch is `/tmp/atomic-metrics-retained-parity-test.patch`, SHA256 `5395ef3ed5501169adb834848535e2586332661319280659dd5146e5f17a55ff`; it excludes the rejected production hunk.


### QuickJS property descriptor ordinal experiment

The existing `engine.scriptjs.bench.property-access` workload now checks an exact native Position oracle after every script run. Its 64, 256 and 1,024 read/write-cycle rows use dyadic increments of 1/64, check the single Part's class and component shape, and compare all three Position components against the native expected values. Verification is inside the broader BENCH boundary in both source builds. These measurements therefore include runtime entry, script execution and native verification; they do not isolate getter cost or prototype construction. The existing command is `just scriptjs-property-access-bench 5`, or `bench_scriptjs --suite engine.scriptjs.bench.property-access --samples 5` under the bench build. Preset `bench` uses first-party `-O3`, compiled heap hooks and compiled on-demand Tracy support. These are not heap-hook-free shipped-release claims or complete all-thread profile captures. Raw benchmark reports remained on stdout.

The production candidate binds a descriptor ordinal as a hint while retaining the original JS name closure and the existing CString conversion and release. Every access resolves the current receiver's merged property span, checks bounds, checks the current spelling and checks `Scriptable`. A moved ordinal or a borrowed accessor whose receiver has another schema falls back to the original name lookup. No descriptor pointer, public schema API or global cache is retained. Source inspection of `Classes::Remerge` established unique merged names and replacement of inherited names in place; descriptors can still move or change after declarations. Receiver identity, lifecycle and schema checks are consequently required on every access.

#### First trial and bounded callback refinement

The first trial kept the hint in an additional JSValue closure slot. Session 63354/fddb7f completed four fresh baseline/candidate/candidate/baseline processes, each running all three rows with eight warmups and five measured samples. Its minima changed by only approximately zero to 1.8 percent, and the final baseline had wide spreads. This trial did not establish a reliable general gain, and its extra closure payload was an unmeasured memory tradeoff. It was not accepted in that form.

| First trial, cycles | Baseline first min / spread ns per item | Candidate first | Candidate reverse | Baseline reverse |
| --- | ---: | ---: | ---: | ---: |
| 64 | 623 / 6 | 616 / 4 | 614 / 4 | 619 / 184 |
| 256 | 437 / 32 | 433 / 7 | 430 / 12 | 430 / 330 |
| 1,024 | 395 / 13 | 388 / 3 | 386 / 4 | 388 / 60 |

The refinement uses the already present callback `magic` field instead. The actual pinned QuickJS implementation stores `JSCFunctionDataRecord::magic` as `uint16_t` at `quickjs.c:6565`, assigns it during function creation at line 6658 and promotes it to the callback's `int` argument at line 6631. Ordinals 0 through 65,534 fit that verified domain; 65,535 is an explicit sentinel for original name lookup. Larger indices are checked before narrowing. Each accessor again retains exactly one JSValue, its original name, with `data_len=1`. This removes the added closure slot rather than assuming an unbounded public `int` storage contract. The original CString behavior, including Unicode conversion and validation exceptions, remains unchanged.

Prototype construction also iterates the merged descriptor span directly and skips non-scriptable entries, instead of building the original temporary filtered pointer vector. This allocation removal is established by source inspection, not measured prototype allocation counters. It is part of the candidate's whole-workload scope and is not separately attributed by these timings.

| Optimized `bench_scriptjs` build | Executable SHA256 | Preserved executable |
| --- | --- | --- |
| Exact-oracle baseline | `bd62595df2ba2f335a36513d7070ce7c4c092c085d4fe812c1c21266831e8f11` | `/tmp/atomic-quickjs-property-access-baseline` |
| First trial, extra JSValue closure | `f02c01e327cd2b2769e5a717073f011dc9bf927412b1e4c83ebead332b4a168c` | First-trial build identity only |
| Final bounded-magic refinement | `6675ff23c8d2dc36adb9fd2b294af3ad81d37669e750f0d072f0be9f75092be1` | `/tmp/atomic-quickjs-property-access-magic` |

Final `JsBindings.cpp` SHA256 is `0084ce1be78555fb4313a8bf0d080e0475ac3a3201b9dc6c2937af102e4c90c7`. The first candidate's true preimage is `/tmp/atomic-quickjs-ordinal-before-magic.cpp`; the refinement-only patch is `/tmp/atomic-quickjs-ordinal-magic-refinement.patch`. The full baseline-to-final implementation patch is `/tmp/atomic-quickjs-ordinal-magic-candidate.patch`.

#### Matched final timing and correctness gates

Dev build 61753/45db72 and optimized benchmark build 41933/a48fe7 completed successfully. Final timing session 89905/429d4f completed all 12 rows across four fresh processes in baseline/candidate/candidate/baseline order, with eight warmups and five measured samples per row. The table reports existing runner minima and spreads, not medians or independently paired getter samples.

| Final trial, cycles | Baseline first min / spread ns per item | Candidate first | Candidate reverse | Baseline reverse | First / reverse candidate change |
| --- | ---: | ---: | ---: | ---: | --- |
| 64 | 626 / 4 | 595 / 3 | 614 / 6 | 609 / 3 | -4.952% / +0.821% |
| 256 | 437 / 13 | 419 / 2 | 423 / 16 | 427 / 35 | -4.119% / -0.937% |
| 1,024 | 393 / 5 | 378 / 98 | 378 / 8 | 390 / 2 | -3.817% / -3.077% |

The 256- and 1,024-cycle minima were lower in both source orders; the 64-cycle row changed direction. The first 1,024-cycle candidate also had a 98 ns spread. The final refinement is accepted as a qualified improvement for the exercised property-access workload, with no universal gain or isolated getter-speed claim.

The new shared fixture checks inherited name shadows, current schema replacement and insertion, scriptable and writable changes, Unicode names and values, conversion refusal, borrowed JavaScript accessors on another receiver class, missing properties, wrong receivers and destroyed handles. Its first run, session 600771, failed because the fixture incorrectly assumed a Luau local/global binding persisted across separate `Run` calls. The fixture was repaired to reacquire named instances from the workspace on each Luau call; repaired baseline gate 71a40d passed 34 assertions in one case. This was a fixture defect, not a production optimization failure. Existing baseline script-call gate 3d4aea passed 1,338 assertions in 24 cases.

The final candidate union of property-access and script-call cases, session ca1b65, passed 1,372 assertions in 25 cases. Unchanged related production boundaries were also covered by InstanceShim gate ba0fb3, passing 38 assertions in four cases, and existing inherited/field-conversion ECS gate f18810, passing 18 assertions in two cases. The earlier `[ecs]` attempt in session 520050 selected the wrong executable and matched no tests, exiting 2; it is not a passing gate. Formatting and isolated syntax checks passed before the final builds.

For the first trial, adjacent background readings averaged 0.6848772432881958 and 0.6649126913134304 busy cores. The active 0.9981021970015718-second interval averaged 0.7414070445121309 cores, with observed excess 0.0665120772113178 above the adjacent baseline. For the final trial, adjacent readings averaged 0.6748707524675094 and 0.7598990219592758 cores. The active 0.999123151996173-second interval averaged 0.7006143322787112 cores, with observed excess of -0.01677055493468138 cores. This negative difference illustrates changing ambient load and short-interval CPU quantization. Aggregate subtraction describes load only, supplies no isolated benchmark CPU attribution and does not correct benchmark wall times. The process namespace does not establish named host-game presence.

No commit was made because Git metadata is read-only. The accepted production refinement and its meaningful fixtures remain scoped changes with preserved preimages and patches.


### SQLite snapshot profiling and validated-buffer transfer

The existing `engine.datastore.bench.sqlite-snapshot` fixture preserves its unsorted 1,024-entry by 256-byte Save input. A separate expected copy is ordered once by key spelling outside the measured calls, matching the portable image's canonical order. Setup verifies the seeded complete image, and every measured Load compares all keys, values, versions and store kinds against that expected copy. Save remains a Save-only row; verification through a separate Load is not added to its measured body. The existing `just datastore-sqlite-bench 5` job prints reports to stdout.

The diagnostic baseline adds fixed load/save/open/prepare/schema/encode/decode/step `ENGINE_PROFILE` scopes. Setting `ATOMIC_SQLITE_PROFILE=1` on the existing benchmark executable enables a bounded owner FrameGraph frame around each invocation, labels the eight warmups, reports phase inclusive and self times separately, exposes enclosing frame and unmarked time, and refuses missing required phases, invalid hierarchy or new frame/heap drops. It permits at most 128 calls per row. Counter reads and stdout reporting belong to the broader diagnostic BENCH boundary; captured and uncaptured timings are separate runs. Preset `bench` uses effective first-party `-O3`, compiled heap hooks and on-demand Tracy support, not a heap-hook-free shipping configuration. No raw benchmark report files were written.

Heap attribution covers intercepted C++ new/delete allocations. The pinned SQLite allocator calls C `malloc`, outside these hooks, so SQLite's internal connection, statement and page-cache allocations remain untracked. Reported phase allocated bytes/blocks, live bytes/blocks, sum of tag high-water peaks and process profiler overhead are distinct counters. A sum of tag peaks is not a simultaneous process peak or RSS.

Baseline dev builds 50388/d610db and 8db271 completed successfully; focused SQLite gate 05b029 passed 45 assertions in four cases. Benchmark builds 52708/e27d33 and 8d535d0 completed successfully. Baseline uncaptured timing session 75358/296b43+d14dfb measured Save at 526,879 ns minimum with 7,613 ns spread and Load at 264,006 ns with 3,726 ns spread, using five samples and eight warmups.

Baseline capture completed 26 invocations, 13 each for Save and Load, including five measured calls each. Save retained six spans per frame and Load five, with zero frame and heap drops. Measured phase mean inclusive durations were:

| Baseline captured operation | Whole parent ms | Codec ms | Open ms | Prepare ms | Schema ms | Step ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Save | 0.5871592 | Encode 0.4085804 | 0.0124672 | 0.0017832 | 0.0123314 | 0.1231672 |
| Load | 0.2098414 | Decode 0.1586994 | 0.0069592 | 0.0091470 | Not executed | 0.0278184 |

These inclusive phase durations locate substantial codec cost and relatively small connection/setup cost in this workload; they are not summed as independent parent cost or extrapolated to all SQLite use cases. Encode allocated 905,464 intercepted C++ bytes in 1,051 blocks per call, while Decode allocated 336,568 bytes in 2,056 blocks. No persistent connection or prepared-statement cache was introduced.

#### Measured copy elimination and correctness

`EncodeSharedStoreImage` now transfers `ByteWriter::TakeBytes()` into the caller's output after the existing final 256 MiB size validation, replacing the final byte-span copy. The API moves the default-allocator vector out of the local writer. Validation, lexical ordering, versions and wire schema are unchanged; every refusal still precedes output replacement. Source inspection covered the disk-save, SQLite-save and HTTP-request-body callers. The change eliminates a copy allocation but transfers the writer's growth capacity, which can exceed the encoded payload size. Callers can retain that capacity until destroying or replacing the output. Allocation churn reduction is therefore not a claim that every residency measure or caller's retained capacity decreases.

| Optimized datastore executable | SHA256 | Preserved executable |
| --- | --- | --- |
| Scoped exact-oracle baseline | `33a6bb9b79bb8f999bfd6170e29519e48f5be8ff719d5812a0493eeebe74c921` | `/tmp/atomic-sqlite-diagnostic-baseline` |
| Validated-buffer transfer | `9005659d93bbac628e1611b682f7b979954253b2b9f1a13e1694649d644c7372` | `/tmp/atomic-sqlite-transfer-candidate` |

The baseline dev test executable is preserved with SHA256 `f1ca390341f09b3c66cefce1a8f09ebc43c4c6f000fc2c0b0b9972e2e088e681`. Final production `SharedStoreFile.cpp` SHA256 is `e02f4c716bfc5f03d5be7316c8393b3c96a702c4256e9ad3779f86af840018c4`; its scoped source/test patch and true preimages are `/tmp/atomic-shared-store-transfer.patch` and `/tmp/atomic-shared-store-transfer-preimages/`.

The candidate build completed successfully. Focused SharedStoreFile tests passed 40 assertions in four cases, and full datastore passed 70 assertions in ten cases. The new image case fixes the exact 58-byte format-one output independently of the encoder, checks repeated/reordered input equality, complete decode equality, duplicate-key/zero-version/unsupported-store refusal without output mutation, and safe output replacement when it aliases a source value after all input bytes are consumed. Existing atomic replacement and trigger-induced rollback coverage remains. The new same-adapter SQLite lifecycle case verifies external path replacement while retaining the old inode, removal/refusal without caller mutation, recreation, externally renamed schema refusal and subsequent schema recovery. These exercise existing synchronous behavior without asserting a nonexistent persistent cache contract.

Separate matched baseline/candidate captures each completed 26 records with six Save spans and five Load spans, zero frame drops and zero new heap drops. For the Encode tag, allocation changed from 905,464 bytes in 1,051 blocks to 617,789 bytes in 1,050 blocks: 287,675 fewer intercepted C++ bytes and one fewer allocation per call. The sum of tag high-water peaks changed from 608,903 to 465,174 bytes; post-call Encode live counters remained 16 bytes in one block in both builds. Decode remained at 336,568 bytes and 2,056 blocks, with sum of tag peaks 631,464 bytes. These counters do not include SQLite C allocations. The five measured calls' mean Encode inclusive duration changed from 0.4031126 to 0.3589696 ms, while the captured enclosing Save frame changed from 0.5938864 to 0.6060512 ms. These diagnostic timings do not establish an end-to-end speed gain.

#### Uncaptured counterbalance and load limits

Four fresh uncaptured processes completed the two rows with five samples and eight warmups in baseline/candidate/candidate/baseline order. Existing runner minima and spreads were:

| Operation | Baseline first min / spread ns | Candidate first | Candidate reverse | Baseline reverse |
| --- | ---: | ---: | ---: | ---: |
| Save | 537,418 / 9,529 | 537,830 / 9,467 | 532,039 / 10,319 | 554,630 / 50,596 |
| Load | 295,464 / 167,445 | 274,766 / 98,194 | 267,392 / 32,711 | 257,042 / 4,299 |

Both operations changed direction between source pairs, with substantial Load variance. The transfer is accepted for its measured allocation reduction and passing exact codec/provider gates, qualified by mixed timing and retained-capacity behavior. No consistent or universal speed gain is claimed.

| ABBA process | Leading / trailing background busy cores | Active busy cores | Benchmark child CPU cores |
| --- | --- | ---: | ---: |
| Baseline first | 1.79976568 / 1.78962020 | 2.06938242 | 0.0796965 |
| Candidate first | 2.32962002 / 2.86929612 | 4.04255574 | 0.0769506 |
| Candidate reverse | 2.77970766 / 1.59944380 | 2.06841238 | 0.0803092 |
| Baseline reverse | 1.98971542 / 1.72940836 | 1.87804866 | 0.0817990 |

The earlier baseline diagnostic window had leading/trailing readings of 0.769844563759811 and 0.7298591714800422 cores, active load 0.828352417172832 cores over 0.5794635109996307 seconds, and observed excess 0.07850054955290542 above the adjacent baseline. Machine-load differences describe changing aggregate load only and do not correct wall timings or isolate SQLite CPU costs. The visible process namespace cannot verify Barotrauma, Steam or other named host processes. Git metadata remains read-only; no commit was made.


### Metrics histogram report scratch allocation

The existing `engine.core.bench.instrumentation` Snapshot row seeds 64 counters and eight full 1,024-observation histograms, takes 100 snapshots, then clears the sink. Its measured setup and workload remain unchanged. A one-time preflight before warmup independently checks all counter fields, canonical name ordering, all histogram aggregates and percentiles, repeated non-draining reads and snapshot ownership after Clear. The focused mixed-window fixture checks full, short, tied and singleton distributions, time metadata and retained snapshot values after later writes and Clear. Existing wrapped-window and concurrent-sink coverage remains.

The opt-in `ATOMIC_METRICS_SNAPSHOT_PROFILE=1` fixture places each Snapshot and returned-object destruction inside one fixed `ENGINE_PROFILE` root and an owner FrameGraph frame. It validates the single-root hierarchy and refuses missing spans or new frame/heap drops. Each of 13 BENCH invocations has 100 complete frames and spans; eight invocations are labeled warmup. Inclusive, self, enclosing frame and unmarked durations remain separate. Heap attribution covers intercepted C++ new/delete, not stack storage or C allocations. Broader capture/report overhead is included only in diagnostic BENCH timings. The optimized `bench` preset includes heap hooks; it is not the heap-hook-free shipping release configuration. Reports remained on stdout through the existing instrumentation benchmark suite.

The candidate replaces the temporary sorted vector in `Rendered` with a local fixed 1,024-double array. Only the fully copied retained prefix is sorted and indexed, preserving the original rank rounding, clamp, copy order, report fields, output ownership and sink locking. The trade is 8 KiB of stack scratch per active Rendered call. This removes temporary heap allocation from histogram rendering, not the returned Snapshot containers, and makes no per-tick or dominant-server-cost claim.

| Optimized core executable | SHA256 | Preserved executable |
| --- | --- | --- |
| Oracle/profile baseline | `eac58fbc9b5039a8b6b7b2a80004972848bf0f21a241d235a6976c92e39398a1` | `/tmp/atomic-metrics-scratch-baseline` |
| Fixed scratch candidate | `e78b8bce102b2249c2f81137bebaf20b07050036ddfadfbe892759d45d1c7b7e` | `/tmp/atomic-metrics-scratch-candidate` |

Baseline Metrics.cpp SHA256 was `352101249547202ea045e88f920d830052254aef4ce7c18bc6dbe68c3b2a4223`; candidate SHA256 is `384e4693178c0d7f9f9b7f0d41d54698af67662b36c0e85ee726b1738fd01a44`. The oracle/profile stage patch and true preimages are `/tmp/atomic-metrics-snapshot-baseline.patch` and `/tmp/atomic-metrics-snapshot-baseline-preimages/`. The scoped production patch is `/tmp/atomic-metrics-percentile-scratch.patch`, with true source preimage `/tmp/atomic-metrics-before-percentile-scratch.cpp`.

Baseline dev build completed successfully and gate 231122 passed 2,531 assertions in 18 Metrics cases. Candidate dev build completed successfully and full core gate c8fffb passed 48,502 assertions in 359 cases. Both optimized benchmark builds completed successfully. Baseline uncaptured reconnaissance 656a4a measured Snapshot minimum/spread 36,391/1,134 ns per call; this precedes the counterbalanced run and is not substituted for its baseline.

| Uncaptured matched Snapshot row, five samples after eight warmups | Min / spread ns per Snapshot |
| --- | ---: |
| Baseline first, 245c91 | 37,028 / 1,638 |
| Candidate first, 575de1 | 34,794 / 1,230 |
| Candidate reverse, 4ebae5 | 34,800 / 1,406 |
| Baseline reverse, 24c173 | 36,352 / 373 |

The candidate minimum was 6.033% and 4.269% lower in the two source orders for this fixture. These include seeding and clearing amortized across 100 calls. They do not isolate sorting or establish a universal speed gain.

Baseline capture 341b69+a6b497 and candidate capture 309935 each completed all 1,300 owner frames and spans with zero frame drops and zero new heap drops. After the first invocation's additional eight-byte allocation, every subsequent 100-Snapshot invocation changed from 6,771,200 allocated bytes in 1,000 blocks to 217,600 bytes in 200 blocks. This is exactly 65,536 fewer intercepted heap bytes and eight fewer allocations per Snapshot. Both builds retained eight bytes in one block after the call. Sum of tag high-water peaks changed from 10,376 to 2,184 bytes; process profiler overhead was 9,696 bytes in both. These are separate counters, not RSS or simultaneous process peaks; stack scratch is not counted by heap hooks.

The five measured captured invocations' mean inclusive/self duration per 100 Snapshots changed from 3.5584694 to 3.7795044 ms. Diagnostic BENCH minimum/spread changed from 38,251/3,622 to 40,301/7,546 ns per Snapshot. Thus the captured timing regressed despite lower heap allocation. The candidate is accepted for the exact allocation reduction and passing parity gates, with fixture-specific uncaptured improvement and mixed capture timing reported separately.

| ABBA process | Leading / trailing background busy cores | Active busy cores | Active minus adjacent background mean | Benchmark child CPU cores |
| --- | --- | ---: | ---: | ---: |
| Baseline first | 0.77989096 / 0.89980525 | 2.11204250 | 1.27219439 | 1.38548318 |
| Candidate first | 1.94965996 / 0.90988575 | 2.75551658 | 1.32574372 | 1.36154813 |
| Candidate reverse | 1.00982017 / 1.84961107 | 3.58995662 | 2.16024101 | 1.36435118 |
| Baseline reverse | 1.11978085 / 1.81970128 | 3.02458557 | 1.55484451 | 1.36792862 |

Leading and trailing windows lasted one second each; the active windows cover the whole instrumentation suite, including contended rows, not Snapshot alone. Active minus adjacent mean background gives descriptive aggregate excess, with changing host load and sampler overhead still included. Child rusage separately covers startup and exit; process interval deltas miss final exits. No aggregate CPU subtraction corrects wall timings or isolates Snapshot CPU. The visible namespace cannot establish whether Barotrauma or Steam are running on the host. Git metadata remains read-only and no commit was made.


### Control discovery exact oracle and complete baseline capture

The existing `engine.control.bench.mcp-control` workload exercises the real data-capture provider through `Surface::Answer`, without a socket or GUI. The tools-list row keeps eight internal warmup requests and 16 separately timed requests per BENCH invocation. A preflight before those requests builds the complete expected ordered tool metadata and input schemas from registered provider descriptors and compares two full JSON-RPC response envelopes. It runs once per observed hook generation outside the internal measured requests. The preflight is part of the broader BENCH batch boundary, so the BENCH row is not one discovery request and its time must not be confused with the internal per-request distribution. No resource/prompt benchmark rows were added: this provider does not install them, and empty-list measurements would not establish populated discovery cost.

The source contract prevents a generic registration-only schema cache: `Tool::Schema` is a callable without a purity or schema-revision promise, and the current list implementation invokes it on every request. The new meaningful fixture verifies callback counts, changing schema values, exception propagation/recovery and retained response copies. Another fixture verifies resource and prompt metadata and lazy callbacks, cleanup-tool visibility during drain, hidden resource/prompt rows, terminal removal, renewed generations and new metadata under reused keys. Source inspection confirms `Answer` reaps hooks before parsing, which can change visibility. Resource/prompt lifecycle correctness is checked here, but their listing cost is not measured by this workload.

Fixed `ENGINE_PROFILE` scopes now cover Answer, tools/resources/prompts list construction and per-tool schema construction. There is no second source timing system. The existing internal request timer and global heap-block distribution remain in the benchmark. `ATOMIC_CONTROL_DISCOVERY_PROFILE=1` adds bounded owner FrameGraph capture around the same 16 timed tools-list operations, validates required phases and parent/depth hierarchy, exposes unmarked time and refuses new frame or heap drops. Internal warmups remain uncaptured; the eight BENCH warmup invocations are labeled. The disabled path retains the original timed request loop without a capture object. Captured timings include collector overhead and are separate from uncaptured results. The existing `just mcp-control-profile` job remains unchanged and prints to stdout; no raw report files were written.

| Preserved baseline artifact | SHA256 |
| --- | --- |
| Optimized `bench_control`, `/tmp/atomic-control-discovery-baseline` | `8d64791dac67d70c166f4b92666b410ade407e83ee8cd69d4ee9e1ae53ade232` |
| Dev tests, `/tmp/atomic-control-discovery-baseline-test` | `a78902aad1b43c84aa97f637562a3c2ff30eabd2bee5e78637c57a751bff2edf` |
| Source/oracle/test stage patch, `/tmp/atomic-control-discovery-baseline.patch` | `46ae0d26c6d6bb025a4aca97bf3ebfe52ba04836c334e138ba4ab6c033fbaff7` |

The stage owns only Surface.cpp, benchmarks/McpControl.cpp and tests/HookRegistry.cpp, with true preimages under `/tmp/atomic-control-discovery-baseline-preimages/`. Final hashes are respectively `8679498721933223f1928a12b4a3f885c2ced2d695a45d95caba940018eb9688`, `a4d6e5eb1fc5a7583fa996baba70a79cb1eb77f34265e0dae1e060891182d3b5` and `e35de93a3794d49fe63035228c4ab37009ebdb4f1d54dea46a694c48c3bb478f`. Formatting, scoped diff and individual syntax checks passed. Dev build 34122/4bcccd+147a17+ac0a6b completed successfully. Focused discovery gate a9107b passed 200 assertions in ten cases; full control gate 2e34eb passed 741,636 assertions in 146 cases. Optimized build 89506/846bee+964170 completed successfully.

Uncaptured run 33571/9afe6e+85bc8f used five samples after eight BENCH warmups. The tools-list BENCH batch minimum/spread was 3,696,344/52,348 ns. The internal distribution includes 208 requests from both BENCH warmups and measured invocations: p50 153,959 ns, p99 185,268 ns, maximum 194,665 ns, and 2,561 intercepted heap blocks at p50/p99/maximum. These figures are workload baseline evidence, not a before/after improvement.

Captured run 240880 completed 13 batches, 208 owner frames and 2,704 spans, with zero frame drops and zero new heap drops. Each request retained an Answer root, a tools-list child and 11 schema children. The five measured batches' mean durations per 16 requests were:

| Captured baseline phase | Inclusive ms | Self ms | Steady exclusive-tag allocated bytes / blocks per 16 requests |
| --- | ---: | ---: | ---: |
| Answer | 2.5115724 | 0.5891226 | 349,248 / 896 |
| Tools list | 1.9224498 | 0.1700646 | 155,456 / 3,568 |
| Tool schemas, aggregated | 1.7523852 | 1.7523852 | 1,519,616 / 36,512 |

The enclosing frame mean was 2.5152742 ms and unmarked mean 0.0037018 ms. Schema scopes accounted for 69.772% of Answer inclusive time in this fixture. Inclusive parent durations overlap child durations and are not summed as independent cost. Allocation figures are exclusive heap-tag churn, not inclusive parent bytes. Answer and tools-list post-call live counters were zero; schema post-call counters held 32 bytes in one block after first invocation and remained flat. Sum of tag high-water peaks was 15,203/5,549/41,854 bytes for Answer/tools/schemas, with process profiler overhead 7,040 bytes. These counters cover intercepted C++ new/delete, not C allocation, stack storage, RSS or simultaneous process peak. The `bench` preset is optimized with compiled heap hooks, not the heap-hook-free shipping release. Captured BENCH batch minimum/spread was 3,954,689/73,057 ns; its internal all-invocation tools-list p50/p99 was 154,850/179,877 ns. No timing gain is claimed.

Adjacent one-second uncaptured background windows averaged 1.4896832650322274 and 1.7295869919184192 busy cores. The active whole-suite window averaged 2.3244801885821946 cores over 0.31404870800179197 seconds, giving descriptive excess of 0.7148450601068714 cores over adjacent mean background. Complete child rusage was 0.07397 CPU seconds, or 0.23518649021876664 cores over launch-to-final observation. The short process-interval sample missed the child's final exit; machine differences include changing ambient load and sampler overhead. They do not correct wall timings or isolate tools-list CPU. Host Barotrauma/Steam presence is not established by the visible process namespace.

The next candidate is source-backed JSON copy elimination while preserving every live schema callback. No discovery cache, schema-purity contract or performance implementation has been accepted from this baseline stage. Git metadata remains read-only and no commit was made.


### Control capture-option schema ownership transfer

The candidate changes exactly four lines in `control/features/DataCapture.hpp`: the two local `optionProperties` objects are mutable and moved once into their respective nested `properties` fields. Both locals have a single consumer and no later reads. The pinned nlohmann JSON initializer-list adapter copies a referenced const lvalue and transfers an owned rvalue. This conclusion comes from the checked-out `json_ref.hpp` and constructor code, not an assumption that all initializer-list members copy. The public constructor reference is [JSON for Modern C++](https://json.nlohmann.me/api/basic_json/basic_json/). Every live schema callback remains invoked on each request; no cache or new purity contract was introduced.

Before the production edit, a control-owned frozen fixture was extracted from the complete factory contracts in `mono.client/tests/fixtures/McpContracts.json`, source SHA256 `570f737172e9fb8071c73bbad24a491410643ee2bcef07968ce6fd2d046a76fb`. Root independently verified exact full-tree equality for both capture schemas, including nested field bounds, required arrays and additional-property rules. The fixture has no runtime dependency on the client module. Baseline build 7297/96db45+892c82 passed; gate 653e04 passed eight assertions in the discovery-schema case and 465 assertions across 13 data-capture cases. The preserved baseline oracle test executable is `/tmp/atomic-control-schema-oracle-baseline-test`, SHA256 `2cae9929a5c7a51e10de69d1515a1d251853a17eb38bb5162fd94f5d273d2577`. The oracle patch is `/tmp/atomic-control-schema-oracle.patch`, SHA256 `9b404d1c520107936c47bc4f7304527d40c281130d80764e2088a278226ef2d9`.

Candidate dev build 23618/42dbe7+669de0 and optimized build 6545/c936f8+ba3468 passed. Full candidate control gate 74bcb9 passed 741,644 assertions in 147 cases, including dynamic-schema invocation/exception recovery and hook lifecycle checks. Expected negative-case ECS/physics errors were printed; the overall test exit was zero. Production preimage is `/tmp/atomic-control-before-option-schema-move.hpp`, SHA256 `dc9d089b1e61ec913c9b026f52cbffb68b1c78bfe4df22a5e884006dea193d87`; final header SHA256 is `31d835994efbfbd2a41e574b4216a087e09075e20f33f2494f918b74bc3546e7`. The scoped production patch is `/tmp/atomic-control-option-schema-move.patch`, SHA256 `55ffe7dbb00f8b1e43b67160d3dbd506587ba8d4586efbf6290f2a1e27d994ed`. Changed lines match clang-format; existing whole-header formatting violations were not included in this four-line change.

The matched preserved optimized executables are baseline `/tmp/atomic-control-discovery-baseline`, SHA256 `8d64791dac67d70c166f4b92666b410ade407e83ee8cd69d4ee9e1ae53ade232`, and candidate `/tmp/atomic-control-option-schema-move-candidate`, SHA256 `644067b6774b888efe290fe72d6e2234b64f6175e0d871291a10e115302d3892`. Both use the same oracle/profile benchmark source, five samples and eight BENCH warmups. Other agents' compilers were idle throughout the four uncaptured runs and two captures. Raw reports remained on stdout.

| Uncaptured tools-list batch | Min / spread ns | Internal all-invocation p50 / p99 ns | Intercepted blocks p50 |
| --- | ---: | ---: | ---: |
| Baseline first, 2760e9+f369b5 | 3,660,982 / 166,514 | 152,417 / 212,650 | 2,561 |
| Candidate first, 3f1156+01733d | 3,500,860 / 105,910 | 145,734 / 177,724 | 2,388 |
| Candidate reverse, 35d997+ad0450 | 3,498,976 / 32,472 | 149,020 / 186,971 | 2,388 |
| Baseline reverse, 12c718+8285c1 | 3,753,655 / 132,320 | 153,830 / 222,518 | 2,561 |

Candidate batch minima were 4.374% and 6.785% lower in the two source orders for this fixture. The broader batch includes eight internal warmups and 16 timed requests, not one request. Internal percentiles include all 208 requests from warmup and measured BENCH invocations. Other suite rows are unchanged and are not claimed as improvements.

Matched captures 050341 and 1c10d9 each retained 208 frames and 2,704 spans with zero frame/heap drops. Steady schema-tag allocation per 16 requests changed from 1,519,616 bytes in 36,512 blocks to 1,335,008 bytes in 33,744 blocks: 11,538 fewer intercepted bytes and 173 fewer allocations per request. Answer and tools-list exclusive allocations remained 349,248/896 and 155,456/3,568 bytes/blocks per batch. Schema post-call residency remained 32 bytes in one block, with zero live bytes for the other two tags. Sum of tag high-water peaks remained 15,203/5,549/41,854 bytes; profiler process overhead remained 7,040 bytes. These counters cover C++ new/delete, not C allocation, stack storage, RSS or simultaneous peak residency.

| Five measured captured batch means | Baseline ms | Candidate ms |
| --- | ---: | ---: |
| Whole frame | 2.4692414 | 2.3497000 |
| Unmarked | 0.0035518 | 0.0031268 |
| Answer inclusive | 2.4656896 | 2.3465732 |
| Answer self | 0.5768346 | 0.5826712 |
| Tools-list inclusive | 1.8888550 | 1.7639020 |
| Tools-list self | 0.1677376 | 0.1706584 |
| Schema inclusive/self | 1.7211176 | 1.5932432 |

Captured BENCH batch minimum/spread changed from 3,905,782/55,404 ns to 3,683,353/115,447 ns. The schema and parent captured means decreased, while Answer/tools-list self means increased slightly. Capture timings include instrumentation and remain separate from uncaptured results. The candidate is accepted for exact allocation reduction, passing full contracts and qualified fixture timing evidence; no universal discovery speed claim is made.

| ABBA process | Leading / trailing busy cores | Active busy cores | Active minus adjacent mean | Child CPU cores |
| --- | --- | ---: | ---: | ---: |
| Baseline first | 0.73986492 / 0.39990295 | 0.63692680 | 0.06704287 | 0.23751246 |
| Candidate first | 1.74970348 / 1.51960276 | 1.75115377 | 0.11650064 | 0.22913477 |
| Candidate reverse | 2.99941264 / 2.89918621 | 2.77101456 | -0.17828486 | 0.23053507 |
| Baseline reverse | 1.46976647 / 2.38939040 | 1.71944766 | -0.21013078 | 0.23947117 |

Adjacent windows lasted one second; active windows cover the whole suite and lasted about 0.314 seconds. Child CPU uses complete rusage, while process interval observations missed child exits. The reverse candidate also observed about 0.956 iowait cores in its active window. Negative aggregate differences reflect changing background load; subtraction is descriptive and never corrects wall timings or isolates tools-list CPU. Named host-game presence remains unverified. Git metadata is still read-only, so no commit was made; owned preimages, patches and verified binaries are preserved.

## Pinned-worker dispatch: diagnostic baseline, candidate not yet implemented

The unchanged scheduler passes the dev jobs gate: 10,084 assertions in 25 cases. The bench preset diagnostic adds one whole `jobs.assigned` owner scope and records each entered worker's completed drain duration plus its existing accumulated body duration. The owner reports the producer hierarchy after the guarded join. The dispatch algorithm, task retirement and `notify_all` behavior are unchanged. A preflight checks complete 64/1024-task output, exact-once visits, ascending per-worker order, processor and thread identity, participant totals, and round-robin/skew placement. No pinned prefix is an explicit unavailable fixture rather than a successful fallback measurement.

With `ATOMIC_PINNED_DISPATCH_PROFILE=1`, five samples and eight outer warmups, 23 workers and 12 physical-core pinned workers produced 32,500 owner frames, 1,552,068 spans and 743,534 entered-worker records. Each completed frame was consumed before starting the next frame; all owner/join/producer/body phase counts, parentage and reported marks passed. Frame span drops and heap-scope drop deltas were zero. These are reconstructed completed-producer aggregates, not a retained whole-run scheduler flame graph or direct Tracy all-thread history. Producer work overlaps the owner and other producers and must not be subtracted from wall time.

| Five measured sample means, per dispatch | 64 tasks, microseconds | 1024 tasks, microseconds |
| --- | ---: | ---: |
| Owner inclusive | 34.81416 | 52.83072 |
| Owner self | 11.24429 | 11.18971 |
| Owner join idle | 23.56985 | 41.64104 |
| Sum of entered producer inclusive | 10.22659 | 321.47200 |
| Sum of producer scan/retirement/timer self | 8.65274 | 294.90440 |
| Sum of producer body | 1.57385 | 26.56732 |
| Owner unmarked | 0.08430 | 0.09199 |

Producer residual includes scanning, retirement and timer overhead, and excludes wake/lock time outside the measured drain. It is not an isolated scan counter. Captured BENCH minimum/spread was 31,193/7,852 ns for 64 tasks and 51,657/10,800 ns for 1024 tasks; these include diagnostic work and are separate from the earlier uncaptured baseline.

The pool retains 552 C++ heap bytes in one diagnostic-reading block. Each warmed captured batch observed 5,206 allocated C++ bytes in 14 blocks, including report/printing work. Repeated calls had the same before/after residency levels within each task count: 15,434,336/15,439,542 bytes for 64 tasks and 15,438,176/15,443,382 bytes for 1024 tasks. These are distinct snapshots, not zero within-call growth. The first capture additionally allocated FrameGraph storage and is labeled warmup. Profiler process overhead was 3,136 bytes. These figures are C++ allocation counters, not RSS, C allocation or inferred task payload.

Adjacent one-second background windows observed 1.23978673 and 0.79980473 busy cores. The suite's 6.03438179-second active window observed 8.30408180 busy cores. Active minus the adjacent mean is 7.28428607 busy cores, descriptive only. Complete child rusage was 47.948964 CPU seconds, or 7.94531022 busy cores over launch to final observation. Process interval deltas missed the child exit. No wall-time correction or named host-game inference was applied.

The next candidate is a measured per-worker task ordinal plan. It remains unimplemented at this gate. It must pass a complete million-row contention oracle, the placement/order gates, small-task controls, matched timing, and allocation/lifetime checks before acceptance. TCP/UDP socket creation still returns EPERM, so the matched 200-client Authority and direct Tracy rerun remain unverified. Git metadata remains read-only; no commit was made.

Verified diagnostic bench binary SHA-256: `efdf7a59cf86fb0931d84e065f11079a08e0dafe47b8cdacdacb780115c9fdb1`. Owned diagnostic patch SHA-256: `05c8f8f5e0e46b63e01dcf5a7b5ce55708a2b188b8675f53e00734ae1762b383`. Raw measurements stayed on stdout.


## pinned assignment plan candidate, September 30

candidate rejected and restored to the measured scan implementation. no speedup claim.

The bench preset used 23 workers and a 12-worker physical-core pinned prefix. The baseline binary hash is
`79fe41a296ffd7df1055526492ff8771670b544a80930b4bc2fac3e057b4fd9f`; candidate is
`e7bf3332739d1bfe7756b81d4719d9e9352a839cbdc549b1d183c1ac8bdb6003`.
Both include the same irregular placement and independent full-million-row output oracles. The candidate
builds retained per-worker heads and per-task next ordinals after each successful pool claim, preserving
ascending per-worker task order. Caller participation and fallback paths are unchanged.

Correctness passed: Jobs 10,084 assertions in 25 cases, child exit check, dispatch placement/order and
round-robin, skewed and irregular full-row comparisons. The temporary candidate patch is preserved at
`/tmp/atomic-pinned-head-next.patch`, SHA256
`e3ea3e0151898f7a05e24bc792fe49854c74db45a42006e403634a089463fd6f`.

Five samples per fresh process, A/B/B/A order. Each cell is minimum/spread in nanoseconds:

| workload | A1 | B1 | B2 | A2 |
|---|---:|---:|---:|---:|
| 64 empty pinned tasks | 33338/2481 | 36794/70781 | 30827/6870 | 90426/19139 |
| 1024 empty pinned tasks | 47161/18259 | 51033/57109 | 47699/12820 | 54732/54444 |
| 1M rows, 64 pinned tasks | 543409/9437 | 578013/5601 | 581670/170931 | 532398/7904 |

Empty dispatch results are confounded: adjacent background-only readings for A1/B1/B2/A2 were
0.990/0.820/0.940/8.779 busy cores before and 0.780/0.880/0.950/5.058 after. Active readings were
8.337/6.641/8.182/9.761 busy cores. Complete child CPU was 47.196/39.847/43.422/38.691 seconds.
No host game identity is inferred from these observations. Subtracting background CPU is descriptive;
it does not correct benchmark wall time.

Real-work candidate minima were 6.368% and 9.255% slower against their adjacent baselines. Its before
background readings were 2.649/2.860/2.870/2.909 cores, after 2.469/1.490/1.410/2.849, active
4.341/3.420/3.577/3.421; complete child CPU was 1.068/0.988/1.030/1.008 seconds. These short
processes exited before process-counter sampling could capture their CPU. Child rusage supplied complete
CPU, and machine observations include other work. This supports rejection, not a precise universal slowdown.

The candidate capture consumed 32,500 frames, 1,582,886 spans and 742,693 entered-worker records across
26 calls, including eight warmup and five measured calls per task count. All 32,500 owner plan spans,
parent/phase/producer assertions passed; FrameGraph drops and heap-scope drop deltas were zero.
The five measured-call means below are milliseconds per dispatch. Producer totals can overlap and are
not subtracted from owner self time:

| phase | 64 tasks | 1024 tasks |
|---|---:|---:|
| owner inclusive | 0.03866231 | 0.05713404 |
| owner self, excluding plan and join | 0.01211530 | 0.01171381 |
| owner plan | 0.0001737791 | 0.002201616 |
| join idle | 0.02637323 | 0.04321864 |
| producer inclusive sum | 0.01356886 | 0.2557972 |
| producer residual sum | 0.01197842 | 0.2295116 |
| producer body sum | 0.001590431 | 0.02628588 |
| unmarked | 0.0000921302 | 0.00009456984 |

Plan storage retained 696 C++ bytes at 64 tasks and 8,376 at 1024, two blocks each. These are measured
heap-tag totals, not inferred entity bytes. Every measured call allocated zero bytes/blocks in the plan tag.
The first captured 64-task warmup attributed 16 bytes in one block to the plan tag; the precise allocation
source was not isolated. The diagnostic reading storage remained 552 bytes in one block. Warming capture itself
allocated FrameGraph storage; warmed complete calls allocated 6,516 bytes in 18 process-wide C++ blocks,
including printing/reporting, and increased snapshot live bytes by that amount within each call. Profiler
overhead was 3,328 bytes. This is C++ allocation evidence, not C allocation, RSS, stack, or all-thread Tracy.

Capture background before/active/after was 1.400/7.693/7.218 busy cores; complete child CPU was
43.127 seconds over 5.768 seconds through final observation. Captured benchmark minima/spreads were
36820/9505 and 53136/19052 ns. Captured and uncaptured wall timings are separate results.

The candidate reduced some producer residual work but introduced owner plan work and retained memory.
Matched wall-time evidence did not justify keeping it. Only its scoped production and diagnostic-extension
changes were restored; earlier producer diagnostics and the full-row/irregular correctness fixtures remain.
Raw benchmark output stayed on stdout. Socket access still prevents the required 200-client rerun, and
read-only `.git` prevents committing the preserved owned patches.


## Name interning baseline, September 30

The existing `engine.core.bench.names` suite passed all 17 rows in the bench preset, five samples each.
The preserved unchanged binary is `/tmp/atomic-name-hit-baseline`, SHA256
`e78b8bce102b2249c2f81137bebaf20b07050036ddfadfbe892759d45d1c7b7e`.
Selected minimum/spread readings, nanoseconds per operation:

| workload | minimum/spread |
|---|---:|
| cycling 4,096 interned texts | 24/0 |
| repeated interned literal | 13/0 |
| construction, 1/2/4/8 threads | 22/0, 34/6, 38/13, 41/5 |
| first-seen text | 199/149 |
| text lookup | 10/0 |
| validated id lookup | 9/0 |

Background before/active/after was 0.810/3.480/2.859 busy cores. Complete child CPU was 1.078338
seconds over 0.514870 seconds through final observation. Process interval deltas missed the short-lived child;
child rusage supplied its complete CPU. No wall-time correction was applied.

A global one-entry thread-local hint is not justified by this baseline alone. Traced tick counter paths
alternate names; the ordinary QuickJS property getter already avoids constructing Name. One concrete local
repeat is `scene::LightingOf` constructing `Name("Lighting")` per call, covered by the existing 100,000-call
sunlight benchmark. Its complete resolver cost and output parity still need measurement before a local hoist.
No Name implementation change was made, and this baseline is not a full stress or end-to-end gain claim.


## Sunlight resolver baseline, September 30

The fixture-only resolver gate passed 51 assertions in four cases in the dev preset. Its independent
analytical oracle covers empty/default worlds, 24 integer and dyadic clocks, duplicate Lighting services
in creation order, renaming, authored mutation/clamps and removal exposing the next service. It checks
all solar, colour, policy and post-effect scalar terms plus empty environment/effect selection. It does
not establish parity for populated environment providers or effects.

The unchanged production resolver baseline is `/tmp/atomic-sunlight-baseline`, SHA256
`366c6337ef1873a792eb5f1d8624c34df86a41b69ce2d4bb0985896474ec908f`, with source hashes in
`/tmp/atomic-sunlight-baseline-freeze.json`. The existing bench preset suite varies ClockTime for
100,000 resolver calls per batch. Five uncaptured samples gave minimum/spread 976/33 ns per call.
Background before/active/after was 1.050/3.067/1.845 busy cores; complete child CPU was 1.300199
seconds over 1.565636 seconds through final observation. Child process interval deltas missed its exit;
complete rusage supplies CPU. No wall-time correction was applied.

A separate opt-in capture of the same frozen binary completed 13 batches: eight warmups and five
measured. Every complete owner batch had one root span, no children/reported work and zero frame or
heap-scope drops. The measured mean owner inclusive/self duration was 97.502417 ms per 100,000 calls;
mean unmarked frame time was 0.000923 ms. These are whole-batch measurements, without per-Name
attribution. Its benchmark minimum/spread was 972/9 ns, kept separate from uncaptured timing.

Every measured batch attributed 54,400,000 allocated C++ bytes in 1,200,000 blocks to the owner tag,
544 bytes and 12 allocations per resolver call. Tagged live bytes/blocks stayed 250/5 before and after
each warmed batch; sum-tag peak was 466 bytes and profiler overhead 73,792 bytes. This is C++
new/delete evidence, not C allocation, RSS, stack, driver memory or all-thread Tracy evidence.
The exact allocation boundaries still need attribution before choosing a production change.
Capture background before/active/after was 1.415/2.064/0.965 busy cores; child CPU was 1.298154
seconds over 1.565419 seconds through final observation. The sampler's process visibility is restricted;
empty visible background process rows do not establish that host background applications were closed.

No Name or sunlight production optimization was made. Raw benchmark output remained on stdout.
Fresh TCP and UDP socket probes both returned EPERM (1), and `.git` remains read-only, so the matched
200-client rerun and requested commits remain unavailable in this environment.


### Sunlight allocation attribution

A separate diagnostic baseline adds allocation-only heap tags at ServiceOf and EachRoot, preserving
the resolver and traversal algorithms. Its binary is `/tmp/atomic-sunlight-attribution-baseline`, SHA256
`0389b179728e9b23c62a95a266aa327ed288c46593591ac900ed9ae2ed58dbee`; source/binary hashes are
in `/tmp/atomic-sunlight-attribution-baseline-freeze.json`. This instrumentation changes hook overhead,
so it is an attribution baseline, not a speed comparison against the earlier binary.

All 13 batches retained exactly one complete owner frame/span and zero frame/heap drops. Three heap
nodes reconciled their exclusive byte/block deltas with the inclusive batch. In each measured batch,
root snapshot work allocated 49,600,000 bytes in 1,000,000 blocks; service lookup outside those snapshots
allocated 4,800,000 bytes in 200,000 blocks; exclusive batch allocation was zero. Their sum is the earlier
54,400,000-byte/1,200,000-block batch total. Live bytes/blocks remained 250/5, sum-tag peak 466 bytes,
and profiler overhead 73,792 bytes. Two root snapshots therefore account for 496 bytes/10 blocks per
resolver call, with 48 bytes/two blocks outside them in service lookup. Code inspection identifies the
large std::function callbacks there; the tags establish the allocation boundaries rather than an
allocation backtrace for each block.

Measured mean owner inclusive/self was 100.9616228 ms per batch, mean unmarked 0.0007888 ms.
Captured minimum/spread was 1000/29 ns per resolver call. Background before/active/after was
0.885/2.006/0.810 busy cores, child CPU 1.317301 seconds over 1.565661 seconds through final
observation. The machine busy-core difference was +1.122 relative to the leading background sample,
+1.197 relative to the trailing sample. Those differences include changing host load and sampler
measurement; they are not causal engine CPU estimates or elapsed-time corrections.

The next candidate narrows ServiceOf to a read-only root-class scan with minimum Entity.Id selection.
EachRoot's snapshot semantics must stay intact for callbacks that mutate or nest traversal. No candidate
gain is established yet, and no global Name change is justified by this evidence.


### Sunlight direct service lookup accepted

ServiceOf now scans borrowed const Hierarchy rows, accepts root class matches and selects the smallest
full Entity.Id. This retains creation-order selection across archetype moves, renaming and reparenting.
EachRoot keeps its snapshot traversal semantics. No Name cache or environment-provider algorithm changed.
The complete dev scene suite passed 521,382 assertions in 622 cases, including Services, Sunlight,
atmosphere, volumes and shader lenses. The production Services.cpp SHA256 is
`e5e7898c7d4c934d82e737d56e46cf34350ae623c3d8b6189623cfd4b82204ca`.

Matched uncaptured ABBA runs used five samples per fresh process with the same attribution instrumentation,
bench preset and 100,000 varying-ClockTime resolver calls per batch. A is the attribution baseline above;
B is `/tmp/atomic-sunlight-direct-service-candidate`, SHA256
`4b09cc79ea186e146317ee61dbc61a6d1e2682ffc5a4d11f33f23eca45bfffab`.
Source/binary hashes are in `/tmp/atomic-sunlight-direct-service-candidate-freeze.json`.

| run | minimum/spread ns per call | leading/active/trailing busy cores | child CPU seconds |
|---|---:|---:|---:|
| A1 | 981/2 | 2.020/1.802/0.790 | 1.292419 |
| B1 | 952/3 | 1.230/1.763/1.075 | 1.255006 |
| B2 | 954/36 | 0.775/1.624/0.835 | 1.256906 |
| A2 | 981/17 | 0.785/1.617/1.025 | 1.307068 |

Adjacent minimum gains were 2.956% and 2.752%. Background remained variable and process visibility
restricted. No background subtraction corrected elapsed timing. This is a local bench resolver result,
not a whole-engine, 200-client or shipped release result. The bench preset retains heap profiling hooks;
release without those hooks has not been measured for this change.

A separate candidate capture retained 13 complete owner frames/spans, eight warmups and five measured
batches, with zero frame/heap drops. The two heap nodes reconciled batch and service lookup totals;
no root snapshot node was visited. Every warmed measured batch allocated zero C++ bytes/blocks.
The first warmup retained 250 bytes/five blocks; warmed live bytes/blocks stayed 250/5, sum-tag peak
was 250 bytes and profiler overhead 73,792 bytes. These counters exclude C allocation, RSS and driver
memory. Mean measured owner inclusive/self was 96.4474106 ms per batch; mean unmarked time was
0.0007126 ms. Captured minimum/spread was 966/2 ns, separate from uncaptured ABBA measurements.
Background leading/active/trailing was 1.545/2.641/2.945 busy cores; complete child CPU was
1.273744 seconds over 1.515496 seconds through final observation.

The candidate is retained for verified allocation removal and its modest matched local gain.
Raw benchmark output remained on stdout. Read-only git and denied sockets still prevent commits
and the required matched 200-client RecoverRows rerun.


A subsequent focused service gate passed 262 assertions in 16 cases after extending the existing
transition fixture to destroy/recreate a root in its recycled slot. The survivor sorts before the
new generation-bearing handle, and ServiceOf agrees with matching EachRoot traversal both before
and after survivor deletion. This distinguishes full-handle ordering from slot-only selection.
Production and benchmark binaries stayed unchanged. The extended Services test SHA256 is
`4a5720096ce87d672d330a9c6cf3cc0716365c6f8499cb778246e40f7763cfb8`; its true preimage and scoped
patch are in `/tmp/atomic-service-full-id-owned/`.


### local channel allocation baseline

headless channel parity passed 16,223 assertions in 18 cases. the bench preset built with
-O3 and heap hooks. frozen binary `/tmp/atomic-channel-baseline-bench` SHA256 is
`31827eb8863d1cdf8d57d0413c8b0f58eb0af7eb4694ee311cf26df4fd6fba46`.
source instrumentation and oracle hashes are in `/tmp/atomic-channel-diagnostic-freeze.json`;
aggregate capture evidence is `/tmp/atomic-channel-baseline-aggregate.json`. raw benchmark
output stayed in memory and on stdout, never in a file.

five-sample uncaptured minima were 70 ns per 64-byte round trip, 143 ns per 4 KiB round trip,
3,652 ns per 256 KiB round trip, and 294 ns per frame for the 10,000-frame 4 KiB fill/drain batch.
the two-thread 100,000-frame row returned 457 ns with 522 ns spread, so that row has substantial
variability. these are local channel measurements, not shipped release or network stress results.

all nine diagnostic rows captured 13 complete owner frames: eight warmups and five measured
batches. serial rows retained one root span; the threaded row retained the root, a joined idle
child and one marked reported consumer span. all rows had zero frame drops and zero heap-scope
drop deltas. reported consumer duration remains producer work and is not subtracted from owner
self time. tagged exclusive allocation totals were checked against process allocation deltas.

per measured batch, 20,000 accepted 64-byte sends allocated 1,759,808 to 1,760,312 bytes in
20,952 to 20,953 blocks. 20,000 accepted 4 KiB sends allocated 82,399,808 to 82,400,312 bytes
in the same block range. 2,000 accepted 256 KiB sends allocated 524,335,880 to 524,336,384 bytes
in 2,095 to 2,096 blocks. receiving allocated one output buffer per batch, preserving caller
capacity. send attribution includes payload vectors and queue storage. refused full/oversized
sends and empty receives allocated zero tagged bytes in their measured loops. the pending-byte
row includes its existing per-batch queue preparation, so its send allocation is not attributed
to the query itself. C++ new/delete coverage does not measure C allocations, RSS or driver memory.

an adjacent two-second background-only sample measured 0.690 busy cores. the uncaptured run
measured 1.842 total busy cores and 1.168 child busy cores, leaving 0.674 observed background
cores during the run. total minus the leading baseline was 1.152 cores. the captured run measured
2.028 total cores and 1.139 child cores, leaving 0.889 background cores; total minus the same
baseline was 1.338 cores. background changed between runs. these differences describe CPU load;
no elapsed benchmark timing was corrected by subtraction. host process visibility is restricted.

bounded send-buffer reuse is the next candidate. no optimization or gain is accepted from this
baseline. TCP and UDP socket probes still return EPERM, and `.git` is still read-only. the matched
200-client RecoverRows rerun and requested commits remain unavailable in this environment.


### local channel spare payload retained

one consumed payload vector per direction is now reusable. actual retained capacity must fit both
configured Capacity and MaximumFrame. this adds at most twice their minimum in cached payload
capacity, plus two vector descriptors, separately from queued-byte admission. Receive still copies
into the caller buffer and preserves its capacity. queue-slot allocation precedes consuming a spare;
receive assignment precedes queue mutation. Close releases both spares while preserving queued drain.

headless parity passed 16,370 assertions in 19 cases, including mixed full-byte ownership/FIFO,
caller capacity, refusal atomicity, opposite directions and close/drain. the new heap-enabled fixture
verified that Close releases the cached payload while queued messages and caller output survive.
owned source/test hashes and true preimages are in `/tmp/atomic-channel-spare-freeze.json`.

frozen candidate `/tmp/atomic-channel-spare-bench` SHA256 is
`0fc462dcf97f9f6a91a721de59be7949c03755033ddc27eace25cf76b10aca5e`. matched five-sample fresh-process measurements used unchanged bench settings and
instrumentation; raw output stayed off disk. aggregate evidence, including all nine rows and a
second ABBA to investigate variable large-message timing, is `/tmp/atomic-channel-spare-abba-aggregate.json`.

| row, ns per call | A1 | B1 | B2 | A2 | A3 | B3 | B4 | A4 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 64-byte round trip | 72 | 30 | 31 | 72 | 69 | 29 | 29 | 69 |
| 4 KiB round trip | 143 | 93 | 95 | 143 | 143 | 92 | 93 | 143 |
| 256 KiB round trip | 5035 | 3927 | 5896 | 3684 | 3541 | 3854 | 3556 | 3995 |
| 10k-frame 4 KiB fill/drain | 296 | 286 | 287 | 281 | 311 | 299 | 293 | 290 |
| two-thread 4 KiB round trip | 472 | 360 | 409 | 474 | 443 | 382 | 467 | 490 |

small-message minima consistently improved. large-message adjacent pairs changed in both directions,
including a slower candidate pair; no stable 256 KiB speed gain is established. two-thread spreads
were 522 to 692 ns across the matched observations and baseline, larger than the claimed minimum
differences, so threaded speed gain is also unproven. the fill/drain row has no consistent gain.
this change is retained for verified allocation reduction and the repeatable small-message local
benefit. these are heap-hook-enabled local bench results, not release or 200-client timing claims.

candidate diagnostic capture retained all 117 owner frames, eight warmups and five measured calls
per each of nine rows, with zero frame/heap drops. 20,000 accepted 4 KiB sends now allocate 479,808
to 480,312 tagged bytes in 952 to 953 blocks, rather than 82,399,808 to 82,400,312 bytes in 20,952
to 20,953 blocks. 20,000 accepted 64-byte sends have the same remaining deque allocation range,
rather than 1,759,808 to 1,760,312 bytes. 2,000 accepted 256 KiB sends allocate 47,880 to 48,384
bytes in 95 to 96 blocks, rather than over 524 million bytes. receiver allocation stays one buffer
per batch. warmed tagged send residency is flat at 568 bytes for the small row, 4,600 for the 4 KiB
row and 262,648 for the 256 KiB row. these levels include retained queue storage and the spare.
threaded sends still allocate 299,888,432 to 346,672,944 bytes per 100,000-frame batch because
only one spare is retained; there is no zero-allocation threaded claim.

background leading samples across the two ABBA sequences were 0.690 to 0.880 busy cores. observed
background during those runs was 0.645 to 0.923 cores after subtracting measured child CPU. total
busy cores minus the adjacent leading baseline ranged from 0.999 to 1.389. no elapsed timing was
corrected by subtraction, and restricted process visibility does not prove host applications idle.

an attempted `--help` call was not a supported benchmark-binary option and ran all suites instead.
its process rows logged missing `/tmp/bench_parallel` child launch errors. that invocation is excluded
from acceptance evidence; only explicit channel-suite commands above support these conclusions.


## GUI collector root snapshot reuse, 2026-10-01

The baseline-only stage tagged actual collector-handle collection/sorting and collector-root collection without changing traversal. The existing six Layout breadth/depth rows retain their authored trees and measured bodies; the 10,000-element, three-level tree has 3,333 direct collector children. Descendant child arenas and per-depth Item scratch already reused storage. The measured remaining root vector was reconstructed and destroyed once per collector layout.

The candidate appends the complete root snapshot into the existing thread-local ChildArena before collector component writes, then iterates its fixed index range by copied entity handle. Descendant measurement may reallocate arena storage, so no iterator or span survives across that work. Existing per-root scopes release descendant generations, and an outer scope restores the prior logical mark on return or exception. Roots are rebuilt for every collector call, with no cached ECS truth, ordering invalidation scheme, new persistent vector or across-world handle use.

This preserves the arena's existing high-water retention contract. There is no fixed capacity ceiling. Logical handle release and allocated capacity are different: capacity can remain after the world is destroyed, until the owning thread exits. A module-private diagnostic reads actual size, capacity and logical payload bytes; it does not widen a public shipping header.

### Matched binaries and correctness

Both versions contain the same analytical fixture, canonical export and profiling/ledger code. The optimized preset is `bench -O3` with C++ heap hooks enabled. These are profiling-enabled benchmark costs, not heap-hook-free shipping-release or 200-client measurements.

| Version | Preserved optimized executable | SHA256 |
|---|---|---|
| Original local root vector | `/tmp/atomic-gui-root-canonical-baseline-evidence/bench_gui` | `f70b64ce1bdccf98bfe117301d2f8d884e3d7e2204509b3ade2ee96024eaf107` |
| Existing arena root run | `/tmp/atomic-gui-root-candidate-evidence/bench_gui` | `9704cd006489d6557634a85565bacab49f7a4233ed1d3f790e39a2e37d5f59c0` |

Baseline and candidate dev builds and the `[gui][layout]` gates completed with exit zero, 460 assertions in 59 cases each. The independent plain-Frame analytical oracle checks every Resolved field against authored ancestor arithmetic, with a 0.001-pixel float tolerance, plus collector canvas/transform. Tests cover initial archetype writes, repeat, anchors, clip, rotation, mutation, display resize, detach/reattach, and 3,072-node/repeat/15-node-other-world/large-world-return ledger plateaus. The analytical tolerance is not the cross-binary equality criterion.

Opt-in correctness export compared 78 complete benchmark records in memory, eight warmups and five measured calls for each of six rows. Every record contains 21 explicit 32-bit words per entity: full 64-bit entity and parent IDs plus every Resolved field, including packed reserved bytes. Canonical stdout comparison covered 30,814,860 bytes, with all records equal; its digest is `ae8197e37d5e482bff91a7af8f3740440c070bcab97c618a4d92922c27132387`. Four additional large/repeat/smaller-world/large-return test exports also matched, digest `1f0c9381f9d1bee19441a9118b4d33803ac925ebf8c2b971ad3f6637e7dc234a`. Equality of the full payloads, not digest equality alone, is the proof. Entity IDs identify these fixed in-process fixtures, not a new durable serialization format. Export runs mark enclosing BENCH timings invalid; canonical export is disabled in all performance/capture comparisons.

### Allocation reduction and retained capacity

Two ABBA sequences completed eight fresh processes with five measured samples and eight warmups per row. All 624 owner frames and 1,296 spans were consumed immediately, including cold setup/warmups; frame and heap drop deltas were zero, required hierarchies and exclusive/inclusive/process allocation reconciliation passed. The first warmup may include the fixture's original setup Layout in addition to its measured-body Layout. Steady figures below describe the five measured calls, which each contain one completed Layout, in each of four processes per version.

| Existing Layout fixture | Baseline root allocated bytes / blocks per call | Candidate root allocated bytes / blocks per call | Baseline / candidate retained arena payload bytes after call |
|---|---|---|---|
| 100 elements, 3 deep | 1,016 / 7 | 0 / 0 | 32 / 512 |
| 1,000 elements, 3 deep | 8,184 / 10 | 0 / 0 | 32 / 4,096 |
| 10,000 elements, 3 deep | 65,528 / 13 | 0 / 0 | 32 / 32,768 |
| 1,000 elements, 2 deep | 8,184 / 10 | 0 / 0 | 32 / 32,768 |
| 1,000 elements, 8 deep | 2,040 / 8 | 0 / 0 | 64 / 32,768 |
| 1,000 elements, 16 deep | 1,016 / 7 | 0 / 0 | 128 / 32,768 |

These retained capacities reflect the fixed six-row process order and carry forward the earlier 10,000-element high-water mark; they are not standalone per-scene minima or allocator heap commitments. Logical handles were zero before/after every completed fixture and repeated same/smaller/world-switch calls plateaued. The 10,000-element process allocation delta changed from 308,130 bytes in 10,021 blocks to 242,602 bytes in 10,008 blocks per measured call, exactly the root allocation difference. Collector-handle collection/sorting remains 16 bytes in two blocks per call in these final matched captures. Layout-exclusive allocations remain 240,000 bytes in 10,000 blocks for that row; this experiment does not remove them or attribute them to a new mechanism.

Heap coverage is intercepted C++ new/delete, with logical vector payload, source live blocks/bytes, cumulative churn, sum of tag peaks, process counters and profiler overhead kept separate. The root snapshot's live handles are released, while its reused backing allocation remains; zero warmed root allocations is not zero resident memory or a leak claim. The measured profiler header overhead for the 10,000-element row was 70,176 bytes in both versions. No GPU, worker-reported spans or idle region is invented for this serial layout workload.

### Timing and acceptance limits

The paired aggregate reports the actual completed Layout scope, not broad BENCH runner timing: the existing heap-tree diagnostic work dominates small-row callbacks. Across the four five-sample run means per version, the 10,000-element Layout mean was 2.12469895 ms baseline and 2.12594565 ms candidate, a 0.05868% increase. Other row means changed by -2.120%, -1.503%, +0.660%, -1.933% and -4.603% in registration order excluding the 10,000-element row; individual source orders vary and these small changes do not establish a general speed gain. Inclusive/self/owner/unmarked values remain separate in the capture aggregate, and no nested inclusive time is double-counted.

Leading background samples across the eight processes ranged from 0.649381 to 0.724509 busy cores. Observed background during active runs, after subtracting measured child CPU from machine busy CPU, ranged from 0.719348 to 0.834431 cores. These are descriptive machine-load observations, not corrections to wall time or proof that named host applications were idle. Socket EPERM still blocks the separate 200-client RecoverRows/Tracy requirement; this headless GUI evidence does not replace it.

The candidate is accepted for the exact warmed root allocation reduction, passing full layout and canonical-output gates, with the increased thread-local high-water residency and neutral/noisy timing stated explicitly. Raw benchmark timing output and canonical payloads were not saved as report files; only in-memory comparisons and aggregate/digest evidence were retained. Source preimage is `/tmp/atomic-gui-before-root-arena.cpp`, SHA256 `4b1a38fd76dbec36e0169dd04de58e91104ba59bf55a86450980b80d26376d44`; final Layout.cpp SHA256 is `897f61b13d36cacf4da437114e65ca18f59ce26365e297ec3560d31bed760628`. The scoped candidate patch is `/tmp/atomic-gui-root-arena-candidate.patch`, SHA256 `297ea2c7f288de6d70ff9d5f4d83257f963018e069f52044718229df4df6db04`. Git metadata remains read-only, so no commit was made.


## GUI child scan callable allocation removal, 2026-10-01

The diagnostic baseline placed the fixed `gui child scan` heap tag around the
existing `ScanChildren` outer `Store::EachChild` expression, including its
implicit callable construction, synchronous invocation and destruction. The
FrameGraph owner and layout span hierarchy, authored workloads and canonical
export stayed unchanged. Every warmed 10,000-element frame attributed exactly
240,000 bytes and 10,000 allocation blocks to this tag, with 24 bytes of tag
peak and zero live bytes before and after. Layout exclusive allocation was
zero; root snapshot allocation was zero after the preceding accepted change.
This is transient callable churn, not a growing live heap.

`EachChild` accepts `const std::function<void(Entity)> &` and forwards that same
reference to the sibling traversal. On this host, libstdc++ 13 stores eligible
small callables inline and allocates larger callables. The original outer
lambda captured the store, arena and result by three references. The accepted
candidate keeps those references in a stack `ChildScanContext` and captures
only its one reference. Local aliases bind the same original objects; the
nested virtual-template callback remains synchronous and captures the same two
referents. First-match modifier selection, child insertion order, arena indices,
collection template selection and the ECS callable API are unchanged. The
callback body was also compared after alias and whitespace normalization.
Inline storage behavior is compiler/library dependent; the measured allocation
removal applies to these exact binaries.

The diagnostic baseline benchmark SHA256 is
`d727f96f030903575ecd25eac733404ea5b0d82c9d6a56d4bd3cf6cc0bceb616`;
the candidate is
`ec7e7b859c2cd92216af242877a7128ca42f7205c743485aa27be1afae3cee4e`.
The candidate `Layout.cpp` SHA256 is
`3f0a144aecae325393f519329e5667fbb63480ef67a0d71052b9b84666a38f92`.
Both dev and bench builds exited zero. The diagnostic baseline focused layout
gate passed 460 assertions in 59 cases; the candidate full GUI gate passed
4,593 assertions in 319 cases, including modifier and virtual collection cases.

Exact cross-binary comparison passed for all 78 benchmark canonical records,
30,814,860 bytes covering complete entity IDs and all 21 explicit output words.
Four large/repeated/smaller/world-switch oracle records also matched. Comparison
used the actual canonical payload in memory, not only digests. Benchmark digest
was `ae8197e37d5e482bff91a7af8f3740440c070bcab97c618a4d92922c27132387`;
oracle digest was
`1f0c9381f9d1bee19441a9118b4d33803ac925ebf8c2b971ad3f6637e7dc234a`.
Canonical export mode marks enclosing BENCH times invalid and those times were
excluded from the performance comparison.

Two ABBA repetitions used eight fresh processes, the `bench` preset at `-O3`
with C++ heap hooks enabled, eight warmups and five measured samples per row.
All 624 frames and 1,296 owner/layout spans were consumed with zero frame or
heap drops and the existing hierarchy and allocation reconciliation gates
passed. No worker or GPU timing is implied. Every one of the six measured rows
had zero child-scan allocated bytes and blocks in the candidate, versus exactly
24 bytes and one block per entity in the baseline.

| Warmed 10,000-element call | Diagnostic baseline | Candidate |
| --- | ---: | ---: |
| Child scan allocated bytes / blocks | 240,000 / 10,000 | 0 / 0 |
| Layout inclusive allocated bytes | 240,016 | 16 |
| Process allocated bytes / blocks | 242,602 / 10,008 | 2,602 / 8 |
| Collector handle allocated bytes / blocks | 16 / 2 | 16 / 2 |
| Profiler overhead bytes | 70,176 | 70,176 |

Child arena retention and logical mark restoration were unchanged from the
preceding accepted high-water reuse experiment. The candidate adds a stack
context, not a retained cache or new heap buffer. CPU allocation coverage is
C++ `new`/`delete`; these counters are not allocator RSS or GPU commitments.

Across the 20 measured layout calls per version for the 10,000-element row,
actual layout scope mean was 2.20470585 ms baseline and 1.8044827 ms candidate,
a decrease of 18.1531 percent. This is an improvement in the matched profiling
build, including savings from avoiding heap-hook work for 10,000 allocations.
A hook-free shipping release was not measured, so this percentage is not a
shipping-cost claim. Smaller rows also had lower aggregate means, but no
universal workload gain is claimed.

Leading machine busy-core observations ranged from 1.77377 to 2.09852; observed
background during the benchmark after child CPU accounting ranged from 1.46110
to 1.96209 cores. These describe machine load and are not subtracted from
elapsed layout time. The process namespace does not prove named game presence.
The change is accepted for exact allocation removal, complete output parity
and the qualified instrumented-build improvement. Raw benchmark reports and
canonical payloads were not written to files. This headless gate does not
substitute for the still socket-blocked 200-client RecoverRows capture.


## Headless HDR blur source-family profiling, 2026-10-01

The existing `imagegraph-source-family-bench` workload now includes two bounded
128x128 captured-image chains. Both use finite constant RGBA samples
`{2.00099, -0.5002475, 0.25012375, 1}`, retaining signed and above-one values.
The constants lie near binary16 rounding midpoints so the Gaussian input-format
pass quantization and subsequent selected-format conversion affect the result.
The Directional chain captures RGBA16F, runs Gaussian size1 with gamma off into
selected RGBA32F, then runs Directional strength8/resolution1/16, direction0,
fade and gamma off, smoothing0 and colourization0. Its dyadic step produces
17 taps. The Zoom chain captures RGBA32F, runs Gaussian size1 into selected
RGBA16F, then runs Zoom strength8/samples8, center64/64, middle origin, normal
mode, fade/gamma/colourization off, also 17 taps. Both motion nodes author an
RGBA8 depth choice to verify their source-defined input-format output behavior.
These work estimates remain below the 256-million operation admission limits.

The fixture persists and compiles the linked graphs, verifies complete shape
and format, checks every decoded output channel for finite analytical values,
and hashes all output bytes together with dimensions and format. Expected
Gaussian values use independently derived unit-weight/alpha-seed arithmetic,
quantizing each pass in the captured input format before selected conversion.
The independent oracle uses the public numeric surface codec, not the blur
executor or sampling helpers. Constant-field motion taps preserve the expected
colour. Repeated evaluation must reproduce the full payload hash. This provides
source-based and analytical coverage, not licensed native/GPU parity or proof
for every textured blur input.

An initial benchmark attempt stopped on the existing WAV-controls row before
reaching HDR: `ResolveWavPreviewControls` bypassed the profiled `EvaluateResult`
entry. A bounded failure diagnostic identified the observed timeline,
processor, source-getter and node scopes with no whole evaluation owner.
A shared `ENGINE_PROFILE("imagegraph.evaluate")` at the WAV control-resolution
entry now covers validation, compilation, evaluation and failure returns.
Required phase gates were retained; no missing-owner gate was weakened.

The completed benchmark exited zero with 130 owner frames and 45,227 spans:
ten families, thirteen calls each, eight warmups and five measured samples.
All frame and heap drop gates passed. The HDR rows additionally validate root
owner, parent/depth order, no reported worker spans, required evaluation,
processor, node.other, surface scratch and image allocation phases. Actual
boundary counters verify three scratch allocations and two output allocations
per HDR evaluation. Inclusive and self durations remain separate in the
captured hierarchy; nested inclusive spans must not be added as elapsed cost.

| HDR row | Directional chain | Zoom chain |
| --- | ---: | ---: |
| Input FNV64 | 14059605786410143226 | 2163348978171903895 |
| Complete output FNV64 | 8424405361765930962 | 2699374278253272549 |
| Mean owner milliseconds, five measured calls | 30.694489 | 38.4234216 |
| Minimum / maximum owner milliseconds | 30.642557 / 30.817627 | 38.156372 / 38.727104 |
| Scratch payload bytes / allocations | 524,288 / 3 | 655,360 / 3 |
| Output payload bytes / allocations | 524,288 / 2 | 262,144 / 2 |
| Process allocated bytes / blocks per measured call | 1,216,556 / 240 | 1,216,202 / 238 |

These are absolute workload costs from `bench` at `-O3` with C++ heap hooks,
not matched optimization gains or hook-free shipping-release measurements.
Scratch/output counters measure actual owned payload at allocation boundaries;
process allocation includes other evaluation work. The benchmark retains
`Readings` containing copied spans, counters and heap-node snapshots for all
completed calls until fixture destruction/reporting. This intentional diagnostic
retention contributes to process live-byte growth across the run. The live
ranges are not evidence of an engine leak, and no heap-soak slope claim is made.
Heap counters cover C++ `new`/`delete`, not GPU commitments or every C allocator.

The preserved benchmark SHA256 is
`d5cbde1c7f2e9bfb27639caadf338c4242efafccd382c7dd43cb7113890f42c0`.
At that build, `Document.cpp` SHA256 was
`1e66a05a402aff6ad3d165526e9c91c44bcfeab63d324495723ecc9ede2775a2`,
`SourceFamilyEvaluation.hpp` was
`33408142e1194e4a34d60ceb6d28713e3123ac82f889e9db055d488509f6c4d2`
and `SourceFamilies.cpp` was
`c9a40ee5664ab31dd65637f5d78c183c528fa0dc900abc03feba6cd854dc9198`.
The preserved profile manifest records the exact remaining executor preimages.
This benchmark preceded the subsequent authored Material-range catalogue
patch; the HDR graphs contain no Material node and do not exercise those limits.
Final correctness evidence is a separate dev build: full imagegraph passed
123,446 assertions in 581 cases, full imagegraphio passed 4,019 in 86, and
explicit headless Studio imagegraph/assetprofiler/key-kind/timeline tags passed
17,832 in 78. The final runtime manifest SHA256 is
`59b6fd876774b2d9bd1db26cb14089caf5d89e50da3510635c69ec9d8f7e27ac`.
These gates do not include a GPU or external executable parity check. Related
Posterize partial-alpha coverage uses explicitly cleared native reduction
targets; it does not establish an external source runtime's initial target
contents or licensed native parity.

Machine CPU observations were 1.176610060744694 busy cores before the benchmark
and 2.1102453205360794 during it, an observed difference of
0.9336352597913853 cores. Elapsed time remains the unadjusted 2.037677779997466
seconds; child CPU was 1.803993 seconds. This machine-load subtraction describes
busy cores only and does not correct graph timings. The current process
snapshot exposed no Barotrauma or Steam entry, which does not establish a quiet
host outside the namespace. Raw benchmark stdout and canonical payload were
not written to files; preserved evidence contains numeric aggregates and hashes.
This headless profiling closes these authored workload gates, not the
socket-blocked complete 200-client RecoverRows capture.


## Persisted Cube source-family profiling, 2026-10-01

Two rows extend the existing capped `imagegraph-source-family-bench` job with
persisted Material -> Cube -> Transform -> GetData graphs. Subdivisions are
1x1x1 and 10x10x10, producing 36 and 3,600 vertices respectively, six face parts,
six owned material descriptors and twelve edges. The existing total-vertex cap
remains 4,096; the earlier 16x16x16 draft exceeded it and was corrected before
this accepted runtime gate. No admission limit or evaluator algorithm changed
for the benchmark fixture.

Each row selects three outputs in separate evaluation traversals: the transformed
mesh, GetData position and Material descriptor. Material diffuse is 0.25 with
explicit default-valued texture and lighting controls, linked into all six Cube
material ports. Its transparent default Surface is 1x1; the source-defined minimum
property map is 32x32 RGBA8, all pixels black/opaque. Cube taper is 0 with six-face
material mode. Cube and outer Transform retain two distinct nonidentity local
position/anchor/scale records with identity rotations, rather than baking those
metadata transforms into the literal cube vertices.

The independent oracle checks every vertex against literal face planes and UV
triangle order, including position, axial normal, UV and tint. It checks all
twelve literal edge endpoints, both complete transform records, the GetData
position, the full literal Material descriptor and every owned material clone.
All numerical fields must be finite. Complete logical output words and material
image payload bytes feed the output digest; deterministic replay must reproduce
it. This does not call the Cube executor, its interpolation helper or private
mesh payload helpers to construct expected geometry. The persisted graph and
selected output shapes are also checked. These are analytical/source-based
headless gates, not an external licensed runtime or raster/GPU parity claim.

The runtime profile exited zero across twelve families: 156 completed frames
and 46,215 spans, thirteen calls per family comprising eight warmups and five
measured samples. Frame and heap drop gates passed. Each Cube call requires all
three evaluation owners with valid root/depth and prior-parent ordering, no
reported worker spans, and shared processor/node.other/mesh.material/mesh.cube/
mesh.transform/mesh.get_data phases. Actual value output bytes, output operations
and node execution counters are captured at their existing boundaries. Nested
inclusive phase means are not summed as elapsed cost; the table uses complete
frame-owner durations covering all three traversals.

| Cube row | 1x1x1 | 10x10x10 |
| --- | ---: | ---: |
| Input FNV64 | 10138654258669517102 | 6797947295033146082 |
| Complete output FNV64 | 5522731637928043096 | 3538447162093008296 |
| Mean owner milliseconds, five measured calls | 0.1370714 | 0.2203592 |
| Minimum / maximum owner milliseconds | 0.136005 / 0.138901 | 0.200646 / 0.275507 |
| Node executions / value output operations | 8 / 11 | 8 / 11 |
| Value output payload bytes per call | 133,996 | 1,160,428 |
| Process allocated bytes / blocks per call | 209,754 / 688 | 1,236,186 / 688 |

Value output payload counts include intermediate outputs over the selected
traversals, not only the final visible mesh. They describe logical owned payload;
heap allocation totals separately include evaluation plumbing and copies. The
production evaluator retains its existing live-output reservations and prior
payload admission checks, and the separate Cube correctness suite covers exact
budget/refusal behavior. Benchmark `Readings` intentionally retain copied span,
counter and heap-node records through reporting; process live growth is not a
leak finding. No heap-soak slope or allocator-RSS claim is made.

The build was `bench` at `-O3` with C++ heap hooks enabled. These are absolute
workload costs with diagnostic overhead, not optimization gains or hook-free
shipping-release results. The preserved benchmark SHA256 is
`1487ca26c0727ce3c61683133e635648cf86bed1e2add3d03707be7a82b2ea81`.
Aggregate stdout digest is
`4b1ed742123e7ac532a1be743e2507b9916dc5e90c070b15053ad76836681077`;
raw stdout was not saved. The reviewed fixture SHA256 is
`03373c2f6822093f9d4c1194bf8231a190755390b29746512c77cfe8ddb52147`,
test fixture registration is
`958775becb44c969c45ad54e26bf60fd201eff77822ead8b75545077e4ff9a8f`
and benchmark source is
`90fcabb5db57538703c5da3a34d71e8ff013ad4cb41123c8a66f4379f1057168`.

The source-family gate passed 24 assertions in one case over all twelve
families. A subsequent full dev imagegraph gate, after the Cube fixture and new
WAV timeline helpers, passed 123,915 assertions in 593 cases. Full imagegraphio
passed 4,019 in 86 after both additions. Explicit headless Studio imagegraph,
assetprofiler, key-kind-popup, timeline-dopesheet and WAV-timeline tags passed
17,873 assertions in 82 cases. The preserved Cube benchmark and its twelve
source-family paths precede the newer WAV timeline helper work; its exact
measurement source/binary manifest is `/tmp/atomic-cube-profile-evidence/manifest.json`.
The later correctness gates do not retime that newer code or alter the measured
Cube workload costs. Fractional taper-axis selection with nonzero taper remains
outside the verified native execution domain; these two rows use zero taper and
integer subdivisions and do not close that gap.

Observed machine CPU was 0.7132914326922805 busy cores before this run and
1.6487878529199518 during it, a descriptive difference of 0.9354964202276713.
Elapsed time remains the unadjusted 2.098511335992953 seconds and child CPU was
1.859697 seconds. The current process namespace exposed no Steam or Barotrauma
entry; that is not proof of an uncontended host. No machine-load subtraction
corrects the frame timings. Complete headless profiling for these authored Cube
rows is verified; the socket-blocked full 200-client RecoverRows capture remains
unverified and is not replaced by this workload.


## WAV timeline observation profile, corrected attribution, 2026-10-01

The existing `just imagegraph-source-family-bench 5` workload now includes family
12, WAV timeline observation. This capture completed 169 frames and 46,488 spans
across thirteen families, with eight warmups and five measured samples each.
Both the executable and independent capture parser require exclusive heap-tag
allocated byte and block sums to equal process deltas on all 169 calls. Required
whole owners, nested phases, hierarchy, counters and frame/heap drop gates passed.
All twelve preceding families retain their input and complete output FNV64 hashes
from the Cube capture; all thirteen match the preceding WAV capture. This is a
correctness check, not evidence that their timings are unchanged.

The WAV workload round-trips its authored graph, links an animated string path
to WAV File In, and borrows two immutable two-channel clips, each 256 samples per
channel at 128 Hz. At 16 FPS, it observes signed frames -1.25, 1.25 and 2.5.
Each observation independently checks all 33 points, original channel count two,
duration two seconds, signed amplitudes and clamped progress 0, 0.0390625 and
0.078125. Opposite channel amplitudes would cancel if the waveform incorrectly
used the Mono mix. The third observation selects the alternate clip through the
persisted path keys. Complete replay hashes and before/after authored data,
resource IDs, rates, both planes and request-clock hashes protect input
immutability. Retained sibling outputs, current output, resolved snapshot and
replacement geometry remain covered by caller and resolver reservations.

| WAV observer measurement | Result |
| --- | ---: |
| Input FNV64 | 8205430881063086401 |
| Complete output FNV64 | 1177703224236840645 |
| Mean frame-owner milliseconds, five samples | 0.0216826 |
| Minimum / maximum frame-owner milliseconds | 0.02156 / 0.021821 |
| Process allocated bytes / blocks per call | 28,713 / 263 |
| Point capacity bytes / allocation operations per call | 1,584 / 3 |

The complete frame owner covers three observations. Each public
`imagegraph.wav_timeline.observe` scope includes resolved-input capture,
validation and geometry; its mean inclusive duration per observation was
0.007169533333333334 ms and self duration 0.005720933333333333 ms.
Nested geometry averaged 0.00027666666666666665 ms per observation. These nested
inclusive durations must not be added to the frame owner. Before-input hashing
and its document serialization occur before the heap snapshots and frame begin;
after-input verification occurs after frame end and the heap snapshots.

Heap tags are exclusive, not inclusive subtree totals. Per call, the observe
tag alone allocated 15,711 bytes in 195 blocks. Its measured descendants added
1,392/16 for timeline, 792/9 for processor, 1,104/15 for node.other and 1,776/4
for geometry, giving the complete observe subtree 20,775 bytes in 239 blocks.
The process root separately recorded 7,938 bytes in 24 blocks. Together these
are exactly 28,713 bytes in 263 blocks. The 1,584-byte, three-operation point
counter reports actual point storage at its allocation boundary; geometry heap
totals include other allocations under that tag and are a different measure.

The first two WAV captures were rejected for precise attribution: heap-node
snapshot vectors could grow during snapshots, causing exclusive tag deltas to
include recorder allocation outside the process delta interval. The correction
reserves both vectors to the actual `HeapProfile::MAXIMUM_NODES` bound of 4096
before any node snapshot or process total. Snapshot collection performs no allocation; recorder capacity is reserved before
the measured interval. The sampler-only 512-node bound is not used. Recording
vectors intentionally retain snapshots, spans and counters through reporting;
process live bytes rose from 125,810,203 to 127,920,499 over the five measured
calls, with live blocks 55,588 to 55,688 and profiler overhead 1,778,816 to
1,782,016 bytes. These retained records are not a leak finding. There is no
heap-soak slope, RSS or global-memory-bound claim.

This is the `bench` preset at `-O3` with heap instrumentation enabled. Mean
machine load was 0.716619954991616 busy cores before capture and
1.644456605735667 during capture, a descriptive difference of
0.9278366507440511. Elapsed time was the unadjusted 2.1101195299997926 seconds;
child CPU was 1.880426 seconds. The visible process namespace had no Steam or
Barotrauma entry, which does not prove an uncontended host. No load subtraction,
shipping performance improvement or hook-free release cost is claimed.

The corrected binary SHA256 is
`0b285e2720ba99e14cf6cbe360f8e8f2e22e3959b851c4850eb1fdc5ce0ce017`.
Aggregate stdout SHA256 is
`abb741de60dfdbd3e84cceb669e2842183c541fe9ddd62171e19529890b52568`;
raw stdout was not saved. Parsed evidence SHA256 is
`e176e833052fc27dbeda401bcf87acea28d61418f813bf7bf67da3cc4e028f07`.
The exact binary, sixteen measurement-source copies and parser driver are
preserved under `/tmp/atomic-wav-observer-profile-evidence/`; its manifest is
`/tmp/atomic-wav-observer-profile-evidence/manifest.json`.

The measured sources include the corrected Cylinder kernel, whose focused dev
gate passed 325 assertions in ten cases. Full dev imagegraph passed 124,243
assertions in 603 cases. The earlier focused source-family gate passed 26 in one
case before the benchmark-only snapshot correction. These correctness gates do
not create a Cylinder benchmark row in this thirteen-family capture. Earlier
WAV core/IO/headless Studio gates remain the scoped evidence recorded above.
Actual Studio cache-update timing, live GPU drawing, device playback, licensed
Pixel Composer executable parity, unresolved fractional source key-map behavior
and full M0-M7 completion remain unverified by this capture.

Exact measurement-source SHA256 hashes:

| Source | SHA256 |
| --- | --- |
| `mono.engine/imagegraph/tests/fixtures/SourceFamilyEvaluation.hpp` | `a6a1ac7b5f2df20ab998db4a68e12abdf89c2ea3cbf0a3914452bb2a280db43a` |
| `mono.engine/imagegraph/tests/SourceFamilyEvaluation.cpp` | `7a3233914c15f1d684a32bbda92edd85a79283c7a513ed65377e4973ef17da7b` |
| `mono.engine/imagegraph/benchmarks/SourceFamilies.cpp` | `2c3e5d5d0d418ef3c49ee7e604ddb1b80fa58dea28c599d37c9223503fae46b5` |
| `mono.engine/imagegraph/src/nodes/MeshOps.cpp` | `6310e043f446c0d70a5c932d1c9e25d2b7d2d9f7feac8a2445660627e478643d` |
| `mono.engine/imagegraph/tests/MeshCube.cpp` | `d36d47fddf587454406efa60b2e5d7e2fe932581e1cdaa42ab6bd0f38fac161c` |
| `mono.engine/imagegraph/src/Document.cpp` | `1e66a05a402aff6ad3d165526e9c91c44bcfeab63d324495723ecc9ede2775a2` |
| `mono.engine/imagegraph/src/ProcessorBatch.cpp` | `0452843a140f2eb1bfd8efa591cbbdbf3e460bf9cda8fc8d490b24d30b8cbe93` |
| `mono.engine/imagegraph/src/Catalogue.cpp` | `42ac46bff48803fad523155c9eb9c1e0703672de1519e404db8bdb8ecc3276e5` |
| `mono.engine/imagegraph/src/NodeExecutors.hpp` | `9cb766290171b227a9d5a72ab738e4158280efc1b154a8ed2c77f21b4f5ee0cd` |
| `mono.engine/imagegraph/src/MeshPayload.hpp` | `b58066b7f0102ba372ec9ed708f4e0d6ba35530c44ccadbdd804b8ac0ff949a2` |
| `mono.engine/imagegraph/src/ArrayOps.hpp` | `034c04ffe6f5350cad866f70725dfa115b1d24d0c1f9b07cdb5812e214408f70` |
| `mono.engine/imagegraph/include/engine/imagegraph/Document.hpp` | `4b2fc81f1a3e2e67fe7b830bc8507285b1adc7403a8b323bfaedbc0070531e73` |
| `mono.engine/imagegraph/include/engine/imagegraph/WavTimelinePresentation.hpp` | `3e2808475ae0b5646052af3a4bcc63d6f93479fc7a5ef3bdbc5d1c05064fc598` |
| `mono.engine/imagegraph/src/WavTimelinePresentation.cpp` | `a51bdb354a571067db8c1362bc9dccac949605d40bbebcfd62209627ba030fa5` |
| `mono.engine/imagegraph/tests/WavTimelinePresentation.cpp` | `9ff1a233cdffc55e51adda1f06cde7af53da419d300e70018490a5cc7c1bec28` |
| `mono.engine/imagegraph/tests/MeshCylinder.cpp` | `b421052fc1a0702713a992203e96e8aa33f5e652bdc89e4170e2a70d60e025c9` |


## Studio WAV cache observation profile, 2026-10-01

`ATOMIC_STUDIO_WAV_TIMELINE_PROFILE=1 just studio-wav-timeline-bench 5`
profiles actual headless `WavTimelinePanel::Update` calls, separately from the
core resolver workload above. Three rows each ran eight warmups and five
measured batches of 64 updates: 39 completed frames and 6,695 spans. Executable
gates reconcile all exclusive heap-tag byte/block deltas with process totals.
The independent parser checks every emitted frame's complete owner, parent/depth
ordering, 64 observation scopes, expected geometry operations, point counters,
zero dropped frame/heap scopes, and absence of idle/reported worker spans.

Each fixture owns two immutable stereo resources, 65,536 samples per channel at
32,768 Hz, totaling 2,097,152 bytes of input plane capacity. The authored WAV
node has Mono enabled; independent checks require the original first-channel
waveform, two channels, two-second duration and all 257 points at 128 FPS.
Before warmup, analytical point/progress checks cover every transition in each
row. Measured calls retain input FNV64 13192926132712762056 and check immutable
authored serialization, both source planes/rates/IDs, configuration and actual
input capacities before and after the captured interval. Unchanged and resource
revision rows finish with output FNV64 15017532154897203557; the signed cursor
row finishes with 17789495128885463908. These hashes cover all points, progress,
duration and channels, not only one displayed sample.

| Per-update owner / batch allocation measurement | Unchanged request | Signed cursor | Resource revision |
| --- | ---: | ---: | ---: |
| Mean owner milliseconds per Update | 0.000052503125 | 0.003506984375 | 0.022259084375 |
| Mean observation milliseconds per Update | 0.000025078125 | 0.003474625 | 0.022230065625 |
| Process allocated bytes / blocks per 64 updates | 2,684 / 8 | 218,014 / 2,769 | 482,568 / 2,838 |
| Observation subtree bytes / blocks per 64 updates | 0 / 0 | 212,696 / 2,753 | 475,912 / 2,818 |
| Geometry capacity bytes / operations per 64 updates | 0 / 0 | 0 / 0 | 263,168 / 64 |
| Geometry cache hits per 64 updates | 0 | 64 | 0 |

The unchanged row returns through its exact-request cache. Cursor updates cycle
six exact signed half-frame clocks, including negative and beyond-duration
positions, resolve controls and reuse the same point buffer. This row has zero
geometry allocation, but its control-resolution observation subtree still
allocates 212,696 bytes in 2,753 blocks per batch. The resource row alternates the
borrowed source and increments input revision on every update, rebuilding
geometry 64 times. Retained point capacity is 4,112 bytes in every row before
and after each batch. Original buffers remain unchanged; invalidation cost does
not copy the full clip payload.

The outer owner has zero exclusive allocated bytes/blocks in all rows; its
children and instrumentation have distinct attribution. Observation subtree
figures include descendants; geometry is contained within observation, so these
columns must not be added. Resource-row geometry heap totals are 263,240 bytes in
66 blocks, separate from the 263,168-byte, 64-operation point capacity counter.
Process totals additionally include frame instrumentation outside the measured
owner, explaining the nonzero process churn in the unchanged row. Integrity
serialization/hashing occurs outside owner timing and both heap snapshots;
reporting and counter/analytical verification occur after capture. Full BENCH
wall cost and child CPU include diagnostics. Summed tag peaks are not a
simultaneous subtree peak, and these short captures do not establish a leak,
RSS bound or global allocation bound.

The build is `bench` at `-O3` with C++ new/delete heap hooks enabled. Owner
measurements are unadjusted absolute diagnostic costs. Machine load was
1.4332077513714394 busy cores before capture and 2.0888898559962317 during it,
a descriptive difference of 0.6556821046247923. Elapsed time was
0.44042532800813206 seconds and child CPU 0.19789199999999998 seconds.
No Steam or Barotrauma process was visible in the current namespace; this does
not establish an idle host. Busy-core subtraction is not used to correct owner
timings. There is no previous implementation timing comparison, shipping cost
or speedup claim.

The preceding release-tests headless WAV panel gate passed 41 assertions in four
cases. Its runtime manifest also records a reproduced unity-build collision
between the unused RenderPipeline `PassTiming` helper and the used inspector
helper, repaired by deleting only the unused definition before the optimized rebuild. The completed measurement manifest
and hashes are recorded below.

Actual GUI drawing/GPU presentation, device playback, external executable
parity and full M0-M7 completion remain outside this headless cache profile.

The preserved measurement manifest is
`/tmp/atomic-studio-wav-cache-profile-evidence/manifest.json`, SHA256
`bf96274c4b3ef61587fa6f9e333ab4a03a236507084eab5e3a2ad0dc54750a27`. It preserves the captured binary, parsed capture, driver and exact source copies.

Benchmark binary SHA256: `18abe63489ac3445b1b8ea0ceb59573b6d2ee5a732adddc85b0e5b812fa226a6`.

Aggregate stdout SHA256: `7d91c52bd6006f2f4190b82befc752e0d3b2a1bfae1e0d105d69c986a38589fd`; raw stdout was not saved.

Parsed capture SHA256: `f7b5a513ff7334051f723de6ce8998aa1e0e062aac9a3686ae277e08d2ad574d`.

Driver SHA256: `1e6a342c1116f8d0c4e0dde0db23fed3b57958eb65dcab1f264af4d35e8d7bad`.

Exact measurement-source SHA256 hashes:

| Source | SHA256 |
| --- | --- |
| `mono.studio/benchmarks/WavTimeline.cpp` | `6dc5bfea603d276acee5179204c8d51b4670955725fac67a5564c5b8e9d389b3` |
| `mono.studio/src/RenderPipeline.cpp` | `0a38c3d7a9bb9c19e406adcc6ec6b9c3727a11fc343a6a560700c3a4d80db806` |
| `mono.studio/src/RenderPipelineInspector.cpp` | `7b2fda1677a300d44a0b3e6877afa061f22029815c2f896ac2939937e9812e46` |
| `mono.studio/src/WavTimelinePanel.cpp` | `c967a399cf761cfe4001f2bea72617cf728a2172c3b66f01265d9ea87aba7fba` |
| `mono.studio/src/WavTimelinePanel.hpp` | `4c1a020f88863a70c49b1267ae66fbc578bb13d5ab343553190ae8ab4d7b75c9` |
| `mono.studio/tests/WavTimelinePanel.cpp` | `9618804a7f5f7b0a5d3ac7afa689d1ad6e7249f588419ef45faecf76f3712829` |
| `mono.engine/imagegraph/src/WavTimelinePresentation.cpp` | `a51bdb354a571067db8c1362bc9dccac949605d40bbebcfd62209627ba030fa5` |
| `mono.engine/imagegraph/include/engine/imagegraph/WavTimelinePresentation.hpp` | `3e2808475ae0b5646052af3a4bcc63d6f93479fc7a5ef3bdbc5d1c05064fc598` |
| `Justfile` | `7bf9d22f1908b42c9500c49b2e4ff0e7d0b334cfc5e36bb4d4ef8c200d81607a` |
| `mono.engine/imagegraph/src/NodeExecutors.cpp` | `cb977dc3c5760df2eaba69efedb33edca39ba0484799452aadf604ec767de5fe` |
| `mono.engine/imagegraph/src/TimelineOverrides.cpp` | `53876dbce529f9c088c1e904bbf882213dd5d344c40590564a85d2eb4790d7fe` |
| `mono.engine/imagegraph/src/ProcessorBatch.cpp` | `0452843a140f2eb1bfd8efa591cbbdbf3e460bf9cda8fc8d490b24d30b8cbe93` |

## Persisted Cylinder source-family profile, 2026-10-01

The `bench` preset measured two static frame-zero, persisted imagegraph workloads. Each uses a persisted Material → Cylinder → Transform → GetData graph. Its three selected outputs are the transformed mesh (Material → Cylinder → Transform), GetData position (Material → Cylinder → Transform → GetData), and Material output (Material only). The fixture analytically checks the complete mesh and material data, transform chain, and GetData value. It does not invoke a renderer, GPU raster path or licensed Pixel Composer executable.

The small workload uses the source defaults of eight sides, one segment, caps enabled and the constant default profile. It produces 96 vertices across the ordered side/top/bottom parts and 32 source edges. The profile workload uses sixteen sides, twelve segments, a two-anchor linear profile and smooth-side normals, producing 1,248 vertices and 416 edges. Both round-trip persisted authored graphs and evaluate the three selected output routes described above.

Each of the 15 source families completed eight warmups and five measured calls, for 195 frames total. The run emitted 47,476 spans and 1,094 heap records. All 195 frames passed the frame and heap drop gates; for every emitted frame, exclusive tagged allocated bytes and blocks summed exactly to the process deltas. Every measured Cylinder output hash matched its fixture verifier, and each family kept the same input/output FNV64 values across all 13 calls. The first 13 families retained their input and output FNV64 hashes from the preceding WAV profile capture. This hash gate checks consistency, not external source parity.

| Cylinder workload | Input FNV64 | Output FNV64 | Vertices / edges | Mean frame owner ms, 5 samples | Min / max ms | Process allocated bytes / blocks per call | Output payload bytes per call |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 8 sides × 1 segment, default profile | 1091151342148873816 | 1165163485403979819 | 96 / 32 | 0.136364 | 0.135805 / 0.137558 | 173,770 / 642 | 101,980 |
| 16 sides × 12 segments, smooth two-anchor linear profile | 16250986319388079848 | 14018840983066563578 | 1,248 / 416 | 0.1634104 | 0.161442 / 0.167785 | 577,200 / 605 | 507,484 |

Both workloads emitted eight node executions and eleven value outputs per call. The frame owner includes three selected output evaluations, processor work and retained graph/output data. Only the mesh and GetData selections visit Cylinder and Transform; the material selection stops at Material. Thus the measured tags emit two Cylinder and Transform spans per sample, three Material spans, and one GetData span. The following heap rows are exclusive tag deltas, not inclusive subtree totals. Their bytes and blocks sum to the process allocation totals above.

| Exclusive heap tag | Default bytes / blocks | Profile bytes / blocks |
| --- | ---: | ---: |
| process root | 11,866 / 36 | 11,866 / 36 |
| `imagegraph.evaluate` | 47,868 / 476 | 45,570 / 439 |
| `imagegraph.processor` | 9,744 / 24 | 9,792 / 24 |
| `imagegraph.node.other` | 24 / 1 | 24 / 1 |
| `imagegraph.mesh.material` | 14,124 / 20 | 14,124 / 20 |
| `imagegraph.mesh.transform` | 44,616 / 40 | 247,368 / 40 |
| `imagegraph.mesh.get_data` | 1,088 / 3 | 1,088 / 3 |
| `imagegraph.mesh.cylinder` | 44,440 / 42 | 247,368 / 42 |

The Cylinder tag ended each call with zero live bytes and blocks. Its maximum simultaneously live tagged bytes were 22,220 for the default workload and 123,684 for the profile workload. Per-call process live bytes ranged from 132,726,271 to 134,847,751 and 139,754,855 to 141,876,335 respectively; process peak bytes ranged from 132,763,549 to 134,885,029 and 139,994,885 to 142,116,365. Heap-profiler overhead ranged from 1,794,976 to 1,800,352 bytes and 1,813,504 to 1,818,880 bytes. These are short-run process and profiler observations, not a leak test or global memory bound.

This was an instrumented `bench` build. Baseline system load was 1.259931765 busy cores; during capture it was 2.156539241, a descriptive difference of 0.896607476 busy cores. Wall elapsed time was 2.10986191 seconds and child CPU time was 1.882154 seconds. The process-name census before capture found no Steam or Barotrauma entries in its visible namespace, which does not establish an otherwise idle host during measurement. The measured times are unadjusted; subtracting system load does not correct them. These are two different workload sizes, not a before/after comparison or a shipping speedup claim.

Focused Cylinder correctness passed 325 assertions in ten cases. The profile does not establish GPU or HDR raster output, CPU/GPU trigonometric byte equality, licensed executable parity, or full M5 completion. The source singular-profile path can produce nonfinite values; the native executor instead refuses them with a profile diagnostic. That difference remains explicit.

Capture binary SHA256: `4f9e9ccd43041f73a44770da7bd1ab9519a7145d3647d9a786996643c08522e7`.
Aggregate stdout SHA256: `245472c0e78bab512910580fa6db34c73f550effceb002f6b8c92081e3bc8526`; raw stdout was not saved.
Parsed capture SHA256: `9bd1639bf99c2e2e636554a808875a62c585465a3cfb6c1e63ff6fc5d6643d98`.
Driver SHA256: `90309e840e5852f6110de5313e9f99bd36bbcf6f362e0b8ba69654c5bb612a48`.
Capture manifest SHA256: `c60061ac96b9ec6d011a3914aa6f0bb415531063ce17d8c226bfa4466de4736f`.
The preserved capture, driver, binaries and sixteen source copies are under `/tmp/atomic-cylinder-profile-evidence/`.

Exact measurement-source SHA256 hashes:

| Source | SHA256 |
| --- | --- |
| `mono.engine/imagegraph/tests/fixtures/SourceFamilyEvaluation.hpp` | `a7a5aca303c38059cde0b9ba231d627975e15bc18824c8114fa4085553eda75d` |
| `mono.engine/imagegraph/tests/SourceFamilyEvaluation.cpp` | `7764c7d565286161b221507c4c35a3cdb737f782b4709792e702a6b9ecdeca7f` |
| `mono.engine/imagegraph/benchmarks/SourceFamilies.cpp` | `41bc85aeb9b6bfe89886784f94f37e5216cd3de012efced6cfd8f8865be52228` |
| `mono.engine/imagegraph/src/nodes/MeshOps.cpp` | `6310e043f446c0d70a5c932d1c9e25d2b7d2d9f7feac8a2445660627e478643d` |
| `mono.engine/imagegraph/tests/MeshCube.cpp` | `d36d47fddf587454406efa60b2e5d7e2fe932581e1cdaa42ab6bd0f38fac161c` |
| `mono.engine/imagegraph/src/Document.cpp` | `1e66a05a402aff6ad3d165526e9c91c44bcfeab63d324495723ecc9ede2775a2` |
| `mono.engine/imagegraph/src/ProcessorBatch.cpp` | `0452843a140f2eb1bfd8efa591cbbdbf3e460bf9cda8fc8d490b24d30b8cbe93` |
| `mono.engine/imagegraph/src/Catalogue.cpp` | `42ac46bff48803fad523155c9eb9c1e0703672de1519e404db8bdb8ecc3276e5` |
| `mono.engine/imagegraph/src/NodeExecutors.hpp` | `9cb766290171b227a9d5a72ab738e4158280efc1b154a8ed2c77f21b4f5ee0cd` |
| `mono.engine/imagegraph/src/MeshPayload.hpp` | `b58066b7f0102ba372ec9ed708f4e0d6ba35530c44ccadbdd804b8ac0ff949a2` |
| `mono.engine/imagegraph/src/ArrayOps.hpp` | `034c04ffe6f5350cad866f70725dfa115b1d24d0c1f9b07cdb5812e214408f70` |
| `mono.engine/imagegraph/include/engine/imagegraph/Document.hpp` | `4b2fc81f1a3e2e67fe7b830bc8507285b1adc7403a8b323bfaedbc0070531e73` |
| `mono.engine/imagegraph/include/engine/imagegraph/WavTimelinePresentation.hpp` | `3e2808475ae0b5646052af3a4bcc63d6f93479fc7a5ef3bdbc5d1c05064fc598` |
| `mono.engine/imagegraph/src/WavTimelinePresentation.cpp` | `a51bdb354a571067db8c1362bc9dccac949605d40bbebcfd62209627ba030fa5` |
| `mono.engine/imagegraph/tests/WavTimelinePresentation.cpp` | `9ff1a233cdffc55e51adda1f06cde7af53da419d300e70018490a5cc7c1bec28` |
| `mono.engine/imagegraph/tests/MeshCylinder.cpp` | `b421052fc1a0702713a992203e96e8aa33f5e652bdc89e4170e2a70d60e025c9` |


## Scalar-only input-context reserve reduction, 2026-10-01

Catalogue evaluation now skips `Images` and `ImageArrays` vector reserves when
neither fixed nor dynamic input declarations can contain images. Image, Any and
Material3D declarations retain the full input-count reservation before borrowed
views are formed. Scalar value/view/link storage and conservative budget
admission are unchanged. This removes unused image-view storage from WAV
control resolution without changing retained geometry or source getter behavior.

Full dev imagegraph passed 124,265 assertions in 604 cases. Optimized headless
Studio WAV tests passed 68 assertions in five cases, including a persisted linked
animated path changing at a fixed input revision. The new snapshot fixture
checks mixed static Any/dynamic Image inputs, exact pixels and atomic last-good
preservation on insufficient replacement budget. Whole-Document format checks
reproduced preexisting formatting failures; the owned changed range and test
files pass formatting. This is not a whole-file format-clean claim.

Ten captures form five fresh-process baseline/candidate pairs in AB, BA, AB, BA,
AB order. Pair one's baseline preceded candidate compilation; subsequent pairs
ran without active builds. The unchanged, signed-cursor and source-revision rows
each retain eight warmups and five measured batches of 64 actual Panel.Update
calls. Across 390 frames and 66,950 spans, exact input/output FNV, complete
hierarchy, zero-drop and geometry/capacity gates passed. Executable exclusive
heap byte/block reconciliation passed on every frame.

| Mean owner milliseconds per Update across five pairs | Baseline | Candidate | Observed change |
| --- | ---: | ---: | ---: |
| Unchanged request | 0.000059185 | 0.00006021875 | +1.75% |
| Signed cursor | 0.003915023125 | 0.0036836675 | -5.91% |
| Source revision, geometry rebuild | 0.02348499125 | 0.023651669375 | +0.71% |

Cursor and resource rows allocate exactly 18,432 fewer bytes and 128 fewer
blocks per batch of 64 updates in every pair. Cursor process allocation changes
from 218,014 bytes/2,769 blocks to 199,582/2,641; its observation subtree changes
from 212,696/2,753 to 194,264/2,625. Resource process allocation changes from
482,568/2,838 to 464,136/2,710; its observation subtree changes from
475,912/2,818 to 457,480/2,690. The unchanged row has no observation allocations
before or after, with process instrumentation still 2,684 bytes in eight blocks.
Point capacities, geometry counters and all output hashes remain unchanged.
These are measured allocation reductions; the timing results do not establish a
broad speedup, especially with source-rebuild and unchanged means slightly higher.

Individual pair owner means, milliseconds per Update:

| Pair / order | Unchanged baseline / candidate | Cursor baseline / candidate | Rebuild baseline / candidate |
| --- | ---: | ---: | ---: |
| 1 / AB | 6.076875e-05 / 5.7825e-05 | 0.003939153125 / 0.00353993125 | 0.022646728125 / 0.023167096875 |
| 2 / BA | 5.5884375e-05 / 5.6575e-05 | 0.003930071875 / 0.003570765625 | 0.0226930125 / 0.023172196875 |
| 3 / AB | 6.603125e-05 / 6.6309375e-05 | 0.003726625 / 0.003794778125 | 0.022933528125 / 0.024777265625 |
| 4 / BA | 5.8453125e-05 / 5.595e-05 | 0.004004328125 / 0.00377919375 | 0.024584125 / 0.023364365625 |
| 5 / AB | 5.47875e-05 / 6.4434375e-05 | 0.0039749375 / 0.00373366875 | 0.0245675625 / 0.023777421875 |

This uses the `bench` preset at `-O3` with C++ heap hooks. Host load varied:
baseline process captures observed 2.149820158228295 to 3.779691327734654 busy
cores before measurement and 2.657214162565421 to 4.73663843841348 during it;
candidate captures observed 2.019822219285062 to 3.419658663348697 before and
2.2498502091455865 to 3.27909006756021 during. Individual capture files preserve
each load reading and unadjusted elapsed/child CPU totals. No background-load
subtraction corrects owner timings. No shipping cost, hook-free gain, GPU/device
behavior, leak result or global allocation-bound claim follows from these pairs.

The preserved comparison, ten captures, baseline/candidate binaries, source
copies and owned patch are under `/tmp/atomic-wav-scalar-reserve-runtime-evidence/`.
Manifest SHA256 is
`f0934ebccb260f03d88fb567073d3386db0bc60b6fbf0808e08f862def1aafe9`.
Raw stdout was not saved; each capture retains its aggregate stdout digest.

Artifact SHA256 hashes:

| Artifact | SHA256 |
| --- | --- |
| `bench_studio` | `ae2b7f68c9f1948b831a906c4deca5ef35c4db1aed4d060d72991f1e30b7a8ac` |
| `test_imagegraph` | `7e7a9cd9d8df4167e98929e03963d70f134e0e3184d0786893697e613b8264ac` |
| `test_studio` | `c6469d3b383d308d1dfd0c03b4f42eadd581a778a0dc5bb7cbf9fdd095f63544` |
| `driver.py` | `bfc557eaab8a097aecb9cc00008dbfc2575158b7a2a1021955ed51d4bc919e2e` |
| `comparison.json` | `242721e35993357ce7e9034e63404b89283981ab0ec1dc15bc8fa3de4bbc2468` |
| `baseline_bench_studio` | `18abe63489ac3445b1b8ea0ceb59573b6d2ee5a732adddc85b0e5b812fa226a6` |
| `baseline_Document.cpp` | `1e66a05a402aff6ad3d165526e9c91c44bcfeab63d324495723ecc9ede2775a2` |
| `owned.patch` | `f4c0c89d9f550fae5af5c421bd67dc67ab24ffd7a88eae48660694603111c3f1` |

Exact source and capture SHA256 hashes:

| Source or capture | SHA256 |
| --- | --- |
| `mono.studio/benchmarks/WavTimeline.cpp` | `6dc5bfea603d276acee5179204c8d51b4670955725fac67a5564c5b8e9d389b3` |
| `mono.studio/src/RenderPipeline.cpp` | `0a38c3d7a9bb9c19e406adcc6ec6b9c3727a11fc343a6a560700c3a4d80db806` |
| `mono.studio/src/RenderPipelineInspector.cpp` | `7b2fda1677a300d44a0b3e6877afa061f22029815c2f896ac2939937e9812e46` |
| `mono.studio/src/WavTimelinePanel.cpp` | `c967a399cf761cfe4001f2bea72617cf728a2172c3b66f01265d9ea87aba7fba` |
| `mono.studio/src/WavTimelinePanel.hpp` | `4c1a020f88863a70c49b1267ae66fbc578bb13d5ab343553190ae8ab4d7b75c9` |
| `mono.studio/tests/WavTimelinePanel.cpp` | `1960cd2c4380fcdae42a24f33143d0fca4578146cb45ac6778fc490a584bed09` |
| `mono.engine/imagegraph/src/WavTimelinePresentation.cpp` | `a51bdb354a571067db8c1362bc9dccac949605d40bbebcfd62209627ba030fa5` |
| `mono.engine/imagegraph/include/engine/imagegraph/WavTimelinePresentation.hpp` | `3e2808475ae0b5646052af3a4bcc63d6f93479fc7a5ef3bdbc5d1c05064fc598` |
| `Justfile` | `7bf9d22f1908b42c9500c49b2e4ff0e7d0b334cfc5e36bb4d4ef8c200d81607a` |
| `mono.engine/imagegraph/src/NodeExecutors.cpp` | `cb977dc3c5760df2eaba69efedb33edca39ba0484799452aadf604ec767de5fe` |
| `mono.engine/imagegraph/src/TimelineOverrides.cpp` | `53876dbce529f9c088c1e904bbf882213dd5d344c40590564a85d2eb4790d7fe` |
| `mono.engine/imagegraph/src/ProcessorBatch.cpp` | `0452843a140f2eb1bfd8efa591cbbdbf3e460bf9cda8fc8d490b24d30b8cbe93` |
| `mono.engine/imagegraph/src/Document.cpp` | `62608054bf9345290cf85fcde6f7aa4d06984c74a58abfa94878844e6d984160` |
| `mono.engine/imagegraph/tests/EvaluationSnapshot.cpp` | `ca7d50d2ef932579ebbe3eb43eb097191b4357945696e14f76f45b802d744785` |
| `atomic-studio-wav-paired-before-1.json` | `c15917f63537efce934c01f1405ec6009a8de15c050eadc2bc6552ad3572cef2` |
| `atomic-studio-wav-paired-after-1.json` | `3b948aba7e0114b4ce9a71840608944db56f2ac44ddcf45697fabf20be29a62d` |
| `atomic-studio-wav-paired-before-2.json` | `b5d83a472ca0071e13ba4e158e544536e82d8f262f1823c2513e90b2a1d4a3c0` |
| `atomic-studio-wav-paired-after-2.json` | `2c6b85495beddf5bd1699d60c18466b5cce553e95bde57e16907d0a05142809d` |
| `atomic-studio-wav-paired-before-3.json` | `e2ef9563058bdfbb364afe8217341b20e5a6483beba070b6df7e53cb22577f0b` |
| `atomic-studio-wav-paired-after-3.json` | `9ea311c46474892a9640be6093ae66412185f60a7ff41f3d02d7fa94a27685d8` |
| `atomic-studio-wav-paired-before-4.json` | `cc33e82bd93c179e0b5ee23865c85b4a05d5cb92d8a9732713f5bfe5e8178f6a` |
| `atomic-studio-wav-paired-after-4.json` | `8c28139b1f5ac435ac0a4678d24f3fc6a9db50da7457340c7d71904a8873a6f1` |
| `atomic-studio-wav-paired-before-5.json` | `a439f0807f5b9ce6acc937a572e6b273bf25e7f0b22cc1a28a62f4993ab0ac68` |
| `atomic-studio-wav-paired-after-5.json` | `98d900345f0a2f3c1748a810e334054d38b3f55a519624538c8b416bf52c79af` |


## CPU skinning diagnostic baseline, 2026-10-01

This baseline profiles actual ResolveBones over six authored worlds: 1 rig with
32 joints, 32 rigs with 64 joints each, and 16 rigs with 256 joints each, with a
static and animated row for each size. Reverse sibling insertion differs from
palette order; an independent affine oracle verifies every bone at the origin
and three basis points, plus inverse-bind-transformed points. Rig/root/Skeleton
and authored Bone fields are protected by before/after input hashes. Static
passes preserve ChangeVersion and write zero bones; animated writes are checked
against independent expected affine changes, not assumed to equal bone count.

The diagnostic capture passed 78 completed frames, 5,252 spans and 546 phase
records: eight warmups and five samples per row. Span counts are exactly 6, 130
and 66 per frame for the three rig counts. Whole-owner/parent/depth/self-time
checks, finite durations, zero frame/heap drops and exclusive heap/process byte
and block reconciliation passed. Stack heap aggregates allocate no snapshot
vectors. Full verification, preparation, input/output hashing and reporting are
outside the captured resolver owner; broad BENCH wall cost includes them.

| Row | Mean resolver owner milliseconds | Allocated bytes / blocks per call | Verified changed writes |
| --- | ---: | ---: | ---: |
| 1x32 static | 0.0049052 | 3,184 / 22 | 0 |
| 1x32 animated | 0.0052080 | 3,184 / 22 | 28 |
| 32x64 static | 0.2445924 | 141,544 / 557 | 0 |
| 32x64 animated | 0.2704646 | 141,544 / 557 | 1,727 |
| 16x256 static | 0.4432428 | 287,208 / 350 | 0 |
| 16x256 animated | 0.5274346 | 287,208 / 350 | 3,514 |

These establish a baseline for further investigation, not an improvement. Pose
scratch contributes 57,600 bytes in 64 blocks for 32x64 and 115,200 bytes in 32
blocks for 16x256, approximately 40.69% and 40.11% of each row's allocation
bytes respectively. It is an allocation-only tag, so its zero timing fields do
not mean scratch setup costs zero time. In static 32x64, joint gather averages
0.1371364 ms and pose compose 0.0898066 ms; in animated 32x64 these are
0.1335332 and 0.1211028 ms. These child phase costs are contained in the whole
resolver owner and must not be added to it. Summed tag peak bytes are not a
simultaneous process peak or a leak finding.

Separate canonical correctness mode validates 78 calls, each containing every
durable fixture Entity ID and all seven raw float words of its WorldFrame. It
checks exact word count, finite frame words, stable entity order, static
version/frame stability and four-pose replay. Full canonical rows stay in memory;
only digests persist. Canonical BENCH timings are explicitly invalid. Canonical
SHA256 is `8f610b665e4b4ecfa6ae45538b23bc366d67f04b2064d902155e0fd92f62b284`.
Normal benchmark mode also exited zero with its default seven samples. Dev
focused skinning passed 103 assertions in 12 cases; full scene passed 521,465
assertions in 624 cases. These are CPU pose-resolution gates, not GPU/raster
verification or an optimization result.

The measured build is `bench` at `-O3` with diagnostic heap hooks enabled, not a
shipped heap-hook-free cost claim. Machine load was 1.8698652992602924 busy cores
before capture and 2.127048485204866 during, a descriptive difference of
0.25718318594457346. Elapsed time remains the unadjusted 0.38080937300401274
seconds; child CPU was 0.143084 seconds including diagnostics. A process check
AFTER capture found no visible Steam or Barotrauma process. It does not prove
absence throughout capture or an uncontended host. No load subtraction adjusts
resolver timings.

Exact binary SHA256 is
`5ac3fcf9eefdaea844d50b9a8011c57ae14979a9ceb5cdb5e416a913dc6aabb7`.
Profile stdout SHA256 is
`fceb590e99241c7de4d3772a22c06d35fae03782d47ca6311f546b374c4a3f2f`;
canonical stdout SHA256 is
`8a38fc0e79119dacf74a28f7e8c45839696f1e5a81f71382b27f0066b9dcbfa4`.
Raw benchmark stdout was not saved. The preserved binary, source copies,
profile/canonical numeric evidence and background observation live under
`.cache/build/bench/evidence/skinning-baseline-2026-10-01/`.
Completed manifest SHA256 is
`b69efc09a1efbec68c8f4b9e2c3aa72ef29e8564f7a14d93310f600a894fb3d1`.
Driver SHA256 is
`837de70e923623f6d0b22759109cf92af57146c29ccef21188684d6f0a362973`;
owned baseline patch SHA256 is
`f72d1745f0e0afd2d6b979fe2846409fcf2af1542496cd2c9d726fcbe7705a02`.
Future optimization measurements are intentionally not part of this baseline.

Exact source and evidence SHA256 hashes:

| Path | SHA256 |
| --- | --- |
| `mono.engine/scene/src/Skinning.cpp` | `fe43b6410a7a6d38b9c9ca95adbaee5e3c395e52254deb649dca737258ab0d4b` |
| `mono.engine/scene/include/engine/scene/Skinning.hpp` | `29edc93370f882b63767a28696bcd264fdd9583c5bd980c69d77ff48ddcf4dd7` |
| `mono.engine/scene/tests/Skinning.cpp` | `4ce6016e40a189ca934072ab83bc968f9300a760d5accf4c16423e1ad59befab` |
| `mono.engine/scene/tests/fixtures/SkinningParity.hpp` | `92f938474275bb8bc33458e23b0af5491420570c921818e0d76278e58c26ad3b` |
| `mono.engine/scene/benchmarks/Skinning.cpp` | `d214538f1789067a9dce9cb4520ff71239b0d162a94681405cf7e02187ba556e` |
| `mono.engine/core/include/engine/core/Profiling.hpp` | `069fc66999abe5501142646d37f3ebbf4c48c4403c33197da860fe1e996efc6a` |
| `mono.engine/core/src/FrameGraph.cpp` | `b0d7fc42304aacb55f2b7179dc9c5a7d3d517fac508a415230043624f864fbb0` |
| `mono.engine/core/src/HeapProfile.cpp` | `ba21a3a1c61db50ff995a7065c8245bd2ddfecd34d06aa7053237c82cc1c6f49` |
| `mono.engine/ecs/src/Instances.cpp` | `92b3a9903facbd6a9c7225473de88c19aba7d77c99f3884ae0ee72ea24c0a2b9` |
| `mono.build/benchmain/BenchMain.cpp` | `70aa83f22c7791b398ff22f52e4dd3b979ad64b3b3f1610ab043cea83b89a004` |
| `Justfile` | `9ee89dc43a1340e56983a68ce484cabc9459124c59cf866de5d4a8d6fb3528f4` |
| `profile/capture.json` | `e32bbe422efb58dbe03c53156cb4f4356952408128b60dd8b98a7ce43dfe1166` |
| `profile/manifest.json` | `fc43ac9ace79eea6f4ef984f7ade83b1af2663998da7dd018a7568fc7f15050a` |
| `canonical/capture.json` | `d265b8d7ffa459cdc56835e7485e0197453d4df25d57fd56d2bc75c26e2420a1` |
| `canonical/manifest.json` | `9c950e213d32de1db19780318eadfe957bd22fee016c20c31ae93fa1b1d0d945` |
| `background-processes.json` | `483143cc486406bfe4f232bb130c997c1921c5f9f46df6b71704f0c97a1b436d` |


## Within-call pose scratch reuse experiment, 2026-10-01

The candidate retains resolved-frame and readiness scratch within one
ResolveBones invocation, reassigning the full logical range to each nonempty
rig's base/false values. Empty rigs do not observe scratch. Gather, descendant
traversal, stable sort, duplicate/lower-parent policy and arithmetic/write order
remain unchanged. Storage ends on return or exception; this is not a cross-tick,
world or TLS pose cache. Higher capacities remain live through later rigs'
gather/sort work, and growth can hold old and new storage simultaneously. Lower
allocation churn does not by itself imply lower peak residency.

Focused skinning passed 631 assertions in 13 cases (seed 2405221923); full scene
passed 521,993 assertions in 625 cases. The mixed-rig control includes large,
small, empty and sparse rigs and deleted-parent changes, verifying reset flags
and frames independently. Formatting and diff checks passed. Complete canonical
comparison against the frozen baseline passed all 78 calls, including every
Entity ID and seven raw WorldFrame words, clocks and versions; canonical digest
remains `8f610b665e4b4ecfa6ae45538b23bc366d67f04b2064d902155e0fd92f62b284`.
Independent source review accepted the frozen candidate; canonical and runtime
allocation gates passed. The review artifact is
`/tmp/atomic-skinning-pose-reuse-review.md`, SHA256
`83431468b24bc6a3204c328689dc0c49b616091881da13176a47dde251ed8390`.

Four balanced captured pairs ran AB, BA, AB, BA, each with the same six rows,
eight warmups and five samples. There are 312 frames per binary, 624 combined;
all zero-drop and exclusive heap reconciliation gates passed. Direct comparison
of all 312 matched frame records confirms identical input/output hashes, exact
clocks, workload dimensions, animation flags and write counts. Allocations are
stable in every matched row/call: 32x64 saves 55,800 bytes and 62 blocks per call;
16x256 saves 108,000 bytes and 30 blocks. Single-rig rows stay at 3,184 bytes and
22 blocks. Allocation savings are established; timing results are mixed.

All measured calls have identical live bytes and live blocks before and after
resolution. The sole residency increase is first warmup row 0, call 1: 32 bytes
and one block, identically in both binaries in each of the four pairs. All later
calls have equal before/after live counters. This finite warmup observation is
not a growth-slope or leak result.

| Row | Baseline / candidate mean owner ms across four pairs | Baseline / candidate allocated bytes | Baseline / candidate blocks |
| --- | ---: | ---: | ---: |
| 1x32 static | 0.00482720 / 0.00517730 | 3,184 / 3,184 | 22 / 22 |
| 1x32 animated | 0.00572190 / 0.00526290 | 3,184 / 3,184 | 22 / 22 |
| 32x64 static | 0.24625330 / 0.23735500 | 141,544 / 85,744 | 557 / 495 |
| 32x64 animated | 0.27906295 / 0.27852180 | 141,544 / 85,744 | 557 / 495 |
| 16x256 static | 0.45056950 / 0.44452690 | 287,208 / 179,208 | 350 / 320 |
| 16x256 animated | 0.52170820 / 0.52146320 | 287,208 / 179,208 | 350 / 320 |

Four additional balanced uncaptured pairs are preserved in `normal-pairs.json`.
Their broad BENCH minimum samples include authored preparation, verification,
hashing and reporting; they are not resolver-owner timings. Results and spreads
vary by row and host load. Both captured and uncaptured runs use diagnostic
heap-hook binaries at `-O3`, not a shipped hook-free build. Baseline opens two
constructor heap scopes per rig; candidate opens one reset scope, so measured
instrumented timing includes that diagnostic difference. No broad performance
gain or shipping improvement is established. Busy-core differences remain
descriptive; no subtraction adjusts timings. Summed tag peaks are not process
peaks, and no leak or allocator-RSS claim is made.

Candidate source Skinning.cpp SHA256 is
`b26deea9486b623edb6e03685169768e7841eef8b3210db446990d78d335dbc3`;
candidate benchmark binary SHA256 is
`e1fcca9aac40a6aba6216db117a240cc7f23842d376560801382e5a9d643007e`.
Baseline binary remains
`5ac3fcf9eefdaea844d50b9a8011c57ae14979a9ceb5cdb5e416a913dc6aabb7`.
Exact sources, binaries, capture manifests and numeric evidence are preserved
under `.cache/build/bench/evidence/skinning-pose-reuse-2026-10-01/`.
Raw stdout and complete canonical words were never written to files; aggregate
stdout digests are retained in each capture. Evidence hashes follow.

| Evidence | SHA256 |
| --- | --- |
| `canonical/capture.json` | `0b22d2c8ba69617a594485e147826214e54350b6b03a5d33875b76af206be6b1` |
| `canonical/manifest.json` | `2c02f9ed4683accab1ff8b01d1bbfba1a36e850502418d06766d2c376f7360e8` |
| `normal-pairs.json` | `1f9b68211e4b0908176f807357e1deca7278b007aae85fadfec032076b7d4cc2` |
| `pair-1-after/capture.json` | `a9657b9d74b5dd0cb7e2fb36dbf4a32c555dde4547a35cc9e74734b2c8dfd61c` |
| `pair-1-after/manifest.json` | `c47f63d9584a578b2330726b7121550ab993449089f7db3f32479471e1317c8a` |
| `pair-1-before/capture.json` | `21de63ff75fb60818688210d6b3acf3a84e48c437e3f2ef5a1d50cda6c623a54` |
| `pair-1-before/manifest.json` | `7a7945ca32bedb59a5f7f9b7c591732964962ebe473685394a0f7524b08c54aa` |
| `pair-2-after/capture.json` | `fc777d3f915d17fd6a52b06d5a517565df260523e7b2d349bc4fa85fe82d6139` |
| `pair-2-after/manifest.json` | `0b21ddcb0d6c18432c409b4718bff03792b8ac40a21eb14c3d174de55f2b8a2c` |
| `pair-2-before/capture.json` | `f51766d8552679fe636d402aed3733ba841b6267804b87b6c012cee3e6fd415c` |
| `pair-2-before/manifest.json` | `312adcfb8358b3179ae67cfcc6554dbb98ba3081de1b03426e3df8199eb318de` |
| `pair-3-after/capture.json` | `07ea46d2e0c0e616c46307af8b9367278f161dfb5ef3c22a3bb5ef68a3e1f430` |
| `pair-3-after/manifest.json` | `441f52b7a0f4f4473975cfd5b422c78f5550b9868b78b054f0b1a8fc10b26aee` |
| `pair-3-before/capture.json` | `7733ac05c9268a623d009e1691ef00c48d5db78fac92f0e74587ff57d0a5fe0c` |
| `pair-3-before/manifest.json` | `cd01a5ad4ef7717ec2b64644ff44629bf161348a7bd8083621c7b5c79529e6e3` |
| `pair-4-after/capture.json` | `fefd7fd36fda8150e3d76ae96defb0f4b17cbfea1cd8de3ec2081cca126e360b` |
| `pair-4-after/manifest.json` | `97cb2c16a039bf6d8c325e5d0f3e2489c58761da8d9c3930fe4805a2f4230f56` |
| `pair-4-before/capture.json` | `5c476db8214bac1f278baf95ca7e11b48345aeb84a00becee0c8d5e8ddca7f19` |
| `pair-4-before/manifest.json` | `4c4a952949722f9dc81c55e7716e22d44c80cc255d19a73e3dd4c0aba6aee20d` |

Exact candidate measurement-source hashes:

| Source | SHA256 |
| --- | --- |
| `mono.engine/scene/src/Skinning.cpp` | `b26deea9486b623edb6e03685169768e7841eef8b3210db446990d78d335dbc3` |
| `mono.engine/scene/include/engine/scene/Skinning.hpp` | `29edc93370f882b63767a28696bcd264fdd9583c5bd980c69d77ff48ddcf4dd7` |
| `mono.engine/scene/tests/Skinning.cpp` | `3c80231dae7c1ee26a9998a479adc78192b3e2d77d3117a3fe562eb4120eaf52` |
| `mono.engine/scene/tests/fixtures/SkinningParity.hpp` | `92f938474275bb8bc33458e23b0af5491420570c921818e0d76278e58c26ad3b` |
| `mono.engine/scene/benchmarks/Skinning.cpp` | `d214538f1789067a9dce9cb4520ff71239b0d162a94681405cf7e02187ba556e` |
| `mono.engine/core/include/engine/core/Profiling.hpp` | `069fc66999abe5501142646d37f3ebbf4c48c4403c33197da860fe1e996efc6a` |
| `mono.engine/core/src/FrameGraph.cpp` | `b0d7fc42304aacb55f2b7179dc9c5a7d3d517fac508a415230043624f864fbb0` |
| `mono.engine/core/src/HeapProfile.cpp` | `ba21a3a1c61db50ff995a7065c8245bd2ddfecd34d06aa7053237c82cc1c6f49` |
| `mono.engine/ecs/src/Instances.cpp` | `92b3a9903facbd6a9c7225473de88c19aba7d77c99f3884ae0ee72ea24c0a2b9` |
| `mono.build/benchmain/BenchMain.cpp` | `70aa83f22c7791b398ff22f52e4dd3b979ad64b3b3f1610ab043cea83b89a004` |
| `Justfile` | `9ee89dc43a1340e56983a68ce484cabc9459124c59cf866de5d4a8d6fb3528f4` |


## Persisted Cone source-family diagnostic profile, 2026-10-01

The `bench` preset measured two static frame-zero persisted imagegraph workloads. Each uses authored Material → Cone → Transform → GetData nodes with nonidentity local transforms. The three selected outputs are transformed mesh, GetData position, and a direct Material output. The fixture independently checks ordered vertices, normals, UVs, tints, edges, materials, transforms, and GetData values. This is a CPU source-family fixture; it does not establish GPU output or parity with the licensed Pixel Composer executable.

Cone8 uses default side and smooth-side controls, producing 24 ordered vertices per part, 48 total vertices across two parts, and 16 edges. Cone104 explicitly sets 104 sides and smooth-side normals, producing 312 vertices per part, 624 total vertices across two parts, and 208 edges. Each family ran eight warmups and five measured calls, 13 frames each. Together the 17-family capture contains 221 frames, 48,672 spans and 1,302 emitted heap records. All previous 15 families retained their input and output FNV64 values. The frame and heap drops were zero, and exclusive tagged allocated bytes and blocks reconciled exactly with process deltas on all 221 frames. The source-family verification run passed 34 assertions in one case (seed 197483942); this exercises all 17 source-family fixture rows, not the focused MeshCone test suite. After capture, the full imagegraph suite passed 474,961 assertions in 628 cases (seed 3872959229), including Cone and Torus; that result also predates the whitespace-only formatting correction.

| Cone workload | Input FNV64 | Output FNV64 | Total vertices / edges | Mean frame owner ms, 5 samples | Min / max ms | Process allocated bytes / blocks per call | Output payload bytes per call |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 8 sides, source defaults | 715445389982582813 | 16525567451453054347 | 48 / 16 | 0.1985384 | 0.197733 / 0.201300 | 158,018 / 685 | 76,308 |
| 104 sides, smooth side | 6162299210649103663 | 7099879238798767968 | 624 / 208 | 0.286396 | 0.231747 / 0.332527 | 361,058 / 691 | 279,060 |

Both workloads emitted ten node executions and thirteen value outputs per call. The measured frame owner is the interval around `Graph.Evaluate` only. It includes processor work and three selected output evaluations against the already compiled persisted graph. Cone compilation and fixture setup occurred before the frame; fixture verification, hashing and reporting occur after it. Per five-sample capture, Cone and Transform each emitted two spans per sample, Material emitted five and GetData emitted one. These nested phase timings are included within the frame owner and must not be added to it. Mean exclusive Cone-tag work was 0.0015209 ms for Cone8 and 0.0151475 ms for Cone104; these five-sample diagnostics are not independent end-to-end timings.

| Exclusive heap tag | Cone8 bytes / blocks | Cone104 bytes / blocks |
| --- | ---: | ---: |
| process root | 11,858 / 36 | 11,858 / 36 |
| `imagegraph.evaluate` | 54,756 / 519 | 54,948 / 525 |
| `imagegraph.processor` | 12,432 / 30 | 12,528 / 30 |
| `imagegraph.node.other` | 24 / 1 | 24 / 1 |
| `imagegraph.mesh.material` | 23,444 / 32 | 23,444 / 32 |
| `imagegraph.mesh.transform` | 27,312 / 32 | 128,688 / 32 |
| `imagegraph.mesh.get_data` | 1,088 / 3 | 1,088 / 3 |
| `imagegraph.mesh.cone` | 27,104 / 32 | 128,480 / 32 |

The Cone tag ended each measured call with zero live bytes and blocks. Its maximum simultaneously live tagged bytes were 13,552 for Cone8 and 64,240 for Cone104. Process live bytes ranged from 146,674,707 to 148,796,899 and 153,645,249 to 155,767,441 respectively. Process peak bytes ranged from 146,694,363 to 148,816,555 and 153,766,281 to 155,888,473. Heap-profiler overhead ranged from 1,833,984 to 1,840,384 bytes and 1,855,712 to 1,862,112 bytes. These short captures do not establish a leak, allocator RSS, or global memory bound. The captures show profiler and process totals at each selected row, not a stable process baseline across a soak.

This was a diagnostic `bench` build at `-O3` with heap hooks. Baseline system load was 3.623106046 busy cores and during capture was 4.559815535, a descriptive increase of 0.936709489. Elapsed time was 2.201843457 seconds and child CPU was 1.970641 seconds. Owner timings are unadjusted; load subtraction does not correct them. These are workload measurements, not before-and-after comparisons, shipping costs, or a performance improvement claim. The run establishes no GPU or licensed-executable parity and does not complete full M5 validation.

The preserved evidence directory is `.cache/build/bench/evidence/cone-source-families-2026-10-01/`. Its manifest SHA256 is `f464fc5a19f5a14a3fedf19bbebb6aa928a51ea0f84c2533f2780eeeb89481ef`; parsed capture SHA256 is `a579385f7baab2b86e0d58cac3100ce268f8113648814346539dcb5ead6c33eb`; aggregate stdout SHA256 is `8376b49112e7fc005fa22588f5c1fec2e2171cd94ace353f3a22bc2d2c93881c`. Raw stdout was not saved. Benchmark binary SHA256 is `d25f0518d0c3fc425027ac60bb43b47f6c6415a55cfc70487d117de27ed4094d` and focused test binary SHA256 is `071736ed03fa95466b23bf7d6e77c45ccc34d589ac000ed6dcf00f5b4cfe048c`.

Exact source hashes from the frozen capture manifest:

| Source | SHA256 |
| --- | --- |
| `mono.engine/imagegraph/tests/fixtures/SourceFamilyEvaluation.hpp` | `6c25087c3b4c86998f1e6283220c59cef9a913c06e60e000d7b77835069ac970` |
| `mono.engine/imagegraph/tests/SourceFamilyEvaluation.cpp` | `5caea98737887c02ebaa8aa57b049f876d743c2ae0965e09d17fc6fe33c9584e` |
| `mono.engine/imagegraph/benchmarks/SourceFamilies.cpp` | `7e05c372720f42ec633191705dc57795405cf5ccecc59c765caa46d30974a3b1` |
| `mono.engine/imagegraph/src/nodes/MeshOps.cpp` (captured Cone implementation) | `640036c6a40ad664a200ed13805c1eb4c1d37169ead5bd5deab667be2447f373` |
| `mono.engine/imagegraph/tests/MeshCone.cpp` | `f466a9156be33416312bac827b709b741b3eeef6edfc2312ccdd12d2f9f88dfc` |
| `mono.engine/imagegraph/tests/MeshCube.cpp` | `d36d47fddf587454406efa60b2e5d7e2fe932581e1cdaa42ab6bd0f38fac161c` |
| `mono.engine/imagegraph/src/Document.cpp` | `62608054bf9345290cf85fcde6f7aa4d06984c74a58abfa94878844e6d984160` |
| `mono.engine/imagegraph/src/ProcessorBatch.cpp` | `0452843a140f2eb1bfd8efa591cbbdbf3e460bf9cda8fc8d490b24d30b8cbe93` |

The source hashes describe the frozen measured snapshot, not necessarily the current working tree after later source edits. A subsequent whitespace-only formatting correction to the fixture and benchmark source passed full-file clang-format-21 checks. The hashes above identify the measured pre-correction source.


## Persisted Torus source-family diagnostics, 2026-10-01

Native Torus correctness passed 342,989 assertions in 12 cases, including
ordered geometry, source integer-neighborhood snapping, fractional raw slice
controls, all four processor schedules through real persisted producers, owned
materials and atomic refusal gates. The corrected oracle was independently
reviewed. Typed Quaternion routes still do not verify Euler/display conversion;
nested authored arrays remain restricted by current document admission. GPU and
licensed executable parity are unverified. This does not complete M5.

Two persisted Material -> Torus -> Transform -> GetData fixtures add default
16x8 cells and smooth 31x22 cells with toroidal angle .5, poloidal angle 13 and
constant right-edge twist .25. Both verify all ordered vertices, normals, UVs,
tints and duplicated edges, full owned material descriptors and image bytes,
local transforms and GetData output independently. All 19 source-family rows
passed 38 assertions in one case; the full imagegraph suite passed 474,965
assertions in 628 cases (seed 4223399216). Formatting and owned diff checks passed.

The captured frame contains Graph.Evaluate only, including three selected output
traversals. Fixture setup/compilation, verification, hashing, recorder snapshots
and reporting are outside it. Diagnostic bench builds use -O3 and heap hooks.
Each row has eight warmups and five measured calls. There are 247 completed
frames, 49,660 spans and 1,510 emitted heap records. Frame/heap drop gates passed;
exclusive tagged allocated bytes and blocks reconcile with process deltas on
all 247 frames. Every preceding 17 family input/output FNV64 pair is unchanged.

| Workload | Total vertices / edges | Input FNV64 | Output FNV64 | Mean frame ms | Allocated bytes / blocks per call |
| --- | ---: | ---: | ---: | ---: | ---: |
| 16x8 default | 768 / 512 | 11684118377324908742 | 3400798290953155578 | 0.1747068 | 418,484 / 545 |
| 31x22 smooth, twisted | 4,092 / 2,728 | 3677753173856075844 | 131479785344590960 | 0.4687404 | 1,801,790 / 557 |

Phase means in capture.json average individual recorded spans, not aggregate
work per call. Nested inclusive phases must not be added to their owner. This
also clarifies the preceding Cone phase means: they describe individual spans.
Process live/peak and overhead records include retained recorder allocations
from earlier calls and families. They do not establish steady process residency,
a leak, allocator RSS or a global memory bound. Torus tagged live storage is
zero at the captured boundaries; this does not make whole-process live zero.
These are workload diagnostics, not an improvement or shipping-cost claim.

Baseline machine load was 4.526390604 busy cores; during capture
it was 6.653061713. The difference 2.126671109 is descriptive.
Elapsed time remains 2.200490636 seconds; no subtraction corrects timings.

Exact evidence lives in `.cache/build/bench/evidence/torus-source-families-2026-10-01/`.
Manifest SHA256: `ae9afbbddfd2c538118c78b65b5bd1ff4842dfab25b76718607f16e5c028c516`.
Capture SHA256: `2cb972440c9c37da116d1ee9ebb7caf93479d1f14c0d89718d61307ee7904b4d`.
Binary SHA256: `4fa6fcb657992423b090d061e5487c511a3d1ca964dcc5900085dd4529104c23`.
Aggregate stdout SHA256: `b693379f15ec207b17da864aa005dd03acbd719617e23a4d8e91260c5b68210a`.
Source copies and exact hashes are preserved by that manifest. Raw benchmark
stdout was not saved. Source-family review is separate from these runtime gates.


## Persisted UV Sphere source-family diagnostics, 2026-10-01

Native UV Sphere correctness passed 711,387 assertions in 13 cases after the
public projection-array carrier correction. The full imagegraph suite then
passed 1,186,353 assertions in 641 cases. After the source-family extension,
all 21 source-family rows passed 42 assertions in one case; the full imagegraph
suite passed 1,186,357 assertions in 641 cases. These are root-observed dev gates,
not a new test run by the audit draft author.

The initial focused gate failed two of 13 cases: 660,583 assertions, 660,581
passed. Both stopped at Read before Compile because the fixture authored an
unsupported enum-array carrier (`a enum 3 e 0 e 1 e 2`). The correction uses
persistable flat Integer arrays `{0, 1, 2}`, matching the source EScroll numeric
array getter. It preserves the latent source Mercator projection case and all
nine-slot, four-mode scheduling assertions through public Write/Read, Compile
and Evaluate. Production geometry was unchanged. Initial failure evidence is
preserved at `.cache/build/dev/evidence/uv-sphere-2026-10-01/manifest.json`.

The pinned `Node_3D_Mesh_Sphere_UV` and `d3d_uvsphere` source at commit
`b69eca232217360cf1502ef0223523d818606652` defines radius .5, positive-Y degree
trigonometry, source-swapped horizontal/vertical loop roles, ordered triangles
and two duplicated cell edges. Smooth normals retain the position vector;
flat normals use the normalized source-ordered cross product. The fixtures
independently verify every ordered position, normal, UV, tint and edge, full
owned material descriptors and image bytes, local transform records and
GetData output. They do not use executor helpers as geometry oracles.

The two new families are static persisted Material -> UV Sphere -> Transform
-> GetData graphs. Each performs three selected output evaluation traversals:
transformed mesh, GetData position and a direct Material output. Geometry uses
default horizontal8/vertical16 flat Lambert controls, or bounded
horizontal31/vertical22 smooth Equirectangular controls. Both retain nonidentity
local transform records. The counts below are total geometry counts, not counts
per triangle or per part.

The diagnostic `bench` build used -O3 and enabled heap hooks. Each of the 21
families ran eight warmups and five measured calls: 273 completed frames,
50,648 spans and 1,718 emitted heap records. The prior 19-family input/output
FNV64 pairs match the frozen Torus capture. Frame and heap drops were zero;
exclusive tagged allocated bytes and blocks reconciled with process deltas
on all captured frames. Root reviewed/applied the source and ran correctness
and capture gates. Independent source review accepted these two CPU descriptor
families; its report and hashes are preserved with the runtime evidence.

| Workload | Total vertices / edges | Input FNV64 | Output FNV64 | Mean frame ms | Min / max ms | Allocated bytes / blocks per call | Output payload bytes per call |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| H8/V16 default, flat Lambert | 768 / 256 | 17310277558349535474 | 14177316402681132143 | 0.1628538 | 0.161744 / 0.165651 | 365,578 / 520 | 303,100 |
| H31/V22 smooth Equirectangular | 4,092 / 1,364 | 12417305954225191630 | 5245748180151018525 | 0.4155888 | 0.383881 / 0.430248 | 1,536,202 / 532 | 1,473,148 |

The frame owner encloses Graph.Evaluate only and all three selected output
traversals. Fixture setup and compilation, analytical verification, hashing,
recorder snapshots and reporting are outside it. Each call emitted eight node
executions and eleven value outputs. Per five-sample row, Sphere and Transform
each emitted ten spans, Material fifteen and GetData five. Phase means average
individual recorded spans, not aggregate work per call; nested inclusive phases
must not be added to their owner. Mean Sphere-tag self time per span was
0.0153898 ms for default and 0.0892648 ms for bounded geometry. These are
instrumented workload diagnostics, not shipping cost or an improvement claim.

| Exclusive heap tag | Default bytes / blocks per call | Bounded bytes / blocks per call |
| --- | ---: | ---: |
| process root | 11,868 / 36 | 11,868 / 36 |
| `imagegraph.evaluate` | 39,354 / 388 | 39,738 / 400 |
| `imagegraph.processor` | 8,976 / 24 | 9,168 / 24 |
| `imagegraph.node.other` | 24 / 1 | 24 / 1 |
| `imagegraph.mesh.material` | 14,124 / 20 | 14,124 / 20 |
| `imagegraph.mesh.transform` | 145,176 / 24 | 730,200 / 24 |
| `imagegraph.mesh.get_data` | 1,088 / 3 | 1,088 / 3 |
| `imagegraph.mesh.sphere_uv` | 144,968 / 24 | 729,992 / 24 |

Sphere-tag live bytes and blocks were zero at the captured boundaries; maximum
simultaneously live tagged bytes were 72,484 and 364,996 respectively. Process
live bytes across the five measured calls ranged from 174,942,729 to 177,064,217
for default and 182,210,738 to 184,332,226 for bounded geometry. Process peaks
ranged from 175,079,653 to 177,201,141 and 182,932,686 to 185,054,174. Profiler
overhead ranged from 1,911,776 to 1,917,152 and 1,930,048 to 1,935,424 bytes.
These process observations include cumulative recorder and earlier-family
allocations. They do not establish steady residency, a leak, allocator RSS or
a global memory bound; zero Sphere-tag live bytes do not mean process live zero.

Baseline machine load was 2.5698819216402424 busy cores; during capture it was
3.149121296509278. Their difference, 0.5792393748690357, is descriptive only.
Elapsed time was 2.181560934986919 seconds and child CPU was 1.953337 seconds.
The recorded background-process counts were `{}`. No load subtraction corrects
owner timings and no before/after speed gain is claimed.

Exact evidence lives in `.cache/build/bench/evidence/uv-sphere-source-families-2026-10-01/`.
Manifest SHA256: `51d729af1a0a4c375998046579fddff9d060a2851626d7d044b11cda68d3ca41`.
Capture SHA256: `c9aafa6d25390eebb1d640b85129c834feefe110d2eab25ed22f263f3472e86d`.
Benchmark binary SHA256: `0fc8168accef93cd36d6d936f12435261a574617a1f849d36323b576a1cc5cdc`.
Source-freeze SHA256: `98a052fbef3bea477f516927ba90c3074abb90ef00dc214b7d160193f7e06d89`.
Aggregate stdout SHA256: `a97549550197069064878241d35b597e68cdec8fb4ec1f784708ca21577b4af2`.
Raw benchmark stdout was not saved. The source-freeze identifies the following
scoped source copies, rather than the entire binary dependency tree:

| Source | SHA256 |
| --- | --- |
| `mono.engine/imagegraph/src/nodes/MeshOps.cpp` | `37a6257834e962ba20e439919dd50409ec76b50f564469ca3a087cd37067a7ce` |
| `mono.engine/imagegraph/tests/MeshSphere.cpp` | `bfb0a5a62c8d3b8fc138e26f24d04ca418545d0397da0831a7c120dc1e73cb36` |
| `mono.engine/imagegraph/tests/fixtures/SourceFamilyEvaluation.hpp` | `75b011f7312af4eb4fa678c2550b1ff55bd36874f1703d7b27761de780c7f3e0` |
| `mono.engine/imagegraph/tests/SourceFamilyEvaluation.cpp` | `abec3e530612ac7d3f06539834551fc16277e6966c152dac1d194729dac7554a` |
| `mono.engine/imagegraph/benchmarks/SourceFamilies.cpp` | `f9204c849996aaaebf682bc44ecc53fc559ec3504fb4d4f8d725730f1ea3182f` |

No catalogue row is promoted. Icosphere remains a separate row. Raw active
fractional horizontal counts retain an explicit UnsupportedExecution diagnostic
because GML real array-index coercion is unverified; source-defined raw vertical
ceil loops retain the original denominator. GPU output, licensed executable
parity and Quaternion Euler/display conversion were not verified. This phase
does not establish full M5 completion or a performance improvement.


## Convex hull local weld-chain acceptance, 2026-10-01

The local-neighbour weld-chain change preserves the six fixture outputs exactly. The production change is confined to the kernel path in `mono.engine/collision/src/ConvexHull.cpp`, with owned coverage in `mono.engine/collision/tests/ConvexHull.cpp`. Benchmark instrumentation in `mono.engine/collision/benchmarks/ConvexHull.cpp` is recorded separately. The focused dev suite passed 425 assertions in 18 cases, and the full collision suite passed 66,835 assertions in 31 cases. The correctness manifest records exit code 0 for both runs.

The baseline and candidate each emitted 97 canonical records: 19 preflight and 78 owner records. The comparison checked all 197,040 canonical hex bytes in memory and reports full record equality. Their canonical digest is `269b9996b632bc29254a1809f928c9c3c71bd1c8843080310dc3411c6da4e73a`. Canonical words were compared before digest-only persistence; raw benchmark stdout was not saved.

The uncaptured timing run used four fresh processes in ABBA order, with five samples and eight warmups per row. A denotes baseline and B candidate. The minima column gives each process minimum in A/B/B/A order, in milliseconds. The paired deltas are candidate minus baseline, so negative values mean lower minima in those pairs. Allocation values come from the separate captured profile run and are bytes / blocks per call.

| Fixture | Uncaptured BENCH minima A/B/B/A (ms) | Candidate minus baseline AB / BA (ms) | Captured allocation A to B (bytes / blocks per call) |
| --- | --- | ---: | ---: |
| Hull box 1024 interior, 1,032 points | 0.349137 / 0.301387 / 0.306336 / 0.365397 | -0.047750 / -0.059061 | 128,352 / 2,171 to 115,968 / 1,140 |
| Hull box 30,000 interior plus seam duplicates, 40,008 points | 12.589510 / 11.589889 / 11.504327 / 12.963844 | -0.999621 / -1.459517 | 3,037,784 / 59,709 to 2,761,868 / 29,911 |
| Hull octagonal prism 1024 interior, 1,040 points | 0.380285 / 0.331454 / 0.336763 / 0.383452 | -0.048831 / -0.046689 | 133,584 / 2,244 to 121,104 / 1,205 |
| Hull octagonal prism weld-cell seams, 1,168 points | 0.406855 / 0.349097 / 0.350479 / 0.394722 | -0.057758 / -0.044243 | 136,944 / 2,328 to 124,648 / 1,247 |
| Hull flat64 points plus duplicate and nonfinite, 66 points | 0.018755 / 0.015389 / 0.015549 / 0.018705 | -0.003366 / -0.003156 | 6,668 / 137 to 5,916 / 74 |
| Hull128 exposed paraboloid capped, 128 points | 0.086482 / 0.079269 / 0.080421 / 0.086303 | -0.007213 / -0.005882 | 65,388 / 878 to 63,852 / 751 |

For the 40,008-point fixture, the separate capture measured 275,916 fewer allocated bytes and 29,798 fewer allocations per call. Its uncaptured BENCH minima were lower by 0.999621 ms and 1.459517 ms in the two AB / BA pairs. Those minima describe the row under the BENCH harness. They do not isolate the narrower `collision hull build` owner span or represent full-world collision cost.

The profile capture was a separate ABBA run. Each of its four processes recorded 78 frames and 429 spans, for 312 frames and 1,716 spans total. Frame and heap drop counts were zero. The executable gates checked phase hierarchy, independent geometry, byte and block reconciliation, and returned-capacity release. Captured frame timing includes verification and release work outside the narrower build owner. The uncaptured BENCH minima and captured profile timings are not interchangeable.

The candidate weld path keeps a `next` index vector and reserves one `size_t` per input point (`points.size() * sizeof(size_t)`). For 40,008 points on this 64-bit ABI, the reserve requests 320,064 temporary bytes. Duplicate-heavy or nonfinite-heavy clouds were not measured and may use more scratch than the baseline path, so these fixture results do not establish a universal allocation or residency reduction.

The `sum_tag_peak_bytes` field sums per-tag peak counters. It is not simultaneous live memory or a universal peak bound. Busy-core differences are descriptive only: 0.442289 to 0.650844 cores across uncaptured runs, and -0.094998 to 0.607838 cores across profile runs. No timing correction was applied. The namespace-visible named-process snapshots were `{}` before and after each process; this does not establish that the host had no background work. This is a local acceptance for these fixtures, not a universal speedup, a release-build gain or a global peak-memory claim.

Exact evidence is in `.cache/build/bench/evidence/convex-hull-weld-chain-2026-10-01/` and `.cache/build/dev/evidence/convex-hull-weld-chain-2026-10-01/`. The benchmark manifest SHA256 is `3f5099f24b2c2d68c0ec1ef6aa35e15dc82fe64f94b967c63451b25bfe026092`; canonical JSON SHA256 is `0f6ee795d199cf12e74b3e463934e9cbb46130639b7e1e754581920a39a841a8`; uncaptured JSON SHA256 is `7349cf1326bc6f8f659492c68e3f471d69ad56f4c362db3917868e514fcf74a8`; profile JSON SHA256 is `93dd708c0c82ac9f507e80106045014f81429b2659eaaaabe9f152d4dce1d5e9`; correctness JSON SHA256 is `25f3ff5ea80705ee3b25e01cee2519e758d515be7162962f1d80dc17a39628a4`. The measured kernel source hashes are baseline `ddfe44ddc5f7989889404dd837f59365dcd3eb0b1220da33e1646bd40ed842cd` and candidate `d91ce6bf42f00bd26ba492788c51526edd4346bc9bf5eda88ca5350becf0abea`. The focused test hashes are baseline `652bf759a45fa4df3e638da41a8ed7024c2bdad16bf99e8d7a441610c080d15c` and candidate `8370f470afde6a1d58790fac7834d4dd9e7dc023b60b54db28170523fa33e76b`. The separately instrumented benchmark source hashes are baseline `951ad6a377abb6459060a1921b1be192fee77069bd8ab4d0ce7362305979ce45` and candidate `73952c9e29eb21606ff4863cd5fdb5c59947deebf299ba9aa7e2be70cf22eca5`; the manifest also records both binary hashes.

## Audio Window observer and Studio panel diagnostic, 2026-10-01

Native `pc.audio_window` evaluation and the headless Studio `AudioWindowPanel::Update` observer now resolve the selected node's linked or animated inputs. The observer copies up to 320 evenly spaced channel-zero waveform samples and records source shape, sample rate, cursor and exclusive interval markers. This is a native CPU observation path; it does not establish licensed executable behavior or a rendered thumbnail.

The `bench` capture completed 299 core frames and 51,870 spans across 23 source families, with eight warmups and five measured calls per family. All 21 prior family input/output hash pairs remained unchanged. The Audio Window rows use 4,096 and 65,536 samples per channel. Their `core.aggregates` means were 0.229872 and 2.6678114 ms. The corresponding `source-profile` records report 719,663 / 490 and 10,550,063 / 490 allocated bytes / blocks per call. Each row retained its input/output hashes across five measured samples. The aggregate timings and source-profile allocation records are separate measurements.

The Studio capture completed 104 frames and 11,752 spans across eight rows, each with eight warmups and five measured batches of 16 `Update` calls. Frame and heap drops were zero. Table timings are mean `studio-audio-window-profile` batch owner milliseconds for 16 `Update` calls. Observer bytes and blocks are inclusive tagged totals for that batch; process totals cover the measured process allocation interval. The two counters differ because allocations can occur outside the owner scope.

| Samples per channel | Batch | Owner ms | Observer bytes / blocks | Process bytes / blocks | Logical snapshot payload bytes |
| ---: | --- | ---: | ---: | ---: | ---: |
| 4,096 | Unchanged | 0.0008976 | 0 / 0 | 2,684 / 8 | 0 |
| 4,096 | Cursor | 0.6342452 | 2,243,960 / 1,476 | 2,253,244 / 1,504 | 1,066,128 |
| 4,096 | Source revision | 0.6446966 | 2,325,880 / 1,492 | 2,336,502 / 1,524 | 1,066,128 |
| 4,096 | Failure recovery | 0.3184038 | 1,122,800 / 756 | 1,132,084 / 784 | 533,064 |
| 65,536 | Unchanged | 0.0008974 | 0 / 0 | 2,684 / 8 | 0 |
| 65,536 | Cursor | 11.6856582 | 33,701,240 / 1,476 | 33,710,524 / 1,504 | 16,794,768 |
| 65,536 | Source revision | 11.8762106 | 33,783,160 / 1,492 | 33,793,782 / 1,524 | 16,794,768 |
| 65,536 | Failure recovery | 5.9547284 | 16,851,440 / 756 | 16,860,724 / 784 | 8,397,384 |

The unchanged rows allocate zero bytes inside the owner and observer tags but still allocate 2,684 bytes and eight blocks per batch at process level. The 65,536-sample cursor row's 16,794,768 snapshot bytes are a sum of logical retained payloads across captures, not allocated bytes or simultaneous residency. These measurements show the costs of these tested rows, not a release gain, a zero-allocation result or a leak conclusion.

The baseline background reading was 4.2393538 busy cores. During the core and Studio runs it was 4.0033663 and 3.7340307 cores, descriptive differences of -0.2359875 and -0.5053231. No timing adjustment was applied. The visible named-process map was empty in the namespace and does not establish that the host had no background work. The `bench` capture is separate from normal uninstrumented timing.

The focused ordered-context Timeline fixture passed 15 cases and 157 assertions after the fixture context restoration correction. The full Studio run still reported 24 failures across 760 cases, without the prior Timeline abort. This audio observer result does not close M3. Licensed raster and extraction-output parity, drawing cost, device audio and continuous source-control changes remain unverified.

Exact capture and settings are in `.cache/build/bench/evidence/audio-window-presentation-2026-10-01-revision4/` and `.cache/build/bench/evidence/audio-window-presentation-2026-10-01-revision4-inputs/`. Independent source review is recorded in `independent-review.md`; it accepted the native CPU benchmark source and accounting scope. Capture SHA256 is `69ba13cc968527fad2be179797ccf05487a80b3602f60d42b3f7b421971dd9d6`.

## Persisted Icosphere source-family acceptance, 2026-10-01

Root accepted two persisted native CPU workloads: default level 1, flat, and supported level 3, smooth. The source starts from normalized seed vertices, then forms each subdivision point by arithmetic midpoint averaging without radial normalization. Ordered geometry and material, transform and GetData outputs passed independent oracles.

Kernel tests cover levels 0 through 3. Each face emits three vertices and three ordered edges, for 60, 240, 960 and 3,840 vertices and edges at levels 0, 1, 2 and 3. Level 1 is the authored default. Levels 4 and 5 exceed the 4,096 element output cap and are refused during preallocation, rather than silently clamped. A raw fractional value that reaches the kernel returns `UnsupportedExecution` pending proof of GML repeat coercion; ordinary scalar and flat inputs are coerced before the kernel. Native carrier validation currently refuses nested integer arrays before execution; source behavior at that depth remains unverified. The focused kernel suite passed 578,418 assertions in 12 cases, and the full core suite passed 1,764,980 assertions in 658 cases. The source-family fixture passed 50 assertions in one case.

The source-family capture used eight warmups and five measured calls. Across 25 families it recorded 325 frames, 52,858 spans and 2,056 heap records. All 23 prior families retained their complete input/output FNV pairs. The two new rows each run three selected output traversals. The benchmark's nonzero refusal guards did not fire; rows do not expose direct drop fields. Exclusive heap totals reconciled with process deltas.

| Workload | Level | Vertices / edges | Mean frame ms | Allocated bytes / blocks per call |
| --- | ---: | ---: | ---: | ---: |
| Default flat | 1 | 240 / 240 | 0.1416576 | 222,236 / 524 |
| Smooth upper supported | 3 | 3,840 / 3,840 | 0.7022634 | 2,154,524 / 530 |

These are captured `bench` workload diagnostics. They are not release timings, a speedup claim or shipped-cost evidence. Nine formatter hypotheses matched at levels 1 through 3; that is not exhaustive formatter proof or licensed executable parity. GPU output, Euler/display conversion and full M5 remain unverified.

Exact evidence is in `.cache/build/bench/evidence/icosphere-source-families-2026-10-01/`. Root acceptance JSON SHA256: `774cf506fd3cea7d5180ebcd1611faa5eaadf96df7b3d65d35744e8e9e0ca0e3`. Capture JSON SHA256: `b683d8ea60cb3e151e4260aca82b012f8f52120d420cc782cf231e445abaa69e`. Manifest SHA256: `c2777221298f61f0cdd7cf06414c0050883d26cc2a70fbc048448e71f0c1d13f`. Accepted source hashes are `MeshOps.cpp` `d0ee9a72276bbe3e4dc0b478ec04bf2d4275be5b0d84a950f70cf5578b2fe838` and `MeshIcosphere.cpp` `c52275b528dd592f77c0b7a41a94b4624076913a639b08600081c72835a1dfae`.

## Audio terminal snapshot move, 2026-10-01

The terminal snapshot path moves owned audio payloads into the retained snapshot after fallible construction succeeds. The CPU `bench` capture used eight warmups and five measured calls per row. Core input and output FNV pairs matched for all 25 source-family rows. Independent FNV phase and root hierarchy checks passed for all 25 core and eight headless Studio rows across 13 calls per row. These are hash-based parity checks, not literal full-byte proof. Before final formatting, the full core gate passed 1,765,203 assertions in 666 cases and the source-family gate passed 50 assertions in one case. After formatting, the focused snapshot rerun passed 233 assertions in nine cases and the focused Studio audio rerun passed 37 assertions in four cases.

Core source-profile allocations fell from 719,663 bytes / 490 blocks to 391,903 / 480 per call at 4,096 samples, and from 10,550,063 / 490 to 5,307,103 / 480 at 65,536 samples. Core retained-payload counters stayed at 333,165 bytes and 5,248,365 bytes respectively across all 13 before/after records.

Studio process allocations and owner mean times below are per batch of 16 `Update` calls. The owner means are milliseconds. Unchanged rows still allocate 2,684 bytes / 8 blocks per batch at process level.

| Samples per channel | Batch | Process allocation reduction, bytes / blocks | Owner mean ms, before to after |
| ---: | --- | ---: | ---: |
| 4,096 | Unchanged | 0 / 0 | 0.000910 to 0.000892 |
| 4,096 | Cursor | 1,048,832 / 32 | 0.648124 to 0.630114 |
| 4,096 | Source revision | 1,048,832 / 32 | 0.653932 to 0.614070 |
| 4,096 | Failure recovery | 524,416 / 16 | 0.313744 to 0.309036 |
| 65,536 | Unchanged | 0 / 0 | 0.000990 to 0.000894 |
| 65,536 | Cursor | 16,777,472 / 32 | 12.305293 to 9.545505 |
| 65,536 | Source revision | 16,777,472 / 32 | 11.879075 to 10.036875 |
| 65,536 | Failure recovery | 8,388,736 / 16 | 5.893321 to 4.809760 |

Studio retained-payload values were unchanged across all 104 before/after calls: cursor and source-revision rows retained 1,066,128 bytes at 4,096 samples and 16,794,768 bytes at 65,536; failure-recovery rows retained 533,064 and 8,397,384 bytes. These are cumulative allocation reductions, not retained-heap reductions. Timing is descriptive only: there was one before/after pair with five samples after eight warmups, and observed host load differed between captures. No timing adjustment or speedup conclusion is supported. The capture is CPU and headless, and does not establish licensed executable or rendered-output parity.

Exact captures and review are in `.cache/build/bench/evidence/audio-snapshot-move-2026-10-01-{before,after}/` and `.cache/build/dev/evidence/audio-snapshot-move-2026-10-01/`.

## World bus exact collection reserve, 2026-10-01

The exact reserve candidate measured 50 serial chatty worlds on CPU with the optimized `bench` preset and heap profiling enabled. Per measured tick, total allocations fell from 11,344 bytes / 9 blocks to 5,800 / 3, saving 5,544 bytes and 6 blocks. The `Collect` portion fell from 9,144 / 7 to 3,600 / 1 bytes / blocks. The normal control run passed 18 rows.

The profile capture recorded 650 owner frames, 365,950 spans and 3,900 phase records, including 400 warmups and 250 measured frames. The canonical capture compared 650 full independent records, totaling 131,172,600 bytes, and matched before and after byte for byte. Profile output fields also matched. Quiet-workload reserve remains zero by source.

Timing has no conclusion from this single before/after pair under changing machine load. These results establish allocation and output parity for the measured fixture, not general shipping timing. Exact captures are in `.cache/build/bench/evidence/bus-collect-exact-reserve-2026-10-01/`.

## Array transpose exact outer reserve, 2026-10-01

The transpose candidate sizes its outer result from the number of columns. A CPU-only, isolated optimized diagnostic used `-O3` with `MONO_HEAP_PROFILE`. The baseline and candidate used the same compiler, 33 translation units, compiler flags, headers and external libraries, with 2,011 matched source, header and library content pins. The only owned source delta was `mono.engine/imagegraph/src/nodes/ArrayStructureNodes.cpp`.

The capture covered 48 fixtures with eight warmups and five measured dispatches per fixture. It produced 624 independent canonical phase pairs. Full canonical content was checked on each run; the cross-run comparison used hashes and counters, so it does not claim literal cross-run byte comparison. Allocation bytes were unchanged for the other 42 fixtures, and allocation block counts were unchanged for all 48. Each transpose row had the same bytes saved in all five samples:

| Extent | Family | Before bytes | After bytes | Saved bytes per dispatch |
| ---: | --- | ---: | ---: | ---: |
| 16 | mixed | 21,559 | 14,871 | 6,688 |
| 16 | packed | 15,719 | 11,671 | 4,048 |
| 16 | image | 21,815 | 15,127 | 6,688 |
| 256 | mixed | 317,239 | 204,951 | 112,288 |
| 256 | packed | 223,079 | 155,671 | 67,408 |
| 256 | image | 321,335 | 209,047 | 112,288 |

The heap interval runs from `BeginFrame` through dispatch and `EndFrame`, including profiler collection. It excludes context and model construction and canonical setup. Logical payload counters describe value payload, not allocator heap bytes. The results establish reduced measured heap allocation for these transpose fixtures; they do not establish a payload reduction.

CPU busy fractions changed from 1.29% baseline and 4.93% active around the original run to 27.26% baseline and 28.66% active around the candidate run. No timing correction was applied, and no timing or speedup claim is made. Steam and Barotrauma are user-reported background applications; their process activity was not verified.

The isolated correctness run passed 581,087 assertions in 164 cases. The production gate passed 348 assertions in 19 cases, and the candidate is applied. No PXC or Studio parity was measured, and this evidence makes no licensing claim. Exact captures are in `.cache/build/bench/evidence/array-structure-transpose-capacity-candidate-2026-10-01/` and `.cache/build/dev/evidence/array-transpose-capacity-production-apply-2026-10-01/`.

## Native array edit executor profile, 2026-10-01

The array edit change adds native CPU executors for Add, indexed nonrandom Get, Set, Insert, Remove, Find and Zip. The optimized diagnostic used an isolated `-O3` build with `MONO_HEAP_PROFILE`. Its 34 translation units and 2,112 recorded input pins were independently verified. The 48 fixtures ran eight warmups and five measured calls each, producing 384 warm and 240 measured phases. Every phase had one `imagegraph.array_edit.dispatch` owner span.

The fixture guard checks the complete native input carrier, including `ElementType`, array shape and owned image content, plus the independent complete output bytes before emitting FNV hashes. All fixture inputs, output hashes and logical counter records were stable across the 13 calls per fixture. Saved numeric evidence contains hashes and counters, not the full byte streams. Heap interval totals run from `BeginFrame` through dispatch and `EndFrame`, including profiler collection; context and model setup and canonical checks are outside the interval. Logical value payload counters are separate from allocator heap bytes.

The standard production build completed with exit code 0, including the Studio executable. The full core and IO suites and selected Studio suites passed:

| Gate | Assertions | Cases |
| --- | ---: | ---: |
| Standard core | 1,765,680 | 698 |
| Standard IO | 4,060 | 88 |
| Standard selected Studio | 17,899 | 74 |
| Combined isolated core | 581,241 | 178 |
| Combined isolated IO | 4,060 | 88 |
| Combined isolated selected Studio | 21,125 | 79 |

The optimized diagnostic freeze predates the production transpose reserve update, and none of its seven edit fixtures invokes transpose. A fresh combined current-source isolated correctness gate passed before production application; the subsequent standard production gate also passed. The transpose tests were unchanged. The optimized edit profile therefore does not remeasure transpose allocation behavior.

CPU busy fractions around the diagnostic were 1.507% baseline and 3.619% active, a 2.112 percentage point difference. This is load context only. No correction was applied and there is no speedup or new-family comparison claim. Steam and Barotrauma are user-reported; process activity was not verified. Random, Unique, identity and coercion behavior, licensed parity, general PXC behavior and open Studio UI behavior remain unverified. No licensing conclusion follows. Exact acceptance records are in `.cache/build/dev/evidence/array-seven-production-apply-2026-10-01/` and `.cache/build/bench/evidence/array-edit-diagnostic-2026-10-01/numeric/`.

## Metrics registered-row hybrid lookup, 2026-10-01

The original unconditional sorted-index candidate was rejected after both counterbalanced orders showed higher medians for all eighteen steady single-thread fixtures at one and eight rows. The applied hybrid uses the existing linear lookup through eight rows and the sorted interned-ID index above eight, while maintaining all three indexes on mutation. Registration order, Snapshot ordering, Drain/Clear behavior, first-writer time flags and existing Name resolution and lock placement are preserved.

The accepted optimized diagnostic used `-O3` with `MONO_HEAP_PROFILE`, compiling all seven units afresh for the hybrid: six current core compilation units, including three generated unity groups, plus the unchanged Instrumentation benchmark. The hybrid's 708 input pins and reused original baseline's 697 pins were independently verified. Normalized source differed only in `Metrics.cpp`; compiler commands, headers, unity composition and five external archives matched.

Four total fresh profile runs exercised both orders, baseline then hybrid (BH) and hybrid then baseline (HB). Each covered 60 fixtures at 1/8/64/256 rows, eight warmups and five measured phases per fixture: 780 records per run and 3,120 total. The independent full-field model checked every Snapshot, buffered read result, Drain order and reset/persistence result before each phase record was accepted. All four processes exited zero with complete heap and timing hierarchies and no dropped scopes. Saved numeric records contain the executed model gate and measurements, not replayable full result payloads or cross-run canonical byte streams.

| Fixture scope | Measured conclusion across BH and HB |
| --- | --- |
| 256 rows, nine steady single-thread operations | Hybrid owner medians were lower in both orders, with reductions from 29.17% to 55.80%. |
| 64 rows | Reads improved in both orders; some write operations remained mixed. No blanket improvement claim. |
| 1 and 8 rows | Results remained mixed. Five of eighteen steady fixtures had higher hybrid medians in both orders; none had disjoint hybrid-slower sample ranges in both orders. Zero regression was not established. |
| Measured allocation churn | Owner and process allocation bytes/blocks matched baseline in every measured fixture in both orders. |
| Retained storage after Clear | Hybrid added 12,288 process payload bytes and three blocks at the measured 256-row highwater, plus a separate 96 bytes of profiler overhead. |

Owner inclusive wall time is the operation-batch measurement. Threaded cases include dispatch, serialization and joins; joins appear as Idle children, and nested inclusive durations are never added again. Logical operation counts are separate from heap allocation bytes. Process residency also includes fixture/model/read/drained-result storage, so the matched retained difference is an index-storage tradeoff, not an isolated absolute heap size or leak result. The small linear path retains the indexes and does not remove that memory cost.

Eight is a practical tested boundary, not a proven optimal crossover; 16 and 32 rows were not measured. Names and preceding Instrumentation state were warm. Index/gauge/histogram outer capacities survive Clear, while Drain transfers counter row capacity out. Index growth can occur in preseed setup outside the owner, and registration uses ascending interned IDs. These captures do not establish fresh-process cold allocation parity or reverse-ID insertion performance.

Background CPU metadata was retained for every run without subtraction or wall-time correction. Steam and Barotrauma were user-reported background activity, not verified host processes. This acceptance is scoped to larger registered sets in a profiling-enabled optimized diagnostic, not shipped cost, universal speedup, zero regression or a leak conclusion.

The isolated hybrid correctness gate passed 6,454 assertions in 38 cases. After exact production application, the fresh normal `dev` core suite passed 50,449 assertions in 363 cases, seed 3244740430. The current-source build, three-file format check, diff check and benchmark-job parse returned zero. The job parse did not execute the installed benchmark recipe. No benchmark stdout was persisted; only numeric/metadata captures were saved under build evidence.

The unconditional candidate and its rejection remain recorded in `.cache/build/bench/evidence/metrics-row-index-pair-v3-2026-10-01/independent-pair-audit/`. Accepted hybrid captures and the independent review are in `.cache/build/bench/evidence/metrics-row-index-hybrid-pair-v2-2026-10-01/`, including `independent-audit/review.md`. Production pins and fresh normal gates are in `.cache/build/dev/evidence/metrics-hybrid-production-2026-10-01/root-production-gates.json`.


## Native Array Unique production gate and diagnostic, 2026-10-01

The native `Unique` executor now retains the first occurrence of supported scalar values. Integer comparisons preserve exact integer identity across the supported numeric carriers. This is an engine policy, not licensed source parity. Comparisons requiring recursive array reference identity or opaque image, surface, or struct identity still return explicit unsupported diagnostics. A singleton payload requires no identity comparison and remains accepted with owned output content.

The five owned production paths and their hashes are recorded in `.cache/build/dev/evidence/array-unique-production-2026-10-01/root-production-gates.json`. The normal production build completed all five stages with exit zero. The full imagegraph test run passed 1,765,769 assertions in 705 cases, seed 2570181930. The four owned C++ files passed the recorded clang-format-21 dry-run gate, and the benchmark job parsed successfully. The full installed benchmark job was not run. These results preserve the earlier audit entries as historical evidence.

The separate optimized diagnostic rebuilt 34 translation units and verified 2,115 frozen input pins. Its 64 fixtures each completed eight warm and five measured calls: 832 phases, comprising 512 warm and 320 measured phases. The independent audit accounts for 793 value publication phases, 13 singleton ImageArray publication phases, and 26 explicit unsupported diagnostic phases. Image-only and diagnostic cases do not fabricate a `SetValue` count. All phases passed the complete canonical output and native input ownership checks, with heap hooks available. Saved hashes summarize the accepted complete comparisons; hash equality alone is not the acceptance oracle.

Owner spans and frame partitions were complete. Logical payload and operation counters remain distinct from allocation bytes, live bytes, peak bytes, and cumulative heap totals. Fixture setup, canonical validation, and output collection are outside the measured owner interval. Aggregate CPU busy load was 0.794% at baseline and 5.338% during the active diagnostic, a 4.545 percentage point difference. This is background-load context, not background subtraction or a timing improvement. This diagnostic establishes no speedup, leak, or shipped-cost claim.

Evidence is retained under `.cache/build/bench/evidence/array-unique-diagnostic-revision4-2026-10-01/`, including the build metadata, results, and independent audit. The reviewed benchmark SHA-256 is `dcd36fdeaec902c2be19ae818955c1733f10b8f6b188b538822e93d5465b5038`. This closes the earlier native Unique implementation and diagnostic gap. Source comparator and coercion parity, recursive and opaque native identity policies, licensed verification, full-family PXC persistence, and live Studio open workflows remain open.


## Native Array Uniform and Rearrange acceptance, 2026-10-01

The native Uniform and Rearrange executor production gate passed. The full `dev` build completed 289 stages with exit zero. The exact eight owned paths are recorded in `.cache/build/dev/evidence/array-rearrange-uniform-production-2026-10-01/owned-manifest.json`. The production test runs passed: imagegraph, 1,767,346 assertions in 741 cases; imagegraphio, 4,060 assertions in 88 cases; selected Studio imagegraph, 17,685 assertions in 59 cases. Post-image guard and diff checks returned zero. This records executor production acceptance. Full source-family UI and licensed parity remain unverified.

The independent optimized diagnostic v3 audit found no blocking artifact or numeric issue. It verified 34 build units, 2,379 inputs and 107 build artifacts. Its 102 fixtures completed eight warm and five measured calls each, for 1,326 phases. Independent canonical models checked all 28 new Rearrange outputs, including reverse and repeated schedules, packed tuple order, full Curve rows, recursive owned images, large integers, empty rows, numeric-zero padding, identity fallbacks and empty categories. The 74 existing array fixture constructors matched the accepted standalone Uniform diagnostic workload. The benchmark-local optional ImageArray comparator correction was verified in the successful v3 package.

The failed v1 optional ImageArray operator comparison and v2-MF preflight remain archived as historical failure evidence. The successful v3 package supplements that record. The optimized diagnostic is not a speedup comparison: there is no paired old Rearrange implementation baseline. CPU busy load was 1.962753% at baseline and 4.811387% during the active run, a 2.848633 percentage point difference. No adjustment was applied. Steam and Barotrauma were user-reported background applications; process activity was not verified. No wall-time correction, speedup, leak, shipped-cost, licensed parity or full-family completion claim follows.

Native Orders identity fallback policy is separate from authored Orders writeback, drag UI and source coercion, which remain open. The diagnostic's identity fallback fixtures do not close those items. Exact production records are in `.cache/build/dev/evidence/array-rearrange-uniform-production-2026-10-01/`. The optimized independent review and numeric evidence are in `.cache/build/bench/evidence/array-rearrange-uniform-diagnostic-v3-2026-10-01/independent-audit/`; the reviewed benchmark SHA-256 is `d7ab7b3210d003dfd90c39e341f7f3cc69da696138552238b5e77967002afe5e`.

## Physics cell-size counterbalance, 2026-10-02

The six owned paths applied were `mono.engine/physics/benchmarks/CellSizeCounterbalance.cpp`, `mono.engine/physics/tests/CellSizes.cpp`, `mono.engine/physics/tests/fixtures/CellSizeParity.hpp`, `mono.engine/physics/tests/fixtures/SteppingScene.hpp`, `Justfile`, and `RUNNING.md`. Their postimage hashes match the application audit. Source patch SHA-256: `405c5d16b13fd91f989e45e524e79245102d3c706cce7857407362886621bb48`. A later format-only application changed adjacent string-literal segmentation in `CellSizeCounterbalance.cpp` and joined the conditional in `SteppingScene.hpp`; it preserves literal bytes and nonliteral tokens, with no native rerun. The production default remains 4 m.

The native physics gate passed HashGrid with 33 cases and 1,129 assertions, and CellSizes with 3 cases and 3,224 assertions. Two fresh benchmark processes ran both orders over pile, stacked, scattered, and mixed-scale layouts. The 832 total pair rows include eight warm-up and five measured ticks per arm; 320 rows are measured. Thirty-two scene and floor-only summaries each cover five matched ticks. Frame and heap scope drops were zero.

Across the four repeat/order combinations, the 2 m cell size had lower pile owner medians by 4.791 to 6.473 ms. The 4 m cell size had lower mixed-scale medians by 0.227 to 0.308 ms. Stacked and scattered results changed sign with repeat or order, so they establish no robust winner. Floor-only controls are separate and were not subtracted from scene timings. Keep the 4 m default. These results establish no universal gain, warm-cache parity, shipping-cost improvement, or 200-client Authority `RecoverRows` result.

Raw benchmark stdout and detailed printed grid, memory, and span rows were consumed in RAM and are not retained. The owner-delta summaries and drop counts are retained. The independent Sol review passed its retained-evidence checks, but it reviewed parser-reported owner medians and did not independently recompute them from raw pair rows, which were not retained.

Root qualification: `/tmp/physics-stress-root-native-2026-10-02-u3ez3fem/source/.cache/build/evidence/physics-stress-root-gate-attempt3/root-qualification.json` (SHA-256 `8a2d9212f58ebbca525ec37a4e52ba568ae925a8b10f4d7e6b3a16d0bfcf32e6`). Independent Sol retained-evidence review: `/tmp/physics-stress-root-native-sol-review-a3485ea00935406e9d2c6d66dcff9a0d.json` (SHA-256 `d2669aaec4c0fe8c917a54cde232eeb3e56c4adab696dfe84e1b154c8262ada4`). Six-path application audit: `/tmp/physics-stress-owned-application-root-2026-10-02-uvsxv3r8/application-audit.json` (SHA-256 `dac6eb77e4a89cac1e0c3c6e2cdd77f9800b059544fa16d332cbd160f412c88d`). Two-file format application audit: `/tmp/physics-stress-format-application-root-2026-10-02-887t8kwj/application-audit.json` (SHA-256 `baa65815a753a22b51dec80a3f4b775b802f2ce1d5327537e22a8ccf1cd0dbe7`).
