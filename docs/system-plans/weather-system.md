# Weather system

## Model

Weather is a set of separate components. `WeatherCompute` simulates and
publishes fields. Other components read those fields and draw or act on them.
Each reading component also works without weather, from its own authored
values. A target weather system (overcast, storm with hail, fog valley,
tornado) is a composition of these pieces, wired by a script.

Canonical time, determinism, checkpoints and gameplay events stay on
`WeatherCompute`. Consumers only read. A missing or disabled `Source` falls
back to the consumer's own behaviour, so nothing goes blank.

## Components

| Component | Role | Code |
|---|---|---|
| `WeatherCompute` | Field source: seeded procedural field or stateful solver, canonical clock, events, checkpoints. `Domain` is `Bounded` or `Horizon` (`HorizonDistanceMetres`, default 16 km). Without a `CloudCompute` sourcing it, it draws itself with its own shadow settings | `mono.engine/scene/include/engine/scene/WeatherCompute.hpp`, `mono.engine/scene/src/WeatherCompute.cpp` |
| `CloudCompute.Source` (`CloudSource`) | Draws a weather field volumetrically with its own look (shader preset, colour, cover, density). Without a source it shell-marches its own noise into the sky. `CloudCompute` is the only cloud class; `Clouds` and `CloudProcedural` documents load as it | `mono.engine/scene/include/engine/scene/Atmosphere.hpp` |
| `CloudShadows` | Child of the cloud that draws the weather (a `CloudCompute` with a `Source`, or the `WeatherCompute` itself). Sun transmittance from the parent's density; `Enabled = false` turns cloud shadows off. Clouds also light their own body with it | `Atmosphere.hpp`, `mono.engine/render/src/WeatherShadows.cpp` |
| `Volume.Source` (`VolumeSource`) | `SourceChannel` (Fog, Humidity, Condensate, Rain) and `SourceScale`. `weather.volumes` samples the source at the volume centre each tick into `VolumeFieldSample`; `ResolveVolumes` scales the authored density by it. Not saved or replicated | `mono.engine/scene/include/engine/scene/Volume.hpp` |
| `GpuParticleField.Source` (`GpuParticleSource`) | Cosmetic rain, snow and hail look: three styles, population cap, rate scale. Weather decides where and how fast it falls, with roof occlusion and wind. Also takes packed spawn samples (`SetSpawnSamples`) for non-weather effects | `mono.engine/scene/include/engine/scene/GpuParticleField.hpp`, `mono.engine/resources/shaders/gpu-particle-field.*` |
| `PhysicalParticleField` (`PhysicalParticleSource`) | Gameplay particles: Q16 fixed point on server and client, spawns a pure function of seed, tick and weather sampled once per clock second (`weather::QueryWeatherAt`). Segment raycasts against anchored parts only. Response Bounce, Stick or Die. `OnParticleCollision` in Luau and JavaScript, capped by `MaxEventsPerStep`. Nothing replicates; late joiners replay the last lifetime silently | `mono.engine/scene/include/engine/scene/PhysicalParticleField.hpp`, `mono.engine/game/src/PhysicalParticles.cpp` (`particles.physical`) |
| `Atmosphere`, `SkyboxCompute`, `Sunlight` | Unchanged by weather. Scripts read weather samples and set sky, ambient and brightness | `Atmosphere.hpp`, `mono.engine/scene/src/Sunlight.cpp` |

Lightning and thunder are scripted from the deterministic samples and clock.
The shared module also emits authoritative lightning and hail events
(`mono.engine/weather/include/engine/weather/Events.hpp`).

## Fields and sampling

- CPU: `weather::QueryWeather(store, provider, position, ground)` returns every
  channel: condensate, temperature, vapour, pressure, wind, ice, and
  precipitation split into liquid, snow and hail. Canonical and deterministic.
- Script: `WeatherCompute:Sample(position[, groundHeight])` returns an owned
  record in Luau and JavaScript. `Tick` and `Epoch` are decimal strings. See
  `mono.engine/weather/docs/Design.md`.
- GPU: `mono.engine/resources/shaders/weather-field.glsl` holds the page lookup
  and channel decode (`WeatherChannelCode`, 0 to 7: density, temperature,
  vapour, pressure, wind X, Y, Z, ice). Consumers name their arrays with
  `WEATHER_FIELD_CELLS` and `WEATHER_FIELD_PAGES`.
  `RendererState::WeatherFieldOf(world, pipeline)` finds a world's resident field.
