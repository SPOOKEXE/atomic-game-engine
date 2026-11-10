# Engine feature roadmap

Current development milestone: v0.26.1.

Development branch: `v0.26.1`.

Each version remains active until its outstanding validation and optimization gates are (fully) complete or with user's permission to defer only when it cannot be completed by means.

# Zero v0.26.1

- Finish ongoing work and unfinished items (check docs)
- Find and fix bugs in studio, interact with every menu and mutate what you can. Test all features through creating scenes.
- Preserve active play runs across factory world replacement.
- Preserve scheduler and cadence state across factory swaps, including plugin-close edits.
- Reduce development build turnaround.
- Cleanup `just` jobs file and group them by categories.
- Optimise the `TornadoSim` compute (gpu-transparent, gpu clouds-compute, Universe::Tick tick exchange integration and jobs.assigned). Also there may be a frame drop every so often which might be the cause of the spikes instead of this high compute value.
- Empty studio scene has un-necessary computations:
* fog being recomputed on cpu, should be full resident gpu now
* by default have no fog in studio, only show fog with a FogEffect
- When i click on a bar in the Frame Graph, it should "focus" the bar and make it span the entire Frame Graph, so i can inspect the subbars.
Adornments Demo (ACTIVE) - Renderer::RenderView.execute graph:
- fog computation mentioned above
- weather-reconstruct computation when no weather is enabled
- portal-overlay computation when no portals exist
- shader-lenses computation when no shader lens exists
- sky, dof, blooms, god-rays, tone-map, present, overlay computations should be optimised
- ssao also needs to be looked at for optimisations (14.66us which is MORE than anything in adornments active demo)
- overlays.overlay drawing is 0.531ms
- in the `Animations` demo, there is a huge gap in frame graph under `Universe::Tick` and another additional smaller gap as well - about 0.04ms - and its NOT serial/parallel compute related (i enabled/disabled the options and no difference).
- frame graph.average.spans takes 0.192ms in `Animation` demo.
- in `Animations` demo, see if you can optimise all descendant chains of `render preparation` - evaluate-animations, resolve-bones and scene-bone-resolves as the primary inspection areas.
- in `Animations` demo, `Universe::Tick` has 0.4ms of unaccounted / unlabeled work that needs to be optimised as well.
- in `BufferCompute.luau` demo, `gpu shadow` hits `10.68us` with only one object in the scene, and `gpu ssao` reaches `12us`.
- in `Mirrors-1-world.luau`, `Universe::Tick` has `0.18ms` of unaccounted and unlabeled work that needs to be investigated and optimised. `capture-mixed-surfaces` under `execute graph` is at `0.069ms` which could be looked at for additional optimisations. Also, `gpu surface capture` reaches `43.34us` and `gpu mirror-overlay` reaches `31.74us` which could maybe be optimised as well.
- launching `PbrTextureUniqueStress` demo causes studio to freeze completely: `[render] texture table: full, refusing editable-image://00000000 (TextureTable.cpp:372)`. When this happens, we should really just make it use the checkered purple textures and stop requesting it over and over again.
- in `PbrTextureInstanceStress`, `frame graph layout` hits `0.234ms`
- add scrolling into the preferences subtabs and the dockwidget tabs (when multiple widgets are combined into a selector).
- create a `empty scene` demo then optimises `ALL` high-cost computation spots in studio. in a empty scene, it should not cost a huge amount.
- WeatherCompute needs a full rework. `Weather` demo shows its just a waste of compute for a square patch in the sky. Redesign it first, get user's confirmation, then build it out more.
- `Weather` demo sits at 50fps with player spawned in. Authority::Publish at 1.7ms, and a bunch of other spots are huge now. Also memory leak:
```[19:19:17.090] [info] [t255408] [studio] Play started in 'Weather'
[19:19:17.163] [error] [t255408] [script] script 'DemoCamera': examples/scripts/client/DemoCameras/Weather.client.luau [client]:3: 'StarterPlayer' is not a service this engine provides
[C] function GetService
examples/scripts/client/DemoCameras/Weather.client.luau [client]:3
  (Runtime.cpp:501)
/usr/include/c++/16/bits/stl_vector.h:1253: constexpr std::vector<_Tp, _Alloc>::reference std::vector<_Tp, _Alloc>::operator[](size_type) [with _Tp = engine::render::InstanceResidency::Entry; _Alloc = std::allocator<engine::render::InstanceResidency::Entry>; reference = engine::render::InstanceResidency::Entry&; size_type = long unsigned int]: Assertion '__n < this->size()' failed.
Aborted                    (core dumped) ./studio
```
- `Magic` demo is SUPER laggy. `gpu shadow` at 60.42us, `gpu gbuffer` at 58.87us, `resolve resident instances` has `resident scene` at 0.851ms and `resident camera` at 0.628ms, `resident.scene.serial probe` at 0.694ms, `render preparation` at 13.348ms, `sync rendered.walk` at at 5.314ms, `collect instances` at 7.904ms with `interpolate` at 0.700ms and `unaccounted` at 3ms. we may need some spatial culling improvement stuff here, research useful algorithms and setups.
- properly separate `renderer` FPS and `simulation` TPS into two labels in studio. Studio operates with `renderer` fps, simulation is what the world runs at. Update everything that uses these across the entire project - also frame graph should separate the items into `simulation` and `render` categories for this next.

