# render refactor execution

Implementation of [RENDER-REFACTOR.md](RENDER-REFACTOR.md), all R01-R17 and P0-P12.
The implementation request authorizes the planned destination. A checked phase
requires its functional and optimization evidence, not merely code or a build.

Current continuation order is at the top of
[RENDER-REFACTOR.md](RENDER-REFACTOR.md#continue-here-portal-crossing-2026-09-07).
The [handoff](PORTAL-HANDOFF.md) records known failures, test evidence and retained
diagnostic paths. The full working-tree checkpoint is not a completed phase.

## phase tasks

- [ ] P0: audit current contracts, policies, hidden work and shader consumers;
  record baseline tests and release costs. In progress.
- [ ] P1: per-step image/projection harness, independent oracles, bounded
  captures, failure artifacts and `just render-check`.
- [ ] P2: graph-owned preparation, residency, uploads, simulation, capture and
  composition; shared worlds and parallel active-view collection.
- [ ] P3: shader runtime schema, cook/compiler split, reflection, variants,
  published bundles, backend payloads and complete cache keys.
- [ ] P4: material definitions/instances, capability flags, effect attachments,
  resident records, VM bindings, save and replication.
- [ ] P5: compositor/material authoring, incremental canvas, cook previews,
  groups, undo/redo and hidden-work gates.
- [ ] P6: lighting, PBR, colour, AO/GI, shadows, post, velocity/history and all
  requested AA choices with numeric and image proof.
- [ ] P7: editable packing/quantization, mip/atlas streaming, four authored LODs,
  decimation, meshlets, tessellation and visual displacement.
- [ ] P8: portal projection, clipping, light transport, physical overlap,
  cross-world ownership and inspected moving demo.
- [ ] P9: real hybrid/progressive tracing, acceleration, transport, accumulation,
  reset, denoise, portal rays and bounded GPU queues.
- [ ] P10: particle/environment nodes, TornadoSim and retained scene-producer
  requirements with deterministic authoritative simulation.
- [ ] P11: all requested examples, backend/tier verification, full-frame
  benchmarks, pressure tests and latency/quality comparisons.
- [ ] P12: delete replaced paths, enforce cooked packaging, finish build/test/
  fuzz/soak gates, reconcile documentation and audit R01-R17.

## workflow for the current phase

- [x] classify: engine-wide implementation, preserving the full plan.
- [ ] discover-core: inspect graph execution, device capture and shader loading.
- [ ] specify: map baseline and image contracts to concrete tests.
- [ ] localize: name production seams and test commands before editing.
- [ ] prototype: only unresolved device/format behavior needs a probe.
- [ ] stubs: review data shapes and function logic before filling bodies.
- [ ] implement: build in dependency order without placeholder success.
- [ ] validate: headless first, then device evidence at final verification.
- [ ] optimize: release measurements with quality and memory held explicit.
- [ ] review: reconcile code, tests, policies and residual gates.

## evidence and current boundaries

- Starting revision: `4daba0b6`; clean worktree before implementation.
- The source plan contains 23 sections, 17 requirements and 13 phases.
- `render/AGENTS.md` still requires live client shader compilation and exempts
  host chrome/window clear from graph ownership. P0 must reconcile these with
  the explicitly requested implementation and verified current code.
- Four offscreen Vulkan image fixtures now pass. The user explicitly requested
  portal visual checks, which are being added to that harness. Interactive Studio
  inspection remains a separate final verification step.
- No phase is complete yet. No renderer speedup is claimed.

## current implementation slices

- [x] P0: locate hidden frame work. `Renderer::Render` flushes meshes before
  graph execution; `ViewRecording::Begin` executes entity selection and packing;
  upload handlers report that completed work. Particle preparation, interface
  preparation, thumbnail capture and final capture/chrome remain migration work.
- [x] P0: add bounded signature benchmarks for 1,024/16,384 rows, 1/2/8/32
  cameras and unchanged/one-row edits. `bench_render` builds; all 16 cases run.
  These diagnostic runs had background work, heap hooks and Tracy enabled.
- [x] P1: independent CPU comparison of byte colour, float and integer-ID images
  with stride validation, per-pixel outliers, RMSE, non-finite rejection and
  mismatch bounds. `[imagecomparison]` passes 112 assertions in 8 cases.
- [x] P1 slice: real-device plane fixtures and failure artifacts. Four Vulkan
  cases pass, including lens/clipping, edited objects and multiple worlds/views.
  A final graph observer retains captured intermediate resources; this preserves
  ordinary aliasing elsewhere. Orthographic/oblique, skinning, PBR, temporal,
  portal and broader lifetime families remain required in their feature phases.
- [x] P2 slice: conservative `View` damage includes object inputs, rather than
  reusing stale resident rows after same-count edits. The GPU edit case reproduced
  this defect and passes after the fix; hosts can still supply precise damage.
- [x] P2 slice: frame setup runs once per resolved pipeline, with shared resources
  available to each world/view. Duplicate prefix reproduced in headless tests.
  Planner, mixed-writer, fallback aliases and idle-view cases pass headlessly.
  This closes scope bookkeeping only; actual hidden-work migration remains.
- [x] P3 slice: retain accepted shader words on failed edits, invalidate removed
  consumers, and key attempted sources by storage incarnation/entity/revision.
  Replacement, duplicate selection, cross-world contamination and snapshot tests
  pass. Snapshot application conservatively invalidates source caches, so unchanged
  shaders may recompile until the content-key cook cache exists.
- [x] P3 slice: bounded canonical cooked shader transport, reflected interface
  identity, corruption tests and sanitizer-backed parser fuzzing. Cooking,
  executable admission and delivery consumers remain required; this is a format
  unit, not a published shader pipeline.
- [x] P1 slice: resolve named resource readbacks through graph alias storage and
  report their actual dimensions. Portal captures reproduced missing textures;
  all eight camera/ambient combinations now reach the pixel comparisons.
- [x] P8 slice: preserve dim-light portal radiance. The initial two-sided image
  oracle matches aperture coverage but fails its colour bound at ambient 0.25.
  The UNORM8 radiance intermediate produces a three-code error after display
  conversion. HDR capture and matching attachment pipelines now pass 13 samples,
  including custom materials, spatial UI and transparent geometry. The additional
  logical target cost is four bytes per portal pixel; no speedup is claimed.
- [x] P8 slice: real Humanoid camera subjects, per-camera selection and one root
  resolver across follow, obstruction and transit. Walking through turned portals
  and back passes headlessly. Authored documents, clones and product follow paths
  preserve automatic/explicit mode and remap Humanoid root references.
- [x] P1/P8 slice: explicit fitted view projection, consistent culling and general
  depth reconstruction. Off-axis, orthographic and oblique fixtures pass after
  translation/rotation too. A projection-only culling matrix reproduced missing
  portal interiors; supplying full view-projection fixes the 13 portal samples.
- [x] P8 slice: bounded named presentation bus outside simulation mailboxes,
  with authenticated endpoint incarnations, owned payloads, backpressure and
  real process-channel transfer. Parser fuzzing passes. Actual portal image
  producers/consumers still need to use this route.
- [x] P8 slice: bounded canonical image request/reply codec, including HDR hash,
  pose/lens/clip checks, truncation and malformed-length tests. No image exchange
  is claimed until the renderer adapters run through the bus.
- [x] P8 slice: graph-owned nonblocking HDR export, bounded source inbox and
  renderer import. Export uses the existing capture node and scene fence, with
  no extra submission. A failed graph cannot publish a successful image, and a
  cancelled download remains owned until its fence completes. Import uses one
  bounded staging buffer and reuses unchanged content without another upload.
- [ ] P8 slice: connect actual image producers and consumers through the
  presentation bus. Two independent Vulkan renderers now exchange destination
  images through the bus without changing the destination's active camera.
  Complete-world fixtures include HDR transparency, effects, nested surfaces
  and spatial interface. Shared-renderer requests can instead exchange an owned
  resident-image receipt without CPU pixel transfers. Product host wiring and
  full child lighting/environment parity remain required.
- [x] P8 host adapter slice: `PortalImageHost` owns bounded viewport sources,
  shared destination producers and resident receipts. Caller-provided routes
  resolve authored world names to the selected player replica. View removal
  closes its endpoint and purges queued work; world removal retires dependent
  images. Two viewport images have distinct ownership and retire independently.
  Client/Studio call sites, complete straddler image sequences
  and process-host adapter verification remain required before replacing
  `AttachForeignSurfaces`.
- [x] P8 authored demand collection: gather linked cross-world seams from ECS
  with the actual viewport lens and explicit surface slots. Full camera paths
  identify requests; ambiguous duplicate paths are refused. Cross-world claims
  exclude the legacy reflection even for unsupported filters. No active-camera
  resource, camera pose or authored surface index is changed. Demand tests pass
  1,117 assertions / 6 cases. The old attachment also carries source straddlers
  into destination images, so replacing product call sites still requires those
  owned geometry inputs in the image producer; enabling only background images
  would remove the far half of a crossing character.
- [x] P8 crossing-image wire data: `PortalGeometry` carries drawables,
  nine asset bindings, mapped poses, material controls, clip/light vectors,
  shadow/alpha/sampling policy and owned skin palettes. No source ECS identity
  or interned-name number is serialized. The codec bounds 256 rows, 4,096 joints
  and 1 MiB, and validates palette ranges before committing decoded output.
  Diagnostic draw names may be empty or repeated: these rows are a complete
  picture, not transferred ECS identities. Initial codec tests passed 774
  assertions / 3 cases; the integrated gate below supersedes that count.
- [x] P8 crossing-image runtime slice: source draw conversion copies referenced
  skin ranges and material/clip fields without source entity IDs, tag bits or
  surface slots. Version 3 image requests carry bounded geometry; body-only
  changes supersede pending requests with a still camera. The producer appends
  decoded rows/palettes to each requested destination view. Copied and resident
  GPU fixtures hide the destination wall and reproduce its expected pixels only
  from the transported body. Combined codec/conversion/runtime/host/table tests
  pass 3,265 assertions / 24 cases. An unnamed-draw refusal and order-dependent
  effects-registration abort were reproduced and corrected. Actual seam clone
  collection is connected below; moving skinned crossing images, product call sites and process
  image sequences remain open.
- [x] P8 authored crossing-image collection: `CollectPortalImageDemands` now
  encodes actual far-half clones using the gathered seam and viewport slots.
  The shared scene clone implementation retains body-fit and complementary
  clipping rules without gathering again or changing the active camera.
  Referenced skin palettes travel with the request. Invalid geometry refuses the
  demand instead of silently omitting a body half. Demand/geometry tests pass
  1,932 assertions / 11 cases, including mapped placement, material preservation,
  skin data, source-row preservation and malformed-palette refusal. Shared scene
  surface tests pass 4,953 assertions / 76 cases. This is headless collection
  evidence; the complete moving product image gate is open.
- [x] P8 authored crossing-image pixel slice: create an authored source portal
  and a body spanning its plane, then feed the collected request and mapped
  sampling into the real producer and compositor. The destination wall is hidden;
  the transported far half supplies the expected red centre pixel under
  destination lighting. Vulkan copied and resident delivery pass independently,
  including zero resident upload checks. Combined runtime/demand tests pass
  2,500 assertions / 15 cases. This
  centre-pixel gate does not prove moving Humanoid silhouettes, reciprocal
  destination mouths or complete product wiring.
- [x] P8 reciprocal-mouth pixel slice: the authored destination now has a real
  return portal aimed at the source world. Six centre/off-centre eyes on both
  faces retain the transported body's expected centre pixel in copied and
  resident delivery. These cases pass without changing the renderer: the tested
  aligned pair is already excluded correctly by destination clipping. The full
  runtime/demand subset passes 3,262 assertions / 15 cases. This does not close
  moving endpoints, aperture-edge comparisons or nested cross-world recursion.
- [x] P8 Client call-site slice: Client owns and tears down `PortalImageHost`,
  configures its local presentation bus, collects authored demands from its own
  composited rows, resolves destinations against player replicas and samples
  completed images. Hidden views release their image endpoint. The legacy
  attachment retains destination-side foreground clones while skipping its
  background and outgoing-body copies; Studio integration follows below.
  Request/capture/reply and source-copy byte counters accompany the new scope.
  Both-direction foreground tests pass; full Client tests pass 9,222 assertions
  / 163 cases. A two-world headless run exits and an inspected offscreen capture
  contains geometry, but that distant composite view is not portal image parity
  or player traversal proof. Product screenshot sequences, Studio integration,
  process-host delivery, replicated interpolation and removal/recreation checks
  remain required. No cache-efficiency or release-speed claim is made.
- [x] P8 integration audit repairs: remove stale implicit-camera-marker
  expectations from the existing presentation test, classify the transfer ledger
  as local world state, and allow its empty unconfigured snapshot to round-trip.
  Active state still requires a nonzero incarnation. The full Client audit above
  and 404 script transfer assertions / 13 cases pass after the reproduced failures.
- [x] P8 replica presentation preservation: the producer no longer recollects
  an adopt-only destination from its latest ECS transforms after presentation.
  A GPU fixture publishes the visible interpolated row while the latest transform
  is offscreen; both portal and direct probes reproduced a 231-level red-channel
  error before the fix. Copied and resident captures now preserve the published
  row and leave the latest ECS transform untouched. Runtime/demand/host tests
  pass 3,420 assertions / 18 cases, and Client relinks. This fixture isolates
  the producer boundary; a complete received-snapshot player sequence remains
  required for product interpolation and camera continuity evidence.
- [x] P8 shared Studio image path: Client and Studio now use one product adapter
  for authored demand, replica selection, image submission and surface claims.
  Studio keeps image ownership per viewport and releases it on viewport closure,
  preview-slot changes, world removal/rename, Play replica teardown, game load
  and shutdown. Foreground crossing clones remain attached while legacy
  background collection is removed from both products. `AppendForeignPortalClones`
  gathers authored seams independently of surface slots and appends only crossing
  bodies from the viewer's destination replica. The obsolete background adapter
  and its product-owned vectors are removed. Focused foreground tests pass
  32 assertions / 2 cases, including unassigned surface slots and replica routing.
  Scenes without enabled cross-world portals skip row copies and destination
  surveys. No measured speedup is claimed.
- [x] P8 completed-presentation reuse: the producer can consume draw rows from
  the host's completed destination presentation without running PreRender again.
  This preserves each destination's interpolation alpha and avoids advancing a
  replica snapshot buffer twice. A GPU test verifies retained draw rows and host
  frame timing; the runtime/demand/host subset passes 3,488 assertions / 18 cases.
  Full Studio passes 6,670 / 519 and Client passes 9,222 / 163 after integration.
  An isolated two-world Studio server run with two viewports exits cleanly and
  produces an inspected scene capture. Its distant camera does not establish
  seam quality. Moving player captures, actual process delivery, static capture
  reuse and interactive Studio inspection remain open.
- [x] Studio scripting timeline clamp: a zero-duration span at the right edge
  reproduced invalid `std::clamp` bounds with standard-library assertions enabled.
  Bar layout reserves its minimum visible width before clamping the start, including
  subpixel and zero-width panels. The focused suite passes 318 assertions / 16
  cases; full Studio passes 6,912 / 520. The isolated arithmetic also passes with
  UndefinedBehaviorSanitizer and standard-library assertions. The Windows screenshot
  has no call stack, so matching this defect to that exact crash remains unverified.
  A second invalid clamp occurs when a caret move returns an empty completion
  list: its old bounds were 0 and -1. This also reproduces with standard-library
  assertions. Empty completion results now reset the selection; nonempty results
  retain valid choices. Combined completion/diagnostics tests pass 569 assertions
  / 34 cases, full Studio passes 6,918 / 521, and Studio and Client build.
  After removal of the old portal background adapter, full Client passes 9,177
  assertions / 159 cases. Removed cases exercised the deleted adapter; image
  runtime and foreground tests retain the replacement feature coverage.
- [x] P8 capture-revision prerequisites for reuse: the producer includes the
  renderer's current flipbook-cell signature alongside spatial-interface state.
  Inactive or stationary cloud clocks are excluded from the copied lighting
  value, so clock advances cannot invalidate an otherwise unchanged capture.
  A Vulkan fixture checks identical pixel hashes and revisions within one cell,
  then changed pixels and revision after the next cell. The stale-cell identity
  risk and unnecessary clock invalidation are corrected; runtime/demand/host
  tests pass 4,033 assertions / 20 cases. Source-owned image renewal is covered
  in the separate slice below.
  No speedup is claimed.
- [x] P8 resource replacement revisions: the portal producer now folds the existing
  renderer resource epoch into its content revision. Same-name texture replacement
  reproduced changed pixels with an unchanged revision. Replacement, removal,
  restoration and refused removal are now checked with owned GPU captures.
  Lens and post-process shader mutations also advance the existing epoch; their
  visual replacement cases remain part of the broader shader gate. Vulkan
  runtime/demand/host tests pass 4,085 assertions / 20 cases. The epoch is global
  to one renderer, so unrelated resource changes conservatively invalidate it;
  per-resource dependency narrowing remains open.
- [x] P8 source-owned image renewal: requests offer the content and lighting
  versions of a still-owned matching image. A distinct bounded receipt extends
  its lifetime without transferring GPU ownership or capturing unchanged pixels.
  Sources reject a receipt whose version differs from the offer. Custom shaders
  that receive presentation time conservatively invalidate on clock changes.
  Vulkan tests cover copied and resident renewal, unchanged handles and upload
  counts, expiry followed by fresh capture, and destination motion producing
  changed pixels with an unchanged camera. Runtime, exchange, host, demand,
  inbox and resident tests pass 5,739 assertions / 35 cases. Actual process
  delivery, narrower CPU invalidation and release cost measurements remain open. This avoids capture
  work, but still presents and hashes the destination for each request.
- [x] P8 renewal pressure and late-reply recovery: copied and resident tests fill
  the reply endpoint, then verify retry sends the retained receipt without another
  capture. Delaying a renewal until its owned image expired reproduced a fresh
  request returning Busy. Image expiry now cancels that image's renewal offer,
  so the source can immediately request new pixels. The late receipt cannot
  revive the expired handle. Runtime, exchange, host, demand, inbox and resident
  tests pass 5,749 assertions / 35 cases on Vulkan. Process transport and release
  performance measurements remain separate gates.
- [x] P8 codec cost benchmark: `just portal-exchange-bench` measures owned reply
  encode/decode batches for 1, 2 and 8 views at 512x512 linear HDR, alongside
  resident receipts and renewals. Each copied image has a 2 MiB pixel payload.
  Decoding validates its hash and size; receipts must round-trip exactly.
  The release-derived `bench` preset built and all nine rows ran with five
  samples on a Ryzen 9 9900X, with heap profiling enabled. Measurements remain
  on the terminal. No GPU backend, world presentation, bus delivery or process
  transport is included, so full-frame and residency benchmarks remain open.
  The unity build also reproduced private constant/helper collisions in the
  presentation and portal geometry codecs; specific helper names fix them
  without changing either wire format. Dev exchange/geometry tests pass 2,109
  assertions / 12 cases and presentation-bus tests pass 944 / 8 after the repair.
- [x] P8 standalone process image producer: `PortalImageHost::Serve` opens a
  bounded local producer without requiring a local viewport. Its returned
  endpoint can be distributed by authenticated host control traffic. A real
  child process owns a Vulkan renderer, captures its world's HDR image, and
  sends copied pixels through `HostLink` and authenticated presentation buses.
  A second request returns a renewal and preserves the parent's owned image;
  the child verifies exactly one capture and one renewal. Producer removal and
  reopening change endpoint generation. Runtime/host/demand tests pass 4,289
  assertions / 22 cases plus 17 child-process assertions. Client and Studio build.
  This initial fixture pre-registers endpoints; product discovery and continuous
  player traversal across process hosts remain open. Moving-scene source
  compositing is checked in the following slice.
- [x] P8 process image source-compositing slice: a child-owned red wall occludes
  a green wall. The unchanged camera first receives red pixels, then renews
  without upload, then receives green pixels after the foreground wall moves.
  The parent composites each received image onto a portal under different source
  lighting. A 3x3 aperture patch matches the expected destination radiance after
  display conversion within 2/255. Upload counts are one, one and two, while the
  child verifies two captures and one renewal. Runtime/host/demand tests pass
  4,428 assertions / 22 cases plus 24 child-process assertions on Vulkan.
  This is a fixed-camera planar fixture with pre-registered endpoints; full
  aperture parity, product discovery and moving player sequences remain open.
- [x] P8 product endpoint lookup: portal routes now carry authored and resolved
  world identity only. The shared image host looks up remote request endpoints
  in the universe's authenticated presentation registry; Client and Studio no
  longer supply an empty endpoint that makes every remote route unavailable.
  Lookup uses the existing bounded registry without a duplicate cache. Tests
  cover absent endpoints, wrong names, close/reopen, stale close, world removal
  and session reset. The real-process fixture resolves its registered endpoint
  through the same lookup. Runtime/host/demand tests pass 4,442 assertions / 23
  cases plus 24 child assertions; presentation tests pass 960 / 9. Client and
  Studio build. Automatic authenticated endpoint advertisement/withdrawal is
  still required: process fixtures currently register endpoints during setup.
- [x] P8 authenticated directory receive path: bounded full endpoint sets carry
  a host session and revision over a dedicated `HostLink` control frame. The
  supervisor attributes them to the actual connection; the driver checks world
  ownership before changing the existing presentation registry. Newer sets
  retire missing or replaced endpoints and their queued messages. Retained
  endpoints keep their message sequence history. Empty sets withdraw all of a
  host's endpoints; retained version records reject old advertisements after
  withdrawal or restart. The full world suite passes 28,467 assertions / 213
  cases, including malformed/truncated frames, duplicate identities, ownership,
  withdrawal and stale replay through the real driver pump.
  Client and Studio build; portal-host tests pass 236 assertions / 6 cases plus
  24 child-process assertions. Renderer-free server hosts explicitly refuse the
  directory frame alongside image traffic.
- [ ] P8 directory product lifecycle: emit changed local directories from render
  hosts, forward accepted endpoints to consumers over authenticated connections,
  retry bounded control traffic, and withdraw endpoints on disconnect/restart.
  The process fixture now exchanges advertisements as recorded below; product
  host pump wiring and abrupt disconnect cleanup remain required. Combine discovery with the continuous Humanoid
  crossing/image/ownership sequence described in the portal acceptance gates.
- [x] P8 directory publication and process discovery: the host link publishes
  changed directory versions and skips unchanged ones. A full channel does not
  advance the published version, allowing a later pump to retry. The process
  image fixture no longer constructs or registers remote endpoint receipts:
  both sides advertise their local directories over the authenticated link.
  Captured and renewed images still meet the source-compositing checks. Before
  shutdown the child withdraws its endpoints, the parent removes the displayed
  image on its next submission, and an acknowledgement lets the child exit.
  Full world tests pass 28,483 assertions / 214 cases; portal-host tests pass
  248 / 6 plus 46 child-process assertions. Client and Studio build. This proves
  the link publisher and process exchange, not automatic product host forwarding
  or abrupt disconnect retirement.
- [x] P8 supervised disconnect retirement: a reproduced driver test kept a dead
  host endpoint discoverable after link close. Presentation pumping now retires
  unlinked hosts. Explicit link replacement, supervised respawn and shutdown also
  retire the old connection and discard its buffered advertisements/images.
  Registry retirement purges queued image traffic while retaining the old host
  session as rejected history; even a higher revision in that session cannot
  restore it. A new host session can advertise again. Tests cover observed close,
  replacement before the next pump, stale directory/direct registration, queue
  retirement and successful restart. Full world tests pass 28,528 assertions /
  216 cases; Client and Studio build. Consumer-host forwarding and the continuous
  player crossing remain open, including retirement at non-supervised peer links.
- [x] P8 driver-to-consumer route forwarding: hosts that advertise presentation
  endpoints receive a separate driver route snapshot containing the other hosts'
  admitted endpoints. Producer sessions/generations remain unchanged; the driver
  versions the aggregate set, so disconnect withdrawals supersede old routes.
  Consumers apply route frames only through their trusted-driver boundary and
  cannot replace a local endpoint. Host-originated route frames are refused.
  Unchanged sets emit no frame, failed control sends remain retryable, and the
  driver retains its bounded outgoing image queue until route control is queued.
  A two-host test discovers both directions, forwards image bytes, disconnects
  the producer and observes withdrawal at the consumer. Full world tests pass
  28,764 assertions / 218 cases; portal-host tests pass 248 / 6 plus 46 child
  assertions. Client and Studio build. Binding the render-owning product host
  loop to this forwarding path and proving continuous player crossing remain
  open; the existing GPU child fixture still uses its direct authenticated link.
- [x] P8 client producer host loop: the actual client executable can select one
  game world, retain other worlds as remote routes, advertise its producer and
  consume driver route snapshots over an inherited process channel. The shared
  render adapter pumps control outside ticks, retains replies under link
  backpressure, and clears image ownership on stop or disconnect. The real
  client process GPU test passes 49 assertions for discovery, a 16 x 16 HDR
  reply, clean stop and abrupt driver disconnect. Client and Studio build.
  Full client tests pass 9,177 assertions / 159 cases;
  portal-host tests pass 248 / 6 on Vulkan. This test checks transport and
  lifecycle, not image content or continuous player crossing. Automatic product
  launch policy, moving cross-world images and combined player handoff remain
  open.
- [ ] P8 slice: transfer the existing rig through an idempotent reserve/commit
  protocol, with bounded replayable state, exact receipt identity and no duplicate
  authority. Retain camera orientation, zoom and Humanoid selection across the
  product session change. Destination admission and session integration are active.
- [x] P8 fresh admission gate: transport admission creates no avatar. The Client
  and load-test session explicitly request a fresh player, and repeated requests
  retain the same player and character. Admission replies survive send
  backpressure with one retained reply per connection; a newer attempt replaces
  the pending reply, and disconnect or shutdown clears it. Resume requires a
  destination lease and the exact committed transfer receipt.
- [x] P8 slice: preserve full camera orientation and scaled lens/controller
  lengths through tilted portals, including skipped presentations and repeated
  crossings. First-person thresholds scale with the camera; input directions
  follow its mapped basis. Physical gravity still uses the destination's world Y.
- [x] P8 replay gate: preserve future entity allocation across snapshot restore.
  The transfer replay test exposed lost free-slot order and receiver page epochs.
  The focused ECS reproduction failed 80 assertions. Snapshot version 7 now saves
  allocator order and page epochs; strict transfer replay passes. Adopted holes,
  released large pages and malformed allocator metadata are covered too.
- [x] P8 session readiness: retain the old replica through a chunked destination
  join. Switch only when every destination object and the mapped Humanoid camera
  are ready. A disappearing destination cancels its pending successor replica.
  This proves session state; the moving visual transition remains required.
- [x] P8 angle oracle: sweep 45 camera positions through both faces, 36 azimuth/
  elevation combinations, roll, near-plane approach and looking away. All 95
  lighting/appearance sections pass their unchanged colour bound. Dynamic objects,
  mixed mirror/portal recursion and imported views remain separate
  acceptance checks.
- [x] P8 moving-light oracle: hold the viewer fixed while changing the destination
  light position three times. Compare each portal image against an independently
  unfolded room and require its content hash to change. All 155 assertions pass,
  with a maximum difference of 1/255 within the unchanged 2/255 bound.
- [x] P8 shipped immersive demo transfer slice: remove the proximity respawn and correct the
  stand-in's half-turn and 0.6-metre plane mismatch. Authored geometry and standing
  near either face now pass. The actual two-world round trip exposed immediate
  reentry from an admitted previous transform. Admission now retires that segment;
  both approaches and return trips preserve pose, momentum, health and limb
  placement with at most one authoritative body. All 150 assertions pass.
- [x] P8 mirror HDR/cache slice: preserve half-float radiance, refresh lighting
  and retained spatial UI, and enforce shared recursive pixel/depth budgets.
  A same-name texture replacement reproduced four stale-capture failures; a
  renderer-owned resource epoch now invalidates retained mirrors after successful
  resource edits. All 19,410 assertions pass, including unchanged capture reuse.
- [x] P8 authored non-Euclidean opening slice: reproduce twelve missing return
  links and ten blocked entrances. Give each entrance a distinct reciprocal
  pane, cut openings through the authored walls and align traversable floors.
  All 365 geometry/clearance assertions pass. Real fixed-tick humanoid movement
  then passes through all 24 openings, with 75 assertions. A separate continuous
  walk reproduced the pillar's missing connection. Its rear doorways now connect
  both hidden rooms, giving 26 surfaces. Two actual humanoid laps, every opening
  and all return mappings pass 512 assertions in four cases. A seekable camera
  tour covers 1,170 shots across those mouths, with pause and single-step controls.
  Both faces, roll, grazing, near-plane and look-back poses pass fixed-tick checks.
  Full examples now pass 100,862 assertions in 61 cases. Inspected demo images and
  interactive crossing sequences remain separate acceptance work.
- [x] P8 object/animated-rig transfer slice: the transaction carries ordinary
  bodies, tools, animation playheads and bounded clip/buffer dependencies.
  Shared source definitions survive. Refusal restores frozen animation bytes;
  scaled skinning passes vertex parity. Script tests pass 665 assertions in 74
  cases, with 69 actual process-child assertions. Full scene tests pass 514,473
  assertions in 525 cases after the roll/cut expectation update.
- [x] P8 requested-view collection slice: stable surface slots work without an
  authored active camera; request-local beams face the requested eye. Billboard
  size derives from the requested projection. Focused billboard pixels pass 66
  assertions after reproducing the incorrect focal scale. The shared child-first
  executor now passes the mixed mirror/portal runtime image gate, including
  request-facing beams, owned bus images and direct-tone parity. Full child PBR
  parity and per-child transparent ordering remain separate work.

## portal acceptance tasks requested by the user

The Studio chunked-arrival test now returns the same player through the inverse
tilted, 0.7-scale seam. First-person and third-person cameras retain Humanoid
ownership, heading, roll and original zoom after two replica handoffs and across
16 camera samples with 15 further simulation ticks. Exact transfer identity and
one returned player are checked. The expanded case passes 1,058 assertions.
It begins transfers explicitly and does not render images, so continuous walking,
far-side contacts and visual continuity still need the combined product gate.

The separate `player input walks across world replicas and back` case now drives
W/S through the real replica input, physics and swept portal admission path.
It adopts each exact successor and continues walking for 24 frames after each
handoff. First-person and third-person variants check Humanoid subjects, heading,
one destination player and camera samples throughout both legs. Two 5.27-metre
third-person jumps were reproduced: trigger colliders obstructed camera rays,
and received replica panes retained solid collision state. Camera rays now skip
triggers at candidate selection, preserving solid blockers behind them; received
portal links reopen their panes before the replica collision index is refreshed.
The walking case passes 1,508 assertions with its unchanged one-metre per-frame
jump bound. This catches the reproduced snaps, not sub-frame visual discontinuity.
The case still has no GPU images or foreign collision obstacle, so the combined
image/contact gate remains open.
Full validation after these changes passes physics 59,674 assertions / 254 cases,
client 9,177 / 159 and Studio 8,772 / 522. Client and Studio build in `dev`;
no release performance or GPU image claim follows from these headless checks.

The walking case now includes a static wall present only in the destination.
Ninety frames of held input stop the leading body before its root crosses;
removing the wall allows the same player to cross and return. Both camera modes
pass 1,526 assertions. Portal setup opens a bounded static-contact tick exchange;
the source collects after input, receives copied destination geometry and applies
it inside physics steps. Replica worlds participate in ticks without originating
contact requests. Missing replies hold approaching bodies instead of treating the
unknown destination as empty. The original long-body/Humanoid overlap test now
passes, including destination removal.

This integration reproduced floor sticking and repeated wall penetration at an
exact touching pose. The copied-contact solver now slides through a bounded
sequence of contacts and leaves 1 mm clearance for the next sweep. Aligned boxes
clip directly instead of rebuilding large point clouds; general hull clipping
refuses capped results that could omit collision geometry. Collection skips body
scans in worlds without foreign seams and resolves character speed once per
collection rather than scanning all characters for every body.

These are local world-host static-contact results. Product phase routing is
implemented and verified below; physical contacts in the combined process portal
walk still need acceptance. Dynamic destination bodies, general compound
assemblies and the combined GPU image/arrival sequence remain required.
Unsupported or over-budget copied geometry
holds the body; it is not reported as a clear path. No release-cost measurement
has been made for this bridge.
Validation passes script 737 assertions / 76 cases, world 28,764 / 218,
physics 59,674 / 254, client 9,177 / 159 and Studio 8,790 / 522.
Client and Studio build in `dev`.

The combined hidden Studio case `[portal-walk-images]` now captures the same
walking/contact/replica sequence on the GPU. The first run failed 28 image
assertions. Its destination stand-in is now invisible, matching the shipped
demo, and every world is presented before collection, including a replica first
adopted on that frame. The unchanged green-room probe still fails 27 assertions:
3,949 pass out of 3,976. Headless walking still passes all 1,526 assertions.
These failures prevent a seamless-crossing claim.

Frame diagnostics record the presented world, eye Z, image availability and
transfer stage. First-person frames 91-93 show the red source room at eye
Z = -5.9162, beyond the Z = -5.8 seam, during preparation/commit. Third-person
frames keep failing while the accepted destination body has an eye still on the
source side. Grey incoming captures also require tracing the receiving pane and
capture freshness; image availability alone does not prove correct content.
Logs are under `.cache/build/dev/tests/portal-walk-images-fresh.log` and
`portal-walk-headless-current.log`; per-frame BMP files are under
`portal-walk-gpu/first` and `portal-walk-gpu/third` in that same directory.

The receiving-mouth slice now carries the mapped entrance rectangle and authored
source-world name in image protocol version 5. The producer excludes only a
unique reciprocal mouth, leaving other surfaces present. Matching accounts for
the scene wire codec's position and rotation error bounds; the replica rectangle
was displaced by about 0.26 mm from the authored plane. Ambiguous nearby mouths
remain visible. Entrance changes invalidate retained replies, and per-request
filtered draw bytes are counted. This removes four failures from the unchanged
walking probe: 3,953 assertions pass and 23 still fail. The remaining frames
require eye-world/session continuity and capture-freshness work.

Full renderer validation also reproduced a separate submillimetre clipping
failure. `ObliqueProjection` discarded every plane within 0.1 mm of the eye,
including valid scaled exits. Only exact zero separation now skips the skew.
The existing 0.2 mm source/0.5-scale case passes its unchanged excluded-point
check. Full scene passes 514,758 assertions / 534 cases and renderer passes
31,998 / 426. Client passes 9,177 / 159, Studio 8,790 / 522, and GPU runtime/host
checks pass 3,315 / 16. Client and Studio build in `dev`; no release cost claim
follows from these checks. Latest logs have the `portal-entrance-` prefix under
`.cache/build/dev/tests`.

Destination routing now waits for a replica's first complete received tick.
The focused route test reproduced an early switch to an empty joining replica.
`SnapshotBuffer::RecordTick` records complete empty ticks too, without inventing
pose rows, and `SurveyWorlds` reads that existing clock rather than maintaining a
second join flag. The live authority remains the source until readiness; a
reset buffer is unready again. This removes the admission-frame capture failure,
leaving 22 image failures in the original replica-row path.

The walking GPU gate also now exercises authority rows with the replica eye,
matching Studio's visual-world choice. That variant must install its viewer
before presenting, as Studio does, so its portal slots are actually available.
Both paths now have the same remaining failures: three first-person frames after
the eye crosses during preparation/commit, and nineteen third-person frames after
body handoff but before the trailing eye crosses. The expanded gate passes 8,248
assertions and fails 44, with unchanged green-room probes. It exercises the
shared image adapter and both visual-world policies, not the complete Editor UI.
Full Client passes 9,180 assertions / 160 cases, replication 20,571 / 260 and
Studio 8,790 / 522; headless walking still passes 1,526 assertions. Logs have
`portal-arrival-route-` and `portal-visual-world-` prefixes under the build's test
directory. Authority-row BMP sequences use `authority-first` and
`authority-third` subdirectories alongside the original captures.

The camera-state foundation now stores an owned eye-world name and a camera
coordinate map, carried through `CameraContinuation`. Body admission rebases
that map without changing the eye world. Camera cuts reset the history, and
replication excludes it. The scene suite covers eye-first and body-first
crossings through rotated, translated and scaled seams, including an exact-plane
sample and aperture misses. Full scene passes 514,809 assertions / 535 cases;
replication passes 20,571 / 260. Logs are `camera-eye-scene.log` and
`camera-eye-replication.log` under the build's test directory.
Studio and the shared walking fixture now consume that state through
`client::ResolveCameraPortalWorld`. The selector keeps input ownership in the
replica, routes the eye by authored name, and scales its clipping distances.
It rejects unavailable local views and ambiguous receiving mouths. The residual
segment skips a unique receiving mouth within the wire position rounding bound
to avoid an immediate reciprocal crossing. General rotated wire-error bounds and
multi-hop/adjacent-mouth tests still need coverage.

The combined local GPU walk now passes 9,057 assertions, covering forward and
backward first-person and third-person walking with both visual-world policies.
The two previous reverse camera jumps came from the fixture creating a predicted
replica camera inside an authority world. Its ID collided with the actual client
camera when that camera lost its subject during handoff. The fixture now follows
`Editor::EnsureViewerCamera`, using a transient authoritative editor entity.
The unchanged one-metre continuity bound passes throughout both walking legs.

Forward image probes remain at their original pixel. Backward probes project a
known point on the destination panel, bounded inside the viewport when close;
the marker is wider so the retreating avatar leaves visible panel area beside
it. The old fixed pixel eventually left the shrinking panel, and the first
projected point became occluded by the avatar, both confirmed in captures.
These are fixture changes, not relaxed colour or continuity thresholds.
Full Studio passes 8,774 assertions / 522 cases. Client previously passed
9,185 / 161, including trailing-eye routing after admission. Logs are
`camera-return-marker.log`, `camera-return-studio-final.log` and
`camera-world-client.log` under the build test directory. This remains an
adapter-level local GPU gate, not complete Editor UI or process-host coverage.
Process full-eye image requests and standalone Client adoption remain unwired.

The image producer now supports whole-eye capture requests in protocol version 6.
`PortalImageProjection::Eye` retains the normal frustum with a canonical zero
clip plane and no hidden receiving mouth. Seam capture remains the default;
unknown modes, mixed eye/entrance requests and nonzero eye clip planes are
refused. The projection mode participates in camera revision matching so a seam
image cannot renew as a whole-eye image. Full render tests pass 32,008 assertions
/ 427 cases, and GPU runtime tests pass 3,088 / 2, including copied and resident
eye captures with radiance pixel checks. Logs are `portal-eye-protocol-headless.log`
and `portal-eye-protocol-gpu.log` under the build test directory. Those GPU checks
verify producer images through the existing image consumer, not a remote player
walk or full-viewport eye compositor; product request routing and composition
still need wiring.

Client and Studio rebuild with version 6. The local forward/backward GPU walk
still passes all 9,057 assertions (`portal-eye-local-walk.log`). The actual Client
product host test now covers both projection modes over inherited process
traffic, with explicit stop and driver disconnect; all 98 assertions pass
(`portal-eye-client-host.log`). This proves endpoint discovery, bounded returned
pixels and child lifetime for an empty authored world. It does not prove remote
player handoff, scene-content parity or full-viewport composition.

`BuildPortalEyeDemand` now converts the already-mapped camera into that request.
It preserves asymmetric perspective frusta, camera pose and clipping distances;
dimensions scale within the configured extent, and image work must fit the pixel
budget. Reconstructing the received projection rejects unsupported skew or
oblique depth instead of silently changing the rays. Camera revisions include
pose, lens and work limits, and refused inputs leave the output untouched.
The actual Client process test uses this builder for its eye-view requests.
Full render tests pass 32,037 assertions / 428 cases; the real process test passes
100 assertions. Logs are `portal-eye-demand-render.log` and
`portal-eye-demand-host.log`. This completes request construction, while remote
eye routing, viewport composition and player-session adoption remain open.

The renderer now has an `eye-image` graph source for full-viewport HDR display.
`View::EyeImage` and `EyeImageKey` select an imported whole-eye image; world ID,
world name, viewport, key, handle and projection/scope must match its owner.
Unavailable or mismatched images clear the target. The node blits the owned HDR
texture into its declared target, then the normal tone-map node reads that graph
input. This also removes the tone mapper's previous dependence on mutable
bindings left by lighting/lens passes.
The GPU fixture verifies tone mapping of radiance above display white at the
viewport corners and centre, owner mismatches, and restoring the valid image.
Combined GPU runtime/compositor checks pass 3,248 assertions / 3 cases; full
render passes 32,040 / 428 and graph passes 8,868 / 224. Logs use the
`eye-compositor-` prefix under the build test directory. The renderer node exists;
product graph selection, remote eye routing and full remote walking still need
wiring and end-to-end verification.
Client and Studio rebuild successfully. The unchanged combined local walking
gate still passes all 9,057 assertions after the tone-map input fix
(`eye-compositor-walk.log`).

`DefaultEyeDocument` now provides the reusable whole-eye graph: imported HDR,
one tone map, local screen interface composition and output. It retains only
the resources needed by those passes. `PortalImageHost::SubmitEye` builds the
bounded camera request, selects that graph once per host, binds the local image
owner and resolves the named destination through the existing endpoint path.
Callers pump after submitting their views and refresh the returned image handle.

The real-process host test now exercises this API alongside pane captures.
It checks a first copied image, renewal with no extra upload, a changed scene,
endpoint withdrawal and child exit. Combined host/compositor GPU tests pass
542 assertions / 3 cases (`eye-host-gpu-final.log`); full render passes 32,040 /
428 and graph passes 8,868 / 224. Final composed targets use the backend display
format, so their pixel checks account for BGRA/RGBA order and UNORM/sRGB storage
instead of assuming the intermediate target's format.
The whole-eye process variant now samples all four corners, four edge midpoints
and the centre. The same 542 assertions pass (`eye-host-corners-gpu.log`). Client
and Studio also build successfully (`eye-host-products-build.log`). These are
offscreen dev-preset correctness checks, not release performance measurements.
This connects the image host to full-viewport display. Studio/Client camera-world
selection still needs to invoke it for remote eyes, with copied seam data for
the return path and player-session handoff coverage.

Remote eye routing needs the following concrete work before enabling remote
results in `ResolveCameraPortalWorld`:

- [x] Define the owned camera topology payload and validate it independently of
  renderer and bus state. `scene::CameraPortalTopology` carries world, revision
  and named foreign mouths. Its versioned codec bounds snapshots at 256 mouths
  and 256 KiB, validates aperture frames and mapping poses, rejects duplicate
  names, and preserves output on failure. Decoding does not intern names;
  explicit resolution happens after host authentication. Copying a mouth drops
  local pane, camera and surface identities. The scene suite passes 516,632
  assertions / 540 cases (`camera-topology-scene.log`), including scaled forward
  and return crossings, body rebasing, truncated/invalid payloads, empty worlds
  and a maximum-size snapshot. This is codec and camera-step evidence only;
  endpoint publication and product routing are not connected yet.
- [x] Publish a bounded, owned destination seam snapshot through authenticated
  presentation messages. Carry authored world and seam names, aperture geometry,
  mapping, crossing policy and a topology revision. No ECS entity handles, local
  intern IDs or pointers cross the boundary. Keep this metadata independent of
  pixel renewal: unchanged pixels do not prove unchanged crossing topology.
- [ ] Bind accepted topology to the producer incarnation and complete snapshot
  revision. Distinguish an authoritative empty seam set from unavailable data.
  Reject incomplete, stale and oversized snapshots atomically. Retire cached
  topology and dependent eye routes on endpoint withdrawal or replacement.
- [ ] Use the same camera stepping code for local and copied seam sets. Preserve
  the receiving-mouth precision rule, bounded multi-hop traversal and reverse
  crossings. Stage history updates until all required destination data is
  available, so a failed lookup cannot leave the eye in an unreachable world.
- [ ] Connect remote selection to `SubmitEye` in the shared presentation path.
  Keep input and the humanoid subject with the player session while selecting
  the eye's image independently. Pump once after all view submissions. Preserve
  image ownership when body admission replaces the replica; require a drawable
  destination before admission and explicitly test delayed/refused publication.
- [ ] Cache seam snapshots by topology revision and endpoint incarnation, shared
  across viewports. Keep only camera history per eye. Rebuild route lookup on
  directory/replica-readiness changes rather than surveying every world per view.
  Bound cached bytes and outstanding requests, coalesce superseded camera poses,
  and reuse resident images or copied uploads only under existing ownership and
  revision checks. Do not introduce a synchronous GPU readback for topology.
- [ ] Run first-person eye-before-body and third-person body-before-eye sequences
  in both directions against a real product process. Include rotated/scaled
  mouths, multiple crossings in one frame, topology changes, endpoint restart,
  missing data and withdrawal. Check camera continuity, humanoid subject, single
  body authority, full-frame pixels and resource retirement together. Measure
  1/2/8 views in release with CPU, GPU, transferred bytes and live/peak residency.

`PortalTopologyHost` now serves named seam snapshots on separate presentation
endpoints. `PortalImageHost::Serve` advertises the topology producer alongside
images, and its pump and teardown service both. The adapter authenticates the
reply endpoint/correlation and world before resolving names, shares one cache
per destination across views, and bounds producers, sources and destinations
at sixteen each. Endpoint retirement immediately makes the cached view
unavailable; reopening uses a new incarnation. Full replies replace snapshots
atomically. Unchanged topology renews with a twelve-byte response, preserving
the resolved seam allocation. The local bus test measures twenty-four queued
payload bytes for the request/renewal pair.

Requests coalesce while pending, refresh no faster than 250 ms and expire after
one second. Producers gather once per requested batch and reuse encoded bytes
when the complete named seam set is unchanged. This does not eliminate the
gather/comparison cost or prove synchronous moving-seam freshness. Camera history
invalidation and drawable admission still need to consume these revisions.

The adapter and image-host headless subset passes 127 assertions / 8 cases,
including changed/empty topology, remote world mismatch, withdrawal, reopening
and queue-pressure recovery. Full render passes 32,118 / 432. Existing process
image/compositor GPU checks pass 542 / 3. The actual Client subprocess now must
advertise both endpoints and return both image and topology through its inherited
driver link; all 137 assertions pass for seam/eye requests and stop/disconnect.
That product fixture still contains an empty authored world, so it is not a
remote walking proof. Client and Studio build successfully. Full Client passes
9,185 / 161 after correcting a camera fixture that used `SnapshotBuffer` before
registering its canonical component name; the initial run aborted on that
registration conflict. Logs use the `portal-topology-` prefix in the dev build's
test directory. No release performance claim is made from these dev checks.

`ResolveCameraPortalWorld` now accepts the image host and uses its authenticated
copied seams for remote worlds. Missing or expired topology leaves the supplied
pose and stored history unchanged; once data arrives, the same bounded crossing
loop completes entry and return. The focused route test passes 69 assertions,
including endpoint withdrawal and expiry with an explicit Humanoid camera target.
It uses copied bus traffic between two universes and manually supplied camera
poses, not a simulated remote player transfer.

Studio now passes that host to camera routing, skips local scene preparation for
a remote eye and submits its mapped camera through `SubmitEye`. It keeps the
client-local interface, refreshes the accepted image handle after pumping, and
services topology even while the selected world is unavailable. The shared pane
image path requests remote return topology while its entrance image is already
demanded. Automatic process launch and standalone Client session adoption remain
open, so this wiring is not an interactive remote-walking acceptance result.

An integration check reproduced a retained-viewport bug: changing `EyeImage`
left the scene's portal signature unchanged. The signature now includes the
image handle and its world, viewport and key ownership. All eighteen signature
assertions pass, covering arrival, replacement, withdrawal and owner changes.
The full render suite passes 32,136 / 433, Client 9,254 / 162 and Studio 8,790 /
522. Client and Studio build; the unchanged local walking GPU gate passes all
9,057 assertions. The actual Client endpoint/image/topology subprocess check
also passes (133 assertions in this run). Logs use `remote-eye-` under the dev
build test directory. The remaining remote gate must include body admission,
foreground character geometry, topology revision changes and nested look-back
images together; those are not proved by local walking or this routing test.

The product admission gate now waits for an explicit `Fresh` application request
before creating a player. The existing process test reproduced premature avatar
creation and now passes all twelve assertions, including repeat-request player
and character identity. The actual headless Client/server probe also assigns a
player and joins the replicated world. Full Client and load-test suites pass
9,254 / 162 and 85 / 19 respectively.

A separate real-socket test reproduced an admission reply lost when the server's
send allowance was exhausted. The host now retains the latest encoded reply per
live client slot and retries after advancing the link. Accepted replies leave
the queue; newer requests supersede pending replies, and disconnect or shutdown
removes them. All sixty focused assertions pass for Ready/Refused replies and
attempt replacement, including exactly one player after repeated fresh requests.
Full server passes 768 assertions / 67 cases. Logs are `portal-fresh-*` and
`admission-pressure-*` under the dev build test directory. These checks establish
fresh admission and reply delivery only; the destination adapter below supplies
the separate resume/commit checks.

Listening server hosts now advertise a `portal-sessions` endpoint with a fresh
host session. Their inherited driver link accepts authenticated route snapshots
and owned messages, publishes endpoint changes and retains outgoing messages
under backpressure. Route-only world entries are removed on withdrawal. Stop or
driver loss retires endpoint ownership, pending traffic and local leases. The
actual server subprocess discovery test passes 26 assertions: it advertises the
endpoint, receives a copied lease request through a newly discovered source
route, refuses an unconfigured destination and stops cleanly.

The destination adapter accepts lease offers only from the named source endpoint
and session. Resume checks the client signing identity, destination incarnation
and exact committed transfer receipt. It reserves the existing player without
assigning input ownership; commit installs the connection's player mapping.
Fresh requests cannot create another player while that connection has a lease.
Repeated commits are idempotent, and one connection cannot reserve two transfers.
Both transports use the configured signing seed or an ephemeral identity whose
public key can be pinned by the lease route.

The real-socket destination test transfers an actual humanoid rig between two
worlds, obtains its lease over the owned bus, refuses premature commit and a
wrong capability, then resumes and commits the same player. All 55 assertions
pass, including one remaining player and character after repeated commit. Full
server passes 849 assertions / 69 cases; game passes 2,676 / 87, including 1,716 /
5 for the session protocol. Logs use `portal-lease-` in the dev test directory.
The fixture explicitly configures physical transfer endpoints and drives the
wire connector. Automatic endpoint configuration and the standalone Client
successor connection remain open. This is not a continuous
product walking or camera/image acceptance result, nor a release cost measurement.

The physical transfer protocol now has an opt-in player admission gate. It
retains the source rig in Preparing until `AdmitPortalPlayerTransfer` names the
exact destination incarnation. Missing admission or a different incarnation
cannot queue commit or retire the source. The policy and approval are part of
the existing world snapshot, so replay preserves both the hold and the accepted
crossing. Sixty assertions pass across missing, matching and mismatched admission
with byte-identical snapshot replay. Object transfers and the existing ungated
local path retain their behavior. Full script passes 797 assertions / 77 cases.

Client connection setup now retains a signing identity for the Client lifetime,
using the configured play key or a newly drawn ephemeral key. This supplies the
identity a successor connection must prove without requiring a manual play key.
The actual headless client/server probe signs in, receives its player and joins
the replicated world. Its first log assertion expected the wrong join prefix;
inspection of the captured log confirms the join. Client and Server build; their
full suites pass 9,254 / 162 and 849 / 69. Logs use `portal-source-` and
`source-identity-probe/` under the dev test directory. Successor connection wiring
remains required; the gate is not yet automatically enabled by the product host.

The transfer protocol now exposes `CancelPortalPlayerTransfer`. Cancelling is
an active, snapshotted phase: source physics and animation stay paused until the
named destination acknowledges cancellation. The destination frees the reserved
body buffer and retains a bounded cancellation record, so duplicate cancellation
and late offers cannot recreate the body. Cancellation retries through dropped
replies and bus refusal. Once commit has been queued, cancellation cannot restore
source authority. A retired receipt whose outcome is no longer known does not
receive a cancellation acknowledgement; recovery after that bound remains a
host-policy requirement, rather than permission to create duplicate authority.

Headless checks cover lost cancellation acknowledgements, matching snapshot
replay, cancellation before the original offer arrives, late offer delivery,
seventy successive cancellations across the sixteen-reservation and sixty-four-
record bounds, and a successful transfer afterward. Full script passes 1,350
assertions / 79 cases. Client, Server and Studio build; full suites pass 9,254 /
162, 849 / 69 and 8,774 / 522 respectively. Logs use `portal-cancellation-` in
the dev test directory. These are protocol and integration checks, not a new GPU
walking result.

The source Server now requests and renews destination leases through the named
presentation endpoint. It offers the resolved host identity, port and exact
transfer claim to the authenticated player connection. `Proceed` admits physical
transfer only after that client accepts the offered claim. `Crossed` is sent only
after the physical receipt reports committed. Offers and terminal replies retry
transport backpressure; lease refusal, client refusal, readiness expiry and
endpoint withdrawal request acknowledged physical cancellation. Disconnect also
cancels a preparation and removes its source character. Lease state is bounded
to sixty-four departures and instrumented through the existing frame profiler.

The real-socket source fixture passes 167 assertions across accepted transfer,
destination refusal, client refusal and endpoint withdrawal. It checks the held
Humanoid, rejects a wrong capability, restores the original player on refusal,
and observes one destination character after acceptance. The fixture uses real
physical transfer between two local worlds and an explicitly driven destination
control endpoint. Initial fixture failures exposed missing world setup and a
missing client send-budget advance; both are corrected. Protocol roundtrip and
truncation coverage includes the new messages. Automatic physical endpoint
configuration, successor Client adoption and continuous camera/image walking
acceptance still remain open.

Full validation found a Server shutdown lifetime bug in the existing Project ZIP
case. Two debugger runs reproduced a Luau runtime detaching its removal hook
through a freed Store: the host retained a shared runtime reference after
destroying its world driver. Shutdown and destruction now release those
references before the driver. The complete previously failing suite order passes
1,016 assertions / 70 cases after the fix. Full game passes 3,044 / 87 and Client
9,254 / 162; Client, Server and Studio build. Architecture and formatting checks
pass. Evidence is in the `portal-source-` logs under the dev test directory,
including the original failure and debugger stacks. No new GPU or release-cost
claim follows from these headless checks.

The standalone Client now stages a successor socket and replica when the source
offers a portal transfer. It pins the offered destination identity and reuses
the client's signing key. An admitted connection, joined destination snapshot
and captured player camera precede `Proceed`; the source stays visible while
the successor waits. After `Crossed`, Resume reserves the existing destination
player. The client creates a replica viewer, maps the retained camera onto that
player's Humanoid, commits admission, then replaces the active connection and
replica. Input is suppressed after Proceed until adoption. The old view channel
is retired before handle reuse, and old script references are dropped before
world destruction. Deferred sound teardown remains available for retry.

The actual Client process test passes 29 assertions against two authenticated
listeners. The fixture admits a copied, rotated and translated rig only after
Proceed and removes the source rig before replying Crossed. It observes Resume,
Commit, the same signing identity, no destination Fresh request and destination
input with no further source input after commit. Its first run reproduced a
missing viewer camera on the staged replica; creating that viewer fixed the
adoption stall. This fixture drives the host application messages explicitly;
the combined Server host discovery and physical bus path remains to be tested.
The offscreen process opens a GPU but records no draw calls, so this is session
and camera-adoption evidence, not portal pixel acceptance.

Client's full headless suite passes 9,264 assertions / 163 cases, including
retired-view handle reuse; Studio passes 8,774 / 522. Client and Studio build,
and architecture and format checks pass. Discovery is closed before replacing
its borrowed socket during adoption. Logs use `portal-successor-` in the dev
test directory. Outstanding
gates include drawable destination publication before Proceed, cancellation and
timeout integration, recovery after a successor fails once Proceed was sent,
content readiness and the independent eye-world image path. Physical transfer
endpoints are not automatically enabled by this client wiring.

Retry validation now covers an immediate replacement offer after acknowledged
refusal. The source previously looked up only the player's newest receipt and
lost the old refusal when a script started another transfer in the same tick.
It now resolves the outstanding departure by its exact transfer receipt before
considering the new one. The Client retains a following offer until the refused
successor has been cleaned up. The source integration passes 207 assertions;
the actual Client process passes both normal and pinned-identity refusal/retry
paths, 64 assertions. These remain session checks with zero draw calls.

That process test also reproduced an identity claim lost to send backpressure.
Connector now retains one encoded proof, retries it after the send budget resets,
and gates application sends and replication acknowledgements behind it. Both
wire stacks sign their session binding through the same helper. A forced socket
refusal then reproduced a reliable sequence hole: the receiver waited for a
packet that had never entered the retransmit queue. Session now reuses the
unsent sequence while each encrypted retry receives a fresh nonce. The focused
test covers first identity delivery and refusal after the stream is established.
Evidence uses `portal-identity-` logs in the dev test directory, including the
original failing socket case. No release performance claim follows from these
checks; post-Proceed recovery and drawable-view readiness remain open.

Final headless suites pass: replication 20,584 assertions / 261 cases, Client
9,264 / 163, Server 1,056 / 70 and Studio 8,774 / 522. Client, Server and Studio
build; the target graph and changed-source formatting checks pass. The final
actual Client successor test passes 64 assertions across both offer outcomes.

Post-Crossed application refusal now retries Resume/Commit on the existing
authenticated connection, retaining the exact lease and any already identified
player. A 250 ms monotonic delay bounds retries while socket polling continues.
The source no longer discards its departure when Crossed enters the reliable
stream: it keeps renewing that claim every five seconds until the old client
connection retires. Crossed itself is queued once, and refused departures still
retire after their terminal reply is accepted.

Both missing paths were reproduced first. The actual Client process previously
stalled after either a refused Resume or a refused Commit; all four fixture
outcomes now pass 138 assertions, including the original normal and pre-Proceed
refusal/retry cases. The source integration previously emitted no renewal after
physical commit; it now passes 213 assertions including the unchanged claim,
attempt and single Crossed reply after the renewal interval. Logs are
`portal-post-proceed-*` and `portal-lease-renewal-*` under the dev test directory.
These cases do not prove reconnect after transport loss, destination restart or
long-delay image readiness. Those remain requirements before automatic walking.
Full headless suites pass Client 9,264 assertions / 163 cases, Server 1,062 / 70
and Studio 8,774 / 522. All three products build; changed-source formatting and
the architecture target graph pass. No new pixel or release-cost claim is made.

The successor now detects transport death separately from historical admission.
After Proceed, a failed connection retires its staged replica and cached content
directory, waits 500 ms, then opens a new pinned connection. It retains the exact
transfer claim, any identified destination player and the mapped Humanoid camera;
it never sends Fresh. Resume/Commit repeat against the new joined snapshot.
Socket polling continues while the source connection keeps its lease alive.

Two actual Client process failures were reproduced by replacing the destination
listener during Resume and Commit while retaining the destination Store and
signing identity. Both now reconnect and adopt the same player. The complete
six-outcome process fixture passes 210 assertions, with no destination Fresh or
source input after commit. This demonstrates session loss within the same world
incarnation, not recovery of a restarted world whose transferred body was lost.
Separate simulated-time tests verify later timeout detection on both datagram
and QUIC connectors. Full replication passes 20,596 assertions / 263 cases and
Client 9,264 / 163. Studio passes 8,790 / 522; all three products build.
Changed-source formatting and the architecture target graph pass.
Logs use `portal-transport-loss-`.
The process still records zero draw calls, so drawable destination readiness and
the combined image/body/camera walk remain open.

Client handoff now requires a completed destination image before Proceed and
again after binding the arrived Humanoid camera, before Commit/adoption. The
staged replica uses the shared eye capture producer and resident receipt path
in reserved Client viewport slot 1. A camera or resolution change invalidates
the staging receipt. The arrived-player phase explicitly retires the pre-crossing
image so an older snapshot cannot satisfy adoption. Reconnect, refusal and
adoption retire the corresponding staging source; normal displayed composition
continues in slot 0.

The image host now offers a strict current-request lookup alongside its normal
display lookup. An old image retained while a replacement is pending is still
available for display, but cannot satisfy the gate. The GPU receipt test passes
24 assertions covering replacement, completion, expiry and viewport cleanup,
with zero upload bytes on the local resident path. Drawing follows Client's
presentation schedule while connection and lease work continue between frames.
This is staged destination drawability, not proof that the visible entrance and
independent eye-world image remain seamless during the complete physical walk.
Content delivery readiness and combined image/body/camera acceptance remain open.
With both drawing gates enabled, the final six-outcome actual Client fixture
passes 210 assertions. Full headless suites pass render 32,137 / 433, Client
9,264 / 163 and Studio 8,790 / 522. Client and Studio build; changed-source
formatting and the architecture graph pass. Evidence uses `portal-drawable-`
logs in the dev test directory. These are correctness checks, not a release
performance measurement or an all-angle portal pixel comparison.

Listening Server hosts now configure their local primary world's physical
portal incarnation from the authenticated presentation session and require
player admission. Setup installs the transfer systems for both scripted and
placeholder worlds before advertising readiness. Source and destination host
tests no longer assign those incarnations manually; the spawned Server discovery
test obtains a real lease route with the configured incarnation and listening
port. Unsupported conflicting preconfigured state fails startup rather than
silently reusing another incarnation or dropping the admission requirement.

The first automatic-setup run reproduced the placeholder world's missing
transfer pump. Host setup now installs it idempotently. Configuration precedes
the recorder's initial snapshot, and replay restores transfer systems from the
recorded configuration. Contact codecs and channel callbacks register before
snapshot loading, so a fresh process can restore that state. A listening Server
recording replayed in a separate process is byte-identical. Full Server passes
1,081 assertions / 70 cases and script 1,350 / 79. Client, Server and Studio
build; changed-source formatting and the architecture graph pass. Evidence uses
`portal-auto-endpoint-` logs and recordings in the dev test directory.
The newly eager contact registration exposed an unclassified private resource
in Client's replication audit. `script.PortalContactRequests` is now explicitly
excluded there; it belongs to tick exchange and was never added to replication.
Final Client passes 9,265 assertions / 163 cases and Studio 8,790 / 522.

This enables the authenticated product transfer path. The complete multi-host
image/body/camera walk and its content/image readiness failure cases still need
combined acceptance; local Client drawability is not all-angle seam continuity.

Process contact integration has a new shared `world::TickExchangeHost` adapter.
Its bounded command/result codecs carry Begin, Collect, Serve, Apply, End and
Cancel phases over owned bytes. Frame identifiers are monotonic per link and
rounds are bounded; exact duplicate commands reuse the previous result without
advancing Input or physics twice. Serve returns copied destination data. Apply
requires the complete matching reply set before integration. Disconnect cancels
an incomplete frame. A frame-matched Cancel also works when the driver's round
acknowledgement was lost.

The focused tests include an actual child process serving contact bytes before
source integration, stale reply rejection, transactional truncated-frame parsing,
direction separation and batch limits. A slower host serves another host's
catch-up round at its committed tick without gaining an extra Input or physics
step. Initial inspection from outside a paused world correctly triggered the
engine's ownership assertion; observations now run inside that world's phase
callbacks. Evidence uses `portal-host-exchange-` logs in the dev test directory.
The final adapter suite passed 29,633 assertions / 222 cases; Client, Server and
Studio builds passed.

`HostLink` now carries these commands and results with separate bounded codecs.
`Supervisor` permits one pending command and one result per connected host. It
checks frame, round and operation, collected source-world ownership, and exact
served request stamps. Outbound Serve and Apply batches must address worlds owned
by the selected host. Cancel may supersede the same frame's pending command;
replacement or retirement clears pending results. Unsolicited, duplicate, stale,
wrong-direction and foreign-world responses are refused and counted. Transport
byte and message counters measure the actual encoded frames.

The child-process contact test now runs through this Supervisor/HostLink path.
Focused checks also cover truncated outer frames, cancellation with a lost reply,
and replacement links. The full World suite passed 29,604 assertions / 225 cases.
Server passed 1,081 assertions / 70 cases. All three product builds, formatting
and the target architecture check passed. Evidence uses `portal-phase-link-`
logs in the dev test directory.

Driver now coordinates local and supervised hosts through Begin, Collect, Serve,
Apply and End under a shared monotonic frame deadline. It routes copied requests
by the world directory, including remote-to-local and same-host destinations.
Every host participates in the maximum owed round count, so a slower destination
can serve another round without advancing again. Missing or retired destinations
answer Unavailable. Failure cancels the frame; an unacknowledged cancellation
closes the link before the next tick. Permanently failed hosts are retired while
surviving worlds continue.

Server enables this mode for the hosts it launches. Its private launch flag makes
children wait for phase commands instead of ticking independently. Application
work runs outside paused phases, other incoming frames are deferred with count
and byte bounds, and completed bus traffic precedes the End acknowledgement.
Disconnect and shutdown cancel the host adapter before world cleanup.

Retry review reproduced duplicate bus publication on a repeated End command.
The host now records the published frame identity and only repeats the phase
acknowledgement. The real-server retry test and shared-frame product test pass
36 assertions / 2 cases (`portal-server-phase-retry-fixed.log`). The contact
process fixture also requests copied data in both directions, including a
slower destination serving the source's catch-up round.

The first product run exposed an unregistered command-line flag (`unknown option:
--host-tick-exchange`); that run was stopped, the flag registered and the suite
rerun. Real-process tests cover successful contacts, destination refusal before
integration, mixed rates, and an unresponsive participant. The product test runs
two actual Server children and the local world for exactly eight frames each.
World passed 29,661 assertions / 227 cases and Server 1,089 / 71. Evidence uses
`portal-driver-exchange-` and `portal-server-phase-` logs in the dev test directory.
Client passes 9,265 assertions / 163 cases and Studio 8,790 / 522. The three
products build, formatting passes and the target graph remains valid. These are
dev correctness checks; no release performance conclusion is claimed.
After the End retry fix, the full Server suite passes 1,117 assertions / 72 cases
(`portal-server-phase-complete-server.log`).
This proves the product tick path, not combined portal image/body/camera
continuity; the complete physical walk and presentation-world work remain.

The Client image producer now also accepts a pinned live replication source.
It creates only an adopt-only replica under the advertised world name, waits for
its joined snapshot before advertising image/topology endpoints, and sends no
Fresh admission, movement or teleport requests. It opens no audio device. Source
session rejection or loss clears its producers, withdraws the directory and ends
the loop. Saved-game production retains its existing launch path.

The live-source run exposed missing return-world entries: saved producers knew
their other game worlds, while a single replica did not. The shared image host
now creates remote world entries from trusted driver routes, rolls them back on
rejected directories and retires entries it created when routes disappear.
The GPU fixture covers saved and replicated worlds with Seam and Eye projections.
A visible panel's authoritative lighting changes produce a different pixel hash
and lighting revision. Both driver shutdown and loss of the replication source
retire the producer, with no fresh player or movement sent. Evidence uses
`portal-replica-producer-` logs in the dev test directory. Automatic producer
launch/routing beside product Server hosts and the combined image/body/camera
walk still remain; a live producer alone does not prove seamless player crossing.
Final GPU validation passes 445 assertions across eight launch combinations.
The default suites pass Render 32,136 assertions / 433 cases, Client 9,265 / 163
and Studio 8,774 / 522. Client and Studio build; formatting and the target graph
pass. This is dev correctness evidence, with no release-cost claim.

The world module now supplies `PresentationRelay` for the next product launch
step. An authoritative host delegates explicitly granted presentation channels
to a trusted child while keeping ownership of its world. The relay translates
parent and child endpoint incarnations, preserves opaque payloads and routes
replies to local or remote consumers. Same-world consumers receive a return
alias so their routes cannot claim the child's local replica. Directory
withdrawal and disconnect retire only delegated channels. Queues share a
64-message / 32 MiB bound with a 4 MiB payload cap; full parent queues retain
replies, while obsolete receipts and duplicate child sequences are refused.
Headless tests cover the relay boundary.
Relay validation passes 565 assertions / 6 cases; the full world suite passes
30,226 assertions / 233 cases (`portal-relay-focused.log` and
`portal-relay-world.log` under the dev tests directory). Formatting and the
target graph pass. No GPU or release profiling was run for this transport step.

Listening Server hosts now accept `--presentation-program PATH` and launch a
live Client producer automatically when configured. The setting passes to remote
world hosts. A separate ephemeral client identity admits the producer on a
restricted listener, while the producer pins that listener's identity. Server
retains world ownership and relays image/topology endpoint receipts through its
existing driver directory. Producer failure and Server shutdown retire the relay
and process. Automatic restart remains open. Server still has no renderer link.

The product fixture exposed and fixed two launch gates: producer incarnations
now fit the CLI's positive signed range, and supervised headless producers can
use their inherited link lifetime instead of a finite frame count. It also
reproduced supervised hosts parsing `.agame` as scripts. These hosts now load the
project, validate their named grants and convert ungranted worlds to remote
entries before starting local scripts. A headless child test checks the chosen
world's authored transform and the other world's remote ownership.

The offscreen product GPU test passes 96 assertions over four launch combinations:
Seam/Eye crossed with Stop/driver disconnect, with restricted admission and a
two-world saved project. It receives captured pixels through Server's delegated
endpoints. Default Server tests pass 1,124 assertions / 73 cases; Client passes
9,265 / 163. Evidence is in `portal-product-producer-` dev test logs. This does
not yet prove the combined physical image/body/camera walk or release cost.

Standalone play transport now has a bounded `PresentationStream` codec. It
fragments the existing presentation directory, routes and message formats into
packets of at most 1,024 bytes. Send refusal retains the next packet exactly,
and each pump has a packet budget. Each direction is bounded to 64 frames and
32 MiB of encoded queued data, including an incomplete receive reservation.
An individual encoded frame is capped at the 4 MiB presentation payload limit
plus envelope room. Unsupported commands, malformed fragments, sequence gaps
and receive overflow close this presentation stream and release queued data.
Unrelated play messages remain available to their existing handlers. Counters
report packet/byte transfers; queue byte counts are not heap residency figures.

The carrier test sends a 128 KiB image payload through real authenticated
Listener/Connector sessions over datagram and QUIC. Both deliver identical bytes
with and without a deliberately lost packet, passing 1,112 assertions across
four combinations. This uses loopback transports with the real protocol stacks,
not separate OS processes. The stream does not grant endpoint authority;
the per-connection adapter below supplies that boundary.
Stream unit checks pass 417 assertions / 6 cases. Full world and replication
suites pass 30,643 / 239 and 21,708 / 264 respectively, with formatting and the
target graph passing. Evidence is in `portal-presentation-stream-` dev test logs.
No GPU checks or release profiling ran for this packet transport step.

Server and standalone Client now carry presentation directories and messages
over the authenticated player connection. `PresentationPeer` maps each player's
consumer receipts to unique host-owned return channels. These channels enter the
existing driver directory under the authoritative world, preserving host ownership
checks without introducing synthetic player worlds. Each peer has 64 numeric
slots per allowed consumer channel; Server bounds presentation peers to 16.
Untrusted client world names remain strings and do not grow the intern table.
Only image/topology producer routes reach players. Forged consumer receipts and
control channels close the presentation stream; transient missing endpoints and
queue pressure drop requests without closing the player's connection. Disconnect
retires its grants. Client replacement clears old routes and orders new directory
updates independently of the producer endpoint's session and generation.
If its consumer directory cannot be queued, Client keeps requests in the bounded
bus until it can queue the directory first.

The product fixture now also discovers routes and receives a fragmented 128 KiB
image and topology through an authenticated UDP player connection. It uses a real
Server child and its live Client image producer, with a CPU Connector consumer.
This does not yet prove standalone Client discovery during the combined physical
walk. The separate actual-Client successor fixture passes 210 assertions over six
outcomes. Peer isolation, withdrawal, forged channels and bounded name churn pass
301 assertions in three cases. Evidence is in `portal-player-` and
`portal-presentation-peer-final.log` dev test logs. Release profiling remains open.
The final product run passes 776 assertions across eight launch combinations,
including a missing-endpoint request before successful image/topology replies.
Default world, Server, Client and render suites pass 30,944 / 242, 1,124 / 73,
9,265 / 163 and 32,136 / 433 assertions/cases respectively. Client, Server and
Studio build successfully. Formatting, whitespace and the target graph pass.

The actual-Client successor fixture now decodes its presentation stream on both
player connections. It checks valid consumer receipts and increasing directory
revisions before handoff, then a fresh stream and the empty withdrawal directory
after the staged view retires. All six existing handoff/refusal/reconnect outcomes
pass: 387 assertions in `portal-discovery-successor-final-gpu.log`. The first run
failed six assertions because it incorrectly expected active portal consumers in
the destination scene, which has no portal views; the final check requires their
withdrawal instead. This proves consumer publication and retirement during session
replacement, not producer route consumption or the complete physical walk.
Only the test target was rebuilt for this check; formatting and whitespace pass.

The connected-replica reproduction exposed two product faults. The Client did
not collect the replica's portal surfaces, and the raw `scene.Portal` codec sent
the process-local destination-name number. A real child decoded `other-side` as
`Brightness`. The codec now writes the destination world as text, preserves the
destination entity and both crossing flags, and omits reserved padding. This
changes the portal component wire/snapshot representation; old raw component
payloads cannot recover their original destination strings.

Client now shares its presentation collector between local and joined worlds.
Joined play selects the replica's camera and unshifted rows in the compositor;
lighting, shaders, editable resources and portal requests use that same world.
The previous compositor used the demo camera and displaced the scene by its
multi-world spacing. Selection changes to the destination replica on adoption.
Independent eye-world crossing remains a separate incomplete item.

The real-Client successor test now receives and decodes a seam-image request from
a portal authored in its server replica after discovering the producer endpoint
over the player connection. Before the fixes it failed with no request; afterward
all six handoff/refusal/reconnect outcomes pass, 425 assertions in
`portal-replica-final-gpu.log`. This fixture does not reply with image pixels, so
it proves discovery/request dispatch and session replacement, not complete visual
crossing. Compositor selection also has a focused camera/row replacement check.
Final verification: default scene passes 516,707 assertions / 541 cases, Client
9,278 / 164 and Server 1,124 / 73. The Server-owned producer GPU fixture still
passes 776 assertions across its eight sequential launch combinations. Client,
Server and Studio build; formatting, whitespace and the target graph pass.
Evidence is in `portal-replica-` dev test logs. No release profiling or complete
walk pixel comparison ran for these fixes.

Server now replaces an exited or disconnected image producer after withdrawing
its relay endpoints and reaping the old process. Retries wait 1, 2, 4 seconds,
doubling to a 30-second ceiling. A process that remains connected for 30 seconds
resets that delay. Each launch creates a fresh admission identity and producer
session; shutdown clears pending retries. The disabled path returns before reading
the clock. Starts and retry attempts have separate operation counters.

The product GPU fixture now includes a producer lifetime limit on its POSIX
player/seam case. It receives an image, observes endpoint withdrawal, discovers a
replacement receipt with a newer generation and receives another image over the
same player connection. A request carrying the retired producer receipt precedes
the valid replacement request without closing that connection. Before the fix,
the image producer stayed absent and the fixture timed out with expired topology.
Afterward all eight launch combinations pass, 935 assertions in
`portal-restart-after-gpu.log`. The short lifetime uses a POSIX test wrapper around
the real Client; this restart injection has not been exercised on Windows.
A CPU process test runs an immediately failing child and verifies bounded retries
while all 400 world ticks complete, passing six assertions in
`portal-restart-backoff.log`. These checks do not prove image retention or camera
continuity during restart in the complete physical walk.
The full Server suite passes 1,130 assertions / 74 cases. Server and the affected
test targets build; formatting, whitespace and the target graph pass. Evidence is
in the remaining `portal-restart-` dev logs. Release profiling remains open.

Standalone Client now runs the shared camera portal resolver before selecting its
displayed view. The eye's mapped world requests a whole-eye image independently
of the body's connection. Existing camera history continues through successor
adoption. Worlds without portal history keep the normal replica presentation path.
An unresolved destination selects the empty eye graph and retires its viewport
image, preventing a retained image from another world being displayed.

The actual Client subprocess fixture now moves its authoritative Humanoid root
incrementally through a replicated mouth and requires a far-side eye request
before Proceed, while the original player remains alive. Before the change this
failed despite seam requests reaching the advertised producer. Afterward all six
transfer outcomes pass, 1,155 assertions in `portal-eye-client-after-gpu.log`.
This fixture publishes root transforms directly and supplies topology replies;
it does not run the complete Server walking solver or return remote eye pixels.
The combined physical walk, third-person trailing arm and reverse crossings
remain open.

A single-device Vulkan check produces a resident eye image, removes its
destination, verifies retirement and reads back a black composed image. It passes
49 assertions in `portal-eye-missing-gpu.log`. Default Client and render suites
pass 9,278 assertions / 164 cases and 32,136 assertions / 433 cases respectively.
The Client and both test binaries build. These dev checks establish correctness
of request routing and missing-view output, not seamless delivery or release cost.

The Server source-admission fixture now loads the production game simulation for
its successful cases and sends `MoveInput` over the authenticated connection.
The character solver walks through an authored mouth from either approach
direction, and the automatic crossing system creates the transfer. Both source
and destination have the production physics setup, including destination sweep
validation. The existing admission gate then checks that the source rig survives
until Proceed, a forged capability is refused, only one player arrives, health
is retained and the arrival momentum points through the mapped exit.
The two walking directions and four refusal/recovery outcomes pass 297 assertions
in `portal-walk-server-directions.log`. Initial failures exposed missing physics
in the old placeholder fixture and its destination; no engine fix was required.
This is a real Server plus a CPU Connector, with a fixture-owned destination
lease reply. It does not cover a destination Server connection, a return trip by
the same player, or the combined displayed image/camera sequence.
The full Server suite passes 1,204 assertions / 74 cases in
`portal-walk-server-full.log`; the affected target builds and formatting and
whitespace checks pass. No performance claim is made from these dev fixtures.

The product Server walking fixture now supervises a real destination Server,
discovers its admission endpoint, reconnects using its advertised identity and
walks the same player back. Both transfers are triggered by networked movement.
The original source body disappears, the destination body disappears on return,
and the returning player retains health with exactly one authoritative player.
This exposed two runtime faults: copied floor cut edges stopped a grounded body,
and the Server driver never pumped presentation directories or lease traffic.
Copied-contact sliding now resolves local support first and rejects tangent box
contacts only when a separating face plane proves they cannot block translation.
The Server pumps presentation outside simulation and outside replay.

The floor/wall fixture passes nine assertions; the full physics suite passes
59,683 assertions / 255 cases. The final Server suite passes 1,246 / 75, including
the 42-assertion product round trip, and the script suite passes 1,350 / 79.
Dev builds, formatting, whitespace and architecture checks pass. Evidence is in
`portal-copied-floor-`, `portal-product-walk-` and `portal-product-return-` test logs.
This is CPU Connector movement against real Server processes. The actual Client's
displayed camera, remote eye images and release performance remain unverified in
this combined physical walk.

The actual Client now also has a combined GPU round-trip fixture. It starts a
listening Server with a supervised destination and both live image producers.
Ordinary SDL keyboard events drive movement; the test observes destination
session adoption before reversing and source adoption before releasing movement. It does
not inject transfer offers, body transforms or image replies. The initial version
passed nine assertions in `portal-client-walk-producer.log`, exercising the Client's current
image readiness gates with the real physical crossing and reconnection path.
The fixture now also requests an offscreen capture and completes its bounded
10,000-frame run after returning. It decodes the resulting 128 x 128 BMP and
requires visible pixels over at least one eighth of the image. All 14 assertions
pass in `portal-client-capture-final.log`. The saved frame shows the floor and
character; this is a final-frame check, not a continuous pixel comparison.

The capture target is necessary: the earlier headless run could advance thousands
of presentation opportunities without drawing the final view. Its transfer and
producer-image evidence remains valid, but did not establish displayed pixels.
Temporary diagnostics also identified the recorded presentation refusals as
`PresentationStatus::Full` at the destination image-request queue, rather than
identity failures (`portal-client-refusals.log`). The diagnostic logging was
removed after classification. No shipped performance claim follows from these
dev frame rates.

Portal sources now keep one request in flight per mouth while camera position,
sampling matrix and copied body geometry change. The host retries its latest
demand after completion; intermediate demands allocate no additional transport
request and skip hashing copied geometry. Seam changes, endpoint replacements
and binding-index changes still supersede pending work. The pending image keeps
the sampling matrix with which it was requested. Explicit invalidation and
receipt incarnation checks remain in force.

A CPU reproduction failed on the second camera revision before the change. It
now bounds 100 camera/sampling revisions to one queued request and sends revision
100 after completion. It also checks immediate seam and endpoint replacement.
GPU cases vary eye position and sampling matrices while the first image is
pending, then require its pixels to arrive over both copied and resident delivery.
The runtime suite passes 3,907 assertions / 11 cases. Full render and Client CPU
suites pass 32,253 / 434 and 9,278 / 164 respectively.

The actual Client round trip now waits for graceful Server shutdown and passes
17 assertions, including its nonblank capture. Neither Server reports a
presentation refusal in `portal-camera-flight-sampling-product.log`; the initial
guard missed changing sampling matrices and still reported 357 source refusals
in `portal-camera-flight-product-final.log`. The final guard covers both portal
and whole-eye requests. Remaining evidence is in `portal-camera-flight-` logs.
Builds, formatting, whitespace and architecture checks pass. Continuous product
camera/pixel parity and release performance remain open.

This fixture reproduced image producers repeatedly losing their relay connection
after joining a world containing a cross-world portal. The producer's normal
viewer path opened consumer endpoints outside its producer-only grant. Delegated
Client producers now skip that viewer request path while continuing to serve
requested cameras. Both producers remain available for the round trip.

The default Client suite passes 9,278 assertions / 164 cases. The existing
Server-owned producer GPU fixture passes. The combined related GPU run initially
failed two image-request assertions in the older session fixture, with 1,348 /
1,350 assertions passing. The older fixture subsequently passed both a comparison
build and the restored fix, with 1,159 and 1,155 assertions respectively. This
intermittent request failure remains unresolved; the passing repeats do not erase
the first failure. Evidence is in `portal-client-walk-related-gpu.log`,
`portal-client-walk-baseline-gpu.log` and `portal-client-walk-session-repeat.log`.
Build, formatting and whitespace checks pass.

The combined fixture now authors a two-world `.agame` with replicated LocalScript
camera observers. At a matched 30 Hz world/driver cadence, two offscreen Vulkan
runs pass 25 assertions each, including both physical handoffs and the final
capture. The observers check the local character's Humanoid subject on Heartbeat
in all three adopted stages. They wait for services and skip retired characters;
this is sampled subject binding, not proof of every displayed camera during the
body's absence between replicas. The default Client and Server suites pass
9,278 / 164 and 1,256 / 76 assertions / cases respectively. Evidence is in
`portal-walk-camera-rate.log`, `portal-walk-camera-repeat.log`,
`portal-walk-camera-client-full.log` and `portal-walk-owner-server-full.log`
under the dev build's `tests/` directory. Formatting and whitespace checks pass.

Authoring this fixture reproduced duplicate world ownership with a saved game
and `--remote-world`: the driver loaded the child-owned world locally, then its
remote registration failed with `name already taken`. `Server::HostProject`
now releases designated child worlds before starting scripts; `Driver::Start`
registers their remote ownership. The CPU `project-placement` case verifies the
remaining primary and the child-owned remote, including when the remote was
the first authored world. Its 10 assertions pass.

The earlier 60 Hz authored-world / 30 Hz driver handoff failure is now reproduced
and repaired. The saved-game CPU fixture showed an authoritative character rig
whose replica entity existed with zero components, before Commit. QUIC can deliver
component deltas before their reliable structure messages. `Replica` counted
received but unapplied parts toward a later acknowledgement of the same tick.
`Authority` also retired older unconfirmed rows when a newer acknowledged tick
omitted them under the recovery budget. Parts now count only after application;
acknowledgements retire only rows carried by that exact tick. Empty tag writes
also report incomplete application while their entity is absent.

Deterministic tests reproduced premature acknowledgement (`2 == 1`), seven
unrecovered values (`0 == 17`), and premature tag acknowledgement before the fixes.
A separate world test reproduced dirty bits lost between two simulation ticks in
one host frame. `World::PrepareTick` now clears changes only at the start of a
batch, retaining all writes until publication. Both ordinary and coordinated
exchange batches are covered.

The combined Client fixture now keeps both authored cadences enabled. It passed
50 assertions with physical entry and return, Humanoid camera samples in all
three stages and nonblank captures at both rates. The saved-game CPU round trip
passed 192 assertions across both rates and both transport modes, requiring the
rig before Commit and preserving Humanoid health on return. These checks do not
establish continuous displayed camera or pixel continuity. Temporary rig logging
was removed.

Final evidence under `.cache/build/dev/tests/`:
`portal-applied-complete-replication.log` passes 21,741 assertions / 267 cases;
`portal-applied-complete-server-walk.log` passes 192 assertions;
`portal-applied-complete-client-walk.log` passes 50 assertions.
`portal-applied-world-full.log` passes 30,982 assertions / 243 cases.
The deterministic failures are retained in `portal-applied-before.log` and
`portal-applied-tag-before.log`. The final capture is also available as
`portal-applied-walk.png`; it shows the returned character, not the seam crossing.
The fixes reuse the existing part bitmap and bounded recovery cursor without a
new message queue or per-entity cache. Release performance remains unmeasured.
Build, formatting and whitespace checks pass.

The product walk now exercises both trailing and scroll-selected first-person
cameras at both authored world rates. It waits for three stationary camera samples
before entry and before return, then requires the selected zoom again after the
round trip. The Humanoid subject remains sampled during movement. Distance from
a moving predicted camera to the delayed authoritative root is not a valid zoom
oracle; the first version of that assertion was replaced with stationary checks.

This exposed movement being submitted and predicted on every update iteration,
even when no simulation tick elapsed. `Client::SubmitMove` now records its last
successful tick, retries a refused send, and resets that record on successor
adoption. A trace check observes actual submissions in all three connection
stages. Removing the guard made only that check fail: 31 / 32 assertions passed,
with 705, 1,010 and 5,083 submissions across the three stages in the 30 Hz trailing
case. `portal-move-negative-walk.log` records the controlled failure.

The stricter trace also reproduced the replica clock returning to zero during
snapshot application. `Store::Apply` now has an explicit `ApplyClock` policy;
replication preserves local time for both prefaces and full snapshots, while the
default continues to restore saved time. The deterministic pre-fix test failed
11 / 20 assertions (`portal-replica-clock-before.log`). The fixed replica-clock
case passes 20 assertions, and the ECS case passes 40 across both entity modes,
both clock policies and corrupt snapshots. No separate client clock was added.

The full dev ECS, replication, Client, Server and world suites pass respectively
134,142 / 442, 21,761 / 268, 9,278 / 164, 1,406 / 76 and 30,982 / 243
assertions / cases. These results are in `portal-preserve-clock-*-full.log`.
The four product walk combinations pass 128 assertions in
`portal-preserve-clock-walk.log`, including increasing move ticks when trace
logging is compiled in. Per-case final captures use
`portal-client-walk-{30,60}-{first,third}.bmp`. The restored final build also
passes 128 assertions in `portal-camera-clock-final-walk.log`; matching PNG
exports were inspected for all four cases. The final capture is still a returned
view, not a sequence through the seam. Architecture passes 47 modules, 6 programs,
33 layered modules and 6 fixtures (`portal-camera-clock-architecture.log`).
Formatting and whitespace checks pass. Release cost remains unmeasured.

The combined fixture now records every submitted camera and captured image with
`--capture-sequence`, using 600 presentations at 60 Hz in each of the four cases.
This reproduced lost mouse and wheel input between independently paced updates:
the trailing cameras never turned, and neither first-person case started walking.
`portal-capture-series-walk.log` records six failed assertions. Camera input now
latches pending turns and wheel notches in the controller until its next update;
raw script input remains unchanged. Each input batch is written once, including
the first batch after joining or adopting another replica. The latch adds 16 bytes
per controller and no queue or allocation. Its focused tests cover accumulation,
single consumption, button release, focus loss and disabled/scriptable cameras.

The frame sequence also reproduced the old character retiring before destination
admission, which made `AimReplicaViewer` borrow the unrelated local demo camera.
The largest vertical jump was 3.17 metres. The client now retains its existing eye
while the staged portal successor has a camera continuation. Every frame after
the initial Humanoid binding must stay within 0.05 metres of the flat fixture's
four-metre eye height. All four cases pass, with a maximum error of 0.00188 metres.
This holds the eye during the missing-rig gap; it does not prove uninterrupted
movement or a valid Humanoid subject during that gap.

Evidence under `.cache/build/dev/tests/`:
`portal-pointer-latch-unit.log` passes 18 assertions / 2 cases;
`portal-pointer-latch-scene-full.log` passes 516,725 / 543;
`portal-pointer-latch-client-full.log` passes 9,278 / 164;
`portal-eye-retention-walk.log` passes 7,348 assertions across all four combinations.
Architecture passes 47 modules, 6 programs, 33 layered modules and 6 fixtures.
Formatting and whitespace checks pass. The per-frame diagnostic has intentional
GPU readback and disk-write costs; these runs are not performance measurements.

Pixel continuity is still failing. A scan of the actual captured BMPs found fully
black runs after the player camera became active: 16 frames on first-person entry,
29 on return, and 36 with a trailing eye after body admission. The runs occur at
both authored world rates. `portal-eye-retention-frame-audit.json` records the
exact frame ranges, and `portal-eye-retention-contact-sheet.png` shows the failure.
The renderer is asked to draw a foreign whole-eye view before its image exists.
Fix image readiness and retention across eye-world changes and admission, then
make the frame-level pixel check an acceptance assertion. Continue the moving-eye
and Humanoid-subject checks through the missing-rig gap as well. Also trace
the presentation refusals and missing content-publisher diagnostics recorded
during its otherwise successful handoff. Queue-full presentation refusals have
since been addressed by the bounded request scheduling above; missing publisher
diagnostics remain open. Release cost remains unmeasured.

The successor now opens its own authenticated presentation stream and discovers
routes while staged. It advertises an empty endpoint directory until adoption,
so it cannot claim the active connection's reply endpoints twice. Adoption moves
that stream, preserving its ordered receive sequence, and replaces the producer
directory directly with the successor's authenticated directory. Unchanged
producer incarnations survive; omitted or changed endpoints still retire through
the existing route validator. A malformed staged stream prevents adoption.

Whole-eye images now belong to the client's persistent local world, with two
dedicated draw/reply slots separate from the current body replica and its portal
surface slot. The current eye and one neighbour are requested near a portal;
neighbour prefetch is limited to one aperture diameter and two resident slots.
The current world's image is also warmed while a successor is staged. Requests
resolve authored names to the advertised producer instead of a transient replica.
The work is covered by `client portal eye` profiling and the existing request,
transfer and residency counters. Release cost is still unmeasured.

An intermediate slot mismatch made the image handles valid while every foreign
eye remained black. That was corrected by keeping the image's owning world and
draw slot together. The product fixture now reads the bottom-centre floor pixel
in every frame claiming a ready eye image, so a handle alone cannot pass.
`portal-warm-eye-final-walk.log` passes 9,148 assertions across all four walks;
`portal-warm-eye-client-full.log` passes 9,278 / 164. The first attempted build of
the pixel assertion failed Catch's expression decomposition check; its boolean
expression was parenthesised and the successful build was tested afterward.

The earlier warm run reduced black intervals to 2 and 15 frames for
30 Hz first person, 13 for 30 Hz trailing, 5 for 60 Hz first person, and 13 plus 2
for 60 Hz trailing (`portal-warm-eye-frame-audit.json`). This is not a continuity
pass. The 30 Hz first-person sequence also shows a more serious routing error:
frame 150 enters `server.world`, frame 151 crosses back on a sub-millimetre input
correction, and frame 599 still names `walk.destination` despite the returned
body being in the source replica. Reproduce the receiving-mouth jitter and retain
the entry-side evidence across frames, without preventing deliberate reversal.
Then require the final eye world and remove every pending-image black interval.

That guarded build passed 9,193 assertions in
`portal-warm-eye-verified-walk.log`; assertion count varies with the number of
ready eye frames. Its full frame scan still finds 2, 13, 5, and 13 plus 2 black
frames in the four cases. The 30 Hz first-person eye still ends in the wrong world.
`portal-warm-eye-verified-camera-transitions.json` preserves the camera, root and
world metadata at each transition and at the final frame.
`portal-warm-eye-verified-frame-audit.json` records the black ranges and final
worlds. The matching contact sheet was inspected. Architecture, formatting and
whitespace checks passed. The subsequent fixes and remaining failures are below.

The receiving-mouth jitter now has a deterministic scene test. Its pre-fix run
failed 3 of 6 assertions. Camera arrival retains the receiving side and a tolerance
derived from wire position error and portal scale until the eye clears the mouth.
Slow deliberate reversal still crosses back, including after body admission
rebases the input camera. Rotated frames and scales 0.25, 1 and 3 are covered.
The full Scene suite passes 516,899 assertions / 544 cases in
`portal-arrival-scene-full.log`. The product fixture now requires the final eye
world to be `server.world` in all four walks.

Topology reply endpoints now belong to the persistent viewport world instead of
the retiring body replica. The current eye producer's topology is prefetched too,
and the two image slots fill empty entries before evicting a retained neighbour.
The CPU route test checks topology survival after replica destruction for both
persistent and replica-owned endpoints (`portal-topology-owner-unit.log`,
144 assertions). This removed the long admission gap, but request traces still
showed requests lost with the old connection waiting for their one-second timeout.

After authenticated connection replacement, image and topology requests now
restart with new correlations. Already received images and topology retain their
original expiry; restarting does not prolong stale data. Late image replies cannot
satisfy replacement requests. Focused restart tests pass 27 assertions / 2 cases.
The full Render and Client suites pass 32,281 / 436 and 9,353 / 164 respectively
in `portal-request-restart-{render,client}-full.log`.

The latest offscreen product walk passes 8,174 assertions across all four cases
(`portal-request-restart-walk.log`). A separate RGB scan checks 531, 531, 531 and
529 frames after initial Humanoid binding. All final eye worlds are correct.
No return black interval remains in this recording. The 30 Hz first-person entry
still has two fully black frames, 81 and 82; the other three sequences have none.
`portal-request-restart-frame-audit.json` records the scan, and
`portal-request-restart-entry.png` was visually inspected. The eye enters the
destination at frame 81 and its first whole-eye image appears at frame 83.
Resolve this cold-entry readiness gap before promoting every-frame pixels to a
continuity assertion. Continuous movement and Humanoid subject preservation
during the missing-rig gap, failure-path walks and release cost remain open.

Cold-entry transport tracing found that requests already began before movement.
In `portal-producer-latency-walk.log`, the first destination image took 93 ms to
reach its producer, 19 ms there, and 186 ms to reach the viewer afterward. Each
128 x 128 RGBA16F reply carried roughly 131 KB. These are diagnostic dev timings,
not release performance claims. Moving the prefetch distance would not make this
spawn case request earlier.

Portal exchange version 7 now compresses copied image pixels losslessly with
Zstd level 1 only when smaller. Scratch is capped at raw payload size, tiny and
incompressible payloads retain raw encoding, and retrying a full transport still
reuses the already encoded message. Decode validates dimensions and a single
frame's declared output size before allocating; it then verifies finite HDR
samples and the original digest. The inbox charges expanded bytes, including
the old image while replacement decode is pending. Resident image receipts
continue to carry metadata only. Codec profiling scopes and raw/wire byte
counters expose the added CPU work and transferred bytes.

The first compressed codec run failed the old version-byte expectation, which
was updated to 7. The first product run also exposed a missed metadata-matcher
check that still required raw encoding. A matching compressed reply is now
required by the codec test. The full Render suite passes 32,650 assertions / 437
cases, including compressed HDR parity, truncation, corrupt metadata/pixels,
incompressible fallback and expanded replacement accounting. The full Client
suite passes 9,353 / 164 (`portal-compressed-{render,client}-full.log`).

The corrected compressed walk passes 8,192 assertions in
`portal-compressed-match-walk.log`. Its first whole-eye reply is 7,038 bytes
instead of 131,200, and reaches the viewer in 199 ms instead of the earlier
298 ms. The adjacent surface reply is 650 bytes instead of 131,209. These are
specific captures, not guaranteed compression ratios or latency bounds.
All final eye worlds are correct and the first-person sequences have no black
frames. The 30 Hz trailing sequence has two source-world black frames, 100 and
101, while the eye is still several metres from the seam. The matching image
sheet `portal-compressed-surface-gap.png` was inspected. This is a separate
portal-surface continuity failure, recorded in `portal-compressed-frame-audit.json`.
The product test now requires an image on every foreign-eye frame, so missing
replies cannot escape the existing ready-image pixel check.

The final build restores producer diagnostics to trace level. Its focused
codec/inbox/restart subset passes 2,028 assertions / 19 cases, and the strengthened
product walk passes 8,508 assertions (`portal-compressed-final-{unit,walk}.log`).
All 289 foreign-eye frames have images; all four final eye worlds are correct.
The independent scan still finds two source-world black frames, 97 and 98, in
the 30 Hz trailing case (`portal-compressed-final-frame-audit.json`). The other
three sequences have none. Architecture, formatting and whitespace checks pass.
The subsequent source-surface investigation is below; release CPU/GPU cost and
failure-path continuity remain unverified.

Per-frame diagnostics now record each portal's imported image handle, key and
aperture geometry. The black source frames retained a nonzero image handle until
the next reply. Temporary raw-pixel inspection confirmed the earlier capture's
floor occupied only a narrow band, with nearer pixels clipped away. The temporary
dump hook was removed; its diagnostic artifacts remain under
`portal-surface-inputs/` in the test build directory.

Imported portal sampling now intersects the current eye ray with the authored
seam plane before projecting into the received image. Previously the pane's
visible box face was sampled directly, despite the capture clipping at another
face of the slab. The correction uses the existing per-draw matrix in primary,
recursive and mixed-surface passes, with no new texture or shader pass. Its CPU
test covers a different capture eye, both slab faces, rotated planes and scales
0.25, 1 and 3 (`portal-slab-scaled-unit.log`, 126 assertions). The first build
failed because one include landed after its use; that placement was corrected.

Sampling correction alone left one black frame. Camera metadata identified the
other cause: the network player inherited the unrelated local demo's fitted
17.1-metre far plane. The standalone Client now starts the network viewer with
its own default lens. A bound character retains its camera, and accepted portal
continuations still carry their lens explicitly. The product fixture requires
the default 500-metre far plane throughout its Humanoid samples and both handoffs.

The full Render suite passes 32,692 assertions / 438 cases before the additional
scale variants; the full Client suite passes 9,353 / 164. Portal and lighting GPU
fixtures pass 7,189 assertions / 2 cases. The image runtime/host GPU run initially
failed 32 instances of an old raw-byte traffic assumption after lossless compression.
It now checks transmitted reply bytes alongside expanded CPU image storage;
the rerun passes 4,369 / 17 (`portal-slab-import-fixtures-fixed.log`). No pixel
comparison in that run failed.

The product fixture now rejects a wholly black image on every frame after initial
Humanoid binding. The final walk passes 16,604 assertions and all 2,123 checked
frames contain scene pixels (`portal-visible-frame-walk.log`). Its 284 foreign-eye
frames have images, all four cameras keep the 500-metre far plane, and all final
eye worlds are correct. `portal-visible-frame-audit.json` records the independent
scan. The inspected `portal-player-lens-contact-sheet.png` still shows a thin dark
strip on the source-side floor in one frame of each trailing-camera sequence.
This is not an all-pixel continuity pass. Resolve that remaining surface edge,
then verify moving-eye and Humanoid-subject continuity through the missing-rig
gap, failure-path walks and release CPU/GPU/residency cost. Architecture,
formatting and whitespace checks pass.

The camera-subject audit confirms a real missing-rig interval on both handoffs:
the source camera's Humanoid target becomes null for 10 to 12 frames before
destination adoption. The old walk test injected mouse motion only when the
character's LocalScript could report its root, so it stopped input exactly during
that interval. Mouse events now follow captured frames, including the gap, and
metadata records controller angles and basis separately from the displayed pose.

This reproduced eight control-yaw resets across the four walks: adoption lost
0.070 to 0.084 radians of accumulated turn (`portal-gap-input-before.log`,
8 failed assertions). A pending automatic camera now keeps copying its controller
after the source rig retires, provided it already had a valid continuation and
Proceed was sent. Explicitly changed subjects are excluded. This carries owned
camera state only; it does not recreate a source body or grant destination control.

The updated walk passes 18,732 assertions (`portal-gap-input-fixed.log`), and the
full Client suite passes 9,353 / 164 (`portal-gap-input-client-full.log`). Maximum
per-frame control yaw change is now 0.007 radians in every case. No checked frame
is wholly black, and all final eye worlds are correct. Architecture, formatting
and whitespace checks pass.

This is an input-preservation fix, not subject or displayed-pose continuity.
`portal-gap-input-fixed-audit.json` still records 24, 24, 22 and 23 frames without
a Humanoid subject across the two handoffs of each case. The displayed eye is
frozen on those frames and catches up by as much as 0.091 radians at adoption.
Next preserve the presentation subject until its accepted replacement is ready
and advance the displayed camera through that interval. Verify actual displayed
pose separately from controller state, including manual Humanoid bindings,
translated/rotated/scaled worlds and refusal/reconnect paths. Keep this requirement
open until the camera remains attached and responsive throughout the handoff.

#### Source retirement: implementation constraints and acceptance gates

Explicit-Humanoid product coverage found an earlier blocker: the LocalScript
could not assign `CameraSubject` on the client's predicted camera. The store
refused all runtime replica property writes. The four explicit walks failed
before movement (`portal-explicit-before.log`). Properties now have a default-off
`PredictedWritable` allowance, enforced by `Store::SetProperty` together with
live predicted ownership. Only `CameraSubject` opts in. Its setter changes the
local camera selection and only reads the referenced target. Authority-owned
cameras, unapproved properties, invalid targets and dead instances remain refused.

With that assignment working, all eight explicit-camera handoffs lost mouse
turn at adoption (`portal-explicit-input-before.log`, 8 failed assertions).
Source camera and subject handles now identify the provenance of the copied
continuation within the retiring replica. They never cross to the successor.
Capture continues only from that camera after its captured subject has died.
Automatic follow clears the raw subject handle; explicit selection retains a
dead raw handle while its public property reads null. The first guard assumed
both were cleared and failed the same eight checks
(`portal-explicit-null-guard.log`). The corrected guard accepts those two
retirement forms and excludes an explicit null or another selected camera.

The expanded product walk passes 41,828 assertions across all eight rate, camera
and binding combinations (`portal-explicit-input-fixed.log`). Every final eye
returns to `server.world`; all checked frames contain scene pixels. The independent
metadata audit records a maximum control-yaw step of 0.007 radians in each case
(`portal-explicit-input-fixed-audit.json`). The full ECS suite passes 134,151 / 443,
Scene 516,911 / 545 and Client 9,353 / 164. Architecture, formatting and whitespace
checks pass. Logs are under `.cache/build/dev/tests/` with the
`portal-local-property`, `portal-local-camera` and `portal-explicit-input` prefixes.

This closes explicit assignment and accumulated input loss, not presentation
subject lifetime. The audit still records 24 missing/frozen frames per 30 Hz walk
and 22 per 60 Hz walk, across its two handoffs, for both automatic and explicit
bindings. Retaining a live subject, advancing its displayed pose, and verifying
camera overrides during cancellation/reconnect remain open. No release timing
or sanitizer result is claimed for this change.

The next change must cover both retirement paths. `Replica::Apply(Structure)`
destroys the listed rows immediately. `ecs::ApplySnapshot(Authoritative)` also
destroys authoritative rows omitted from a replacement snapshot. A callback on
the first path alone cannot guarantee camera continuity. The source transfer
removes the character and then its Player after sending the physical Commit;
delaying that authoritative removal until a client's session acknowledgement
would change the ownership protocol and is not a camera fix.

`Store::ClonePredictedInstance` is not yet a suitable handoff primitive:

- `CloneSubtree` skips `NotArchivable` instances, including any marked child.
  Transfer presentation must not depend on an author's editor-cloning flag.
- `CloneOne` copies component rows, including script-bearing instances. Giving
  a cloned Player to `LocalPlayer` can make its new LocalScript identities
  eligible for `ClientScriptsIn`; preserving the GUI and running script state
  must be tested rather than assumed.
- `CloneInstanceInRange` remaps writable reflected references within that one
  subtree. The Player and character are separate trees, linked by
  `PlayerCharacter::Model` and `Character::Owner`; camera and prediction
  resources also refer to the original entities. Cloning does not establish a
  complete presentation identity map by itself.
- Predicted indices survive authoritative snapshot omission, but that only
  establishes storage lifetime. It does not establish valid subject bindings,
  script lifetime, current destination motion or correct camera obstruction.

Use a bounded presentation representation with explicit identity and lifetime.
Its exact storage/API remains to be selected and validated; do not add a generic
replica retirement callback before both snapshot and structure paths are covered.
Do not pause the whole source replica or retain its authoritative indices:
unrelated objects must keep updating and those indices can be reused.

Build and verify in this order:

1. Specify the retained rows and references needed by the existing player-camera
   path. State how `LocalPlayer.Character`, its Humanoid, the camera target,
   prediction root and script-visible references remain consistent. Capture the
   last presented/predicted pose, not an older authoritative pose. Require no
   second physics owner, duplicate drawn body or duplicate script startup.
2. Establish retention before either form of replica retirement. Prove that
   destruction of the old Player, model, Humanoid and root in any received order,
   a full snapshot, and immediate authoritative-index reuse cannot detach the
   retained subject or attach it to another player. Explicit camera changes and
   Scriptable mode must keep the author's choice.
3. Advance the retained presentation from destination motion and current input.
   Define the input route and tick acknowledgement while the successor is not
   yet adopted. A rotating eye around a frozen root is not continuous walking.
   Keep camera-arm obstruction in the eye's mapped collision space and preserve
   seam scale, roll, clipping distances and the receiving-mouth exclusion.
4. Replace the representation only with the exact accepted transfer receipt's
   Player and complete rig. Release it on successful adoption and cancellation;
   after physical Commit, reconnect without recreating source authority. Preserve
   valid presentation across a successor retry and retire stale callbacks by
   attempt and world incarnation.
5. Extend the existing product walk, rather than introducing another smoke
   fixture. Check actual Humanoid/root identity on every post-binding frame;
   compare displayed orientation with delivered pointer input independently of
   controller angles. Check root/eye displacement, GUI/script lifetime and body
   counts through both handoffs. Repeat automatic and explicit Humanoid bindings,
   first/third person, 30/60 Hz, non-identity seams and refusal/reconnect cases.

Keep work proportional to the affected rig: allocate retention once per handoff,
reuse its storage during frames and release it deterministically. Record copied
rows/bytes, live/peak bytes and handoff allocations at their real boundaries.
Do not copy a world or rebuild image targets to preserve a camera subject.
Retain existing resident image slots and valid producer caches; invalidate only
the affected seam, endpoint incarnation or view history. Measure release CPU
busy/wait and GPU time separately after the continuity checks pass. The existing
1/2/8-view workload remains the cost gate, not the instrumented capture fixture.

Baseline recheck: the existing headless `[camera-continuation]` suite passes
(`.cache/build/dev/tests/portal-retirement-camera-baseline.log`). This checks
copied camera application and seam transforms; it does not close the measured
missing-subject or frozen-display gap. No new retention implementation or release
performance result is claimed by this refinement.

Capture metadata now separates the handshake from rig availability. The eight
walks pass 41,835 assertions (`portal-handoff-stages-walk.log`), and
`portal-handoff-stages-audit.json` records all sixteen gaps. At the fixture's
60 Hz presentation cadence, the destination first contains a character six to
eight frames after the source subject disappears. The exact accepted rig is
available eight to ten frames after disappearance; adoption follows two frames
later. At 30 Hz, a destination character can exist two frames before Crossed and
four before Ready. Character count alone therefore cannot authorize a camera
binding, and eliminating the final session acknowledgement wait cannot close
the initial no-rig interval. These are instrumented dev capture observations,
not release performance measurements.

The bridge must start before `Client::PollServer` drains the source connection,
so both structure deletion and full snapshot replacement are covered. Retain
the last presented prediction in source coordinates, then reconcile with owned
destination pose data after the exact receipt resolves. `RecordReplicatedTick`
and `ReconcileLocalPlayerPrediction` currently run immediately after the source
poll; the latter clears prediction when `LocalPlayer.Character` is absent.
Retaining only a camera target would leave that motion path broken. Destination
`Server::ApplyMove` also ignores unassigned peers until session Commit populates
its player map. Input routing and acknowledgement must be addressed with the
retained subject; replaying unaccepted input as if the destination applied it
would conceal a body/camera divergence.

Movement now has a source-host bridge through the existing named portal bus.
`game::ApplyMoveInput` retains the newest validated source-space direction and a
jump counter in the exact outgoing transfer receipt, including changes during
preparation and after source retirement. The destination rotates direction into
its seam space and applies it to that receipt's admitted Humanoid. Acknowledgements
follow application; duplicate or reordered packets cannot replay a jump. The
first native destination input closes the old route. Source disconnect submits
zero movement, and precommit refusal restores the newest input with source authority.

Storage remains bounded by the existing 64 receipt records, with one latest move
per receipt. Open player routes cannot be evicted to admit another transfer.
Closed routes can retire; their existing peer retirement history answers late
movement with closure. Pending states retry through the normal bus budget, and
acknowledged unchanged movement produces no new message. Snapshot codecs retain
movement, jump/application counters and route closure. This is input-state
acknowledgement between hosts, not a client prediction-tick acknowledgement.
Mapping those acknowledgements into retained prediction remains open.

Verification under `.cache/build/dev/tests/`:

- `portal-move-script-full.log`: 1,717 assertions / 84 cases, including separate
  processes, snapshot replay before and after closure, lost acknowledgements,
  reordered movement, refusal and live-route pressure through 65 transfers.
- `portal-move-game-full.log`: 3,059 / 88, including the shared host movement API.
- `portal-move-server-full.log`: 1,406 / 76.
- `portal-move-client-full.log`: 9,353 / 164.
- `portal-move-product-walk.log`: 41,832 assertions across all eight existing
  rate/camera/binding combinations. Both physical handoffs complete in each.
- `portal-move-architecture.log`: 47 modules, 6 programs, 33 layered modules.
  Formatting and whitespace checks pass.

`portal-move-camera-audit.json` still records 22 to 24 missing-subject/frozen
frames across each round trip's two handoffs, with a 0.007-radian maximum
controller-yaw step. Inspection of `portal-move-handoff-contact.png` confirms the
frozen source view and its visible change at adoption. The movement bridge does
not close camera continuity, script/GUI lifetime, collision-space camera checks,
restart recovery, or the release profiling gate. No sanitizer or release timing
result is claimed for this change.

The product check now verifies input submissions during Proceed and source
retirement, using `submitted_move_tick` in capture metadata. This exposed a
remaining `SubmitMove` early return: the host bridge worked, but the client
stopped submitting after Proceed. `portal-client-input-before.log` fails at
frame 51 with `50 > 50`. The client now keeps submitting through its authenticated
source connection even while the source rig is absent.

Restoring submissions exposed excessive prediction and one black frame in the
expanded eight-walk run (`portal-client-input-all-walks.log`: 42,178 / 42,179
assertions passed). In the failed capture, the received root was near z=-3.73
while prediction reached z=-14.26. A temporary smaller near plane reproduced the
same frame failure (`portal-client-near-probe.log`) and was removed.
`Connector` had been discarding inputs by the replica's world tick, even though
`Client::SubmitMove` uses the client's own tick. Their origins and rates differ.

Protocol 12 carries the host's highest consumed client input tick in each delta.
The replica accepts it only with a complete update and rejects inconsistent
parts. The client echoes the consumed tick independently of its world/snapshot
acknowledgement. A quiet world retries a pending input acknowledgement within its
normal message/byte budget and stops when that consumed tick is echoed. A
replacement snapshot therefore cannot falsely confirm an earlier lost input
acknowledgement. Reconciliation replays the connector's remaining input list
without comparing client ticks with world ticks. Both programs must use the
matching protocol build.

Headless coverage includes younger and older client clocks, multipart updates,
lost acknowledgements, snapshot replacement and both QUIC and authenticated
datagram connections. This confirms consumption by the current host, not a
forwarded move's application to a destination pose. Retained prediction still
needs that destination acknowledgement associated with the exact transferred
body baseline, including partial replication budgets and successor retries.
The shot rewind consumer is now separated from the input clock; see the shot
view-time validation below. This does not establish cross-portal shot routing.

Final validation uses the protocol-12 build:

- `portal-input-clock-final-unit.log`: 44 assertions / 2 cases, including both
  transport modes and the snapshot/acknowledgement case.
- `portal-input-clock-final-replication.log`: 21,813 / 270.
- `portal-input-clock-final-client.log`: 9,356 / 165.
- `portal-input-clock-final-server.log`: 1,406 / 76.
- `portal-input-clock-final-walk.log`: 42,170 assertions across all eight visual
  round trips. The stronger input-continuity and existing image checks pass.
- `portal-input-clock-architecture.log`: 47 modules, 6 programs, 33 layered
  modules. Formatting and whitespace checks pass.

`portal-input-clock-final-audit.json` records 340 advancing handoff-input samples
with no stopped submissions. Maximum predicted/received root distance is 1.6 to
1.87 metres in these fixtures. Each eye returns to `server.world`, but 22 to 24
missing-subject frames remain per round trip. The inspected
`portal-input-clock-final-contact.png` still shows the held view changing at
adoption. No retained character chain was added in this change, and no sanitizer
or release performance result is claimed.

Retained camera-character progress:

`CameraCharacterHold` now owns four predicted instances in the source replica:
Player, Model, root and Humanoid. The client prepares them before draining the
connection, covering both structure retirement and authoritative snapshot
replacement. Activation remaps LocalPlayer, Character, Humanoid.RootPart,
the unchanged camera subject and prediction to those local handles. Preparation
refreshes the last presented prediction while the source rig remains intact.
The root carries no Visual, Collider, RigidBody, Motion or Simulated component;
no script subtree is cloned. Release removes the four instances on replica drop
or refusal and preserves a newer explicit camera selection.

Source replication updates no longer reset retained prediction to the held
root's initial pose. A headless client test advances ten movement steps across
unrelated authoritative ticks and checks that the retained root's stored pose
remains presentation-only. The client activates the hold only when it has a
prediction to remap. Retention allocates once per handoff and reuses its rows;
actual allocation/byte counters and release profiling remain outstanding.

Validation under `.cache/build/dev/tests/`:

- `portal-camera-hold-walk.log`: 46,827 assertions across all eight product
  round trips. Every frame after initial binding now requires a Humanoid target.
- `portal-camera-hold-audit.json`: zero missing-subject frames in all eight
  captures. Retention spans 20 to 24 frames per round trip. The audit also records
  prediction steps above 0.6 metres within the same input world; these remain
  evidence to investigate, not an assertion of smooth motion.
- `portal-camera-hold-client-unit.log`: 9,400 assertions / 166 cases.
- `portal-camera-hold-scene-full.log`: 517,048 / 547 before the final selection
  coverage extension; `portal-camera-hold-selection.log`: 175 / 2 afterward.
  This covers root, Humanoid, Player, model and Character-link retirement,
  snapshot omission, explicit other/null subjects, replacement camera and
  Scriptable selection. Immediate authoritative-index reuse still needs coverage.
- `portal-camera-hold-architecture.log`: architecture passes. Formatting and
  whitespace checks pass. No sanitizer or release performance result is claimed.

The inspected `portal-camera-hold-contact.png` still shows a visible change at
adoption and a close arriving body in first person. Four retained identities do
not preserve player GUI/script lifetime or drawn limbs. Final adoption also needs
coverage for a new explicit null/other camera choice. Keep the continuity task
open until destination motion/application acknowledgement, exact accepted pose,
body visibility and those lifetime checks pass together. The former 22 to 24
missing-subject frames are fixed; full seamless presentation is not yet proven.

Adoption-pose correction requirements:

`portal-retained-motion-walk.log` passes 47,614 assertions across all eight
scenarios. The corrected metadata distinguishes retained storage from authority;
173 retained-input steps advance, with zero missing subjects.
`portal-retained-motion-audit.json` records all sixteen adoptions: none has a
successor prediction on the adoption frame, and same-eye-world camera steps span
1.60 to 2.93 metres. These captures reproduce the remaining continuity defect;
the passing walk does not yet gate its magnitude. Build, formatting and whitespace
checks pass. This instrumentation run is not a release performance measurement.

Capture inspection reproduces a 1.60 metre backward eye step on a first-person
adoption. `Client::PumpPortalSuccessor` maps the camera continuation but installs
no successor `LocalPlayerPrediction`. The next camera placement follows the
received destination root, replacing the advancing retained pose. Copying the
prediction at adoption alone would move the same correction to the next
`ReconcileLocalPlayerPrediction` call.

The movement bridge now retains `Input.Tick` from `Server::ApplyInputs` through
`ApplyMove`, `game::ApplyMoveInput` and the exact transfer's `ForwardMove`.
`ApplyForwardMove` records that stamp when assigning Humanoid control in
PreSimulation and returns it in MoveAck. `PortalTransferReceipt::AcknowledgedInputTick`
exposes the latest acknowledged stamp at the source. This is not a post-simulation
pose acknowledgement and cannot be interpreted as a count of client movement
steps integrated by the destination.

Newer stamped input advances even when direction is unchanged; one latest state
per receipt is sent through the existing per-tick pump. Repeated or older stamped
input cannot overwrite that state or repeat a jump. Unstamped host/local calls
remain available with tick zero, including disconnect stop. The bus envelope is
PPT3 so an older movement body cannot be misread as the new format. The originating stamp adds eight bytes to Move and control acknowledgement
bodies; the completed sample adds a presence byte and 85 bytes when present. Receipt, pending stamp and applied stamp survive
same-build snapshot restore; this does not add an unbounded input queue.

Stamped bridge validation under `.cache/build/dev/tests/`:

- `portal-input-stamp-script.log`: 1,403 assertions / 22 transfer cases, including
  process delivery, snapshot replay, lost acknowledgements and delayed movement.
- `portal-input-stamp-game.log`: 3,059 / 88.
- `portal-input-stamp-server-final.log`: 1,414 / 76. The product round trip sends
  client tick 900000 after source retirement and requires its acknowledgement
  through the exact source transfer receipt, over both wire modes and world rates.
- `portal-input-stamp-client-unit.log`: 9,400 / 166.
- `portal-input-stamp-walk.log`: 47,568 assertions across all eight visual walks.
  Existing subject, retained movement and image checks pass; adoption displacement
  remains outside their gate and is not claimed fixed.
- `portal-input-stamp-architecture.log`: architecture passes, alongside build,
  formatting and whitespace checks. No sanitizer or release profile was run.

The first broad server run failed two process launches with `Permission denied`
while the server executable was being relinked. The final server run above took
place after all builds ended and passed. Future process tests must not overlap
relinking any executable they launch.

The destination now publishes a completed motion sample in the replication
phase, after physics and animation. `PortalTransferReceipt::Motion` carries the
destination incarnation, completed world tick, originating input stamp, root
pose, linear/angular velocity, walk/jump speeds and grounded state. No destination
entity handle crosses the bus. The PPT3 MoveAck carries that sample atomically;
finite-value, quaternion, boolean, incarnation and input-frontier checks happen
before the source receipt changes. Older destination ticks cannot replace the
latest accepted sample.

One cached sample per incoming/outgoing receipt stays within the existing receipt
bounds. Unchanged poses and control state reuse the cache; a repeated request
still obtains a reply. Failed enqueue keeps the reply pending, and existing Send
metrics count actual encoded bytes/messages. Snapshot codecs preserve the cached
sample and pending reply. The authenticated source session now relays each newer
sample to the owning client as `PortalSessionKind::Motion`, carrying the exact
resume claim and attempt. Send failure leaves the destination tick eligible for
retry. The shared script codec handles both bus and play-session samples; there
is no second pose grammar.

The client retains a sample only during its active Proceed attempt, for the
identical claim, and with an input stamp no newer than its submitted input.
Destination ticks must advance and input stamps cannot regress. Capture metadata
exposes this sample under `portal_handoff.completed_motion`. Adoption still does
not reconcile with it.

`portal-client-motion-audit.json` records 63 captured sample frames across the
eight round trips, with both destinations represented in every run. Samples
arrive 11 to 13 client input ticks behind presentation in this instrumented
fixture. Comparing each completed pose with the historical prediction bearing
that input stamp yields gaps from under a millimetre to 1.07 metres; this is
stronger evidence than comparing poses from different times, and does not prove
prediction parity. The separate retained history below now survives source
consumption, which retires inputs from the ordinary unconfirmed queue before
the completed destination sample arrives. Missing history is never inferred
from velocity, and client ticks are not destination world ticks.

Completed-motion validation under `.cache/build/dev/tests/`:

- `portal-completed-motion-delivery-tests.log`: 1,426 assertions / 23 transfer
  cases, including real physics pose matching, process delivery, snapshot restore
  and acknowledgement loss/retry.
- `portal-completed-motion-malformed.log`: 78 / 2 after the final test extension.
  Invalid grounded flags, sample incarnations, future input stamps and zero
  quaternions cannot change the receipt or advance its acknowledgement. A valid
  retry recovers. The physics case compares pose, velocities, grounded state and
  movement speeds with the destination state recorded at the sample's exact tick.
- `portal-completed-motion-server.log`: 1,414 / 76, requiring the completed
  tick-900000 sample through the product route at both wire modes and world rates.
- `portal-completed-motion-client.log`: 9,400 / 166.
- `portal-completed-motion-walk.log`: 47,569 assertions across all eight visuals.
  The existing continuity checks pass; the adoption-displacement defect remains.
- `portal-completed-motion-architecture.log`: architecture passes; build,
  formatting and whitespace checks pass. No sanitizer or release profile was run.

The initial pressure test assumed closure was the last acknowledgement byte.
Its updated assertion verifies closure followed by an absent optional sample.
The original failure is preserved in `portal-completed-motion-script.log`.

Client sample-delivery validation under `.cache/build/dev/tests/`:

- `portal-client-motion-game.log`: 3,420 assertions / 89 cases, including motion
  round trips, every truncated prefix, invalid incarnations and nonfinite state.
- `portal-client-motion-server-final.log`: 1,426 / 76, including delivery to the
  owning source connection with the exact attempt, claim and tick-900000 sample.
- `portal-client-motion-unit.log`: 9,400 / 166.
- `portal-client-motion-walk.log`: 47,816 assertions across all eight visuals.
  Both handoff legs require an accepted completed sample, with no future input
  stamp. Existing subject, input movement and image checks remain enabled.
- `portal-client-motion-architecture.log`: architecture passes. Build, formatting
  and whitespace checks pass. No sanitizer or release profile was run.

The initial server test expected exactly four total replies and counted the new
motion update as a fifth. `portal-client-motion-server.log` preserves the failure;
the corrected fixture requires four control replies and independently validates
motion delivery. The final full suite above passes.

#### Retained input replay and adoption evidence

The client now records successfully submitted moves and their actual prediction
durations in a 1,024-entry ECS ring scoped to the exact resume claim. Source
consumption cannot retire this history. A completed destination sample replays
later entries in destination coordinates, then maps the presentation back to
the retained source rig. The authoritative root is untouched. Samples older than
retained coverage, repeated destination ticks and mismatched claims are refused.
Snapshot restore resets local transient history; authoritative snapshots preserve
the active client history. Refusal and replica teardown release it.

The ring is bounded, with no per-submission allocation or full-history copy.
Profiler scopes cover begin and replay; a byte counter records actual entry
copies. These are instrumentation, not a measured release performance claim.

Validation under `.cache/build/dev/tests/`:

- `portal-input-replay-client.log`: 9,439 assertions / 167 headless cases.
- `portal-input-replay-walk.log`: 49,333 assertions across all eight visual
  scenarios. Both legs require replay and no history overflow.
- `portal-input-replay-reset-test.log`: 40 assertions / one focused case after
  rejecting recording into reset, inactive history.
- `portal-input-replay-direction-before.log`: two failed direction assertions
  reproduced an extra inverse rotation when a sample covered all retained input.
  Replay now maps the held direction into destination coordinates before the
  inverse mapping, including when there is nothing left to replay.
- `portal-input-replay-direction-after.log`: all 42 focused assertions pass.
- `portal-input-replay-client-final.log`: 9,442 assertions / 167 headless cases
  after the inactive-history guard and direction fix.
- `portal-input-replay-walk-final.log`: 49,342 assertions across all eight
  visual scenarios after those fixes. Existing continuity gates pass; the
  adoption displacement remains outside those gates.
- `portal-input-replay-architecture.log`: architecture passes.

The capture audit `portal-input-replay-audit.json` contains 64 replayed frames
and 16 adoptions. None of the adoption frames has prediction. Same-world eye
steps at adoption range from 1.598708 to 2.399739 metres. The inspected
`portal-input-replay-contact.png` also shows third-person view changes there;
the flat first-person fixture offers weaker visual motion evidence. Existing
image and subject gates pass, but this does not close seamless movement.

That audit preceded adoption carry. `Client::PumpPortalSuccessor` now captures
the held presentation under the exact claim and maps its values through the
seam before dropping the source replica. The copy contains no entity handles.
`AdoptPortalPrediction` resolves the accepted Player/rig and keeps destination
Humanoid references; only its presentation movement state is replaced. The
authority root remains untouched. `ReconcileLocalPlayerPrediction` ignores an
authority tick older than the carried baseline, while newer authority state
still corrects normally. No second pose history or retained source authority is
introduced. The carry's tick fields identify its baseline and replay frontier,
not a newly completed physics sample.

Adoption-carry evidence under `.cache/build/dev/tests/`:

- `portal-prediction-adopt-unit.log`: 55 assertions / one case, including scaled
  mapping, local destination references, unchanged authority rows, older-update
  rejection, newer-update correction and invalid-player refusal.
- `portal-prediction-adopt-client.log`: 9,455 assertions / 167 headless cases.
- `portal-prediction-adopt-walk.log`: 49,319 assertions across all eight visuals,
  now requiring prediction on adoption. Build, architecture, formatting and
  whitespace checks pass. No sanitizers or release profile ran.
- `portal-prediction-adopt-audit.json`: prediction exists on all 16 adoption
  frames. Same-eye-world camera steps there range from 0.027988 to 0.540353
  metres. Within the next eight frames, the largest step is still 2.399861
  metres. In the 30 Hz first-person case, correction occurs two frames after
  adoption, with 1.066502 and 1.333183 metre steps on the two legs.
- Inspected `portal-prediction-adopt-third-{0,1,2,3}.png`: the third-person
  fixture still switches visibly from a body/floor view to foreground wall
  coverage at adoption. Camera displacement alone cannot prove seamless image
  composition or retained body/limb continuity.

The remaining change must cover the first native correction and the image
composition through that interval. Keep each connection's acknowledgements
distinct even when the Client preserves one input timeline. Do not acknowledge a complete
pose using only the replica-wide consumed-input frontier when the root may have
been deferred by the replication budget. Add displacement gates on both frames
before claiming continuity. Sanitizers and release profiling remain unrun.

#### Native connection pose delivery

`game::PlayerMotion` now carries the destination connection's own Player/root
handles and one completed pose using the existing motion codec. Its authenticated
connection defines the input session. `PlayMessage::PlayerMotion` is distinct
from the old source connection's receipt-bound `PortalSessionKind::Motion`;
their input clocks must never be merged. Malformed, truncated or trailing bytes
leave the decoded output unchanged. Zero input frontiers and missing identities
are refused.

`Authority::StatusOf` exposes its existing consumed-input frontier. Server reads
that value while publishing the completed world, before applying the next input
batch. `CapturePlayerMotion` packages that tick's root, velocity and Humanoid
movement state as one message, independent of component replication budgets.
It resolves entities inside the destination store and retains no world pointers.
Only the owning generation-checked connection receives the sample. The send
queue's accepted world tick is cached on its connection/player record; refusal
leaves the next publication eligible to send the latest completed state. Actual
encoded bytes are counted by `server.player.motion.bytes`. No native-input state
was added to the bounded portal receipt table, which may evict closed receipts.

Initial validation under `.cache/build/dev/tests/`:

- `native-player-motion-codec.log`: 15 assertions / one codec case, including
  every truncated prefix and unchanged output on refusal.
- `native-player-motion-clock.log`: 45 assertions / two input-clock cases;
  host consumption is visible even when the corresponding update is dropped.
- `native-player-motion-game.log`: 3,435 assertions / 90 game cases.
- `native-player-motion-server-targeted-final.log`: 27 assertions / one real
  two-client process case. Motion goes to its owning player, not the idle second
  client, and echoes native input 900000 against a smaller completed world tick.
- `native-player-motion-server.log`: 1,436 assertions / all 76 Server cases.
- `native-player-motion-walk.log`: 49,361 assertions across all eight visual
  scenarios with the rebuilt Server sending native poses. Existing gates pass;
  the Client ignored these new samples at this stage of the work.
- Build, architecture, formatting and whitespace checks pass. Sanitizers and
  release profiling were not run.

`native-player-motion-server-targeted.log` preserves the failed first run:
the test executable had rebuilt but its launched Server program was stale, so
no native sample arrived. Explicitly building target `server` and rerunning the
same case passed. Client integration is described below.

#### Native prediction replay

After a successful prediction carry, the new Connector selects application-pose
acknowledgements. Ordinary component-delta consumption no longer retires its
inputs. The existing Prediction buffer stays bounded (256 entries by default),
and records the coverage lost to acknowledgement or eviction. Future and
uncovered application acknowledgements are refused; fresh connections retain
the ordinary delta-driven mode unless they opt in.

One transient ECS resource retains the newest native sample. The Client checks
the accepted local Player, destination incarnation, submitted input frontier
and monotonic pose/input ticks. Reconciliation waits for the matching root
generation and its Transform/Humanoid. It then replays only later pending moves
from the atomic pose, using the replica's fixed simulation delta, and retires
exactly that prefix. Authority components stay untouched. Old poses, missing
history and nonfinite replay results cannot replace prediction or acknowledge
inputs. Capture metadata exposes native received/applied ticks and coverage.

Validation under `.cache/build/dev/tests/`:

- `native-player-replay-clock.log`: 100 assertions / three cases, covering both
  transports in automatic and pose-driven modes, plus bounded-history coverage.
- `native-player-replay-replication.log`: 21,865 assertions / 270 cases.
- `native-player-replay-client-final.log`: 9,475 assertions / 167 headless cases.
- `native-player-replay-overflow-before.log`: three assertions reproduced a
  finite incoming pose overflowing during replay and advancing its acknowledgement.
  Final-result validation fixes this; `native-player-replay-overflow-after.log`
  passes all 75 focused assertions, also covering a delayed root component.
- `native-player-replay-held-walk.log`: 106,328 assertions across 16 scenarios.
  The original eight stop-at-adoption cases remain. Eight additional cases hold
  movement for 12 native submissions and require moving native prediction after
  both handoffs. This visual run preceded the extreme-value rejection guard;
  focused and full headless checks passed afterward.
- `native-player-replay-held-audit.json`: all 32 adoptions reconcile natively
  within the following 15 frames. Adoption steps range from 0.266836 to 0.540353
  metres. Later steps reach 1.599661 metres for stop cases and 1.066519 metres for
  held-key cases. These are measured gaps, not seamless-motion passes.

Build, architecture, formatting and whitespace checks pass. Sanitizers and
release profiling remain unrun. The next work must explain the remaining
correction against actual input assignment and simulation intervals: an atomic
consumed-input prefix alone does not prove that replaying each coalesced input
for one client delta matches the host's persistent controller steps. Establish
that timing contract before choosing any bounded presentation correction. Keep
queue refusal, budget splitting, respawn and reconnect in the combined gates,
and address the abrupt third-person image composition separately. Delivery,
subject identity and nonzero image pixels do not prove seamless crossing.

#### Input timeline across adoption

The product Client no longer resets submissions to the younger successor
replica's clock. At adoption it captures the source's current mapped input tick
and the successor's local tick, then maps subsequent local ticks through those
epochs. The mapping preserves elapsed simulation ticks, including skipped
submissions; it does not increment a synthetic counter merely because adoption
occurred. World clocks, native acknowledgement buffers and receipt identities
stay independent. Clock rewind or arithmetic overflow cannot wrap the input
sequence. Both replicas use the configured Client tick rate.

`portal-input-timeline-client.log` passes 9,475 assertions / 167 headless cases.
`portal-input-timeline-walk.log` passes 106,444 assertions across all 16 visual
scenarios, now checking increasing submissions globally across connections and
no regression at adoption. Build, architecture, formatting and whitespace
checks pass; no sanitizer or release profile ran.

`portal-input-timeline-audit.json` covers 32 handoffs with no input regression.
In the 30 Hz first-person example, source submission 126 is followed by native
submission 127 while the successor's local epoch is 29. Source and native pose
samples from the same destination incarnation are now directly comparable in
both clock domains. Across the fixture, elapsed input time exceeds elapsed
destination simulation time by 50 to 83.333 ms between the last captured source
sample and first applied native sample. This uses the configured 60 Hz client
input rate and each case's 30/60 Hz world rate. It is an observed difference
between sampled frontiers, not proof of the exact control interval consumed by
each physics step. A correction of 1.332874 metres remains in that example.

Next, instrument or retain the actual destination control-assignment intervals
at the route switch. Verify whether native input overtakes source input's
existing schedule before designing host-side input scheduling. Any scheduled
input must acknowledge actual application in the completed pose, not merely
its removal from the network inbox. Keep this state bounded and independent of
closed receipt eviction, preserve authority's physics cadence, and include the
stop and held-key cases. A camera offset alone must not hide an unresolved
simulation-time mismatch.

Control-assignment tracing is now available through the opt-in `portal-input`
log category. Both assignment boundaries report destination incarnation, player,
world tick, input tick, step duration, direction and root position. The disabled
path uses a cached log-category guard and retains no per-frame history. Product
forwarded assignment runs in PreSimulation; native assignment runs after the
completed world step in `ServeClients`, so it first affects the next tick. The
analysis must retain the final assignment per physics tick, because multiple
native inputs can overwrite each other before physics runs.

`portal-control-trace-walk-aborted.log` failed the retained-motion displacement
gate at frame 121 of the 60 Hz third-person automatic-subject stop case:
`0.000038548 > 0.0001` failed (squared metres). The actual step was about 6.21 mm
while input advanced and predicted speed was 16 m/s. The first completed
destination pose arrived on that frame, changing the prediction baseline from
source tick 92 to destination tick 100 with input 109. Adoption had not occurred.
Both BMP/PNG frames and metadata are preserved in `portal-control-trace-failure/`.
This is a first-sample correction defect, separate from later native takeover;
the assertion is unchanged. The aborted run recorded 18 switches, each skipping
about 66.7 ms of input time relative to physics. Artifacts are under
`.cache/build/dev/tests/`.

Native input playout implementation contract: anchor bounded native input playout to the last actual
forwarded control assignment, carrying explicit client input duration rather
than assuming client and world rates match. Preserve held controls while queued
native input waits. Close the old forwarding route on native acceptance, but
advance the pose acknowledgement only when the scheduled input actually affects
physics. Keep queue ownership in ECS and its lifetime independent of closed
receipt eviction; cover generation changes, reconnect, overflow, jump edges,
and stopped/held movement. Validate the first destination sample independently,
then require displacement continuity through both correction points. The
implementation is described below; replay continuity still requires the source
inputs that were pending at adoption.

The second full trace run (`portal-control-trace-walk.log`) passes 106,441
assertions across all 16 scenarios. It does not reproduce or close the first
run's retained-motion failure. `portal-control-trace-audit.json` reduces 8,291
assignment rows to 32 route switches, 16 at each world rate. The native input
time lead ranges from 50 to 83.333 ms after phase adjustment and per-step
coalescing. `portal-control-trace-audit.py` records the calculation with the
fixture's configured 60 Hz client input rate. Game motion codec checks pass
15 assertions; build, formatting, architecture and whitespace checks pass.
The saved failed-frame PNGs were inspected. No sanitizer or release performance
profile ran; opt-in trace logging can perturb wall-clock arrival timing.

The input duration prerequisite is implemented: `MoveInput::StepSeconds` carries
seconds per input-clock tick, and product Client samples its actual world Delta.
Game decoding copies the whole validated input after normalizing direction;
this fixes the new duration test's initial five failures caused by copying only
direction and jump. Negative and nonfinite durations are rejected, while zero
explicitly denotes unavailable timing and slow client clocks remain supported.
The portal forward record carries duration through the bus and both outgoing
and incoming snapshot codecs. The bus envelope is now PPT5; movement payloads
grow from 14 to 22 bytes. Client and Server must be rebuilt together. This is
timing metadata used by the native input scheduler below. Unknown timing must
not be guessed from host tick rate.

`portal-input-duration-game.log` passes 3,474 assertions in 91 cases, including
120 Hz and slow-clock duration roundtrip, malformed/truncated input and unchanged
decode output on refusal. `portal-input-duration-script.log` passes 340 assertions
in six movement cases, including a non-default duration across transfer snapshot
replay. Headless Client passes 9,475 assertions in 167 cases; the Server product
portal round trip passes 204 assertions over both world rates and transports.
The visual walk passes 106,384 assertions across 16 scenarios. Its duration audit
finds 8,279 timed assignments across 32 transferred worlds, all preserving the
60 Hz client's actual 0.01666666753590107-second step at both 30 and 60 Hz hosts.
The final relaxed bound for slow clocks is codec-tested; the visual fixture uses
60 Hz client input. Logs, `portal-input-duration-audit.py` and its JSON output
are under `.cache/build/dev/tests/`. Formatting, architecture and diff checks
pass. No sanitizer or release profile ran. Each movement payload adds eight
bytes; no performance improvement or correction fix is claimed.

Native portal input scheduling is now implemented in `script.PortalPlayerInput`,
a host-only component on the Player with a 64-entry ring. Forwarded assignments
retain input/world tick epochs and their independent step durations. Native
inputs queue against that clock, coalesce within a physics step without losing
jump edges, and close the previous source route on acceptance. The queue runs
before physics; `CapturePlayerMotion` uses its applied frontier rather than the
network inbox's consumed frontier. A maximum future lead of one second or one
world step bounds incompatible input clocks. Duplicate queued inputs are inert;
invalid rates, overflow, incompatible rates and capacity exhaustion are refused.
Untimed local control cancels pending input. Character replacement invalidates
old root/Humanoid handles. The explicit snapshot codec preserves the queue;
receipt eviction cannot remove it because the Player owns it.

The initial visual run exposed return transfer rejection of this host-only
component. `portal-native-schedule-walk-before.log` records 3,309 failed checks,
including the unsupported `script.PortalPlayerInput` diagnostic. Scene capture
now accepts an explicit set of host-local components to retain, supplied by the
Script adapter for this queue. Unknown components remain refused by default,
and the queue does not cross the world boundary. The Client classification test
also caught the missing host-only classification, now supplied. These failures
were not fixed by weakening the walk's assertions.

Pending source controls remain a separate required continuation. In the saved
30 Hz first-person case, source history retained inputs 115 through 126 at
adoption. Actual destination forwarding last applied input 122 at world tick
56; native scheduling first applied input 128 at tick 59. The elapsed input and
physics intervals match, but source controls 123 through 126 were not carried
onto the native route. At frame 133, the first native sample still acknowledged
122 and the camera corrected by 0.53327 metres; another 0.53334-metre step
followed at frame 135 while the host continued its held control. Metadata and
matching control traces are in `portal-native-schedule-pending-gap/`.

The continuation implemented below carries the owned, receipt-validated source
input history into the new connection. Its requirements are to map directions
once and deliver still-unapplied controls to the
native queue before newer input overtakes them. Preserve replay coverage and
jump edges through send-budget refusal and reconnect. Merely importing replay
history without delivering those controls would let the host acknowledge a
different held interval. Keep the first completed destination sample correction
as its own displacement gate. Scheduling alone does not establish seamless
movement or resolve the remaining all-angle visual requirements.

Validation: `portal-native-schedule-walk.log` passes 106,759 assertions across
all 16 scenarios after fixing host-local capture. The audit finds 32 scheduled
switches; phase-adjusted input time minus physics time is between -2.785 and
0.216 nanoseconds, consistent with the recorded float step rounding. This closes
the measured 50 to 83.333 ms route timing lead, not the presentation correction.
`portal-native-schedule-game-final.log` passes 3,855 assertions in 92 cases;
its scheduling case also covers explicit local-control cancellation and return
transfer. A fixed-seed full run exposed two collision test fixtures that had
used `CollisionShapes` before canonical scene registration; their setup now
registers scene components, and seed 2037832867 passes. The failure and debugger
origin trace remain beside the validation logs.

Scene transfer checks pass 12,109 assertions in 12 cases; Script movement checks
pass 340 in six; headless Client passes 9,476 in 167; Server native pose and
product portal checks pass 231 in two cases. Architecture, formatting and diff
checks pass. The generated component catalogue passes for all 190 components;
five existing portal-related purpose gaps were filled. `script.PortalPlayerInput`
is 2,136 bytes per affected Player in this dev build, with no per-input queue
allocation and no replica wire row. That is object size, not a measured heap
allocation or release performance claim. No sanitizer or release profile ran.

Source input continuation is now implemented. `CapturePortalPrediction` copies
only the history bound to the accepted receipt, preserves its coverage frontier,
and encodes each retained move after mapping direction into destination space.
Input ticks, jump edges and actual prediction durations survive. The successor
connector's maximum prediction count matches the existing 1,024-entry portal
history. `ContinueInputs` validates a fresh manual-acknowledgement connection and
ordered input before recording anything. The existing prediction buffer owns
both replay and resubmission, avoiding a second input queue. Its unsent prefix
runs before newer submissions; local send-budget refusal retains the suffix.
Applied-pose acknowledgement can prune inputs already handled by the source
without resending them.

An early retransmission can be older than the last applied forwarded control.
`AppliedPortalPlayerInput` therefore exposes that forwarded frontier before the
first native control is queued. The new pose case reproduced six failures when
`CapturePlayerMotion` instead reported the older received input stamp; the
corrected case now passes. This prevents a partially transmitted continuation
from falsely describing the pose with the network inbox's older prefix.

`portal-input-carry-walk.log` passes 106,742 assertions across all 16 scenarios.
The timing audit finds 32 scheduled switches with rounding-scale clock error.
In the 30 Hz first-person stop case, native pose 57 now acknowledges input 124,
then pose 58 acknowledges 126. The first native camera correction falls from
0.53327 metres to 0.013641 metres; the following correction falls from 0.53334 to
0.002311 metres. `portal-input-carry-camera.json` records all 32 adoptions with a
native sample in the following 15 frames. The largest sampled camera step is
0.542834 metres. The 30 Hz third-person held case still alternates roughly
0.533-metre and zero steps among 0.267-metre steps. Input delivery is improved;
this cadence is not proven seamless.

The saved held-case frames narrow this cadence issue to local tick advancement.
Frames 128 through 134 report local ticks 30, 32, 32, 33, 34, 36, 36 and
submitted inputs 128, 130, 130, 131, 132, 134, 134. Both predicted root and camera
move 0.53333 metres after the first two-tick advance, then zero after the
repeated tick. This happens before native pose 57 first arrives on frame 133,
so native pose reconciliation alone cannot explain the pattern. Check the
simulation accumulator and render interpolation against capture timestamps
before changing replay or adding camera smoothing. These frames demonstrate
cadence, not a measured release performance regression.

`portal-input-carry-clock.py` now audits the saved frames against fractional
simulation time, rather than capture wall time alone. Across frames 128 to 138,
`tick + alpha` advances between 0.997190 and 1.001505 ticks per captured frame.
The corresponding integer ticks still alternate between two and zero. This
rules out a comparably large accumulator stall in that sequence. Current
`PredictedFrame` returns `LocalPlayerPrediction::Frame` directly, and
`OffsetReplicaCameraForPrediction` uses its position directly; neither consumes
the presentation alpha. The next fix must derive one fractional presentation
pose for both body and camera without advancing authoritative state, replay
coverage, or input acknowledgement. Preserve that displayed pose across source
to successor clock changes, and test held movement at differing simulation and
presentation rates, stopped input, rotation, and a completed-pose correction.
Do not add an arbitrary camera-only smoothing delay.

A second timing constraint remains visible in `Client::Step`: `SubmitMove`
runs after `Universe::Tick`, once per observed tick, even if that call executes
multiple simulation ticks. Replay currently integrates each retained input by
one local delta. Any dropped intermediate input time must be accounted for
explicitly before treating fractional presentation as a complete movement fix.
The audit is saved as `portal-input-carry-clock.json`; no new runtime or GPU
check was run for this read-only diagnosis.

Replay duration is now corrected independently of fractional presentation.
Both ordinary and native replay had integrated every decoded move with the
current local delta, even when `MoveInput::StepSeconds` retained a different
source duration. The existing prediction and scaled-seam cases now exercise
untimed fallback, 10 ms input, and 30 ms input. Before the fix they fail eight
assertions: both timed inputs move 0.266667 metres at 16 m/s, rather than 0.16
and 0.48 metres respectively. `ReplayLocalPlayerInput` now uses positive
recorded duration, falling back to local delta only for untimed input. It
rejects a duration beyond the float integrator range before conversion; native
replay leaves its applied pose and acknowledgement unchanged on that refusal.

`portal-replay-duration-before.log` retains the eight failures. After the fix,
`portal-replay-duration-after.log` passes 362 assertions in four prediction
cases, and `portal-replay-duration-client.log` passes 9,704 assertions in all
167 headless Client cases. Build, formatting, diff and architecture checks
pass. Logs are under `.cache/build/dev/tests/`. No GPU fixture, sanitizer or
release profile ran for this replay-only change. Fractional presentation,
intermediate tick input coverage, and the earlier cross-world visual gates
remain open.

`portal-input-carry-replication.log` passes 22,409 assertions in 271 cases,
including both transports, tight-budget suffix retry, byte-preserving order,
and retiring a source-applied prefix before retransmission. Game passes 3,867
assertions in 92 cases, headless Client passes 9,491 in 167, and Server native
pose/product portal checks pass 231 in two. Architecture, formatting and diff
checks pass. The 30 Hz first-person frames 132 and 133 were visually inspected.
They show mostly black background and floor, with a yellow surface entering
frame 133; this does not close the lighting or all-angle visual gates. The pose failure
is retained in `portal-input-carry-pose-before.log`. All artifacts and audit
scripts are under `.cache/build/dev/tests/`. Network packet loss/reordering,
combined product traffic budgets and successor reconnect still require dedicated
end-to-end checks; the local-budget wire tests do not prove those cases. The
held-input cadence and first completed destination-pose correction remain next
movement gates. No sanitizer or release profile ran.

Fractional prediction presentation is now implemented. `PredictionPresentationFrame`
advances a copy of the completed prediction by the world's fractional tick time;
the draw collector calculates it once and shares it across the root and limbs.
The Humanoid-follow camera uses the same presented position. Neither path writes
the predicted baseline, authoritative transform, replay coverage or applied
input acknowledgement. This is bounded fractional motion, not a camera smoothing
filter. A copied phase offset preserves the fractional input time when adoption
switches to a successor whose accumulator has a different phase. Both ordinary
and native replay retain that offset for the same live rig.

The new body/Humanoid-camera case reproduces 12 failures before the fix across
30, 60 and 120 Hz with positive, negative and stopped movement. It now verifies
fractional camera/body agreement and unchanged authority/prediction state.
The scaled-seam case additionally draws both source and destination roots at
different clock phases and a different destination delta, proving that their
presented positions agree through the seam. The final targeted run passes 428
assertions in five cases (`portal-fractional-phase.log`). The full headless run
before the added draw-equality assertions passes 9,755 assertions in 168 cases
(`portal-fractional-client.log`). Build, formatting, diff and architecture
checks pass.

The offscreen dev portal run passes 106,689 assertions across all 16 scenarios
(`portal-fractional-walk.log`). All 32 input handoffs remain scheduled with
rounding-scale timing error (`portal-fractional-timing.json`). The corresponding
camera audit finds native samples within 15 frames for all 32 adoptions; first
native camera steps range from 0.008737 to 0.268703 metres, and the largest
sampled step is 0.280262 metres, previously 0.542834 metres. In the 30 Hz
third-person held case, both crossings now move about 0.2667 metres each frame
instead of alternating 0.533-metre and zero steps. These are captured movement
measurements, not a release performance claim.

The saved held frames 132 and 133 were visually inspected; they show a gray
surface against a mostly black scene and do not establish correct lighting or
all-angle rendering. Logs, JSON audits and PNG inspection copies use the
`portal-fractional-` prefix under `.cache/build/dev/tests/`. Intermediate tick
input coverage, source first-pose correction under adverse timing, packet loss,
combined traffic budgets, reconnect, explicit camera overrides, and the wider
visual gates remain open. No sanitizer or release profile ran.

The per-frame prediction camera overlay now respects camera ownership.
Previously `OffsetReplicaCameraForPrediction` moved every active camera whenever
local player prediction was active, including Scriptable cameras, explicitly
cleared subjects and cameras following unrelated objects. The new camera case
reproduces six failures over those three selections and two rendered frames.
The overlay now follows `CameraSubjectRoot`, skips Scriptable control, and only
applies when that resolved subject belongs to the predicted root or one of its
limbs. A limb camera uses the limb's presented offset rather than the root's
offset, preserving its attachment to the displayed body.

Positive fractional-follow coverage now includes explicit Humanoid, root-part
and limb subjects at 30, 60 and 120 Hz with forward, reverse and stopped motion.
`portal-camera-override-before.log` preserves the failures; the corrected
prediction suite passes 533 assertions in six cases, and the full headless
Client suite passes 9,875 assertions in 169 cases. Build, formatting, diff and
architecture checks pass. Logs use the `portal-camera-override-` prefix under
`.cache/build/dev/tests/`. No GPU walk, sanitizer or release profile was rerun
for this subject-ownership guard. Changing an override during the asynchronous
portal adoption handshake still requires a product test and is not closed by
these per-frame checks.

Explicitly clearing `CameraSubject` is now represented in copied camera
continuations. `SubjectCleared` distinguishes an authored null from automatic
follow waiting for a rig; contradictory automatic/cleared values are refused.
Capture and seam mapping preserve that choice, and apply installs a null subject
without requiring a destination root. The prior apply path always installed the
provided destination Humanoid. The new copied-continuation case reproduces four
failures against that behavior, including moving the supposedly free camera.

`Client::PumpPortalSuccessor` now refreshes a previously captured continuation
when the author explicitly clears the active subject. Previously its bound or
retiring-subject filter skipped the refresh and left the old Humanoid camera
available for adoption. The retained prediction rig is also prepared when the
camera has a cleared or unrelated subject. Activation retargets only a camera
that was following the retained source rig; unrelated selections remain intact.
The expanded cleanup case reproduces a failed preparation when the camera
changed before preparation, and now covers both earlier and later overrides.

Validation: `portal-camera-clear-before.log` retains the four continuation
failures, and `portal-camera-clear-hold-before.log` retains the preparation
failure. The final camera-continuation suite passes 601 assertions in eight
cases. Full Scene passes 517,215 assertions in 548 cases, and headless Client
passes 9,875 assertions in 169 cases. A first expanded-test run exposed an
incorrect sixth-case expected camera in the test itself; that expectation was
corrected before the final run. Build, formatting, diff and architecture checks
pass. Logs use the `portal-camera-clear-` prefix under `.cache/build/dev/tests/`.
No GPU or release profile was run for this change. The subsequent product gate below covers the asynchronous late-clear
handshake; these headless checks alone do not prove its end-to-end behavior. Following an unrelated source-world
object after its body connection migrates remains a separate open camera case.

The late-clear product gate now runs four real Client/server round trips at
30/60 Hz in first/third person. `RunPortalWalk` shares the existing fixture,
without duplicating its server setup or capture checks. A LocalScript clears
the source camera when its character changes to the retained rig. After
adoption, the fixture sends R through SDL and UserInputService to restore
Humanoid follow, then finishes the return crossing. The test requires cleared
frames on both sides, a null subject on the adoption frame, fixed camera pose
while cleared, restored follow, and two completed adoptions.

This gate reproduced another camera ownership defect: all four cases retained
the null subject, but `AimReplicaViewer` then replaced the free camera pose and
lens with the unrelated local demo's values. Eye height moved over two metres.
The product run failed four assertions (`portal-late-clear-walk-before.log`),
and the focused viewer/prediction case failed eight (`portal-late-clear-fallback-before.log`).
Fallback aiming now applies only while automatic follow waits for a subject;
explicit selections and Scriptable cameras retain their own pose and lens.

Validation: all 16 original scenarios and four new scenarios pass 133,249
assertions in two cases (`portal-late-clear-walk-after.log`). After adding the
fixed-pose assertions, the four new scenarios pass 27,128 assertions
(`portal-late-clear-pose.log`). Headless Client passes 9,896 assertions in 169
cases. Build, formatting, diff and architecture checks pass. The final JSON
audit records exactly 12 retained and 12 native cleared frames in each case,
a null subject on each outbound adoption, and zero position change between
successive cleared-camera frames. Logs and audits use `portal-late-clear-`
under `.cache/build/dev/tests/`; failing captures are retained under
`portal-late-clear-failure/`. No sanitizer or release profile ran.

The visual inspection exposes a separate unresolved image handoff mismatch.
In the final 60 Hz third-person case, frames 125/126 have identical eye pose
and `eye_world = server.world`, while input switches from `client.replica` to
`client.portal.1` and presentation switches from the local replica to the
persistent viewport's remote image. The local frame shows the view through
the portal, but the remote frame shows a gray panel. Of 16,384 pixels, 7,159
differ by more than 2/255 in a channel; maximum difference is 160/255.
`portal-late-clear-final-before/after.{json,png}` and
`portal-late-clear-image-difference.json` preserve this evidence. Fixed camera
pose does not close visual seamlessness. Next trace the local versus remote
portal rendering paths at this identical eye, including nested image binding
and producer render settings, before accepting the image switch as seamless.

The complete-world producer now builds nested cross-world demands through
`CollectPortalImageDemands`. Sixteen retained parent slots wait for named child
replies, with reduced recursion depth, shared parent/child pixel budgets and a
one-second deadline. Entrance suppression is preserved. Child content and
lighting versions contribute to the parent cache signature, so renewing a parent
cannot hide changed child content. The server relay admits the sixteen bounded
nested reply channels. Successor readiness lets an in-flight image complete
while the camera moves, instead of cancelling it every frame.

Copied and resident GPU fixtures verify a blue nested child, unchanged child and
parent renewal without redraw, then green child content invalidating both.
Budget rejection and clearing a waiting parent are also covered. The focused
product image-handoff run passed 14,174 assertions across the 30/60 Hz scenarios
in `portal-nested-product-moving.log`. Saved fixed-eye pairs and
`portal-nested-product-audit.json` show far-side blue pixels surviving adoption:
2,465 to 2,742 at 30 Hz, and 2,794 to 2,211 at 60 Hz. Character appearance still
differs across that switch, so this does not establish full visual parity.

The broader product run stopped without a final test summary. Its log
`portal-nested-product-all.log` contains failures at frames 86/87 in the 30 Hz
first-person walk: the eye has entered `walk.destination`, but no eye image is
available until frame 88. Inspected `portal-eye-gap-{85,86,88}.png` confirms a
fully black frame 86. The next crossing fix must make the destination image
ready before the eye crosses, including nested capture latency. Returning a
source-world fallback or weakening the visibility check would not close this.

Failure replies previously vanished when their reply mailbox was full. The
producer now retains at most sixteen failed replies separately from GPU capture
slots and retries their encoded bytes until the original deadline. Overflow is
refused. Headless checks cover delivery after mailbox drain, exactly-once send,
expiry, clear and endpoint replacement. Logs use `portal-reply-retry-` under
`.cache/build/dev/tests/`. The full headless render suite passes 32,822
assertions in 439 cases; the selected runtime suite, including copied/resident
GPU fixtures, passes 4,157 assertions in 12 cases. Formatting and diff checks
pass. The duplicate black-frame PNG was removed; representative gap frames
remain beside the original captured JSON.

The crossing trace reproduced the black frames after the nested change. In
`portal-eye-gap-trace.log`, the first 30 Hz eye request took 364 ms to return.
The server collected child image traffic only after forwarding host traffic,
leaving nested messages waiting another simulation tick per hop. Pumping the
producer before that forwarding reduced this observed request to 299 ms and
removed the automatic Humanoid camera's black frames. Saved
`portal-eye-early-relay-{85,86,88}.{json,png}` and request traces preserve this
comparison. These are dev trace observations, not release performance claims.
The broader check still found one black frame with an explicit Humanoid subject
at frame 84, so the crossing gate remains open.

The current server change pumps the image relay within the existing one-ms
host/driver wait loop, before `ServiceLink`, and again at the existing
post-presentation point. Presentation traffic can progress while the host waits;
simulation timing is unchanged. The next validation uses
`portal-eye-wait-relay-` logs. The earlier `[server]` test selection incorrectly
included the hidden `saved-host-child` entry, which requires an inherited driver
channel; its standalone initialization failure is a harness selection failure.
The corrected server selection `[server]~[.]` passes 1,459 assertions in 76
cases. Architecture and diff checks pass. The current product run passed the
30 Hz automatic and explicit Humanoid camera crossing checks, then stopped on
held return movement at frame 236: squared displacement was 0.00003136 against
the required 0.0001. This is not a passing full crossing run. The retained
prediction advanced from destination tick 107/input 223 to tick 108/input 226,
so one 30 Hz authority step discarded three 60 Hz inputs. That is evidence to
trace input coverage and replay phase before changing correction behavior.
`portal-eye-wait-motion-{234,235,236,237}.json` preserves these states, with
images for frames 235/236. The first explicit-camera crossing has an image at
frame 86, preserved in `portal-eye-wait-explicit-86.{json,png}`. The 60 Hz cases
were not reached by this abort-on-first-failure run.

The held return failure is localized to the motion/replay time contract.
`ApplyForwardMove` coalesces to the newest received input and acknowledges that
number after one physics step. `ReconcilePortalInputHistory` then discards every
entry through that number and replays only later inputs. In the saved frames
235/236, acknowledged duration advances 0.05 s while destination physics advances
0.033333 s. The extra removed replay duration is one client step, matching the
near-zero movement. `portal-replay-clock-audit.json` records this calculation.
Native replay also skips entries solely by `InputTick`, so fixing only held
portal replay would leave the same time ambiguity after adoption.

The next motion contract must distinguish consumed input sequence from completed
simulation time. Preserve the consumed stamp for delivery acknowledgement and
jump handling, and carry enough destination clock/phase information to replay to
a consistent presentation time across forwarded and native control. Test
coalesced batches, unequal clock rates, no newly received input, delayed samples,
and adoption with the same timeline. Do not infer simulation duration from the
number of acknowledged inputs or solve this by weakening the movement check.
The full 22-scenario product matrix is now running without abort-on-first-failure
in `portal-wait-relay-full-product.log`, so later 60 Hz and cleared-camera cases
can provide evidence despite the known held-motion failure.

Motion samples now carry `SimulationSeconds` from the completed destination
world clock. Forwarded samples and native `CapturePlayerMotion` use the same
field and codec. Negative/nonfinite seconds, truncated payloads and malformed
clock fields are refused without replacing accepted output. The bus envelope is
PPT5, and the shared motion sample grows by eight bytes. Client and Server were
rebuilt together. This adds the required time fact; replay still needs to consume
it and preserve its mapping through adoption. No movement-fix claim is made.
The full game suite passes 3,925 assertions in 92 cases, and client prediction
checks pass 554 assertions in six cases. Clock logs use `portal-motion-clock-`. Portal transfer checks, including the
process fixture, pass 1,501 assertions in 24 cases. Architecture and formatting
checks pass. The first failing 60 Hz explicit-camera pair is preserved as
`portal-60-explicit-route-gap-{82,83}.{json,png}`.

The pre-clock full product matrix completed with 1,711 failed assertions out of
148,423. Failures are confined to three 60 Hz scenarios in that run: one
third-person image-handoff landmark failure, and explicit first-person walks
with/without held adoption, which lose eye images and fail the return. The
cleared-camera test case passes. `portal-wait-relay-full-failure-scenarios.json`
groups the failures. These are distinct remaining visual/route failures alongside
the held-motion timing bug; a passing focused 30 Hz run does not close them.

Held and native prediction now maintain a `PredictionReplayClock`: completed
simulation seconds, acknowledged input stamp, and the difference between elapsed
input duration and completed world duration. A three-input acknowledgement after
a two-input-duration physics step keeps the missing interval as completed motion
extrapolation before replaying remaining controls. If completed time overtakes
input time, replay skips only the covered integration duration; unacknowledged
control/jump edges still apply. A stale simulation clock cannot replace prediction.
When authority has advanced beyond the entire input horizon, the uncovered
interval rebases onto that completed pose so a slow client can keep reconciling. The mapping is copied
through `PortalPredictionContinuation` into native prediction on adoption.

The focused headless fixture checks continuous constant-speed movement for the
observed 30/60 coalescing case, adoption into native prediction, no-new-input
samples, authority overtaking the entire input horizon, jump retention and stale
simulation time.
Clock logs use `portal-replay-clock-`. The completed product walk across both rates reports no held-movement threshold
failures, but still fails 874 assertions in the 60 Hz explicit first-person stop-on-adoption
case, chiefly missing eye images and the return. The full headless client suite
passes 9,944 assertions in 170 cases, and the focused clock/prediction selection
passes 602 assertions in seven cases. Architecture and formatting checks pass.
`portal-replay-clock-product-audit.json` groups the product failures. These focused checks do not close
moving-camera, collision, or product visual parity gates.

The next image investigation is `ResolveCameraPortalWorld` returning an invalid
world while destination topology is unavailable. `PreparePortalEye` then submits
an invalid destination to `SubmitEye`, which removes the viewport and its prepared
image. The resolver also keeps a just-crossed history only in its local copy until
all topology hops complete. This code path can discard useful known-world
presentation while waiting for topology. Verify it with a delayed-topology fixture
before changing it; missing topology must not be treated as proof of no further
seams, and images must still belong to the actual resolved world.

The routing diagnostic rerun (`portal-eye-route-enabled.log`) passes all 16
walk scenarios with 110,535 assertions. Only logging changed, so this does not
resolve the intermittent failure. The preceding run stops at the first failed
assertion in the 60 Hz automatic first-person case, frame 85; its saved image is
entirely black, and subsequent captured frames also lack foreign-eye images.
`portal-eye-route-auto-{84,85}.{png,json}` preserves that entry, while
`portal-eye-route-enabled-auto-{84,85}.{png,json}` preserves the passing rerun.
The earlier `portal-eye` category was not enabled by the product fixture; absence
of those messages did not establish successful routing. Resolver failures now
use the fixture's enabled `client` trace category. The passing rerun reports no
routing failures. The focused routing test still passes 144 assertions.
A repeat (`portal-eye-route-repeat.log`) fails in the 60 Hz automatic third-person
case at frame 131 on adoption, with 56,656 of 56,657 assertions passing before
abort. No routing failure is logged. The source eye is selected, but its image is
missing. `portal-eye-adoption-gap-{130,131,132}.{png,json}` preserves the transition.
The server repeatedly retires its image producer before and after this handoff.
The previous automatic and explicit first-person failure logs show the same
producer retirement and exponential retries. Investigate that shared producer
failure before changing topology semantics. The server supervisor now logs its
world, process exit reason/code/signal, connection state, relay refusals and link
malformed/drop counters before retirement; the old warning conflated process exit
with relay failure. The headless server suite passes 1,459 assertions in 76 cases.
`portal-producer-loss-product.log` passes all 16 walks with 109,904 assertions
and no producer loss. The subsequent combined selection in
`portal-producer-loss-matrix.log` stops in the 60 Hz automatic third-person walk:
61,686 of 61,687 assertions pass, both adoptions complete, but `nativeWorlds.size()`
is one instead of two. No producer loss or resolver failure is logged in that
run. It aborts before the cleared-camera and image-handoff test cases, so those
cases were not revalidated. Treat this native-prediction coverage failure
separately from the earlier missing-image failures; neither is fixed by logging.

The saved native failure shows destination prediction remaining at pose tick 122
while received native samples advance to tick 240. Captured frames 104 to 130
span 1.315 seconds, but only 26 new input steps are submitted. The previous replay
clock rejected every sample beyond that shorter input horizon, preventing native
correction throughout the destination visit. `portal-slow-client-original-samples.json`
retains the relevant metadata. Deterministic native and held fixtures reproduce
this rejection (`portal-slow-client-before.log`, two failed assertions).
Reconciliation now rebases any authority time beyond all remaining input time
onto the completed pose after applying pending controls with zero covered
integration. It preserves ordinary overlap replay and stale-clock rejection.
The slow-native fixture checks three consecutive corrections and unacknowledged
jump retention; the held fixture checks the same overrun before adoption.
Native frame metadata now includes both replay and sample simulation clocks.
Focused prediction tests pass 663 assertions in seven cases; the non-GPU,
non-hidden client selection passes 8,298 assertions in 142 cases. Architecture
and formatting checks pass. `portal-slow-client-product.log` passes all four
cleared-camera scenarios and the eight 30 Hz walks, then fails at frame 121 of
the 60 Hz automatic third-person walk: held displacement squared is 0.000041923,
below 0.0001. Of 83,512 assertions, 83,511 pass before abort. That scenario's
completed capture shows native poses applied in both destination and return
worlds. The image-handoff case and later walk scenarios did not run.
`portal-slow-client-held-{119,120,121,122}.json` and images 120/121 preserve the
remaining movement failure. Frame 121 accepts the first destination motion
sample, advancing held authority from tick 92 to 100 and acknowledgement to 109.
This is initial correction, before any previous replay-clock mapping exists;
it does not exercise the newly changed clock-overrun branch. Its correction
nearly cancels the frame's forward movement. Resolve first-sample presentation
continuity without suppressing authority or weakening the displacement gate.
No producer retirement or camera-route failures are logged in this run.
No sanitizer or release profile ran for this change.

Position reconciliation now retains a client-local display offset for the shared
body/camera pose. The raw prediction and acknowledgement advance immediately;
small offsets settle over 100 ms of presentation time. Larger offsets settle
at no more than half walking speed, within one second; corrections beyond that
bound snap to authority. Repeated corrections start
from the current displayed position. An unchanged baseline preserves the
remaining interval instead of restarting it. Held and native replay use the same
operation as ordinary prediction; adoption rotates/scales the vector once and
carries its remaining duration. No authority row receives this offset.
`PresentedPlayerPrediction` supplies capture metadata from the same calculation
used by body collection and Humanoid camera follow. `predicted_root` retains the
raw baseline, while the unchanged held displacement threshold now measures
`presented_predicted_root`, the actual displayed pose. This tests visual
continuity without treating an immediate authority correction as rendered motion.
Headless tests verify a moving corrected body and camera, unchanged authority,
repeat correction, stopped settling, non-restarting unchanged replies, and
scaled adoption including fractional presentation time. Focused checks pass
696 assertions in eight cases; non-GPU/non-hidden Client checks pass 8,331
assertions in 143 cases. `portal-correction-product.log` rejects the first fixed
100 ms blend at frame 116 of the 30 Hz first-person cleared-camera walk. Its
1.59-metre correction cancels roughly 16 m/s of walking throughout that interval.
The updated shared-pose fixture includes this six-input-step correction and
requires at least half the nominal forward displacement, as well as immediate
authority state and eventual settling. `portal-correction-bounded-focused.log`
passes 713 assertions in eight cases; its non-GPU/non-hidden client selection
passes 8,348 assertions in 143 cases. The failed fixed-duration images/metadata
are retained as `portal-correction-fixed-duration-{115,116}.{png,json}`.
`portal-correction-bounded-product.log` stops at the floor-pixel gate on frame 87
of the 30 Hz explicit third-person held/cleared-camera scenario: 1,078 of 1,079
assertions pass before abort. A whole-eye image is ready, but its bottom-centre
floor pixel is black. `portal-correction-bounded-eye-{86,87,88}.{png,json}` preserves
the transition. The camera position is unchanged through adoption at frames
86/87, and its world remains `server.world`; the imported image shows a black
lower region around destination geometry. No producer retirement or resolver
failure is logged. This is an open whole-eye rendering/parity gate, not proof
that the full movement matrix passes. Later scenarios did not run. No sanitizer
or release profile ran for the correction change.

The whole-eye foreground fixture now checks a red floor beside a nested
cross-world child through the actual eye-output pipeline. All eight combinations
of copied/resident images, default/explicit material, and default/legacy lighting
retain the floor through three captures while the child changes blue/green/blue.
Pixel checks verify both the floor and refreshed child content, and capture counts
verify that changed child content invalidates the parent. The focused run
`portal-eye-refresh.log` passes 732 assertions; the full `[portal-runtime]`
selection passes 4,889 assertions in 12 cases in `portal-eye-refresh-runtime.log`.
This does not reproduce the product adoption gap above. Failure previews are written only when a pixel check
fails; misleading fixture-calibration previews were removed, while the product
failure images remain available.

The fixture additionally exercises the saved product eye position, rotation,
field of view and doorway dimensions using planar geometry. Its narrow floor
strip is filtered with the child image at 32-square resolution, so this variant
checks nonzero red floor contribution rather than red dominance. It still does
not exercise the product's box meshes, player geometry or endpoint timing.
`portal-product-eye-runtime.log` passes 5,597 assertions in 12 cases.
`portal-floor-product-repeat.log` fails the ready-image check at frame 89 of the
30 Hz third-person explicit held/cleared-camera case: 1,049 of 1,050 assertions
pass before abort. Saved capture metadata shows adoption at frame 89, no whole-eye
image through frame 102, then a ready image at frame 103. No producer retirement
is logged. The bottom-centre pixel is black even in ready frames 103 and 104,
so delayed readiness and the foreground gap both remain open. Captures
`portal-floor-repeat-{88,89,102,103}.{png,json}` preserve both transitions.
The next product check must cover source-eye readiness before adoption as well
as the actual box-floor render; a ready destination image alone is insufficient.

`Client::PumpPortalSuccessor` now resolves the arrived camera's actual eye world
before committing or adopting the successor. If that eye is foreign, it waits
for an image in the persistent viewport slot while keeping the previous
presentation. This supplements the successor-body image gate, which cannot prove
that a trailing or explicitly cleared source-world eye is drawable.
`portal-eye-adoption-product.log` reaches adoption at frame 85 with an image.
Inspection of the recorded 600-frame sequence finds no missing foreign-eye image
after joining, including the return adoption. This is one run, not full matrix
proof: the existing floor-pixel assertion still fails at frame 85, with 1,029 of
1,030 assertions passing before abort. `portal-eye-ready-{84,85,86}.{png,json}`
preserves that transition. Non-GPU/non-hidden Client checks pass 8,348 assertions
in 143 cases, and architecture checks pass. No release profile or sanitizer ran.

The renderer fixture now uses built-in box floors in both rooms, the thick
product doorway, and a reciprocal child-world portal for the saved camera case.
A one-room setup initially failed because the doorway exposed empty child space;
that setup was corrected and its misleading previews removed. The complete
box-room fixture passes 1,464 assertions; the full runtime selection passes
5,621 assertions in 12 cases (`portal-box-rooms-runtime.log`). Coverage includes
repeated child updates,
copied/resident transport, and default/explicit materials and lighting. It does
not reproduce the product's missing floor, so player geometry, published replica
rows and product capture state remain investigation targets.

The floor gap is now reproduced and corrected. Temporary producer diagnostics
confirmed an unclipped default-cube floor with the expected size, pose and tint.
The fixture's backdrop walls had concealed the missing rays. The saved product
camera variant now uses 128-square images, default cube meshes, two floors and
reciprocal thick portals, with those backdrop walls hidden. Before the fix,
`portal-floor-only.log` fails all 24 floor checks across eight material, lighting
and transport combinations while its other 1,472 assertions pass.

Portal surface draws now project their depth onto the authored capture plane
using the same homogeneous eye-ray intersection as image sampling. Previously
the slab's nearer physical face could cover source geometry in front of that
plane, while the child capture correctly clipped that same region away. This
left black pixels despite a ready image. The rule applies to local and imported
portals, including nested draws, without adding a render pass or shader uniform.
The existing sampling mathematics tests compare all three projected coordinates.
The corrected floor fixture passes 1,448 assertions. Temporary draw diagnostics
and misleading setup previews were removed; `portal-floor-only-before.png`
preserves the actual small reproduction.

`portal-plane-depth-product.log` passes 27,777 assertions across all four
generated cleared-camera round trips at 30/60 Hz in first/third person. The first
30 Hz third-person adoption now keeps the bottom-centre floor pixel at RGB
(169, 96, 45) before, during and after adoption. The matching product captures
are `portal-plane-depth-product-{84,85,86}.{png,json}`, alongside the prior
`portal-eye-ready-{84,85,86}` failures. The ordinary walking matrix and landmark
handoff gate pass 124,993 assertions across 18 generated round trips in
`portal-plane-depth-walk.log`. Together these product runs cover all 22 current
scenarios. They are evidence for this fixture matrix, not all remaining crossing
requirements below.

`portal-plane-depth-gpu.log` passes 15,922 assertions in 17 cases, covering portal
runtime transport, nested images, Humanoid camera walking, radiance, lighting,
angle sweeps and sampling mathematics. The non-GPU/non-hidden renderer selection
passes 10,732 assertions in 337 cases; architecture and formatting checks pass.
Successful product-run BMPs were reduced to adoption PNG pairs, retaining all
capture metadata and the curated failure/fix images: 13,200 redundant BMPs were
removed. No release profile or sanitizer ran for this depth change.

Waiting-parent endpoint recovery is now covered with copied and resident images.
The fixture replaces a child request endpoint before its producer runs, verifies
that the parent reports failure without publishing an image, then retries against
the new generation successfully. A separate clock-driven timeout case leaves the
old child request queued, expires the parent, checks that image allocations are
released, and verifies that a fresh parent request recovers. These cases exercise
failure and retry, not continuous visible-image availability during disruption.

`portal-child-timeout.log` reproduced two failed assertions in resident recovery:
"nested destination capture failed: resident capture receipt refused". Resident
export was incorrectly nested inside the CPU staging-buffer growth branch.
After a cancelled request used a slot for copied pixels, a resident request could
reuse its sufficiently large staging buffer and skip resident texture export.
`RecordResourceImages` now handles resident delivery before CPU staging allocation.
It always records the resident texture copy and does not allocate a download
buffer for that delivery mode. The combined portal-runtime/resource-image suite
passes 13,137 assertions in 16 cases (`portal-child-recovery-fixed.log`). No release
profile or sanitizer ran for this change.

Pending child captures now revalidate the authored seam before using their image.
`portal-moved-child-before.log` reproduces the previous behavior: moving an
entrance while its child producer had not run left the parent waiting with its
old demand. The producer now rebuilds only the camera/seam demand values and
compares the seam revision; disappearance, unsupported mapping or changed mapping
fails the parent and releases its child image state. The initial collection skips
this duplicate work, and revalidation does not collect crossing geometry again.
Tests cover entrance translation, entrance rotation and destination translation
in copied and resident modes, checking refusal without image publication followed
by successful retry. The full portal runtime suite passes 5,946 assertions in
12 cases (`portal-moved-seams-runtime.log`); architecture and formatting pass.
This does not yet prove discovery of newly appearing child portals while another
child is pending, nor uninterrupted image delivery through continuously moving
seams. No release profile or sanitizer ran for this change.

The pending-child check now compares the complete set of current cross-world
demands with the captured child set. A newly appearing demand, changed mapping,
or missing prior demand invalidates the parent rather than publishing a capture
that silently omits an opening. Demand validation still uses camera/seam values
without collecting crossing geometry. `portal-appearing-child-before.log`
reproduces the missed new opening; the focused fixed check passes 71 assertions
across copied and resident modes. It verifies failure without publication, then
successful retry with a pixel budget for both children. Combined runtime and
resource-image tests pass 13,409 assertions in 16 cases
(`portal-child-set-runtime.log`); architecture and formatting pass. Continuously
moving seams can still invalidate successive attempts, so uninterrupted progress
under sustained topology changes remains open. No release profile or sanitizer
ran for this change.





Sequential resident-world replacement now has a GPU lifecycle check. One
persistent viewport receives alternating red/blue images across 32 destination
incarnations using the same name, and another 32 using distinct names. Every
replacement checks new pixels and handles, zero uploaded image bytes, release of
retired imported textures, and bounded logical GPU live bytes after warm-up. The
bound permits three 8-square RGBA16F images above the warm baseline; it measures
engine payload accounting, not driver heap commitments. The focused fixture
passes 1,664 assertions (`portal-world-churn.log`); the portal host suite passes
2,126 assertions in seven cases (`portal-world-churn-host.log`). Captures stay in
memory and create no image files. Concurrent pending captures, source-world
retirement and long-duration growth still need their own evidence. No release
profile or sanitizer ran for this test-only addition.

The lifecycle matrix now also retires the source world before its queued request
runs, recreates the same named source, and requires exactly one capture for the
replacement request. It then retires that source while its resident image is
live, checks viewport removal and invalidation of the old handle, and checks the
same texture-release and logical-memory bounds before removing the destination.
Reused/distinct destination names and both retirement orders cover 128 cycles.
The focused check passes 3,840 assertions (`portal-source-churn.log`); the host
suite passes 4,302 assertions in seven cases (`portal-source-churn-host.log`).
This covers local-bus queued-request cancellation and source-owned image cleanup.
Concurrent viewports under churn, delayed cross-process replies and long-duration
growth remain separate gates. Captures remain in memory; no image files are added.

Still required: cyclic routes under finite depth without parent-slot starvation,
continuous capture progress under moving seams, concurrent captures and long-duration residency under world churn, first-person character
exclusion, and complete product camera/crossing visual gates. The nested fixtures
and focused adoption check do not prove these. No sanitizer or release profile
ran for this change.

The shared input channel also carries shots. `Shot::ViewTick` now carries the
rendered authoritative fractional tick from `SnapshotBuffer::RenderTick()`.
`Server::ApplyInputs` samples that time directly; the envelope input sequence
only orders input and is never converted into world time. Interpolation delay
and transport latency must not be subtracted again from an already rendered
view tick. Zero and out-of-history requests resolve against present state;
empty history still skips the shot. Negative and nonfinite times are rejected.

The shot payload grows from 28 to 36 bytes. Client and Server must be rebuilt
together; the old payload is rejected. The moving-character process case aims
at a saved view after the target moves clear, using input sequence 900000.
Against the old Server consumer it failed five assertions; the corrected
consumer passes all 45 assertions across current and historical view cases.
`shot-view-clock-before.log` and `shot-view-clock-after.log` preserve this check.
`shot-view-clock-codec.log` passes 40 assertions in 11 cases, including fractional
time roundtrip, malformed times and missing timestamp bytes. These logs live
under `.cache/build/dev/tests/`. This establishes independent shot and input
clocks, not cross-portal shot routing or seamless movement at native takeover.
Full dev validation passes: `shot-view-clock-server.log` has 1,459 assertions
in 76 cases; `shot-view-clock-client.log` has 9,475 assertions in 167 headless
cases. Architecture, formatting and diff checks pass. The portal GPU fixture
was not rerun for this shot-only change. Sanitizers and release profiling were
not run. No performance improvement is claimed; each shot adds eight wire bytes.

Client integration still requires: pose, velocity, required Humanoid prediction state,
receipt and source/destination incarnations must form one accepted sample;
partial component replication must not acknowledge a pose that has not arrived.
Continue source input until the destination's native route takes over, and
retire retained inputs only against the matching destination sample. Define how
coalesced held-direction intervals map to simulation time before replaying them;
do not equate direction-change counts, client ticks and destination ticks.

Use that accepted sample to reconcile the held prediction before and through
adoption, map position/rotation/linear velocity and camera scale once, and keep
ordinary authoritative correction behavior after the handoff. Verify dropped,
delayed, duplicated and budget-split samples plus successor reconnect. Extend the
existing walk with a cross-adoption displacement gate after correction is built;
passing subject identity or nonzero pixels cannot close this defect. First-person
body exclusion in remote eye captures remains a separate requirement.

First-person exclusion implementation gates, still open:

- [x] Resolve the native rig and held camera's account identity through
  `SelectFirstPersonBody` and `ResolveEyeBody`. Cover detached held roots,
  duplicate account identities, cleared subjects and first/third-person modes.
- [x] Carry canonical bounded player identity in whole-eye requests and their
  codec. Numeric ECS handles remain local; seam requests refuse this selection.
  Check distinct selected players with identical display/model names.
- [x] Preserve bounded player identity in copied geometry and select imported
  primary-eye rows without assigning destination ECS handles. Keep selection
  correct when an earlier mesh is unloaded and after source model retirement.
- [ ] Extend identity checks across cancellation, stale endpoint incarnations,
  delayed selection changes and actual product seam crossings. Native account
  lookup alone does not establish a transported row's identity.
- [ ] Exclude matching primary rig rows from the existing camera draw order.
  `ViewRecording::Begin` copies the ordered entity output into `DrawOrder`
  before computing its opaque/transparent partitions. Its separate `SceneOrder`
  feeds secondary views and shadow casters. Preserve that separation, stable
  blended ordering and seam variants. Do not use `LocalTransparency` or remove
  rows from the shared instance collection to implement a camera-only rule.
- [ ] Include the selection in request camera revisions and object presentation
  signatures. Re-resolving to a replacement root must invalidate the primary
  image even when camera pose and draw geometry are unchanged. Keep unrelated
  environment and shadow inputs reusable. Check retained-image renewal when
  switching first/third person and when the Humanoid subject changes.
- [x] Verify ordinary body exclusion preserves the complete shadow map and
  visible floor shadow, with a negative control that disables body casting.
  Verify copied whole-eye selection across separate renderer devices and check
  upload bytes at submission, followed by zero pending CPU bytes.
- [ ] Extend existing GPU character fixtures with opaque and blended limbs,
  a second rig, mirrors, local and remote portal child views, and shadow pixels.
  Compare direct and remote primary views while checking the viewer remains
  visible in secondary views. Exercise both copied and resident image delivery.
- [ ] Run product first-person entry, held-subject gap, adoption, reversal and
  subject-clear sequences. Keep selected before/after images and frame metadata;
  clean temporary sequences after inspection. Measure release CPU selection
  time, upload bytes and logical GPU residency. No extra scene copy, render pass
  or texture is required by the proposed visibility selection; verify that with
  counters rather than treating the design as measured performance.

The first implementation now resolves `PlayerIdentity::UserId` from the active
first-person Humanoid, including its detached held character. Whole-eye requests
carry its canonical decimal spelling in PIMG version 8; both peers need this
build. Destination resolution uses the current player-character links, omits
held camera copies and clears ambiguous or missing matches. Local rig handles
stay inside the receiving world. The codec rejects noncanonical and out-of-range
identity strings and refuses body selection on seam requests.

The primary draw-order stream filters the selected world's ordinary rig rows
and recomputes opaque/transparent counts without changing resident scene rows.
Secondary portal views and mirror captures retain the character. Object image
signatures include the resolved rig; primary-eye selection is omitted from the
mirror signature so changing it does not rebuild an identical reflected image.
Changing the requested player supersedes pending work. The last completed image
remains presentation history while the replacement waits, and `CurrentImage`
reports no fresh result until that replacement completes.

Validation so far: `eye-body-headless.log` passes 1,974 assertions in 15 cases;
`eye-body-render-headless.log` passes 32,934 in 443 cases;
`eye-body-client-headless.log` passes 10,055 in 171 cases. The combined portal
checks pass 13,822 assertions in 35 cases (`eye-body-portal-gpu.log`). The focused
eye and mirror GPU checks pass 19,796 assertions in three cases
(`eye-body-mirror-gpu.log`), including identical reflected pixels and zero
additional mirror passes when toggling selection. Architecture and formatting
checks pass. The four 30/60 Hz product subject-clear round trips pass 27,837
assertions (`eye-body-product-camera.log`). Logs and captures are under
`.cache/build/dev/tests/`.

`eye-body-comparison.png` shows opaque and blended bodies with neither, the blue
player, and the green player excluded. Both characters deliberately share their
display names. `eye-body-product-transition.png` records inspected product
transition frames. Forty-eight selected join/adoption images and their frame
metadata were retained; 2,400 temporary product BMPs were removed.

The remaining 18 standard-walk and image-landmark scenarios also pass, with
123,389 assertions in two cases (`eye-body-product-walk.log`). Together with the
four subject-clear scenarios, all 22 product round trips pass on this build.
Another 144 adoption images were kept and 10,800 temporary BMPs removed. These
walks verify crossing and camera continuity; their current assertions do not
prove per-pixel exclusion of every synthetic seam copy.

The broad gates above remain open. This establishes native rig selection,
resident remote-eye pixels, local portal visibility and mirror cache behavior.
Imported rows now support primary-eye selection through their copied player
identity, as described below. Local synthetic rig variants and actual product
crossing geometry still need separate coverage. End-to-end transported-character
shadow pixels, delayed selection changes and the full crossing matrix remain
unproved. Release profiling and
sanitizers have not run; no measured performance improvement is claimed.

The native shadow gate now compares the complete 2048-square depth map before
and after primary body exclusion. They are identical. Disabling that body's
casting afterward changes both the map and the visible floor pixels. A second
caster keeps the negative-control pass live, so this cannot pass by reading a
stale map after the last caster disappears. The focused test passes 100 assertions
(`eye-body-shadow.log`). `eye-body-shadow-comparison.png` shows the visible body,
hidden body with its shadow, and hidden body without its shadow, respectively.
Temporary PPMs are removed after inspection.

Copied whole-eye selection now has a two-device check: visible, hidden, visible
again through owned presentation messages. It verifies image pixels, uploaded
bytes increasing at render submission and pending CPU bytes draining afterward.
The initial check read upload bytes before submission and failed that assertion;
the pixels already passed. The corrected combined eye/portal runtime/character
suite passes 7,896 assertions in 21 cases
(`eye-body-shadow-copied-gpu.log`). This is two renderer devices in one process,
not a separate-process first-person proof. Formatting and diff checks pass.
Those shadow/copied test additions did not change engine behavior after the
preceding 22-round-trip validation.

Copied geometry now carries a canonical decimal player identity in PGE2. Its
codec shares validation with whole-eye identity and bounds the string before
copying it. This changes the geometry payload format; geometry peers need the
matching build. Entity handles and diagnostic paths do not identify received
players. Source conversion preserves a retired row's visual data, and an active
held character can identify its exact retired source root. Explicitly foreign
rows cannot borrow a coincident local handle to claim that player's identity.

`AppendPortalDraws` returns request-local selected indices alongside the complete
geometry. `KeepLoaded` remaps only those indices while filtering unavailable
meshes. The renderer retains at most 256 selected indices for this bounded
payload, rather than an index per destination row. Primary object signatures
include the selection; mirror signatures omit it. Shadow and secondary draw
streams remain complete. This is a storage bound from the implementation, not a
release timing or allocation measurement.

The copied-image fixture compares native and transported characters with a
missing destination mesh preceding the body. It destroys the source model after
encoding, then requests visible, hidden and visible images across two renderer
devices. `eye-import-comparison.png` shows inspected native/imported comparisons.
The initial expanded fixture crashed because it omitted the required `DrawList`
resource before calling `CollectInstances`; the debugger identified the missing
fixture setup, which now follows the producer's registration and visibility
setup. The failure trace is retained in `eye-import-debug.log`.

The existing portal, mirror and shadow fixtures also exercise imported-row
selection. Opaque/blended primary rows disappear while local portal pixels and
mirror pixels stay unchanged, mirror captures stay cached, and the complete
shadow map plus visible shadow survive selection. These checks establish local
secondary-view behavior for imported rows. They do not establish propagation of
incoming geometry through a remote child request.

Final validation for imported selection: render headless passes 33,013 assertions
in 444 cases (`eye-import-render-headless.log`); Client headless passes 10,055 in
171 (`eye-import-client-headless.log`); draw filtering passes 147 in 28
(`eye-import-drawinstance.log`). Focused eye, portal runtime, geometry and mirror
checks pass 27,292 assertions in 27 cases (`eye-import-final-gpu.log`). Client,
`test_client`, `test_render` and `test_scene` build. Architecture passes 47
modules, six programs, 33 layered modules and six fixtures. Formatting and diff
checks pass. Thirteen temporary PPM captures were removed after retaining the
comparison. No product walk rerun, sanitizer run or release profile is claimed
for this change.

`CollectPortalEyeGeometry` now maps ordinary source rows through mouths that
lead directly to the requested destination. It excludes foreign and synthetic
rows, omits native same-world geometry, compacts skin palettes and refuses
invalid or over-budget geometry transactionally. A body claimed by multiple
matching mouths is refused rather than copied into an arbitrary chart. The
usual native prefix is borrowed; only interleaved composition needs a filtered
row copy. Collection has a Render profiling scope. No release timing or heap
improvement is claimed from this structural change.

`PortalImageHost::SubmitEye` accepts an explicit `PortalEyeGeometrySource`, so
the body world need not be the image reply owner. Client supplies its presented
rows and joints from the input world for both selected and prefetched eyes.
Readiness calls without draw rows still work. Refused collection preserves the
last image and emits a refusal counter plus a debug diagnostic.

The resident GPU fixture uses distinct body, reply-owner and destination worlds.
Its negative control has no crossing body; supplying source geometry shows it,
first-person selection hides it, clearing selection restores it, and leaving
the seam removes it. Malformed skin input preserves the completed image. The
fixture retains one imported image and reports zero image upload bytes.
`eye-crossing-comparison.png` records the five inspected phases. An initial
fixture used the opposite side of the copied clipping plane as its eye, so it
correctly could not see the kept half. It now derives the destination eye and
backdrop from that plane's side. A build also caught an incorrect metric field
name in the test; it uses the existing `Images` counter.

Focused geometry demand and GPU checks pass 1,323 assertions in ten cases
(`eye-crossing-focused.log`). Render headless passes 33,030 in 444
(`eye-crossing-headless.log`), Client headless 10,055 in 171
(`eye-crossing-client-headless.log`), and relay headless 565 in six
(`eye-crossing-relay-headless.log`). Architecture passes 47 modules, six programs,
33 layered modules and six fixtures. Logs and images are under
`.cache/build/dev/tests/`.

The first 22-scenario product run failed 738 assertions in the 60 Hz third-person,
explicit-subject, non-held-adoption variant. Its producer relay refused a
directory and repeatedly restarted; remote eye images expired and the return
handoff failed. `eye-crossing-product.log` retains all results, and
`eye-crossing-failure.log` isolates the relevant trace. The 16 standard scenarios
then passed 110,830 assertions with the same test seed
(`eye-crossing-relay-product.log`). This rerun does not establish a fix for the
intermittent failure. Relay diagnostics now identify ungranted/colliding endpoints
and report terminal directory status, session, revision and endpoint count.

The subsequent 22-scenario diagnostic run passed 151,608 assertions, and the
broader focused GPU run passed 12,580 assertions in 37 cases. Capture inspection
still found a missing third-person avatar: the 60 Hz explicit-Humanoid capture
had zero yellow avatar pixels at frame 128 and 694 at adoption frame 129.
`eye-crossing-product-comparison.png` preserves that baseline. The old product
checks only required nonblack room pixels, so they did not detect this gap.

Client now uses an available source-world eye image while its camera character
hold is active. The local replica has already retired its body geometry at this
point. A missing image retains the local fallback. The existing product walk
test now requires yellow avatar pixels immediately before and at outbound
adoption for third-person follow cameras. Script-cleared cameras are excluded
from this body-in-view expectation. The 60 Hz explicit-Humanoid rerun shows 712
avatar pixels on both sides (`eye-retirement-60-explicit-comparison.png`).

This is not complete body continuity. The first 16-scenario rerun failed 828
assertions: two missing-avatar checks in the 30 Hz explicit-Humanoid case,
plus 826 checks after a producer failure in the 60 Hz first-person held case
(`eye-retirement-product.log`). `eye-retirement-failure-comparison.png` shows
the body absent even though the remote image is ready. Preserve geometry or
otherwise establish visual-body readiness across the producer's transfer gap;
an image handle alone does not prove it. Client headless still passes 10,055
assertions in 171 cases, and the current target graph passes architecture.

The first reruns had not relinked the server executable after adding relay
diagnostics. After rebuilding it, the 22-scenario run completed with 153,704
passing assertions and four failures, all the new missing-avatar check in the
30 and 60 Hz automatic-subject, non-held-adoption variants
(`eye-retirement-linked-product.log`). Camera-clear and image-handoff cases
passed. No producer disconnect occurred in this run, which does not resolve
the earlier intermittent rejection. `eye-retirement-linked-failure-comparison.png`
preserves frames 140 through 142 of the 30 Hz failure. Do not interpret the
earlier lack of detailed relay warnings as evidence that directory rejection
was excluded. Useful transition PNGs remain;
temporary BMPs are removed only after each completed scenario has been checked.
The final run retains 396 transition PNGs across 22 scenarios and no temporary
numeric BMPs. Eighteen temporary focused-GPU PPMs were also removed after their
comparisons were inspected. No sanitizer or release performance claim is made.

The source-owned interval now has a continuous body-pixel assertion, not only
the two adoption samples. A focused 30 Hz automatic-subject walk reproduces
34 missing-body assertions (`body-continuity-product.log`: 6,983 passing of
7,017). Run this exact variant without the full matrix:

```sh
.cache/build/dev/tests/test_client '[portal-product-walk]' -g 0 -g 0 -g 0 -g 0 --rng-seed 1224913465
```

The four generator indices select tick rate, first-person mode, explicit subject
and held adoption respectively. The installed Catch2 supports this filtering;
no duplicate test or temporary source edit is needed to select a variant.

`body-probe-product.log` records a temporary information-level capture trace.
The retained diagnostic is now trace-level `portal image prepared`, with world,
request, reply channel, sampled tick, total rows, native-rig rows, eye selection
and camera position. Its row scan is skipped when trace logging is disabled.
Do not equate zero native-rig rows with no copied geometry: imported rows carry
no native entity handles. The row count and request geometry provide that context.

In that diagnostic run the body disappears at frame 87 while the source still
owns it and its predicted root has passed z=-3.99. It reappears at frame 121,
disappears again over frames 130 through 155, and returns at 156. The nested
destination capture at 01:58:09.009 has one floor row and no native rig. Its source
parent captures two room rows at 01:58:09.044; the viewer accepts that image at
01:58:09.124. Destination native body rows are present in a later nested capture
at 01:58:09.209. Source request 4 is replaced by request 5 when adoption restarts
the carrying connection at 01:58:09.276; the next source image arrives at
01:58:09.557. `body-probe-gap-comparison.png` preserves both gaps.

The next implementation must therefore cover three connected intervals:

- Keep the predicted body visible after it fully clears the mouth, before its
  authoritative transfer commits. Straddling-only geometry collection ends too early.
- Retain the owned body presentation and animation through source retirement,
  carry its named identity and mapping into nested remote captures, and suppress
  duplicate destination-native rows for that same presentation identity.
- Preserve body continuity while changing the presentation connection and
  draining or replacing in-flight captures. A ready room image is insufficient;
  freezing the whole eye or delaying the physical crossing is not completion.

The trace build and final client/render rebuild pass. Render headless passes
33,031 assertions in 444 cases (`body-continuity-render-headless.log`). The
continuous visual check intentionally remains failing until these intervals
are covered; the previously passing room checks do not establish seamlessness.

Remaining connections include explicit geometry in Studio's whole-eye caller,
geometry needed by remote child captures, and avoiding duplicates when the
native destination character arrives while source presentation is retained.
Verify source-retirement geometry and animated limbs at actual product seams,
not only manually supplied rows. Resolve the intermittent producer refusal with
its full diagnostic sequence. Sanitizers and release profiling remain open.

Predicted-body continuation now keeps far-side clones after a fitted root sweep
fully clears the mouth. Client updates local `scene.PortalBodyView` from the
presented prediction before cutting native rows. The saved entry normal keeps
the same clipping half while the source still owns the body. Return crossings,
root retirement, nonfinite positions and changed mouth geometry discard the
history. Stationary roots without a crossing skip the seam walk; seam collection
reuses scratch storage. These are implementation choices, not measured speedups.

The resource has an empty snapshot codec because its entity handles and interned
names describe local presentation history. Restoring a snapshot resets it.
An initial codec omission caused three Client snapshot assertions to fail;
`body-view-fixed-headless-client.log` now passes 10,055 assertions in 171 cases.
The focused scene test exercises both directions and scales 1 and 2, aperture
fit, one-way rejection, foreign rows, mouth changes, retirement and snapshot
reset: 412 assertions pass. All scene tests pass 517,630 assertions in 549 cases.
Render headless passes 33,031 assertions in 444 cases.

A shuffled GPU run also reproduced fallback registration of `world::Replica`
before world startup. Optional replica lookup now checks the canonical registry
entry before touching the typed resource. The authored-demand test subsequently
registers mailbox types to exercise that startup order. Repeating seed
2752013840 passes 5,880 assertions in 18 focused cases
(`body-view-registration-gpu.log`). The dev build, architecture graph and its
six fixtures pass.

Product continuity remains open. The first repeat with body continuation had
10 missing-body checks at frames 87 through 96 (`body-view-repeat-product.log`);
its comparison also shows partial bodies at other frames. The final 30 Hz
third-person run has 7,373 passing assertions and 18 failures at frames 36
through 53 (`body-view-registration-product.log`). Its retained comparison
shows an unready gray mouth during this early interval, then a visible body.
Timing differs between runs, so this does not prove the later gap is resolved.
The useful comparison and transition PNGs are retained; each completed run's
600 temporary BMPs are removed after inspection. No release profiling or
sanitizer validation was performed for this increment.

The 30 Hz first-person automatic-subject variant also fails: 7,449 of 7,511
assertions pass (`body-view-firstperson-product.log`). Frames 34 through 64
have no whole-eye image and fail the nonblank image check. The retained
`body-view-firstperson-comparison.png` shows blank frames, destination imagery
at frame 65, and body geometry near the camera at frame 140 that still needs
selection/occlusion investigation. Its 600 BMPs were removed after inspection;
21 transition PNGs remain. This run does not validate first-person continuity.

Client now continues bounded neighbour-eye prefetch when camera routing is
unresolved. The resolver leaves the input eye unchanged on failure, so the
fallback gathers mouths from the input world and maps that eye into its nearest
eligible neighbour. This keeps destination image requests independent of the
topology reply; the two viewport slots and aperture-distance limit still apply.
`eye-prefetch-firstperson.log` confirms a destination image request at
02:47:41.169 while camera routing waits for topology through 02:47:41.285.
The previous path stopped prefetch altogether during that wait.

The dev build and 10,055 headless Client assertions in 171 cases pass. The
30 Hz first-person product run still fails 38 assertions across 19 blank frames,
35 through 53, with 7,145 passing assertions. Discovery is still missing for
the first part of that interval. Startup timing differs between runs, so the
smaller failure count is not a performance measurement or a continuity pass.
`eye-prefetch-firstperson-comparison.png` preserves the blank interval and the
later body-selection problem. Temporary BMPs are removed after inspection.

Capture metadata now records `eye_rig`, optional `eye_player` and the local
player's `player_user_id`. Product walk assertions require the selected account
to match during first-person follow, including held subjects and both adoptions;
third-person and cleared cameras must carry no selection. A diagnostic run
confirms identity 1 survives both crossings. Its body-bearing pixels lie inside
the destination portal image, so they do not alone prove primary-eye exclusion
is broken. Native/copy selection in the nested capture still needs verification.

The rebuilt first-person walk passes 8,388 assertions
(`eye-selection-check-first.log`). The third-person walk passes 8,164 of 8,241;
all 77 failures are existing body-pixel checks, not account-selection checks
(`eye-selection-check-third.log`). Its comparison shows the body present at
frame 111, but the adoption check reported there samples `bodyFrame` 110.
Distinguish the test's current frame from the preceding image it checks.
Startup timing varied, so the passing first-person rerun does not close the
previously reproduced readiness gap. Three inspected comparison sheets remain;
1,800 temporary BMPs were removed. The build and diff whitespace check pass.

Portal preparation now flushes the play presentation stream immediately after
collecting image/topology work. Previously `Client::Run` sent it only after
`Step` finished rendering and writing capture files. The early flush lets the
producer begin while those operations run; the final loop pump still handles
frames without an eye and later connection work. Directory ordering and stream
backpressure continue through the existing `PumpPlayPresentation` path.

The dev build and 10,055 headless assertions in 171 Client cases pass
(`eye-early-send-build.log`, `eye-early-send-headless.log`). The 30 Hz
third-person walk passes 7,966 of 7,994 assertions: 26 continuous body checks
and two adoption samples fail (`eye-early-send-third.log`). The inspected
comparison preserves the unready mouth at frame 35 and absent body at frames
87 and 88, with the body visible again at frame 140. Useful PNGs remain and
600 temporary BMPs were removed. No latency improvement is claimed from these
varying startup runs; endpoint readiness and source-retirement geometry remain
required work.

The held camera now retains a bounded `scene.CameraBodyPose` in its replica
store. Collection saves only native body rows and their rebased skin palettes
after skinning and before clipping. On retirement it replaces remaining source
body rows with that pose, carried by the current presented root. It creates no
extra script or physics instances. Storage is capped at 256 rows and 4,096
joints, reuses vector capacity, preserves the last valid pose on refused input,
resets on snapshot load and is removed with the camera hold. The source root's
local drawing identity remains valid for seam history and first-person selection
while the held root lives; it is not treated as a live source ECS entity.

This retains the last limb and skin pose, not a running animation player.
Animation continuation, copied geometry in remote child captures and arrival
deduplication remain required. In particular, a source-world whole-eye request
still does not carry its held rows into the nested destination capture, so this
storage change cannot close every third-person gap by itself.

The dev client/server/test build passes. All scene tests pass 517,812 assertions
in 549 cases before the additional malformed-palette and row-budget assertions;
the final focused scene run passes 803 assertions in three cases
(`body-retain-validated-scene.log`). Focused portal GPU coverage passes 5,880
assertions in 18 cases (`body-retain-gpu.log`).
Headless Client passes 10,055 assertions in 171 cases. Eye-body tests pass 946
assertions in nine cases. Architecture passes 47 modules, six programs and six
fixtures. No sanitizer or release performance measurement was run.

The 30 Hz third-person product run has 7,538 passing assertions and ten failures
at frames 87 through 96 (`body-retain-product.log`). The first-person run has
9,229 passing assertions and 42 failures across blank frames 33 through 53
(`body-retain-first.log`). Account selection checks pass in both. Inspected
comparison PNGs remain; 1,200 temporary BMPs were removed. These runs leave
visual continuity open, including partial-body images during the held interval.

Incoming request geometry now propagates into nested portal requests through
`ForwardPortalDraws`. It maps poses, extents and clipping planes using the child
mouth, preserves diagnostic names and canonical account identity, and compacts
local skin palettes. A matching accepted clipping plane permits a body already
fully beyond the mouth; an unrelated plane cannot borrow that continuation.
Unclipped geometry still needs to straddle and fit the aperture. Older copied
rows for the same player in the child payload are replaced. Row and joint limits
are checked before growth, including repeated references to the same input
palette; refusal leaves the child payload unchanged. Collection runs once per
parent request and has a profiling scope; no release timing is claimed.

Geometry tests pass 910 assertions in seven cases; render headless passes
33,070 assertions in 445 cases. A real two-renderer fixture passes 79 assertions
with a supplied body already beyond the parent mouth. The no-body baseline
passes 77 and shows the blue child wall; forwarding shows the red body, including
after the wall changes colour. The first fixture attempt clipped against the
mouth normal rather than the camera's entry side and failed four pixel checks;
correcting that fixture plane produces the expected result.
`body-forward-comparison.png` preserves the inspected baseline and forwarded
images. Build and architecture checks pass. The two temporary PPMs were removed.

Source-world eye collection now supplies the live local body or the held source
body, with its current clipping plane and source light direction. It omits other
worlds' rows and synthetic copies, caps rows before growing the temporary vector,
and encodes canonical account identity even after source retirement. Requiring
the retention cache initially left the first request empty; request tracing
reproduced that gap. The live local character supplies the body before the hold
is prepared. `body-origin-live-third.log` confirms the first source-eye request
now carries 1,315 geometry bytes.

`AppendPortalDraws` can replace destination-native body rows for matching account
identities before appending the incoming pose. Held native poses use their held
account when the source root is gone. Foreign handles and synthetic copies are
excluded from this lookup. Removal affects the complete draw list, including
shadows, rather than only primary-eye ordering. Import reports appended and
replaced rows separately; net list growth no longer stands in for imported row
count. All payload validation precedes removal, so malformed input preserves
the previous rows and selection.

Focused export/replacement/forwarding tests pass 75 assertions in three cases.
The final render headless run passes 33,108 assertions in 447 cases. Client
headless passes 10,055 assertions in 171 cases, and focused portal GPU coverage
passes 5,889 assertions in 19 cases before the live-body export adjustment.
Build and architecture checks pass. No sanitizer or release timing claim is made.

Product continuity is still incomplete. The final third-person run passes
7,942 of 7,972 assertions, failing 28 continuous body checks and two adoption
samples. The failure intervals are frames 37 through 54 and 79 through 89
(`body-origin-live-third.log`); the adoption check includes its preceding image.
The inspected comparison retains the unready mouth and absent body during the
held interval despite nonempty request geometry. Live limb animation, startup
readiness and continuity across in-flight image replacement remain open.

The final first-person run passes 8,876 of 8,920 assertions, with missing eye
images and blank pixels at frames 34 through 55 (`body-origin-live-first.log`).
Player-selection checks pass. The inspected comparison retains those blank
frames and the first destination image at frame 56. Five product comparisons
were kept across this investigation; 3,000 temporary BMPs were removed after
inspection. Request traces now include geometry byte count and selected account
to distinguish missing input geometry from a late or stale returned picture.

Coincident cross-world clipping is now reproduced and fixed. The source cut
pass incorrectly applied same-world duplicate suppression to foreign halves.
An identity mapping therefore left the source row without its cut plane, which
also prevented a fully cleared row from forwarding into a child request.
The existing body-view case now covers overlapping worlds, both entry directions
and scales one and two. Before the fix it fails 20 assertions; afterward all
518,298 scene assertions in 549 cases pass. Render headless passes 33,108
assertions in 447 cases. The child-image GPU case passes 79 assertions, and its
inspected PNG retains the forwarded red body over the blue destination wall.
The temporary PPM was removed. The fix adds no storage or allocation.

Product verification remains incomplete. `body-coincident-third.log` passes
7,500 of 7,510 assertions, with ten missing-body checks at frames 87 through 96.
`body-coincident-comparison.png` shows a head before the gap and a complete body
after it. Those frames still use local source composition, before the retained
character interval, so this remaining gap is not solely a retirement problem.
The first-person run passes 8,388 assertions, but its early black captures precede
the joined-world checks; it does not establish startup readiness. Both image
comparisons are retained and 1,200 temporary BMPs were removed after inspection.
Build and whitespace checks pass. No sanitizer or release profile was run.

The remaining pre-retirement gap has a measured temporal cause. Source portal
requests 1 to 3 have zero geometry bytes; request 4 first carries 1,315 bytes.
Their respective request-to-import times are 131, 134, 133 and 134 ms. Capture
frames 87 to 96 display earlier images, and frame 97 first displays request 4's
handle 5. The retained timeline is `body-coincident-request-timeline.svg` under
the dev test output directory. This evidence comes from the existing completed
product run; no additional GPU run or performance benchmark was made.

Continuous local-body composition work, required before the crossing gate closes:

- [x] Define the copied-reply depth encoding and implement bounded codec and CPU
  inbox accounting. PIMG v9 keeps color/depth under one capture key and revision
  set, with independent hashes and bounded compression. Depth uses destination
  camera-forward distance, finite and positive, or positive zero for no surface.
- [x] Add an optional typed depth read to graph capture and pair the two outputs
  within one GPU submission. Copy tight R32F rows through the existing bounded
  staging buffer, or transfer both resident textures together with byte accounting.
- [x] Import copied color/depth together with finite-sample and digest validation,
  combined CPU/GPU limits, joint upload completion and replacement cleanup. Compare
  both attachment digests for reuse, including queued depth removal.
- [x] Connect paired capture to portal producers. Convert the built-in FarPlane
  background to the wire no-surface convention. Bind capture camera, revision and
  endpoint incarnation throughout production, renewal and resident publication.
- [x] Construct whole-eye sampling with the request and preserve it in the image
  binding. Its clip-w must match destination camera-forward depth; seam sampling
  includes seam scale. Do not attach a newer camera transform to an old reply.
- [ ] Produce depth consistent with final portal radiance, including nested
  apertures, transparency and lens processing. Raw G-buffer depth precedes these
  stages and is not sufficient evidence of final composition correctness.
- [x] Add an opaque image-pair compositor and expose paired imported eye depth to
  graph consumers. Compare fresh foreground color/depth against a retained room
  and write both selected outputs in one pass. Refuse absent or unpaired eye data.
- [ ] Add destination-depth composition for the current local body through the
  mapped aperture. Preserve near/far cuts, scale, oblique camera projection,
  destination wall occlusion and first-person exclusion. Color-only overdraw
  and drawing an uncut source body do not satisfy this requirement.
- [x] Allow canonical primary-body selection on seam captures as well as whole-eye
  captures. Keep other accounts, source/destination entities, shadows and child
  portal images intact. Ordinary seam demand continues to select no excluded body.
- [ ] Separate primary local-body ownership from copied remote observer/shadow
  geometry. Transfer that ownership across retained-source and adopted rigs by
  canonical account identity without duplicate visible bodies or simulation.
- [ ] Bind destination body-lighting inputs to the accepted capture and endpoint
  incarnation. Pending camera or lighting requests must not relabel the retained
  room. Compare non-emissive body materials, then cover body-to-room shadows and
  ambient occlusion separately; source viewport lighting is not a substitute.
- [ ] Reuse room images/depth while updating bounded body and palette buffers.
  Measure actual resident bytes, staging churn and transfer operations. Preserve
  the current request anti-starvation rule while optimizing transport separately.
- [ ] Exercise an intentionally delayed room reply while the whole body crosses
  and reverses, with a destination occluder and animated limbs. Require direct
  reference-image agreement for both halves, not just one yellow head pixel.
  Cover both camera modes, rotated/scaled seams, nested views, disocclusion,
  endpoint replacement and first image readiness before product closure.

Depth codec validation: `portal-depth-headless.log` passes 34,335 assertions in
449 render cases; `portal-depth-client.log` passes 10,055 assertions in 171 Client
cases. Coverage includes raw and compressed depth up to 512 by 512, truncation,
corruption, hash-valid NaN rejection, noncanonical clear samples and exact inbox
byte limits. `portal-depth-host.log` passes 4,686 assertions in nine host cases
using the rebuilt protocol; these existing GPU cases still exercise color-only
capture. The added hash-corruption test initially failed compilation because
`uint8_t` was assigned to `std::byte`; the corrected explicit conversion builds.
The implementation is a transport prerequisite, not a visual-continuity fix.
No sanitizer or release performance measurement was run.

Paired capture validation: `paired-capture-final-gpu.log` passes 12,407 assertions
in four resource-image cases. The resident/copy fixture covers both paired and
color-only output with identity and translated/rotated cameras. Copied depth is
four units across the wall, color matches the resident path, resident bytes include
both textures, color-only replacement releases depth, and final image retirement
returns the import accounting to zero. Capture copies share the existing fence;
no blocking wait was added to presentation. Existing failure/cancellation cases
remain passing. File capture still selects its color input when depth is declared.

`paired-capture-graph.log` passes 8,868 assertions in 224 graph cases,
`paired-capture-headless.log` passes 34,335 assertions in 449 render cases, and
`paired-capture-client.log` passes 10,055 assertions in 171 Client cases. Build,
whitespace and architecture checks pass (47 modules, six programs, 33 layered
modules and six architecture fixtures). No product walk was repeated because
portal producers do not yet request this pair. No sanitizer or release profile
was run. This closes capture storage/lifetime plumbing, not the body continuity
gate or final-depth composition.

Copied GPU import now owns both attachments. The staging limit includes the
maximum combined payload; row pitches and plane offsets are aligned, and counters
charge both vector capacities, resident textures and actual upload operations.
Same-sized textures are reused. Partial allocation failure releases new scratch
textures before changing the old pair. Invalid reply layout is refused before
mutating the import; depth-only updates acquire a new image handle.

An existing resident/copy fixture extended to repeated queued depth removal
reproduced four failures: the reuse check compared pending color against old
resident depth and invalidated the first returned handle. It now compares the
pending attachment set whenever an upload is queued. `paired-import-reuse-before.log`
retains the failure; `paired-import-final-gpu.log` passes 12,886 assertions in
seven resource/import/eye-image cases after the fix. Coverage includes paired
reuse, changed depth with unchanged color, malformed depth, color-only replacement,
aligned odd-sized capture, owner refusal and release accounting. These cases prove
upload and lifetime behavior; depth-driven body occlusion still needs its own
rendered comparison when the composition pass is connected.

`paired-import-headless.log` passes 34,336 assertions in 449 render cases and
`paired-import-client.log` passes 10,055 assertions in 171 Client cases. Build and
whitespace checks pass. No new product walk, sanitizer or release profile was run;
portal producer requests still use the color-only path.

Portal producers now export paired color and camera-forward depth. The depth
export runs inside the view block before presentation and owns a separate R32F
target. Its zero background does not overwrite the FarPlane background required
by lighting. Repeated zero-background exports are accepted while the lighting
depth stage remains a singleton. The copied path hashes both planes; resident
publication transfers both through the existing capture and renewal identity.

The first GPU runs exposed graph ordering, singleton and scope refusals. Placing
the export inside the view block and distinguishing its independent target fixed
those failures without widening the scope rule. The broader run then exposed 42
stale color-only CPU byte expectations; paired payloads correctly charge 12 bytes
per pixel instead of eight. `producer-depth-final-gpu.log` passes 19,192 assertions
in 21 producer, resource-image, import and eye-image cases. The focused producer
fixture checks zero background and wall distance for eye and oblique seam cameras.
Existing cases cover copied/resident renewal, cancellation and image replacement.

`producer-depth-headless.log` passes 34,339 assertions in 450 render cases,
`producer-depth-graph.log` passes 8,873 assertions in 224 graph cases, and
`producer-depth-client.log` passes 10,055 assertions in 171 Client cases. The build
and shader contract checks pass, including all 33 staged shader modules. No new
product walk, sanitizer or release profile was run. No temporary image captures
were needed for this numeric depth check. Final depth through transparency,
nested views and lenses, plus current-body composition, remain open.

Whole-eye demand now constructs the retained sampling matrix alongside its camera
request. The previous request-only builder left the host binding at identity: the
new off-axis, translated/rotated fixture reproduced clip-w = 1 for a point four
units ahead (`eye-depth-transform-before.log`, two failed assertions). The builder
now returns one demand with both values; all request-only callers use its Request
member. Refusal leaves both request and binding unchanged. Existing seam-angle
coverage also checks that clip-w equals destination camera-forward distance.

`eye-depth-transform-headless.log` passes 34,456 assertions in 450 render cases;
`eye-depth-transform-client.log` passes 10,055 assertions in 171 Client cases.
`eye-depth-transform-final-gpu.log` passes 5,382 assertions in 17 host and eye-body
cases, including real-process delivery and renewal. The first broad run caught
six old one-plane upload expectations in the process fixture. It now checks the
depth payload and two-plane upload count, including no additional upload on renewal.
These checks establish capture coordinates and transport behavior, not continuous
body occlusion. `eye-depth-transform-client-host.log` also passes 448 assertions
in the saved/replicated Client product host case, across eye/seam requests and
intentional disconnection. No shaders changed. No product walk, sanitizer or
release profile was run in this increment.

`depth-compose` now selects the nearer opaque layer in camera-forward units and
writes HDR color plus R32F depth together. Paired `eye-image` output supplies the
owned destination depth and refuses missing depth instead of retaining a previous
frame's target. Inputs resolve by authored port name, including shuffled edges;
nearest depth sampling preserves discontinuities. Texture allocation is lazy,
renderer-owned and released on shutdown. Counters report output bytes and eye
blit operations at the recording boundary.

The GPU body fixture compares separately composed opaque blocks with a direct
render. Both copied and resident room images pass front, behind and return poses
under identity/rotated cameras. It checks nonempty red wall and blue body pixels,
every output depth sample, no room re-upload and no new texture allocations after
warm-up. Missing and color-only images refuse composition. The inspected visual
comparison is `tests/body-depth-previews/comparison.png` under the dev build;
temporary PPMs were removed. This fixture isolates visibility with emissive blocks,
not animated Humanoids or destination lighting.

`body-depth-compose-visual.log` passes 29,074 assertions in the focused case.
`body-depth-compose-final-gpu.log` passes 52,596 assertions in 29 resource, portal
runtime, host and eye-image cases. `body-depth-compose-headless.log` passes 34,458
assertions in 450 render cases, and `body-depth-compose-client.log` passes 10,055
assertions in 171 Client cases. Shader validation passes all 34 staged modules;
architecture passes 47 modules, six programs, 33 layered modules and six fixtures.
The initial graph suite caught an incorrect Source flag on the compositor, with
eight assertions failing because it has required image inputs. After correcting
that flag, `body-depth-compose-final-graph.log` passes 8,878 assertions in 224 cases.
The preview conversion initially needed a corrected GLM packing include to build.

A final pair-extent guard requires matching color/depth sizes within each input
pair. `body-depth-compose-pair-check.log` and `body-depth-compose-pair-graph.log`
pass after that guard; the matching build is `body-depth-compose-pair-build.log`.

Product current-body selection and graph wiring remain open, along with camera
matching, aperture cuts, primary-copy exclusion, animation, transparent layers,
lenses and nested depth domains. No product walk, sanitizer or release profile was
run for this backend increment.

The request codec no longer restricts primary `EyePlayer` selection to whole-eye
projection. The existing field, canonical identity validation and source request
invalidation apply to seam captures too; the wire layout is unchanged. Default
seam demand remains empty, so receiver ownership must explicitly enable selection.
The expanded codec test first reproduced the seam refusal in
`seam-body-selection-before.log` (one failed assertion).

`seam-body-selection-focused.log` passes 88 assertions, exercising native and copied
Humanoid bodies, account changes, restoration and entity lifetime through real
seam captures. `seam-body-selection-gpu.log` passes 7,267 assertions in 23 runtime
and eye-body cases. The child-forwarding case now requests primary exclusion and
still sees the body in its child image, using both copied and resident delivery.
Existing native/imported shadow fixtures verify identical shadow depth while the
primary body is hidden, and a different shadow map after disabling the caster.
The inspected comparison is `tests/seam-body-selection-shadows.png` under the dev
build; temporary generated PPMs from these checks were removed.

`seam-body-selection-headless.log` passes 34,480 assertions in 450 render cases;
`seam-body-selection-client.log` passes 10,055 assertions in 171 Client cases.
The build and formatting checks pass. No shader changed, and no product walk,
sanitizer or release performance profile was run. Product body-layer readiness,
opaque capture profile selection and ownership handoff are still open; enabling
primary exclusion alone does not close the crossing gap.

The producer's `OpaqueLighting` graph now stops before sky and exports the
`lit` target after deferred lighting, paired with zero-background camera depth.
CompleteWorld retains its existing final HDR capture path. A live gravitational
lens fixture first reproduced opaque radiance contamination in
`opaque-profile-before.log` (47 assertions passed, one failed). The fixture loads
the lens shader explicitly and checks off/on/off restoration through independent
requests.

`opaque-profile-visual.log` passes 60 assertions in the focused GPU case, including
optional preview writes. The inspected comparison is
`tests/opaque-profile-previews/comparison.png` under the dev build. The four
intermediate PPMs were removed. `opaque-profile-gpu.log` passes 35,544 assertions
in 17 producer/compositor GPU cases, and `opaque-profile-headless.log` passes
34,480 assertions in 450 render cases. Build and formatting checks pass.

Product selection of this profile, body-layer readiness, ownership handoff and
transport of the later transparent/lens stages remain open. No product walk,
sanitizer or release performance profile was run for this increment. Scope-aware
cache signatures still need review: opaque capture should not be invalidated by
changes confined to omitted lens stages.

`eye-image` now accepts an explicit graph `scope`: `complete-world` by default,
or `opaque-lighting` for a body-layer graph. The selected scope must equal the
imported binding's scope. Before this change the opaque variants of the existing
body/wall fixture failed all four copied/resident and camera combinations:
`opaque-eye-before.log` has 29,084 passing assertions and four failures.

The expanded fixture checks both scopes, refusal of the opposite scope, absent
and unpaired data, wall occlusion, camera rotation, return movement and retained
room allocation/upload counts. `opaque-eye-focused.log` passes 58,100 assertions;
`opaque-eye-gpu.log` passes 77,192 assertions in 22 GPU cases.
`opaque-eye-headless.log` passes 34,479 assertions in 450 cases and
`opaque-eye-graph.log` passes 8,883 assertions in 224 cases.
`opaque-eye-client.log` passes 10,055 assertions in 171 Client cases after rebuilding
the client and test executable. Build and formatting checks pass. No shader changed and no new preview was needed for this scope check.

This closes the graph's opaque import restriction, not product body ownership.
`PreparePortalEye` still submits and displays CompleteWorld captures. The client
needs a complete layer set and mapped current-body composition before enabling
primary-body exclusion or replacing that display path. Product walk, sanitizer
and release performance verification remain open.

The retained-room composition fixture now includes six-part Humanoid rows from
`CollectInstances`, with a changing right-arm pose, a half-scale `SeamTransform`
and a world-plane cut. Direct and composed color/depth agree across front,
behind, changed-pose return and restored-pose return frames. The changed pose
must change image content and restoring it must restore that content. An uncut
reference must expose more body pixels, preventing a disabled clip from passing.
A CameraSubject targeting the Humanoid and LockFirstPerson controller select the
same rig/account through `SelectFirstPersonBody`; the resulting composed image
must equal the retained room pair exactly.

`humanoid-compose-final.log` passes 155,144 assertions across all 16 combinations
of body kind, capture scope, camera rotation and copied/resident room delivery.
Existing per-frame allocation and upload assertions remain enabled. Build and
format checks pass after correcting fixture includes and API spellings. The
inspected comparison is `tests/humanoid-compose-previews/comparison.png` under
the dev build; intermediate PPMs were removed after inspection.

This is a GPU compositor verification increment. No product path changed.
The fixture uses rigid articulated limb parts and emissive materials; skin-mesh
animation, destination lighting, finite aperture mapping, complete layer transport
and the delayed product walk still need verification. No sanitizer or release
performance profile was run.

Producer retirement exposed a readiness gap in copied images: closing or replacing
the producer endpoint left `CurrentImage` and GPU residency alive until timeout.
`producer-retirement-before.log` reproduced six failed assertions across the two
copied-image variants. Source polling now compares each producer's full receipt
with the current Universe directory, cancels pending work and drops retained
images when the receipt is gone or replaced.

`producer-retirement-focused.log` passes 466 assertions, covering copied/resident
delivery, close/replacement, idle/pending requests and fresh-image recovery from
the replacement producer. `producer-retirement-gpu.log` passes 11,674 assertions
in 25 runtime/host cases. Headless render and Client checks pass 34,480 assertions
in 450 cases and 10,055 assertions in 171 cases, respectively. Build and formatting
checks pass; runtime fixture PPMs regenerated by these checks were cleaned.

This enforces retirement at the image-source boundary. Combined product walks
with successor disconnection, directory propagation delay and lost cancellation
acknowledgements still require verification. No release profile or sanitizer run
was performed for this change.

Re-running the standalone successor process fixture against the current client
exposed 24 failed assertions in `successor-retirement-product.log`. The fixture
rejected pre-commit presentation discovery, retained stream framing across a
replacement connection, omitted successor producer routes, and expected source
input to stop at commit instead of adoption. These conflicted with the current
handoff contract and prevented the fixture from supplying required services.

The existing fixture now publishes producer routes on both authenticated
connections, serves bounded uniform HDR replies for the portal case, and starts
a fresh stream/directory when replacing its listener. Pre-commit consumer
ownership must remain empty. Portal-free scenes still send no eye request or
reply endpoint. Source input must continue during delayed refusal/reconnect
handoffs, then quiesce after destination input takes over; the check allows
200 ms for in-flight source packets and requires over one second of destination
input observation. Original source/destination Fresh counts and all session
outcomes remain asserted.

`successor-input-continuation.log` passes 3,013 assertions across all six real
Client subprocess scenarios: normal, source identity refusal/retry, destination
Resume/Commit refusal, and connection loss during Resume/Commit. All six log
successful adoption. The preceding corrected-fixture run also passed, before
adding the positive source-continuation assertion. Build and formatting checks
pass. The temporary fixture configuration was removed after the processes exited.

This verifies session/discovery wiring and input routing with synthetic image
services. It does not verify complete physical crossing pixels, actual supervised
producer withdrawal during the walk, camera motion continuity, or cancellation
acknowledgement loss. Those combined product checks remain open.

The physical image-handoff walk exposed a grey portal pane when the returned
replica became native before its portal images arrived. Native frame preparation
now warms demanded portal images before display selection. It retains an already
owned whole-eye image for the same authored world until those images arrive,
without requesting a whole-eye fallback for a cold viewport. Caller spans are
refreshed after portal/surface collection. The walk now checks retained scene
pixels at both adoptions, requires every external portal in the returned native
view to own an image, and requires actual native return samples.

`native-portal-warm-product.log` passes 15,010 assertions at 30 and 60 Hz.
Before/after frame records show missing returned-native portal images falling to
zero; `native-portal-warm-comparison.png` retains the visual evidence. This closes
the grey pane, not temporal pixel parity between retained and native views.

The broader camera walk then reproduced an intermittent producer refusal:
`native-portal-warm-cameras.log` failed 814 assertions when a valid new directory
arrived during an open coordinated tick frame. Endpoint registration returned
`WrongHost`; cleanup also ran inside the frame, cleared relay ownership without
closing the endpoints, and made replacement producers collide with those leaked
endpoints. The deterministic `relay-frame-before.log` reproduces five failures,
including the leaked endpoint after disconnect.

The server now defers its entire supervised producer pump while that frame is
open, including process polling, restart and cleanup. The relay separately defers
transport consumption and disconnect cleanup until the frame ends.
`relay-frame-focused.log` passes 588 assertions in seven cases, including new
directory delivery and disconnect during an open frame. Full headless world
checks pass 31,005 assertions in 244 cases. `relay-frame-product.log` passes
47,435 assertions across all six first/third-person camera-clear and image-handoff
round trips at 30/60 Hz, with no producer-loss or directory-refusal warning.
`relay-frame-server.log` passes 204 assertions for the server round trips.
Build and formatting checks pass. Failure/recovery PNGs and selected frame records
are retained; temporary frame sequences are cleaned after inspection.

These checks do not close the delayed articulated-body composition, full layer
transport, all-angle Studio/demo verification, combined producer replacement walk,
or release profiling and sanitizer work listed below.

Checking the next body-composition boundary exposed inconsistent paired-image
resampling. The eye-image node linearly filtered color while selecting nearest
depth, combining radiance from different surfaces under one surface distance.
`eye-pair-resize-before.log` reproduces 67 inconsistent pixels from a two-by-two
image enlarged to 13 by seven. The node now selects nearest samples for both
planes when exporting paired data; color-only display keeps linear filtering.

The GPU fixture checks four distinct color/depth samples, including zero-depth
background, through copied import and resident replacement at enlarged and
reduced output extents. The expanded initial run also caught two fixture failures:
resident adoption already replaces the matching imported owner, so dropping the
old handle again was invalid. The corrected fixture asserts one remaining image.
`eye-pair-resize-gpu.log` passes 155,882 assertions across paired resizing and the
existing opaque articulated-body compositor case. The comparison PNG retains the
invented mixed-color edge before the fix and the surface-consistent result after
it. Point reconstruction loses smoothing; edge-aware reconstruction, finite
aperture composition and the delayed product body layer remain open.
Headless render checks pass 34,479 assertions in 450 cases. Build, formatting and
whitespace checks pass. Temporary preview PPMs were cleaned after inspection;
no sanitizer or release performance profile was run for this change.

The existing opaque body fixture now exercises a finite portal mouth with a
source foreground strip. Its reference directly renders the complete articulated
body and destination wall behind a physical opening, without a PortalView or
imported image. The initial image-only composition failed at two oblique-mouth
pixels in both copied and resident variants: those body pixels lie in front of
the portal plane and project outside its opening. `body-aperture-gpu.log` retains
the two failed comparisons; `body-aperture-before.png` shows the missing near half.

The fixture now splits current body rows into complementary plane-clipped halves.
It renders the far half against retained room depth through the existing opaque
compositor, samples that image through the mouth, and draws the near half in the
source view. The independent direct reference remains a complete body. Pixel
checks require visible room, source frame and body inside the aperture, plus
outside-aperture body pixels in the oblique third-person case. A second pass uses
the real Humanoid camera's first-person rig selection and requires both body
halves absent while the room remains visible.

`body-aperture-final-gpu.log` passes 155,554 assertions, including the existing
articulated compositor variants and the new front/oblique, copied/resident,
first/third-person comparisons. `body-aperture-comparison.png` retains the checked
images; temporary PPMs were cleaned. Build and formatting checks pass. This is a
render-path prototype with emissive opaque geometry and a fixed retained camera,
not product integration. Cross-world scale, camera disocclusion, moving lighting,
transparent/lens layers, body ownership during delayed transport and full crossing
reversal remain open. No release profile or sanitizer run was made.

The aperture prototype now uses `SplitPortalBodyDraws` in the shared render
library instead of its own split loop. This bounded helper preserves the selected
source-side plane while rows move, maps the far geometry/plane/light direction,
clears source-world tag bits on that half, and compacts repeated skin ranges once
for both outputs. It refuses unrelated prior cuts, synthetic copies, invalid or
collapsed geometry, palette overflow and input/output aliasing before mutation.
Successful calls reuse output storage; it does not choose body/session ownership.

`body-split-final-focused.log` passes 5,895 assertions across scale factors
0.25/1/4, both source sides, rotated/translated mappings with a nonzero origin,
motion across the fixed plane and back, shared palettes, storage reuse and
transactional refusal. The preceding full headless render run passes 40,373
assertions in 452 cases; the final focused run adds the explicit palette-alias
check. `body-split-final-gpu.log` passes 155,274 assertions using the helper in
the existing body and finite-aperture comparisons. The direct GPU reference keeps
its original palette, independently of the helper's compacted skin indices.
Build, formatting and whitespace checks pass. No images were regenerated and no
release profile or sanitizer run was made. Actual scaled GPU mapping, skinned
mesh visuals, retained capture-camera binding and product ownership remain open.

Retained-image camera metadata now has its own acceptance boundary.
`PortalImageSource::Capture` returns the image handle, producer incarnation,
accepted binding/sampling matrix, pose/frustum/clip plane, excluded player and
image extent. A preview retains those values separately from its pending
request. Copied import, resident acceptance and confirmed renewal update them
together. Expiry and endpoint withdrawal make the snapshot unavailable.

`retained-camera-gpu.log` passes 12,695 assertions in 25 runtime/host cases,
including initially absent snapshots, accepted metadata, camera changes while an
older image is retained, replacement, renewal and final cleanup. Headless render
checks pass 40,374 assertions in 452 cases. Additional focused eye and seam runs
change the requested excluded player as well as camera/matrix, require the old
metadata while pending, and require the new values only after acceptance.
Both copied and resident variants pass in `retained-camera-eye.log` and
`retained-camera-seam.log`. Separate runs were used because the combined section
selection exercised only one section. Build and formatting checks pass; temporary
runtime previews were cleaned. No release profile or sanitizer run was made.

This supplies immutable inputs for the next body-view construction step. The
product compositor does not consume the snapshot yet, and scaled GPU mapping,
complete radiance layers and delayed physical crossing remain open.

The producer's private camera reconstruction is now available through
`ResolvePortalCaptureCamera` for accepted capture snapshots. Producer rendering
uses the same entry point. It validates finite/unit pose data, ordered frustum
bounds, eye/seam clip rules and an invertible explicit projection before updating
only the destination view's camera fields. This avoids a second interpretation
of asymmetric frusta, oblique clipping and camera-forward depth in body views.

`capture-camera-focused.log` passes 216 assertions for off-axis frustum-edge rays,
a tilted clipping plane, camera-forward clip-w at 0.25/1/4 scales, retained view
ownership fields and transactional invalid-input refusal. `capture-camera-gpu.log`
passes 12,699 assertions in 25 producer/runtime/host cases using the shared path.
Headless render checks pass 40,590 assertions in 454 cases. Build and formatting
checks pass; runtime previews were cleaned. No release profile or sanitizer run
was made. The mapped body renderer still needs to consume accepted snapshots;
this camera path alone does not close scaled GPU composition or product crossing.

The finite-aperture prototype now exercises actual rotated and translated seam
mapping at scales 0.25, 1 and 4. It reconstructs the capture camera through the
shared resolver, maps the far body and wall into destination space, and samples
the result through the source aperture. The direct reference remains an ordinary
complete body behind a physical opening. First-person Humanoid selection and
third-person cuts pass straight and oblique comparisons in copied and resident
variants. Both intermediate resident transfers add zero uploads; independent
readbacks remain in the fixture for visual comparisons.

`eye-image` now has an explicit `projection` selector, defaulting to `eye`.
The seam composition graph selects `seam`; tests reject mismatched captures in
both directions. `scaled-body-resident-gpu.log` passes 156,648 assertions in two
cases, including paired image resizing. `scaled-body-headless.log` passes 40,591
assertions in 454 cases and `scaled-body-graph.log` passes 8,888 assertions in
224 cases. Build and formatting checks pass. `scaled-body-comparison.png` retains
the inspected direct/composed scale comparisons; temporary PPM captures were
removed. No release profile or sanitizer run was made. Product consumption of
accepted snapshots, full radiance layers and delayed physical crossing remain
open.

The product adapter now exposes `PortalImageHost::Capture(viewSlot, portal)`.
This forwards the source's accepted snapshot without a second metadata cache.
The existing two-viewport resident fixture verifies distinct cameras and owners,
old camera/key retention during pending replacement, atomic accepted replacement,
snapshot copy stability, viewport removal isolation and expiry. An initial test
incorrectly compared the draft request's zero request ID with the assigned ID;
the corrected assertion checks assignment and camera revision separately.
`host-capture-final-gpu.log` passes 4,715 assertions in nine host cases.
`host-capture-headless.log` passes 40,590 assertions in 454 render cases. Build
and formatting checks pass. This exposes the product input; it does not yet
connect local body composition to the client's displayed eye.

The opaque body fixture now includes non-emissive blocks and six-part Humanoids
under directional sunlight. Identity/rotated cameras, copied/resident pairs,
front/behind/return poses and changed arm poses match direct rendering. A
light-off probe darkens the current body while preserving retained wall radiance
and composed depth; restoring the light restores the image exactly without new
room uploads. `body-lighting-final-gpu.log` passes 317,290 assertions in the
expanded existing case. Build and formatting checks pass. The inspected
`body-lighting-comparison.png` retains three direct/composed Humanoid poses;
temporary captures were cleaned. These checks disable shadow casting and ambient
illumination. They do not establish ambient occlusion, dynamic room shadows,
lighting metadata transport, full radiance layers or product integration. No
release profile or sanitizer run was made.

PIMG v10 now transports optional captured sunlight through copied replies,
resident receipts and renewals. Its 48-byte payload contains the evaluated sun
direction, ambient terms and direct intensity as explicit floats. The codec
rejects nonfinite colors, negative intensity and nonunit directions, with
transactional output on malformed or truncated messages. Producers attach the
same evaluated lighting used for rendering. Accepted capture snapshots expose
it without a second lighting cache. Pending camera/light changes keep the old
metadata, and matching-revision renewals with altered sunlight are refused.

The first GPU run caught a missing snapshot assignment. The next caught failure
replies retaining sunlight after rendering was refused. Both are corrected;
failure replies now clear that metadata before encoding. The nested resident
traffic bound accounts for two additional 48-byte records, while its zero-pixel
transfer and pending-byte checks remain unchanged.
`capture-sun-validated-gpu.log` passes 12,975 assertions in 25 runtime/host cases;
`capture-sun-validated-headless.log` passes 41,666 assertions in 455 render cases.
Directional metadata is implemented, but point lights, fog, shadow ownership,
complete radiance layers and product body composition remain open.
Client and server were rebuilt with the new wire version. Headless product
checks pass: 10,055 assertions in 171 client cases and 1,459 assertions in
76 server cases (`capture-sun-client.log` and `capture-sun-server.log`). Formatting
and whitespace checks pass; six runtime previews were removed. No release
profile or sanitizer run was made.

PIMG v11 extends the accepted lighting payload to fog and the exact bounded
point/spot-light list selected by the producer's render view. `PortalCaptureLighting`
replaces the sunlight-only type. It carries at most 16 local lights in destination
units, with a compile-time check against `MAX_SCENE_LIGHTS`. No additional metadata
cache or dynamic light-vector allocation is introduced. Wire cost is 69 bytes plus
44 per light. Codecs validate finite values, positive ranges, cone bounds, unit
spot directions, ordered fog distances and canonical unused array entries.

The expanded codec case round-trips all 16 lights and maximum-length portal keys
through copied replies, resident receipts and renewals; excessive counts are
rejected before array access. The producer/runtime case adds both a point and a
spot light during a pending camera replacement and verifies retained old metadata
and accepted new positions, brightness-folded colors and ranges. Headless render
checks pass 41,779 assertions in 456 cases (`capture-lighting-headless.log`);
runtime/host GPU checks pass 12,991 assertions in 25 cases
(`capture-lighting-gpu.log`). Shadow maps, environment texture ownership, full
radiance layers and product body composition remain open.
Client/server builds pass with PIMG v11. Headless product checks pass 10,055
assertions in 171 client cases and 1,459 assertions in 76 server cases
(`capture-lighting-client.log`, `capture-lighting-server.log`). Formatting and
whitespace checks pass, and six runtime previews were cleaned. No release
profile or sanitizer run was made.

`ResolvePortalCaptureLighting` now applies validated capture shading terms to a
render view with caller-owned storage for local lights. The shared validator
serves codecs and view preparation, without an intermediate copy or allocation.
Invalid data or short storage leaves both view and storage unchanged; successful
preparation preserves camera/owner and unrelated environment/layer fields. The
producer uses the same path before rendering and sends invalid-lighting failures
through its bounded failure queue.

The runtime case injects a NaN local-light range after accepting a valid camera
replacement. It requires zero rendered captures, one failure reply with the
lighting diagnostic, and preservation of the last accepted image/lighting pair.
`resolve-lighting-headless.log` passes 41,812 assertions in 457 cases;
`resolve-lighting-gpu.log` passes 13,007 assertions in 25 runtime/host cases.
Client/server builds pass. Formatting and whitespace checks pass; six runtime
previews were cleaned. Product walk and product suites were not rerun for this
render-only conversion change.
These checks do not close product body composition, shadow/environment ownership
or complete radiance layers. No release profile or sanitizer run was made.

`graph::DefaultPortalBodyDocument` now owns the reusable opaque body graph, with
explicit eye/seam projection and `opaque-lighting` room scope. It captures
composed HDR and camera-forward depth at `export`, stopping before sky and later
world layers. The GPU fixture's duplicate composition graph was removed; its
independent direct reference remains. Opaque-only CompleteWorld fixtures adapt
the scope in test code, while the shared graph requires the opaque profile.

Graph compilation, paired output formats, projection/scope parameters and text
round-trip checks pass in `body-graph-headless.log`: 8,922 assertions in 225 cases.
The existing body, scaled-aperture, lighting and paired-resize GPU checks pass
317,772 assertions in two cases (`body-graph-gpu.log`). The inspected
`body-graph-comparison.png` retains direct/composed oblique views at 0.25/1/4
scales; temporary previews were cleaned. Build, formatting and whitespace checks
pass. Product suites, release profiling and sanitizers were not rerun for this
graph extraction. Product ownership/wiring and complete radiance layers remain
open.

Repeated resident body exports reproduced two new texture allocations per frame
after warm-up (`resident-reuse-before.log`). Replaced image pairs now enter a
four-slot cache keyed by exact extent and depth presence. The next resident
export can reuse a detached pair; writes remain ordered after prior sampling on
the renderer submission queue. No extra wait or CPU readback was introduced.
Cached payload is reported by `CachedTextureBytes` and its metric gauge, while
reuse operations have a counter. The cache has a 12 MiB maximum, shares the
32 MiB imported-texture budget and is evicted under allocation pressure. Last
image removal and renderer shutdown release it.

The fixture verifies flat allocation counts after warm-up while color and depth
change, zero uploads, five extent changes, bounded eviction, fresh final pixels
and depth, and zero cached bytes after removal. Its oracle reads back only after
the resident sequence. Resource/runtime/host GPU checks pass 342,714 assertions
in 32 cases; import ownership checks pass 473 assertions in three cases.
Headless render checks pass 41,813 assertions in 457 cases. The rebuilt product
image-handoff test passes 15,774 assertions across 30/60 Hz world rates, including
both physical adoptions and native return-image readiness (`resident-reuse-walk.log`).
The inspected `resident-reuse-handoffs.png` and its JSON preserve adjacent frames
around both adoptions. Temporary sequences were cleaned. Build and formatting
checks pass. These are dev correctness/allocation checks, not release timing or
sanitizer evidence. Product local-body ownership/composition and complete
radiance layers remain open.

The real GPU budget-pressure fixture now covers both eviction paths. Nine copied
512-square HDR/depth pairs displace the optional cache at 30 MiB live payload.
A later colour-only resident adoption displaces a mismatched paired cache at
31 MiB live payload. Both stay within the shared 32 MiB limit and final removal
leaves no live, cached or pending image bytes. The first run exposed an incorrect
test expectation: upload operations count colour and depth separately, so nine
pairs mean 18 operations. After correcting that expectation, resource-image,
import and eye-image GPU checks pass 330,238 assertions in 11 cases
(`resident-budget-gpu.log`). No production code changed for this check. Existing
comparison PNGs remain; this run did not request temporary preview sequences.

Dropping the source's cancellation acknowledgement reproduced a stuck product
Client: Proceed, Resume, Commit and destination input never followed
(`portal-cancel-before.log`, 18 passed / 5 failed assertions). Cancellation now
retries the same attempt every half-second until acknowledgement, retaining the
source authority and allowing the subsequent offer to proceed. The one-shot
`CancelSent` flag was replaced by a monotonic retry deadline.

The real Client subprocess with authenticated source/successor listeners passes
all seven outcomes, including the dropped acknowledgement, application refusal
and transport loss: 3,017 assertions in two cases (`portal-cancel-fixed.log`).
The real Server source-lease fixture also repeats cancellation after departure
retirement and checks the source player remains enabled with no destination
arrival: 310 assertions in one case (`portal-cancel-server.log`). Headless Client
checks pass 10,055 assertions in 171 cases. Builds, formatting and whitespace
checks pass. These fixtures cover protocol/adoption, not image fidelity; no image
sequence was generated. Destination-host cancellation loss, readiness expiry,
product local-body composition and complete radiance layers remain open. No
release profile or sanitizer run was made for this change.

Readiness coverage now withholds the first successor snapshot while keeping the
authenticated connection and presentation discovery live. After the actual
15-second Client deadline, the fixture checks the timeout diagnostic, source
player survival, cancellation and a later successful transfer. A second case
restarts the authenticated successor before its first snapshot and requires the
connection-ended diagnostic followed by the same recovery. The initial expiry
run found a fixture bug: presentation framing was reused across different
connection identities. The fixture now resets that state per connection.

All nine product successor outcomes pass 3,292 assertions in four cases
(`portal-readiness-all.log`). A Server source-lease case drops destination
transfer replies for 40 ticks, observes at least two drops and a still-held
Cancelling rig, then allows acknowledgement and verifies recovery without a
destination player. Its six outcomes pass 363 assertions in one case
(`portal-destination-cancel.log`). Build, formatting and whitespace checks pass;
a missing namespace qualification in the test was corrected during the build.
These are headless protocol checks with authenticated connections, not rendered
image checks. No image sequence, release profile or sanitizer result was added.
The combined physical walk under these faults remains open.

The existing GPU depth compositor now has a `transparent` mode. A caller feeds
ordered straight-alpha foreground radiance/depth over premultiplied background
radiance, back to front. Visible layers blend without changing the opaque depth;
zero depth is empty and ties retain the background. The opaque default remains
unchanged. The mode is declared in the graph catalogue and uses the same paired
textures, pipeline and allocation path with one small fragment uniform.

The GPU fixture applies two independently captured layers over empty depth, a
near body, an exact depth tie and a far wall. It checks every color/depth sample
at zero, half and full opacity, including a transparent empty background. The
first run exposed an oracle mismatch between the full PBR reference's far-plane
clear and the body graph's zero-depth export; the reference now distinguishes
empty samples. The compiler also caught a temporary document lifetime in the
fixture, corrected before running it.

Final resource-image GPU checks pass 493,445 assertions in nine cases
(`transparent-compose-final-resources.log`), and graph checks pass 8,927 in 225
cases (`transparent-compose-final-graph.log`). All 34 shader modules pass the
SPIR-V/MSL contract check. Client, Server and test binaries build; formatting and
whitespace checks pass. `transparent-compose-comparison.png` and its JSON retain
the inspected CPU-reference/GPU comparison; eight temporary PPM files were
removed. These are synthetic ordered-layer checks, not product glass capture.
Ordered-layer production, bounded payload/ownership, explicit overflow behavior,
lens inputs, nested depth domains and product body selection remain open. No
product walk, release timing or sanitizer run was made for this primitive.

Ordinary transparent geometry has a `transparent-layer` capture node using the
shared surface shader. Selection uses native D32 depth through `opaque-z` and
optional `previous-z`, both from the same projection. The nearest-fragment pass
stores its depth attachment, then a gather pass blends all exact ties in draw
order. Outputs are premultiplied HDR colour, R32 camera-forward depth and D32
native depth. `depth-compose` accepts `mode=premultiplied` to avoid weighting
already blended radiance twice. Shadow sampling is a declared dependency;
unsupported surface/custom-shader rows refuse capture.

The coplanar fixture first reproduced lost contributions. Selecting ties with
reconstructed forward distance also failed for shifted panes under a rotated
camera. Native device-depth selection fixes pixel parity without an epsilon.
Mathematically coplanar transformed panes may round into adjacent native depth
layers; checks require complete combined opacity, bounded forward depth and
unchanged direct-render image tolerance. Exact native-depth ties must combine.
The fixture also checks an intervening opaque body, exact opaque ties and a third
layer as an overflow probe. The two-pass gather reuses existing outputs without
extra textures or CPU readbacks; its additional GPU draw cost is not yet profiled.

Graph failure before capture completes matching queued requests as failed.
Unrelated pipeline/view requests remain pending and can subsequently capture;
recorded copies retain their fences but cannot publish a failed graph as success.

Focused GPU checks pass 43,892 assertions (`coplanar-final-gpu.log`). Broader
resource/runtime/host GPU checks pass 550,304 assertions in 35 cases
(`coplanar-broad-gpu.log`); headless render checks pass 41,815 in 457 cases and
graph checks pass 8,932 in 225 cases. All 35 shader modules pass SPIR-V/MSL
contract checks. `coplanar-comparison.png` and JSON retain ten inspected
comparisons, including shifted coplanar panes and opaque ties. Temporary PPM
captures are removed after inspection.

Particles, ribbons, nested apertures, bounded atomic layer-set publication and
explicit overflow refusal remain open. No product walk, release timing or
sanitizer run was made for this capture change. Do not exclude the producer's
primary body until the receiver owns the complete drawable layer set.

Bounded capture groups now use the existing four export slots. Admission validates
all tokens, capture declarations, pipeline/view/delivery identity and available
capacity before reserving any slot. Copied collection waits for every member and
returns them in token order; invalid or unfinished groups consume nothing. Failed
results can be collected together for cleanup. The actual renderer-local capture
frame is exposed separately from request tokens and world ticks. The glass
fixture queues and collects all four outputs as a group and checks their frame
identity. Pending-member, duplicate-token, missing-node, mixed-view/delivery,
capacity-pressure, resident-member and graph-failure checks exercise refusal.

Resource/runtime/host GPU checks pass 550,379 assertions in 35 cases
(`image-group-final-gpu.log`), and headless render checks pass 41,816 in 457
cases (`image-group-headless.log`). Client, server and test-client builds pass
(`image-group-final-build.log`), with existing warnings in untouched code.
Formatting and whitespace checks pass. Six temporary runtime PPM previews were
removed; this change uses the existing numeric glass comparison without adding
new retained image artifacts.

This is capture admission and collection, not atomic layer publication. Product
the default product path still publishes one image. Complete layer-set receipts, copied transport, overflow refusal
and receiver body composition remain required. Grouping adds no GPU allocation,
submission or wait; copied results move their existing byte vectors. No release
profile or sanitizer run was made for this change.

Resident capture adoption now uses a bounded group preflight. Captures must share
frame, pipeline, view and extent; destination owners and tokens must be distinct.
Validation, slot availability and the entire replacement peak are checked before
any capture, import, cache or output handle changes ownership. The single-image
API delegates to the same implementation. On success, texture pointers move
inside the owning renderer and replaced images enter the existing bounded cache.
No GPU handle crosses a world boundary.

The GPU fixture fills 27 MiB of imports and attempts two paired 3 MiB captures:
one would fit alone, but the group refuses without consuming either. Removing
one old image allows the same captures to replace two owners, leaving 24 MiB live
and 6 MiB cached. Invalid bindings, duplicate owners/tokens, singular sampling and
mixed capture frames preserve ownership. A separate small-image case fills 15
of 16 import slots and proves the last slot is not partially consumed by a
refused two-image group; freeing one slot permits the complete adoption.

Focused GPU checks pass 139 assertions (`resident-group-final-gpu.log`). Before
the final slot-pressure assertions were added, the broader resource/runtime/host
run passed 550,458 assertions in 36 cases (`resident-group-broad.log`); headless
render passed 41,815 in 457 cases. Client, server and test-client builds pass
(`resident-group-final-build.log`), as do formatting and whitespace checks.
Six temporary runtime PPM previews were removed. No new image artifact was needed
for these ownership and budget checks.

This closes local resident ownership transfer only. Publishing authenticated
layer-set receipts, transporting copied layer sets, checking overflow and wiring
current player-body composition into product views remain required. No release
profile or sanitizer run was made for this change.

Copied layer sets now encode as one PIMG kind-5 envelope. One opaque image and
zero to two premultiplied transparent images share capture metadata and paired
depth. Each member reuses the existing digest-checked, lossless reply codec.
All member prefixes are admitted before any decompression. Total expanded pixels
fit the existing 262,144-pixel budget, regardless of compression, and wire bytes
remain bounded by 4 MiB. The encoder reserves the actual compressed total and
copies each member once into the final envelope. The decoder commits output only
after every member and cohort check passes. Opacity and empty-pixel semantics
are checked in addition to finite samples and digests.

The exchange suite passes 8,552 assertions in 16 cases
(`portal-layers-codec-final.log`); headless render passes 46,076 in 459 cases.
Checks include truncation, trailing bytes, valid individual messages from
mismatched captures, deterministic mutations and compressed expansion limits.
An initial compression-size assertion was too strict and failed at 66,655 bytes;
the test now checks compression relative to raw bytes and independently checks
the pixel budget. Product and test builds, formatting and whitespace pass.

The `MONO_FUZZ_PORTAL_EXCHANGE` CMake option builds `fuzz_portal_exchange` with
Clang, libFuzzer, ASan and UBSan. It instruments the actual exchange and nested
geometry codec sources; linked dependencies use their ordinary build flags.
The target's `--write-seeds <directory>` writes raw and compressed layer examples.
A seeded 50,000-run check passes with no sanitizer finding
(`portal-layers-fuzz-final.log`). Build, corpus and artifacts stay under
`.cache/build/portal-fuzz`. This is bounded fuzz evidence, not a complete engine
sanitizer run or release performance profile.

Product routing still publishes one flattened image. The new envelope must be
connected to demand profiles and authenticated layer-set receipts; producers
must prove no overflow before publication. Receiver imports, complete transparent
content, lenses and current player-body composition remain required. The codec
cannot establish endpoint authenticity or prove that no visible layer was omitted.

The receiver body graph now has an ordered-layer option. After composing the
current opaque body with the retained room, two repeatable `eye-image` nodes read
paired transparent layers and blend them back to front. Base/near/far roles are
renderer-local binding fields, so they share portal ownership without replacing
one another. Non-base roles require depth and opaque-lighting scope. Each layer
must match the base capture key, owner, projection, sampling, extent and content
and lighting revisions. Swapped or mismatched layers fail the capture rather than
silently composing a mixed image. Ordinary flattened portal sampling excludes
non-base roles. Layer handle changes, withdrawal and reordering participate in
the existing portal presentation signature.

The GPU fixture decodes the copied envelope, imports three same-portal images,
and draws a current local body rectangle. Per-pixel colour and depth match an
independent projection/alpha oracle before, between and behind the panes and room,
including rotated camera coordinates. Moving the body updates the composition
without another room upload. Wrong-role and mismatched-camera-key cases refuse;
restoring the valid set restores the image. The resident group fixture now also
adopts multiple roles under one portal. An initial emission expectation failed
because strength 4 is quantized to about 4.0156; the fixture now uses exactly
representable strength 16 instead of weakening the image tolerance.

Focused GPU checks pass 23,546 assertions in two cases
(`layer-body-checked-gpu.log`). The broader resource/runtime/host suite passes
573,861 in 37 cases (`layer-body-broad-gpu.log`). After adding cache invalidation,
headless render checks pass 46,087 in 459 cases. Graph checks pass 8,991 in 225
cases. Client, server and test-client builds pass (`layer-body-products-build.log`),
as do formatting and whitespace checks. Sixteen body PPM previews and six runtime
PPM previews were cleaned after image inspection.

`layer-body-comparison.png` and JSON retain eight inspected expected/actual pairs.
Temporary PPM captures are removed after inspection. This closes a receiver graph
path, not product crossing: product copied-set publication still needs routing and
readiness-driven presentation, alongside overflow, complete transparent
content, lenses, Humanoid presentation and physical walk validation remain open.
No release profile or sanitizer run was made for this receiver change.

Copied layer sets now queue as immutable groups in new renderer slots. A shared
owned-data validator checks their full codec contract without serializing them.
Admission checks all bindings, slots, CPU capacity and the complete live texture
peak, then prepares every texture pair before committing any bytes or handles.
The previous group remains readable. Group readiness becomes true only after all
member uploads are submitted. Individual copied or resident updates skip grouped
owners; sampling rejects a mixture of different groups even if their other
metadata matches. Dropping any group member retires all its members.

The current-body GPU fixture now uses grouped imports. It renders the old room
while uploading a differently coloured replacement, verifies the new pixels after
switching, and can return to the still-owned old group. Malformed members preserve
input and output handles. Ten pending 3 MiB images trigger CPU-budget refusal;
after upload they trigger texture-budget refusal. Freeing that pressure permits
the same complete input to queue. Pending-group cancellation releases its bytes
and all members without disturbing the old image.

Retired group textures use the existing four-pair cache. Six same-extent cycles
create no new textures at queue admission, upload and render correctly, then
return their pairs to the cache. Aggregate live/cache budget accounting includes
reused pairs without charging them twice. The last import release clears the
cache. This prevents per-update texture recreation while retaining the old set.

Focused Vulkan checks pass 24,322 assertions (`copied-layer-cache-gpu.log`).
The broader resource/runtime/host run passes 574,840 in 37 cases; headless render
passes 46,121 in 459 cases. Client, server and test-client builds pass
(`copied-layer-products-build.log`), as do formatting and whitespace checks.
The instrumented codec/shared-validator fuzzer passes another 50,000 seeded runs
with ASan and UBSan (`copied-layer-fuzz.log`); this does not instrument the GPU
ownership path. Six temporary runtime previews were removed.

Product routing must still drive upload progress while the old displayed view is
cached, await group readiness, switch all bindings and retire the old group.
Authenticated receipts, overflow proof, complete later layers, Humanoid wiring
and physical walks remain open. Device-allocation fault injection and a product
walk were not run for this ownership change; capacity refusals and actual Vulkan
uploads are covered.

The producer now accepts an explicit PIMG v12 `OrderedLayers` profile. It requires
opaque-lighting scope, zero recursion and no renewal, and charges all four
captures including the overflow probe against the existing pixel budget.
Malformed profile bytes, one-pixel budget excess, incompatible scope/recursion
and retained-image renewal are refused without changing codec outputs. The old
wire version is rejected. The single-image inbox cannot satisfy this profile
with flattened successful pixels; failure replies still complete it normally.

A separately cached producer graph exports opaque radiance/depth, two ordered
transparent pairs and a third pair for overflow detection. Renderer-generated
token groups use the existing atomic admission path, skipping explicit caller
tokens and preserving outputs on refusal. Collection requires the same successful
renderer frame and extent for every member. Visible overflow returns a bounded
`BudgetExceeded` reply without publishing partial layers. Otherwise the producer
encodes the complete layer-set envelope once and sends it through the existing
bus. Full-mailbox retries retain that envelope without new rendering. Cancellation
retires all four capture tokens. Encoded ordinary replies also release retained
depth bytes alongside their pixel bytes.

The actual producer fixture checks all pixels for zero, one and two panes,
visible third-pane refusal, an opaque blocker hiding the third pane, recovery on
the same producer after overflow, and repeated full-mailbox retries. The broader
resource/runtime/host GPU run passes 590,545 assertions in 38 cases
(`producer-layers-broad.log`). Headless render passes 46,448 assertions in 461
cases (`producer-layers-headless.log`). Client, server and test-client builds pass
(`producer-layers-products-build.log`), alongside formatting and whitespace checks.
The request/layer codec fuzzer now exercises both request projections and ordered
profiles with updated seeds; 50,000 runs pass under ASan and UBSan
(`producer-layers-fuzz.log`). These sanitizers do not cover producer GPU ownership.

Product demand selection and source receipt handling still need the layered path.
The producer fixture issues explicit bus requests and decodes their replies; it
does not claim product adoption or a physical walk. Next integrate authenticated
layer-set inbox admission, source import/readiness ownership and host binding
switches, then current Humanoid geometry. Resident group receipts, complete later
layers, combined physical fault walks and release CPU/GPU profiling remain open.

Authenticated copied layer-set inbox admission is now connected. The borrowed
matcher checks both transparent member prefixes, exact request key and extent,
paired depth and a common capture tick before pixel decoding. The inbox first
checks sender/receiver incarnations and correlation, then charges all expanded
member pixels, depth and text alongside the still-held previous set. A malformed
member, mismatched endpoint, stale request or insufficient replacement peak does
not consume the pending request or old image. Complete sets transfer through
`TakeLayers`; the single-image extractor cannot consume only their opaque member.
Failure replies preserve held sets, and withdrawal/expiry release all members.

The real producer fixture now issues through the inbox and admits the resulting
bus replies there before checking every returned layer pixel. Runtime/host GPU
checks pass 28,721 assertions in 26 cases (`layer-inbox-gpu.log`); headless render
passes 46,496 assertions in 462 cases (`layer-inbox-headless.log`). New cases cover
incomplete sets, wrong endpoint incarnations, budget refusal before malformed
pixel decoding, corruption, failure preservation, whole-set extraction and
expiry/withdrawal. The matcher joins the instrumented codec fuzzer; 50,000 seeded
runs pass ASan/UBSan (`layer-inbox-fuzz.log`). GPU ownership is not instrumented. Client, server and test-client builds pass
(`layer-inbox-products-build.log`), as do formatting and whitespace checks. Six
temporary runtime PPM previews were removed.

Next wire `PortalImageSource` to `TakeLayers`, keep old handles and accepted camera
metadata until the new group is uploaded, expose pending upload work to the host,
and retire the old group after switching all bindings. The product still selects
flattened views; current Humanoid composition and combined physical crossings
remain unverified by these inbox checks.

Source layer ownership is now wired. Accepted sets enter new renderer slots while
the old displayed group and captured camera remain intact. `HasPendingUploads`
exposes pending work; only complete upload readiness advances the capture snapshot,
returns success and retires the old image. Snapshots carry both transparent
handles. Requests avoid single-image renewal/resident reservation for this profile,
and mismatched single receipts cannot complete it. Restart, expiry, supersession
and endpoint withdrawal release staged groups. Switching to a flattened image
retires all members of the preceding group.

The source GPU fixture passes 121 assertions, covering deferred initial success,
old-image retention during replacement, captured camera identity, pending work,
cancellation before/after submission, endpoint retirement and grouped/single
profile transitions (`layer-source-gpu.log`). Broader resource/runtime/host checks
pass 590,687 assertions in 39 cases; headless render passes 46,497 in 462 cases.
Client, server and test-client builds pass. Product host scheduling and current
Humanoid composition still need this path; the source tests do not claim that
integration or full-engine sanitizer coverage.

Host upload scheduling is now connected. `PortalImageHost::HasPendingUploads`
aggregates staged source groups, and pending work forces client scene/portal
damage even when the displayed image is cached. The graph upload boundary can
submit immutable groups belonging to other worlds and viewport slots; sampling
still requires the original owner. Independent single images retain their
existing per-view upload filter. No helper render target or extra render is used.

The new empty-world host fixture initially crashed in `BindInstanceBuffers`
(`portal-upload-host-debug.log`): the transparent layer pass bound absent instance
buffers after clearing an empty scene. It now ends the pass after clearing when
there are no transparent rows. Resource/runtime/host GPU checks pass 590,703
assertions in 40 cases (`portal-upload-broad-gpu.log`), including deferred host
publication and upload through an unrelated view. The added foreign-world sampling
check passes with the body/layer pixel fixture, 24,326 assertions
(`portal-upload-ownership-gpu.log`). Headless render passes 46,496 assertions in
462 cases; client and test-client builds pass. Temporary runtime PPMs were removed.
Product layered demand selection, current Humanoid composition and the complete
physical body oracle remain open. These checks do not establish the player fix.

The user reports that player characters do not pass through portals properly.
The existing physical image-handoff fixture was rerun at 30 and 60 Hz world rates
and passes 16,327 assertions (`player-crossing-recheck.log`), but its cleared-camera
case excludes body-visibility assertions. Image inspection confirms disappearance
in the 60 Hz run, with zero avatar-yellow pixels in frames 87 through 98.
`player-crossing-recheck.png` and JSON retain the frames and sample metadata.
This remains a product defect. Prioritize host upload scheduling and current body
composition, then require the physical body oracle through the full transition.

The physical body oracle now excludes only frames whose camera has actually
been cleared, rather than the entire late-clear scenario. Rebuilding test-client
passes, but the corrected `[portal-product-image-handoff]` test fails 14 of 15,859
assertions (`portal-body-oracle.log`): zero yellow body pixels at 30 Hz frames
86-94 and 60 Hz frames 87-91. All failed frames still have a Humanoid subject and
`eye_image == false`. They render native `client.replica` views with external
portal images, before the whole-eye handoff. This corrects the earlier assumption
that the observed disappearance belongs to the whole-camera image path.

`portal-body-oracle.png` shows both rates before, during and after disappearance;
its JSON retains every failed frame's metadata. The 2,400 temporary BMP/JSON frame
files were removed. The test is deliberately left exposing the unresolved defect,
not relaxed or marked as an expected failure.

Next integrate current body composition at the external seam image boundary.
The source-side body is already clipped while the remote picture contains geometry
from an earlier request. Render the complementary current body against the accepted
room depth using the capture's camera and original binding, then consume that
composite through the aperture. Keep body exclusion specific to the primary capture
so nested views retain their bodies. Ordered room captures still need complete
world-layer support before this can replace the flattened product path generally.
Whole-eye composition remains required afterwards, but cannot by itself fix these
native-view failures. Verify both paths with the continuous Humanoid oracle.

`Renderer::ComposePortalBodyImage` now provides the renderer-owned composition
operation needed by the seam adapter. It requires a ready, complete imported group
with matching owner, key, member roles, extent, sampling and projection. It renders
bounded opaque body rows using the accepted capture camera and lighting, then
adopts the paired HDR/depth export directly into a resident image. Repeated calls
replace the preceding composed image while retaining the room group. Refused input
or failed capture returns zero and cancels its export token. Transparent/custom
body materials are explicitly refused until their composition path exists.

The operation caches separate seam/eye graph definitions and forces body scene
refresh independently of the caller's presentation-damage flags. The caller owns
the output lifetime and chooses the target extent. Use the parent's extent when
sharing its view slot to avoid alternating scene-target sizes. Current GPU coverage
exercises the eye variant, compares changed body pixels with the direct composition
reference despite an unrelated caller camera and empty damage flags, and checks
wrong owner, swapped roles, invalid camera and unsupported body transparency.
Room upload counts stay unchanged and residency remains three room members plus
one composed image. The final focused test passes 24,341 assertions
(`portal-body-operation-damage-gpu.log`); broader resource/runtime/host checks pass
590,722 in 40 cases before the damage-forcing addition, and headless render passes
46,497 in 462 cases. Client, server and test-client builds pass.

This operation is not yet called by the product seam adapter. Next connect ordered
demands, primary body exclusion, current mapped/clipped body rows, composed-handle
sampling and retirement there. Verify the seam variant through the actual aperture,
then rerun the still-failing physical crossing oracle. Complete world-layer support,
transparent body materials and resident layer transport remain required; this
operation alone does not establish seamless player crossing. Six temporary runtime
PPMs were removed.

The host now owns composed images through `ComposeBodyImage`. Room snapshots
remain available through `Image`/`Capture`; composing does not replace their group.
Successful output replacement retires the preceding composed handle, including
changed surface bindings. Refusals retain ownership without reporting a new image.
Hidden portals, viewport/source/destination removal, endpoint withdrawal, profile
changes and expiry retire composed resources alongside the source state. The
stored producer incarnation prevents a composed image outliving its route owner.

Host integration exposed two defects in the new renderer operation. A nonzero
viewport could not export because the frame capture node still selected slot zero.
Its cached graph now explicitly selects the owning target slot, with separate
cached graphs per projection and viewport. The node parameter and request slot
must agree; changing only the request to zero still exported the wrong target.
The operation also replaced the slot's cached source rows. A following parent view
with unchanged object damage reused the body subset and produced wrong pixels.
The new pixel check reproduced that failure; composition now invalidates that
source cache so the parent restores its own rows.

Focused body/host GPU checks pass 24,506 assertions in two cases, including the
nonzero-slot seam operation and six retirement paths
(`portal-body-host-final-gpu.log`). Broader resource/runtime/host checks pass
590,870 in 40 cases (`portal-body-host-broad.log`); headless render passes 46,496 in
462 cases. Client, server and test-client builds pass. Six temporary PPMs were
removed. The client now connects player-specific ordered demands, primary body
exclusion, current seam-row selection and composed aperture sampling in its joined
native view. Focused demand checks pass 1,333 assertions in 10 cases. This profile
still needs complete later-layer support, transparent body materials and resident
transport before it can represent all product worlds.

The existing body-and-glass pixel test now exercises both seam and eye projections,
including translated/rotated cameras and the accepted-camera composition operation.
It passes 48,702 assertions (`portal-body-seam-check.log`). The latest normal
physical walk fails 20 body checks at 30 Hz, frames 35-54, with zero external
portal handles in the inspected frames (`portal-body-current-walk.log`). Its first
source request is logged at frame 48, after the missing-body interval has begun.
The retained PNG/JSON show the cold-start gap. Investigate endpoint discovery and
initial-image availability as well as geometry continuity. Earlier debugger zeros
were read at a source line that may precede assignment; renderer return and guard
breakpoints did not reproduce a composition refusal. Do not treat those zeros as
proof of a renderer failure. The physical crossing gate remains open.

A separate `[portal-product-warm-image-handoff]` physical case now waits for the
first completed imported portal frame before pressing movement. Both world rates
retain every existing crossing assertion; 15,806 assertions pass
(`portal-warm-walk.log`). Captured images show the avatar before/after both
handoffs. `portal-warm-walk.png` and JSON retain this evidence. The original
cold-start case and its failing body oracle are unchanged. Initial images arrived
at frames 87 (30 Hz) and 79 (60 Hz), before warm-case input. This localizes the
latest normal-run failure to initial availability without establishing all-angle,
full-layer, startup or fault-recovery completion. Producers already omit demos;
their GPU initialization and authenticated replica join precede endpoint publication.
The next product decision is initial playable-world readiness while those required
images are absent. Physics must remain independent of render visibility.

The native image adapter now prefers an authenticated remote image endpoint over
a matching local player replica. Replica snapshot arrival previously opened a new
local producer and switched image ownership; a focused test reproduced that
switch (13 passed / 1 failed assertion). Camera and physics destination selection
retain their existing replica preference. If the remote endpoint is withdrawn,
image requests can still fall back to the ready local replica.

Route, replica-arrival and copied camera-topology checks pass 163 assertions in
three cases (`portal-image-route-final.log`). The warm bidirectional product walk
passes 15,820 assertions at 30/60 Hz; its log contains no image requests switching
to `client.portal` replicas. Inspected handoff images retain the avatar. The cold
walk still fails one body assertion at 30 Hz frame 86: the external image handle
is zero there and becomes nonzero in frame 87. This is not a startup completion
claim; the duration varies with process and device startup. Logs and retained
PNG/JSON evidence are named `portal-image-route-warm` and
`portal-image-route-cold` under `.cache/build/dev/tests`. Temporary frame sequences
were removed after retaining those artifacts.

Image producers without GPU particle batches now service captures directly after
world presentation, skipping the unused ordinary viewport and screen-interface
frame path. Capture pumping reuses that prepared world and runs inside the frame
profiler. Producers with particle batches retain the normal GPU step, then service
captures without presenting the world twice. Interface initialization is retained
for that fallback. Producer service frames use the existing monotonic schedule,
defaulting to 60 Hz or the explicit maximum frame rate; update polls use its short
idle wait. Removing the viewport without pacing initially caused 100,000 service
frames in seconds. The paced restart fixture reports 119 service frames in 2.0 s.
This is a scheduling check, not a release CPU/GPU speedup measurement.

Saved/replicated producer, lighting update, shutdown and server restart checks pass
847 assertions in two cases (`portal-producer-paced-host.log`). The combined warm
and cold walk run passes 31,672 of 31,673 assertions: warm crossing passes, while
the cold case fails its return-handoff landmark check at 30 Hz frame 251. Blue
floor pixels change from 270 to 130 against a minimum of 135. Body checks passed
in this run; this is a distinct handoff-image discontinuity, not the earlier
missing initial image. `portal-producer-paced-handoff.png` and JSON retain the
inspected frames. Initial availability, handoff pixel continuity and full world
layer support remain open. Four temporary frame sequences were cleaned. Final rebuild passes; the saved/replicated
producer check passes 550 assertions after retaining the particle fallback
(`portal-producer-final-host.log`).

Capture-sequence metadata now records the accepted image identity and camera:
producer world/session/generation, request and seam/camera revisions, projection,
frustum, clip plane, dimensions and viewport slot. Whole-eye records include the
bound handle; external aperture records include their accepted room capture.
These are capture-only diagnostics. The physical fixture verifies that each
whole-eye record names the bound handle and a valid eye request.

The rerun passes 17,340 assertions (`portal-capture-metadata-walk.log`), but its
metadata exposes a stronger continuity gap than the landmark oracle detects.
At 30 Hz frames 257-258 the accepted whole-eye image retains request 11 and handle
103 across return adoption. Its camera position is about 4.5 metres from the
current eye before adoption. Maximum accepted/current eye-position separation
in the recorded sequence is about 8.36 metres at both world rates. This measures
camera positions in this unit-scale fixture, not a latency estimate. The previous
handoff landmark failure remains valid evidence of intermittency; this passing
rerun does not close it. Retained PNG/JSON artifacts share the log's stem.

The whole-eye gate must compare the displayed result against the current camera,
including angular motion and depth/parallax, rather than only checking image
availability and body colour. Current whole-eye sampling can display a retained
capture from a different camera. Establish current-eye reprojection/composition
and direct-view image parity, including disocclusion and later world layers,
before marking continuous camera crossing complete. Temporary raw frames were
removed after retaining the handoff samples and displacement series.

Successor readiness no longer submits its staged camera into the persistent
whole-eye slots. The debugger reproduced `PumpPortalSuccessor` calling
`PreparePortalEye(..., prepareNative=false)` and then `SubmitEye` for slot 3
(`portal-staged-eye-call-debug.log`). That allowed readiness checks and the
displayed camera to compete for one request stream. The readiness branch now
only checks the matching displayed-eye image; the normal frame remains its
request owner. Camera route resolution and the successor's own drawable slot
remain in place.

Both warm and cold walks pass 35,120 assertions after this change
(`portal-eye-owner-walk.log`), with successful builds and formatting checks.
This fixes competing request ownership, not the whole camera-lag problem.
Maximum accepted/current position separation remains approximately 8.3-10.4 metres
across these four runs. `portal-eye-owner-walk.json` retains the per-frame series,
handoff metadata and worst-displacement samples; its PNG shows those samples.
Investigate retained full-eye request latency and nested capture dependencies,
then verify current-camera pixel parity. Do not infer that eliminating staged
requests eliminates delayed images. Four raw frame sequences were cleaned.

The request-age audit correlates consumer issue/receive timestamps in
`portal-eye-owner-walk.log` with frame timestamps and accepted request IDs in
`portal-eye-owner-walk.json`. These are dev/offscreen diagnostic timings from
128 x 128 captures, not release performance measurements. At each run's maximum
position separation:

| World tick rate | Start | Frame | Request to receipt | Receipt to displayed frame | Total camera sample age |
| --- | --- | --- | --- | --- | --- |
| 30 Hz | Cold | 193 | 285 ms | 284 ms | 569 ms |
| 60 Hz | Cold | 195 | 251 ms | 318 ms | 569 ms |
| 30 Hz | Warm | 277 | 253 ms | 251 ms | 504 ms |
| 60 Hz | Warm | 206 | 317 ms | 318 ms | 635 ms |

The source permits one outstanding request per compatible binding and displays
its previous accepted image while awaiting the next reply. This accounts for
sample age approaching two request cycles. The `eye-image` node in
`render/src/nodes/OutputNodes.cpp` directly blits the accepted texture; it does
not transform samples from the accepted camera into the current camera.
Reducing request latency cannot by itself establish continuous camera parity.

Inspection of `PortalImageProducer::Pump` rules out ordinary scene revision
changes repeatedly rebuilding children: each job collects them once, then
revalidates the visible seam set. Seam changes fail the job explicitly. The
consumer log measures 16 local nested replies with a median 99.5 ms round trip;
it does not expose the remote producers' internal dependency timings. Do not
attribute the whole-eye delay entirely to nesting from this evidence.

Next validation must deliberately retain an accepted eye capture while moving
the consumer camera and compare with a direct render at that new camera. Cover
translation, rotation, depth edges and newly exposed surfaces. Current-camera
composition must handle missing samples and later world layers explicitly;
reusing an arbitrary retained child or warping only colour cannot close this gate.

The missing gate is now executable in `render/tests/ResourceImage.cpp` as
`[eye-current-camera]`. It captures an emissive room once, imports its colour and forward depth
through the same `DefaultEyeDocument` used by the product, then compares its HDR
output with a direct render from the current camera. The scene has a near red
occluder, a hidden green post and a blue rear wall. Independent colour checks
prove the post is absent initially and becomes visible after translation.

Dev/offscreen Vulkan builds without warnings. The targeted run
`SDL_VIDEODRIVER=offscreen .cache/build/dev/tests/test_render '[eye-current-camera]'`
fails three image comparisons: translation differs at 986 pixels, rotation at
786, and combined movement at 424, each out of 2,405 pixels. The stationary
control passes. The run has 102 passing assertions and 3 failing assertions
(`eye-current-camera-gpu.log`). This is an unresolved correctness test, not an
expected-failure annotation or a passing compatibility claim. Selecting the
resource-image GPU suite now includes this known failure.

`render-failures/eye-current-camera/comparison.png` retains expected, actual and
difference panels; per-motion raw HDR samples and manifests retain precision.
The missing green post demonstrates that camera reprojection alone cannot
recover all newly exposed geometry from a single colour/depth image. Continuous
crossing still needs destination scene/layer coverage sufficient for the current
camera, plus correct camera-dependent composition. No runtime fix is claimed by
this test addition.

Parent captures now defer camera-dependent lights, ribbons and spatial UI while
cross-world child images are pending. The first waiting pump reproduced one
unnecessary prepared view (`portal-wait-preparation-before.log`). The shared
runtime continues validating seams and collecting surfaces for child budgets;
it prepares the remaining layers from the current world state once children
are drawable. `render.portal_snapshot.prepared_views` counts actual preparation,
and snapshot byte accounting excludes retained layers on skipped pumps.
Runtime/host Vulkan tests pass 29,041 assertions in 28 cases
(`portal-wait-preparation-gpu.log`). This removes observed redundant work; no
release speedup or reduction in network round-trip latency has been measured.

The scheduling change also passes the real cold and warm player walks at 30/60 Hz:
34,610 assertions in two cases (`portal-wait-preparation-walk.log`). The retained
PNG shows the first handoff and worst-displacement frame for each run; the JSON
keeps both handoffs and per-frame accepted/current camera separation. Maximum
separation is still 7.8-10.1 metres. These results verify character/image continuity
for this fixture, not current-camera parity. All 4,800 temporary BMP/JSON frames
were removed after extracting the evidence.

Destination scene coverage must reuse authenticated, per-viewer replication
admission and filtering. `Server::BeginPresentationProducer` gives its renderer
an independent identity and restricted-world admission; that grant does not
belong to a portal viewer. `Client::InitialisePresentationHost` consumes the
ordinary replicated world through an inherited driver channel. Re-exporting the
producer's entire ECS snapshot through an image reply would bypass that ownership
boundary and include resources beyond the requested visual scene. Do not use
`Store::Save` as an unfiltered portal-view payload. A destination visual replica
must retain its own admitted stream and endpoint incarnation, remain separate
from player/input ownership, and retire with its view subscription. Images still
serve the explicit bus-driven capture path. The image-only versus destination
scene-data product choice was presented to the user and remains unanswered.

A held Humanoid no longer forces the native source world to display a retained
whole-eye image once its aperture images are drawable. `Client::PreparePortalEye`
now keeps the current native view in that case; foreign eyes and unavailable
native images retain their existing routing. Product walk tests require at least
one held-character frame on the native path, alongside the existing body,
camera-subject and movement checks. The four latest runs cover 17-18 such frames
each. Builds and formatting pass. The initial cold-only run passes 17,119
assertions (`portal-native-held-walk.log`).

The combined cold/warm run has 34,000 passing assertions and one failing landmark
assertion (`portal-native-held-final-walk.log`). Cold 30 Hz return frame 257
reduces the blue floor patch from 300 to 143 pixels, below the half-size threshold.
The body and native-held checks pass. Both failing boundary frames use remote
eye images: request 10 becomes request 11, whose accepted camera is at z=19.56
while the current camera is at z=15.11. `portal-native-held-return.png` and JSON
retain this failure; do not weaken the landmark threshold or describe the whole
walk gate as passing. `portal-native-held-final.png`/JSON retain native-held,
handoff and worst-displacement evidence. Maximum foreign-eye separation remains
8.1-9.5 metres. All 4,800 temporary capture files were removed. This native-path
change does not close the foreign-eye current-camera composition gate.

The wider `[portal-product-walk]` matrix now ran all 16 combinations of 30/60 Hz,
first/third person, automatic/explicit Humanoid subject and held/released movement.
It has 138,627 passing assertions and six failures
(`portal-native-held-camera-matrix.log`). First-person 30 Hz with explicit subject
and released movement has no eye image at frame 84. First-person 60 Hz with
automatic subject and held movement has no eye image at frames 86-87. Each frame
fails both image readiness and visible-output checks. These are initial foreign-eye
readiness gaps before adoption, distinct from the return landmark failure.

The trace places the first-person `walk.destination` request's receipt on the next
frame after the 30 Hz gap and at frame 88 after the 60 Hz gap. In the latter case
it was issued at 13:22:34.288 and received at 13:22:34.589. Preserve this first-image
readiness requirement alongside current-camera parity; a successful transfer alone
is insufficient. Handoff contact sheets and metadata for all 16 combinations are
under `portal-native-held-camera-matrix/`; 19,200 temporary capture files were
removed. Those contact sheets cover handoffs, not the earlier missing-image frames;
the failure log is the evidence for the latter. That run preceded the initial
entry gate described below; its image oracle remains unchanged.

The player presentation pump now flushes newly queued image packets before the
server sleeps. QUIC pacing needs the current monotonic time for this second flush,
not the earlier tick timestamp. The old timestamp flushed zero active wires;
current-time samples flush 87 wires over 84 calls and 97 over 95 calls. This is
transport evidence in dev, not a matched release performance result. The full
camera matrix after that change has 136,402 passing assertions and 21 failures
(`portal-presentation-current-flush-camera-matrix.log`). Cold 30 Hz third-person
frames 35-55 have no yellow body and an unfilled external portal. The first image
arrives at frame 56. `portal-current-flush-cold-failure/` preserves sampled PNG/JSON
frames and a contact sheet; the 19,200 raw capture files were removed.

Initial entry now holds keyboard/gamepad movement until visible native portal
images and the nearby prefetched foreign eye are ready. Look and zoom remain
active, so first-person requests use the selected body-hiding profile. This is a
one-time viewport gate that survives body-world adoption, not a freeze at every
crossing. The existing overlay shows `Loading portals`; capture metadata records
`loading_portals`. The product walk checks zero horizontal predicted velocity
while loading and requires an observed loading interval. The loading preference
was unanswered; this proceeds with the stated loading-phase assumption.

Client/test_client builds and formatting pass. The initial cold/warm 30/60 Hz run
passes 34,184 assertions and fails one return landmark check
(`portal-entry-readiness-walk.log`). Warm 30 Hz return frame 269 changes the blue
floor patch from 345 to 143 pixels, below the half-size threshold. All loading
movement checks pass. `portal-entry-readiness-evidence/` retains loading/ready
scene images and the failing return boundary; 4,800 raw captures were removed.
Scene captures exclude the host overlay, so these images do not verify the
loading label. The first wider run exposed a first-person loading stall:
`CurrentImage` requires no pending refresh, but live look input starts another
request before the gate reads it. The diagnostic run was stopped with SIGINT
(73,560 passing / 29 failing assertions, including interruption). Its log is
`portal-entry-readiness-camera-matrix.log`; first-person frames 100 and 599 are
retained under `portal-entry-readiness-evidence/stuck-first-person/`. The 11,864
interrupted raw captures were removed. The gate now accepts a usable completed
image with the selected body-hiding profile while a camera refresh is pending.
Endpoint expiry still belongs to the source runtime. This does not claim exact
current-camera rendering. The corrected full matrix passes all 136,954 assertions
(`portal-entry-compatible-camera-matrix.log`): all 16 combinations of 30/60 Hz,
first/third person, automatic/explicit Humanoid subject and held/released movement.
Loading spans 13-39 captured frames across those runs. Per-case loading, first-ready
and both adoption frames are retained under `portal-entry-compatible-evidence/`,
with a JSON summary. All 19,200 raw captures were removed. After entry, the gate
no longer copies capture metadata merely to recompute unused readiness. The final
client/test_client build and `git diff --check` pass. The final authenticated
producer-host check passes 305 assertions in `test_client`
(`portal-entry-client-host.log`). An earlier invocation against `test_server`
matched no tests and provides no validation. Current-camera parity, loading
refusal/retry UX and complete crossing acceptance remain open.

A retained-source prototype was tested and removed from the runtime. It kept one
previous authenticated connector and advancing snapshots, stopped the old gameplay
script systems, and used a separate local whole-eye slot with a warm remote
fallback. Tests confirmed advancing snapshot ticks and displayed local images.
Reusing the remote slot first discarded its ready image: 34,069 assertions passed
and 81 failed (`portal-retained-walk.log`). A non-increasing scheduler replacement
revision then triggered the safety fallback and all eight retention assertions
failed (`portal-retained-slot-walk.log`), correctly rejecting a false success.

With script stopping corrected, the retained path passed 36,966 assertions and
failed 103 (`portal-retained-clock-walk.log`). One return was delayed until roughly
frame 486. Frames 240-440 show `proceed=false` and `crossed=false`, with expired
nested successor captures. The delay precedes physical handoff; lease renewal is
not established as its cause. The source nevertheless renews the original transfer
lease while its old connection remains live, which observer lifetime must address.

Retiring the observer when successor staging begins removed the long delay in the
next four sampled runs. They still fail: 34,812 assertions pass and three fail
(`portal-retained-staging-walk.log`). Warm 30 Hz frame 260 shrinks the floor patch
from 267 to 121 pixels. Warm 60 Hz frame 141 is completely black, while frames
142-143 show the scene. All three use local resident handle 670 with identical
camera and capture metadata. This needs a focused GPU first-use reproduction;
a nonzero handle and an arbitrary one-frame wait are not correctness proofs.

`portal-retained-staging-evidence/` preserves the black-frame contact sheet,
PNG/JSON boundaries, four-run summary and `prototype.patch`. Earlier failure
samples are under `portal-retained-evidence/` and `portal-retained-clock-evidence/`.
Each run's 4,800 raw captures was cleaned up. The four runtime files exactly match
the pre-experiment copies; client/test_client rebuild, formatter and diff checks
pass after restoration. The restored producer-host check passes 305 assertions
(`portal-retained-rollback-host.log`); the full camera matrix was not rerun after
restoring those exact sources. Observer content delivery, endpoint/lease retirement, full visual
layers and exact current-camera rendering remain open. This experiment does not
claim a retained-source feature or seamless crossing completion.

The focused first-use fixture now passes 688 assertions across 16 combinations
(`eye-first-use-lifetime-gpu.log`, dev Vulkan). It immediately adopts a resident
capture, switches from viewer slot 3 to either 3 or fresh slot 4, retains the
fallback image, and checks the first two frames. It covers full versus scene-only
damage, texture-only versus headless presentation, and the normal eye pipeline
versus an extra HDR readback. The normal presentation branch uses the product's
`RequestSceneCapture` path. HDR bytes match the independent source capture and
the displayed centre remains blue. Successful BMPs are removed by the fixture.

A window-backed variant crashed inside swapchain initialization before drawing,
including with the client's window flags (`eye-first-use-window-flags-gpu.log`).
It was removed: the product crossing tests use a headless device with presentation
enabled, so a window was not needed to match that path. This is not evidence that
the product black frame is a driver defect. The isolated test does not reproduce
the product failure, and no renderer runtime fix follows from its passing result.
Next, isolate the local producer publication/adoption and nested capture ordering
from the saved product prototype. The retained observer and full crossing work
remain incomplete.

### Render-stage snapshot probe

`ATOMIC_RENDER_PROBE_DIR` enables a diagnostic probe in the client or render tests.
`ATOMIC_RENDER_PROBE_FIRST` and `ATOMIC_RENDER_PROBE_LAST` select an inclusive
renderer-frame range (defaults 0 through 8, maximum span 1025 frames). Renderer
frames include producer captures, so these are not client capture-sequence indices.
Each renderer creates its own run directory to separate viewer and producer devices.
For example, the checked fixture command is:

```sh
ATOMIC_RENDER_PROBE_DIR=.cache/build/dev/tests/stage-probe-check \
ATOMIC_RENDER_PROBE_LAST=5 SDL_VIDEODRIVER=offscreen \
.cache/build/dev/tests/test_render '[stage-probe],[eye-first-use]'
```

Prefix the existing client command with the same environment settings to probe
that scene. `index.html` shows the snapshots in execution order with stage/resource
labels. Each snapshot has a metadata JSON, raw `.bin` pixels with declared row
pitch and SDL format, and a BMP preview. Preview channels clamp to [0,1]; raw HDR
values remain unchanged. The probe also copies the imported whole-eye input before
`eye-image`, so a bad imported image can be distinguished from a later bad stage.
Metadata includes pipeline, world, viewport, eye handle, camera position and whether
the node accepted and ran. Nodes without texture outputs and unsupported formats
are recorded explicitly. Texture snapshots currently cover base mip and layer zero.

Each copy is recorded directly after its graph node in the same GPU command buffer,
before later nodes can reuse its storage. GPU-to-CPU waits and disk writes happen
only after submission. Captures are bounded to 256 snapshots and 256 MiB per render
batch. This diagnostic changes timing and must not support performance claims.
Submission failures discard the queued readbacks; missing files must not be treated
as valid snapshots. Disable the environment setting for the ordinary rendering path.

The dev client and test_render build pass. The probe-enabled first-use checks and
same-command blue-then-red overwrite test pass 756 assertions in two cases
(`render-stage-probe-final-gpu.log`). All 1,072 recorded snapshots had raw and preview
files, with no unsupported formats in this fixture. Thirty-two imported-eye/output
pairs match exactly, excluding transfer padding. One browsable trace and a stage
comparison PNG remain under `stage-probe-check/`; duplicate traces and the overwrite
fixture's temporary images were removed. This validates the probe, not the product
black-frame fix.

The saved retained-source prototype was rerun with stage probing and then removed
again. `ATOMIC_RENDER_PROBE_VIEW` now optionally restricts capture to one viewport;
omitting it captures all selected-frame viewports. The local-eye-only run used
view 4 and renderer frames 0-1024. It passed 16,924 assertions and failed one
(`portal-stage-probe-walk.log`): warm 60 Hz client frame 245 reduced the floor patch
from 556 to 185 pixels. Capture metadata changes from remote handle 733 to 736;
the old captured camera is roughly eight metres behind the live camera, while
the replacement remains several metres behind. All 786 stage snapshots were
saved, with no black whole-eye or final-scene output in that trace.

The wider run captured every viewport at renderer frames 180-360. It passed 16,826
assertions and failed ten (`portal-stage-all-walk.log`). At warm 30 Hz client frame
145, corresponding to renderer frame 215, `eye-image` receives handle zero and
writes black to `eye-hdr`; tonemap and present retain that black output. The
normal missing-image clear in `nodes/OutputNodes.cpp` explains this stage result.
The client log immediately before capture reports a camera route waiting for
`server.world` topology from `walk.destination`. A reply arrives in slot 3 while
the displayed view has slot 2 and no image. The selection/lifecycle cause still
needs isolation. This is distinct from the earlier valid-handle black frame,
which these runs did not reproduce.

The broad probe materially changes timing: GPU waits and roughly 6.6 GiB of
stage files extended the run and coincided with image expirations. It is useful
stage evidence, not a timing-neutral reproduction or a performance result.
`portal-stage-evidence/missing-eye/` retains renderer frames 214-216, client frames
143-147, raw pixels, metadata, a browsable index and `stages.png`. The first run's
floor boundary and first local images remain under `portal-stage-evidence/30/`
and `60/`, and first-local stage copies remain under `portal-stage-trace/`.
The broad trace and both raw client capture directories were removed. All five
prototype files match their pre-run bytes; client/test_client/test_render rebuild
passes, and the restored producer-host check passes 305 assertions
(`portal-stage-probe-rollback-host.log`). No seamless-crossing fix is claimed.
Next isolate the missing-image route selection with a narrow probe, then return
to the original valid-handle failure and full current-camera acceptance.

A focused topology recovery test reproduced an additional outage: a renewal
requested at 250 ms and processed at 1000 ms was discarded because the cached
snapshot expired at 1000 ms, although the authenticated request remains valid
until 1250 ms. The old code left the request pending after discarding that reply.
The failing test reported 13 passing assertions and one failure
(`topology-renewal-before.log`). `PortalTopologyHost::Pump` now accepts matching
renewals within the pending request deadline, including after cache expiry.
Snapshot access still returns null before the reply arrives; endpoint identity,
correlation, revision and request-deadline checks remain in force. A reply at the
1250 ms deadline cannot revive the snapshot. This closes a proven recovery gap;
it does not prove the earlier product black frame had this exact cause.

The topology/restart suites pass 124 assertions across seven cases, and the copied
remote-camera routing test passes 144 assertions (`topology-renewal-after.log`,
`topology-renewal-route.log`). The dev client and both test binaries build. The
cold/warm product image-handoff checks pass 33,872 assertions across two cases
and four 30/60 Hz runs (`topology-renewal-product.log`), with the retained-source
prototype absent and stage probing disabled. Input-world adoption boundaries and
neighbouring images remain under `topology-renewal-evidence/`; all 4,800 raw client
capture files were removed. Formatting and diff checks pass. The full 16-combination
camera matrix was not rerun for this narrow renewal change, and the original
valid-handle black frame and current-camera parity remain open.

Route selection now consumes already-delivered topology messages before checking
its cache. The expanded `remote-eye-route` test reproduced two failures when an
authenticated destination reply was in the universe inbox but `PortalImageHost`
had not been pumped (`topology-ready-before.log`, 178 passing / two failing
assertions). `RequestTopology` now pumps only the topology host before requesting
refresh. It does not run image producers, enter a GPU wait, or wait for network
traffic. The existing endpoint, world, correlation, deadline and revision checks
still decide whether a queued reply may update the cache. Routing now passes 288
assertions with and without an earlier host pump, including expired topology and
withdrawn endpoints. The topology/restart/non-GPU host checks pass 172 assertions
across eleven cases (`topology-ready-route.log`, `topology-ready-host.log`).

Direct rendering of a retained local world remains the next larger construction
step for current-camera parity. The current client view assembles its input world's
lighting, shaders, instances, particles, ribbons and interface before selecting
an eye image. Replacing that image with only the retained world's geometry would
mix worlds. The retained prototype also drops `Content` and `ContentRelay` at
adoption. A complete implementation must retain authorized content delivery and
use one engine-owned world-view preparation path for these layers, shared with
local image producers rather than another partial client collector. Snapshot
inputs crossing the world enter/leave boundary must be owned copies; renderer
pointers must remain local. Use the current eye pose on every draw, preserve body
ownership and aperture composition, and keep image-bus rendering for worlds whose
visual data is not locally admitted. The previous replica's observer lifetime
must also stop gameplay lease renewal without losing authorized presentation.
No direct retained-world renderer is implemented by these topology fixes.

The full product camera matrix also passes 136,117 assertions
(`topology-ready-camera-matrix.log`): all 16 combinations of 30/60 Hz, first/third
person, explicit/automatic Humanoid subjects, and held/released movement. Stage
probing is disabled and the retained-source prototype is absent. The two adoption
boundaries and neighbouring image/metadata files for each run remain under
`topology-ready-camera-evidence/` with a JSON summary; 19,200 raw capture files
were removed. Build, formatting and diff checks pass. This verifies the routing
ordering change against that matrix, not complete current-camera parity or the
original valid-handle black-frame case.


The user also identified Terrain's `editable collision workers` span. It calls
`BuildTriangleMesh` for changed chunks. The builder now caches centroids, selects
median partitions and restores canonical leaf order instead of fully sorting
every range. Internal bounds are combined from children. Small meshes avoid
centroid scratch, and hierarchy capacity is bounded by the balanced leaf level.
The new `just terrain-collision-build-bench` job measures the existing query cases
and a flat 64-by-64 chunk with 8,192 triangles. In the release-derived bench preset,
build time changed from 984 to 858 microseconds, about 13 percent lower. Reserved
hierarchy nodes change from 16,384 to 4,095 for that chunk, about 75 percent lower;
centroid scratch adds 98,304 temporary bytes and is released after construction.
This is a CPU builder measurement, not the Terrain demo's total frame time.
Canonical full-sort parity passes across flat and folded meshes, preserving every
node bound, child index and leaf triangle order. Collision tests pass 66,816
assertions in 29 cases; scene tests pass 518,298 in 549 cases; physics tests pass
59,683 in 255 cases (`terrain-*-tests.log`). Products and test executables rebuild
successfully. Full Terrain worker/frame profiling remains to be measured.

Next work, in dependency order:

- [x] Keep Client input submissions live during Proceed and source retirement;
  distinguish consumed client input ticks from replication world ticks.
- [x] Route source-host movement to the exact transferred Humanoid until native
  destination input takes over; preserve retries and bounded receipt storage.
- [x] Resolve the saved-game successor rig missing at 60 Hz world / 30 Hz driver
  cadence; require both physical handoffs and camera samples at both cadences.
- [x] Wire destination lease offer/reserve/commit to authenticated product routes
  and the exact transferred-player receipt.
- [x] Request destination leases from the source product host and wire admission
  and acknowledged cancellation to the client acceptance state machine.
- [x] Connect the successor Client without a fresh avatar, adopt its committed
  player and restore its Humanoid camera on the accepted replacement replica.
- [ ] Keep the Humanoid subject and displayed camera responsive between source
  retirement and replacement, with continuous motion, input and script lifetime.
  Apply the retirement and acceptance gates above; adoption alone is not a pass.
- [x] Configure listening primary-world transfer endpoints with authenticated
  player admission and Client destination drawability gates.
- [x] Recover application refusal and successor session loss after Proceed
  without recreating source authority, while the destination body survives.
- [ ] Verify readiness expiry and disconnect with the complete successor
  connection, including lost destination cancellation acknowledgements.
  Authenticated product Client fixtures now cover snapshot readiness expiry and
  disconnect before the first snapshot, followed by cancellation and a later
  adoption. The Server source-lease fixture covers lost destination cancellation
  replies and requires the held rig to wait for acknowledgement. Combining these
  with the complete physical image walk remains open.
- [x] Exclude the unique receiving mouth within the scene wire precision budget.
  Preserve other panes and blockers; ambiguous apertures do not hide geometry.
- [x] Keep the live image producer while the destination replica joins, including
  the source player disappearing before the replacement snapshot is ready.
- [ ] Track the eye's presentation world independently of body/session ownership.
  Standalone Client now uses the shared resolver and whole-eye request path;
  the subprocess approach fixture verifies the eye request precedes Proceed.
  Cover predicted first-person entry before commit and the third-person arm
  remaining in the source after commit, then reverse both sequences. Route owned
  view requests by names and incarnations; do not retain duplicate body authority.
- [x] Require a current drawable destination publication before Proceed and
  after arrival, excluding retained or expired images from the readiness gate.
- [ ] Test missing, delayed and refused views in the combined physical walk
  without snapping the camera or dropping its subject.
- [ ] Repeat the unchanged combined image checks with clipped animated limbs,
  foreground seam clones, moving lights, look-back, oblique and scaled seams.
- [x] Connect process-host contact rounds and phase-controlled product Server
  launch, with bounded cancellation and exact shared frame counts.
- [x] Allow a supervised image producer to render a pinned live replica without
  creating a player, and retire its images when that source session ends.
- [x] Add a bounded host relay for delegated image/topology channels, preserving
  authoritative world ownership and distinct parent/producer endpoint receipts.
- [x] Connect configured automatic image-producer launch to listening Server
  hosts and relay their endpoints through the existing driver directory.
- [x] Carry bounded presentation frames over authenticated play connections,
  including packet backpressure and reliable recovery on both transport stacks.
- [x] Wire per-connection endpoint grants and host-owned return channels into Server
  and standalone Client. Advertise only image/topology producer routes to players;
  validate consumer channels and retire every grant with its connection.
- [ ] Connect standalone Client discovery/adoption in the complete walk over
  copied process images and local resident receipts, including producer restart.
- [x] Replace exited/disconnected image producers with bounded retry delays and
  fresh endpoint incarnations. Verify withdrawal and new image delivery on the
  existing player connection, including a request to the retired receipt.
- [x] Reproduce and fix connected-replica portal collection, named destination
  serialization and player-world compositor selection. Verify that the standalone
  Client discovers an advertised route and emits its replica's seam-image request.
- [ ] Compare joined-world portal and lighting pixels before and after successor
  adoption in the complete physical walk. The replica request check above does
  not prove rendered image parity or independent eye-world selection.
- [ ] Measure this complete path in `release`: CPU busy/wait, delayed GPU time,
  request/copy/readback/upload bytes, live/peak resources and retirements. Cache
  immutable producer inputs, coalesce superseded view requests and invalidate
  eye history on endpoint/seam changes. Keep pixel bounds and 1/2/8-view workloads
  fixed; successful local image-slot reuse is not an end-to-end speed result.

- [ ] Compare direct and portal views with destination lighting, moving lights,
  shadows, exposure and camera changes, using aperture masks and numeric probes.
- [ ] Verify visual object crossing and real player characters whose
  `CameraSubject` is a `Humanoid`, in first-person and third-person views.
  Public per-camera subject storage, save/load, cloning and product follow paths
  pass headless checks. The moving-character GPU gate now covers authored roll,
  scale, both entry directions and obstruction. All 33 real-player captures pass
  1,265 assertions with maximum difference 1/255 under the unchanged 2/255 bound.
- [ ] Deliver the other world's images through bus messages and bounded jobs;
  test ownership, stale/out-of-order replies, unload/reload and process isolation.
  Client and Studio use the shared image host; the remaining foreground adapter
  copies only seam clones. A child-process producer now returns captured pixels
  and renewal receipts; product endpoint discovery and continuous image sequences
  across process-host worlds remain open.
- [ ] Walk both directions through cross-world portals with one authoritative
  body and continuous subject, camera, movement, animation and collision.
  `ImmersivePortals` now uses engine transfer and preserves its actual rig in both
  directions. Physical cross-world overlap proxies, compound assemblies and
  camera continuity across the complete product image/arrival sequence remain open.
- [ ] Reproduce and fix the existing non-Euclidean portal demo using the common
  engine path, then inspect deterministic and interactive crossing sequences.
- [ ] Sweep every side/azimuth/elevation, grazing and off-centre views, near-plane
  crossings and look-back. The aperture must show the correct other side without
  holes, inversion, stale pixels or a detached rectangular picture.

### remaining portal proof and optimization gates

Close these gates against the authored portals used by Client and Studio, not
only hand-built render rows. Keep fixture-level results separate from a complete
player sequence. The following work connects the existing tests to that sequence:

| Requested check | Existing evidence location | Next required proof |
|---|---|---|
| Lighting and camera visuals | `render/tests/PortalFixtures.cpp`, `PortalCharacterFixtures.cpp` | Compare the full destination pipeline with an unfolded scene while the camera and lights move; inspect HDR, depth, aperture edges and final colour |
| Objects and Humanoid cameras | `render/tests/PortalCharacterFixtures.cpp`, `script/tests/PortalTransfer.cpp` | Collect actual seam clones into image requests; show rigid and animated clipped halves with the real Humanoid subject before, during and after acceptance |
| Images between worlds | `render/tests/PortalImageRuntime.cpp`, `PortalImageHost.cpp` | Extend the child-process capture/renewal fixture to product endpoint discovery and moving images across every active Studio viewport |
| Walking between worlds | `examples/tests/ImmersivePortals.cpp`, `script/tests/PortalOverlap.cpp` | Combine destination images, far-side contacts, transfer acknowledgement and camera adoption in one continuous bidirectional sequence |
| Non-Euclidean demo | `examples/tests/NonEuclidean.cpp` | Capture and inspect the actual deterministic tour and walking circuit, including each repaired opening and repeated look-back |
| All viewing angles | `render/tests/PortalFixtures.cpp`, `PortalImageDemand.cpp` | 2 mm approach now passes both delivery modes; add between-sample, moving-endpoint and aperture-edge probes |

For the combined sequence, record source and destination world generations,
capture request/tick, accepted body owner, resolved Humanoid subject and camera
pose beside each frame. Check approach, first overlap, root crossing,
acknowledgement, full exit and reverse entry. A frame with a correct background
but a missing limb, stale camera image or duplicate body fails the sequence.
Hold the camera still while a destination object, light or animation changes to
prove that camera-only caching cannot freeze the other world.

| Gate | Required evidence | Cost and residency check |
|---|---|---|
| Full destination composition | Direct/unfolded comparison with shadows, differing world sun/sky, AO, volumes, transparency and recursive mirrors/portals; retain intermediate HDR probes | Shared child scratch, transparent ordering per child, total recursive pixels and no duplicated global lighting |
| Actual product image path | Client and each Studio viewport request their own mapped camera; repeat with copied process messages and resident receipts | Bound jobs, queue bytes, imported images and GPU retirement; no CPU readback/upload for resident delivery |
| Receipt failure and lifetime | Superseded/out-of-order requests, full queue, failed graph/submit, timeout, unload/reload and endpoint generation changes | Cancel captures at source deadlines; no retained failed token, duplicate ownership or unbounded retry allocation |
| Physical player sequence | Actual Humanoid subject and complete animated rig, first/third person, both directions, obstruction and acknowledged session change | One authoritative body; deterministic joined contacts; no render-visibility dependency in physical crossing |
| Body straddling | Rigid, elongated, compound and animated objects contact far-side geometry before the root crosses; compare clipped halves in both worlds | Share immutable mesh/skin inputs; bound copied contact data and report proxy draw amplification |
| All-angle close approach | Both faces, roll, off-axis lens, grazing, near-plane, shrink/enlarge seams, moving endpoints and look-back; test points between fixed tour samples | Conservative aperture culling; destination-space clip bias; quantify error through resolution size transitions |
| Existing non-Euclidean demo | Inspect deterministic tour images and continuous player circuits using the same engine path | Compare static reuse with camera/light edits; no demo-specific rendering bypass |

For each performance comparison, hold scene, camera sequence and image tolerance
fixed. Measure release CPU busy/wait time, delayed GPU timestamps, bus bytes,
capture/upload/readback operations, live/peak GPU payload and resource creation
counts. An unchanged view must avoid capture and upload work; flat live memory
alone does not prove target reuse. Sweep 1/2/8 views across one and several worlds.
Resident delivery and copied process delivery need separate results. Finite angle
samples establish tested coverage, not a mathematical guarantee at every angle.

The close-approach demand test reproduced six failures at 2 mm: the nominal
3 mm near plane culled the mouth before capture. `PortalNearPlane` now uses
half the actual positive distance, bounded by the authored near plane. A
coplanar eye retains a nonzero fallback. This also avoids reversed clamp bounds
for small authored or scaled near planes. Full scene tests pass 514,758
assertions / 534 cases, including actual projection checks below 3 mm.

`ClearOfPanes` used the legacy fitted-camera clearance for cross-world mouths.
A reproduced 0.2-stud shove is corrected by sharing the local portal clearance.
The copied and resident image fixtures include both-side 6.1 mm and 2 mm
approaches. They now render the actual approach camera against the authored
mouth plane; the old diagnostic display plane was offset and was invalid for
these near views. Vulkan runtime/demand tests pass 3,959 assertions / 16 cases.
The existing local-portal sweep and Humanoid player image sequence also pass
8,299 assertions / 2 cases on Vulkan.
The shrinking-exit test independently checks 20 mm source approaches mapped
down to 0.5 mm at the destination.

These are sampled image and camera-clearance checks. A continuous product
Humanoid-camera crossing, moving endpoints, aperture edges and scales near the
oblique projection's coplanar tolerance remain required. No all-angle guarantee
or release performance conclusion follows from these samples.

## validation record

| Check | Result |
|---|---|
| Original frame prefix over two worlds | Reproduced duplicate setup, test exit 42 |
| Graph correction after scope review | 8,838 assertions / 223 cases passed |
| Render correction, excluding GPU | 28,001 assertions / 370 cases passed |
| ECS storage identity and existing suites | 132,762 assertions / 438 cases passed |
| Scene shader identity and existing suites | 498,946 assertions / 506 cases passed |
| Client and Studio after shader consumer fix | 9,212 / 163 cases and 5,929 / 515 cases passed; camera migration not included yet |
| Cooked shader asset format | 6,992 assertions / 10 cases; all assets 30,380 / 230 cases passed |
| Cooked shader parser fuzzing | Clang 21, ASan/UBSan, 10,000 runs passed; routine maximum input 64 KiB |
| Image comparison, normal render binary | 112 assertions / 8 cases passed |
| Signature benchmark build and run | 16 cases passed; no baseline accepted |
| First GPU fixture run | 16 failed assertions: aliased capture lifetime and stale default object damage reproduced |
| Corrected GPU image fixtures | Vulkan, 380 assertions / 4 cases passed, unchanged tolerances |
| Portal image oracle after readback fix | Vulkan, 612 assertions passed / 4 failed; four camera angles at two ambient levels; no exterior mismatches |
| HDR portal correction and expanded oracle | Vulkan, 970 assertions passed across 13 samples; opaque/custom/UI exact, transparent maximum 1/255 with unchanged 2/255 bound |
| Fitted camera correction | Vulkan, 211 assertions passed, including translated/rotated off-axis, oblique and orthographic lenses; initial missing-view culling defect reproduced |
| Expanded portal camera sweep | Vulkan, 7,034 assertions / 95 sections passed across 45 camera samples; near-plane approach on both faces exact, maximum sweep difference 1/255 within the unchanged 2/255 bound |
| Humanoid camera migration | Scene 501,269 assertions / 508 cases; physics 59,648 / 250 cases; client 9,213 / 163 cases passed |
| Studio camera migration | Missing fixture camera reproduced and corrected; 5,935 assertions / 515 cases passed; client 9,213 / 163 passed |
| Authored camera mode preservation | Three failures reproduced and corrected; game 960 assertions / 82 cases and ECS 132,771 / 439 cases passed |
| Presentation bus and real process transfer | World 27,885 assertions / 204 cases passed; focused 944 / 8, including 300 KB acknowledged process round trip |
| Actual process character handoff | Script 240 assertions / 7 cases and 51 child-process assertions passed after fixing external deliveries cleared at the barrier; full world 27,905 / 206 passed including snapshot and recreated-world delivery lifetime |
| Presentation parser fuzz | Clang 21 ASan/UBSan, 10,000 runs passed; 64 KiB routine mutation bound |
| Portal image codec and bounded inbox | 978 assertions / 10 cases passed before content-scope extension; current codec/inbox/runtime subset 1,069 / 16 passed |
| Owned HDR export | Vulkan, 7,309 assertions passed; partial-graph success defect reproduced and corrected; odd-width row stride, cancellation, caller-owned tokens and reuse checked |
| Remote HDR import | Vulkan, 167 assertions / 2 cases passed; independent colour oracle, zero local recursion, ownership, finite pixels, admission limits, unchanged-image reuse and unload |
| Cross-world image runtime subset | Vulkan, 138 assertions across 3 sections passed; two renderer devices, destination lighting, unchanged active camera, stale endpoint, image expiry and full-queue retry without rerender |
| Player camera continuation | First-person scale threshold and single-part Humanoid shift-lock defects reproduced and corrected; scene 513,541 assertions / 517 cases and focused physics character checks 42 / 9 passed |
| Studio exact portal arrival | Initial duplicate session and invalid adoption defects reproduced; 95 assertions / 3 cases passed before expanded chunked-join and disappearing-destination checks |
| Studio chunked portal arrival and lifecycle | Four stale-successor failures reproduced and corrected; 740 assertions / 5 focused cases and 6,670 / 519 full headless cases passed |
| ECS allocation replay | 80 failures reproduced; 1,331 assertions / 2 expanded cases passed. Existing full ECS 132,894 / 440 passed before the added test-only cases |
| Existing example scenes | 88,280 assertions / 53 cases passed after per-camera subject and carried-heading fixture migration; new immersive round-trip gate remains separate |
| Immersive authored scene | 10 failures reproduced; all 150 assertions / 3 cases pass after geometry, proximity and reciprocal-admission fixes. Updated Luau passes scriptcheck |
| Non-Euclidean openings and circuit | 22 opening failures and the absent pillar connector reproduced; all 26 openings, reciprocal mappings and two continuous humanoid laps pass 512 assertions / 4 cases. Updated Luau passes scriptcheck |
| Non-Euclidean fixed camera tour | 1,170 deterministic shots, repeatable seek, pause/step, malformed controls, mode changes and actual humanoid camera ownership pass; full examples 100,862 assertions / 61 cases |
| Explicit camera cuts | 20 assertions pass for previous/current pose reset, preserved humanoid subject and controller mode, continuous-write crossing and invalid-pose refusal; both scripting VMs expose `Camera:CutTo` |
| Actual player image sequence | 33 Vulkan captures pass 1,265 assertions, including both faces, first/third person, rolled/scaled exits, obstruction, straddle and no-input continuation; maximum error 1/255, unchanged limit 2/255 |
| Supplemental portal radiance | Shared ambient was reproduced at 0.482666 instead of 0.25. Corrected empty, emissive and local-light probes pass 1,314 assertions; shared global light is excluded from the supplemental probe. Different-world sun/sky transport remains open |
| Accessory ownership and transfer | Actual authored attachment hats, cloning/detach and animated transfer pass focused scene 12,177 / 18, full scene 514,701 / 534, script protocol 404 / 13 and actual process-child 74 / 1; both VM accessory/cut methods pass 24 / 2 |
| Mixed local capture executor | Vulkan runtime 875 assertions pass, including mirror-to-portal HDR half-float probes, child-facing beams, owned bus/import/direct tone parity and complete-world effect layers; planner 52 / 5 and effects 247 pass |
| Cross-world contact before centre crossing | Reproduced 0.1-metre destination-wall penetration for a long body and a real humanoid while each root remains source-side: 26 passed / 2 failed assertions. Deterministic contact exchange is in progress |
| Animated and ordinary body transfer | Focused scene 11,969 assertions / 10 cases, full script 665 / 74 and actual process-child 69 / 1 passed; renderer vertex parity 19 / 1 passed |
| Authored seam roll and body-side cuts | Full scene 514,473 assertions / 525 cases passed after six old fixed-front expectations were corrected; full player image sequence still expanding |
| Request-local surface rendering | Surface planner 40 assertions / 4 cases, mirror-only no-active-camera GPU control 63 assertions and billboard GPU focal-scale gate 66 assertions passed; mixed mirror/portal runtime still reproduced red |
| Source and architecture checks | Sourcecheck exits 0 with unused forward-API diagnostics; architecture passes 47 modules, 6 programs and 33 layered modules |
| Shader and scripting validation | 33 shader modules pass SPIR-V/MSL checks; 59 Luau examples and TypeScript declarations pass; no Metal device run |
| Full phase/build/backend acceptance | Incomplete; individual checks above do not establish it |
| Per-viewport portal reply ownership | Second viewport endpoint rejection reproduced. Canonical reply channels bind each source to its renderer slot; independent replies with equal request IDs pass 67 assertions / 6 runtime cases |
| Cross-world requested camera conversion | 36 both-face azimuth/elevation views through a rolled 1.5-scale seam, direct-ray comparison, off-axis lens, wire round trip and revision invalidation pass. Twelve malformed-input failures reproduced and corrected: finite rigid frames and nondegenerate orthogonal aperture axes are required before culling. All 1,044 assertions / 4 cases pass; product wiring remains open |
| Resident graph capture ownership | Vulkan resource-image subset passes 7,385 assertions / 4 cases. Submitted HDR captures move into local portal ownership without CPU readback/upload; early/wrong binding, repeated adoption and release checked. Resident and copied captures produce identical sampled portal pixels, with nonblank channel-order checks. Failed graphs refuse adoption and cancellation permits exact-token reuse after completion |
| Resident receipt codec and ownership table | Codec subset passes 1,141 assertions / 6 cases; combined codec/table subset passes 1,173 / 7. Bounded metadata, transactional truncation refusal, endpoint/request identity, reservation capacity and monotonic expiry checked |
| Resident and copied bus runtime | Vulkan runtime passes 1,114 assertions / 7 cases. Shared-renderer fixture delivers its receipt in the first producer pump using 204 bus bytes, with zero CPU image staging/upload. Separate-device copied delivery remains covered. Product integration, resident failure propagation and broader lifecycle combinations remain open |
| Resident publication and source deadlines | Failed-graph publication and delayed reservation expiry reproduced two failures. Publication now requires a successful submitted resident capture of the exact extent, rejects duplicate token ownership, and source expiry cancels its reservation. Queued/copied/adopted/wrong-extent captures cannot publish. Combined demand/runtime/resource/table gate passes 9,635 assertions / 18 cases on Vulkan; graph-refusal logs are expected negative cases |
| Shrinking-exit demand clipping | Clip bias uses destination length units. Both-face approaches with scales 0.025/0.05/0.5/2 retain the aperture between the mapped camera and far plane; demand subset passes 1,092 assertions / 5 cases. The default 2 mm approach is now covered separately above |
| Shared product presentation adapter | Host/runtime subset passes 1,162 assertions / 11 cases, including Vulkan. One destination handles two source viewports, equal portal names stay isolated, hidden views purge pending requests and release imported images, and removed endpoints can reopen. Host GPU test checks ownership and zero upload bytes; existing runtime fixtures supply pixel evidence. Product call-site integration remains open |
