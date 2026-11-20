# Weather from fields and primitives

Status: approved by the user (2026-11-20). Slice 1 in progress.

## Goal

Weather is not one feature. It is general engine pieces, combined by a
script. `WeatherCompute` only computes. The pieces that draw are the same
pieces a world uses without weather:

| Piece | Today | After |
|---|---|---|
| `WeatherCompute` | Simulates the field, draws clouds with its own integrator, spawns precipitation, casts cloud shadows | Simulates and publishes fields only: density, precipitation (rate and type), wind, temperature |
| `Clouds` / `CloudCompute` | A 2D presentation layer and a voxel-like environment texture | Draws any density field volumetrically: the current weather integrator (Horizon domain, profiles, ray budget, reconstruction) moves here. `Source` names a field; without one it draws its own procedural noise |
| `CloudShadows` (new) | Part of `WeatherCompute` | A child of a `CloudCompute`. Its own component, so it can be disabled or removed. Builds the sun transmittance map from its parent's density, so shadows always match the drawn body. Clouds also light themselves with it when present |
| `Volume` | A bounded medium with its own noise | Optional `Source`: density from a field channel (fog banks from humidity, a low cloud bank), else its own noise |
| `GpuParticleField` | Generic GPU particles, already follows a vector field | Optional `Source`: spawn rate and style from a precipitation channel (rain, snow, hail), velocity from wind. Particles pass through the world |
| `PhysicalParticleField` (new) | None. Rain under roofs is hidden by 96 CPU trajectory queries per frame | A `GpuParticleField` whose particles collide with the world and bounce, stick or die per style (hail bounces, rain splashes). Fires `OnParticleCollision` |
| `Atmosphere` / `SkyboxCompute` | Authored scattering and sky colours | Unchanged in the engine. The demo script reads weather samples and sets them, as it does today |

The Weather demo becomes a script that builds one `WeatherCompute` and wires
`Clouds`, two or three `Volume`s, a `GpuParticleField` and the sky to it. The
same pieces make a hand-authored cloud bank, a smoke column or a snowfall
without any weather.

## Field contract

- A field is a named, versioned channel set on one `WeatherCompute`:
  `Density`, `PrecipitationRate`, `PrecipitationType`, `Wind`, `Temperature`.
- GPU: the existing resident pages and summaries, exposed through one binding
  any consumer pass can read. CPU: the existing canonical samplers.
- A consumer holds a reference (`Source`) and a channel name. A missing or
  disabled source falls back to the consumer's own authored behaviour, so
  nothing goes blank.
- Canonical time, determinism, checkpoints and gameplay events stay on
  `WeatherCompute` exactly as now. Consumers only read.

## Slices

Each slice keeps every existing world loading and drawing the same, adds its
tests, and gets a paired measurement before it is committed.

1. Field binding. Publish the field channels from `WeatherCompute` behind one
   GPU binding and one CPU sampler API. No visual change. Tests: channel
   values match today's samples.
2. Clouds draws a field. Move the integrator, profiles and the ray budget
   behind `Clouds.Source`, and the cloud shadow map into a `CloudShadows`
   child that can be disabled. `WeatherCompute` stops drawing.
   Serialization reads old worlds by creating the `Clouds` link they imply.
   Tests: Weather demo images match before and after; a `Clouds` without a
   source still draws.
3. Volume reads a field. `Volume.Source` and `Volume.Channel`. The demo's
   low bank and ruin mist read humidity instead of fixed noise.
4. Particles read a field. `GpuParticleField.Source`: precipitation
   rate and type pick spawn rate and style; wind drives velocity. The
   internal precipitation field goes away. Roof occlusion keeps working.
   Hail as a style, so the hail-on-grass reference can be built.
5. Physical particles. See "Physical particles" below.
6. Demo rebuild. `Weather.luau` composes the pieces. Presets match the
   references: a cloud sea seen from above, scattered cumulus, overcast
   stratus, a storm with hail.
7. Acceptance. Visual checks by the user, the three measured rounds from
   `ZERO_WEATHER_REDESIGN.md`, the full gate, then prune the Zero item.

## Physical particles

Decided by the user (2026-11-20): collisions count for gameplay. Server and
client each simulate them; nothing replicates. Both derive the same result
from the seed and the authoritative clock.

That rules out the GPU for gameplay particles. GPU floating point differs
between vendors and drivers, and a dedicated server may have no GPU. So a
physical field has two tiers:

