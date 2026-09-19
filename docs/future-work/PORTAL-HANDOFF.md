# Portal handoff

Updated 2026-09-08. Render work resumed from the committed checkpoint.

- Objective remains product endpoint discovery and continuous, seamless player crossing.
- Concise status: [ROADMAP](../ROADMAP.md). Detailed design and evidence: [render plan](RENDER-REFACTOR.md) and [task list](RENDER-REFACTOR-TASKS.md).
- The Terrain BVH change, benchmark job and initial handoff were committed as `cbe7a6ed`. The user subsequently explicitly requested committing all remaining working-tree changes together, including earlier changes of uncertain ownership.

## Verified work

- Terrain BVH construction caches centroids, partitions medians and preserves canonical leaf order. Release-derived bench: 8,192 triangles, 984 to 858 microseconds. Full Terrain worker cost remains unmeasured.
- Topology recovery accepts an authenticated current renewal after cache expiry, while retaining request deadlines. Ready topology replies are consumed before camera route selection.
- Product camera matrix passes all 16 variants: 30/60 Hz, first/third person, explicit/automatic Humanoid subject, held/released movement. `topology-ready-camera-matrix.log`: 136,117 assertions.
- Opt-in render-stage probe copies images before later stages overwrite them. Raw pixels, BMPs, metadata and an HTML index are saved. `render-stage-probe-final-gpu.log`: 756 assertions in two cases.
- Probe and topology changes are included with their wider renderer/client dependencies in the full checkpoint. No fresh full-suite validation was run for that checkpoint.

## Resumed lease retirement

- `LeaseAdopted` is a bounded claim-only world-bus reply. The destination renews
  its retry window and reports completed adoption on repeated lease requests.
  The source checks the attempt, endpoint and exact accepted claim before
  removing its departure. Its presentation connection is not closed.
- Reproduced both missing behaviors before the server fix: destination still
  returned `LeaseRoute`, and source still renewed after adoption. The focused
  run failed four assertions in two cases.
- After the fix, codec/lease tests pass 2,660 assertions in seven cases. Server
  lease and real-process crossing tests pass 748 assertions in six cases,
  including 30/60 Hz and Datagram/QUIC crossings. Source tests hold the old
  connection open, reject stale claims and replay the completed adoption reply.
- Full game suite passes 4,124 assertions in 93 cases; full server passes
  1,543 assertions in 76 cases. `test_game`, `test_server` and `server` build,
  and changed C++ files pass clang-format 21. Evidence is under
  `.cache/build/dev/tests/portal-lease-*.log`. No GPU or release performance
  claim accompanies this host-lifetime change.

## Resumed source observation and content

- Client adoption now retains one admitted source connection, socket, replica
  and content session. Its snapshots keep advancing; gameplay scripts, local
  input and prediction are stopped. Old session callbacks only accept content
  traffic. The observer retires on link loss, a new successor, departure of both
  the eye and visible portal demand from that world, or shutdown.
- Content pumping has one implementation in `ClientContent.cpp`, with explicit
  session and world inputs. Active and observed sessions keep separate signed
  catalogues, relays and pending requests, sharing one frame intake budget.
  Relay callbacks bind their owning connector rather than the replaceable
  active connection slot.
- The product content fixture holds the source manifest until destination input
  proves adoption, then publishes new texture references in both worlds. Each
  admitted route serves exactly one distinct texture bundle. Local fallback and
  cached content are disabled. This passes 95 assertions; the initial fixture
  failed two assertions because automatic local CDN discovery bypassed its relay.
- Final `client` and `test_client` builds pass. Full headless Client passes
  10,215 assertions in 172 cases. Successor and observation checks pass 3,866
  assertions in five cases; the 30/60 Hz product image handoff passes 17,098
  assertions in one case. Logs are `portal-observer-*.log` under the test build.
  Selected PNGs and metadata live in `portal-observer-evidence/`; inspected
  outbound 30 Hz and return 60 Hz adoption frames show the body and world floor.
- This slice does not select a new local eye renderer. Complete per-world
  presentation preparation and same-name asset isolation across publishers
  remain open. The texture fixture uses distinct names. Moving-camera parity,
  release profiling, sanitizers and interactive Studio were not rerun here.

## Shared world and camera packets

- `render/WorldView.hpp` now owns the shared collection boundary used by the
  portal producer. One world packet copies the published pose, joint palette,
  lighting, surface slots, seams, portals and detached particle inputs. It does
  not rebuild replica transforms or run presentation again. The packet cannot
  be copied because its particle batches refer to its own detached arrays.
- Ready cameras collect their own light selection, facing ribbons, spatial GUI
  commands and canvas placements. Waiting nested captures keep the existing
  preparation gate. `InterfacePass` can submit this packet without reopening
  the store; another camera cannot replace its billboard placements. The
  producer now submits spatial packets after leaving the world. Snapshot byte
  accounting includes the copied collector placements.
- New host coverage checks ownership after store destruction, reuse for an
  empty destination, published replica poses, light-budget selection, ribbons,
  billboard sizing and unchanged active-camera state. The first light fixture
  incorrectly expected sorting below the light budget; it was corrected to
  exercise camera-dependent selection above the budget.
- Final `test_render`, `client` and `test_client` builds pass. Full headless
  render passes 46,592 assertions in 465 cases; full headless Client passes
  10,215 in 172. Device-backed `[portal-runtime]` passes 24,161 assertions in
  18 cases, covering complete layers and requested-camera captures. Evidence:
  `world-view-final-build.log`, `world-view-final-host.log`,
  `world-view-client-host.log` and `world-view-final-gpu.log` under the test build.
  Formatting and whitespace checks pass. The producer build still reports
  existing missing-field-initializer warnings. No sanitizer or release profile
  was run for this slice.
- This moves the producer's collection into the engine's shared API. The next
  slice below binds the Client's retained source eye; content namespace
  isolation and the complete moving-camera image gate remain unfinished.

## Direct observed-world eye

- Client now presents its retained source replica and binds its owned world and
  camera packets when the eye remains there after player adoption. Geometry and
  palettes follow the received pose; lighting, lights, particles, ribbons,
  surfaces and spatial UI come from the observed world. The former input world
  does not supply those layers. Foreign straddlers keep their source identities
  and rebased palettes.
- `WorldViewInterface` routes spatial recording to the observed world's
  interface and screen recording to the active player's interface. Its host
  checks cover readiness refusal, separate ownership and a shared hook prepared
  only once. Captures record `observed_world` and `observed_tick`; product image
  walks require a direct observed frame with geometry and no whole-eye image.
- The first product run failed three landmark assertions. At initial adoption,
  source-player removal changes body-layer demand to complete-room demand. The
  direct path displayed the pane before the replacement capture arrived. It now
  retains the admitted eye until the nested image is ready and refuses a failed
  presentation phase. The original three-failure log is
  `observed-eye-product.log`; this does not establish that every intermittent
  landmark loss or the earlier valid-handle black frame has been resolved.
- After the readiness gate, cold 30/60 Hz image handoff passes 16,244 assertions
  and warm handoff passes 16,323. The 16-variant 30/60 Hz, first/third-person,
  explicit/automatic-subject, held/released-input matrix passes 131,965
  assertions. Logs: `observed-eye-readiness-product.log`,
  `observed-eye-camera-matrix.log` (warm only; an incorrect extra filter matched
  no cases), and `observed-eye-full-matrix.log`. Full headless Client passes
  10,215 assertions / 172 cases; world-view and interface routing checks pass
  90 / 3. Builds, formatting and whitespace checks pass.
- `observed-eye-evidence/` preserves the failed adoption frames, successful
  direct frames, and selected samples from all 20 completed product runs. Bulk
  captures were removed. Inspection shows the destination floor and body in the
  direct path, but `ready/60-179.png` still shows gray around a retained portal
  image. The directly rendered room does not fix parallax inside that image.
- Resource names still share the renderer's process-wide mesh/texture tables.
  Per-world shader, editable-resource and viewport-widget preparation also needs
  completion. These are explicit limits of the new binding, not completed
  foreign-world rendering. No release profile or sanitizer run accompanies it.

## Admitted local portal producers

- `UpdatePortalImages` accepts an explicit admitted destination replica. It
  must be snapshot-ready, match the authored destination, and differ from the
  source. Merely discovering a staged replica preserves the authenticated
  remote producer. Host routing coverage passes 33 assertions in one case.
- The observed source eye selects the active admitted replica for its nested
  portal. The previous world remains available while either the eye or a
  visible demanded portal needs it. Native presentation presents that observer
  before selecting it as the local destination producer. Retention stays bounded
  to one previous world; new successors and link loss still retire it.
- The earlier gray border in `observed-eye-evidence/ready/60-179.png` used a
  remote capture from a different camera. The inspected local-producer image
  in `observed-local-evidence/60-182.png` removes that large border. Product
  image checks now require the admitted producer, position within 1 mm, and
  equivalent rotation for the fixture's identity seam. This does not establish
  disocclusion correctness for remote or unadmitted worlds.
- The first local-routing run failed one return landmark assertion:
  `observed-local-product.log`, 16,531 / 16,532 assertions. Keeping the observer
  while its portal remains visible then passed cold/warm image handoff with
  38,707 assertions in two cases (`observed-visible-product.log`). Full
  headless Client passes 10,232 / 172 (`observed-visible-client-host.log`).
- Final validation added the rotation assertion and combined the 16 camera
  variants with cold/warm image handoff. It still fails one return-adoption
  landmark check: 192,211 / 192,212 assertions, two of three cases pass
  (`observed-local-final-product.log`). At 30 Hz cold frame 270, blue pixels
  fall from 334 to 132, below the required 167. The whole-eye fallback swaps
  remote requests 10 and 11, captured at substantially different camera
  positions, before native portal readiness. Frame 271 uses native geometry
  and the retained local destination producer. Do not report seamless handoff
  as passing or relax the landmark check. All new producer/pose checks pass.
- `observed-local-final-evidence/` preserves PNG/JSON samples from all 20 runs,
  including failed frames 268 through 271. Frames 269 and 270 were inspected;
  both show the brown source floor and body, with reduced blue floor behind it.
  Bulk captures were removed. Builds, formatting and whitespace checks pass.
  These are dev, offscreen Vulkan, 128-pixel fixtures. Release profiling,
  sanitizers, interactive Studio and `[eye-current-camera]` were not rerun.

## Current-camera successor presentation

- Local admitted portal demands now capture the complete world and body
  together. `ResidentDestinations` bypasses the delayed body-layer profile,
  preserving the requested recursion and pixel budgets. The producer can use
  its existing same-device resident image path instead of reading back separate
  body layers. Third-person includes the body; first-person retains the selected
  account identity. Remote and unrelated destinations keep their prior profile.
  Demand coverage passes 1,351 assertions in ten cases.
- Complete local captures fixed the native return camera lag, but did not by
  themselves close adoption: `resident-portal-product.log` fails two landmark
  checks out of 40,650 assertions. The preceding frame still used a remote eye.
- The Client now uses one `PortalWorldView` packet/preparation path for retained
  observers and authenticated successors whose player rig is ready. Both bind
  the live eye camera, world lighting, effects and spatial interface. Screen UI
  stays with the current input world. The selected packet is borrowed only for
  the draw and cleared when its session retires. Captures mark
  `observed_successor`, and image tests require actual pre-adoption direct frames
  with authenticated readiness and a drawable rig.
- Rendering the successor exposed a second discontinuity. Its replica body was
  behind the continued source prediction, so the body grew at adoption and
  obscured the floor. `successor-world-view-product.log` fails one of 40,792
  assertions; saved warm 60 Hz frames 246/247 show the size change even though
  both portal captures match the live camera. Successor preparation now copies
  the authenticated prediction continuation before presentation and refuses an
  unsuccessful application. Adoption takes over that same continued pose.
- With continued prediction, cold/warm 30/60 Hz handoff passes 40,831 assertions
  in two cases (`successor-prediction-view-product.log`). Inspected warm 60 Hz
  frames 244/245 in `successor-prediction-view-evidence/` retain body size and the
  blue destination floor. The combined 16 camera variants plus cold/warm handoff
  then pass 170,301 assertions in three cases
  (`successor-view-final-product.log`). All 20 runs have selected PNG/JSON
  evidence in `successor-view-matrix-evidence/`; bulk captures were removed.
- First-person foreign views also resolve their native body identity in the
  destination store, while keeping copied source rows separately hidden. This
  matters when the authenticated successor and source use different rig IDs.
- Full headless render initially aborted because the empty-particle-world test
  implicitly registered `ParticleSystem` before the world-view fixture installed
  its stable component name. The ordered two-test reproduction is in
  `resident-portal-registration-repro.log`. The empty-world fixture now registers
  its accessed effects components first. The formerly failing full-suite seed
  passes 46,633 assertions in 466 cases (`successor-view-final-render-host.log`).
  Full headless Client passes 10,232 / 172.
- With destination body lookup, all 24 product runs pass 202,963 assertions
  in four cases (`successor-view-body-product.log`): the 16 camera variants,
  four camera-clear walks and four cold/warm image walks. Selected artifacts
  from every run live in `successor-view-body-evidence/`, including 1,186 direct
  foreign frames and 163 authenticated successor frames counted in the summary.
  Bulk captures were removed. Final warm 60 Hz frames 276/277 were inspected:
  body size and the floor persist, but frame 277 has a thin vertical edge above
  the body's left side. These landmark checks do not establish pixel-perfect
  continuity; that edge still needs attribution.
- The final demand adjustment preserves first-person account selection even
  after the source's player row retires. Final builds and format/whitespace
  checks pass. Full headless render passes 46,636 / 466, full headless Client
  10,232 / 172, and the device-enabled body/shadow/portal checks 967 / 9.
  Logs: `resident-body-selection-build.log`,
  `resident-body-selection-render-host.log`,
  `resident-body-selection-client-host.log`, and
  `resident-body-selection-gpu.log`. The product matrix above precedes only
  this final account-selection adjustment; its demand and rendering behavior
  are covered by the final host and focused device checks.
- These are dev, offscreen Vulkan, 128-pixel checks. They do not close the remote
  moving-camera gate, same-name resource isolation, or per-world shader,
  editable-resource, viewport-widget and staged-content preparation. Release
  profiling, sanitizers and interactive Studio remain unrun for this slice.

## Scoped texture residency

`TextureTable` now keys residency and pending arrivals by content owner and asset
name. The default empty owner preserves the shared namespace. Scoped lookups
never fall back to shared content or another owner. Upload, adoption, replacement,
dimensions, flipbook lookup and removal use the same key. `DropOwner` releases
that owner's textures and cancels its pending arrivals while retaining shared
entries. The existing byte limit remains shared across all owners.

This initially established the texture table contract only. Client uploads and
render consumers still use the shared namespace. Same-name product asset
isolation requires carrying session ownership through those paths, including
material maps, environment, particles and UI, before this gate can close.

- Focused host/device checks pass 70 assertions in two cases. They cover same-name
  uploads, replacement, adoption, independent pending arrivals, animation and
  owner retirement. The first GPU run failed six accounting assertions because
  initialization installs a built-in texture; the corrected fixture measures
  count and byte deltas from that baseline. Both logs remain available.
- Full headless render passes 46,651 assertions in 467 cases; full headless Client
  passes 10,232 in 172. Client, Studio and their test targets build successfully.
  Formatting and whitespace checks pass.
- Logs under `.cache/build/dev/tests/`: `texture-owner-gpu.log` records the initial
  fixture failure; `texture-owner-final-gpu.log`,
  `texture-owner-final-render-host.log`, `texture-owner-final-client-host.log`,
  `texture-owner-final-build.log` and `texture-owner-studio-build.log` record the
  final checks. No product portal matrix, release profiling, sanitizers or
  interactive Studio check was run for this table change.

## Scoped mesh residency and renderer calls

`MeshTable` now uses the same owner/name namespace for registration and lookup.
Retiring an owner removes its entries while returning their vertex/index ranges
to the existing deferred reuse queue. Other owners and shared built-ins remain
registered. Missing scoped meshes use the normal fallback instead of another
owner's same-name mesh.

`Renderer` exposes the owner on mesh/texture upload and lookup operations, pending
texture arrivals and texture removal. `DropContentOwner` retires both tables and
pending arrivals, advancing the resource revision only when something changed.
These calls prepare the product boundary; Client uploads and draw consumers
still need a content-session owner carried through them. Do not close the product
same-name isolation gate from these table/API checks.

- Mesh host checks pass 301 assertions in 12 cases (`mesh-owner-host.log`),
  including independent same-name bounds, replacement, invalid replacement,
  scoped absence, retirement and deferred range reuse.
- The first device run failed at renderer initialization in both device cases
  (`content-owner-gpu.log`). The built-in fallback lookup still used the old
  numeric key; the empty owner's nonzero sentinel made it miss. The lookup now
  uses the compound key. Final offscreen Vulkan checks pass 106 assertions in
  three cases (`content-owner-final-gpu.log`), exercising both tables through the
  renderer and verifying resource revision changes and unaffected owners.
- Full headless render passes 46,682 assertions in 468 cases
  (`content-owner-final-render-host.log`). Client passes 10,232 in 172
  (`content-owner-client-host.log`, before the final fallback-lookup correction).
  Client, Studio and their test binaries build successfully
  (`content-owner-final-build.log`). This build reports existing initializer
  warnings in portal runtime/tests and Client replication, plus a structured
  binding copy warning in PortalImageImport tests. These files were not changed
  by the mesh/renderer ownership slice. Formatting and whitespace checks pass.
- Product portal walks, interactive Studio, sanitizers and release profiling
  were not rerun for this slice. Next, attach a session-owned namespace to view
  preparation and carry it through mesh/submesh material bindings, skybox,
  particle/ribbon and UI texture lookup before migrating Client uploads. A
  publisher key alone is insufficient because one publisher can serve distinct
  manifests with the same authored names.

## Draw-time content bindings

`View` now carries a native content owner and explicit foreign-world owner
bindings. Authored resource names and `DrawInstance::SourceWorld` remain intact.
The unloaded-mesh filter passes the complete source row to its residency test;
mesh lookup, draw metadata and material texture lookup select that row's owner.
Draw runs split when owners differ even when their mesh pointer and texture name
match. Built-in meshes and the built-in checker keep shared residency.

Native environment, particle and ribbon texture bindings use the active view's
content owner. Owner/world bindings and resource revision changes invalidate
retained object metadata and force scene work, even if the caller supplied a
quiet-frame damage mask. Client content-session assignment, UI callbacks and
foreign packet binding still need wiring before the product isolation gate can
close. No resource names are rewritten in ECS storage or serialized as ids.

The first scoped image fixture caught a quiet-frame integration fault: view
preparation marked the new bindings dirty, but node registration still read the
original damage mask and installed idle handlers. Registration now reads the
prepared request. The initial failure is retained in `content-binding-gpu.log`;
the corrected run passes 184 assertions (`content-binding-fixed-gpu.log`).

Final evidence:

- The scoped fixture now compares rendered albedo against distinct-name reference
  images for shared built-in geometry and named meshes with submesh textures.
  It checks native/copy binding, a native owner swap, a foreign-only owner swap
  under a quiet damage mask, and retirement that removes only the absent mesh.
  The final focused run passes 240 assertions (`content-binding-foreign-gpu.log`).
- The broader offscreen Vulkan camera/resource/particle sweep passes 8,397
  assertions in 13 cases (`content-binding-final-gpu.log`). It precedes only the
  final foreign-only fixture extension; renderer code did not change afterward.
  Native environment/particle/ribbon scope plumbing is present, but those paths
  do not yet have the same owner-specific pixel proof as object albedo.
- Full headless render passes 46,682 / 468, scene 518,302 / 550 and Client
  10,232 / 172 (`content-binding-final-{render,scene,client}-host.log`). The focused
  source-world filter suite passes 151 / 29 (`content-binding-scene-host.log`).
- Client, Studio and all affected test targets build
  (`content-binding-final-build.log`). A new test initializer warning was removed;
  `content-binding-test-build.log` and `content-binding-foreign-build.log` are clean.
  The broader build retains initializer warnings in game PortalSession and Client
  Replicated code outside this slice. Formatting and whitespace checks pass.
- Product content-session wiring, spatial/screen UI callbacks, staged content,
  per-world shaders/editable resources/widgets, the full portal matrix, release
  profiling, sanitizers and interactive Studio remain open. These renderer
  fixtures do not prove end-to-end same-name product asset isolation.

## Portal capture content owners

Local portal producers now accept a native content owner and copied foreign-world
bindings. `PortalImageHost::SetContentOwner` configures existing producers and
producers opened later; removing a world or clearing the host retires its binding.
Both the camera preparation and capture draw use those owners. Spatial UI image
lookup uses the producer's native owner, including dimensions and flipbook cells.
Bindings contribute to the portal content revision, so changing an owner without
uploading an asset still prevents renewal of the preceding content version.

Renderer mesh/texture lookup APIs now resolve built-in assets through shared
residency, matching the draw path. Ordinary scoped names retain strict lookup.
Client uploads and session-to-view/UI assignment remain shared until they are
migrated together; this producer adapter is a prerequisite for that migration.