- Precipitation exists only on the CPU. Particle consumers read it through
  `QueryWeather`.

## Shared weather module

`mono.engine/weather/` (headless, no GPU):

| File | Content |
|---|---|
| `Weather.cpp`, `WeatherBatch.cpp`, `WeatherSample.hpp` | Algorithm version 1: integer noise on per-purpose rotated horizontal lattices (`PurposeRotation`), eight-byte semantic cells, arbitrary-time seeking, four-lane SSE2/NEON batches |
| `Clock.cpp` | Anchors, `ResolveTime`, `Reanchor` |
| `Simulation.cpp`, `StatefulRuntime.cpp` | Stateful FP32 grid: pressure projection, condensation, source and boundary budgets, terrain solids, surface moisture, falling-water transport |
| `Checkpoint.cpp` | Format 3 complete state, bounded replay, transactional refusal |
| `Residency.cpp`, `Assemblies.cpp` | Sparse page planning; exact merged assemblies with hysteresis |
| `Consumers.cpp`, `Events.cpp` | Precipitation split, melting, hail growth, fog; lightning and hail events |

## Composition into target systems

`mono.engine/examples/assets/scripts/Weather.luau` builds the reference set:

- `WeatherCompute` "Weather Clouds", `Domain = Horizon`, 16 km.
- `CloudCompute` "Weather Cloud Layer" with `Source = weather` and a
  `CloudShadows` child (2 cascades, 256, 2048 m).
- `GpuParticleField` "Weather Precipitation" sourcing the weather.
- `PhysicalParticleField` "Weather Hail", channel `All`, scale 0.02, enabled in
  the storm preset only; it bounces off the roof and ruins and settles on the
  ground.
- Two `Volume`s (low bank, ruin mist) reading channel `Fog`, scale 0.003.
- Presets: clear, scattered cumulus, overcast stratus (cloud sea from above),
  towering storm with hail, wispy ice, ground fog. The script lerps sky
  brightness and ambient per preset.

The same pieces without a weather source make a hand-authored cloud bank, a
smoke column or a snowfall.

Tornado target (`mono.engine/examples/assets/worlds/TornadoSim.aworld`,
behaviour reference: the standalone TornadoSim project): today a scripted world.
`StormControls` holds presets, `StormField` the script-side analytical vortex.
The server applies forces and damage through script components
(`Tornado.Response`, `Tornado.Link`, `Tornado.Vegetation`) and replicates
`Tornado.State`. Visuals use `GpuParticleField` spawn samples and a legacy
`CloudCompute` deck. The intended mapping onto the field primitives:

| Tornado part | Piece |
|---|---|
| Analytical vortex | A field source publishing wind, pressure and condensation |
| Funnel and wall cloud | `CloudCompute` drawing that condensation at fine clipmap cells |
| Dust skirt | `Volume` reading near-ground debris density |
| Rain, cosmetic debris | `GpuParticleField` with the source's wind |
| Physical debris, hail | `PhysicalParticleField` |
| Damage | A query on the maximum-wind ring |

## Clock and residency

| Data | Residency |
|---|---|
| Packed cells, assemblies, page tables, summaries | Retained GPU buffers |
| Cloud history, reconstruction, shadow maps, baked clipmap textures | Retained GPU textures |
| Stateful cells, falling water, terrain flags, scratch | Retained GPU buffers; checkpoint decode is transient |
| Phase counter and displacement | One 32-byte GPU buffer per world/pipeline generation |
| Definitions, anchors, events | Shared ECS and copied network messages |
| Client clock estimate | Local ECS, monotonic time |

- `mono.engine/render/src/WeatherGpuClock.cpp` plans absolute anchors or
  increments; `weather-clock.comp` adds 64-bit phase with carry and wraps
  displacement modulo uint32. No CPU volume mirror and no clock readback.
  Unchanged ticks and repeated views do no dispatch. Absolute anchor at start,
  every five source seconds, and on identity, revision, rate, backward or large
  seek changes.
- Server and Studio Play send reliable clock anchors every second, respecting
  interest. Clients estimate receipt with RTT/2 (capped 0.5 s) and slew their
  local rate within 90% to 110%. Errors over two seconds, new epochs and
  timeline changes reconstruct. Without fresh samples the estimate extrapolates
  two seconds, then holds. Authoritative time never follows a client.
- Stateful GPU evolution is cosmetic; authoritative state is the CPU solver and
  its checkpoints. Publication swaps only after validation.

## GPU path

Shaders in `mono.engine/resources/shaders/weather-*`; host in
`mono.engine/render/src/Weather*.cpp`.

