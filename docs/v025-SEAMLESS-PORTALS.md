# v0.25 seamless portals plan

## Purpose and definition of done

This plan makes a portal behave as a continuous opening for an ordinary moving body, including a player who stops with their torso on the seam. It covers drawing, camera routing, authority transfer, client prediction, replication, and physics. The implementation is in progress. The evidence below records completed checks and current failures, rather than claiming acceptance of the full plan.

### Implementation and acceptance status, 24 September 2026

The scene crossing fix and deterministic capture pacing have focused regression coverage. The 90-frame front-crossing matrix was captured at requested 30, 60, 144, and 240 FPS and at 1080p and 4K. Matched fixed ticks and strict image comparisons passed in that matrix. The lighting-stable body mask reports zero uncovered and zero duplicate samples across the 90 matched pairs; an earlier positive duplicate count was a mask false positive caused by a bright near lamp. The requested 240 FPS row achieved only 60.58 FPS on the measured portal run. Keep requested frame rate and achieved frame rate separate.

The process transfer impairment fixture covers the configured RTT, jitter, loss, duplication, and reorder profiles, with injected fault counts recorded. A threaded-world byte-parity case also passes. Those protocol results do not establish the product's visual continuity or frame-rate targets. Release profiling has counters for uploaded and decoded bytes, queue depth, image age, and handoff samples; the full quiet-host CPU and GPU cost comparison at 1080p and 4K remains open.

The process-hosted product walk remains failing. The immutable 30/60 run at `.cache/portal-profile/remote/runs/20260924-1051/` contains its log, capture sidecars, and representative adoption frames. A focused render regression for a delayed complete-world capture with one seam and both image and radiance children passes (57 assertions). Both product rates now Commit and adopt in both directions. The first approach image remains present across Transfer, and the first crossing no longer fails the yellow-body or capture-continuity assertions. At first adoption, however, 30 Hz blue floor pixels fall from 7,068 to 74 and yellow body pixels from 194 to zero; 60 Hz falls from 6,613 to 319 blue and 259 to zero yellow. The 60 Hz return still has a separate black interval. Retaining the mapped old camera route indefinitely was tested in `.cache/portal-profile/remote/runs/20260924-1054/` and regressed transfer and camera motion, so that experiment was reverted. Product timings are contended functional evidence only. This plan is not accepted until the visual gaps and remaining performance and fault matrix checks pass.

"Seamless" means every visible point of a crossing body is drawn in its chart at one presentation time. The pictures meet without uncovered or duplicate opaque samples, pose-time disagreement, camera discontinuity, or ownership pop. The simulation has one canonical body and authority. The second visible half is derived presentation only.

This scope supports `Part`, `MeshPart`, `EditableMesh`, skinned meshes, tools, rigid assemblies, stationary animation, independent first and third-person camera routing, remote observers, motion, rotation, and uniform scale.

It does not promise that a retained remote image can conceal missing replication, missing assets, or an arbitrary network outage. A portal can look distant through a bounded retained image. A portal that a body can traverse must have live local presentation data near the seam. If that data is unavailable, the system must clearly remain in an image-only state and refuse the seamless traversal promise.

The existing render cleanup plan remains the owner of render-pipeline extraction and portal coordinator cleanup. This document adds the cross-module contracts that the cleanup needs. See [v025-RENDER-PIPELINE-CLEANUP.md](v025-RENDER-PIPELINE-CLEANUP.md).

## Evidence and working model

The following observations describe current code. They are not claims that the current implementation already meets the target.

