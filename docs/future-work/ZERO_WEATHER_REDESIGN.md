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
canonical density, so no edge appears at the extent.

The Weather demo is back on Bounded: a long Horizon Play run lost the Vulkan
device (1,585 `VK_ERROR_DEVICE_LOST`), most likely because rays across the
horizon band reconstruct density at full cost with no empty-space skipping
outside the resident pages. Before re-enabling it, bound the per-ray cost:
cap steps by distance, skip reconstructed samples above the layer envelope,
and measure a long run. Scene, render, weather, examples and client suites pass.

Horizon short-run measurement (2026-11-20, Debug, headless Play, 1280x720,
300 frames, starting camera, temporary `Domain = Horizon`): gpu
weather-integrate 2.13 ms mean, 4.85 ms max; gpu weather-shadow-map 3.42 ms
mean; no device loss in 300 frames, so the loss needs the long travelling run.
Each missing-page sample interpolates 8 `WeatherDensityCode` reconstructions,
and each runs the full `WeatherEvaluate` noise set; the shadow pass marches up
to 128 sun steps per texel every frame with the same reconstruction. Rejected
candidate: a density-only evaluator that stops after the coverage test
(identical codes) measured 2.02 and 3.25 ms, inside run-to-run noise, because
the storm preset covers most of the sky. The levers that remain are the ones
listed below: cached light transmittance and empty-space skipping outside
resident pages.

Slice 2, per-preset profiles: `weather-profile.glsl` shapes the cloud and
cloud-shadow passes from `CloudComputeShader`. Stratus is a thin even sheet
low in the layer, Storm towers through it denser with a darker base, Voxel
samples at block centres two cells across, and Cumulus stays the canonical
body so existing worlds are unchanged. The preset is part of the shadow map
signature. Presentation only. GPU case `each cloud preset draws its own body
profile` renders all four from below the layer and requires each pair to
differ; weather suites and the 94-module shader check pass. Visual acceptance
in the Weather demo is still the user's.

Slice 3, lighting along the body: each cloud sample's direct sun is scaled by
the transmittance the cloud shadow map already caches in four height bands
(`WeatherShadowFactor`), so bases darken under their own tops at the cost of
one texture read per sample. Ambient is unchanged, and with no shadow map
this frame every sample is fully lit. The GPU preset case checks that
switching cloud shadows off changes the lit body; that check first passed on
leftover temporal history, and after the history fix below it is verified to
fail with the shader's shadow factor forced to 1. Weather suites, the shader
check and the pre-commit tier pass.

Temporal history: camera cuts and travel beyond 32 m or two cells already
reset it. The preset and the cloud-shadow toggle now join its content key, so
a switch no longer blends the previous body's frames.

Per-ray bound: a ray stops after 1,024 density rebuilds for cells no resident
page holds (128 fully rebuilt samples). Resident pages and the approximation
guard over them are not counted, so Bounded worlds are unchanged and weather
suites pass. The short Horizon run is unchanged (gpu weather-integrate 2.09 ms
mean against 2.13) because its rays stay under the budget.

Long runs (2026-11-20): the first two attempts ran a copy of the staged Studio
outside the build tree, where `--demo Weather.luau` is not found, so they never
loaded the Weather world; their results are void and are not evidence.
Valid pair, from run directories holding both `studio/` and `assets/`: windowed
Studio on a private Xvfb display, Weather in Play with `Domain = Horizon`,
5 minutes each at about 50 fps, once with the pre-budget shader and once with
the budget. Neither lost the device and neither logged a new error. The
scripted click focused the demo's control panel rather than the world, so the
player did not travel. A travelling pair then moved the player's root part
1 km every 10 seconds through `instance_set` for 5 minutes (30 km, falling
as it went), windowed on Xvfb, once per shader: no device loss and no new
error either way. The original loss does not reproduce on the current tree
with or without the budget, so the budget is a bound on worst-case work, not
a demonstrated fix. With the cost bounded, the Weather demo is back on
Horizon (`Domain = Horizon`, 16 km).

