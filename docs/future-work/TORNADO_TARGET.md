# Tornado demo target

Status: target recorded (2026-11-20), for after the weather field plan.

## What it should look like

References from the user:

- `/home/declan/Documents/GitHub/TornadoSim`, the standalone simulation the
  demo should match in behaviour.
- A Roblox tornado demo video; stills every four seconds from 0:08 are in
  [tornado-reference/stills.jpg](tornado-reference/stills.jpg).

The look in the stills:

- A dark, low overcast storm sky with a darker cloud base above the funnel.
- A grey condensation funnel that grows from a thin rope into a wide cone
  as it approaches, with a churning dust and debris skirt at the ground.
- Slanted rain streaks in the foreground.
- A flat grass plain, a paved road lined with houses, power poles and
  signs, which the funnel then reaches.

## How TornadoSim builds it

One continuous analytical wind field, not a fluid solver. Condensation,
rain, debris, damage and the UI all sample the same model:

- A two-cell vortex: central pressure deficit, eye subsidence, rising
  condensation eyewall, maximum-wind ring, broad outer circulation, radial
  surface inflow, and a closed vertical cycle into an anvil.
- Condensation from pressure and humidity, stored in an adaptive sparse
  octree (about four-metre cells near the funnel, coarser outside).
- Debris with quadratic drag from air density, area, drag coefficient and
  mass; tumbling, ground impacts, bounces; buildings with breakable joints.
- A damage signal at the maximum-wind ring that structures and vehicles
  query.
- GPU cosmetic particles from 262k to 50M, with physical debris on the CPU.

## How it maps onto the weather field plan

See [WEATHER_FIELD_PRIMITIVES.md](WEATHER_FIELD_PRIMITIVES.md). A tornado is
one more field source next to `WeatherCompute`:

| TornadoSim part | Engine piece |
|---|---|
| Analytical vortex wind field | A new field source publishing wind, pressure and condensation channels |
| Condensation funnel and wall cloud | `Clouds` drawing the source's condensation channel through the baked density texture (`CLOUD_GPU_STRUCTURE.md`), at fine cells near the funnel |
| Dust skirt | A `Volume` reading the source's near-ground debris density |
| Rain and cosmetic debris | `GpuParticleField` with the source's wind as its velocity |
| Physical debris and hail | `PhysicalParticleField` (deterministic CPU tier) colliding with the world |
| Damage | A query on the source's maximum-wind ring, used by scripts and physics |

The funnel needs finer cells than the weather layer's 64 m, so it is the
first real user of the clipmap levels in `CLOUD_GPU_STRUCTURE.md`.