# First v0.26.2

- character spawning & preset outfits built into engine
- proper character & camera controller luau modules (instead of C++ unreachable code)
- proper character movement & AutoRotate & StepHeight
- character intent item in document
- Define player-independent characters, player roster and possession ownership,
  and neutral driver leases for players, AI, scripts, and replay. See the
  [character system plan](../future-work/character-system.md) and the
  [navigation and AI action seam](../future-work/navigation-ai-system.md).

# Second v0.26.3

- animation graphs & event
- default animation controllers and graphs
- G08 animation graphs, masks, synchronise, and event markers
- C02
- Define character state-machine policy, animation graph composition and
  blending, and how animation follows committed character state. See the
  [character system plan](../future-work/character-system.md).

# Three v0.26.4

- P02 contact policies & complete physical events
- P03 complete fast-body collision and batched world queries

# Four v0.26.5

- R3 matrial graphs, instances and permutation control
- R4 projected decals and mesh-particle effect authoring
- R5 HLOD, imposters & foliage authoring

# Five v0.26.6

- R08 real geometry ray queries and reference path tracing
- may be worth moving to zero specifically for optimisation items
- C03

# Six v0.26.7

- W04
- U01
- U02
- U03
- U04

# Seven v0.26.8

- improve physics to real-time (look at arxiv papers?)
- terrain diffusion as a noise generation option
- NEW\_WEATHER\_FEATURES.md
- check events/signals are not pumping if no events are connected
- improved custom version of lumen based on what it does? (real-time global illumination)
- mobile & console builds
- web renderer?
- godot's render graphs
- refined ECS handlers for cache friendliness
- 2D & 3D graph modes (also allow for orthogonal views/projections)
- more built-in useful libraries
- build toolbar and plugin system out
- game maker's ease of use options (as built-in plugin tooling)

# Rules

1. Must plan the generalisation of these systems so they're adaptable for any purpose (i.e. why we're making a lot of things graphs and have luau APIs, so they're accessible).
2. Prune (document) upon completion.
3. Three rounds of optimisations to improve clock time, latency, memory, compute, etc.
4. Plan first, present to user, user says any changes, tweak, repeat until user happy, then implement once told (usually a goal will be set). Include key factors in your documents and whatnot so we know what we're doing.

v0.26.8 onwards would be the continuation of the imagegraph system, however, some parts may be used in earlier versions between v0.26.0 to v0.26.7 as part of each implementation item where its applicable. Either you can re-write sections, take high level implementation features and rewrite with new introduced underlying systems, or blend the code with the new work so we just 'slot it in'.