| Area | Observed evidence | Consequence for this plan |
|---|---|---|
| Seam mapping | [`SurfaceCameras.hpp`](../mono.engine/scene/include/engine/scene/SurfaceCameras.hpp) provides `SeamTransform`, `SeamCut`, and portal body view state. `SeamTransform` maps points, directions, rotation, and motion with uniform scale. | Reuse this mapping. No portal path may invent a second transform convention. |
| Camera handoff | [`CameraPortalView.cpp`](../mono.engine/scene/src/CameraPortalView.cpp) already keeps route world, path, input map, signed crossing, edge-on waiting, arrival tolerance, and reversal guards. | Camera routing remains independent from body ownership. It needs a shared presentation-time contract. |
| Body draw splitting | [`PortalGeometryDraw.cpp`](../mono.engine/render/src/PortalGeometryDraw.cpp) derives near and far draw halves from current rows and one skin palette, with complementary cuts and far tag suppression. `PortalImageDemand` calls it in production. | Validate the required drawable set and clip paths against the acceptance matrix. |
| Same-world ordinary body split | `FarHalfOfRow` in [`SurfaceCameras.cpp`](../mono.engine/scene/src/SurfaceCameras.cpp) already derives far halves for ordinary anchored crates, meshes, and skinned limbs with rig-wide fit. | Preserve this proven split meaning while unifying it with the production cross-world draw route. |
| Production portal geometry | `PortalImageDemand` calls `SplitPortalBodyDraws` in production. `PortalImageRuntime` appends wire geometry through `AppendPortalDraws`; `ForwardPortalDraws` forwards nested demand geometry. | Verify the production route against every required drawable and clip path. |
| Shader and depth clipping | Portal fragment paths already discard by `SeamPlane`; the SDL pipeline uses oblique near clipping. [`RenderPipelines.cpp`](../mono.engine/render/src/RenderPipelines.cpp) contains the depth clipping setup. | Main depth, shadow, motion-vector, alpha, and compositing paths must share one clip contract. |
| Capture time | Portal capture currently uses destination `WorldFrame.Seconds`, while the client view is presented independently. | Crossed halves can be sampled at different times. Shared presentation time is required before visual correctness can be evaluated. |
| Physics | [`Portals.cpp`](../mono.engine/physics/src/Portals.cpp) creates bounded static proxy bodies. [`PortalIsland.cpp`](../mono.engine/physics/src/PortalIsland.cpp) solves copied dynamic island contacts through [`PortalSeamCoordinator.cpp`](../mono.engine/game/src/PortalSeamCoordinator.cpp). | Validate dynamic contacts, materials, and moving mouth cases across threaded and process-separated worlds. |
| Authority transfer | [`PortalTransfer.cpp`](../mono.engine/script/src/PortalTransfer.cpp) already has Offer, Ready, Commit, Done, and Cancel with source and destination incarnation fencing. It removes source simulation before destination admission in the current path. | Extend the protocol with preparation and a fenced handoff baseline. Do not replace it with a new unrelated transfer system. |
| Client continuity | [`ClientPortal.cpp`](../mono.client/src/ClientPortal.cpp) prepares destination connection and replica, retains prediction around source removal, and continues ordered inputs. | Build on its prewarm and continuation path, but define exact baseline, clock, and reset rules. |

The existing `CutOfSeam` fit check is a useful broad guard, but refusing every wide mesh is not the final answer. A shoulder, tool, or limb may overlap a finite opening without the whole bounds fitting. The final finite-aperture mask keeps a source sample unless it is behind the seam plane *and* inside the transfer volume. The destination is its complement, so unrelated overhang is not sliced.

## Invariants and state

Each logical body has an explicit, serialized, globally unique `BodyKey` and lifetime generation. Account identity does not identify a spawned body. Rename and reparent preserve its key; clone and rebirth mint one. The protocol refuses live-key collisions between body generations. A key is never a declaration-order number, ECS entity number, component ID, or object path. A draw identity names `BodyKey`, part or submesh, and chart route. A crossing body has one canonical simulated assembly, script owner, input consumer, physics owner, and authority epoch. Its visible halves are derived presentation rows.

The ECS crossing state references the existing canonical `Transform`, `PreviousTransform`, `Motion`, and animation components. It adds only portal-specific facts: `BodyKey`, assembly reference-anchor rule, topology revision, authority epoch, active seam, crossing phase, and monotonically increasing presentation revision. It must not duplicate poses, velocities, or animation state in a second component. Renderer caches contain copied presentation rows keyed by that revision. They cannot become a second source of simulation truth.

