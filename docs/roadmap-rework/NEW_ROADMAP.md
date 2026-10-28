# Engine feature roadmap

Current development milestone: v0.26.1.

Development branch order: `v0.25.0 -> v0.26.0 -> v0.26.1`.
Zero v0.26.1 remains active until its outstanding validation and optimization
gates are complete. First v0.26.2 below is the next planned milestone.

# First v0.26.2

- character spawning & preset outfits built into engine
- proper character & camera controller luau modules (instead of C++ unreachable code)
- proper character movement & AutoRotate & StepHeight
- character intent item in document
- Define player-independent characters, player roster and possession ownership,
  and neutral driver leases for players, AI, scripts, and replay. See the
  [character system plan](docs/future-work/character-system.md) and the
  [navigation and AI action seam](docs/future-work/navigation-ai-system.md).

# Second v0.26.3

- animation graphs & event
- default animation controllers and graphs
- G08 animation graphs, masks, synchronise, and event markers
- C02
- Define character state-machine policy, animation graph composition and
  blending, and how animation follows committed character state. See the
  [character system plan](docs/future-work/character-system.md).

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


