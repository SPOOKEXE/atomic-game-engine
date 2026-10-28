# Built-in weather features and simulation expansion

Status: proposed implementation plan, grounded in the existing engine. No runtime implementation is claimed by this document.

Reviewed branch: [`SPOOKEXE/atomic-game-engine`, `v0.26.1`](https://github.com/SPOOKEXE/atomic-game-engine/tree/v0.26.1), commit `ec6a43db29e00d00b1142d9df2ec3e95c61df8ef` (`assets-with-aworld`, 27 October 2026). This was the highest numbered `v0.26.*` branch returned by the remote branch listing during this review. The branch and `VERSION` file both identify version `0.26.1`; the requested “v26” is interpreted accordingly. Review date: 28 October 2026.

## 1. Intended result

Make a storm a connected, evolving weather system. A cell develops an updraft, grows cloud, transports ice and liquid, produces a precipitation core, cools its downdraft, spreads a cold pool and advances an outflow boundary. Air at different heights can travel in different directions. Cloud structures advect, stretch, lift, condense and evaporate within those winds. Hail grows aloft and can survive a warm descent more effectively after precipitation has cooled the column.

Keep two explicit modes:

- **Procedural:** deterministic, directly seekable weather with authored profiles, analytic cells/fronts and convincing cloud motion. Thermal and hail diagnostics remain named approximations.
- **Stateful:** history-dependent transport, phase changes, cold pools and hail growth. Complete checkpoints and bounded replay preserve the history. This is a real-time game atmosphere model, with declared approximations and validation, rather than a numerical weather forecast.

All unchecked items below are proposed. Existing facilities are recorded separately so an implementation does not rebuild or mistakenly advertise them as new.

## 2. What v0.26.1 already provides

The following comes from source inspection at the pinned commit, not just the repository's future-work plans. Paths below link to that commit.

| Area | Existing behavior | Expansion needed |
| --- | --- | --- |
| Authoring | [`WeatherCompute.hpp`](https://github.com/SPOOKEXE/atomic-game-engine/blob/ec6a43db29e00d00b1142d9df2ec3e95c61df8ef/mono.engine/scene/include/engine/scene/WeatherCompute.hpp) has physical reference controls, a wind vector, turbulence/shear/lift scalars, front contrast, precipitation, hail/lightning strengths, boundaries, terrain revision and clocks | Explicit vertical profiles, persistent cells/fronts, microphysics and consumer policies |
| Procedural weather | [`Weather.cpp`](https://github.com/SPOOKEXE/atomic-game-engine/blob/ec6a43db29e00d00b1142d9df2ec3e95c61df8ef/mono.engine/weather/src/Weather.cpp) and `WeatherSample.hpp` provide integer noise, separate random purposes, cumulative wind anchors and an eight-byte semantic codec | Height-dependent displacement and physical profile semantics; a front is currently a correlated noise response, not a tracked gust-front boundary |
| Current shear | [`WeatherSample.hpp`](https://github.com/SPOOKEXE/atomic-game-engine/blob/ec6a43db29e00d00b1142d9df2ec3e95c61df8ef/mono.engine/weather/src/WeatherSample.hpp) adds an envelope-scaled shear scalar to both horizontal wind components | Independent vector direction/speed by height, including counterflow and different cloud-layer motion |
| Stateful dynamics | [`Simulation.hpp`](https://github.com/SPOOKEXE/atomic-game-engine/blob/ec6a43db29e00d00b1142d9df2ec3e95c61df8ef/mono.engine/weather/include/engine/weather/Simulation.hpp) and [`Simulation.cpp`](https://github.com/SPOOKEXE/atomic-game-engine/blob/ec6a43db29e00d00b1142d9df2ec3e95c61df8ef/mono.engine/weather/src/Simulation.cpp) provide a constant-density MAC-style solver, pressure projection, scalar advection/diffusion, buoyancy, phase conversion, terrain solids and conservative falling-liquid transport | Momentum advection, background stratification, moist buoyancy, precipitation loading/drag, falling-rain evaporation, ice/hail mass and energy, turbulence closure |
| Current thermal model | The solver retains vapour latent energy and cloud/vapour conversion. Its saturation law is an exponential proxy. Falling liquid is a separate mass reservoir, with liquid sensible heat explicitly omitted | Pressure-aware water/ice saturation, full declared phase enthalpy, evaporation of falling rain, melting/freezing/sublimation and local temperature feedback |
| Hail and melting | [`Consumers.cpp`](https://github.com/SPOOKEXE/atomic-game-engine/blob/ec6a43db29e00d00b1142d9df2ec3e95c61df8ef/mono.engine/weather/src/Consumers.cpp) and [`StatefulRuntime.cpp`](https://github.com/SPOOKEXE/atomic-game-engine/blob/ec6a43db29e00d00b1142d9df2ec3e95c61df8ef/mono.engine/weather/src/StatefulRuntime.cpp) derive hail from ice/updraft/strength proxies, then reduce frozen precipitation using a warm-degree-metres melting curve | Hail size, number, growth history, trajectories, particle heat exchange and explicit phase mass transfer. A positive surface temperature must not automatically eliminate hail |
| Events and sound | [`Events.hpp`](https://github.com/SPOOKEXE/atomic-game-engine/blob/ec6a43db29e00d00b1142d9df2ec3e95c61df8ef/mono.engine/weather/include/engine/weather/Events.hpp) supplies copied lightning/hail events, stable identities, duplicate suppression, branched lightning and delayed thunder | Cell-linked discharge regions, spatial hail swaths and material-dependent impacts |
| Render and consumers | Sparse GPU cloud pages, assemblies, shadows, temporal reconstruction, fallback clouds, precipitation, fog and atmosphere coupling already exist. [`WeatherEffects.cpp`](https://github.com/SPOOKEXE/atomic-game-engine/blob/ec6a43db29e00d00b1142d9df2ec3e95c61df8ef/mono.engine/render/src/WeatherEffects.cpp) clips precipitation with opaque geometry bounds | Deforming/advection-aware cloud history, distant rain shafts, precise shelter policies, runoff, wind responses and phase-specific surfaces |
| Persistence | Checkpoint format 3 includes physical cells, falling water, forcing, budgets and terrain. Replicas query validated checkpoint data | A new complete state format for phase reservoirs, hail distributions, sounding/profile history and any turbulence memory |

Source status documents also matter: `docs/future-work/WEATHER_IMPLEMENTATION_STATUS.md` records substantial implementation and Vulkan checks; `WEATHER_VISUAL_VALIDATION.md` still records an intermittent black portal-crossing frame and failed client high-FPS handoff gates. These are pre-existing open findings, not verified fixes. This planning task did not run engine builds, GPU tests or benchmarks.

## 3. Reference media and design requirements

All three photos and all six videos are inputs. Timestamped speech appears in Appendix A. Video observations and spoken explanations must remain separate from measured meteorological facts.

| Input | Evidence or role | Feature requirement |
| --- | --- | --- |
| `IMG_0050.jpeg`, `IMG_0053.jpeg` | Broad dark storm base, horizontal bands and a low leading cloud edge above buildings/trees | Shelf-like front geometry, layered opacity, depth, a ragged turbulent underside and coherent movement |
| `IMG_0056.jpeg` | Windy satellite display with clustered lightning and an infrared brightness-temperature scale | Optional in-engine weather map, storm footprints, discharge history and an explicitly approximate satellite/IR view. The displayed `256 K` is not a measured surface temperature or proof of hail |
| `IMG_0059.mp4` (13.8 s) | Low banded cloud structure and discussion of apparent speed | Front propagation distinct from local texture drift; optional accelerated debug playback. Spoken speed estimates are not calibration data |
| `IMG_0068.mp4` (24.5 s) | Rain and wind near a roof; source orientation differs from other clips | Wind-driven precipitation and shelter transitions, with media orientation normalized for visual comparison |
| `IMG_0070.mp4` (195.2 s) | Dense rain veil, vegetation movement, roof-edge water and changing visibility | Shared gust field, rain curtains, roof runoff, impact sound and wet surfaces; do not infer confirmed hail size from appearance/audio alone |
| `IMG_0071.mp4` (36.9 s) | Visible cloud field and spoken surprise at the wind current/direction | Different directions at different altitudes; cloud motion independent of the viewer and surface vegetation |
| `IMG_0082.mp4` (127.53 s), `IMG_0083.mp4` (6.0 s) | Additional explanations of cloud movement, with broken layered storm cloud | Use the timestamped transcription to drive layer counterflow, shape evolution and wind-profile acceptance scenes |

A handheld view does not establish true wind vectors, altitude, rotation or storm class. Use stationary landmarks for comparisons. The shapes support a shelf/front visual target; they do not prove a supercell, tornado or measured cold pool.

## 4. Proposed built-in feature catalogue

Priorities: **P0** foundations; **P1** requested storm behavior; **P2** environmental integration; **P3** optional expansions. Every item needs a working default, authoring controls, headless sampling where applicable, persistence, and an example scene.

### 4.1 Air, winds and changing clouds

- [ ] **P0: Atmospheric sounding.** An ordered height profile of temperature, pressure, vapour and horizontal wind; derive relative humidity, freezing level, wet-bulb temperature and stability from it. Support inversions, dry middle layers and humid near-surface air.
- [ ] **P0: Multi-layer wind profile.** Independent horizontal vectors, turbulence and mixing by altitude; smooth transitions with explicit shear thickness. Surface flow, storm inflow, mid-level steering and anvil flow may disagree or oppose each other.
- [ ] **P0: Cloud deformation.** Large structures advect at their sampled altitude; differential advection stretches them. Updrafts raise and replenish them; dry-air entrainment erodes them. Small detail follows the same transport, with bounded additional turbulent motion.
- [ ] **P1: Coherent gusts and lulls.** Spatially correlated wind pulses with duration and propagation, shared by rain, vegetation, flags, loose objects and audio.
- [ ] **P1: Turbulent billows and scud.** Local vortical motion along strong shear and front boundaries; ragged low cloud forms where lifted moist air saturates.
- [ ] **P2: Terrain effects.** Ridge lift, valley channeling, lee drying and roughness-based surface reduction. Buildings receive a local wake/shelter approximation; these approximations never become an implicit full CFD promise.
- [ ] **P3: Anvil, virga, mammatus-like and wave-cloud presets.** Use common moisture/wind/phase fields; label unresolved shapes as appearance models rather than new conserved physics.

### 4.2 Storm cells and fronts

- [ ] **P0: Persistent cell identity.** Seeded initiation, position, steering, life stage, updraft/downdraft cores, water supply and a bounded storm footprint.
- [ ] **P1: Cell lifecycle.** Developing, mature and dissipating phases follow moisture, instability, entrainment and precipitation feedback, with an authored mode for narrative control.
- [ ] **P1: Outflow/gust front.** Rain-cooled air spreads away from a downdraft and lifts warm inflow at its boundary; the wind shift can arrive before the rain.
- [ ] **P1: Shelf and roll appearance.** A shelf remains associated with a lifting outflow edge; a detached roll is a separate appearance/state transition. Do not attach an identical shelf mesh to every storm.
- [ ] **P1: Cell groups.** Neighboring cells share moisture and cold pools; merge outflows and allow conditional new cells along convergence. Preserve parent identities and deterministic merge/split rules.
- [ ] **P2: Squall lines and bowing fronts.** Chains of cells with along-line variability and locally stronger rear-inflow/outflow; reuse the front system.
- [ ] **P2: Air-mass fronts.** Separate authored cold/warm/stationary boundaries from convective outflow. A large-scale front can initiate cells without being the same object as their gust fronts.
- [ ] **P3: Rotating storms.** Optional storm-scale rotation driven by the sounding and resolved/parameterized dynamics. Funnel/tornado gameplay is a separately validated feature, never inferred automatically from every turbulent base.

### 4.3 Rain, ice and hail

- [ ] **P0: Separate hydrometeor reservoirs.** Cloud liquid, cloud ice, falling rain, snow and graupel/hail have explicit mass and phase transfers.
- [ ] **P1: Rain cooling.** Evaporating falling rain increases vapour and removes sensible heat; near saturation the evaporative sink weakens. Cooling feeds buoyancy and the cold pool.
- [ ] **P1: Hail growth aloft.** Embryos collect supercooled liquid in an updraft; growth is limited by available water and the ability to release freezing heat.
- [ ] **P1: Hail descent and survival.** Size, density, ventilation, humidity, temperature, descent time and meltwater shedding control ground hail. Melting transfers mass to liquid and absorbs heat.
- [ ] **P1: Mixed precipitation transitions.** Rain can precede hail; larger hail can survive while smaller stones melt; later rain can carry melted hail water. No instantaneous global phase swap.
- [ ] **P1: Slanted/bent rain shafts.** Altitude-dependent wind and particle inertia curve the fall path, including strong outflow near the ground.
- [ ] **P2: Virga and dry downbursts.** Precipitation can evaporate aloft while a cooling-driven outflow still reaches the surface.
- [ ] **P2: Accumulation and melting.** Persistent surface water equivalent, snow and hail mass, with material temperature and drainage. Hail melting can continue after the storm passes.
- [ ] **P3: Freezing rain and icing.** Require supercooled liquid and a suitable surface energy balance; distinct from hail formation.

### 4.4 World, sound and tooling

- [ ] **P1: Shared wind sampling.** Physics, vegetation, effects and sound consume copied samples with position, height, tick and validity. Strong wind is not a renderer-only animation.
- [ ] **P1: Surface-aware hail impact.** Authoritative impact swaths/damage from mass and speed; bounded cosmetic bouncing stones. Metal, glass, soil and water have different sounds and effects.
- [ ] **P2: Roof interception and runoff.** Captured rain becomes gutters, drips, overflow or sheets according to catchment area and storage. Roof clipping alone does not create runoff.
- [ ] **P2: Wetness and puddles.** Surface rain flux, absorption, evaporation and drainage drive wetness, roughness and puddle depth; roofs and terrain must not receive the same water twice.
- [ ] **P2: Dust at outflow boundaries.** Dry exposed soil emits dust according to surface wind stress, moisture and cover; rain suppresses emission.
- [ ] **P2: Storm audio.** Rain/hail intensity by material, wind buffeting, roof roar and sheltered transitions. Extend existing delayed thunder with channel-distance rumble and bounded indoor filtering.
- [ ] **P2: Lightning context.** Link strike rates and locations to cell mixed-phase/updraft diagnostics. In-cloud and ground channels share authoritative identities. Do not claim a fully resolved electrical solver.
- [ ] **P2: Weather inspector.** Sounding/hodograph, front contours, updrafts, cold-pool depth, particle trajectories, hail size bins, water/energy residuals and per-stage cost.
- [ ] **P2: Motion-comparison tools.** Freeze a reference frame, stabilize against landmarks, show frame differences/optical-flow tracks and color cloud tracers by altitude. Keep image-plane motion distinct from a recovered physical wind vector.
- [ ] **P2: Weather maps.** Top-down precipitation rate, wind vectors, cell IDs, lightning age and cloud-top diagnostic. Simulated radar reflectivity/IR are labeled approximations and separate from physical rain rate.
- [ ] **P2: Reusable scenarios.** Built-in recipes for a layered gust front, wet microburst, dry microburst, rain-to-hail transition and post-storm clearing; every recipe uses the same system.

## 5. Canonical representation and ownership

Extend `scene::WeatherCompute` with bounded semantic profile/cell/front settings or ECS-owned related components. Prefer named subrecords over a long list of unrelated scalar controls. Names and units below are proposed API design, not existing symbols.

| Proposed record | Core fields | Ownership and meaning |
| --- | --- | --- |
| `WeatherSounding` | Sorted `HeightMetres`, `TemperatureCelsius`, `PressurePascals`, `VapourKgPerKgDryAir`, `WindMetresPerSecond` knots | ECS-authored environment; explicit world-height or terrain-relative coordinate policy |
| `WeatherWindProfile` | Vector knots, reference height, transition thickness, surface roughness, turbulence energy/length/time scales | One accepted profile sampled by all consumers; a cloud layer is not a separate conflicting source of wind |
| `StormCell` | Durable text ID, birth tick, position, stage, updraft/downdraft radii, moisture supply, steering policy, parent IDs | ECS-owned authored or evolved cell state; procedural descriptors and stateful diagnostics are different modes |
| `WeatherFront` | Durable ID, kind, oriented curve/level set, thickness, air-mass contrast or cold-pool diagnostic | Distinguish imposed synoptic forcing, analytic procedural outflow and diagnosed stateful outflow |
| `MicrophysicsSettings` | Versioned phase model, collection/ventilation coefficients, hail bins, fall-speed law and validity bounds | Physical-model changes start a compatible new physical epoch |
| `WeatherSurfaceState` | Water/snow/hail mass per area, temperature, wetness, drainage and catchment identity | Optional ECS state at the surface integration boundary; not owned by the cloud renderer |

Use one water convention consistently. Keep vapour as kg/kg dry air if retaining the current API; store condensates as kg/m³. Convert using the declared reference dry-air density before any transfer. Do not add these representations without conversion.

Extend full physical state for separate phase masses, particle number/size moments or bins, phase thermal energy, required turbulence memory and background profiles. The eight-byte cell stays a derived display encoding. It cannot encode a hail size distribution or a replayable cold pool. Keep separate diagnostic pressure and pressure-projection correction.

The current constant-density solver can be extended as a local-domain Boussinesq-like approximation. For deep storm columns, introduce a versioned anelastic/reference-density model before claiming physically credible full-depth convection. Its constraint is `div(rho0(z) u) = 0`; projection, scalar fluxes, dry-air weighting and checkpoints all change together. Do not simply add a lapse rate to the existing energy equation and claim adiabatic cooling.

## 6. Algorithms

These are proposed engineering algorithms. Qualitative weather mechanisms are supported by the primary references in section 12. Numerical choices, budgets and coefficients require engine fixtures and calibration.

### 6.1 Multi-layer winds and cloud motion

1. Validate finite, ordered, uniquely spaced height knots and bounded vectors. Interpolate vector components, not compass bearings: interpolating 359° and 1° must not turn through 180°. Use linear interpolation first; optional monotone smooth interpolation must not overshoot authored speed limits.
2. Sample a shared field: `u(x,z,h,t) = U_profile(h,t) + u_cell + u_front + u_turbulence + u_terrain`. Give every contribution a declared role so stateful solved outflow is not added again as an analytic gust.
3. Retain cumulative accepted displacement at each procedural profile knot: `D_k(t) = integral(U_k(t), dt)`. Reanchor every affected knot at the edit tick using the old controls first, including retained fractions. Interpolate displacement consistently with the same profile weights. Constant-profile direct seeking remains bounded arithmetic.
4. For height-only horizontal shear with no vertical motion, sample the original density in material coordinates `(x - D_x(h,t), h, z - D_z(h,t))`. This directly creates differential layer drift without independently sliding every noise octave.
5. General vertical motion, changing height, entrainment and nonlinear distortion require a stateful flow map or transported cloud fields. A simple displacement lookup is not an exact trajectory through a changing three-dimensional wind field. Keep an analytic cosmetic mode explicitly approximate.
6. In stateful mode, advect moisture, condensates and energy with the same projected velocity. Cloud growth/erosion follows phase changes; density/detail reconstruction uses transported material coordinates or stable phases. Do not reseed cloud shape every frame.
7. Reproject temporal cloud history using local motion integrated over the actual interval, plus camera motion. Reject history when transmittance, topology or phase changes invalidate it. A single global wind offset will ghost counter-moving layers.

The extra narration (`IMG_0082.mp4`, approximately 00:20-00:59) describes upper/lower counterflow, curling movement and cloud shapes that resemble fire. Reproduce that with vertical shear, local lift, turbulent entrainment and condensate evolution. Vertical shear can generate horizontal vorticity; a vertically rotating storm requires additional dynamics such as tilting/stretching. Counterflow alone does not prove a tornado or a vertically spinning cloud.

Example acceptance profile, an artistic fixture rather than measured wind from the media: low cloud at 0.5 km moves east at 8 m/s, middle cloud at 2 km moves west at 12 m/s, and upper cloud at 7 km moves northeast at 25 m/s. Surface vegetation follows a separate low-level sample. Transition bands deform continuously; editing one knot does not teleport cloud.

### 6.2 Momentum, buoyancy and turbulence

The existing solver advances acceleration, pressure and relaxation, then transports scalars. Add explicit momentum transport so resolved eddies and gust fronts can carry their own velocity.

- Use a conservative, bounded staggered-grid momentum scheme with deterministic face traversal. Add validated higher-order flux reconstruction only after the first-order reference passes; pair it with positivity-preserving scalar transport.
- Replace uniform-temperature buoyancy with perturbations about the declared sounding. A local dilute approximation is `B = g * ((T - T0)/T0 + 0.61*(r_v-r_v0) - r_cond)`, with absolute temperatures and all mixing ratios on the same dry-air basis. Treat this as a model approximation. Deep columns need the anelastic upgrade described above.
- Include precipitation loading and a consistent particle/air drag exchange. Do not count suspended weight, terminal drag and hail momentum exchange as three independent copies of the same force.
- For procedural cosmetic turbulence, take a curl of a smooth vector potential to create correlated vortical motion. Use world coordinates, separate random purposes and accepted time; preserve mean flow. This produces an incompressible disturbance in the continuum, but boundary handling and discrete divergence still need checks.
- For stateful turbulence, begin with resolved momentum plus a bounded subgrid eddy viscosity/diffusivity from strain and mixing length. Add prognostic turbulent kinetic energy only if measured quality needs it. Its production, transport and dissipation become checkpoint state; dissipated kinetic energy is accounted for under the declared energy model.
- Limit turbulence by grid resolution and stability; increase activity at front shear and terrain roughness. Optional cosmetic vorticity enhancement must stay out of authoritative forces unless its energy injection is explicitly budgeted.

Compute a conservative substep bound from all face velocities, maximum hail/rain settling speed, diffusion, relaxation and microphysics rates. The relevant CFL sum includes transport on all axes. Do not retain a rain-only terminal-speed bound after hail is introduced.

### 6.3 Cells, cold pools and fronts

**Procedural mode:** retain bounded seeded descriptors for cell center, lifetime, steering, updraft and downdraft volumes. Separate cell translation from internal winds and from gust-front expansion. An analytic signed-distance field around an oriented, deformable front curve defines a finite-width lifting zone, cloud shelf and outflow transition. Use smooth distance responses and seeded along-front variation rather than a circular ring visible in every storm.

**Stateful mode:** diagnose cells from coherent updraft/condensate regions with minimum size and temporal hysteresis. Match connected regions across ticks by overlap and distance with deterministic tie-breaking. New IDs do not depend on ECS iteration order. Splits record parents; mergers retain a stable lineage.

Rain evaporation and hail melting produce negative buoyancy. Integrate negative buoyancy over the diagnosed cold-pool depth to obtain a diagnostic propagation scale, `c ≈ sqrt(max(0, -2 * integral(B dz)))`. This is a density-current estimate, not an extra acceleration to add to already solved outflow. Extract the leading near-surface temperature/buoyancy-gradient contour for front rendering, events and queries.

In coarse/procedural mode only, a parameterized cold-pool depth and energy deficit can evolve with explicit rain cooling input, entrainment, surface heating and decay. Propagate the boundary using that estimate plus ambient advection. Document the reduced budget; do not call it exact three-dimensional conservation. Merge overlapping cold pools without adding their masses/energy twice.

Lift moist inflow along convergence boundaries. Condensation determines whether a shelf/scud cloud is visible; a dry front may carry dust without a cloud bank. Conditional initiation requires moisture, instability, lift and a cooldown/spacing rule. Outflow can also undercut a parent updraft and end the cell.

### 6.4 Falling-rain evaporation and actual cooling

Introduce phase-aware saturation `r_sat(T,p,phase)` and consistent thermodynamic helpers. The present consumer's Tetens function and the solver's exponential proxy must not silently become two different physical models. Preserve the old model under its version when loading old state.

For a substep, let `Delta m_evap` be evaporated rain concentration in kg/m³, `rho_d` dry-air density in kg/m³ and `c_pd` dry-air heat capacity. A simple local transfer obeys:

```text
rain_new   = rain_old - Delta m_evap
r_v_new    = r_v_old + Delta m_evap / rho_d
Delta T    = -L_v * Delta m_evap / (rho_d * c_pd)
```

This is the leading-order cooling relation; the complete model also includes condensate sensible heat and particle heat exchange. Compute the rate from particle surface area/size, ventilation and vapour deficit. Solve the finite transfer together with temperature and saturation using a bounded iterative solve or a verified analytic relaxation. Limit by available rain and the energy/water solution; a simple “initial humidity deficit” cap can overshoot final saturation because evaporation also cools the air.

As the air cools and humidifies, evaporation slows. In the isolated adiabatic parcel limit it approaches the wet-bulb state; mixing, radiation and external forcing can change that limit. Rain is not a fixed temperature offset. A dry column can cool strongly while losing much of its rain before the ground; an already saturated column does not receive the same evaporative sink.

Update buoyancy from the resulting thermal/moist state within the substep. Keep cloud evaporation, rain evaporation and ice sublimation separate transfers with their appropriate latent heats. Condensation/freezing release latent heat; evaporation/melting/sublimation absorb it.

### 6.5 Hail growth, transport and melting

Start with a bounded bulk size distribution, for example four hail diameter classes spanning small graupel through large hail, plus mass and number concentration per class. Diameter class edges, density and transfer laws are versioned settings; these are not universal meteorological categories. Remap growth and melting conservatively between classes. An optional small set of representative trajectories supports debug/close-view detail, without replacing the canonical bulk inventory.

For a representative stone, diameter `d`, radius `r`, mass `m`, area `A = pi*r²`:

```text
dx/dt       = v_particle
dv/dt       = gravity + drag(u_air - v_particle) / m
dm_collect/dt = E_collect * A * C_supercooled * |u_air - v_particle|
```

`C_supercooled` is liquid concentration in kg/m³ and collection efficiency is dimensionless. In a bulk terminal-speed model, use `u_air - v_terminal*vertical_unit` for transport instead of integrating drag again. A sphere-based diagnostic is `v_terminal ≈ sqrt(2*m*g/(rho_air*C_d*A))`; actual drag/density/shape laws need bounded calibration.

Growth requires embryos and available supercooled droplets. Permit liquid below 0°C; an instantaneous all-liquid-to-ice rule would remove the water hail needs to collect. Freeze collected liquid only at a rate the stone's energy balance permits. Dry growth freezes collected water promptly; wet growth retains/sheds liquid when heat removal is insufficient. Growth consumes cloud liquid and releases freezing heat to the stone/air system. Track a bounded embryo source and its mass transfer rather than creating hail mass from a probability.

For heat transfer use a declared ventilation law. A prototype sphere relation is `Nu = 2 + 0.6*sqrt(Re)*Pr^(1/3)`; do not extrapolate this low/moderate-Reynolds-number relation across large hail without a validated high-Re correction or hail-specific parameterization, with `Re = rho_air*|v_rel|*d/mu`. Surface heat exchange then has the form `Q_conv = Nu*k_air*pi*d*(T_air - T_surface)` in watts. Add vapour exchange, collected-water enthalpy and relevant radiation as separate terms. Warm ice toward 0°C before allocating positive net heat to melting; at the melt point `dm_melt/dt = max(Q_net,0)/L_f`. Bound by remaining ice, and transfer meltwater to the correct reservoir.

Particle thermal state and liquid coating cannot disappear between ticks. Bulk classes need retained energy or a documented equilibrium approximation that closes the same budget. Particle shrinkage changes fall speed and therefore descent time. Hail can fall when updraft support weakens or particle settling exceeds lift; repeated cycling is possible, not mandatory for every stone.

**Rain cooling feedback for hail:** growing hail aloft enters an evolving lower column. Rain evaporation can lower air/wet-bulb temperature; hail melting also removes heat; both can strengthen the cold pool. Stones sample that updated column along their descent. This can reduce subsequent melting and change ground hail size, time and swath. Rain does not automatically freeze hail at ground level or guarantee its survival. The test must compare otherwise identical warm columns with cooling enabled/disabled and close the mass/energy balance.

### 6.6 Precipitation flux and surfaces

Derive phase-specific water-equivalent precipitation from completed downward mass flux through a face:

```text
R_phase [mm/h] = 3.6e6 * F_mass [kg/(m²*s)] / rho_water [kg/m³]
              ≈ 3600 * F_mass, for rho_water = 1000 kg/m³
```

Include air vertical velocity and phase/size-specific settling. Ground precipitation is a surface flux, not a sum of cloud conversion rates along the column. Hail number and impact energy are additional outputs; water-equivalent rate alone cannot determine damage or hail diameter.

Use swept collision queries for sparse authoritative hail impacts or canonical impact patches, with a deterministic sampling/budget policy independent of visible particles. Cosmetic counts must not change damage. Keep nearby bounces GPU/client-side unless a specific object collision has authoritative gameplay relevance.

Interception transfers atmospheric precipitation to a surface reservoir exactly once. Terrain, roofs, open water and porous materials have explicit deposition policies. Roof runoff rate follows catchment inflow minus storage change, evaporation and drainage. Emit cosmetic drips/sheets from that outflow. Wetness darkening, puddle depth, hail bounce and sound consume the same surface state.

### 6.7 Water and energy accounting

Define a thermodynamic reference with liquid water at 0°C as zero phase enthalpy. Then, for a local constant-reference-density model, a compatible diagnostic is:

```text
H/V = rho_d*c_pd*T
    + rho_d*r_v*(L_v0 + c_pv*T)
    + C_liquid*c_l*T
    + C_ice*(c_i*T - L_f0)
```

`T` here is Celsius relative to the declared 0°C reference; phase concentrations have kg/m³ units. Sum cloud/rain liquid and cloud-ice/snow/hail at their own retained temperatures when these differ from air. Use temperature-consistent latent heat definitions. Kinetic/potential energy and drag dissipation require a separately declared mechanical-energy policy; do not claim total atmospheric energy conservation from this thermal diagnostic alone.

Every internal phase transfer is equal and opposite in water mass and consistent with the phase enthalpies. Include vapour and hydrometeors in transport boundary fluxes. Surface precipitation carries mass and thermal energy out of the atmospheric domain; surface coupling receives them. Authored moisture, cooling, heating, turbulence forcing and lateral inflow have named external budgets.

Report water and thermal-energy residuals before any correction. Reject an invalid trial tick transactionally. Small roundoff tolerances must be scaled and tested, not used to hide negative concentrations or an unstable step.

## 7. Fixed-tick integration order

Retain the existing accepted-definition, clock and transactional-publication design. A proposed weather tick is:

```text
1. Resolve accepted controls, sounding/profile changes and era compatibility.
2. Select deterministic substeps from declared transport and process bounds.
3. For each substep:
   a. Apply external sources and accepted boundary/profile forcing.
   b. Advect momentum; apply buoyancy, consistent precipitation drag and mixing.
   c. Project velocity with the selected reference-density/boundary model.
   d. Transport vapour, phase masses, phase energy and number/size state.
   e. Solve bounded condensation, evaporation, freezing, growth and melting.
   f. Accumulate boundary/surface fluxes, budgets and diagnostics.
4. Diagnose cells/fronts from completed state; schedule bounded copied events.
5. Validate positivity, finite ranges, CFL, divergence and conservation residuals.
6. Publish the complete tick; consumers/rendering see only accepted publication.
```

Cooling from step 3e changes buoyancy on the next substep. If coupling error is too large, use a tested predictor/corrector or split microphysics around dynamics with a final projection. Include this coupling error in timestep-convergence tests. A five-second maintenance pass remains a resource-maintenance cadence, never the physical timestep.

## 8. Integration with the repository

| Location | Planned change |
| --- | --- |
| `mono.engine/scene` | Authored semantic sounding/profile/cell/front settings, validation, reflection and accepted-definition records |
| `mono.engine/weather` | Shared profile evaluation, procedural anchors, momentum/microphysics/cold-pool algorithms, full state, checkpoints, sampling and events |
| `mono.engine/resources/shaders` | Matching versioned procedural semantics; cosmetic cloud advection, phase presentation and GPU validation paths |
| `mono.engine/render` | Packed derived layouts, cloud motion/history, rain shafts, near precipitation, shadows, shelter and fallback behavior |
| `mono.engine/replication` | Bounded copied definitions, new checkpoint/event codecs and compatibility refusal; no GPU readback |
| `mono.engine/game` | Host integration for collision terrain, wind-to-physics and surface interception; preserve the existing `WeatherGround` adapter role |
| `mono.engine/script`, VM adapters | Extend existing `WeatherCompute:Sample`; expose owned profile/column/trajectory diagnostic records and documentation in both languages |
| `mono.engine/audio`, `mono.client`, `mono.studio` | Engine-owned sound consumers and editor controls; applications only wire services and present controls |
| `mono.engine/examples` | Small scenario scenes with fixed reference cameras and reproducible parameter sets |
| `mono.tools` | Component purpose entries, generated bindings/checks, architecture metadata if edges change and test recipes |

Weather remains L8 shared and cannot depend on renderer, physics, replication, effects or audio. A host adapter passes copied weather values to physics; do not introduce a sideways `physics -> weather` dependency. New core weather code stays under `mono.engine`, as required by the repository policy. ECS owns canonical storage; caches contain derived indices, compiled arithmetic or resources only.

Extend sample records with optional `CellId`, `FrontDistanceMetres`, `WetBulbCelsius`, phase fluxes, `HailDiameterPercentilesMetres`, `ColdPoolDepthMetres`, `TurbulenceRmsMetresPerSecond` and a named validity/model mode. Keep exact tick/epoch string conventions in scripts. A query outside a physical domain returns an explicit unavailable or environmental continuation result; it must not invent a simulated rain-cooled column below a cloud-only grid.

Multiple authored cloud providers may sample the same environment wind profile. For overlapping physical volumes, require explicit ownership/composition: one authoritative phase inventory per region, or conservative exchange across declared boundaries. Never sum duplicated precipitation from overlapping layers.

### Persistence and compatibility

- Add a new checkpoint schema and physical-model version. Persist all phase masses, thermal state, size/number state, sounding/profile forcing history, cell lineage if authoritative, front state if parameterized and all required turbulence history.
- Preserve complete old-model loading, or refuse an incompatible old checkpoint with a named reason. A v3 checkpoint cannot recover missing hail/cold-pool history by reseeding.
- Profile/wind edits reanchor procedural motion and enter tick-stamped forcing for stateful evolution. Define which physical edits start a new epoch; changing model/size-bin topology cannot reinterpret old arrays silently.
- Use stable text IDs across save/network boundaries; process-local interned numbers and GPU slots never become durable identities.
- Extend events only with bounded copied values and versioned names. Keep exactly-once receipt guards and bounded history. If history was dropped, report that instead of replaying invented impacts.

## 9. Performance and scalable quality

Keep authority independent of camera, graphics backend, render LOD and particle count. Use one bounded canonical domain initially; regional fixed domains with conservative exchange are a later measured extension. Sparse render paging is not adaptive physical simulation.

The current base physical cell is 32 bytes and falling liquid adds 4 bytes per cell, excluding terrain, forcing, scratch, checkpoints and GPU resources. A `64³` domain therefore already needs 9 MiB for those cell arrays alone. Six additional FP32 phase/moment fields add 6 MiB; four hail bins with both mass and number add another 8 MiB. Phase energy and transactional workspace add more. These arithmetic examples expose cost, not a proposed complete layout. Reconcile with the current 32 MiB state budget before implementation.

Prefer structure-of-arrays phase storage for measured transport locality. Allocate only the fields required by an enabled physical model. A zero-hail model may omit hail arrays if the schema records that choice; it cannot discard active hail history when the camera moves away. Put hard ceilings on knots, cells, fronts, bins, trajectories, events, replay ticks and diagnostic captures.

Presentation tiers can reduce ray samples, distance, cloud shadow updates, particles and close-up bounces. They keep phase rates, event identities and canonical clouds consistent. Under resource failure retain a compatible completed publication or use the existing graphics fallback. Reduced visual quality must not switch the authoritative physical model.

Profile separate stages with existing engine profiling/metrics: momentum, projection, transport, each microphysics process, cell/front diagnostics, checkpoint encode/decode, queries, cloud reconstruction and effects. Report p50/p95/max tick cost, bytes live/peak, allocations, network bytes and residuals. Record hardware, preset, backend, grid, active phases and weather parameters. No new performance claim is made here.

## 10. Implementation stages and acceptance gates

Stages are dependencies, not release-date promises. Do not label the whole catalogue complete after delivering only its first stage.

| Stage | Deliverable | Gate |
| --- | --- | --- |
| 0: Reference contract | Pin baseline, add schema/units design, media traceability and reproducible scenario definitions | Existing weather cases remain the baseline; known portal/handoff failures stay visible |
| 1: Layer winds | Accepted profile knots, cumulative procedural anchors, shared sampler and multi-layer cloud motion | Counterflow scene passes; edits are continuous; scalar/batch/GPU procedural parity is verified |
| 2: Dynamic transport | Momentum advection, sounding perturbations, bounded mixing and revised stability schedule | Vortex/gust transport, pressure, boundary and timestep-convergence fixtures pass |
| 3: Thermal rain | Rain evaporation, wet-bulb diagnostic, phase-aware saturation and revised thermal budget | Dry/saturated column comparisons and water/energy closure pass |
| 4: Ice and hail | Supercooled liquid, embryos, size bins, growth, settling, melting and new checkpoints | Growth water is consumed; meltwater is retained; cooled-column ground hail differs correctly |
| 5: Cell-front system | Persistent cells, diagnosed/parameterized outflows, shelf/scud and lifecycle | Gust front can precede rain; outflow collision and cell merge/split replay consistently |
| 6: World response | Surface deposition, runoff, wind consumers, materials/audio and tooling | Roof/world water balance and shared gust timing pass; particle LOD does not change damage |
| 7: Quality validation | Media-inspired scenarios, fallback, late join, mirrors/portals and performance | Real-device visual review and named budgets pass on each claimed backend |

### Required headless fixtures

- **WindProfile:** vector interpolation across north, negative heights, bounds, inversion/transition layers, invalid knots and equal/opposite flows.
- **CloudAdvection:** analytic shear translation, height-changing parcel distinction, profile edit continuity, very large ticks, pause/resume, origin rebasing and scalar/batch parity.
- **Momentum:** advect a vortex/gust, preserve uniform flow, enforce boundary fluxes and demonstrate resolution/timestep convergence with tolerances appropriate to the model.
- **RainCooling:** isolated dry parcel cools/humidifies as rain decreases; saturated parcel has reduced evaporative sink; no water/enthalpy creation and no oversaturation caused by a stale cap.
- **HailGrowth:** no growth without available supercooled liquid; collection consumes liquid; weaker updraft shortens retention; freeze heat is accounted for.
- **HailSurvival:** paired identical fall columns with rain cooling enabled/disabled; smaller stones melt more readily under the chosen law; positive surface temperature alone does not force zero hail; bin remap conserves mass/number according to its declared rule.
- **ColdPool:** negative buoyancy follows cooling; diagnosed front advances continuously; moisture controls shelf/scud visibility; two outflows merge without double inventory.
- **SurfaceBudget:** deposition is transferred once; roof catchment inflow equals storage/outflow/evaporation; no underground precipitation or hail impacts through opaque shelter.
- **ReplayAndCodecs:** continuous run equals checkpoint+replay under the engine's determinism policy; truncated/oversized/old-incompatible records refuse transactionally; all required history survives; late join and edits preserve event identity.
- **ConsumerIsolation:** changing camera, visual particles, render backend, mirror count or LOD leaves authoritative samples/events unchanged. Worker scheduling cannot reorder reductions or cell identities.

### Visual scenarios based on the supplied media

1. **Layered front approach:** low shelf-like edge, banded middle base, distinct upper drift and trees responding to surface wind. Compare fixed-camera sequences, not isolated stills.
2. **Counter-moving cloud deck:** strongly visible low/high counterflow with continuous deformation and cloud growth/erosion. Add cloud tracers and a height/wind overlay for verification.
3. **Rain-to-hail passage:** rain curtain arrives, temperature/wet-bulb diagnostics evolve, hail size and phase flux change, metal-roof impacts differ from rain, and melting persists afterward.
4. **Wet downburst:** descending rain bends into spreading surface outflow; vegetation, flags, particles and sound share gust timing.
5. **Dry outflow:** virga aloft, dust on a dry surface and a weak/absent rain footprint.
6. **Shelter and runoff:** walk from open ground beneath a roof; rain/hail clip correctly while roof-edge sheets remain outside, with conserved catchment outflow.
7. **Multi-view stress:** simultaneous ground, cloud-interior, above-cloud, mirror and portal views do not alter state or produce cloud seams/history ghosts. Recheck the existing portal black-frame/handoff findings explicitly.

Visual thresholds need reviewed reference sequences and measured tolerances before becoming release gates. Do not assert backend parity from one Vulkan run, or photorealism from passing semantic tests.

## 11. Decisions settled by this plan

- Build on existing weather, clocks, checkpointing, GPU residency and events.
- Make altitude-dependent vector winds a first-class shared input.
- Treat cloud shape change as transport plus source/sink evolution; use direct procedural displacement only within its stated assumptions.
- Add genuine rain evaporation and hail heat/mass exchange before calling hail thermodynamic.
- Keep cells, ambient fronts and gust fronts distinct, with one canonical physical cause for each stateful effect.
- Use bulk hail distributions for world simulation and bounded cosmetic/diagnostic trajectories for detail.
- Preserve fixed ticks, deterministic identity, ECS ownership and complete stateful history.
- Deliver foundations and coupled storm behavior before optional tornadoes, icing or sophisticated synthetic instruments.

## 12. Primary technical references

These support the physical mechanisms; the proposed solver and real-time approximations still require independent validation. The sphere heat-transfer relation in section 6.5 is a prototype, not a hail-calibrated universal law. Hail growth/melt and terminal-speed parameterization choices must be included in sensitivity tests.

- [NOAA NSSL: hail basics](https://www.nssl.noaa.gov/education/svrwx101/hail/): updraft-supported hail, supercooled droplet collection and wet/dry growth.
- [NOAA JetStream: hail](https://www.noaa.gov/jetstream/hail): growth, loss of updraft support and melting during descent.
- [NOAA JetStream: damaging wind](https://www.noaa.gov/jetstream/wind_damage): precipitation evaporation, cooled downdrafts and spreading gust fronts.
- [Bureau of Meteorology: observing thunderstorms](https://www.bom.gov.au/storm_spotters/handbook/observing_thunderstorms.shtml): shelf/scud appearance, outflow, rain curtains and dry-region downbursts.
- [Bureau of Meteorology: thunderstorm structure](https://www.bom.gov.au/storm_spotters/handbook/thunderstorms.shtml): core/anvil/inflow-outflow organization and height-dependent winds.
- [ECMWF Metview: thermodynamic functions](https://confluence.ecmwf.int/pages/viewpage.action?pageId=247890548): pressure/temperature/humidity units and saturation/mixed-phase conventions. This is an archived documentation page; pin the implemented coefficient set and validity interval in the model version.

- [Morrison and Milbrandt (2015): predicted particle properties microphysics](https://doi.org/10.1175/JAS-D-14-0065.1): a research reference for prognostic ice properties; the four-bin game design above is a separate engineering proposal.
- [List (2014): New Hailstone Physics, Part I](https://doi.org/10.1175/JAS-D-12-0164.1): hail heat/mass exchange and growth modeling.
- [Vagasky et al. (2025), NASA-hosted HAILMELT study](https://ntrs.nasa.gov/citations/20250003323): sensitivity of falling hail to melting, terminal velocity and atmospheric environment.

## Appendix A. Timestamped media transcription

Transcribed from all six audio tracks using a local English speech model. This is a machine draft, with approximate clip-relative timestamps. Rain and wind obscure several passages. Square brackets identify uncertainty or non-speech; uncertain text is not a measured weather observation. Repeated model-generated phrases over weather noise have been excluded rather than presented as speech. Spoken claims about hail or lightning are the speaker's observations, not independent confirmation.

### IMG_0059.mp4

| Time | Transcript |
| --- | --- |
| 00:00-00:05 | “That actually might be like 50 kilometres per hour.” |
| 00:05-00:07 | “50? Or 40?” |
| 00:07-00:13.8 | [Wind; remaining speech unclear. Recognition suggests a question involving thunder, but the wording is unreliable.] |

Design use: apparent front/cloud speed and a low structured cloud edge. The spoken estimate is not a measured wind speed.

### IMG_0068.mp4

| Time | Transcript |
| --- | --- |
| 00:00-00:20 | [Strong rain/wind; any background speech is not reliably intelligible.] |
| Approximately 00:20-00:24.3 | “When did you get here?” |

Design use: near-roof rain and wind. No reliable technical explanation was recovered from this clip.

### IMG_0070.mp4

| Time | Transcript |
| --- | --- |
| 00:00-00:06 | [Unclear reaction about the storm's intensity; recognizer drafts disagree on the wording.] |
| Approximately 00:06-00:07 | “Oh my God.” |
| 00:07-03:00 | [Predominantly heavy rain, wind and roof/water noise; no reliable continuous speech recovered.] |
| Approximately 03:00-03:15.2 | [Unclear speech. Recognition suggests “… can shock me,” but the preceding words are unreliable.] |

Design use: rain curtains, wind in trees, visibility and roof runoff. Do not turn the unclear reaction into a hail-size measurement or an electrical observation.

### IMG_0071.mp4

| Time | Transcript |
| --- | --- |
| 00:00-approximately 00:22 | [Weather/background sound; no reliable speech recovered.] |
| Approximately 00:23-00:25 | “Oh, that's going that way, what the fuck is going on?” |
| Approximately 00:32-00:33.5 | “That's a strange wind current.” |
| Remaining time to 00:36.9 | [Background/weather sound.] |

Design use: changing cloud direction and differing flow at different heights.

### IMG_0082.mp4

This is the main explanatory cloud-movement clip. The directional terms refer to the speaker's camera and pointing; they are not absolute compass bearings.

| Time | Transcript |
| --- | --- |
| 00:00-00:05.1 | “I don't know if you can tell here, but that's coming towards us, so kind of just the second…” |
| 00:05.1-00:11.8 | “…wave, and that's all the angry hailstorm over there, but that's going that way, so…” |
| 00:11.8-00:15 | “…that must be coming back now.” |
| 00:15-00:27.5 | “Yeah, so that up there is [going] that way, or like sort of that angle there, and then that, that cloud down…” |
| 00:27.5-00:35.4 | “…there, this cloud down here is going like over. It's like fire almost, which is…” |
| 00:35.4-approximately 00:40 | “…pretty cool. Oh yeah, now that's speeding up, so your top clouds are going…” |
| 00:40-00:45.5 | “…this way, sort of like that way. Then the [hill/hail, unclear] clouds are sort of going over the hill now.” |
| 00:45.5-00:48.9 | “So it's going like it's being sucked that way and then brought back around.” |
| 00:49.8-00:53.7 | “Although it's probably just low winds going that way, high winds coming this way…” |
| 00:53.7-00:59.8 | “…and that sort of creates a spin, and that spin can be the clouds.” [Final clause uncertain.] |
| 01:00-01:15 | “It might be hard to tell, but if you do a delta frame here, you can see it moving.” |
| 01:15-01:19.5 | “Oh, apparently that's raining right now above us.” |
| 01:20-01:26.6 | “Oh, I saw lightning in between there.” |
| 01:26.6-01:34.3 | “Oh, so that little patch there, that's going that way.” |
| 01:34.3-01:36.1 | “The wind's coming towards me now.” |
| 01:36.1-01:38.9 | “Let me see which way is it…” |
| 01:40-01:52.8 | “…about there. So the wind's coming towards me this way. That doesn't sound heavy. Is that…” [Middle phrase uncertain.] |
| 01:52.8-01:59.7 | “…the plane there? Oh yeah, so those clouds are going sort of left on the camera, but…” |
| 02:00-02:07.53 | “…towards me a little bit, and then that's for the new storm, so I'm gonna run inside.” [“for” uncertain.] |

Extracted requirements: successive storm waves; distinct upper/lower directions and speeds; curling/deforming small cloud patches; a surface gust that differs from cloud motion; subtle motion visible through frame comparison; rain/lightning integrated with the same evolving cells.

The proposed physical interpretation is vertical shear plus local storm circulation and condensation/erosion. The speaker's suggested “spin” is a useful visual requirement, not proof of a measured vortex.

### IMG_0083.mp4

| Time | Transcript |
| --- | --- |
| 00:00-00:06 | “You can see that multi-layered, so you've got this front…” [Final words unclear; recognition returned “camera on,” which is not reliable enough to treat as the intended wording.] |

Extracted requirement: clearly distinguish multiple cloud layers at a moving front. Preserve independent layer motion while keeping the full storm coherent.