- Gameplay tier: a bounded count (a few thousand, for example hailstones),
  simulated on the CPU in fixed point, the same code on server and client.
  Spawn position, time and velocity are pure functions of the field's seed,
  the canonical weather sample and the clock tick, so a late joiner computes
  the same particles without receiving them. Collision uses the physics
  world's queries against static parts. `OnParticleCollision` fires from
  this tier, on both sides, with position, direction, velocity, normal, the
  part hit and the particle's style.
- Cosmetic tier: the existing GPU field draws the gameplay particles and any
  extra visual particles, which have no events and need not agree between
  machines.
- Response per style: bounce with restitution, stick (settle as hail on
  grass), or die (rain becomes a splash).
- Lifetime: the field's CPU record stays alive while its particles are.
  Destroying the instance marks it dying; the gameplay tier keeps stepping
  its live particles and releases the record when none remain. Its
  connections die with the instance, so a dying field reports nothing.
- Cost: the gameplay tier is bounded by count, one segment query per moving
  particle per step, and a per-step spawn cap, so a full field stops
  spawning rather than slowing the tick.

## Decisions

- Particle collisions count for gameplay, through determinism, as above.
- Lightning and thunder are scripted: a script reads the deterministic
  weather samples and the clock, so server and clients choose the same
  strikes. No native piece.
- Cloud shadows are a separate `CloudShadows` child of `CloudCompute`.

## Progress

Slice 1, field binding:

- GPU: `weather-field.glsl` holds the page lookup and the channel decode
  (`WeatherChannelCode`, channels 0 to 7: density, temperature, vapour,
  pressure, wind X, Y and Z, ice). `weather-clouds.comp` and
  `weather-shadow.comp` include it instead of their own copies. A consumer
  names its cell and page arrays with `WEATHER_FIELD_CELLS` and
  `WEATHER_FIELD_PAGES`.
- Renderer: `RendererState::WeatherFieldOf(world, pipeline)` is the one
  lookup for a world's resident field; the ten hand-written searches use it.
- CPU: `weather::QueryWeather(store, provider, position, ground)` already
  returns every channel (condensate, temperature, vapour, pressure, wind, ice,
  precipitation split into liquid, snow and hail), canonical and
  deterministic. It is the CPU sampler; no wrapper was added.
- Precipitation exists only on the CPU. The GPU field has no precipitation
  channel; particle consumers read it through `QueryWeather`, as the
  precipitation field does today.
Slice 2, first part:

- `CloudCompute.Source` (component `CloudSource`) names a `WeatherCompute`.
  The cloud then draws that weather's field with its own look, and the
  weather stops drawing itself. `Environment::CloudProvider` is the weather's
  entity, since its events, samples and checkpoints are keyed by it.
- `CloudShadows`, a child class of a `CloudCompute`, replaces the drawn
  weather's shadow settings; `Enabled = false` turns cloud shadows off.
- Worlds without either keep today's behaviour: a lone `WeatherCompute`
  draws itself with its own shadow settings.
- The cloud pass marches within a fixed ray budget (640 x 360) on large
  views, commit `3de2c6b6`.
- Left in slice 2: the cloud GPU structure, a camera clipmap of 3D
  textures with baked lighting, in
  [CLOUD_GPU_STRUCTURE.md](CLOUD_GPU_STRUCTURE.md).

Slice 3, volumes:

- `Volume.Source`, `SourceChannel` (Fog, Humidity, Condensate, Rain) and
  `SourceScale` (component `VolumeSource`). The `weather.volumes` system
  samples the source with `QueryWeather` at the volume's centre each tick
  and stores the factor in `VolumeFieldSample`, which `ResolveVolumes`
  multiplies the authored density by. The sample is canonical, so server
  and client agree and the factor is neither saved nor replicated.
- Units: fog extinction per kilometre, vapour g/kg, condensate g/m^3,
  precipitation mm/h. Low volumes read the CPU sampler because the baked
  cloud texture only spans the cloud layer.

Slice 4, particles:

- `GpuParticleField.Source` and `SourceScale` (component
  `GpuParticleSource`). The weather still decides where and how fast rain,
  snow and hail fall, with roof occlusion; an authored field sourcing it
  supplies the three styles (rain, snow, hail), caps the population at its
  `RequestedCount`, scales the rates, and stops the precipitation when
  disabled. Without one, the built-in look applies. Wind already reaches the
  spawn velocities through the weather sample.

Slice 5, physical particles:

- `PhysicalParticleField` (components `PhysicalParticleField` and
  `PhysicalParticleSource`): spawn box, velocity, acceleration, rate,
  lifetime, radius, response (Bounce, Stick, Die), restitution, caps, seed,
  and an optional `Source` weather with a channel (Rain, Snow, Hail, All)
  and scale.