The spatial image fixture reproduced an additional renewal defect: when UI was
the only visible layer, texture replacement, owner changes and flipbook frames
changed pixels without changing `ContentRevision`. `ScenePresentationSignature`
deliberately excludes UI. The producer now folds resource and animation inputs
into the complete capture revision as well, leaving the scene-layer contract
unchanged. The failing run recorded ten assertions in
`portal-content-owner-final-gpu.log`; the earlier mesh-only run passed 319
assertions in `portal-content-owner-gpu.log`.

- Final offscreen Vulkan checks pass 1,006 assertions in five cases
  (`portal-content-owner-renewal-gpu.log`). The portal fixture covers direct and
  hosted producers with ordinary surfaces and spatial image labels. It checks
  owner switches without asset writes, texture replacement/removal/restoration,
  flipbook advancement, configuration before producer creation and binding
  retirement. Changed-image requests carry the previous known version and must
  return fresh pixels instead of a renewal. Renderer binding and built-in lookup
  checks are included in that run.
- Full headless render passes 46,682 / 468
  (`portal-content-owner-signature-host.log`). Client passes 10,232 / 172
  (`portal-content-owner-final-client-host.log`, before the final complete-capture
  signature correction). Client, Studio and their test targets rebuild
  (`portal-content-owner-signature-build.log`); the final known-version test-only
  extension builds in `portal-content-owner-renewal-build.log`. Existing missing
  initializer warnings remain in portal runtime/test and Client replication code.
  Formatting and whitespace checks pass.
- No product portal matrix, release profiling, sanitizers or interactive Studio
  check was rerun. Client session assignment, owner-scoped delivery, editable
  uploads and main/observed UI callbacks remain the next integration work.

## Editable resource upload scopes

Editable image and mesh uploaders accept a content owner and track revisions by
store identity, owner and entity handle. Generated editable names contain the
entity handle, which can repeat in another world. Callers must assign distinct
owners to those worlds even when they share a delivery session. Alternating
between prepared worlds preserves each scope's upload stamps.

`ForgetWorld` and `ForgetOwner` retire upload stamps explicitly; they do not
release GPU resources. The renderer's existing owner retirement controls device
lifetime. Existing product calls retain the shared owner default until Client
assignment, lookup and retirement migrate together.

- Offscreen Vulkan checks pass 54 assertions in two cases
  (`editable-owner-gpu.log`). They cover colliding entity handles and content
  names, distinct mesh bounds/image dimensions, unchanged refreshes, changed
  image revisions, world cache retirement, owner cache/device retirement and
  rebinding to another owner.
- Full headless render passes 46,681 assertions in 468 cases
  (`editable-owner-host.log`). The render target builds in
  `editable-owner-build.log`.
- Client host checks pass 10,232 assertions in 172 cases
  (`editable-owner-client-host.log`). Client, Studio and their test targets build
  without warnings in `editable-owner-consumers-build.log`. Only header comments
  changed after that build. Formatting and whitespace checks pass.
- No product portal matrix, release profiling, sanitizers or interactive Studio
  check was run for this slice. These uploader contracts do not establish
  end-to-end Client asset isolation.

## Portal editable preparation

Portal producers now refresh editable images and meshes in their native content
owner before collecting the world and checking capture revisions. Preparation
runs once for the demanded capture batch. Each producer keeps its own upload
stamps; changing its owner or clearing the producer retires those stamps without
releasing resources owned by other views.

The device fixture reproduced the missing preparation before the change: all
four direct/hosted and ordinary/spatial image variants returned a capture with
no editable texture in the configured owner (`portal-editable-before-gpu.log`,
four failed assertions out of 224). The first implementation run passed the
image variants, including known-version requests after edits. Its mesh pixel
check failed twice (`portal-editable-gpu.log`) while scoped upload, changed
bounds and changed capture revisions passed. The mesh fixture now changes its
shape at fixed bounds and includes both face windings.

- Final offscreen Vulkan checks pass 1,386 assertions in four cases
  (`portal-editable-lifetime-gpu.log`). They cover scoped editable image/mesh
  preparation, no repeated uploads on unchanged captures, changed pixels and
  revisions after edits, known-version replies, image owner rebinding and
  producer clearing/reopening. Existing upload-owner and draw-binding checks
  are included.
- Full headless render passes 46,681 assertions in 468 cases
  (`portal-editable-final-host.log`). Client, Studio and their test targets
  rebuild (`portal-editable-final-build.log`); existing missing-initializer
  warnings remain in portal runtime and test code. The final test-only lifetime
  extension builds in `portal-editable-lifetime-build.log`. Formatting and
  whitespace checks pass.
- No product portal matrix, release profiling, sanitizers or interactive Studio
  check was rerun. Per-producer preparation is still outside graph execution;
  the graph-owned residency requirement remains open.

Client delivery ownership and staged content remain open. Preparing staged
uploads in the current shared namespace would allow successor assets to replace
source assets, so the content pump must migrate with its view/UI bindings.
Per-world shader and viewport-widget preparation also remain open.

## Client world residency bindings

Client uses each local world's name as its mesh/texture residency owner. Delivery
decodes once and uploads to the worlds served by that content route. Pending
texture markers follow the same owners. Adding a served world replays demand for
its missing resident copies and catalogue facts while preserving outstanding
requests; removing a served world clears its pending markers.

Native and copied-world views, main/observed image callbacks, portal producers
and main viewport widgets use those bindings. Editable preparation runs before
interface compilation and separately for observed/staged world packets. Dropping
a portal replica retires its upload stamps and renderer owner. Authored replica
names also map to their local residency names for copied rows.

A staged successor now owns a content client and relay before adoption. Its
directory and content replies are consumed while it is staged, and its intake
shares the frame budget with the active and retained source routes. Adoption
moves that content session with its existing connection instead of rebuilding
it. Retry destroys the content borrower before the old connection.

- The broader product run passes 30,694 assertions in two cases / three crossing
  runs (`client-world-owner-final-product-gpu.log`): the new editable room-colour
  fixture at 60 Hz and the existing warm-image fixtures at 30/60 Hz. Both legs,
  source retention and staged eye drawing are exercised. These are offscreen
  Vulkan dev runs; the full product matrix was not rerun.
- Final review connected observed editable uploads to the Client resource-change
  flag, matching main-world preparation. The final textured product repeat passes
  10,349 assertions (`client-world-owner-observed-gpu.log`) and Client headless
  remains 10,232 / 172 (`client-world-owner-observed-host.log`). Client/Studio and
  their tests rebuild in `client-world-owner-observed-build.log`. Textured captures
  now have a separate `-warm-owned` directory suffix; the earlier artifact-only
  repeat passed 10,329 assertions (`client-world-owner-artifact-gpu.log`).
- Viewport/resource device checks pass 368 assertions in four cases
  (`client-world-owner-viewport-final-gpu.log`). The viewport fixture compares
  same-name scoped textures to separate-name reference images, including an
  owner switch in the same scene slot.
- Full Client headless passes 10,232 / 172 (`client-world-owner-final-host.log`);
  render headless passes 46,681 / 468 (`client-world-owner-render-host.log`).
  Client, Studio and affected tests build; the broader implementation build is
  clean (`client-world-owner-final-check-build.log`). Formatting and whitespace
  checks pass.
- Failed development evidence is retained. The first product fixture used
  `TextureID` on a Part instead of a MeshPart (`client-world-owner-product-gpu.log`).
  The next oracle incorrectly expected the source colour even when its sampled
  camera ray crossed the portal first (`client-world-owner-meshpart-gpu.log`,
  19 colour assertions). The corrected oracle intersects the captured camera ray
  with the aperture and floor (`client-world-owner-oracle-gpu.log`, 10,373 passed,
  before the final pending-marker bookkeeping adjustment). The viewport fixture
  needed renderer warm-up and separate-name reference images instead of assumed
  raw primary channel values (`client-world-owner-viewport-gpu.log`).

Same-name published-asset delivery and failure/retirement still need end-to-end
proof. Shaders remain shared; foreign viewport-widget preparation and graph-owned
residency remain open. No release profiling, sanitizers or interactive Studio
verification was run. The per-world copies and full binding list are correctness
plumbing, not a claim of minimal residency or cache invalidation cost.

## Published texture ownership proof

The existing retained-route fixture passes on the current Client (96 assertions
in `published-owner-baseline.log`). Its new rendered variant uses the same
texture name in two distinct manifests signed by one publisher identity. A
destination wall references its texture before adoption. The fixture holds the
commit reply until the destination bundle is served, so delivery must progress
while the successor is staged. The source manifest remains withheld until
destination input proves adoption, then the retained source asks for its texture.

The final captured frame must report two accepted texture arrivals, a native
destination view with its destination content owner, and blue destination pixels
after the source's red texture arrives. Each route requests its bundle exactly
once. This checks actual signed delivery and Renderer uploads, not only a table
API or a served network reply. Capture JSON now includes `content_owner`,
`delivered_meshes` and `delivered_textures`; the latter two count accepted asset
arrivals, not GPU upload operations or live resources.

- Final offscreen Vulkan dev run: 210 assertions in two product cases
  (`published-owner-final-gpu.log`). The first rendered run with staging enforced
  passed 110 assertions (`published-owner-staging-gpu.log`), before the accepted
  asset metadata and exact request-count assertions were added.
- Client, Studio and their test targets build without warnings
  (`published-owner-final-build.log`). Formatting and whitespace checks pass.
  No broad headless suite was rerun for this test/diagnostic-only change.
- The first rendered launch mistakenly removed `--headless`, causing an early
  signal exit under the offscreen driver (`published-owner-gpu.log`). Restoring
  the Client's existing headless render mode corrected the fixture. This was
  not diagnosed or reported as a renderer defect.
- The final image and JSON sequence is `portal-published-owner-frames/` under
  `.cache/build/dev/tests/`. Published mesh isolation, failed delivery and owner
  retirement still need product proof. Shader isolation and graph-owned
  residency remain open. No release profiling, sanitizers or interactive Studio
  check was run.

## Published mesh, refusal and per-owner demand

The rendered delivery fixture now includes same-name meshes. The destination
publishes a blue quad and the late source publishes a red triangle with the same
bounds. Refusal variants serve the destination normally and refuse the source
bundle after adoption. The final frame must keep destination pixels, report only
the accepted assets, retain the source world and have no pending content requests.
The new `pending_content` capture field sums issued/pending asset requests across
active, staged and retained content routes; `retained_world` records that world's
local name even while the native destination is drawn.

The wider run reproduced a timing defect in Client demand history. When the
destination asset completed while staged, adding the local world at adoption
cleared the session's entire `Asked` set and fetched the destination asset again.
The failing run recorded six assertions across texture success/refusal variants
(`published-mesh-refusal-gpu.log`), including two bundle requests and excess
accepted asset arrivals. Earlier runs had moved still-pending requests and missed
this ordering.

Attempts are now keyed by owner and asset name. New worlds get catalogue facts
and their own demand scan without invalidating existing worlds' history. Pending
requests are coalesced and marked for their current owners. A later local-world
reference still requests an already-delivered name when that world lacks its own
copy. The fixture delays commit acknowledgement by 200 ms after serving the
destination bundle to force the completed-before-adoption ordering; a separate
variant creates its local reference three seconds into the run and requires the
second request to occur afterward.

- The first corrected sweep passes 584 assertions in three cases / five product
  runs (`published-owner-demand-gpu.log`). Full headless Client passes 10,232 / 172
  (`published-owner-demand-host.log`). Client, Studio and their tests rebuild
  (`published-owner-demand-build.log`); the strengthened test fixture builds in
  `published-owner-demand-test-build.log`. Formatting and whitespace checks pass.
- Final bounded sweep passes 680 assertions in four cases / six product runs,
  including the existing retained-route case (`published-owner-bounded-gpu.log`).
  The capture-cadence test change builds in `published-owner-bounded-build.log`.
- Capture transitions and final frames are retained in
  `portal-published-owner-{10,11,12,13,14}-frames/`; bulk frames were removed.
  The fixture now uses `--uncapped --max-fps 60` to bound subsequent capture output.
- GPU owner retirement, shader isolation and foreign viewport-widget preparation
  remain open. These checks do not establish graph-owned residency or minimal GPU
  copies when a new world needs an existing asset. No release profiling,
  sanitizers or interactive Studio check was run.

## Submitted-draw owner retirement

The existing native/copied owner fixture now retires the source owner immediately
after submitting a draw, before its readback wait. The submitted image still
matches the two-owner reference. The next quiet view removes the retired mesh,
and reloading the same owner restores the reference without explicit damage.
Twelve retire/reload cycles preserve the other owner's texture handle and pixels.
Each retirement releases one four-byte texture in the logical GPU counters;
texture residency returns to its preceding level after reload, and mesh buffer
bytes remain flat after four warmup cycles.

- Final Vulkan offscreen checks pass 1,283 assertions in 16 cases, including the
  existing texture-owner and mesh-table checks (`owner-retirement-final-gpu.log`).
- Full headless render passes 46,682 assertions in 468 cases
  (`owner-retirement-host.log`); build passes (`owner-retirement-final-build.log`).
- The first attempt failed because an unchanged restored view correctly skipped
  its next draw (`owner-retirement-gpu.log`, 692 passing assertions and one failure).
  The fixture now explicitly requests that submission, then clears damage before
  retirement to preserve the invalidation check. No production fix was needed.
- These are renderer device checks, not Client-driven teardown proof. Submitting
  before retirement does not guarantee the tested GPU was still executing at the
  moment of release. Delayed-completion stress, driver heap commitments, complete
  world teardown and graph-owned residency remain unproven. No release profiling,
  sanitizers or interactive Studio run was performed.

## Client observation expiry and texture retirement

The published-content fixture now also closes the source UDP transport one
second after replying with its late texture. The successor connection keeps
running while the retained source times out. Capture metadata records logical
GPU live/buffer/texture bytes, texture count and cumulative released bytes.
These counters exclude driver heap commitments and include capture transfers.

The first offscreen product check passes 123 assertions
(`client-owner-retirement-gpu.log`). Both same-name textures arrive before
expiry. The observed transition reduces residency from 47 textures and
19,661,832 texture bytes to 46 textures and 19,661,828 bytes. The final frame keeps
the successor owner and blue pixels. The final assertions require exactly one
four-byte texture to disappear, unchanged buffer bytes and at least one second
of successor rendering after the last retained-world capture. Both content
routes request their bundle once and the final pending request count is zero.

Full headless Client checks pass 10,232 assertions in 172 cases
(`client-owner-retirement-host.log`). Client and test builds pass in
`client-owner-retirement-build.log`; the tightened fixture builds in
`client-owner-retirement-final-build.log`. The final device sweep passes 357
assertions in two cases / three product runs, covering retained texture/mesh
owners beside texture expiry (`client-owner-retirement-final-gpu.log`). Transition
and final captures remain in `portal-published-owner-{10,11,15}-frames/` under the
build's `tests/` directory; bulk frames were removed.

This covers Client-driven observation expiry for published textures. Product mesh
range reclamation, staged cancellation with resident assets, repeated world
teardown, delayed GPU completion and shader ownership still need proof. It does
not establish graph-owned residency or driver memory release timing.

## Owner-scoped shader compiler cache

`ShaderLibrary` now accepts a residency owner on material/lens refresh and lookup.
Each module key includes its owner, and a refresh removes stale demand only from
that owner's module family. Alternating worlds can retain independently accepted
SPIR-V under the same name without recompiling unchanged sources. Failed edits
retain only that owner's accepted code. Scoped misses never borrow shared modules.
`DropOwner` removes both families and reports their removed names through the
changed lists; the empty owner preserves the shared namespace.

Headless shader checks pass 550 assertions in 16 cases
(`shader-owner-library-tests.log`). New checks cover both families, alternating
same-name worlds, steady refresh, shared/scoped separation, failed edits, empty
demand, re-demand after eviction and owner retirement. The initial build caught
a Catch expression needing parentheses (`shader-owner-library-build.log`);
the test expression was corrected. Client, Studio and both test binaries build
(`shader-owner-library-final-build.log`). Full headless render passes 46,768
assertions in 469 cases (`shader-owner-library-host.log`), and Client passes
10,232 in 172 (`shader-owner-library-client-host.log`). Formatting and diff checks
pass. No device run or release profile was added for this host-cache change.

These compiler-cache checks alone do not establish device isolation. The material
and Client migration below uses this contract. Interface and postprocess
device isolation, Studio migration, product shader isolation, graph-owned
preparation and cooked-only migration remain open.

## Material shader device ownership and Client cache migration

Renderer material variants now use owner/name keys for registration, replacement,
lookup and removal. Native and copied draw runs resolve through their existing
slot content owner, including HDR opaque and transparent portal draws. A missing
scoped variant uses the engine default, never a shared same-name variant.
`DropContentOwner` releases that owner's material variants after the existing
frame wait and invalidates quiet views while preserving other owners.

Client now refreshes and queries compiler modules under the presented world's
residency name, uploads material variants into that namespace and drops compiler
entries when a portal replica retires. Returning to a cached owner preserves its
unchanged material pipelines. Interface, lens and postprocess device slots are
still shared, so an owner switch explicitly restores their selected programs
from that owner's cache. This is not simultaneous foreign-view shader isolation.
Shader preparation still runs only for the presented world.

The offscreen Vulkan device sweep passes 1,350 assertions in five cases
(`shader-owner-device-reference-gpu.log`). Same-name constant-colour programs are
compared with distinct-name references through portal draws, in both opaque and
transparent modes. Checks cover owner swaps on quiet views, replacement, refused
empty replacement, individual shader removal, strict scoped fallback, restoration
and surviving pixels after complete owner retirement. The earlier retirement
reference was stale because the fixture did not request a draw after changing
its instance span: two checks failed out of 1,352
(`shader-owner-device-final-gpu.log`). Explicit reference damage fixes the fixture;
the subsequent retirement still relies on automatic resource invalidation.

Client, Studio and both test binaries build (`shader-owner-device-final-build.log`,
`shader-owner-client-cache-build.log`); the corrected device fixture builds in
`shader-owner-device-reference-build.log`. Headless render passes 46,768 assertions
in 469 cases (`shader-owner-device-host.log`). No release profiling, delayed-GPU
stress, sanitizer or interactive Studio run was added. Native deferred custom
material coverage and product same-name shader crossing still need explicit proof.

The remaining shader work includes owner-scoped interface/postprocess device
consumers, preparation for retained/staged/foreign worlds, Studio migration and
product shader lifecycle checks. Those are required before the resource ownership
gate can close.

Final Client headless checks pass 10,232 assertions in 172 cases
(`shader-owner-client-cache-host.log`). The owned editable-room round trip and
published-texture observation expiry pass 10,493 assertions in two offscreen
product cases (`shader-owner-client-cache-gpu.log`). These establish compatibility
with the cache migration, not product custom-shader isolation. Transition/final
frames remain in `portal-client-walk-60-third-explicit-held-clear-image-frames-warm-owned/`
and `portal-published-owner-15-frames/`; bulk captures were removed. Formatting and
diff checks pass.

## Lens shader device ownership

Lens registration, lookup and removal now include the residency owner. The
`shader-lenses` node selects each pipeline with its recorded view's content owner,
so two views sharing a shader name can execute different programs in one frame.
There is no shared-name fallback for a scoped miss. Whole-owner retirement also
releases lens pipelines through the existing frame-wait boundary.

Client uploads and removes lens programs under the presented world's owner.
Returning to an unchanged owner does not rebuild its accepted lens pipelines;
changed modules and missing device entries still upload. The compiler cache and
material/lens device tables now agree on that namespace. Interface and postprocess
device isolation, foreign-world preparation and Studio migration remain open.

Offscreen Vulkan checks pass 620 assertions in three cases (`lens-owner-gpu.log`).
The new two-view case verifies distinct same-name outputs, quiet owner swaps,
refused empty replacement, successful replacement, individual removal, strict
scoped misses, restoration and surviving pixels after owner retirement. The sweep
also includes material shader ownership and the existing portal check proving
opaque lighting is unaffected by later lenses. Headless render passes 46,767
assertions in 469 cases (`lens-owner-host.log`). Headless Client passes 10,232
in 172 (`lens-owner-client-host.log`). Client, Studio and both test binaries build
(`lens-owner-build.log`); formatting and diff checks pass.

These checks prove native view/lens pipeline ownership on the tested device.
Product same-name lens crossing and preparation for retained/staged/foreign views
remain unproven. No release profiling, delayed-GPU stress, sanitizer or interactive
Studio run was added.

## Postprocess device ownership

Postprocess selection now keeps one shader/pipeline pair per residency owner.
The tonemap node selects from its recorded view's exact owner, with the engine
tonemap as fallback. Shared grades cannot fill scoped misses. A successful
replacement waits before releasing that owner's old pipeline; failed replacement
keeps the accepted program. Clearing a selection and whole-owner retirement both
preserve other owners. The plain-tonemap portal preview branch is unchanged.

