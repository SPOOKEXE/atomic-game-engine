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
