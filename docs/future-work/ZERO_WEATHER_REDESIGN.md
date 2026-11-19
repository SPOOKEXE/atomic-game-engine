# Zero v0.26.1 weather redesign

Status: approved as written (2026-11-19). Slice 1 (presentation domain) is done; see Progress.

## Current problem

`Weather.luau` authors a finite 2048 by 2048 metre cloud region. The main
`weather-clouds.comp` integrator clips rays to its axis-aligned bounds. Increasing
the extent moves the visible square boundary rather than removing it.

The integrator also uses one lighting value along a ray. The demo's inherited
Cumulus, Stratus, Storm and Voxel shader selections do not select corresponding
body profiles in this integrator. These source findings need fresh runtime
measurements; old frame costs are not current acceptance evidence.

## Proposed model

- Keep accepted ECS weather, canonical time, CPU sampling, gameplay events and
  stateful checkpoints as the source of truth.
- Add explicit `Bounded` and `Horizon` procedural presentation domains. Existing
  authored worlds retain bounded behaviour. The Weather demo selects Horizon.
- Horizon intersects the cloud altitude layer, scene depth and a finite viewing
  distance. It has no world-origin XZ edge. Camera movement changes derived
  residency, never canonical weather identity or authoritative state.
- Keep existing bounded stateful simulation. This proposal does not replace the
  deterministic weather solver with a viewer-dependent atmospheric simulation.
- Define explicit vertical, body and detail profiles consumed by the main
  volumetric path. Presets must visibly distinguish scattered cumulus, layered
  overcast, storm and stylised block clouds.
- Cache density and light transmittance in bounded sparse pages. Demand is the
  bounded union of participating views, keyed by canonical world coordinates.
  Coarse coverage remains available during maintenance and teleports.
- Shade along the cloud body using cached light transmittance, direct light and
  ambient response. Preserve depth-aware temporal reconstruction, with explicit
  invalidation for camera discontinuities, definition changes and lighting.
- Quality controls affect presentation sampling and residency only. They cannot
  alter weather events, CPU queries, accepted time or checkpoints.

The renderer keeps its current owner-thread resource and submission contracts.
Pages, upload work and history remain bounded. Allocation or submission refusal
retains the last accepted output and retries without publishing false cache hits.

## Acceptance

1. Native and script roundtrips preserve both domains, profiles and existing
   bounded assets. Stable authored names cross file and world boundaries.
2. Horizon travel does not reveal a finite square. Above-cloud, below-cloud and
   interior views retain coherent depth and lighting.
3. Presets differ through the main integrator, including storm lighting and
   precipitation. Roof occlusion and CPU weather samples remain correct.
4. Multiple views, mirrors and portals use consistent world coordinates without
   unbounded per-view residency. Pause, resume and teleports preserve time rules.
5. Three measured rounds cover model correctness, retained work and allocations,
   then GPU critical-path cost. Compare the same scene, camera, resolution,
   character state and quality in Debug and an optimized build. Record actual
   pass samples, bytes, operations, tails and refusals. No universal frame-rate
   claim follows from one device.
6. Complete visual scene checks and the full repository gate pass before pruning
   the Weather redesign item.

## Decision requested

Approve the Horizon procedural cloud layer plus explicit cloud profiles and
lighting, retaining finite local/stateful weather. The alternative is to retain
finite regions and redesign only their appearance; it does not remove their
world-space boundary. A full atmospheric solver replacement is a separate,
larger design and is not assumed by this proposal.

Reference: [Guerrilla's Nubis cloud authoring presentation](https://www.guerrilla-games.com/read/nubis-authoring-real-time-volumetric-cloudscapes-with-the-decima-engine)
describes regional authoring, cloud types and volumetric lighting. It supports
the design direction, not a performance promise for this engine.

## Progress

Slice 1, commits `c4e65986` and `16dbc7cb`: `WeatherCompute.Domain` (Bounded,
Horizon) and `HorizonDistanceMetres`, as properties, bindings and serialization
format 3 (format 2 reads as Bounded). A procedural Horizon layer integrates its
cloud band over twice the horizon distance around the eye; shadows centre on
their casters, the 2D fallback spans past any camera, and residency selects
pages from the union of every view's horizon square. Missing pages reconstruct
canonical density, so no edge appears at the extent. The Weather demo uses
Horizon. Scene, render, weather, examples and client suites pass.

Remaining, in order: a visual check of Horizon travel and the cost of
reconstructed pages beyond the resident set; explicit vertical, body and detail
profiles per preset in `weather-clouds.comp`; lighting along the cloud body
with cached light transmittance; temporal reconstruction invalidation for
camera discontinuities; the three measured rounds and visual acceptance above.