Client sets, queries and clears the presented owner's postprocess slot. Returning
to a cached unchanged grade keeps its device pipeline. The remembered selection
used by presentation signatures follows the current owner's device slot. Portal
producer clock invalidation also queries its own owner rather than the shared
selection. The Client owner field is now named `LastShaderOwner`, since it covers
material, lens and postprocess preparation.

Offscreen Vulkan checks pass 823 assertions in four cases
(`postprocess-owner-gpu.log`). The new two-view case verifies distinct same-name
grades, quiet owner swaps, refused empty replacement, successful replacement,
clear/fallback, restoration and surviving output after owner retirement. The
sweep also covers lens/material ownership and the existing portal lens boundary.
Headless render passes 46,768 assertions in 469 cases
(`postprocess-owner-host.log`). Client, Studio and both test binaries build in
`postprocess-owner-build.log`. The Client owner-field rename builds in
`postprocess-owner-client-build.log`; final headless Client checks pass 10,232
assertions in 172 cases (`postprocess-owner-client-host.log`). Formatting and
diff checks pass.

Interface shader device ownership, foreign-world preparation, Studio migration
and product same-name shader/grade crossing remain open. These device checks do
not prove complete foreign-view preparation or the full render refactor. No
release profiling, delayed-GPU stress, sanitizers or interactive Studio run was
added.

## Screen interface shader ownership

`InterfacePass` screen shader variants now use owner/name keys. `SetContentOwner`
selects the submitted world's namespace without rebuilding geometry; callers
invalidate their interface image when switching owners. Scoped misses use the
pass default rather than shared same-name variants. Registration, replacement,
lookup, individual removal and whole-owner retirement all preserve other owners.

Client selects the presented interface world's owner and uploads changed or
missing screen shader variants into that namespace. Returning to a cached owner
keeps its accepted pipelines. Replica retirement drops that owner's interface
variants beside compiler and renderer resources. Direct portal packets and portal
producer interface passes also receive their content owner at submission.

The Vulkan device sweep passes 953 assertions in four cases
(`interface-owner-gpu.log`). A real screen rectangle checks distinct same-name
programs, owner switching with zero geometry upload, refused empty replacement,
successful replacement, strict fallback, restoration and retirement. The sweep
includes material, lens and postprocess ownership. Headless render passes 46,768
assertions in 469 cases (`interface-owner-host.log`). Client, Studio and both test
binaries build (`interface-owner-build.log`). Final headless Client checks pass
10,232 assertions in 172 cases (`interface-owner-client-host.log`). Formatting and
diff checks pass.

Spatial interface rendering currently uses fixed pipelines and does not select
custom `DrawCommand::Shader` variants. Screen ownership does not close that gap.
Custom spatial shader support, foreign-world shader preparation, Studio migration
and product same-name shader crossing remain open. No release profiling,
delayed-GPU stress, sanitizers or interactive Studio run was added.

## Spatial interface shaders and fragment clipping

The interface owner fixture reproduced ignored spatial shader selection: red and
green programs produced identical spatial output, with two failed checks out of
266 (`spatial-shader-missing-gpu.log`). Shader registration now builds a coherent
set of screen, depth-tested spatial, always-on-top spatial and supported HDR
variants. `RecordWorld` selects the owner-scoped variant for its target and depth
mode. Partial pipeline creation is refused without replacing the accepted set;
replacement, owner retirement and shutdown release every variant.

A stronger clipping check reproduced a second defect. Both default and custom
interface shader creation declared zero fragment uniform buffers even though the
fragment shader reads the collector clip block. Screen GPU scissors concealed
the omission; spatial pixels outside the clip remained green. The probe records
outside green `(0,255,0)` against background `(23,15,13)` in both spatial modes
(`spatial-shader-clip-probe-gpu.log`). Both creation paths now declare one fragment
uniform buffer, matching the pushed clip data and shader interface. Header and
implementation comments describe that contract.

Final offscreen Vulkan checks pass 1,382 assertions in five cases
(`spatial-shader-clip-binding-gpu.log`). Screen and display-format spatial modes verify
distinct same-name output, ownership/lifetime, geometry reuse and actual inside/
outside clip pixels against a background frame. The sweep includes material,
lens, postprocess and the existing portal lens boundary. This checkpoint originally
called those spatial checks HDR. That was incorrect: `DefaultPbrDocument` writes
the transparent pass to display-format `display`. The follow-up below adds the
missing HDR execution proof.
Headless render passes 46,768 / 469 (`spatial-shader-host.log`) and Client passes
10,232 / 172 (`spatial-shader-client-host.log`). Client, Studio and both test
binaries build (`spatial-shader-clip-binding-build.log`). The owned editable-room
portal round trip passes 10,365 assertions in one product case
(`spatial-shader-client-gpu.log`); this is compatibility evidence, not product
custom-shader crossing proof. Transition and final captures remain in
`portal-client-walk-60-third-explicit-held-clear-image-frames-warm-owned/`; bulk
frames were removed. Formatting and diff checks pass.

Fixture corrections are retained in the evidence: the first attempt aborted on
late GUI component registration (`spatial-shader-repro-gpu.log`); registration now
precedes the screen section too. The initial clip edit used a nonexistent Rect
field (`spatial-shader-clip-build.log`); its accidentally executed older binary
is recorded as `spatial-shader-preclip-old-binary-gpu.log` and is not clip proof.
The corrected clip probe compiled before the reported binding failure and fix.

Foreign-world shader preparation, Studio migration and product custom-shader
crossing remain open. These checks do not establish graph-owned preparation,
minimal pipeline residency, delayed-GPU safety or release performance. No
sanitizer or interactive Studio run was added.

## Interface target formats and depth modes

The owner fixture now runs screen, depth-tested spatial and always-on-top spatial
sections through both `DefaultPbrDocument` and `DefaultWorldHdrDocument`. It asserts
the authored `display` resource is respectively RGBA8_SRGB or RGBA16F. The existing
pixel checks cover clipping, same-name owner isolation, accepted replacement,
refused replacement, retirement and zero geometry upload on owner changes in all
six combinations. A foreground wall additionally proves depth-tested UI is hidden
while always-on-top UI remains visible at the same sample.

Offscreen Vulkan passes 1,959 assertions in five cases
(`interface-format-depth-gpu.log`), including the material, lens, postprocess and
portal lens boundary checks. The test target builds
(`interface-format-depth-build.log`); formatting and diff checks pass. Only the
fixture and checkpoint documents changed, so the earlier headless and product
runs were not repeated. This closes the spatial target-format evidence gap, not
product custom-shader crossing or foreign shader preparation.

Preparation tracing confirms Client still refreshes shaders only for its main
presentation world. Sharing that work with retained/staged views and portal
producers must account for independently retained InterfacePass pipelines: a
consumer can miss another consumer's ephemeral `ShaderLibrary::Changed` list.
Source revision alone is insufficient across source replacement and store reuse.
The next checkpoint implements persistent accepted-program identity and interface reconciliation. Shared world preparation is still incomplete.

## Independent interface shader preparation

`ShaderModule::CodeHash` identifies accepted SPIR-V bytes, including built-ins.
It is computed when words are accepted, retained on a failed edit and cleared
when no accepted program remains. Source replacement, store reload and identical
revisions cannot make different accepted words compare equal by revision alone.
The existing host source-identity and reload cases now check this identity.

`InterfacePass::RefreshShaders` reconciles GUI demand against the shared library's
current accepted modules. Each pass retains its own accepted and attempted hashes.
Unchanged accepted programs reuse pipelines; failed device attempts retain the old
pipeline and are not retried for the same words. Demand removal or missing source
retires only that consumer's owner-scoped variants. Client GUI preparation uses
this path each frame, replacing its changed-list-driven interface upload block.
Material, lens and postprocess preparation still use the existing Client path.

The GPU fixture drives two independent interface consumers, clearing the library's
changed list before the delayed consumer prepares. Both receive the edited pixels.
Fresh consumer demand, steady reuse, failed source edits, demand removal, redemand
and source deletion are checked through screen, depth-tested and always-on-top
placements in display and HDR graphs. The final Vulkan sweep passes 2,631 / 5
(`interface-shared-preparation-final-gpu.log`). Headless render passes 46,802 / 469
and Client 10,232 / 172 (`interface-shared-preparation-host.log` and
`interface-shared-preparation-client-host.log`). Client, Studio and both test
binaries build (`interface-shared-preparation-checked-build.log`). The first build
caught a missing ContentHash include; no test was run against that failed build.

The owned editable-room product round trip passes 10,290 assertions
(`interface-shared-preparation-client-gpu.log`). This remains compatibility
evidence rather than a custom-shader crossing test. Transition and final frame
pairs are retained in the owned-roundtrip directory; bulk captures were removed.
Formatting and diff checks pass. Device-pipeline creation failure caching has not
received fault-injection coverage. No release profiling, sanitizer or interactive
Studio check was run.

Next: share material/lens/postprocess preparation with retained/staged worlds and
portal producers, using accepted identities rather than ephemeral change lists.
Their independently owned interface passes can now reconcile the shared modules,
but those foreign-world preparation calls are not wired yet. Graph ownership and
published shader delivery remain required by the full refactor plan.

## Idempotent device shader registration

Material variants, lens pipelines and postprocess selections now retain the hash
of their accepted words. Registering the same owner, name and words succeeds
without rebuilding pipelines, waiting for a submitted frame or advancing the
renderer resource revision. Different programs still take the existing atomic
replacement path. Hashes are published only with accepted pipelines and retire
with their owner. Lens pipeline storage now keeps the handle and hash together;
draw lookup, shutdown and owner retirement use that same entry.

The owner fixtures explicitly check unchanged resource revisions after repeated
material, lens and postprocess registration. Existing replacement, refusal,
strict owner lookup and retirement pixel checks remain in the sweep. Offscreen
Vulkan passes 2,639 assertions in five cases (`shader-device-identity-gpu.log`).
Headless render passes 46,802 / 469 and Client 10,232 / 172
(`shader-device-identity-host.log`, `shader-device-identity-client-host.log`).
Client, Studio and both test binaries build (`shader-device-identity-build.log`).
Formatting and diff checks pass. No release timing, sanitizer or interactive
Studio check was added; no speedup is claimed from these correctness checks.

The next implementation still needs to consolidate the Client's shader loop and
invoke preparation for retained/staged views and portal producers. Device
registration is now safe to repeat for accepted programs, but this checkpoint
does not itself add those callers or establish graph-owned preparation.

## Shared shader preparation for direct Client views

`PrepareWorldShaders` now owns the common CPU resolution and device preparation
sequence for the Client's main viewport, retained source and staged successor.
The Client's old shader loop and `LastShaderOwner` bookkeeping are removed.
Each direct world view prepares its own material/lens programs, optional grade
and InterfacePass before collection. The helper carries a `world shaders prepare`
profile scope and reports changed resources to Client damage tracking.

`Renderer::PrepareShaders` reconciles current demand against accepted program
hashes. It retires vanished material/lens demand, clears missing or disabled
grades and updates programs even if another library refresh consumed the changed
list. GUI-only and postprocess-only programs no longer get offered as material
shaders. Failed device attempts are cached by family, owner, name and code hash;
removing demand or retiring the owner clears those refusal entries. Missing-source
diagnostics still log on the source transition. Each interface consumer uses its
existing hash reconciliation. `SetContentOwner` now returns a damage signal so a
switch between warm owners still repaints the cached screen image.

The material, lens, grade and interface fixtures now drive this shared helper.
They check delayed refreshes, edits, failed source edits, demand/source removal,
strict owner isolation, zero material residency for GUI/grade-only demand and
postprocess disable/reenable. Warm interface-owner switches change damage without
changing the renderer resource revision or uploading geometry. Display and HDR
screen/spatial variants remain in the sweep.

Final offscreen Vulkan checks pass 3,135 assertions in five cases
(`world-shader-owner-damage-gpu.log`). Headless render passes 46,801 / 469 and
Client 10,232 / 172 (`world-shader-owner-damage-host.log` and
`world-shader-owner-damage-client-host.log`). Client, Studio and both test binaries
build (`world-shader-owner-damage-build.log`). The owned editable-room Client round
trip passes 10,940 assertions in one case
(`world-shader-owner-damage-client-gpu.log`). Transition and final frame pairs are
retained in the owned-roundtrip directory; bulk images were removed. This product
run checks compatibility with direct retained/staged preparation, not authored
custom-shader crossing. Formatting and diff checks pass.

The producer preparation gap described at this checkpoint is addressed below.
Studio migration, actual product custom-shader crossing, device-failure fault
injection, graph-owned preparation and release performance remain open.

## Shared portal-producer shaders and deferred opaque colour

PortalImageHost now shares one ShaderLibrary across producers, using the supplied
Client library when available. Standalone producers create a library on demand.
They run PrepareWorldShaders before world collection and capture signatures.
The Client postprocess setting reaches producers so they cannot reenable a disabled
grade for the same owner. Native producer residency defaults to the local world
name; custom fixture meshes/textures now carry the owners of their consuming worlds.
Host Clear preserves content bindings until producer removal has retired owned
compiler modules.

Complete-world capture signatures include demanded GUI names and accepted code
hashes. Every producer InterfacePass reconciles those hashes independently.
Successful spatial shader edits therefore invalidate renewal even when the
renderer resource revision is unchanged. Material edits, source removal, malformed
edits retaining accepted words, unchanged renewal and disabled postprocessing all
have direct producer and host-driven coverage.

This reproduced a separate deferred-rendering defect: authored opaque programs
compiled and replaced successfully but did not change portal pixels. The GBuffer
node now draws authored HDR colour and depth first into its existing emissive/depth
outputs. Cleared normal alpha marks those fragments as already shaded. Ordinary
geometry writes all GBuffer fields when nearer; discarded authored fragments leave
background depth available. The lighting resolve returns authored colour directly.
DrawSlots skips those authored runs only in its GBuffer family, retaining indirect
argument positions. No extra graph node or undeclared resource was added.

The new cutout fixture checks blue authored pixels between green background and
red foreground, with exact reference depths outside the authored region. Its first
oracle incorrectly required byte-identical ordinary colour after adding a wall;
readback showed changed lighting with correct material and depth. That failure is
retained in producer-cutout-diagnostic-gpu.log. The final oracle checks visible
material channels separately from depth. Custom fragment discard in shadow passes
is not implemented by this change.

Final offscreen Vulkan portal/owner checks pass 34,171 assertions in 32 cases
(producer-shaders-final-gpu.log). Headless render passes 46,801 / 469 and Client
10,232 / 172 (producer-shaders-final-host.log and
producer-shaders-final-client-host.log). Client, Studio and both test binaries build
(producer-shaders-final-build.log). All 35 staged shaders pass SPIR-V and translated
MSL resource checks (producer-opaque-shadercheck.log). C++ formatting and diff
checks pass. Build output retains the existing DrawList::JointFrames initializer
warning in PortalImageRuntime.cpp.

Actual product custom-shader crossing, Studio migration and graph-owned
preparation remain open. No release profiling,
sanitizer or interactive Studio run was added.

The owned editable-room Client round trip also passes 10,968 assertions in one
case (producer-shaders-final-client-gpu.log). This checks crossing compatibility
with shared producer preparation; the fixture does not author custom shaders.
Twelve transition/first/final frame pairs remain in
portal-client-walk-60-third-explicit-held-clear-image-frames-warm-owned; bulk
successful captures were removed.

Authored opaque occlusion coverage now passes 125 assertions in one case
(authored-occlusion-final-gpu.log; authored-occlusion-final-build.log). Large and
small authored/ordinary rows exercise both phases, including a half-screen discard.
Colour and depth match the ordinary GBuffer path on initial and repeated draws.
Removing authored programs changes both reference images, and the occlusion path
must issue more draws than the baseline, preventing a silent one-pass fallback
from satisfying parity. This is dev/offscreen Vulkan functional evidence, not a
performance measurement.

## Product material shaders across adoption

The real Client round-trip fixture now authors a ShaderScript named RoomShader in
both worlds, with red source-world code and blue destination-world code. White
floor materials select that same name. A missing shader therefore cannot pass the
room-colour oracle. Camera-ray checks cover native, retained and staged views,
including room pixels seen through portal apertures. The run requires samples of
both colours and of a retained-world view, plus the existing two-adoption camera,
input and landmark checks.

The first product run stopped in the expanded fixture on a null capture descriptor
before the first image arrived (product-shader-owner-gpu.log). The descriptor guard
was corrected. The next run reproduced white floors throughout source viewing and
failed the destination landmark (82 assertions; product-shader-owner-capture-gpu.log).
Those complete failed captures are retained separately in
product-shader-owner-null-capture-evidence and product-shader-owner-white-floor-evidence.

A headless replica test then reproduced the cause: collect-replicated read an empty
SurfaceAppearance shader because BuildReplicatedWorld never scheduled material
resolution. Replica presentation now runs ResolveMaterials before collection, as
other presented worlds already do. The new test checks selection and clearing in
an adopt-only store. The original failing headless evidence is
replica-material-reproduce.log.

After the fix, the dev/offscreen Vulkan product run passes 11,292 assertions in one
60 Hz case (replica-material-final-gpu.log). Full headless Client passes 10,237 / 173
(replica-material-final-host.log). Client, Studio and test_client build; the existing
NativePlayerPrediction::Sample initializer warning remains in the log. Formatting
and whitespace checks pass. Thirteen first/final/transition/sample frame pairs are
retained in portal-client-walk-60-third-explicit-held-clear-image-frames-warm-shaders.
Frames 190 and 599 were inspected: the former has the blue room beyond the aperture
and red foreground, while the latter has red returned-room ground and a blue portal.
PNG copies exist only to make those BMP readbacks viewable by the inspection tool.

This closes the reproduced material-shader selection gap in replicated views and
provides actual material-shader crossing evidence. Product spatial GUI/lens/grade
shader crossing, shader edits during transfer, Studio migration and release
profiling remain open. No sanitizer or interactive Studio run was added.

## Reproduced spatial GUI omission before adoption

The Client crossing fixture now selects a typed RoomShader mode: none, material or
spatial GUI. The spatial case places a depth-tested SurfaceGui/ImageLabel across
the white floor, using the same RoomShader name with red/blue code in the two
worlds. It retains the material case's camera-ray colour and two-adoption checks.
The landmark threshold is now a nonfatal CHECK rather than REQUIRE, retaining the
same pass condition while allowing the rest of the sequence to report failures.

The first combined run passes the material case but fails the spatial case at
outbound adoption (product-spatial-shader-gpu.log: 12,981 assertions, one failure
across two cases). Frame 148 has only 16 blue landmark pixels; frame 149 has 4,702.
Both images were inspected and PNG viewing copies retained in
product-spatial-shader-pre-adoption-evidence. The full second run confirms exactly
one failure across 11,285 assertions (product-spatial-shader-full-oracle-gpu.log).
All later room-colour, retained-view, camera and return-adoption checks execute.
Both build logs pass; C++ formatting and diff checks pass. No runtime fix is claimed.

The metadata identifies the cut: before adoption, the portal uses an
opaque-lighting ordered-layer capture from walk.destination. After adoption it
uses a complete-world capture from client.portal.1. The producer deliberately
creates/submits its InterfacePass only for CompleteWorld; the ordered-layer graph
stops at opaque lighting and peels ordinary transparent geometry. Therefore the
remote room omits spatial GUI until its admitted local replica can render it.
This is broader than shader compilation or owner lookup.

The next implementation must carry spatial presentation through remote body
composition while preserving depth, transparency and current-body ordering.
Changing the request to CompleteWorld alone would bypass the layer contract that
keeps the local body current. Adding GUI colour to OpaqueLighting alone would mix
its radiance/depth contract with arbitrary spatial overlays. Preserve this failing
product check while extending the remote presentation inputs and graph stages.
The same boundary still needs the planned authored lens inputs after body
composition. The full failed spatial sequences remain in their evidence directory
and portal-client-walk-60-third-explicit-held-clear-image-frames-warm-spatial-shaders.
The latest passing material sequence replaced that fixture's prior successful
capture directory; its transition/first/final/sample pairs are retained.

## Depth-tested spatial GUI in ordered portal layers

Ordered-layer producers now prepare and submit their spatial InterfacePass.
The opaque radiance image keeps its existing scope. Depth-tested spatial batches
join the peeled transparent layers, so the receiver still composites the current
body against the same ordered depth domains. No wire version or image-count change
is needed for these batches.

FrameOverlayHook exposes prepared batch capture through WorldInterfaceCapture.
InterfacePass reuses its submitted mesh, clipping, owner-scoped authored shaders
and requested-camera transforms, with an additional HDR pipeline that writes
fragment depth. Each batch first draws to graph-declared RGBA16F/D32F scratch
attachments. The interface-layer shader then selects fragments against opaque and
previous-layer depth, followed by a colour replay at the selected depth. Scratch
colour is already premultiplied, so this replay uses a matching blend state.
Discarded and zero-alpha fragments leave deeper layers available. All shader
variants include the new capture pipeline in atomic installation and retirement.