Four independent conditions must remain distinct:

| Condition | Definition | Effect |
|---|---|---|
| Volume overlap | The swept geometry or animation envelope intersects the finite transfer volume. | Draw source and destination presentation halves. |
| Reference crossing | Root, centre of mass, or named assembly reference crosses the ownership chart plane. | Begin or finish authority transfer according to the protocol. |
| Eye crossing | The camera eye crosses its route plane. | Route the camera view independently. |
| Full clearance | No geometry, tool, trail source, or relevant effect overlaps the old chart transfer volume. | Retire the old presentation half after its safe fence. |

Standing halfway is a normal steady state.

| Moment | Body and presentation | Camera | Authority |
|---|---|---|---|
| A. Approach | Source body is whole. Destination live view prewarms. | Eye is source-routed. | Source owns. |
| B. Limbs enter | Finite mask draws source and destination halves from one pose. | Eye may still be source-routed. | Source owns because root is still source-side. |
| C. Root crosses | Far native presentation replaces only the transferred region. The trailing source half remains until full clearance. | Eye can be on either chart. | Fenced handoff makes destination owner at `H`. |
| D. Stop halfway | Both halves persist, including sleeping bodies and animated limbs. | Third-person eye may remain in the opposite chart. | Destination owns exactly once. |
| E. Clear or reverse | Retire the old half only after all relevant geometry clears. A post-Commit reversal is a new crossing. | Route changes independently at eye crossing. | Remains destination until the new fenced crossing. |

The reference anchor must be explicit for each assembly type. A humanoid uses its declared root. A rigid assembly uses its centre of mass or stored assembly root. A tool follows its owning assembly unless it has a separate physical assembly and persistent identity. This avoids treating a limb, mesh bound, or camera offset as the authority decision.

At zero signed distance, the body remains in its current chart indefinitely. Crossing requires hysteresis, direction from the prior stable side, and transfer-volume intersection. Jitter cannot flip ownership. Reversal before Commit restores pre-transfer state; reversal after Commit is a new crossing. These rules cover root-first limbs and camera-first bodies.

Initial release supports one active seam per connected body. If a connected body intersects a second seam while already split, it blocks the later seam deterministically at its rim and reports the limiting seam identity. This is an explicit scope boundary, not accidental undefined behaviour. Supporting intersecting seams later requires a bounded partition of the body into chart-labelled regions, deterministic ordering by portal identity, and one solver island spanning every involved chart. Do not claim arbitrary overlapping portals before that partition exists.

A split has no timeout. A sleeping or stationary body without `Motion` remains split while it overlaps the opening, subject only to resource count and byte budgets. It retires only at actual full clearance. Active topology pins endpoint pairing and scale revision while overlap exists, but endpoint poses continue to be sampled from `T(t)` at every fixed step and presentation time. Close or retarget requests defer while occupied. A forced removal makes an explicit authority decision to evacuate or suspend the body and records that decision; it never silently drops one visible half.

## Rendering contract

### One pose, two chart images

For every view, evaluate the canonical body pose, previous pose, animation palette, endpoint transform, aperture shape, clip planes, material state, and presentation timestamp once. `SplitPortalBodyDraws` or its replacement then derives two draw rows from those values:

1. The source row uses the source chart transform and the source half of the finite aperture mask.
2. The destination row maps the same pose through `SeamTransform` and uses the complement of that mask.

At the mathematical seam boundary, one documented tie owner wins, for example source on zero before reference commit and destination on zero after it. The tie rule is identical in CPU culling, shader clipping, depth, shadow, and motion-vector paths. It prevents coverage holes without putting a different small clip bias on each half. Independent clip biases visibly erode the body or create double coverage at close range.