Measured round 1 (2026-11-20), the Weather demo as shipped (Horizon, 16 km),
headless Play at 1280x720, 900 frames, same scene and character state, no
device loss in either build:

| | Debug | Optimized `bench` |
|---|---|---|
| Frame mean, p99 | 7.09, 10.2 ms | 1.51, 3.98 ms |
| gpu weather-integrate p99 | 4.58 ms | 4.28 ms |
| gpu weather-shadow-map p99 | 3.80 ms | 3.72 ms |
| weather sparse residency plan mean | 0.62 ms | 0.11 ms |
| script-heartbeat mean | 1.23 ms | 0.23 ms |

GPU work is the same in both builds and is the optimized critical path; the
integrate and shadow passes run on weather ticks (optimized p50 0.005 ms), so
their means depend on the tick rate.

Measured round 2, retained work and allocations (Debug with `--heap-report`,
same scene, 900 frames; the optimized preset builds without heap hooks): CPU
heap 23.4 MiB live and 23.6 MiB peak; 340.6 MiB allocated in 1.27 million
blocks, about 1,400 allocations and 380 KiB per frame across the whole
process; GPU logical heap 148.5 MiB live and 161.6 MiB peak (155 MB of it in
188 textures). The five second growth sample fits poorly (r about 0.43) and
reads as start-up ramp, not a measured leak.

Measured round 3, GPU critical path (from round 1's optimized run): frames
with a weather tick carry gpu weather-integrate (4.3 ms p99) plus
gpu weather-shadow-map (3.7 ms p99), about 8 ms of GPU time against a 16.7 ms
60 Hz frame; frames between ticks spend about 0.005 ms on each. The CPU frame
p99 is 3.98 ms, so the tick frames' GPU work is what a frame-rate target meets
first. Moving the shadow map to its own cadence or splitting it across frames
is the lever; both change when shadows update and are left for the user.

Shadow stagger (2026-11-20, user choice "stagger by one frame"): a ready
cloud shadow map that goes stale is kept for that frame and rebuilt the next
time its world is recorded, never put off twice in a row, so the rebuild
leaves the frame that pays for the weather step. Paired measurement, Debug,
headless Play of the Weather demo, 900 frames, three alternating pairs with
the stagger switched off and on, no device loss:

| | Off | On |
|---|---|---|
| Shadow-map rebuild frames | 74, 76, 77 | 38, 37, 38 |
| Frame p99 ms | 12.35, 9.94, 10.35 | 10.65, 12.73, 10.00 |
| Frame max ms | 14.3, 14.5, 12.4 | 14.8, 16.6, 13.7 |
| gpu weather-integrate p99 ms | 4.08, 4.08, 4.09 | 4.08, 4.08, 4.11 |

In the demo the map goes stale on about every other Weather draw, so a
deferral and the next staleness merge into one rebuild: shadow GPU work
halves and shadows lag the clouds by one draw. Frame times are inside
run-to-run noise in this CPU-bound Debug build. Kept.

Cost beyond the resident set and Horizon travel, after the cloud bake
(`CLOUD_GPU_STRUCTURE.md`, 2026-11-20): the march reads a baked 3D texture,
and the bake rebuilds a non-resident cell from canonical noise once per
voxel rather than up to 1,024 rebuilds per ray. Worst case, with nothing
resident so every voxel is canonical (windowed client, 1280x720, capped at
60 fps): cloud pass 0.41 ms p50, against 0.23 to 0.24 ms with residency and
19.6 to 21.1 ms before the bake. Travel run: Studio windowed on Xvfb, Weather
in Play, the player teleported 1 km every 10 seconds for 5 minutes (30 km):
no device loss, no new error.

Remaining: per-preset detail erosion beyond the vertical body (an erosion
pass was measured as about 10% slower on the old march and dropped; with
the bake it can be applied once per voxel instead), and visual acceptance
by the user.