Scratch targets are ordinary graph resources, reused between batches. The node
counts actual render passes and interface batches. This adds per-batch GPU passes;
no release performance claim is made. Ordered captures include accepted GUI words
in their resource signature. Always-on-top collectors are explicitly unsupported
by this depth-ordered path and cause capture failure instead of silent omission.
Their separate ordering contract, along with lens inputs after body composition,
remains required work.

The initial product check passes 11,200 assertions (spatial-layer-product-gpu.log).
The render sweep passes 80,383 / 30 (spatial-layer-verified-gpu.log), including portal
runtime/host, interface owner and authored opaque occlusion cases. The expanded
ordered-layer fixture passes 94,420 assertions (spatial-layer-mixed-gpu.log): ordinary
glass, spatial GUI, discard, zero alpha, and mixed glass/GUI in both depth orders.
It retains half-alpha, exact layer distance, opaque occlusion, visible-overflow and
recovery checks. An intermediate test-oracle error expected a discarded front layer
to reveal a third pane behind the opaque wall; that 816-assertion failure remains in
spatial-layer-discard-gpu.log and was corrected by applying the same opaque bound
to expected layer indices.

Headless render passes 46,802 / 469 and Client 10,237 / 173
(spatial-layer-verified-host.log and spatial-layer-verified-client-host.log).
Graph tests pass 8,993 / 225 (spatial-layer-graph.log). All 36 staged shaders pass
SPIR-V/MSL binding validation (spatial-layer-shadercheck.log). Client, Studio and
both test binaries build; the final layer-fixture build is
spatial-layer-mixed-build.log. The existing DrawList::JointFrames initializer warning
remains. No sanitizer or interactive Studio run was added.

Final spatial product verification passes 11,192 assertions
(spatial-layer-verified-product-gpu.log), retaining the original landmark threshold.
Inspected frames 145 and 146 show the blue floor on both sides of outbound
adoption. The successful spatial capture directory retains 12 BMP/JSON pairs:
first/final frames and both sides of each ownership or view transition, plus PNG
copies of those two inspected frames. Full failed sequences remain preserved in
their evidence directories. The final shader formatting rebuild stages SPIR-V and
MSL successfully (spatial-layer-shader-format-build.log); all 36 modules pass again
(spatial-layer-final-shadercheck.log). This last shader change is whitespace only.

## Shared world-view batch routing

WorldViewInterface now forwards ordered-layer capability, prepared batch count and
batch recording to its spatial hook. Previously it inherited the empty batch API:
the same InterfacePass that worked directly in a producer lost its spatial batches
when wrapped for foreign-world presentation. Screen UI remains routed separately.
Failed or not-yet-run spatial preparation exposes no batches and records nothing;
shared spatial/screen hooks still prepare only once.

The expanded existing world-view ownership test reproduced ten failed assertions
(world-layer-routing-before.log). After the fix, headless render passes 46,820 / 469
and Client passes 10,237 / 173 (world-layer-routing-render.log and
world-layer-routing-client.log). Client, Studio and both test programs build
(world-layer-routing-build.log); formatting and diff checks pass. No new GPU,
interactive Studio, sanitizer or release profiling run was made for this routing
change. Always-on-top capture and the post-body lens chain remain unfinished.

## Spatial overlay wire member

PortalImageLayerSet now has an optional SpatialOverlay member for premultiplied
world-space always-on-top GUI. It carries no depth: it belongs after current-body
composition, rather than inside the physical transparent depth order. It must
share the opaque image's request, extent, capture tick, content revision, lighting
revision and lighting snapshot. Screen UI is outside this payload.

PIMG is now version 13. A canonical presence byte follows the transparent count;
the optional overlay is the last length-prefixed image. All member prefixes are
admitted before decompression. Expanded pixel limits include the overlay, and
ImageCount lets inbox admission charge its pixels and copied metadata before
transactional decode. Held-byte accounting, replacement, expiry and TakeLayers
move or release the complete group. Zero alpha requires zero colour, and alpha
outside [0,1] is refused. Invalid groups preserve the caller's output.

This is transport construction, not completed rendering support. Producers still
refuse always-on-top ordered captures. QueuePortalImageLayerSet explicitly refuses
groups containing SpatialOverlay until GPU import and composition are wired;
accepting it there now would silently discard the member. Next work must capture
the top-only interface into a graph-owned HDR target, charge that extra capture,
import it atomically with the paired layers, and blend it after current-body and
transparent composition before the authored lens chain. Preserve it through
source replacement and endpoint retirement without inventing a depth value.

Wire/inbox focused checks pass 10,654 / 13 (spatial-overlay-wire-focused.log).
After strengthening the pixel-budget oracle to enlarge all members together,
headless render passes 52,631 / 469 (spatial-overlay-wire-render.log). Client passes
10,237 / 173 (spatial-overlay-wire-client.log). Existing producer-layer and source
upload GPU cases pass 94,541 / 2 (spatial-overlay-wire-gpu.log), exercising version
13 with the overlay absent. Overlay coverage includes round trips, truncation,
byte mutations, individually valid members with mismatched capture metadata,
malformed alpha/depth, compressed expansion and inbox replacement pressure.
It does not yet prove overlay pixels on a GPU.

Client, Studio and both test programs build (spatial-overlay-wire-final-build.log);
the final test-only rebuild is spatial-overlay-wire-budget-build.log. Formatting
and diff checks pass. Existing initializer and range-loop-copy warnings remain.
No interactive Studio, sanitizer or release profiling run was added.

## Atomic spatial overlay GPU import

QueuePortalImageLayerSet now accepts the optional overlay as the last returned
handle. Its local binding role is three, independently of the number of physical
transparent members. Standalone QueuePortalImage still refuses that role: overlay
ownership is admitted with the complete capture group. No depth texture is
allocated for it. Cache admission and reuse distinguish paired images from
colour-only overlays, preserving exact logical residency and pending-byte totals.
Allocation still finishes before any input bytes or output handles move.

The existing copied-layer/body GPU fixture now uploads overlay groups in eye and
seam cases. It checks readiness after upload, four-member residency at 44 bytes
per pixel, whole-group retirement through the overlay handle, malformed capture
refusal and a second cycle with no new texture allocations. The case passes
48,768 assertions (spatial-overlay-import-gpu.log). This proves import and lifetime,
not overlay blending: its rendered body reference still uses the original group.

Headless render passes 52,630 / 469 (spatial-overlay-import-render.log), and Client
passes 10,237 / 173 (spatial-overlay-import-client.log). The render test build and
Client/Studio builds pass (spatial-overlay-import-build.log and
spatial-overlay-import-products-build.log). Formatting and diff checks pass.
Existing initializer warnings remain; no sanitizer or release profiling was run.

The producer still refuses always-on-top ordered captures. Source publication
still expects three handles and must be extended before it can admit an overlay
group. Next connect its handle through retained capture metadata and View, add
the graph-owned top-only capture and post-body blend, and charge the extra capture
against the request budget. The authored lens chain remains later work.

## Spatial overlay source publication

PortalImageSource now reserves four upload handles and passes only the actual
member count to atomic import. It publishes SpatialOverlayImage in
PortalImageCapture after the complete group becomes ready. Pending replacements
retain the old overlay together with its camera and paired images. Publication
without an overlay, flat-image replacement, expiry and endpoint retirement clear
the handle; request cancellation retires all pending members without changing the
currently displayed capture.

The source lifecycle GPU fixture alternates overlay-bearing and ordinary groups
in both directions. It checks pre-publication invisibility, retained metadata,
seven simultaneous images during replacement, cancellation before/after GPU
submission, flattening and endpoint withdrawal. The focused run passes 254
assertions (spatial-overlay-source-gpu.log). The final combined source/body run
passes 49,022 / 2 (spatial-overlay-source-final-gpu.log).

Headless render passes 52,630 / 469 and Client passes 10,237 / 173
(spatial-overlay-source-render.log and spatial-overlay-source-client.log).
Client, Studio and both test programs build (spatial-overlay-source-final-build.log).
New test-count signedness warnings from the first build were corrected; existing
initializer warnings remain. Formatting and diff checks pass. No sanitizer,
interactive Studio or release profiling run was added.

This closes source publication only. The current-body graph still accepts only
the three paired images and refuses an overlay-bearing group. Next carry the
handle into View, import and blend it through graph-declared resources after
body/transparent composition, then add top-only producer capture with an explicit
pixel charge. The real always-on-top product crossing remains unverified.

## Graph composition of imported spatial overlays

View now carries EyeSpatialOverlayImage and includes it in presentation damage.
The eye-image node accepts a spatial-overlay selection with a colour-only output,
checks its capture group and owner against the base image, and refuses missing or
foreign overlay handles. DefaultPortalBodyDocument can append this import and a
colour-compose node after body and ordered transparent composition. Its export
keeps reading the existing opaque depth resource unchanged.

colour-compose blends premultiplied source-over HDR colour with two samplers and
no depth input or attachment. It uses graph-owned output storage and reports its
draw and output bytes. ComposePortalBodyImage validates all four handles when an
overlay is present and selects a separate cached graph variant for that case.
This does not assign synthetic physical depth to always-on-top GUI.

The expanded copied-layer/body fixture covers both eye and seam projections,
four body distances and rotated cameras. It compares every blended pixel against
the unoverlaid body/room result and requires byte-identical exported depth. It
also checks foreign/missing overlay refusal and retains import/retirement/cache
checks. Combined source/body GPU tests pass 68,236 / 2
(spatial-overlay-compose-matrix-gpu.log). The initial narrower run passed 51,112
assertions (spatial-overlay-compose-gpu.log).

Graph tests pass 9,088 / 225 (spatial-overlay-compose-graph.log). Headless render
passes 52,636 / 469 and Client 10,237 / 173 (spatial-overlay-compose-render.log and
spatial-overlay-compose-client.log). All 37 staged shaders pass SPIR-V/MSL checks
(spatial-overlay-compose-shadercheck.log). Client, Studio and both test programs
build (spatial-overlay-compose-final-build.log); the final expanded test build is
spatial-overlay-compose-matrix-build.log. The first build failed on a new fixture
variable-name collision, corrected before these runs; its output remains in
spatial-overlay-compose-build.log. Formatting and diff checks pass. Existing
initializer warnings remain. No release profiling, sanitizer or interactive
Studio run was added.

Producer capture remains unfinished: ordered producers still refuse always-on-top
collectors. Next record top-only spatial UI to a graph-owned HDR image, include it
in the atomic capture result and charge its extra pixels before submission. Then
verify real authored GUI across the product crossing. Authored lens inputs and
their post-body chain remain separate required work.

## Producer capture of always-on-top spatial GUI

Ordered producers now record top-only spatial GUI through a graph-owned HDR
colour target. The depth attachment exists for pipeline compatibility and never
becomes overlay payload. Depth-tested GUI remains in the two physical layers;
player screen UI stays outside this capture. A visible top collector requests
a fifth export alongside opaque, two physical layers and the overflow probe.
The producer admits its pixel cost before queueing, then collects all exports
from the same submitted frame before publishing the optional overlay member.
Automatic body demands reserve this possible fifth image when fitting extent.

The renderer export pool and submission index list share a five-slot capacity.
Adoption planning derives that same bound. The first GPU attempt reproduced
missing overlay completion: submission tracking still had four entries. The
parallel review also found a four-entry resident adoption scratch array. Both
were corrected. The resident fixture now exercises five-member adoption and
waits for GPU completion before testing subsequent slot reuse.

Producer and resource-image GPU tests pass 714,927 assertions / 14 cases
in spatial-overlay-producer-validated-gpu.log. This includes authored top-GUI
premultiplied HDR pixels, empty physical layers, removal/recovery, explicit
overlay budget refusal, copied export completion and five-image resident
adoption. The command excludes eye-current-camera. The preceding broad run in
spatial-overlay-producer-capacity-gpu.log reproduces its three existing failures:
986 translated, 786 rotated and 424 combined-motion mismatched pixels. It also
contains the resident fixture's pre-wait failure. No current-camera fix is claimed.

Headless render passes 52,641 / 469 and Client 10,237 / 173. Graph passes
9,091 / 225; all 37 staged shaders pass SPIR-V/MSL validation. Evidence is in
spatial-overlay-producer-capacity-headless.log,
spatial-overlay-producer-headless-client.log,
spatial-overlay-producer-graph-final.log and
spatial-overlay-producer-shadercheck.log. Client, Studio and test programs build
in spatial-overlay-producer-capacity-build.log; the final resident fixture build
is spatial-overlay-producer-resident-test-build.log. Earlier graph-order,
inputless-source declaration and compile failures remain in their producer logs.

The always-on-top product crossing still needs its own oracle. Existing product
captures use ordered layers before destination replica admission, then switch to
complete-world captures. The new oracle must prove the optional overlay was
present in that earlier ordered group. Authored lens inputs, their post-body
chain, current-camera coverage, release profiling, sanitizers and interactive
Studio verification remain open.

## Product always-on-top GUI crossing

The new portal-product-spatial-overlay fixture keeps the existing authored
red/blue floor and both landmark gates. A small green always-on-top SurfaceGui
belongs only to walk.destination. At outbound adoption, the fixture requires
the earlier portal capture to come from that remote producer with opaque-lighting
scope and a nonzero spatial_overlay_image. It projects three interior marker
positions through each accepted capture camera and checks green pixels on both
sides of adoption. Client capture metadata now includes the overlay handle.

The product test passes 11,362 assertions in product-spatial-overlay-gpu.log.
The first outbound transition is frames 149/150, retained in
product-spatial-overlay-first-pass-evidence.
Frame 149 has overlay handle 107; frame 150 uses the complete-world local capture.
Both BMPs were converted to PNG and inspected: the green marker is visible above
the player and the blue destination floor persists. All 600 BMP/JSON pairs remain
available. The path is distinct from prior spatial-shader and baseline evidence.

Additional producer discard and zero-alpha top-GUI cases pass 126,740 assertions
in spatial-overlay-producer-transparent-gpu.log. This verifies canonical empty
overlay pixels alongside removal/recovery and the existing ordered-layer cases.

The subsequent graph-owned lens prerequisite is still under validation. The
first build reproduced leftover LensA/LensB lookups, then built after removal.
An unrelated concurrent Studio edit failed on candidate.Index; it was not
changed as part of this work. Client/render targets build in
lens-graph-product-overlay-final-build.log. Headless render passes 52,642 / 469
in lens-graph-headless-render.log. The initial lens graph test run has two
failures: expected resource count and missing internal-scratch intent in graph
diagnostics. These are not closed by the passing product test.

## Graph-owned lens inputs and intermediates

shader-lenses now resolves named graph colour and R32F depth inputs, plus
distinct HDR final and scratch outputs. Private LensA/LensB allocations and
lookups were removed. The default graph declares the scratch resource, repeated
lens nodes can own separate intermediates, and output extents follow the graph
resource dimensions. Empty and odd-length chains publish the declared final
image. Invalid port offsets and aliased input/output textures are refused.

PortSpec now marks internally consumed outputs explicitly. Lens scratch uses
that marker, so diagnostics keep its memory/lifetime visible without calling
it an unused output. A node whose final output is unused still reports dead.
The diagnostics test covers reordered named ports and reused scratch; the
profile test includes the added resource. Graph tests pass 9,105 / 226 in
lens-graph-contract-tests.log. The initial contract compile missed propagation
through the catalogue's local port descriptor; corrected output is in
lens-graph-contract-final-build.log.

The expanded lens GPU fixture first reproduced a separate cache bug: replacing
a named pipeline released its graph textures, but unchanged scene damage then
selected no-op scene passes. The new graph had no output texture. Each installed
pipeline now receives a monotonic revision, and that selected revision participates
in the viewport content signature. Replacement, switching and reinstallation can
therefore invalidate retained scene work without invalidating unrelated pipelines.
The fixture remains unchanged after reproducing this failure in
lens-graph-verified-gpu.log.

The corrected combined GPU run passes 195,727 assertions / 5 cases in
lens-graph-invalidation-gpu.log. It includes zero through three noncommutative
authored lenses, repeated graph nodes, a distinct remapped depth resource, and
full/half-sized lens outputs. Chained transforms compare against a fused shader
with one final 8-bit level allowed for intermediate half-precision rounding;
depth remapping compares exact pixels and keeps both depth textures live to
prevent aliasing from masking the wrong sampler. Producer layers, source/body
composition and opaque-versus-lensed capture also pass in that run.

Client, Studio and test programs build in lens-graph-invalidation-build.log.
The unrelated concurrent Studio compile error from the first attempt no longer
appears in the final build. No sanitizer, release profiling or interactive Studio
run was added. This is the graph prerequisite only: bounded authored lens inputs,
accepted program identities and capture time still need transport and post-body
application. The current-camera image failures remain separate and open.

Final headless render passes 52,641 / 469 and Client 10,237 / 173 in
lens-graph-final-render.log and lens-graph-final-client.log. The product rerun
against the final renderer passes 11,285 assertions in
product-spatial-overlay-final-gpu.log. Its outbound transition is frames 148/149
in portal-client-walk-60-third-explicit-held-clear-image-frames-warm-spatial-overlay,
with remote overlay handle 90 before adoption and complete-world capture afterward.
Both PNGs were inspected and retain the green marker and blue floor. Formatting
and diff checks pass. All product captures and earlier failure logs are retained.

## Captured authored lens chain, integration in progress

PIMG v14 adds a bounded ordered lens chain to PortalImageLayerSet. Each entry
carries a copied rigid pose, shader name, accepted program hash, radius, inner
radius, falloff, strength, spin, priority and sphere shape. Local entity IDs stay
out of the wire. The chain carries capture time; an empty chain has canonical
zero time. Borrowed metadata validation and record/name byte admission precede
image decompression. Codec/inbox checks pass 31,523 / 27 in
captured-lenses-codec-inbox.log.

Ordered producers collect accepted program identities under their content owner
and refuse missing programs or invalid parameters. Source publication moves the
chain with its atomic image group; pending replacements keep the previous chain,
and cancellation, flattening and expiry preserve or clear it with that image.
Body composition resolves the authenticated producer through the view's foreign
content bindings, checks exact accepted shader hashes and uses a lens-only owner
and time override. Geometry keeps its own content owner. Those overrides also
participate in retained viewport invalidation.

The body graph can run captured lenses after body/transparent/overlay composition
using explicit composed depth and separate HDR final/scratch resources. Graph
tests pass 9,325 / 226 in captured-lenses-graph.log. Headless render passes
69,213 / 469 and Client 10,237 / 173 in captured-lenses-headless-render.log and
captured-lenses-headless-client.log. Client, Studio and tests build in
captured-lenses-final-build.log after fixing the missing hash-type include from
captured-lenses-build.log.

The first combined GPU run in captured-lenses-gpu.log passes three cases and
fails the body-chain oracle at 2,312 pixels. The observed 0.0104857 error matches
three intermediate half-float stores rounded toward zero, while the initial
oracle used unrounded arithmetic. The revised fixture uses binary fractions
and explicit intermediate quantization without widening its tolerance. The rerun
passes 87,709 assertions / 3 cases in captured-lenses-quantized-gpu.log.
It still checks distinct A/B/A shader runs, captured time, composed
depth, unchanged alpha/depth and missing owner/hash refusal.

Product review also found divergent native order: DefaultPbrDocument applied
lenses before transparent geometry/top GUI, while DefaultWorldHdrDocument applied
them afterward. DefaultPbrDocument now authors the common HDR chain, and
DefaultWorldHdrDocument reuses it. Surface capture, portal/mirror composition,
transparent geometry and spatial GUI precede lenses, then display tonemapping.
Catalogue formats and graph-order expectations match that chain. Graph tests
pass 9,305 / 226; headless render passes 69,207 / 469 and Client 10,237 / 173
in captured-lens-unified-graph.log and captured-lens-unified-headless-*.log.

The first source/host GPU sweep exposed stale four-byte readbacks of portaled,
which is now eight-byte HDR. Display oracles now capture tonemapped and keep
independent no-portal baseline renders where required. The corrected sweep
passes 219,600 assertions / 15 cases in captured-lens-unified-gpu.log. Client,
Studio and tests build in captured-lens-integration-build.log.

The real moving-lens product fixture fails 19 checks in
product-captured-lens-first-gpu.log: captured images and accepted lens hashes
arrive, but the displayed portal image remains zero and neither adoption occurs.
All 600 frames are retained in product-captured-lens-first-failure-evidence.
This reproduces a circular dependency: the initial portal needs the destination
program before walking, while successor replica creation requires a crossing
offer. Preparing shaders only on that successor cannot resolve initial readiness.

PIMG v15 now carries a deduplicated accepted SPIR-V dictionary, bounded to 16
programs and 1 MiB of code, with hash and byte admission before decompression.
Renderer-local program leases validate SPIR-V and the fixed lens ABI before
device creation; immutable name/hash groups share references in 256 reusable
slots. Captured pipelines have storage separate from authored world owners.
A real owner named portal.lens.programs/0 reproduced a collision in the initial
shared-map design; both creation paths now share only the device-creation helper.
Source upload, publication, cancellation and retirement move or release
those references with the image group. Producer words come from the accepted
shader library module and must match the installed program hash. The first build
found a missing registry declaration, now added. Client, Studio and test targets
build in captured-lens-verified-build.log.

