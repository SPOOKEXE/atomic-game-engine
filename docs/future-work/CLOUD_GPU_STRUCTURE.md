# Cloud GPU structure

Status: proposal (2026-11-20). Part of slice 2 of
[WEATHER_FIELD_PRIMITIVES.md](WEATHER_FIELD_PRIMITIVES.md): this is the GPU
side of the `Clouds` piece.

## Where the time goes

Windowed client, Weather demo, 1280x720, capped at 60 fps (trustworthy GPU
timestamps; headless has no backpressure, see below):

| Pass | GPU p50 |
|---|---|
| weather-integrate (the cloud ray march) | 19.6 ms |
| every other pass, weather or not | 0.3 ms or less each |

The march costs 640x360 rays x 128 steps, about 29.5 million samples, and
its cost follows steps almost exactly (8 steps: 0.28 ms; 128 steps: about
4 ms in a smaller Studio panel). So the problem is the cost of one sample.

One sample today does, in order, all dependent memory reads:

1. `EmptyNeighborhood`: a page search (a binary search over up to 512 page
   keys, repeated per LOD until found), or 27 brick-summary reads in dense
   mode.
2. `DensityAt`: 8 trilinear corners. Each corner runs its own page binary
   search, then reads the packed cell, decodes it, and for compact pages
   decodes an assembly. A missing page rebuilds the corner with the full
   noise set.
3. The LOD approximation guard, which may re-sample the 8 corners again.
4. `WeatherShadowFactor`, a cascade texture lookup.
5. The profile and erosion maths.

That is on the order of a hundred dependent reads per sample, and the GPU
waits on each. Fewer rays did not help in small views because the pass is
latency bound: one ray's 128 serial samples set the time.

## Target structure: a camera clipmap of 3D textures

The canonical field stays as it is: the simulation, determinism, CPU
samples and gameplay never touch this structure. The `Clouds` piece bakes a
presentation copy from it, and only the ray march reads the copy.

### Clipmap

- Five levels of one 3D texture each, centred on the camera, for example
  128 x 32 x 128 voxels per level (the layer is thin, so Y needs fewer).
- Level k covers 2^k times the extent of level 0. With 16 m voxels at level
  0, level 0 spans 2 km around the camera and level 4 spans 32 km, which is
  the whole Horizon domain. Detail is high near the camera and falls off
  with distance, which is the LOD asked for.
- Channels per voxel: density (R8 or R16F), and the baked light
  transmittance toward the sun (R8). About 1 MB per level, 5 MB in all.
- Each level is toroidal: when the camera moves, only the newly exposed
  slab of voxels is rebaked; the rest stays. Memory and bake cost are fixed
  whatever the domain size.

### Occupancy

- Each level carries a small mip of maximum density per 8^3 brick (16 x 4 x
  16 texels). A ray reads one texel and skips a whole empty brick in one
  step, so clear sky costs a few samples rather than 128.

### Bake (compute, amortized)

- A bake pass writes voxels from the canonical field: decode, the preset's
  profile and erosion, all applied once per voxel rather than once per ray
  sample.
- The same pass marches a short shadow ray per voxel toward the sun through
  the same clipmap and stores the transmittance. Clouds light themselves
  and the ground's cloud shadows read the same texture; the separate
  per-sample shadow lookup goes away. `CloudShadows` controls this bake.
- What is rebaked per frame is bounded: new slabs after camera motion, and
  a fixed number of bricks after a weather tick (spread over frames, which
  is the same idea as the shadow stagger). A budget, never a full rebuild
  in one frame.

### March

- One sample is one hardware-filtered 3D texture fetch (trilinear for free)
  plus the light channel from the same fetch. The level is chosen from the
  sample's distance to the camera, so step length grows with the level's
  voxel size: near samples are fine, far samples coarse. 48 to 64 steps
  cover the horizon at the same near quality as today's 128.
- Empty bricks are skipped through the occupancy mip.
- Early exit when transmittance falls below a threshold, as now.
- Ray count stays within the budget already committed (640 x 360), and
  blue-noise jitter plus the existing temporal reconstruction hide the
  coarser stepping. A later option is a checkerboard: march a quarter of
  the rays each frame and reproject the rest.

### Expected cost

From about a hundred dependent reads per sample to one filtered fetch, plus
an occupancy read per brick, and about half the samples. A tenfold to
fiftyfold cut of the march is the expectation to measure against: 19.6 ms
toward 0.5 to 2 ms at 1280x720. Bake cost is bounded by its budget and
measured separately.

## Alternatives considered

- Sparse brick atlas with an indirection texture (virtual texture): one
  indirection fetch plus one atlas fetch per sample, and it handles very
  sparse fields well. It keeps a page table and an allocator, which is the
  complexity the clipmap avoids, and its LOD needs a per-level indirection
  anyway. Keep it in reserve if a world's clouds are too sparse for a
  clipmap to pay off.
- Keep the packed field and replace only the binary search with a direct
  page lookup: a smaller change, but each sample would still decode 8
  corners by hand and look up shadows separately. It cuts the search, not
  the rest.
- 2D weather map plus a 3D noise texture (the Nubis approach): fast and
  proven, but it gives up the 3D simulated field, which is the point of
  `WeatherCompute`.

## Also found

Headless rendering has no backpressure. Without a swapchain the CPU keeps
submitting while the GPU falls behind: GPU memory grew by about 2 GB a
second in a headless Weather run (13 GB in five seconds at 1280x720, 24 GB
at 2560x1440), and GPU timestamps include the queueing (several unrelated
passes all read about 107 ms). The same run windowed stays flat. Headless
benchmarks and data-factory captures need the same frames-in-flight wait a
swapchain gives.

## Order

1. Experiment: one dense 3D texture level baked from the field, read by
   the existing march, to confirm the per-sample saving before building
   the rest.
2. Clipmap levels with distance-chosen sampling and growing step length.
3. Occupancy mip and empty-brick skipping.
4. Baked light transmittance replacing the per-sample shadow lookup, and
   the ground's cloud shadows reading it (`CloudShadows`).
5. Toroidal updates and the per-frame bake budget.
6. Headless frame throttle (independent; can land any time).

Each step: weather tests, the paired windowed measurement at 1280x720 and
in a full-size view, and a visual check.
