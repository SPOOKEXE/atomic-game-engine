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
- Lifetime: the field's CPU component stays alive while its particles are.
  Destroying the instance marks it dying; the gameplay tier keeps stepping
  its live particles and releases the record when none remain, so events
  from a dying field still carry its components.
- Cost: the gameplay tier is bounded by count and by a per-tick collision
  query budget; a field over budget refuses new spawns rather than slowing
  the tick.

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

- Finding for slice 2: the preset image test is not repeatable run to run
  (up to 2,545 of 4,225 pixels differ, by up to 48 levels), because live
  weather presentation reads `steady_clock` unless a fixture sets
  `RendererState::WeatherPresentationSeconds`. Slice 2's before and after
  image check must set it.

