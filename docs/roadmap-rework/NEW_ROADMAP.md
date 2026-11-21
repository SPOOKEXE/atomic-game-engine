# Engine feature roadmap

Current development milestone: v0.26.1.

Development branch: `v0.26.1`.

Each version remains active until its outstanding validation and optimization gates are (fully) complete or with user's permission to defer only when it cannot be completed by means.

# Zero v0.26.1

- [_] Revisit the physics system, particularly around the PhysicsStress demo with the tens/hundreds of thousands of parts. Even with ECS that lags considerably which is an issue. We should be using spatial querying to reduce the search amount to only nearby entities (which should be blazing fast and can be parallel). Check if we can do a search then apply step so we can parallel/vectorise before the linear apply (and also apply as much in parallel when they are far away from each other enough like using a frontier loop (hold a frontier list, add neighbours that havent been scanned, run physics, set frontier to neighbors) system)
- [_] Check if we can convert a bunch of the .Each queries into .EachParallel for ECS items (ones that can be async like getters or individual updates that DO NOT rely on accessing and modifying other components).

# First v0.26.2

- [_] Start a server `Script` added at runtime. Only the client role runs new scripts (`Runtime::RunNewScripts`), so a server script cloned or parented in while the game runs never starts; `StressScripts.luau` uses closures for this reason.
- [_] Add team test (run server on team create host machine, everyone can join team test, when host stops server all disconnect / cancel connecting).
- [_] Add a team create chat if not already done
- [_] Test 16 person team test and improve where needed
- [_] Build out the script editor and polish the workflow around it
- [_] Add preference options for external editors for the game - temporary two way sync in filesystem?
- [_] Polish the autocompletion and typing systems to be complete - Build out the "engine dump file" that contains the default built-in things like datatypes and services, dynamic resolution, and code-based resolution (i.e. entity creation and destroy).
- [_] Was thinking of making a "foilage" and "forest" test world based on `[Chlorophyll Engine] 10M Trees - 15km² Test` video.
- [_] Was thinking for the `Screen Space Shadows and Micro Shadows test in the forest demo` and level-of-detail shadow system and adding it to the forest demo and engine as a whole too.
- [_] Also `Custom Engine Forest Showcase [uR5EvBWxN14].webm` is another great demo.
- [_] Was thinking how we can make the render graph more convenient and conventional. Need ways to mark recursivity in renders (e.g. portals, mirrors, etc), and showing them in graph and making them editable - i.e. modular.
- [_] consider adding two render options for: `path tracing` and `ray tracing` as built-in extras with mixed scales (upsampling? dlss support? amd one? etc.).
- [_] Revisit and consolidate the spatial querying system. It may need some upgrades and consolidation. Do this before raycasting (but keeping it in mind, as well as bbox-based searching). Also consider ECS-based algorithms around it for performance (i.e. gpu? vectorised? contiguous memory.).
- [_] Revisit and consolidate the raycasting system. It may need some upgrades and consolidation as its split and unperformant (needs better spatial querying algorithms). Also consider ECS-based algorithms around it for performance (i.e. gpu? vectorised? contiguous memory.).

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
- [_] Stress demos to build once the character and constraint work above lands: a crowd of a few hundred animated, skinned characters (skin palettes, animation tracks, humanoid physics) and long joint and weld chains against the 4,096 joint cap, which still faults on count.

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