The same-camera native/captured-body oracle uses actual room radiance and two
peeled glass layers at three body depths, with front and rotated cameras. Its
integer A/B/A shifts and quarter-turn also match an independent byte-exact
coordinate oracle. Imported/native HDR retains the existing .004 glass-store
bound and .003 depth bound, over every pixel. Malformed SPIR-V, wrong shader
stage, wrong sampler set/binding and mismatched token identities are refused;
authored-owner deletion and final captured-program release cannot delete each
other's pipelines. These checks pass in captured-lens-final-gpu.log. That broad
run passes 32 / 33 cases and 674,463 / 674,466 assertions. The only failures are
the previously known current-camera translation/rotation/combined checks, with
unchanged mismatch counts 986 / 786 / 424. Headless render passes 86,179 / 469,
Client 10,237 / 173 and graph 9,305 / 226 in captured-lens-final-headless-*.log
and captured-lens-final-graph.log. Shadercheck passes all 37 staged modules.

The program-carrying product run now completes both handoffs. Its initial
pixel oracle forgot that the source world's lens also moves the portal image:
equal opposite shifts cancelled. Unequal destination/source shifts now require
a visible net shift and an empty vacated edge. Those pixel and captured-hash
checks pass across frames 157/158, inspected as PNGs, in
product-captured-lens-combined-shift-gpu.log. That run still fails one of 11,234
assertions: after returning, the predicted body settles but the script-visible
Humanoid.MoveDirection remains nonzero. Camera distance is correct and key
release occurs; the missing rest-state sample is under investigation. Earlier
cancelled-shift frames are preserved in product-captured-lens-cancelled-shift-evidence.
This checkpoint does not close authored lens crossing or current-camera parity.

The movement probe reproduced a separate stale replicated Humanoid.MoveDirection:
frame 599 has a stopped predicted direction but a nonzero ECS direction on the
returned humanoid. Frames are retained in product-captured-lens-stale-direction-evidence.
No movement behavior was changed. Optional portal-input and portal-input-stop
traces now identify the authoritative humanoid and applied direction. Traced
runs pass, including product-captured-lens-stop-repeat-gpu.log with 12,107
assertions and both final directions zero, but no traced failure yet establishes
where the stale value originates. The intermittent failure remains open.

The lens fixture's vacated-edge sample now uses world X=1.2; the former X=1.6
sample could overlap the shifted marker as its projection grew. All three
positive shifted-marker samples remain. Passing frames and failed movement
frames are retained separately. These fixes do not weaken the rest-state gate.

### Current-camera packet binding

BindWorldView now borrows the retained world's full geometry and camera layers
while preserving the caller's camera, projection, target, slot and body selection.
It validates the live owner name and store identity before mutation, then clears
imported eye and captured-lens state. Client::PreparePortalWorldView uses this
same helper. Caller-owned damage is computed after binding in the client.

The current-camera fixture explicitly checks that image-only output stays at
its capture camera. Its new full-packet branch renders the normal graph with
the retained hidden post, retaining all four original motions and the .002
pixel bound. current-camera-binding-gpu.log passes 155 assertions; the existing
WorldView suite passes 172 assertions across four cases in
current-camera-binding-headless.log. The first build rejected a test CFrame
comparison without an equality operator; component comparisons fix that error.
This establishes packet rendering, not disocclusion recovery from pixels alone.

Client scene eligibility now requires a live, admitted, fully joined authenticated
successor with the matching destination and no refusal/failure. Joined follows
complete authoritative snapshot application. Player prediction still requires
Ready and DrawingArrivedPlayer. Existing portal-image readiness refusal remains.
No existing content-session flag guarantees that every asset loaded, so this
change makes no such guarantee. Product routing now passes the scene, pixel and handoff checks in
current-camera-scene-gates-product-gpu.log. That run passes 11,547 of 11,548
assertions; the one remaining failure is the unchanged post-return rest-state
gate. The earlier routing run had 52 assertions for the old player-arrival
requirement; these now check admitted/joined/live/matching scene ownership and
absence of rejection or failure.


The traced routing failure proves the authority applied zero to the same returned
humanoid at world tick 297/input 299. Client frame 304 had replicated through
300 but retained the nonzero component, still stale at replicated tick 598 in
frame 599. Native motion acknowledgements also froze at pose tick 324/input 326;
this is recorded separately rather than assumed to be the same failure. Full
frames and log are in current-camera-routing-first-evidence. Replication row
tracing is the next diagnostic step; no movement behavior fix is claimed.

A separate headless reproduction found eight missing presentation invalidations
for content-owner bindings and effective lens inputs. The signature now includes
those bindings on active layers and active lens parameters/program/time. An
inactive lens clock does not dirty an otherwise unchanged scene. The targeted
presentation/WorldView run passes 641 assertions across 49 cases in
current-camera-signature-headless.log.

Final checks for the packet-binding/signature slice pass: render headless
86,260 assertions / 471 cases, client headless 10,237 / 173, and the focused
current-camera/lens/layer GPU group 83,786 / 4. Logs are
current-camera-final-headless-render.log, current-camera-final-headless-client.log,
and current-camera-final-gpu.log. No release profiling, sanitizers or interactive
Studio run was performed for this slice.

Opt-in replication-row tracing now records humanoid component signature changes,
packing, acknowledgement retirement and application hashes without a scene-layer
dependency or behavior changes. Build passes in replicated-stop-row-trace-build.log;
replication tests pass 22,409 assertions / 271 cases. The traced product run still
fails the single rest-state assertion in replicated-stop-row-trace-product-gpu.log.
Its application traces include changing server snapshot-store identities; these
must not be mistaken for proof that the client applied the stop. Correlating the
actual client recipient remains in progress.

Client-scoped replication tracing reproduced the loss in
replicated-stop-client-trace-product-gpu.log (12,444 / 12,445 assertions pass).
Native peer ...0880 sends/applies/acknowledges humanoid tick 258, then has no
candidate for the authoritative zero at tick 270. The other two peers receive
that change. This places the loss before native-client packing, rather than in
component decoding or a later client overwrite. The strongest current hypothesis
is snapshot streaming: its branch skips ordinary change collection while the
global changed list is replaced at the next publication. A deterministic test
and explicit streaming trace are being added before a behavior fix.

The deterministic snapshot test reproduced loss both during initial world
streaming and an oversized component slice: the acknowledged small value stayed
1 after a one-shot change to 0. The first fix retains changes in existing
Unconfirmed rows while streaming, clearing superseded SentAt acknowledgements.
Both reproduction sections pass, and the product run passes 11,600 assertions
in snapshot-changes-fixed-product-gpu.log. Broader replication testing found two
regressions from also retaining values already included in a newly captured
initial snapshot: an unnecessary post-join message and a delayed audit. These
are being fixed rather than weakening the existing checks. Both change-detection
modes and a real delayed acknowledgement now extend the regression fixture.

### Snapshot-stream change retention fix

Both deterministic sections reproduced the missing update before the fix.
Authority now preserves known, sendable, unsuppressed changes in its existing
Unconfirmed rows while a snapshot is already streaming, or before starting an
oversized slice. A superseded SentAt is cleared so an older acknowledgement
cannot retire the newer value. Fresh full snapshots already include their
capture-tick changes and do not redundantly queue them. Recovery sends the
latest component value when ordinary publication resumes.

The expanded fixture covers observed and signature detection across initial
snapshot, oversized slice and a real delayed acknowledgement. Its setup fixes
kept the serialized string payload observed while varying detection for the
small state component. All 22,503 assertions / 272 replication cases pass in
snapshot-changes-final-replication.log, including the existing no-extra-send and
audit checks. Client headless passes 10,237 / 173 in snapshot-changes-final-client.log.
Client/server and Studio builds pass; snapshot-changes-final-build.log records
the final Studio and replication-test build.

The refined traced product run passes 11,660 assertions. With replication tracing
disabled, the final product run passes 12,379 assertions in
snapshot-changes-final-product-gpu.log, including both handoffs, lens/scene pixels
and the post-return rest-state gate. This closes the reproduced lost-update bug.
It does not claim image-only disocclusion, nested scaled-aperture composition or
the full render-refactor acceptance matrix. Release profiling, sanitizers and
interactive Studio verification were not performed for this change.

### Nested ordered capture, implementation started

The top-GUI/authored-lens checklist item is complete within its own scope; nested
scaled-aperture composition remains the distinct acceptance case required by
RENDER-REFACTOR.md. The new production-path GPU section requests ordered opaque
layers through one child portal. It fails before dispatch because ValidRequest
rejects OrderedLayers with nonzero recursion. The request has a valid binding and
an eight-capture pixel budget. nested-ordered-capability-red-gpu.log records the
32 / 33 assertions before rejection. This is a capability reproduction, not a
native pixel/depth comparison.

The next representation is a bounded flat capture tree: each node retains its
own ordered images, accepted camera, lighting, lens programs and producer
identity; each parent-before-child edge carries the authored aperture geometry
and parent-to-child transform. Child linear depth stays in child units. Leaves
compose their mapped current body first; parents sample completed child colour
at the parent aperture depth before their physical layers and lenses. The entire
tree must be admitted, uploaded, replaced and retired atomically under aggregate
node, geometry, pixel and program budgets.

The later independent depth oracle uses outer scale 2 and inner scale .25, a
parent mouth at depth 8, a parent blocker at depth 6 and a child body at depth 4
(equivalent parent distance 16 for that mapping). Raw child depth must never beat
the parent blocker. Native unfolded HDR comparison, per-node depth probes and a
second body pose without room re-upload remain required. Borrowed geometry
preflight and the tree codec are being built first; runtime support is not yet
claimed.

The standalone capture-tree codec now builds and passes its tests. PIMG kind 6
(version 15) carries at most 16 parent-before-child nodes and depth 4. It retains
per-node camera, endpoint claims, required lighting and layer sets, plus encoded
aperture geometry and canonical scaled transforms. Borrowed preflight validates
aggregate pixels, geometry, program bytes and metadata before image decoding.
Program hashes share the distinct-program limit across nodes; physically repeated
program words still consume the byte budget. Sibling aperture keys must be unique.
Endpoint authentication remains the presentation adapter's responsibility.

MeasurePortalGeometry and DecodePortalGeometry share parsing/validation, with
transactional output and mutation/truncation parity checks. Tree tests cover
independent cameras, two physical layers, top GUI, lens dictionaries, malformed
topology, depth boundaries and whole-tree budgets. An early test command used the
previous binary and did not match the new tree suite; it is not tree evidence.
After final linking, portal-capture-tree-headless.log passes 55,687 assertions /
39 cases. Full headless render passes 92,515 / 475 in
portal-capture-tree-full-headless.log; existing layered GPU cases pass 83,550 / 2
in portal-capture-tree-existing-gpu.log. Final codec build and formatting checks
pass. This codec checkpoint did not implement recursive producer collection,
GPU tree import, composition or the native scaled-depth oracle.

### Capture-tree admission and ownership

The inbox now accepts ordered recursion through depth 4. Borrowed preflight
matches the root request key, exact camera, dimensions and authenticated endpoint,
then requires the source adapter to approve every child against current Universe
routes before image decoding. Whole-tree pixel and metadata budgets keep the old
capture charged during replacement. A successful flat reply cannot satisfy a
recursive request; flat failure replies still complete it.

The renderer imports each tree as one lease over all image groups and captured
lens programs. Partial preparation rolls back. Publication waits until every
group has been submitted on the device queue, making later sampling queue-safe;
this is not a claim that upload fences have completed. Dropping any member,
forgetting its world or shutting down retires the entire tree. Sparse physical
layers retain the fixed top-GUI handle slot.

PortalImageSource publishes the complete tree atomically and checks child
incarnations during retention. Retiring an old child removes its old capture
without cancelling an unrelated replacement. Its payload-only mode retains
ordered captures in the bounded inbox for a parent producer, wraps leaf layers
as one-node trees and performs no GPU import. Payload expiry and consumer
endpoint retirement prevent later extraction. The body graph has an optional
aperture overlay before depth export, physical layers, top GUI and lenses.

An initial build caught an optional-projection expression in the import code;
the corrected final build passes. The first payload test constructed replies
with pre-stamping request keys; fixtures now decode the actual issued request.
portal-tree-source-full-headless.log passes 92,822 assertions / 480 cases.
portal-tree-source-gpu.log passes 83,772 / 4 on offscreen Vulkan, covering tree
imports, source replacement/retirement and existing flat layered paths.
portal-tree-graph-tests.log passes 9,881 / 226, including all 32 body-graph option
combinations. Formatting and diff checks pass. No release profiling, sanitizer
run or interactive Studio verification was performed for this slice.

The production capability case now reaches the producer, which explicitly
refuses recursive ordered capture: portal-tree-producer-capability-gpu.log has
35 passing assertions and one failure, `Status 4 != Ok`, with diagnostic
"unsupported recursion depth or camera clip plane". Producer tree collection,
bottom-up current-body composition and the independent scaled native HDR/depth
oracle remain unfinished. ComposePortalBodyImage refuses a tree capture until
that composition path exists, preventing a root-only image from implying nested
support.

### Recursive producer, body composition and delegated identity

The producer now collects ordered child payloads into bounded capture trees
without importing child images on its GPU. Each node keeps its own camera,
lighting, depth units and captured programs. Current-body composition walks the
tree bottom-up, maps the body through each seam and uses the encoded aperture
geometry to compose child output before parent physical layers, GUI and lenses.
Fresh output adoption preserves the prior complete image if an intermediate
allocation fails. Local reflected surfaces still return an explicit unsupported
diagnostic; this does not close recursive mirror support.

The independent three-room native comparison reproduced incorrect depth export
ordering. Aperture colour was ready while depth export could run before the
aperture wrote hardware depth. An explicit graph dependency now orders that
export after aperture composition. The scaled native HDR/depth comparison passes,
including a second body pose without room uploads and capacity-failure rollback.
Original failure frames remain in
`.cache/build/dev/tests/nested-body-depth-pre-order-fix`.

Host relay bindings now carry exact local and published endpoint tuples. The
source authenticates the transport separately from the canonical tree identity;
binding replacement and withdrawal retire affected captures. An alias refusal
test reproduced acceptance of a local child tuple as a published identity
(231 passing assertions, two failures in portal-tree-alias-red.log). The fixed
check passes all 233 assertions. Binding decoding also refuses conflicting local
tuples across exports and return routes before allocating owned strings.

Final dev checks pass: portal-tree-final-headless.log has 93,055 assertions /
481 cases; portal-tree-final-world.log has 32,635 / 249; and
portal-tree-final-graph.log has 10,449 / 226. Offscreen Vulkan checks in
portal-tree-final-gpu.log pass 320 / 2 for native scaled composition and actual
HostLink/PresentationRelay root export, withdrawal and replacement. The latter
currently carries a one-node tree, so it does not prove nested relay routing.
portal-tree-final-source-gpu.log passes 127,247 / 4 for producer layers, source
publication and tree import. portal-tree-final-producer-gpu.log passes 81 / 1
for actual recursive producer collection, budget refusal and child withdrawal.
Build, formatting and diff checks pass.

Nested relay routing, the broader animated/material/oblique native matrix and
the combined product failure/crossing gates remain open. No release profiling,
sanitizer run or interactive Studio verification was performed for this slice.

### Nested relay and oblique native follow-up

The relay fixture now also carries a two-node tree. Its child request and reply
cross HostLink and PresentationRelay to a separate parent-owned producer, using
the same bounded nested-reply channel policy as ServerPresentation. It verifies
both published identities, replacement, and child-only withdrawal while the
root endpoint stays live. This relay case passes in
portal-tree-nested-oblique-gpu.log.

The native oracle now retains the original scaled case and adds independently
translated/rotated room frames with an oblique camera. Its unchanged HDR and
depth limits expose three failures: four stationary aperture-edge pixels differ
in each root image, and native centre depth at the middle room's second body
pose differs by 0.000139236 against the 0.0001 bound. The combined log reports
675 passing assertions and three failures across two cases. This is an open
rendering gate, not completed oblique support. The failure images are under
`render-failures/nested-body-depth/transformed-room-0-pose-0` and `pose-1`
beside test_render.

Double-precision inverse-VP setup reduced the depth error only to 0.000135422
and left the four colour pixels unchanged; that experiment was reverted.
Geometry placement arithmetic alone does not explain the error. Packed instance
rotation, hardware depth and aperture rasterization remain under investigation.

### Oblique coverage and rotation precision

The four colour failures were reproduced numerically as frame/aperture coverage
disagreements caused by SNORM16 quaternion rounding. An eight-byte,
twenty-bit-component experiment cleared three pixels but left one gap. Full-float
rotations clear every HDR mismatch at the original 0.002 bound. The aligned and
translated/rotated/oblique cases retain all three subtree roots, both body poses,
separate depth units and the no-room-upload check.

Instance rows now use four aligned vectors, 64 bytes instead of 48. Joint rows
use seven words instead of five. CPU upload, shader instance/skin decoding and
build-time stride guards agree; the discarded twenty-bit codec is removed.
Independent double-rotation tests retain a 30-micrometre displacement bound at
100 metres. A new GPU fixture compares two weighted joints and two palette
updates against independently CPU-deformed meshes across the full HDR image.

The native-only ideal-ray depth assertion was a separate reference error.
The RTX 4090 reports eight subpixel precision bits, and independently snapping
the triangle vertices reproduces its pose-specific depth difference. The native
export check now starts from captured hardware depth and independently computes
camera-space distance with double-precision matrix math. Its 0.0001 tolerance
is unchanged. Parent/child aperture-depth analytic checks remain unchanged, and
the leaf composition also compares against native depth at 0.0001. This corrects
the raster reference; it does not claim ideal rays and rasterized triangles have
identical depth.

portal-skin-oblique-final-gpu.log passes 632 assertions / 2 cases on offscreen
Vulkan. Broader projection, character, layers, imports and relay checks pass
137,159 / 16 in portal-rotation32-regression-gpu.log. Full headless render passes
101,264 / 481 in portal-rotation32-headless.log; world passes 32,635 / 249 in
portal-rotation32-final-world.log. Shader validation passes all 37 staged modules,
including SPIR-V/MSL structure and bindings. Dev and optimized bench builds pass
after fixing private-name unity collisions in capture-camera conversion and the
world presentation stream. Formatting and diff checks pass.

The existing instance benchmark printed three optimized samples to the terminal:
10,000 unchanged rows measured 25 ns/row to pack, 17 ns/row to upsert prepacked
data and 16 ns/row for exact source reuse. This is current CPU cost, not a matched
old/new GPU performance comparison. The byte increase is explicit; broader
release 1/2/8-view traffic and timing gates remain open. No sanitizer run,
interactive Studio check or hardware execution of MSL was performed.

Original oblique failure artifacts are preserved under
`.cache/build/dev/tests/nested-body-depth-pre-rotation-fix`. Animated nested
crossing, moving lighting, full material/lens composition, recursive mirrors and
the remaining product acceptance matrix still need their own evidence.

### Nested animation, replacement capacity and lighting interaction

The three-room native oracle now includes two weighted joints, two palette
poses and an inherited body cut distinct from each entrance plane. Independent
CPU-deformed reference meshes cover aligned and translated/rotated/oblique room
frames, all three subtree roots and unchanged HDR/depth tolerances. These checks
pass while the retained room images receive no new uploads. Physical product
crossing and its lifecycle combinations remain separate acceptance gates.

Two new failing cases exposed production defects. A three-node capture with
two physical layers uses nine images, so the previous sixteen-image pool could
not keep an old tree while preparing its replacement. The derived limit is now
145 images: two maximum sixteen-node, four-image trees, sixteen composition
outputs and one retained root. Tree leases remain capped at sixteen; CPU/GPU
image budgets remain 32 MiB each. Tests cover ordinary nine-image replacement,
maximum sixty-four-image replacement, exact slot exhaustion and retirement.

Finite aperture axes of magnitude 1e30 overflowed the float cross product and
let an invalid entrance clip reach composition. Shared double-precision normal
construction and finite derived-value checks now refuse it before allocations.
Correct entrance clips still compose with both ordinary and 1e30 axes. A nearby
finite-input sampling overflow returned failure only after creating fifteen
textures (82 allocations versus 67 in portal-tree-sampling-overflow-red-gpu.log).
Sampling elements and determinant now pass the existing adoption validity rules
before any child renders. The final focused gate, including relay, passes
2,360 assertions / 5 cases in portal-tree-sampling-overflow-fixed-gpu.log.
Headless render passes 101,394 / 481 in
portal-tree-capacity-overflow-headless.log; broader Vulkan fixtures, character,
relay, producer and source checks pass 137,085 / 15 in
portal-tree-capacity-regression-gpu.log. These are dev checks, not release
performance measurements.