- `game::StepPhysicalParticles` ("particles.physical", Animation, after the
  weather) steps every field in Q16 fixed point on server and client. A
  sourced field steps on its weather's clock. Spawns are a pure function of
  seed, tick and the weather sampled once per clock second
  (`weather::QueryWeatherAt`), and the per-step cap is
  `MaxParticles / lifetime ticks`, so nothing depends on history. A machine
  that starts late replays the last lifetime of ticks and reports none of
  it. Tests check two worlds agree and a late joiner matches an early one.
- Collisions: segment raycasts against anchored parts only; a moving body is
  skipped (it can differ between machines at one tick).
- `OnParticleCollision` on the field, in Luau and JavaScript, with
  Position, Direction, Velocity, Normal, Speed, Age, ParticleId, Instance
  (part hit), Field and ParticleComponents (Radius, Color, Response). Events
  go through `ParticleCollisionLog`, capped per step by `MaxEventsPerStep`;
  each runtime reads past its own cursor.
- Lifetime: destroying the field marks its record dying. Spawns stop, live
  particles keep falling and are drawn, and the record goes with its last
  particle. A dying field reports nothing, since no script can still be
  connected to it.
- Drawing: each particle is an `engine.Sphere` row appended after the seam
  rows, placed between its last two steps by the clock fraction.
- Finding: the weather's hail channel is zero at the demo's ground. Falling
  ice melts over the warm column (melting about 0.9996 in the storm, even at
  a 4 C surface), and the sampled column has no updraft for hail growth. So
  the demo's "Weather Hail" reads the storm's whole precipitation rate
  (channel All, scale 0.02) and only the storm preset enables it. Hail that
  survives a warm column needs a size-aware melt in `Consumers.cpp`.

Slice 6, demo rebuild (first part): `Weather.luau` now builds the weather
as pieces. `WeatherCompute` "Weather Clouds" only computes; a `CloudCompute`
"Weather Cloud Layer" draws it (`Source`) with the look the panel changes
(shader, colour, cover, density); its `CloudShadows` child holds the shadow
settings; both fog volumes take their density from the weather's fog
(`SourceScale` 0.003 keeps the authored density at the default preset's
about 320 per km); and a `GpuParticleField` "Weather Precipitation" is the
rain, snow and hail look. The opening frame matches the monolithic demo
within 20 levels (the fog now tracks the weather). A `PhysicalParticleField`
"Weather Hail" lands hail in the storm preset.

Slice 6, presets against the reference photos (wing shots in the user's
`weather/iPhone-clouds`, about 10 km up):

- Lighting: the march adds a multiple-scattering term (a quarter of the sun,
  reaching deeper than the direct beam) and a weak back lobe to the phase.
  Before, a cloud was lit almost only by the blue ambient, so cumulus seen
  from the ground was grey and a deck seen from above was dark and blue.
- Cloud sea (IMG_1133, 1250, 1273): the overcast stratus preset is the deck
  seen from the ABOVE view. Now 512 m thick from 768 m, coverage 0.92,
  cover 0.9, density 0.85, so from above it is a continuous pale sheet
  instead of a thin, see-through one.
- Scattered cumulus (IMG_1105, 1106, 1112, 1256): `FeatureCells` 16 to 8,
  so the field breaks into many smaller clouds with ground between them.
- Limit found: at 64 m cells, smaller features show the cell grid as
  blocks and streaks (tried 4 cells). Sub-cell noise in `ProfileErode` was
  tried and reverted: at the 640 x 360 ray budget it turned to speckle and
  thinned cumulus away. Finer clouds need the clipmap's finer levels
  (`CLOUD_GPU_STRUCTURE.md`). Three levels now cover about 16 km around
  the camera: detailed near it, and smooth out to the horizon.

- Finding for slice 2: the preset image test is not repeatable run to run
  (up to 2,545 of 4,225 pixels differ, by up to 48 levels), because live
  weather presentation reads `steady_clock` unless a fixture sets
  `RendererState::WeatherPresentationSeconds`. Slice 2's before and after
  image check must set it.

Slice 7, acceptance (2026-11-20):

- The user checked the Weather demo in Studio (cloud sea from above,
  cumulus, storm with hail, erosion) and reported it works.
- Automated check of the shipped demo the same day: in the storm, the hail
  field logged 2,846 `OnParticleCollision` hits in nine seconds, on the
  ground (2,397), roof (239), ridges, ruins, columns and beacon, and hail
  rests visibly on the ground.
- Measured rounds: the three rounds in `ZERO_WEATHER_REDESIGN.md` plus the
  lighting and clipmap measurements; the cloud pass is 0.33 to 0.41 ms p50
  in Debug at 1280x720 with all three clipmap levels.
- Full gate passed after each slice.

