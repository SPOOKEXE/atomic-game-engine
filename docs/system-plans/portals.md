# Seamless portals plan

A portal must behave as a continuous opening for an ordinary moving body,
including a player who stops with their torso on the seam. "Seamless" means
every visible point of a crossing body is drawn in its chart at one
presentation time, with no uncovered or duplicate opaque samples, pose-time
disagreement, camera discontinuity or ownership pop. The simulation keeps one
canonical body and authority; the second visible half is derived presentation.

Scope: `Part`, `MeshPart`, `EditableMesh`, skinned meshes, tools, rigid
assemblies, stationary animation, independent first and third-person camera
routing, remote observers, motion, rotation and uniform scale. A retained
remote image does not hide missing replication or assets: without live local
data near the seam the portal stays image-only and refuses traversal.

Design history and the rationale behind the current passes is in
[`docs/retired/NON-EUCLIDEAN.md`](../retired/NON-EUCLIDEAN.md).

## Built

- Seam mapping: `SeamTransform`, `SeamCut` (`scene/SurfaceCameras.hpp`), the
  only transform convention.
- Camera routing independent of body ownership (`scene/src/portals/CameraPortalView.cpp`).
- Body draw splitting from one pose and skin palette
  (`render/src/portal/PortalGeometryDraw.cpp`, `FarHalfOfRow`), seam-plane discard in
  opaque and shadow paths, oblique near clipping.
- Static contact proxies (`physics/src/Portals.cpp`) and copied dynamic island
  solves through `game/src/PortalSeamCoordinator.cpp` (`physics/src/PortalIsland.cpp`).
- Transfer protocol Offer, Ready, Commit, Done, Cancel with incarnation fencing
  (`script/src/PortalTransfer.cpp`); client prewarm and prediction continuity
  (`mono.client/src/ClientPortal.cpp`).
- Light through the hole (seam light, local lamps one hop, beam shadows) and
  particles across a seam.

## Contract

- **Identity.** Each logical body has a serialized `BodyKey` and lifetime
  generation, never an entity number or path. Crossing state references the
  canonical `Transform`, `PreviousTransform`, `Motion` and animation components
  and adds only portal facts (key, anchor rule, topology revision, authority
  epoch, active seam, phase, presentation revision).
- **Four separate conditions.** Volume overlap draws both halves; reference
  crossing (declared root or centre of mass) moves authority; eye crossing
  routes the camera; full clearance retires the old half. Standing halfway is a
  steady state with no timeout.
- **Hysteresis.** At zero signed distance the body stays in its chart.
  Reversal before Commit restores state; after Commit it is a new crossing.
- **One active seam per connected body.** A second seam blocks at its rim and
  reports the limiting seam.
- **Finite-aperture mask.** A source sample is cut only when behind the seam
  plane and inside the transfer volume; the destination is its complement.
- **Shared presentation time.** Portal capture must sample the destination at
  the client's presentation time, not its own `WorldFrame.Seconds`.
- **Topology edits during overlap** are revision-gated: keep endpoint pairing
  and scale until clearance; a forced close evacuates or suspends explicitly.

## Open work

1. **Fenced handoff.** Add a `Prepared` phase and versioned `PrepareRevision`
   and `SealBaseline` before Commit. `H` names clock domain, seam step and both
   ticks; source owns `t <= H`, destination `t > H`. The baseline carries
   identity, epoch, transforms, velocities, animation, sleep and contact keys,
   consumed input sequence and a complete component baseline. Deltas name epoch
   and baseline ID; stale tombstones cannot remove a newer incarnation.
2. **Decision journal.** The seam coordinator persists the transfer decision and
   sealed baseline before Commit; recovery freezes the body until the outcome is
   known and the destination activates exactly once.
3. **Exact-once input.** Bounded ordered per-tick input segments with a
   consumed-through acknowledgement and reject-through fence.
4. **Interest through the aperture** from both sides; render readiness is per
   client and never delays server transfer.
5. **Static seam physics completeness.** Finite-aperture imported colliders and
   materials, rim collision, swept time of impact, support and wake transfer.
6. **Moving and scaled mouths.** `vB = uB(xB) + s R [vA - uA(xA)]`,
   `wB = omegaB + R(wA - omegaA)`; constant mass, `I_B = s^2 R I_A R^T`;
   scale-aware Jacobians. Same fixed timestep and one ordered seam barrier only.
7. **Product walk visual continuity.** The process-hosted walk commits and adopts
   both ways at 30 and 60 Hz, but at first adoption floor and body pixels drop
   to near zero and the 60 Hz return has a black interval. Retaining the old
   camera route indefinitely regressed transfer and was reverted.
8. **Ribbons and billboards across a seam**, using the same path
   `CollectParticleBatches` uses.

## Acceptance

`PortalSeam.luau` is the focused scene; `ImmersivePortals.luau` the long one.
`scripts/demos/capture-portal-seam.sh` and `portal-seam-report.py` compare
matched-room frames for uncovered and duplicate body samples.

| Scenario | Assertion |
|---|---|
| Slow forward, backward, rest within epsilon | Stable owner and route; two halves cover the body |
| Reversal before and after handoff | One cancel before Commit, one new fenced transfer after; no double input |
| Fast body, rotated portal, floor and ceiling | Swept impact, rim contact, no tunnelling |
| Animated stationary body, trailing limbs | Same palette both halves; clearance waits for limbs and tools |
| Mesh, editable, skinned, transparent, alpha, trail | One aperture decision through every pass |
| Scale and moving mouth | Declared numeric tolerances for velocity, inertia and contacts |
| Remote observer, third-person camera | Shared delay and chart time on both halves |
| Loss, jitter, reorder, late join, restart, close, capacity | Fences prevent stale authority; degraded state is named |
| Threaded and process-separated worlds | Same deterministic result |

Run at 30, 60, 144 and 240 FPS with a stall; 0, 50, 150 and 300 ms RTT; 0 and
30 ms jitter; 0, 1 and 5 percent loss plus duplicate and reorder. Record
requested and achieved frame rate separately. At 1080p and 4K the seam band
must be under one pixel of positional mismatch with zero uncovered or duplicate
opaque samples. Measure CPU and GPU time, residency, upload and decode bytes,
queue depth, staleness and p95 and p99 handoff time in a release preset.