The new nested lighting oracle remains failing with default SSAO enabled.
portal-tree-lighting-ssao-gpu.log confirms seven full-HDR lighting failures
(217 passing assertions / 224): 8 to 17 pixels differ, with maximum error
0.0032959 against 0.002.
Body colour probes pass, but native room pixels beside the body silhouette are
darker. The retained room-only image lacks the current body's ambient occlusion.
The fixture retains distinct room lighting, a changed deepest-room local light
and recomposition of the old capture after replacement. Its tolerance and SSAO
configuration have not been relaxed.

Correcting this requires merging room/body depth and normals before per-room
SSAO, then applying that result only to ambient radiance. Multiplying finished
RGB would also darken direct light, emission and fog. Retaining the full G-buffer
would additionally require the matching shadow and lighting resources when
relighting; an alternative is shared-shader factorization into unaffected
radiance plus ambient response times SSAO. Neither transport is implemented yet.
Child aperture composition must remain after the parent's SSAO so depths from
different rooms never enter one occlusion domain. The next implementation must
also test room-to-body occlusion, fog/material AO and no room reuploads during
body movement, while preserving old/new capture ownership.


### Retained ambient transport and bright HDR precision gate

PIMG16 now carries paired native RGB10A2 normals and RGBA32F ambient response
with opaque captures. Response RGB includes material AO and fog attenuation;
alpha retains the original filtered SSAO sample. Four-plane capture, resident
adoption, copied import, hashes, compressed preflight and inbox/tree accounting
include both auxiliary planes. Opaque payload grows from 12 to 32 bytes per
pixel. Existing wire and CPU/GPU byte budgets remain unchanged. Auxiliary
planes must be paired and cannot appear on transparent or overlay layers.

Each retained room merges its own depth/normals with the current body before
SSAO. The body uses that occlusion in deferred lighting; room correction changes
only ambient radiance. Child apertures enter after this stage and keep separate
depth domains. Native padded normal allocations are cropped at capture to the
logical view extent without resampling their packed values.

The original and contact lighting cases pass 608 assertions in
portal-ambient-contact-gpu.log, including room-to-body and body-to-room AO,
two body poses, lighting replacement, retained-old isolation and zero room
reuploads. Expanded native controls also pass fog, emission and material AO.
The expanded run is nevertheless red: portal-ambient-material-gpu.log reports
1,428 passing assertions / 1,442 with fourteen failures, all in ambient=8.
The original 0.002 HDR tolerance and enabled SSAO remain unchanged.

Bright retained colour exposes lost precision before correction. In the leaf
revision-zero, pose-zero artifact, 84 pixels fail; all 215 failing RGB components
are lower by exactly 0.00390625, one binary16 ULP in [4,8). The correction starts
from rounded RGBA16F room colour and rounds again on output. Full-float response
cannot recover the lost baseline bits. Preserving the unrounded lighting
baseline or equivalent sufficient lighting terms remains implementation work;
this is not a completed general HDR solution. Artifacts are under
render-failures/nested-room-lighting/ambient-8-* in the dev test directory.

Broader capture/composition verification in portal-ambient-wide-gpu.log passes
833,464 of 833,670 assertions, 22 of 23 cases. The remaining 206 failures are in
rotated, offset-coplanar transparent panes, with roughly one-unit red/green
errors. Direct drawing orders transparent objects by centre distance, while
peeling groups exact per-pixel depths. Attribution and correction remain open;
no baseline establishes that this failure predates the current work.
Headless render passes 103,026 / 483 in portal-ambient-final-headless.log;
graph passes 10,564 / 228 in portal-ambient-final-graph.log; shader checking
passes all 40 modules in portal-ambient-final-shadercheck.log. These checks do
not close product crossing, shadows, recursive mirrors or release performance.


### Full-float baseline closes the reproduced bright ambient gap

PIMG17 adds a group-owned RGBA32F lighting baseline beside normal and response.
An optional deferred-lighting MRT variant writes the same unrounded result to
this plane and the ordinary RGBA16F colour in one invocation. Room correction
uses the full-float baseline. Ordinary deferred rendering retains one output
and does not allocate this plane. All three auxiliary planes are inseparable;
codec, compressed preflight, staging, resident reuse and retirement include the
additional hash and bytes. Opaque payload is now 48 bytes per pixel, with the
existing byte limits and image-handle limits unchanged.

portal-baseline-lighting-gpu.log passes 1,326 assertions across original,
contact, fog/material and ambient=8 scenarios. The unchanged 0.002 HDR gate
covers all subtree roots, both body poses, two lighting revisions and retained
old captures without room uploads. This closes the reproduced bright ambient
failure, not every possible HDR/material/lighting interaction.
portal-baseline-headless.log passes 103,311 assertions / 483 cases;
portal-baseline-graph.log passes 10,586 / 229; shader checking passes 41 modules
in portal-baseline-shadercheck.log. The dev build passes in
portal-baseline-build.log.

The transparent diagnostic in portal-coplanar-diagnostic-gpu.log confirms
reversed blend order: both red and green layers survive capture, while direct
object-centre ordering puts red last and per-pixel peeling selects green nearer.
Their linear depths differ by only a few float steps around three units and
sometimes quantize to the same exported depth. The 206 existing parity failures
remain open. A wider baseline run also exposed an incorrect new storage test
assumption that GPU attachment half conversion exactly matches CPU nearest
rounding; correcting that representation oracle and checking optional baseline
output ordering are the next verification steps.


Final baseline integration: portal-baseline-final-gpu.log passes 931,180 of
931,386 assertions, 24 of 25 cases. Only the 206 coplanar transparent ordering
assertions remain failing. Five-plane copied/resident storage, import reuse,
producer/relay/tree composition, all four lighting scenarios and optional
baseline extent checks pass. The storage oracle now bounds attachment conversion
between adjacent half values instead of requiring CPU nearest-rounding identity;
the full-image 0.002 HDR tolerance is unchanged.

The optional-output test reproduced a real sizing defect: baseline-first with
33x19 baseline and 65x37 colour incorrectly executed deferred lighting
(portal-baseline-extent-red-gpu.log, 23 / 24 assertions). ViewRecording now
resolves lighting dimensions from the named colour output. Both output orders
accept matching extents and reject mismatches in the final GPU run. The final
dev build passes in portal-baseline-extent-fixed-build.log, and headless render
passes 103,312 assertions / 483 cases in portal-baseline-final-headless.log.
Graph and shader results above remain current. No release performance run,
sanitizer run or interactive Studio check was performed for this change.


### Retained glass body-depth ties and ordering conflict

A new single-pane native oracle captures glass without an opaque body, then
imports that glass over a newly rendered body. It covers identity and
rotated/translated cameras, an oblique pane and body positions before, exactly
on and behind the pane. This is independent of transparent-to-transparent
ordering. The old exact-tie fixture included the body during capture, allowing
opaque-z to discard the glass before the import boundary was exercised.

portal-transparent-tie-red-gpu.log reproduces 309 incorrectly visible pixels
at the transformed exact tie, maximum HDR error 0.50415 (147 / 148 assertions).
Transparent capture exported an interpolated world-position dot product;
opaque body depth came from actual D32 reconstruction. The two calculations
were not equal for the same surface. The transparent-layer node now exports
selected D32 through the existing depth-linearise shader, matching the body's
calculation. Removed geometric R32 writes from ordinary and interface peel
shaders and their MRT attachments. A final fullscreen pass writes the existing
R32 output; it adds no image plane, wire bytes or CPU readback. Its pass count
and output bytes are reported. Its release performance has not been measured.
portal-transparent-depth-fixed-gpu.log passes all six cases / 138 assertions
at the unchanged 0.002 full-image tolerance.

The ordering conflict remains open. Scenario five in ResourceImage uses two
intersecting panes centred at (0,0,-3), rotated by opposite 0.35-radian yaw.
The nearest colour changes between samples on the left and right, with near
and far depths separated by more than 0.1. Direct rendering's whole-object order
cannot match per-pixel peeling on both sides. The combined initial diagnostic
in portal-transparent-intersection-tie-red-gpu.log records 608 ordering
assertion failures, including the previous 206 coplanar failures, plus six
fixture projection-setup failures subsequently corrected. No epsilon or
parity tolerance change was introduced. A user question about which ordering
should define final behavior is pending; the body-depth correction is valid
under either choice.


Final shared-depth verification: portal-transparent-depth-wide-gpu.log passes
939,704 of 940,312 assertions, 25 of 26 cases. Only the 608 coplanar/intersecting
transparent ordering assertions fail. Body-depth ties, baseline lighting,
resource storage/import, producer/interface layers and tree/relay checks pass.
Headless render passes 103,312 / 483 in portal-transparent-depth-headless.log;
shader checking passes all 41 modules in portal-transparent-depth-shadercheck.log.
The build passes in portal-transparent-depth-build.log. Read-only integration
review found no additional defect. No sanitizer or interactive Studio run was
performed. The full render and product acceptance scope remains open.


### Scaled depth ties, nested single glass and ambient codec cost

The retained single-pane depth oracle now covers scales 0.37, 1 and 3.1,
including scaled camera near/far planes and translated/rotated room frames.
All eighteen before/tie/behind cases pass 414 assertions in
portal-scaled-tie-gpu.log at the unchanged 0.002 HDR tolerance.

The three-room lighting oracle adds one real physical transparent layer in leaf
C, separately before and behind its moving opaque body. Source room and glass
captures share a frame; the native reference renders complete geometry through
ordinary transparency without layer extraction. All subtree roots, both body
poses, both lighting revisions, retained-old isolation and zero room reuploads
remain checked. Native controls prove that glass affects room pixels and affects
or preserves the body centre according to its depth.

Initial failures were fixture mistakes: a view pass declared after shared
present, a leaf readback of the opaque intermediate instead of final glass
composition, and an AO control measured after glass attenuated its signal.
The fixture now declares view work before frame exports, reads the actual final
leaf output and compares AO before transparency at the original thresholds.
No production change was needed for this extension. The combined final GPU run
passes 2,616 assertions / 2 cases in portal-nested-glass-final-gpu.log, covering
all six lighting scenarios and eighteen scaled depth cases. The dev build
passes in portal-nested-glass-readback-build.log. The separate 608 multi-pane
ordering failures remain open; this does not establish general glass/HDR parity.

`just portal-ambient-bench 3` now measures the PIMG17 five-plane CPU codec for
one, two and eight distinct 256x256 views. The bench preset uses the optimized
build; the runner reports the minimum of three measured samples after warmup,
per view batch. Raw and compressed decode include validation; automatic
roundtrip includes encode, decode and validation of all five planes.

| CPU operation | 1 view | 2 views | 8 views |
| --- | ---: | ---: | ---: |
| Raw decode and verify | 1.766 ms | 3.513 ms | 14.441 ms |
| Compressed decode and verify | 1.797 ms | 3.691 ms | 14.246 ms |
| Automatic encode, decode and verify | 3.696 ms | 7.419 ms | 30.303 ms |

Each view has 3,145,728 uncompressed plane bytes. This deliberately patterned
fixture is highly compressible: its first view encodes to 12,703 bytes versus
3,146,074 bytes in equivalent raw framing. This is not a general bandwidth
ratio. The raw packet is validated against the same production-decoded image;
no production compression switch was added. Output was printed to the terminal,
not saved as benchmark result files. These measurements exclude GPU capture,
upload, rendering and network latency. GPU 1/2/8-view timing and residency gates,
plus profiling of the measured CPU codec cost, remain open. Existing aggregate
initializer warnings appeared during the successful optimized build; no warning
cleanup was mixed into this work.

### Profiled ambient sample validation

`just portal-ambient-profile 3` now prints the existing FrameGraph hierarchy
during setup, then disables collection for timed batches. Fixed codec scopes
separate hashing, sample validation, compression, framing and materialisation.
Linux perf events were unavailable (`perf_event_paranoid=4`); no system setting
was changed. The engine profile recorded zero dropped spans.

Before optimization, raw decode spent 1.731 of 1.792 ms in validation. Baseline,
response and depth sample scans consumed 0.636, 0.587 and 0.149 ms respectively.
Portable little-endian integer classification replaces the per-float ByteReader
scans in codec validation and direct import. Depth still rejects negative zero;
baseline accepts every finite value; response admits negative zero and bounds
alpha to one. Hashes, plane pairing and admission budgets remain enforced.
The shared private helper uses byte loads without alignment or aliasing
assumptions. Classification tests compare the original floating-point rules
across both signs, all exponents, boundary mantissas, seeded random words,
unaligned planes and each component position.

Matched optimized bench measurements use the same profiling scopes, patterned
256x256 five-plane fixture and minimum of three samples after warmup. Times
below are milliseconds per batch; profile collection is off during measurement.

| CPU operation | 1 view before / after | 2 views before / after | 8 views before / after |
| --- | ---: | ---: | ---: |
| Raw decode and verify | 1.885 / 0.486 | 3.736 / 0.971 | 15.290 / 4.099 |
| Compressed decode and verify | 1.921 / 0.535 | 3.846 / 1.070 | 15.533 / 4.294 |
| Automatic encode, decode and verify | 3.991 / 1.213 | 7.984 / 2.431 | 32.428 / 10.286 |

The post-change raw profile measures baseline/response/depth scans at
0.078 / 0.076 / 0.016 ms. Hashing now accounts for more of validation cost.
These are CPU measurements, not GPU or network performance claims; direct
import speed was not separately measured. Benchmark output stayed in the
terminal. Dev and optimized builds pass. Headless render passes 131,931
assertions / 485 cases in portal-codec-samples-headless.log. Focused offscreen
Vulkan import and six-scenario nested lighting pass 2,856 assertions / 3 cases
in portal-codec-samples-gpu.log. Independent read-only review found no acceptance
gap. The separate 608 multi-pane ordering failures and broader render/product
acceptance requirements remain open.

### Retained directional-shadow oracle remains red

The seventh nested-lighting scenario enables real shadow casting with an angled
sun. The six previous scenarios explicitly disabled casting on every row and
therefore did not prove shadow interaction. Two native controls toggle only
the room caster or current-body caster, preserving geometry and light-fit
bounds. Both controls require receiver pixels to darken by more than 0.002,
for both body poses and both lighting revisions.

The first fixture attempt used single-sided planes and failed its first native
control in portal-shadow-oracle-gpu.log. The shadow pipeline intentionally culls
front faces. Closed slab casters now provide back faces while keeping the
original front depths and receiver masks. This is a fixture correction, not a
production shadow change.

The corrected run in portal-shadow-closed-gpu.log passes 2,712 of 2,726
assertions. All native controls and the six earlier scenarios pass. Fourteen
full-image comparisons fail across three subtree roots, two body poses, two
lighting revisions and retained-old checks. Maximum HDR error is 0.139465
against the unchanged 0.002 limit. The leaf pose-zero image has 44 mismatched
pixels in region (25,27,13,11). Expected, actual, difference and raw HDR artifacts
are under render-failures/nested-room-lighting/shadows-*. Dev build passes in
portal-shadow-closed-build.log. No performance conclusion comes from this run.

Current composition retains scalar lighting and ambient derivatives but no
room shadow map. Its native shadow pass fits and draws current body/aperture
rows; retained room casters are absent. The room correction shader adjusts
ambient radiance only, so newly inserted body shadows cannot alter retained
room direct lighting. A complete fix must preserve light-space room occlusion,
the matching shadow projection and the room's direct-light response under
existing ownership and byte bounds. Reconstructing only visible room depth
would omit off-camera casters. This requirement remains open alongside the
independent multi-pane transparency ordering decision.

Implementation constraint: native light fitting uses the union of all source
row bounds and current body bounds. A body outside the original bounds changes
the light matrix, so an old shadow image cannot simply be reprojected with exact
parity. Keep room caster geometry at the source. A prospective image-only path
requests the source shadow image for the shared matrix and exact retained
snapshot, then combines it with current body shadow depth locally. A source
that cannot serve that snapshot must refuse and refresh the capture rather
than mix revisions. Room directional response can carry unshadowed RGB and
original shadow visibility, allowing the existing unrounded baseline to apply
the new visibility delta. This design is not implemented or verified; shadow
image byte admission, ownership, readiness and retirement remain required.

### Directional response output verified; shadow delivery remains open

Deferred lighting now has an optional `directional-response` RGBA32F output
paired with `lighting-baseline`. Its RGB is the unshadowed directional term
after fog attenuation; alpha is the original four-tap shadow visibility.
The same shader invocation writes ordinary RGBA16F colour and its unrounded
baseline, preserving their common calculation. Background and authored final
colour have zero response and visibility one. The ordinary pipeline does not
request this output. A lazy three-attachment pipeline and graph-owned target
serve connected outputs, with tracked allocation and a directional output-byte
counter. Response adds 16 bytes per pixel, or 1 MiB at 256x256.

Backend validation rejects response without baseline, shared output resources
and mismatched extents. Colour determines dimensions regardless of output
ordering. Graph serialization roundtrips the optional port, and devices only
need its RGBA32F capability when an optional output is connected.

The new GPU oracle compares closed-caster shadow-on and shadow-off native
lighting with unchanged scene bounds. Response correction in both directions
matches the full-float baseline within the unchanged 0.002 HDR bound, including
direct intensity eight, material AO, emission and fog. Independent ordinary
deferred colour agrees with the optional-output path. A bounded camera phase
sweep positively exercises fractional PCF visibility and its correction;
empty output, output order, mismatched sizes, missing baseline and shared
attachments are also checked.

An initial build caught accidental edits in unrelated callbacks; those were
removed before GPU execution. An initial GPU comparison read the native lit
target after volumetrics reused it, causing four fixture failures. Both oracle
pipelines now stop immediately after deferred writes. No tolerance changed.
The final dev build passes in portal-directional-fractional-build.log. Focused
Vulkan passes 271,282 assertions / 4 cases in portal-directional-final-gpu.log,
including all eighteen scaled transparent/body depth cases. Headless render
passes 131,931 / 485 in portal-directional-headless.log; graph passes 10,597 /
229 in portal-directional-graph.log. Shader checking passes 42 modules with
SPIR-V and MSL structural validation in portal-directional-shadercheck.log.
No Metal hardware, sanitizer, interactive Studio or release timing was run.

This output is a prerequisite, not the retained-shadow fix. It is not yet
transported in PIMG17 or consumed by retained composition. The broader import
and seven-scenario lighting run passes 3,366 / 3,380 assertions, retaining the
same fourteen shadow failures in portal-directional-retained-gpu.log. Source
shadow snapshots, shared light fitting, bounded delivery, current-body depth
composition and atomic image ownership remain required. Native 2048x2048 D32
maps are 16 MiB each, so copied delivery cannot rely on compression to meet the
4 MiB wire bound; a complete design must account for retained and scratch maps
under GPU admission limits and share compatible maps across views.

### PIMG18 directional response ownership and delivery

The optional directional response now travels through production capture,
PIMG18 encoding, borrowed preflight, inbox/tree accounting and GPU import.
Presence uses flag 256 and compression flag 512 in the existing 16-bit mask.
The plane requires the complete ambient set, depth, opaque scope and captured
lighting. Its exact 16-byte-per-pixel size, hash, finite nonnegative RGB and
visibility in [0,1] are checked; negative zero remains valid. Transparent and
spatial layers cannot carry it. Ambient-only captures remain valid without the
new plane. PIMG17 packets are not accepted by the PIMG18 decoder.

Six-plane captures cost 64 bytes per pixel, with 52 auxiliary bytes charged by
borrowed preflight before decompression. The wire limit remains 4 MiB; CPU and
GPU import limits remain 32 MiB. Per-image staging covers the maximum six-plane
extent at 16 MiB. A raw 256x256 six-plane payload fills 4 MiB before framing and
is refused; compressed payloads are still charged at their expanded size.
Tests cover raw/compressed roundtrips and truncations, invalid flags, hash-valid
bad samples, compressed invalid samples, inbox exact limits and tree preflight.

Copied capture, resident capture/adoption, cache reuse, hash-only replacement,
plane removal, upload retries and retirement include the directional texture,
hash and pending capacity. Producer capture moves and hashes all six planes
from one frame. The nested lighting oracle now carries six-plane room captures
through both lighting revisions and retained-old checks without room reuploads.

Review found and corrected the capture port whitelist, direct transparent-layer
admission and tree capacity accounting. A new exact-byte eye-image test then
reproduced a remaining five-output guard failure in
portal-directional-eye-red-gpu.log. Raising that guard to six makes imported
directional samples reachable through the graph. The test compares a distinct
17x9 response pattern, and all other planes, after upload and again after
resident recapture/adoption. Six uploads occur once; final drops release all
import and cache bytes. The fixed run passes in portal-directional-eye-fixed-gpu.log.

Dev build passes in portal-directional-transport-fixed-build.log after adding
a missing test generator include; the final guard build passes in
portal-directional-eye-fixed-build.log. Headless render passes 134,163
assertions / 487 cases in portal-directional-transport-headless.log. Graph passes
10,598 / 229 in portal-directional-transport-graph.log. GPU storage/lifecycle
passes 875,676 / 13 in portal-directional-storage-gpu.log, excluding the already
red multi-pane ordering fixture. Producer, source, import and seven-scenario
lighting pass 131,422 / 131,436 assertions, five of six cases; only the same
fourteen shadow comparisons fail in portal-directional-transport-gpu.log.
The first combined GPU filter misspelled resourceimage; the separate storage
run above supplies that missing coverage.