The aperture test is narrow phase. Broad phase may use swept bounds for candidate pairs. A point transfers only when behind the source plane and inside the finite transfer volume. The source complement is non-convex: it keeps a sample in front of the seam *or* outside the footprint. It cannot be one intersection of enabled clip planes. Use a fragment logical mask or region partition. Map the destination complement from the same decision. Mesh, skinned, and editable paths may differ, but must produce that rule.

The sweep includes root movement, rotation, joint animation, and tool motion. Bounds alone miss a rapidly moving limb. Broad phase may conservatively include animation bounds; the narrow clip decides samples. Tests cover a large plank, wide character, and rotating tool entering a smaller aperture.

Connect the current draw splitter to production before claiming this behaviour. `AppendPortalDraws` and nested `ForwardPortalDraws` carry the same split rows, palette, clip metadata, and timestamp. No capture path calculates a second pose. Expand surface and variant limits through opaque, material, transparent, and effect rows. Unsupported rows diagnose and block seamless traversal.

### Camera, depth, lighting, and temporal history

A routed camera uses one optical lens definition, jitter sample, presentation time, and route revision for both sides. Derive each chart's view and clip projection from those shared inputs, since the destination oblique matrix necessarily differs. Its aperture mask survives an eye on or near the seam without a depth-changing thick slab. Use analytical ray-plane or clipped screen-space masking. For an eye-on-plane or parallel ray, guard the denominator and keep the last valid routed side. Use finite plane clipping while oblique projection is unstable near the plane.

