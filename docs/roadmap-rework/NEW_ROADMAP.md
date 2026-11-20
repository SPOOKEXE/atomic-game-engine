# Engine feature roadmap

Current development milestone: v0.26.1.

Development branch: `v0.26.1`.

Each version remains active until its outstanding validation and optimization gates are (fully) complete or with user's permission to defer only when it cannot be completed by means.

# Zero v0.26.1

- [_] Check WeatherCompute full rework is finished, and if not started, find or build the plan documents for it.
- [_] Studio asset pipeline graph: a node editor for asset processing graphs,
  drawn beside the render pipeline graph (`TODO(asset-pipeline)` in
  `mono.studio/src/Interface.cpp` and `mono.studio/include/studio/Editor.hpp`).
- [_] Network panel shader assets: register baked shader modules in the graph
  shader store so render graph nodes can use them, not only count them
  (`TODO(shader-assets)` in `mono.studio/src/Network.cpp`).
- [_] Add network (bytes and traffic breakdown) dropdown in studio AND client. F9 to open debug panel in clients with compute graph breakdown (Frame Graph), network bytes graph breakdown (Network Diagnostics) and memory breakdown (total enitites, total components, total memory usage, per-breakdown, etc). Should be a small imgui user interface that is interactable.
- [_] Double check team create works (each player has a flying ball with billboard for name)

# First v0.26.2

- [_] ```const char *CameraModeName(scene::CameraMode mode) {
	switch (mode) {
	case scene::CameraMode::Classic:
		return "classic";
	case scene::CameraMode::LockFirstPerson:
		return "lock_first_person";
	case scene::CameraMode::ShiftLock:
		return "shift_lock";
	case scene::CameraMode::Scriptable:
		return "scriptable";
	}
	return "unknown";
}``` move all camera and character management code to a "PlayerModule" script called CameraController / ControlModule.
Same with movement system, needs to be server authoritive but pure-lua so it can be changed.
When you create a new world/scene, it auto appends the scripts in.
- character spawning & preset outfits built into engine
- proper character & camera controller luau modules (instead of C++ unreachable code)
- proper character movement & AutoRotate & StepHeight
- character intent item in document
- Define player-independent characters, player roster and possession ownership,
  and neutral driver leases for players, AI, scripts, and replay. See the
  [character system plan](../future-work/character-system.md) and the
  [navigation and AI action seam](../future-work/navigation-ai-system.md).
- [_] Add team test (run server on team create host machine, everyone can join team test, when host stops server all disconnect / cancel connecting).
- [_] Add a team create chat if not already done

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