`just portal-ambient-bench 3` passes on the optimized build with the existing
five-plane 256x256 fixture under PIMG18. Roundtrip batch minima are 1.252 / 2.532 /
10.684 ms for 1 / 2 / 8 views. Payload and framing sizes are unchanged. Output
stayed in the terminal. These figures do not measure the new sixth-plane cost
or GPU performance. Existing aggregate initializer warnings remain unrelated
to this change. No shader source changed in this delivery step.

The directional response is now delivered but retained composition does not
yet apply its shadow delta. Source shadow snapshots, matching light matrices,
bounded shadow-image delivery and current-body shadow composition remain open,
as do the broader render and product acceptance gates.

### Retained shadow delivery constraints, inspected 2026-09-09

Source capture currently retains image tokens and pixels, not immutable caster
assets. `WorldFrame` is reused during producer pumping. Copying draw rows and
joints would not preserve later mesh, texture or shader revisions. An old room
may reuse a matching captured source shadow image, but cannot request current
casters under an old image revision without checking the complete source state.
A changed fit or unavailable old state requires a complete capture refresh.

Native fitting uses all source and body row bounds, including noncasters and
off-camera rows. Capture must carry the source bounds and exact light matrix;
reuse requires the union fit with the current body to match that matrix.
`EyePlayer` hides primary camera rows while preserving their shadows, so it
cannot be repurposed as authenticated shadow-caster exclusion.

The native target uses the selected depth format, with D32, D24 and D16 device
fallbacks. A D32 delivery path must explicitly reject a different native format.
Vendored SDL supports exact depth-to-depth texture copies with matching formats
and depth transfer-buffer upload/download. The existing global shadow target
can be scratch: copy source depth, then load it while drawing body casters with
cycling disabled. No extra global scratch target is required. This is inspected
API support, not a completed GPU delivery path or Metal hardware proof.

The existing image capture API requires a colour plane, matching extents and at
most 512 pixels per side. Shadow capture therefore needs a separate depth path.
Sixteen 512-square D32 tiles cost 1 MiB each and fit the 4 MiB message bound even
without compression. Admission must reserve the full expanded 16 MiB before
accepting tiles and bind every tile to one authenticated snapshot and fit.
Mixed snapshots, conflicting duplicate tiles and incomplete transfers must
never publish a ready map. This tiled protocol is still to be implemented.

One retained source map plus a six-plane 256-square eye image costs 20 MiB of
the existing 32 MiB import limit. The renderer's existing global scratch costs
another 16 MiB outside that import limit. Two distinct retained source maps fill
the import limit before any eye planes. Sharing equal maps does not solve the
worst case for recursive worlds or concurrent revisions. Delivery scheduling
and explicit readiness under pressure remain required; no limit was widened.

### Native directional correction primitive

`ambient-correct` now accepts an optional all-or-none group: directional
response, retained room depth, retained room normal and an explicit D32 shadow
map. The original three-input ambient shader remains unchanged. The new lazy
variant reconstructs room-world positions with the capture camera and applies
both ambient and directional deltas before one RGBA16F store. Native deferred
lighting and correction share the same PCF function, bias, taps and sampler.
The explicit map enables sampling independently of local caster presence and
supplies its own texel size. It must match the current light matrix and world
domain; this node does not authenticate source snapshot metadata.

Backend installation rejects all fourteen partial combinations. Execution
checks eye-plane formats/extents, square D32 shadow format and output aliasing.
The map can have a different extent from the eye. Headless render passes
134,227 assertions / 488 cases in portal-shadow-correction-headless.log,
including all sixteen optional-input combinations and document roundtrips.
Graph passes 10,598 / 229 in portal-shadow-correction-graph.log. Shader checking
passes 43 SPIR-V/MSL modules in portal-shadow-correction-shadercheck.log.

The GPU oracle imports six planes captured from native shading and executes
the actual correction node against a changed native map. Both signs of the
shadow delta, intensity eight, fog, emission, material AO and fractional PCF
agree within the unchanged 0.002 HDR bound. The initial run had sixteen
failures because the new ambient-response producer reused undeclared readback
storage. Explicit final capture reads fix that lifetime. The first lifetime
fix also put a frame capture between view nodes and was correctly refused;
the correction graph now branches before that final capture. The receiver
keeps native shadow clearing active when the closed caster is disabled, so
the explicit map actually represents the unshadowed state. Final dev build
passes in portal-shadow-correction-map-build.log. Focused Vulkan passes
355,681 / 4 in portal-shadow-correction-map-gpu.log.

The production retained-tree graph is not connected to this optional group
until it owns an authoritative source-plus-body map. Its seven-scenario test
still has the same fourteen shadow failures. Combined with scaled transparent
body checks, portal-shadow-correction-retained-gpu.log passes 3,168 / 3,182
assertions, one of two cases. This primitive does not close retained shadows.
No sanitizer, Metal hardware, interactive Studio or GPU release timing ran.

`just portal-directional-bench 3` now measures six-plane PIMG18 CPU delivery.
The timed fixture is 255-square so raw framing fits the unchanged wire bound.
Setup also verifies compressed 256-square equality and raw refusal at
4,194,686 bytes. At 255-square, 1 / 2 / 8-view payloads are 4,161,600 /
8,323,200 / 33,292,800 bytes; compressed wire sizes are 16,475 / 32,681 /
135,382 bytes. Optimized three-sample batch minima in milliseconds:

| Operation | 1 view | 2 views | 8 views |
| --- | ---: | ---: | ---: |
| Raw decode and verify | 0.674 | 1.347 | 5.981 |
| Compressed decode and verify | 0.733 | 1.467 | 5.886 |
| Encode, decode and verify | 1.663 | 3.315 | 13.945 |

Benchmark output stayed in the terminal. The existing five-plane default job
remains available. These measurements exclude shadow-map delivery, GPU work
and network latency. Existing aggregate initializer warnings remain unrelated.

### Exact native shadow capture and shared fitting

`View::DirectionalShadowBounds` now accepts an explicit enclosing AABB in the
view's world. Native views keep their ordinary all-row fit. Invalid, inverted,
overflowing or non-enclosing overrides refuse rendering; source bounds remain
separate from the chosen domain. Override changes invalidate retained view
content. A GPU oracle renders full native casters and then source/body subsets
under the shared fit: the minimum of the two subset depth maps equals the full
native map exactly at scales 0.37, 1 and 3.1, including an off-camera caster.

The new per-view `shadow-capture` node reads the native D32 map into
`ResourceImage::Depth`, with no colour or auxiliary planes.
`ResourceImageKind::DirectionalShadow` distinguishes this result. Its `Shadow`
metadata owns source bounds, fitted domain bounds and the exact column-major
light matrix, alongside the existing producing-frame identifier. Non-D32
native formats and resident delivery are explicitly refused. This is copied
capture ownership, not a resident shadow-import API or a wire protocol.

Capture runs before another view can overwrite the global shadow target.
Pending shadow captures force fresh world work, including all-one maps for
empty sources. Explicit domains and pending captures isolate their world
preparation; `FramePreparation::InvalidateWorld` restores ordinary preparation
for following views without repeating completed frame effects. Tests compare
two worlds' batched captures with separate native renders and verify that an
explicit fit does not leak into the next ordinary view of the same world.

All retained capture transfer allocations, including colour capture, now share
an explicit 32 MiB cap. Idle transfer buffers are reclaimed before allocation
refusal; recorded or submitted buffers remain protected until their fences
complete, including cancellation. Two native 16 MiB readbacks can be staged
together; a third returns failure without allocating. This is a capture
transfer-buffer bound, distinct from the existing portal import limits and
from owned CPU images returned to callers. The five capture slots stay bounded.

The first build caught missing private test constants/namespace qualification.
The first GPU run then reproduced an empty-source crash. GDB identified
`BindInstanceBuffers` passing nonexistent storage into Vulkan. A clear-only
shadow pass now binds no draw resources. The fixture originally mistook
`FrameResult::Ran` for evidence of GPU clearing; the new `render.shadow.clears`
counter verifies no clear occurs without a request and exactly one occurs for
an empty-source capture. No tolerance changed.

Final dev build passes in portal-shadow-capture-batch-build.log. Headless render
passes 134,244 assertions / 489 cases in portal-shadow-capture-headless.log.
Graph passes 10,600 / 229 in portal-shadow-capture-graph.log. Focused Vulkan,
including existing image lifecycle and directional correction, passes
1,231,849 / 22 in portal-shadow-capture-final-gpu.log. Coverage includes exact
16 MiB depth bytes, same-frame colour/shadow identity, matrix/bounds metadata,
queued and submitted cancellation, empty maps, capacity refusal and retry,
cross-world captures, shared fit, malformed domains and ordinary-view recovery.
The existing multi-pane ordering failure remains excluded from this image
lifecycle run. No shader source changed in this step. No sanitizer, Metal
hardware, interactive Studio or release GPU timing ran.

The production retained tree still has fourteen shadow failures: 2,754 / 2,768
assertions pass in portal-shadow-capture-retained-gpu.log. Next work is binding
source exclusion and snapshot identities, bounded tiled delivery, imported
shadow ownership, copying source depth into scratch and loading current body
casters before the correction node. Multiple retained maps and revisions must
respect admission and readiness; capture primitives alone do not close this
gate or the full render plan.

### Tiled shadow delivery and imported source/body composition

`PortalShadowImage` carries a native 2048-square D32 image as sixteen raw
512-square tiles. Each packet repeats the expected producer incarnation, eye
key and pixel hash, capture tick, content/lighting revisions, excluded player,
source/domain bounds, light matrix and full depth hash. Assembly reserves its
16 MiB budget before accepting tiles, checks each tile hash and finite depth
samples, accepts exact duplicates, and publishes only after all tiles and the
full image hash agree. Cancellation releases the allocation. The caller must
supply an authenticated expected snapshot; packets do not establish trust.
There is no presentation-channel or source-job integration yet.

The renderer now owns immutable imported shadow handles. Their actual D32
textures and pending CPU capacity share existing portal import limits. Two
maps fill the 32 MiB limit, leaving no room for retained eye textures; admission
refuses further imports without consuming their inputs. Uploads use the shared
16 MiB staging buffer, retain CPU data after failed submission, and release it
on successful submission. Drop and world retirement release ownership. Code
review checked resident-image adoption against the same texture admission
limit; pending source captures remain separate capture allocations.

A matching view requires the bound world, explicit enclosing domain and exact
native light matrix. Its shadow pass uploads once, copies the source depth into
the native shadow target, then loads it while drawing current body casters.
The body compositor selects the complete directional correction graph and
checks the retained eye identity, producer, exclusion and revisions. Imported
eye capture ticks now survive direct import, replacement, reuse and layer sets.
Missing or different ticks refuse composition. Resident colour adoption has no
authenticated producer tick and therefore cannot establish this pairing.

The GPU integration test captures a source room, sends its exact depth through
the tile codec, imports it, and renders only current body geometry. Combined
D32 bytes equal a full native render exactly. Exported body-compositor colour
matches within the unchanged 0.002 tolerance. Disabling either caster changes
visible lighting, so parity does exercise shadows. Coverage also verifies one
upload across repeated views, shared shadow/eye capacity refusal, release, and
a valid but stale-tick map refusing the otherwise matching retained image.

Dev build passes in portal-shadow-delivery-final-build.log. Focused Vulkan
passes 1,282,685 assertions / 23 cases in portal-shadow-delivery-final-gpu.log,
including previous capture, fitting, image lifecycle and correction tests.
Headless render passes 134,844 / 493 in
portal-shadow-delivery-final-headless.log. Graph passes 10,788 / 230 in
portal-shadow-delivery-graph.log, including complete directional bindings for
eye/seam documents with ambient prerequisites enabled automatically. The first
build caught a test constant colliding with Catch's CAPTURE macro, now fixed.
No shader changed, tolerance widened, or interactive Studio check ran. No
release GPU timing, sanitizer or Metal hardware verification was performed.

This does not complete retained tree shadows. Authenticated source exclusion,
source snapshot production and routing, per-node shadow association, refresh
on changed fitting, and bounded scheduling across multiple retained revisions
remain required. The tile encoder currently validates the full 16 MiB image
for every tile; this has no measured throughput claim and needs profiling
before use in the production path.

The production retained-tree rerun remains at 2,754 / 2,768 assertions with
fourteen shadow comparison failures in portal-shadow-delivery-retained-gpu.log.
The retained-old-lighting pose reports maximum error 0.139465 against 0.002.
The new standalone import/compositor route is not yet connected to this tree.

### Serial tree shadows and explicit source-body exclusion

The renderer now exposes Begin/Poll/Accept/Cancel for retained tree composition.
One job owns copied body rows, palettes and content-owner bindings. Common tree
preparation serves this path and the existing synchronous compositor. Each
pending request identifies one node's accepted eye image, producer incarnation,
capture tick, revisions, exclusion and mapped body/aperture bounds. Nodes run
from children toward the root. Accepted maps must match that identity, the
source/body domain union and the native light matrix. Only Poll transfers the
completed root image to its caller; a late Accept cannot consume the result.

Only one job is admitted. It checks the existing import byte limits before
requesting/accepting another 16 MiB map and preserves the pending request on
budget refusal. Child images remain owned until their parent consumes them.
A real ordered-queue fence gates map retirement and advancement, avoiding an
unbounded backlog of deferred map allocations. Cancellation, tree retirement
and asset-epoch changes invalidate work. Submitted cancellation retains GPU
resources until the fence completes; normal Render calls also reap cancelled
jobs. These are import ownership limits, separate from graph scratch, capture
transfer buffers and driver allocations. No latency or throughput improvement
is claimed for this serial path.

The nested lighting oracle now drives this API with source fixtures that own
immutable scene inputs and lighting for two revisions. All fourteen previously
failing shadow comparisons pass without changing the 0.002 tolerance. They
cover three subtree roots, two body poses, two lighting revisions, and retained
old lighting after the sun direction changes. Both casting directions still
have visible receiver controls. Tests destroy caller geometry/target storage
after Begin, refuse stale ticks and a competing map's byte pressure, retry after
release, cancel pending work, retire its tree, and send a late final response.
This verifies the renderer job; it does not connect host source jobs or network
tile routing automatically.

PIMG and capture-tree wire version 19 add a distinct `RetainedBodyPlayer` account
identity. Ordinary `EyePlayer` still hides only primary colour while preserving
shadows. The new ordered capture profile removes the authorized body from both
source colour and shadow rows, after computing the original full native fitting
domain. Selection covers native/held rigs and decoded forwarded rows, remaps
primary-hidden row indices, and does not infer foreign ownership from coincident
local handles. Tree nodes and admission bind the same retained-body identity.

`PortalImageProducer::SetRetainedBodyAuthorization` supplies the host policy for
an exact requesting endpoint incarnation and canonical account. Nonempty
exclusion defaults to refusal; authorization is checked before work, after waits
and before publishing completed captures. Application policy and automatic
request opt-in are not yet connected. The GPU producer test proves ordinary eye
selection preserves D32, authorized exclusion changes source colour and D32,
shared-domain depth equals an independent remaining-room render, room-only
fitting differs, and revocation after submission cannot publish a success.

`SourceEmpty` now distinguishes no source rows from the native empty-scene
fitting fallback. Empty source bounds are canonical zero and empty depth is
exactly clear. Capture metadata also includes an imported seed's source bounds
when recapturing a combined map. Import and job fitting no longer union a
fictional origin box into an empty non-origin source. Tick zero is accepted as
a legitimate initial ECS tick; missing imported tick identity remains distinct
and refused. Wire tests cover both new cases and their malformed alternatives.

Verification: portal-shadow-tree-verified-build.log passes. Headless render
passes 135,394 assertions / 500 cases in
portal-shadow-tree-verified-headless.log. Vulkan passes 194,347 / 16 in
portal-shadow-tree-verified-gpu.log, including tree composition/import, source
runtime, exclusion and recovery. The separate existing image/shadow lifecycle
run passes 1,231,849 / 22 in portal-shadow-tree-lifecycle-gpu.log. Existing
multi-pane ordering failures remain excluded from that lifecycle filter. No
shader changed, graph algorithm changed, sanitizer ran, Metal hardware ran, or
interactive Studio session ran in this step.

The first runs exposed an empty-source metadata rejection, an ordinary request
identity change from hashing an absent exclusion, and rejection of unused caller
lights in the old-image job path. All are fixed. Broader runtime testing exposed
four appearing-child recovery assertions: a one-message presentation queue
returned Full for the second child and was treated as Unsupported. That branch
now retries transient transport pressure after cancelling the unsent inbox and
resident reservations. The existing copied/resident recovery cases pass. Temporary
production diagnostics were removed. Fixture mistakes in the shadow port name,
eye clip plane, light direction and test-only constants/constructor were corrected;
map comparisons avoid dumping entire 16 MiB vectors on failure.

Next source integration must capture shadow and eye data in one producing frame,
construct an authenticated manifest, route bounded tiles to the pending node,
and retain or refuse the exact old source snapshot when fitting changes. There
is no retained caster asset revision pinning yet. The current ordered pipeline
uses all five capture slots when spatial overlay is present. A sixth bounded
slot can support the complete bundle without increasing the 32 MiB capture
transfer cap: padded worst cases under current pixel admission are 24,840,192
bytes without overlay and 23,740,928 with overlay. This capacity change and the
source collector are not implemented yet. Multi-view scheduling, source policy
installation and the full R01-R17/P0-P12 plan remain open.

`just portal-directional-bench 3` builds and passes under PIMG19. Raw 256-square
six-plane admission still refuses 4,194,686 bytes; compressed exact roundtrip
accepts 17,545 bytes. At 255 square, three-sample minimum CPU roundtrips for
1 / 2 / 8 views are 1.695 / 3.395 / 14.438 ms. Plane and wire byte totals are
unchanged from the preceding codec measurement. Output stayed in the terminal.
This measures the existing eye-image codec, not tiled shadow transport, GPU
composition, network latency or end-to-end frame cost. The optimized build
exposed missing aggregate defaults on the new retained-body selection fields;
explicit empty defaults remove those warnings. Earlier unrelated aggregate
initializer warnings remain.

Final explicit-default build passes in portal-shadow-tree-defaults-build.log;
formatting and diff checks pass. The temporary diagnostic failure log is kept
as portal-shadow-recovery-diagnostic-gpu.log.gz because the original failed
vector assertion printed both complete depth maps.


## Same-frame source shadow retention and manifest admission

The ordered retained-body source profile now queues its native D32 map alongside
opaque, transparent/overflow and optional spatial-overlay captures. Renderer
capture capacity is six; the shared transfer allocation bound remains 32 MiB.
Collection requires matching nonzero renderer CaptureFrame values and validates
the native dimensions, kind and metadata before publishing the eye. World tick
0 remains valid and is preserved independently of the renderer frame number.
The synthesized leaf tree now preserves RetainedBodyPlayer too.

Each producer reserves at most two native maps, 32 MiB total, before submitting
the GPU capture. The reservation accounts for maps still being captured and maps
awaiting retrieval; actual retained CPU bytes are reported separately. Successful
eye delivery makes the original owned map retrievable once through
TakeShadow(exact requester incarnation, exact eye, now). Pending maps cannot be
taken. Endpoint/policy changes, cancellation and Clear release ownership; a ready
map expires one second after eye delivery. Retrieval never samples a later world
revision. These are per-producer retention bounds, separate from the unchanged
renderer transfer/import/staging bounds, not a new global host memory budget.

The manifest codec reuses the canonical SHDW snapshot prefix used by tiles.
Decoding requires exactly one complete manifest and rejects trailing tile data,
truncation, oversized strings and invalid metadata without changing the output.
MatchesPortalTreeShadowRequest binds producer, eye, tick, revisions, eye hash,
exclusion, exact source/body domain and native light matrix. The pending request
now owns its accepted LightDirection; both pre-assembly checks and renderer
acceptance use the same matcher. Host envelope authentication remains required.

Verification:

- Library and test builds pass: portal-shadow-pair-library-build.log,
  portal-shadow-pair-test-build.log and portal-shadow-pair-final-build.log.
  Existing unrelated aggregate-initializer warnings remain.
- Headless render passes 136,066 assertions / 503 cases in
  portal-shadow-pair-headless.log. The subsequent change only adds GPU fixture
  overlay coverage and was compiled in the final build.
- Final offscreen Vulkan source/tree/import/runtime checks pass 194,488 / 17 in
  portal-shadow-pair-final-gpu.log. The actual source fixture includes a visible
  spatial overlay and therefore six distinct capture resources. It checks exact
  native D32 bytes, paired metadata, wrong requester/key refusal, take-once,
  revocation, two-map saturation, third-map refusal, expiry and pending Clear.
  A generic six-result group also verifies same-frame collection and seventh
  slot refusal. No image tolerance was changed.