1. `weather-generate.comp` packs cells; `weather-evolve.comp` advances stateful
   cells.
2. `weather-summary.comp`: one 64-lane workgroup per page, exact integer
   reductions. Certified-empty pages carry no payload.
3. `weather-bake.comp` bakes a coarse 3D density texture around the eye;
   `weather-detail.comp` fills three finer clipmap levels
   (`WeatherBake::DETAIL_LEVELS`, about 16 km). Level k's voxel is 2^k times
   level 0's; centres snap to a quarter span so nearby eyes, including portal
   virtual eyes, share a box. Non-resident cells rebuild from canonical noise
   once per voxel. The bake keeps two cells of margin past the layer so the
   cubic B-spline reaches baked data at the layer's top and bottom. Levels 1
   and 2 are read with the B-spline; `ProfileDetail` keeps only the octaves a
   level's voxel can hold, and the coarse path keeps the broadest one.
4. `weather-clouds.comp` marches the bake within a fixed 640x360 ray budget.
   `weather-profile.glsl` shapes Cumulus, Stratus, Storm and Voxel bodies and
   `ProfileErode` detail.
   Direct sun is scaled by `WeatherShadowFactor`, plus a multiple-scattering
   term and a weak back lobe.
5. `weather-shadow.comp`: cloud shadow map. A stale ready map is kept one frame
   and rebuilt on the next record of its world, never deferred twice.
6. `weather-temporal.comp`, `weather-reconstruct.comp`: depth-aware temporal
   reconstruction. Camera cuts, travel over 32 m, preset and shadow toggles
   reset history. Exact-neutral samples preserve the scene bit for bit.
7. `weather-fallback.frag`: bounded fragment clouds for Tier B, missing graph
   products or refused compute.

Portal and mirror captures use stored depth, their own camera, bank-owned
scratch, the generation's clock and no shared temporal history.

## Validation

| Command | Covers |
|---|---|
| `just test` / `just test-pre-commit` | `test_weather` (`engine.weather.*`), scene sources and volumes, `ParticleCollision` in both VMs, render headless weather, `engine.examples.tornado-sim`, `engine.scene.gpuparticlefield` |
| `just test-pr` | Adds GPU cases |
| `.cache/build/dev/tests/test_render '[gpu][weather],[gpu][weather-stateful],[cloud-seams]'` | Device parity, seams, eviction, shadows, portal and mirror weather, precipitation, lightning, stateful evolution, presets |
| `just weather-fuzz` | Checkpoint decoding under ASan and UBSan |
| `just weather-bench` | Shared module: sampling, packing, clocks, assemblies, residency, solver |
| `just weather-residency-bench`, `weather-temporal-bench`, `weather-shadow-bench`, `weather-effects-bench`, `weather-descriptor-bench` | Device-free render planning |
| `just weather-gpu-bench 3 bench 1920 1080 256` | Real device generation, summary, integration, shadows; `MONO_WEATHER_GPU_BENCH_TICK` picks a phase |
| `just gpu-particle-field-bench` | GPU particle presets |
| `just ray-environment-tornado-bench` | TornadoSim cloud deck |

Image fixtures must set `RendererState::WeatherPresentationSeconds`; live
presentation reads `steady_clock` and is not repeatable. Measure on a quiet
machine and compare paired runs; Vulkan is the only exercised backend.

## Open work

- Tornado field source: a native analytical vortex publishing wind, pressure
  and condensation, so TornadoSim composes `CloudCompute`, `Volume`,
  `GpuParticleField` and `PhysicalParticleField` instead of its scripted field.
- Size-aware hail melt in `mono.engine/weather/src/Consumers.cpp`. Hail
  currently melts out over a warm column, so the demo hail reads the whole
  precipitation channel.
- GPU precipitation channel in `weather-field.glsl`, so particle consumers can
  read precipitation without the CPU sampler.
- Storm dynamics from the proposal: atmospheric sounding, multi-layer wind
  profile, cloud deformation by shear and updrafts, storm cells, cold pools and
  fronts, falling-rain evaporative cooling, hail growth and transport, water
  and energy accounting. None is implemented.
- Automatic ambient dimming by cloud cover. Today only scripts do it.
- High-FPS portal crossing: one all-black frame at 144 FPS and
  `adoptedClearSamples` gate failures in `mono.client/tests/PortalWalk.cpp`.
  Next step: producer and consumer image readbacks at the failing pose.
- Metal execution and the NEON evaluation branch have never run.
