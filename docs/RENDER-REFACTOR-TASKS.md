# render refactor execution

Implementation of [RENDER-REFACTOR.md](RENDER-REFACTOR.md), all R01-R17 and P0-P12.
The implementation request authorizes the planned destination. A checked phase
requires its functional and optimization evidence, not merely code or a build.

Current continuation order is at the top of
[RENDER-REFACTOR.md](RENDER-REFACTOR.md#continue-here-portal-crossing-2026-09-07).
The [handoff](PORTAL-HANDOFF.md) records known failures, test evidence and retained
diagnostic paths. The full working-tree checkpoint is not a completed phase.

## continuation 2026-09-08

- [x] P8 gameplay departure retirement: repeated lease requests receive an
  authenticated `LeaseAdopted` reply after destination adoption. Source checks
  the accepted claim, attempt and endpoint before removing its departure while
  leaving its connection available for presentation. Destination retry state
  remains bounded by the existing lease timeout. Both missing behaviors were
  reproduced before the fix. Codec/lease tests pass 2,660 assertions / 7 cases;
  server lease and process crossing tests pass 748 / 6. Full game passes
  4,124 / 93 and full server passes 1,543 / 76. Detailed evidence is in [PORTAL-HANDOFF.md](PORTAL-HANDOFF.md).
- [x] Retain one admitted source observer and its content session after product
  adoption. Stop its gameplay input/scripts while polling snapshots and serving
  content through its own connector. Share the intake budget across explicit
  world/session pumps. Delayed source content plus newly authored source and
  destination references pass 95 assertions with local fallback/cache disabled.
  Full headless Client passes 10,215 / 172; successor/observation checks pass
  3,866 / 5; 30/60 Hz product image handoff passes 17,098 / 1. Builds and format
  checks pass. Evidence and limitations are in [PORTAL-HANDOFF.md](PORTAL-HANDOFF.md).
- [x] Share the producer's world and camera collection through engine-owned
  `WorldViewFrame` and `WorldCameraFrame` packets. Preserve published replica
  interpolation, detach particle inputs, and copy per-camera spatial placements
  before leaving the store. Interface submission now accepts the owned packet.
  Waiting nested captures retain their preparation gate. Full render passes
  46,592 / 465; full Client passes 10,215 / 172; device `[portal-runtime]` passes
  24,161 / 18. Build, formatting and whitespace checks pass. This is the shared
  collection boundary; complete resource-aware foreign-eye binding remains open.
- [x] Bind retained source-world packets to the Client eye after adoption,
  including lighting, particles, ribbons, spatial UI and foreign straddlers.
  Keep screen UI with the active player through `WorldViewInterface`. Refuse
  failed presentation and hold the admitted eye while nested captures change
  from body layers to complete-room demand. The initial run failed three
  landmark assertions; after the gate, cold/warm 30/60 Hz handoff passes
  16,244 / 16,323 assertions and the 16-variant camera matrix passes 131,965.
  Full headless Client passes 10,215 / 172; world-view/interface checks pass
  90 / 3. Inspected evidence and the then-unresolved gray portal-border case are in
  [PORTAL-HANDOFF.md](PORTAL-HANDOFF.md). Resource namespace, shader/editable
  preparation and complete current-camera foreign rendering remain open.
- [x] Select explicitly admitted, ready local replicas for nested portal
  captures. Retain the previous observer while a visible portal needs it and
  present it before native portal demand. Host routing passes 33 assertions;
  full headless Client passes 10,232 / 172. Product checks now enforce producer
  ownership and matching position/rotation for the identity-seam fixture.
- [x] Close the reproduced return-adoption camera/body transition. Admitted
  local captures use complete world/body rendering instead of delayed body
  layers. Authenticated successors with ready rigs share the direct packet
  path and receive the existing prediction continuation before presentation.
  The initial local-capture and successor-only attempts still failed landmarks;
  copying the continued body pose closes the reproduced size jump. The 24-run
  product matrix passes 202,963 assertions / 4 cases with unchanged landmark
  thresholds and explicit current-camera/producer checks.
- [x] Resolve first-person body identity in directly viewed destination stores
  and retain account selection after source-player retirement. Final demand,
  rendering and lifetime checks pass: full render 46,636 / 466, full Client
  10,232 / 172, focused device-enabled body/shadow checks 967 / 9. The final
  account-selection adjustment follows the product matrix and is covered by
  those host/device checks. Fix the reproduced empty-world test registration
  order so the full render gate is independent of that test order. Builds and
  formatting pass. Artifacts and limitations are in [PORTAL-HANDOFF.md](PORTAL-HANDOFF.md).
- [ ] Complete observer endpoint/lifetime failure coverage and same-name content
  isolation, prepare complete foreign-world layers and close the moving-camera
  image gate. The prior image staging prototype remains rolled back; this
  packet path now draws retained sources and authenticated ready successors.
  Unadmitted eyes and remote nested views still use retained images. Staged
  content, per-world shaders, editable resources and viewport widgets remain
  part of the required preparation work.

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

## do not keep a ongoing log here.