SDL uses zero-to-one depth and Y-up conventions. Any adaptation of Eric Lengyel's oblique near-plane technique must use the backend's clip convention and validate the resulting near and far depth behaviour. The paper describes modifying a projection for an oblique near plane, but its OpenGL-oriented formula cannot be copied unchanged into this backend. See [Lengyel, *Oblique View Frustum Depth Projection and Clipping*](https://terathon.com/lengyel/Lengyel-Oblique.pdf).

Imported portal depth is reconstructed by the exact projection that wrote it into a destination-chart world position `xB`. To compare it with a source-chart host view, map `xB` through the inverse seam `xA = T_AB^-1(xB)`, then apply the current host view-projection `P_host * V_host * xA`. Comparing imported depth directly to host depth is invalid because they are values in different projections. Opaque depth, fragment clipping, shadow casting, shadow receiving, transparent sorting and depth testing, motion vectors, and picking must use the same chart and finite-aperture ownership decision.

Shadows need both halves of a crossing caster in the respective chart. The retained-body tests already cover some geometry and shadow behaviour, so extend them rather than asserting shadows are absent. A receiving surface may have deliberately different authored light on each side. A matched-room reference scene must still show no artificial seam attributable to portal rendering. HDR images compose before the one exposure and tone-map operation. Do not tone-map each portal image separately.

Temporal AA history is chart-labelled. Transport a body's previous pose and prior chart through the seam when the route remains valid. Invalidate only the changed route, view, or portal topology revision. Never blend positions from unrelated world coordinate systems. Local deformation, material animation, and skin motion use the same current and previous palette on both halves.

Particles, trails, and transparency are part of the promise only when their emitters and sample geometry use the same chart, clip, depth, and time contract. Initial implementation may admit a bounded supported subset, but an unsupported live effect must report why seamless entry was declined. Do not render it once per chart and hope alpha hides duplication.

### Time and visibility

Every draw in a portal composition uses an explicit `PresentationTime` selected by the host view. It maps to source and destination simulation time through declared world clock mappings. A capture cannot silently take destination `WorldFrame.Seconds` while the host camera is presented at another time. The draw packet stores its chosen time and snapshot revision for diagnostics and test assertions.

A local player uses one predicted own-pose for both halves. A remote body uses the same interpolation delay and clock mapping for both halves. Crossing interpolation is piecewise at the crossing fraction or time of impact: interpolate within the source chart to the boundary, map the boundary pose through the seam, then interpolate the remaining segment in the destination chart. Equivalently, map both bracket poses into one chart before interpolating that segment. It never linearly interpolates source-world coordinates with destination-world coordinates.

When the body is near enough to traverse, prewarm a local presentation replica and required assets for a bounded far region. The region covers the current portal frustum, the full expected frustum immediately after the eye crosses, the return portal where visible, and a motion and disocclusion margin. The spatial bound is aperture extent plus body reach plus `maximumSpeed * (networkBudget + loadBudget)`. Promotion from retained image to live geometry needs a readiness record: required replica baseline, topology revision, asset revision and residency, pose time range, and capacity reservation. Use entry and exit hysteresis so small motion does not swap between image and live mode every frame. A stale retained image is permitted only for distant, non-traversable viewing. It excludes matching logical bodies to avoid duplicate geometry and shadows.

There is no synchronous GPU readback on the frame path. Capture, asset upload, remote decode, and compositing use bounded queues, retained resources, and completed-result polling. Record capacity, bytes, staleness, and dropped work. On budget exhaustion, reduce portal detail or remain image-only according to an explicit policy, then state that seamless traversal is unavailable.

## Authority, replication, and prediction

The existing Offer, Ready, Commit, Done, and Cancel exchange remains the transfer backbone. Add a `Prepared` phase after destination authority readiness and before the irreversible decision. The destination creates inactive rows from a copied baseline but does not simulate or publish them as authority. `H` is a named tuple of clock domain, seam step, source tick, and destination tick. Source authority owns simulation for `t <= H`; destination owns `t > H`.

The initial coupled-crossing capability supports worlds with the same fixed timestep and one shared ordered seam barrier. Unequal world rates are unsupported until a separate scheduler substep feature defines exact clock conversion and barrier ordering. Do not describe rational scheduling as available before that feature exists. `world/src/Universe.cpp` currently runs complete `World::Tick(OwedList)` batches and joins them; its bus frame barrier is not an intra-physics seam barrier. The required scheduler and physics refactor collects copied pre-solve state from both worlds, solves the coupled island once, applies both result records before either world advances or publishes the affected physics phase, and exposes only opaque records through generic world hooks.

The current receiver rejects changed Offer bytes for the same receipt. A fresh final state therefore cannot mutate the original Offer. Before Commit, add a versioned `PrepareRevision` and `SealBaseline` exchange with a monotone baseline ID and expected prior revision. The source seals a final authoritative baseline at `H`; the destination revalidates it, records its baseline hash, and acknowledges the sealed `H` baseline. Commit contains transfer string ID, source and destination world IDs, source and destination incarnations, topology revision, authority epoch, baseline ID, baseline hash, and final source receipt. A stale prewarm cannot activate.

The source continues simulating throughout `Prepared`. After it seals the final snapshot, it holds the affected island at `H` until Commit or Cancel, so no further state can appear between sealing and adoption. A Cancel releases the hold only through the safe pre-Commit re-solve path.

Durable decision ownership is new work. The game portal seam coordinator owns a decision journal through an existing storage adapter or an explicitly added persistence adapter. Before it sends Commit or fences source authority, it persists `{transferId, BodyKey, incarnations, H, baselineId, baselineHash, authorityEpoch, decision}` and the sealed final baseline. Recovery freezes the affected body until that authoritative journal outcome is known. Replaying a committed source never resumes source authority; the destination activates exactly once under the stored epoch; and a tombstone stays until every configured recovery and replication-retention fence has passed. Pre-Commit cancellation may return from the wait only after a safe re-solve. Post-Commit failure never restores the source from a timeout.

The handoff baseline contains the complete canonical body state needed to continue one body:

| State | Reason |
|---|---|
| Persistent identity, transfer ID, incarnations, topology revision, authority epoch | Deduplicate reordered messages and prevent stale rows deleting newer incarnations. |
| Current and previous transform, velocities, angular velocities, seam chart, clock mapping | Continue presentation and fixed-step motion without a coordinate discontinuity. |
| Animation state, palette inputs, tool and attachment state | Make both visible halves use one pose. |
| Sleep state, support/contact keys, material and collider state | Preserve contact and wake behaviour through the boundary. |
| Last consumed input sequence and bounded input history | Consume each input exactly once after ownership moves. |
| Complete component baseline and codec revision | Apply a self-contained row before deltas reference it. |

Snapshot deltas name authority epoch and baseline ID. A receiver refuses a delta before its baseline, from an old epoch, or from a mismatched incarnation. Commit, spawn, despawn, and receipt handling is idempotent. In particular, an old source tombstone cannot remove a newly admitted destination row with the same logical identity. Presentation may keep an inert old row until the destination presentation baseline is admitted, but it cannot run scripts, physics, or input.

Input must become a bounded ordered per-tick segment, rather than relying only on the current latest-value-and-jump forwarding path. The source records a consumed-through sequence acknowledgement; the destination receives a reject-through fence and consumes only the remaining sequence once. Late in-flight source inputs are forwarded with their original sequence. Retransmit changes delivery, never reapplication. Discrete action events carry the same deduplication key. Retain bounded history for replay in either direction. If the history window is exhausted, correction resets to the committed baseline with a visible diagnostic rather than pretending continuity. Reconciliation smooths error only in the current chart. It maps a correction through the seam when the authoritative chart changed, then corrects there.

AOI and replication interest see through an aperture from both sides. Observers in either world receive the needed baseline and deltas while the body can affect the aperture. Authority and physics readiness are shared transfer conditions. Render readiness is per client and cannot make a server delay transfer until every spectator has a GPU-resident view. A local client may use chart-predicted presentation only while it has bounded admitted data; other clients use a coherent delayed baseline. The same spatial prewarm bound informs observer interest, local assets, and contact snapshot demand. Late join, disconnect, reversal, portal close, topology replacement, destination restart, source restart, message loss, jitter, reordering, duplicate Commit, and budget refusal each need a recorded state-machine test. Baselines and transfer fences persist long enough for the configured world-recovery interval, and a committed authority is never rolled back by timeout.

## Physics and moving portals

Cross-seam physics has to preserve one physical result, not merely render a convincing split. Before a body can reach a remote static surface, import immutable copied collision shapes and materials into the authoritative chart. Restrict broad phase and narrow phase by the finite aperture and the body segment that lies in the far chart. The original portal rim remains a collider. No implementation may remove an infinite wall merely because a portal exists. Continuous collision detects time of impact at high velocity, advances to that impact, and solves the remaining fixed substep.

Dynamic contacts that span a seam require one deterministic coupled-island solve. The proposed owner is a game-level portal seam coordinator: `game` is L12 and can link `physics`, `scene`, and `world`. It exchanges copied request and result records with world runners at fixed seam barriers. `physics` receives value records and never holds a world pointer. `world` remains L4 and transports opaque bytes without understanding scene or physics. Generic `replication` remains L12 and cannot gain a sideways dependency on game or render; client and server adapters connect its generic handoff API to the game coordinator.

The coordinator maps all involved bodies into a chosen canonical chart, solves the coupled island once, then commits copied results to world stores at the barrier before publication. It does not send a delayed impulse to another world next tick. Before the transfer commit, an unready dynamic island blocks entry safely at the rim. After commit, a network fault can suspend the affected island and report readiness while retaining the committed owner. A distributed simulation cannot guarantee no latency artifact through an outage, and the plan must not claim otherwise. The cooperative scheduler waits only at the declared bounded barrier, without busy waiting.

Mouth motion is mandatory for ordinary portal movement. At each fixed substep and presentation sample, evaluate endpoint transform `T(t)`, endpoint linear velocity field `u(x)`, and endpoint angular velocity. For a fixed uniform scale `s` and rotation `R`, map body velocity as:

`vB = uB(xB) + s R [vA - uA(xA)]`

`wB = omegaB + R(wA - omegaA)`

The portal movement contributes work, so there is no global energy-conservation claim. The proposed physics policy for a scale crossing keeps mass constant and maps inertia as `I_B = s^2 R I_A R^T`. This is a deliberate game convention, not density preservation. Density preservation would change mass and must never be mixed with this policy. The coupled solve also needs scale-aware Jacobians and virtual-work transpose impulse mapping; rotating an impulse as though scale did not exist is insufficient. Unit-scale cross-world dynamics may land first, but scaled dynamic contact cannot be called seamless until these equations and tests land.

Portal topology retargeting or scale edits during overlap are revision-gated. The supported policy is to retain active endpoint pairing and scale revision until full clearance, while continuing to evaluate their moving endpoint poses. Apply a pending edit after clearance. If the old endpoint cannot remain valid, defer normal closure or make an explicit authority decision to evacuate or suspend the affected body. Do not silently apply a new static transform to a body whose contact was solved against the old moving transform, or silently erase its source or destination image.

## Delivery stages

Each stage leaves a buildable system and adds tests beside its owner. Do not begin later stages by deleting a still-used transfer path.

| Stage | Owners and likely files | Required result and gate |
|---|---|---|
| 1. Characterise contracts | scene, render, client, script tests; `SurfaceCameras`, `PortalGeometryDraw`, `PortalImageRuntime`, `ClientPortal` | Trace and test a shared presentation timestamp, split-row route, camera route, and current transfer state. Establish the production caller gap. No behaviour claim yet. |
| 2. Canonical crossing state | `mono.engine/scene`, `script`, client presentation collection | Add stable logical identity, crossing state, hysteresis, reference-anchor rule, full-clearance retirement, and one-active-seam rule. Test slow stand, epsilon jitter, reversal, root-first and camera-first cases. |
| 3. Live split rendering | `mono.engine/render`, shaders, client portal paths | Wire split rows into production geometry and capture paths. Enforce complementary finite masks, shared pose/palette/time, depth and shadow parity, and chart-labelled history. Test skinned, editable, tool, alpha, trail, and large partial aperture cases. |
| 4. Near-field readiness | render portal coordinator, client, assets, replication adapters | Add local replica and asset prewarm, promotion readiness, hysteresis, image-only fallback, capacity accounting, and matching-body suppression from retained captures. Test late assets, stale image, close, reverse, and budget exhaustion. |
| 5. Fenced authority handoff | script transfer, scene transfer codec, replication, game journal, client, server adapters | Add `PrepareRevision`, `SealBaseline`, durable decision journal, seam tuple `H`, epoch and baseline fences, inert presentation row, and exact-once input segments. Static-only crossings stay blocked where Stage 7 physics capability is required. Test loss, jitter, reorder, duplicate messages, late join, and source or destination restart. |
| 6. Static seam physics | physics, scene, game coordinator | Add finite-aperture static contact snapshots, physical materials, rim collision, swept TOI, support and wake transfer. Test rotated floor, ceiling, high velocity, and moving mouth transforms. |
| 7. Coupled dynamic islands | game scheduler coordinator, physics value records, product adapters | Refactor the fixed-step phase around the ordered seam barrier, then add deterministic cross-world dynamic solve, bounded waiting, scale-aware mapping, and explicit unavailable state. Enable dynamic crossing only after this capability passes. Test pushes, stacks, tools, opposing motion, thread-per-world, and process-per-world configurations. |
| 8. Acceptance and tuning | demos, tests, profiling recipes | Run reference scenes, release measurements, headless checks, and approved live GPU checks. Remove obsolete clone paths only after replacement tests cover their behaviour. |

Test homes are [`CameraPortalView`](../mono.engine/scene/tests/CameraPortalView.cpp), render [`PortalGeometryDraw`](../mono.engine/render/tests/PortalGeometryDraw.cpp), [`PortalRetainedBody`](../mono.engine/render/tests/PortalRetainedBody.cpp), and [`PortalTransparentBodyDepth`](../mono.engine/render/tests/PortalTransparentBodyDepth.cpp), script [`PortalTransfer`](../mono.engine/script/tests/PortalTransfer.cpp) and [`PortalTransferProcess`](../mono.engine/script/tests/PortalTransferProcess.cpp), and client and server [`PortalWalk`](../mono.client/tests/PortalWalk.cpp) and [`PortalWalk`](../mono.server/tests/PortalWalk.cpp) suites. Add solver and protocol integration tests, not presence or deletion smoke tests.

## Acceptance matrix and measurement

`PortalSeam.luau` is the focused visual and network scene. `ImmersivePortals.luau` is the longer interactive integration scene. `scripts/demos/capture-portal-seam.sh` and `portal-seam-report.py` compare temporal matched-room frames, including uncovered and duplicate body samples. The matched-frame capture rows and process transfer impairment rows above are recorded evidence. Product handoff, measured frame-rate targets, and quiet-host profiling remain open. Each report should name the fixture, resolution, backend, pipeline revision, presentation time, topology revision, and tolerance.

| Scenario | Required assertion |
|---|---|
| Slow forward, backward, and rest within epsilon | Stable owner and camera route. Two chart images cover the body without alternating side ownership. |
| Reversal before and after handoff | One cancellation before Commit, then one new fenced transfer after Commit. No double input consumption. |
| Fast body, rotated portal, floor and ceiling | Swept time of impact, rim contact, support continuity, no tunneled seam. |
| Animated stationary body and trailing limbs | Current and previous palette match on both halves; full clearance waits for limbs and tools. |
| MeshPart, EditableMesh, skinned mesh, transparent surface, alpha, trail | Identical aperture decision through geometry, depth, shadow, transparency, and effect paths. |
| Scale and moving mouth | Endpoint velocity field, angular motion, mass and inertia policy, and scale-aware coupled contacts meet declared numeric tolerances. |
| Remote observer and third-person camera | Shared delay and chart time on both halves; camera may route independently of body. |
| Loss, jitter, reorder, late join, restart, close, and capacity limit | Epoch and baseline fences prevent stale authority. Diagnostics identify image-only, blocked, suspended, or reset state. |
| Threaded and process-separated worlds | Same deterministic result for the declared common fixed-step capability. |

Run the planned workload at 30, 60, 144, and 240 FPS, including a variable-frame stall; with 0, 50, 150, and 300 ms RTT; 0 and 30 ms jitter; and 0, 1, and 5 percent loss plus duplicate and reorder injection. The focused image and process protocol rows described above have been exercised, but the complete product acceptance matrix has not passed. Beyond retained input, replication, or render-data budgets, the expected result is a named degraded state, not a false continuity pass.

For matched-room reference scenes at 1080p and 4K, compare a portal route with an equivalent continuous-room camera route. The seam band target is under one pixel positional mismatch and zero uncovered or duplicate opaque samples after defined rasterisation and anti-aliasing tolerance. Exact image equality is not a reasonable requirement where temporal jitter, filtering, or stochastic shading differ. Capture a full pose timeline keyed by persistent logical body identity and chart, then verify that each physical sample has one authority epoch and each presentation sample has one well-defined chart owner.

Measure CPU and GPU time, residency, upload and decode bytes, queue depth, staleness, crossing state, baseline latency, and p95 and p99 handoff time. Use `ENGINE_PROFILE`, `Metrics::Count` for rates, and gauges for current levels. Release results name preset, backend, scene, counts, resolution, and settings. A nonzero capacity with no measurement is unmeasured.

The normal code gates are the narrow module suites, `just preset=ci check`, `just test-architecture`, `just source-check`, and `just em-dash-check`, with suite filters verified from their IDs before use. New benchmark recipes belong in `just` and write only under the build output location. Authorized headless GPU captures have run, but the product walk still fails the visual continuity checks above. This document makes no full visual or performance pass claim.