- Formatting and whitespace checks pass. This is dev Vulkan correctness
  evidence. No new release timing, sanitizer, Metal or interactive Studio run
  was performed. The earlier codec benchmark does not measure these maps.

Next integration must retain the authenticated requester chain. A child map is
owned by its immediate parent's internal Pending.Replies endpoint; the final
root consumer cannot retrieve it using its own requester. Finish currently drops
the parent/child job associations after tree delivery. Retain a bounded delivered
tree route record binding parent requester + parent eye to each accepted child
producer, child eye and original internal requester. Pulls must traverse these
records with current endpoint and policy checks, never impersonate a requester
named in an untrusted payload. Relay bounded packets without assembling maps at
every intermediate producer.

PortalImageHost::Pump and PumpDriverLink are the existing scheduling/authenticated
transport boundaries. Host integration still needs explicit trusted retained-body
policy configuration, composition begin/poll/cancel ownership, shadow-message
demultiplexing, and a receiving assembly charged against existing renderer CPU
import admission. Keep one map/tile/assembly in flight per serial job. Validate
the manifest before tile allocation. A changed exact domain must refuse or
refresh the eye; old caster mesh/texture/shader revisions are not pinned, so
regenerating an old map from current source assets remains invalid. Automatic
host routing and full R01-R17/P0-P12 completion remain unfinished.


Final lifecycle follow-up passes 1,231,853 assertions / 22 Vulkan cases in
portal-shadow-pair-lifecycle-verified-gpu.log. The first run had four failures in
one ResourceImage capacity test: an earlier generated-group setup still left
two free slots while expecting refusal at the former five-slot limit. Its stale
oversized-group check also used six entries. The fixture now leaves one slot
free for atomic-group refusal and uses seven entries for oversized admission.
No production change was needed. The first result is retained in
portal-shadow-pair-lifecycle-gpu.log; the test-only rebuild passed in
portal-shadow-pair-capacity-build.log. The run includes the existing shared
transfer-budget, cancellation, idle reclaim, native-domain and directional
response checks. Known transparent-layer coplanar/intersection failures remain
outside this focused filter, unchanged and unresolved.


## Retained requester routes, host policy and charged shadow assembly

Each retained source capture now reserves a delivered route record before GPU
admission. Four records per producer each hold at most sixteen accepted target
eyes, with bounded endpoint/key strings. They bind the exact parent requester
and eye to the original direct-child requester, actual transport endpoint and
accepted child-root eye. ResolveShadowRoute returns these owned values only
from a delivered record, after authorization and current endpoint/published
identity checks. The local target resolves to the original requester and
producer endpoint. Taking a map does not destroy its child route record.
Records expire one second after eye delivery; Clear, policy changes and endpoint
retirement release them. Retirement of any recorded descendant invalidates the
whole record. ShadowUsage reports route ownership and conservative metadata
bytes separately from the two-map CPU reservation.

PortalImageHost::SetRetainedBodyAuthorization installs an owned trusted policy
for a local world, applying it to both current and future producers. The default
remains denial. Replacing or removing policy clears affected capture work.
Policies and producer entries pin ecs::Store::Identity, so a reused WorldId or
same world name cannot inherit old authorization. Retired entries are pruned;
policy count is bounded by the existing maximum producer count. RemoveWorld and
Clear discard policies. No game-specific authorization is inferred or installed
by this API.

Renderer-owned tile assembly is now attached to the pending serial tree job.
BeginPortalCaptureTreeShadowAssembly validates the manifest with the existing
exact matcher before allocating. Its 16 MiB owner is charged against the same
PendingCpuBytes used by eye and shadow imports. AcceptPortalCaptureTreeShadowTile
requires the exact current job/node. The final tile tries to import and compose;
BudgetExceeded retains the completed map and charge, and
CommitPortalCaptureTreeShadowAssembly retries without replaying the tile.
Successful import transfers the charge once, with no yield between owners.
Cancellation, tree/world retirement and shutdown release assembly ownership;
submitted GPU resources retain their existing fence lifetime. The direct
full-image acceptance API remains available when no assembly is active.

Verification:

- Builds pass in portal-shadow-routes-build.log and
  portal-shadow-routes-final-build.log. Existing aggregate warnings remain.
- Headless render passes 136,501 assertions / 506 cases in
  portal-shadow-routes-verified-headless.log. New host tests send actual owned
  requests and check current/future policy forwarding, replacement/removal,
  served/unserved world-slot reuse, configuration bounds and capacity recovery.
  The first headless run had six failures in two new cases because the fixture
  selected Eye projection while leaving the default nonzero seam clip plane.
  Setting the required zero clip plane fixed the setup; the encoder remains
  unchanged. portal-shadow-routes-headless.log retains the first failure.
- Offscreen dev Vulkan passes 179,651 / 21 in
  portal-shadow-routes-initial-gpu.log. The later rebuild changed only the
  headless host fixture. The nested source test proves root-requester refusal at
  the child, original nested requester recovery, root-map take without route
  loss, exact eye/endpoint checks and child withdrawal. Leaf tests cover
  pre-delivery refusal, expiry, four route records independent of map ownership,
  fifth-route refusal before rendering and Clear reclamation.
- The native retained-tree lighting oracle now exercises real tile assembly,
  duplicate manifest admission, stale job/node and damaged tile refusal,
  cancellation, shared CPU pressure, completed-map texture pressure, commit
  retry and final native colour parity. Existing runtime, host GPU, tree import
  and composition cases are included. No image tolerance changed.
- Formatting and whitespace checks pass. No release performance measurement,
  sanitizer, Metal or interactive Studio check was performed for these changes.

Next: connect these APIs through bounded authenticated shadow control messages,
manifest/tile demultiplexing and PortalImageHost composition begin/poll/cancel
scheduling. Forward pulls through the retained route chain; never take a nested
map under the root's requester identity. Keep one outgoing map/tile and one
charged receiving assembly per serial job. Exercise actual multi-hop relaying,
transport Full/retry, cancellation and endpoint replacement. Current tests prove
the two-node requester boundary, not complete distributed transport. Existing
maps retain their original exact domain; changed-domain old-eye refitting still
requires pinned source asset revisions or explicit full eye refresh/refusal.
The end-to-end host flow and full render plan remain unfinished.

### Authenticated shadow transport and host composition follow-up

SHCT control messages now carry bounded manifest, tile and cancellation pulls.
Successful replies must match the exact target producer, accepted eye and tile;
the authenticated presentation envelope supplies requester identity. No requester
override crosses the wire. Canonical SHDW validation checks tile metadata, hashes
and samples without allocating a full receiving map. The existing 4 MiB message
bound and 16 MiB original shadow map remain unchanged.

Producers serve their original retained maps and relay descendant pulls through
the recorded immediate requester chain. Five bounded transfer contexts allow a
producer to reappear in a capture path. They share a 4 MiB outbound wire budget.
Lost replies, Full and BudgetExceeded retry under a fixed ten-second deadline;
duplicate child pulls preserve correlation and do not renew that deadline.
The source separates shadow replies from ordinary image completions in one
bounded mailbox. The renderer retains ownership and charging of tile assembly.

PortalImageHost now exposes BeginBodyComposition, PollBodyComposition and
CancelBodyComposition. It copies the current body into the renderer job, pulls
each exact manifest and tile, retries assembly admission and commit, and returns
one host-owned result. Completed compositions retain their capture and route for
another body pose. Retirement, cancellation and replacement release ownership.
The source capture lease is fixed at its first pin, at most ten seconds, while
the original image expiry stays intact. A fresh eye request can remain pending
beside retained pixels; accepted replacement retires the old lease. Repeated
composition does not renew it. Cancellation targets the captured published root
identity while transport authentication uses the actual carrying endpoint.

Host cancellation uses a bounded queue and retries the existing Part17 reply for
one second. Producer cancellation releases local ownership and dispatches child
cancellation. It does not confirm descendant reclamation; fixed expiry remains
the fallback when a child cannot admit that control message.

Verification:

- The integration fixture first reproduced two production failures: slow
  transfer lost the source eye at its ordinary one-second expiry, and completion
  cancelled the map needed for a second pose. Both are fixed by the leases above.
  portal-shadow-transport-lifetime-gpu.log retains those failures.
- Final dev build passes in portal-shadow-transport-final-build.log. The first
  lease build found a shifted positional Preview initializer; named fields fixed
  that compile error. Existing missing-field warnings remain.
- Headless render passes 137,818 assertions / 509 cases in
  portal-shadow-transport-lease-headless.log.
- Offscreen dev Vulkan transport passes 1,161 assertions / one generated case
  in portal-shadow-transport-verified-gpu.log. Its nine scenarios cover a lost
  manifest pull, three distinct rooms, slow transfer, repeated composition past
  ordinary expiry, A-to-B-to-A producer routing, cancellation during charged
  assembly, a full reply mailbox, a fresh request before pinning and a fresh
  request after pinning. Repeated composition expires at the original deadline.
  Completed paths check visible output, exact shadow upload bytes and actual
  presentation byte traffic. Cancellation and retirement check CPU reclamation.
- Earlier fixture failures were corrected without changing image tolerances:
  opaque-lighting output needs the matching eye viewer scope, leaf host requests
  need recursion enabled to obtain a capture tree, and the host combines camera
  revisions before sending them. Logs preserve those initial failures.
- Existing runtime, host and native tree lighting GPU checks pass 177,568 / 18
  in portal-shadow-transport-lifecycle-gpu.log. Two requested tag names matched
  no cases; a separate corrected run covers tree import and composition, passing
  2,082 / 3 in portal-shadow-transport-tree-gpu.log.
- Formatting checks pass. No release GPU performance, sanitizer, Metal or
  interactive Studio verification has been performed for this transport slice.

Next: connect trusted game authorization and current-pose composition into the
product loop. Automatic depth-zero layer-to-tree admission, changed-domain eye
refresh policy and complete disconnect/restart cancellation coverage remain
open. Old source asset revisions are still not pinned; changed-domain requests
must refuse or obtain a full new eye, never redraw an old map from current assets.
The full R01-R17/P0-P12 render plan remains unfinished.

### Depth-zero retained body admission and product audit

Authenticated retained-body layer replies now become one-node imported trees at
the receiving source. The node uses the pending capture camera/projection,
published producer identity and exact retained player from the admitted request.
It uses the existing tree upload, atomic publication, pin and retirement paths.
Ordinary ordered layers keep their existing flat import and PIMG is unchanged.
Host shadow composition therefore no longer needs a nonzero recursion depth for
a leaf. The transport fixture now requests depth zero in all seven leaf scenarios.

Verification:

- Dev build passes in portal-shadow-leaf-fixed-build.log. The first build found
  an unqualified duration literal in the new test; std::chrono::seconds fixed it.
- Headless render passes 137,818 / 509 in portal-shadow-leaf-headless.log.
- Offscreen dev Vulkan passes 1,618 / 4 in portal-shadow-leaf-gpu.log, covering
  the nine transport scenarios, ordinary and retained leaf admission, existing
  flat layer publication and recursive tree publication. New checks verify
  camera, producer, request and retained identity, pre-upload invisibility,
  capture pin admission and complete cleanup.
- Formatting and whitespace checks pass. No release performance, sanitizer,
  Metal or interactive Studio run was performed for this change.

The product audit found prerequisites beyond the call-site swap. Scene.cpp still
uses EyePlayer and synchronous composition. Demand collection forces depth zero
and does not request RetainedBodyPlayer; whole-eye submission is separate.
Server connection/player/receipt ownership is not carried by PresentationBindings,
so a supervised producer cannot currently derive an authoritative exclusion grant.
World-local numeric UserId equality cannot authorize unrelated authorities.

The current async job copies its pose at Begin, then fetches maps across multiple
round trips. Its completed image is not proof of current-frame avatar motion.
Fresh accepted eyes can also cancel every in-flight job, and each completed node
releases its imported shadow texture, so another pose fetches again. Fair host
scheduling, current-pose presentation, ownership grants/delegation, and first-person
shadow/child-view rig preservation must be addressed before enabling the new
product path. RENDER-REFACTOR-TASKS.md records concrete integration and acceptance
requirements. No product call was switched to a path that lacks those guarantees.

### First-person retained rig and receipt ownership follow-up

Retained-tree composition now accepts root primary visibility selection without
dropping the rig rows. The asynchronous job owns EyeRig, EyePlayer and validated
sorted hidden-row indices, including their metadata charge. Root composition uses
that selection; child composition clears it. Both synchronous and asynchronous
paths retain all body geometry for directional fitting, shadow casting and child
views. Invalid, duplicate and unsorted hidden indices refuse before submission.

PresentationPeer::OwnsReceipt provides the exact live alias ownership lookup
needed to join a presentation requester to its authenticated server connection.
It refuses another peer's receipt, original client addresses, withdrawn or closed
receipts, and externally replaced endpoint incarnations. It does not itself grant
body exclusion or propagate policy to a producer.

Verification:

- Dev world and render builds pass in portal-receipt-owner-build.log and
  portal-first-person-shadow-build.log.
- All world tests pass 32,650 assertions / 249 cases in
  portal-receipt-owner-world.log, including receipt isolation and retirement.
- Headless render passes 137,818 / 509 in
  portal-first-person-shadow-headless.log.
- Offscreen dev Vulkan passes 8,008 / 3 in portal-first-person-shadow-gpu.log.
  The native retained lighting oracle adds first-person rig and explicit-index
  scenarios, checks a visible-versus-hidden native difference, preserves child
  lighting/shadow comparisons, and destroys caller selection storage after Begin.
  Existing tree composition and all nine transport scenarios also pass.
- Formatting and whitespace checks pass. No image tolerance changed. No release
  performance, sanitizer, Metal or interactive Studio run was performed.

The authorization investigation found usable transfer provenance:
PortalTransferId identifies source world, incarnation and sequence; the source
can resolve an outgoing receipt from its exact player, and the destination can
resolve the committed live player from that receipt. Destination reservation also
binds the connecting authenticated identity. This mapping must scope grants;
world-local numeric player equality is not a substitute. Copied geometry currently
carries scalar Player identity only and loses foreign player identity on re-encode,
so grant transport alone cannot preserve ownership through nested copies.

Next: implement trusted grant transport and scoped copied-row ownership, then
separate immutable shadow preparation from current-pose composition. Imported
shadow textures are still released per node and fetched again for each job.
Product scheduling, continuous eye replacement, latest-frame motion and the full
render plan remain unfinished.

### Packed source shadow import foundation

PortalShadowPacked defines an exact local depth representation with 64 consecutive
samples per block. Two descriptor words hold a minimum bit pattern and an absolute
payload offset/bit width. Integer deltas pack least-significant bits first;
constant blocks have no payload. Encoding measures output before allocation.
Decoding validates the complete canonical layout, offsets, minimal width, sample
range and exact end before allocating. CPU encoding/decoding preserves all accepted
normalized D32 bit patterns, including positive subnormals, and refuses budgets
transactionally. The network SHDW representation is unchanged.

Renderer::QueuePackedPortalShadowImage retains eligible maps in a GPU storage
buffer. The existing shadow graph node decodes into native D32 scratch, then loads
current body casters. The map remains reusable across poses without another upload.
Packed buffers share the existing import GPU byte ceiling; pending vector capacity
and raw-plus-packed construction peak share the CPU ceiling. Upload staging keeps
its existing bound. TextureBytes reports logical import payload including these
buffers. Upload and decoder draw counters account for actual work. Raw imports
continue through their original copy path. The bounded shadow handle table now
allows a displayed tree and replacement candidate, subject to shared byte admission.

The GPU subnormal test found a real representational limit: fragment depth output
flushes positive subnormal values to zero on the tested Vulkan device. Packed import
therefore detects those values and uses the existing raw D32 texture transfer,
preserving exact bits within the same budgets. It does not quantize or reject an
otherwise admissible depth map. portal-packed-subnormal-gpu.log retains the failed
direct-decoder test; the production fallback now checks that same input family.

Verification:

- Dev builds pass, including portal-packed-final-build.log.
- Headless render passes 138,027 assertions / 516 cases in
  portal-packed-verified-headless.log. Seven new codec cases cover constants,
  widths, block boundaries, independent bit packing, randomized values, hostile
  layouts, invalid samples and exact budget/refusal ownership.
- Offscreen dev Vulkan passes 108,424 / 5 in portal-packed-verified-gpu.log:
  standalone packed reconstruction, raw/packed native shadow and colour parity,
  multiple moving poses without another map upload, exact subnormal fallback,
  retained first-person/nested lighting and all nine transport scenarios.
- After adding the decoder draw counter, the focused import rerun passes
  101,801 / 2 in portal-packed-final-gpu.log. Formatting and whitespace pass.
- No release timing, full-tree pose performance, sanitizer, Metal or interactive
  Studio run was performed. Storage correctness and transfer reuse are verified;
  frame-rate suitability is not yet measured.

This is the import/decoder foundation. Serial tree jobs still use the existing
per-node transfer path. Next connect whole-tree preparation and admission, an
independent prepared-tree lease, all-node current-pose domain validation and atomic
replacement before changing host/product scheduling. Trusted grants and scoped
copied-row ownership remain open, as does the full render plan.

### Prepared capture-tree verification

Prepared capture trees now retain child-first packed source shadows and their tree
lease before a current pose arrives. Prepared composition validates every node's
current body domain before submission, records the body pose as one queue-ordered
operation, and reuses the retained source maps across poses without further shadow
transfers. Owner release keeps the leased tree usable until preparation retirement.
Root primary selection still hides only the visible root copy, leaving full rig
geometry available to directional fitting, shadows and child views.

Prepared maps use dedicated staging buffers only while preparation owns them.
Ordinary source shadows continue to use the shared portal staging buffer. The
shared buffer capacity is tracked separately from aggregate staging use, so a
normal eye-layer upload and a full raw D32 shadow can coexist without either
incorrectly borrowing the other's allocation.

Verification on the final dev binary:

- `ninja -C .cache/build/dev test_render -j2` linked successfully. The build log
  is `.cache/build/dev/tests/portal-prepared-build.log`.
- Offscreen Vulkan `[shadow-import],[portal-tree-lighting],[portal-tree-import]`
  passed 109,782 assertions in 6 cases. This covers raw and packed source imports,
  native retained-room parity, prepared multi-pose zero-upload reuse, domain
  refusal, replacement ownership and first-person root hiding.
- Headless `[render]~[gpu]` passed 115,936 assertions in 414 cases.
- Offscreen Vulkan `[portal-shadow-transport]` passed 2,440 assertions in 4 cases.

Product host scheduling, trusted grant propagation and scoped copied-row ownership
are still required before changing the client path. Release full-tree pose timing,
sanitizers, Metal and an interactive Studio run remain outstanding.

### Exact-domain source-prefit verification

Host preparation now sends each renderer-validated node body bound through the
owned SHCT route. Producers deep-own the accepted source view, render an immutable
shadow prefit for the exact source-plus-body domain, and return retryable pressure
until its readback is ready. A resource-epoch change cancels the fit; stale depth is
never retagged under a new light matrix. Nested relays retain their authenticated
parent/target identities while their per-node bounds remain exact. Host upload
readiness includes pending producer fits, so callers submit the readback render.

The bounded lease fixture holds two preview leases and two real preparation leases,
then rejects a fifth owner. It also checks hard retirement. The transport fixture
covers nested transfer, malformed and changed bounds rejection, fixed expiry,
endpoint withdrawal, delayed fit transfer, latest-pose composition, repeated-pose
zero network and shadow upload, and no mixed generations.

Final dev verification used the final source snapshot:

- `ninja -C .cache/build/dev test_render -j2` linked successfully.
- Offscreen Vulkan `[portal-tree-lighting],[portal-tree-import],[shadow-import],[portal-shadow-transport]`
  passed 113,263 assertions in 10 cases.
- Headless `~[gpu]` passed 138,031 assertions in 516 cases.

Logs are appended to `.cache/build/dev/tests/portal-prepared-build.log`. Trusted
grant propagation, scoped copied-row ownership, client product scheduling and
fairness remain open. Release pose timing, sanitizers, Metal and an interactive
Studio run remain unmeasured.

### Caller-driven preparation fairness

Preparation tickets retain only an accepted portal identity, tree and eye key. The
caller supplies its reference body only when the FIFO head is ready. A stale ticket
refuses rather than substituting a newer capture; callers cancel it and enqueue a
new tail ticket. Cancellation is the explicit invisible-demand withdrawal.

Prepared source maps can be released independently after composition, retaining an
already displayed body image while freeing one of the two preparation slots. The
transport fixture checks FIFO head selection, non-head waiting, explicit ticket
cancellation, exact queued-start identity, three-ticket progress after release,
and repeated latest-pose zero transfer reuse.

Final dev checks: offscreen Vulkan `[portal-shadow-transport]` passed 3,499
assertions in 4 cases; headless `~[gpu]` passed 138,030 assertions in 516 cases.
The build log is `.cache/build/dev/tests/portal-prepared-build.log`. Product grant
propagation, client scheduling and release performance validation remain open.
