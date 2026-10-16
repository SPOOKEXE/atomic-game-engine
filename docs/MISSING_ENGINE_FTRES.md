# Missing engine features worth building

Research date: 2026-10-15. Repository baseline: `VERSION` 0.26.0, revision
`ce69e58d`, including the working tree available during this review.

The strongest additions would help someone finish a multiplayer game: dependable
character movement, combat, navigation, animation, reusable content, and world
streaming. Rendering and compute have useful opportunities too, particularly
lighting, material authoring, residency, and reusable GPU workloads.

This is a researched selection of 42 feature proposals, not a replacement
roadmap or a claim that every proposal should be built. Existing system plans
remain the design owners. Priorities, relative effort, and proposed acceptance
scenes below are engineering judgments. They are not user-survey results,
measured performance claims, delivery estimates, or commitments.

## How to read the gaps

- `Extension`: working foundations exist, but the proposed workflow needs more.
- `Planned`: an existing system plan describes the missing work. This does not
  mean its implementation is complete or that all of the plan is still current.
- `Not found`: no complete implementation appeared in the reviewed public APIs,
  relevant consumers, examples, and targeted source searches. This is a bounded
  finding, not proof that no related code exists anywhere.
- `P1`: broad value, or a blocker for the stated game type.
- `P2`: valuable once its dependencies and a real game need exist.
- `P3`: specialised or expensive enough to require a demonstrated need first.

Effort is relative: `M` extends a contained path, `L` crosses several systems,
and `XL` changes fundamental runtime, content, or deployment contracts. A first
useful slice can be much smaller than the complete feature.

The review used CodeGraph, current headers and runtime consumers, examples,
tests as source evidence, the roadmap, and existing future-work documents.
Engine binaries and demos were not run for this document. Older plans contain
stale absence claims, so current source takes precedence.

## Existing foundations to preserve

These are present in the reviewed checkout. Recommendations below extend them.

| Area | What already exists | Repository evidence |
|---|---|---|
| Rendering | Graph-driven rendering, PBR texture maps, shadows, SSAO, temporal/post effects, portals and mirrors, editable images/meshes, automatic mesh LOD, GPU culling and indirect work | [Renderer](../mono.engine/render/include/engine/render/Renderer.hpp), [materials](../mono.engine/scene/include/engine/scene/Materials.hpp), [mesh residency](../mono.engine/render/include/engine/render/MeshTable.hpp), [LOD](../mono.engine/render/include/engine/render/AutomaticMeshLod.hpp) |
| Graphs | Typed node declarations, dependency waves, execution plans, barriers, resource lifetimes, transient allocation planning, profiling, and Studio graph editing | [render graph](../mono.engine/graph/include/engine/graph/RenderGraph.hpp), [schedule](../mono.engine/graph/include/engine/graph/Schedule.hpp), [allocation planning](../mono.engine/graph/include/engine/graph/PipelineProfile.hpp), [Studio graph](../mono.studio/src/RenderPipelineGraph.cpp) |
| Compute and VFX | Authored compute dispatch, GPU particles and fields, weather, live image graphs, and bounded capture/readback paths | [authored nodes](../mono.engine/render/src/nodes/AuthoredNodes.cpp), [particle fields](../mono.engine/scene/include/engine/scene/GpuParticleField.hpp), [image graphs](../mono.engine/imagegraph/include/engine/imagegraph/Document.hpp), [capture](../mono.engine/render/include/engine/render/DataCapture.hpp) |
| Physics | Fixed-tick rigid bodies, contacts, solver, sleeping, fast-body sweeps, weld assemblies, named collision groups and a C++ collision matrix, raycasts, overlaps, and shape casts | [physics world](../mono.engine/physics/include/engine/physics/PhysicsWorld.hpp), [queries](../mono.engine/physics/include/engine/physics/Query.hpp), [continuous collision](../mono.engine/physics/include/engine/physics/Continuous.hpp), [collision groups](../mono.engine/spatial/include/engine/spatial/CollisionGroups.hpp), [BasePart.CollisionGroup](../mono.engine/scene/src/Part.cpp) |
| Characters and animation | Humanoid movement, health/damage and respawn, tools, skeletons, animation clips/tracks, weighted playback, and rendered bone poses | [characters](../mono.engine/scene/include/engine/scene/Characters.hpp), [tools](../mono.engine/scene/include/engine/scene/Tools.hpp), [animation state](../mono.engine/scene/include/engine/scene/Animation.hpp), [pose evaluation](../mono.engine/render/include/engine/render/Animation.hpp) |
| Networking | Transport, discovery, admission, snapshots/deltas, interest predicates, priority, ownership records, native client movement prediction with input replay/reconciliation and correction smoothing, cross-process portal handoff, and a position-rewind helper | [authority](../mono.engine/replication/include/engine/replication/Authority.hpp), [prediction](../mono.engine/replication/include/engine/replication/Prediction.hpp), [client prediction](../mono.client/src/Replicated.cpp), [portal session](../mono.engine/game/include/engine/game/PortalSession.hpp), [rewind](../mono.engine/replication/include/engine/replication/Rewind.hpp) |
| Persistence | DataStore routing, file/SQLite/HTTP adapters, script services, and actual server load/save/flush integration | [world stores](../mono.engine/world/include/engine/world/DataStore.hpp), [SQLite](../mono.engine/datastore/include/engine/datastore/Sqlite.hpp), [HTTP](../mono.engine/datastore/include/engine/datastore/Http.hpp), [server integration](../mono.server/src/Server.cpp) |
| Replay | Universe snapshots and ordered barrier-envelope recording/replay, with an explicit same-binary, same-machine contract | [recording](../mono.engine/world/include/engine/world/Recording.hpp) |
| Audio | Scene sounds/listeners, stereo spatialisation, a mixer graph, and sample-deadline command scheduling | [audio graph](../mono.engine/audio/include/engine/audio/Graph.hpp), [mixer](../mono.engine/audio/include/engine/audio/Mixer.hpp), [spatialisation](../mono.engine/audio/include/engine/audio/Spatial.hpp) |
| Game UI | Retained layout, bindings, virtual collections, shaped/bidirectional text, localisation catalogues, semantic accessibility, and native accessibility adapters | [GUI APIs](../mono.engine/gui/include/engine/gui), [localisation](../mono.engine/gui/include/engine/gui/Localization.hpp), [semantics](../mono.engine/gui/include/engine/gui/Accessibility.hpp), [native adapter](../mono.client/src/Accessibility.cpp) |
| Studio | Explorer/properties, asset tools, live graph editors, play sessions, collaborative editing, profiling, debugger tools, and automation surfaces | [Studio sources](../mono.studio/src), [control APIs](../mono.engine/control/include/engine/control) |

Two especially important limits:

1. [Client prediction reconciliation](../mono.client/src/Replicated.cpp)
   already rebuilds local predicted state from authority, replays unconfirmed
   movement inputs, and retains visual correction for smoothing. Deferred item
   D00109 in [DEFERRED.md](DEFERRED.md) is stale. The remaining gap is a shared,
   player-independent capsule and movement-mode simulation with richer
   collision-aware parity. The existing rewind helper retains positions, not
   complete historical combat hitboxes. The [Arena example](../mono.engine/examples/assets/scripts/Arena.luau)
   explicitly resolves shots against current physics state.
2. The [raytrace shader](../mono.engine/resources/shaders/raytrace.comp) marches
   the visible depth buffer. The [global-illumination shader](../mono.engine/resources/shaders/global-illumination.comp)
   estimates lighting from G-buffer values. The [pathtrace shader](../mono.engine/resources/shaders/pathtrace.comp)
   accumulates an estimate without traversing scene geometry. These are
   implemented effects, but they do not establish a geometry ray tracer or a
   physically based path tracer.

## What the other engines demonstrate

These are capability examples from official documentation, not a scorecard.
Package, renderer, platform, and version restrictions still apply.

| Engine | Useful patterns to study | Why the pattern matters here |
|---|---|---|
| Unity | [Shader Graph](https://docs.unity3d.com/Manual/shader-graph.html), [humanoid retargeting](https://docs.unity3d.com/Manual/Retargeting.html), [Prefabs](https://docs.unity3d.com/Manual/Prefabs.html), and [Netcode for Entities physics history](https://docs.unity3d.com/Packages/com.unity.netcode@1.10/manual/physics.html) | Make specialist features reusable by game authors; connect simulation history to actual networked interactions. |
| Unreal Engine | [Gameplay Ability System](https://dev.epicgames.com/documentation/en-us/unreal-engine/understanding-the-unreal-engine-gameplay-ability-system), [World Partition](https://dev.epicgames.com/documentation/en-us/unreal-engine/world-partition-in-unreal-engine), and [Render Dependency Graph](https://dev.epicgames.com/documentation/en-us/unreal-engine/render-dependency-graph-in-unreal-engine) | Coordinate gameplay across systems, stream authored worlds, and expose resource/queue behaviour to developers. |
| Roblox Studio/platform | [Pathfinding](https://create.roblox.com/docs/characters/pathfinding), [instance streaming](https://create.roblox.com/docs/workspace/streaming), [DataStoreService](https://create.roblox.com/docs/reference/engine/classes/DataStoreService), [MemoryStoreService](https://create.roblox.com/docs/reference/engine/classes/MemoryStoreService), and [TeleportService](https://create.roblox.com/docs/reference/engine/classes/TeleportService) | Offer approachable scripting workflows around navigation, partial worlds, persistent state, shared ephemeral state, and travel. Hosted services are a separate responsibility from the engine runtime. |
| Godot | [AnimationTree](https://docs.godotengine.org/en/stable/tutorials/animation/animation_tree.html), [NavigationAgent avoidance](https://docs.godotengine.org/en/stable/tutorials/navigation/navigation_using_navigationagents.html), [GI choices](https://docs.godotengine.org/en/stable/tutorials/3d/global_illumination/introduction_to_global_illumination.html), and [compute shaders](https://docs.godotengine.org/en/stable/tutorials/shaders/compute_shaders.html) | Provide bounded, understandable tools with clear limitations. A navigation path, avoidance, physical movement, and rendering remain distinct jobs. |

There is also direct game evidence. Riot describes timestamped shots and bounded
server rewind in [VALORANT's netcode](https://www.riotgames.com/en/news/peeking-valorants-netcode),
and team vision as an important part of [League's fog of war](https://www.riotgames.com/en/news/story-fog-and-war).
Those articles support the practical value of the corresponding features below.
They do not imply that League uses Unity, Unreal, Roblox, or Godot.

## Start with these

The order assumes the engine should support several multiplayer genres. For a
specific game, follow the genre table later in this document instead.

| Rank | Proposal | Main users | Effort | Dependency or reason |
|---|---|---|---|---|
| 1 | [G01: character intent and collision parity](#g01-character-intent-shared-movement-and-prediction-parity) | Shooters, action RPGs, MMOs | L | A common movement path extends existing prediction to bots, replay, and abilities. |
| 2 | [G05: tiled navigation](#g05-tiled-navigation-meshes-and-bounded-route-queries) | MMOs, MOBAs, co-op games | L | Turns existing movement and queries into usable NPC routes. |
| 3 | [G08: animation graphs and events](#g08-animation-graphs-masks-synchronisation-and-event-markers) | Almost every character game | L | Playback already exists; coordinated locomotion and attacks are the next useful step. |
| 4 | [G03: abilities and effects](#g03-reusable-abilities-attributes-effects-and-gameplay-cues) | MOBAs, RPGs, hero shooters | L | Gives game scripts one tested contract for costs, interrupts, and effects. |
| 5 | [G02: historical hit validation](#g02-historical-hitboxes-and-authoritative-combat-queries) | Competitive shooters, action combat | L | Builds on G01 and existing rewind; proves the shooter path. |
| 6 | [W03: prefabs and variants](#w03-prefabs-variants-and-versioned-content-packages) | Every content-heavy game | L | Reuse a complete enemy, weapon, building, or UI subtree safely. |
| 7 | [P01: solved general constraints](#p01-solve-the-existing-generic-constraints) | Physics games, vehicles, ragdolls | L | Makes existing authored joint data do useful runtime work. |
| 8 | [R03: material graph](#r03-material-graphs-instances-and-permutation-control) | Artists across genres | L | Makes existing shader support practical without hand-authoring every GLSL program. |
| 9 | [G04: team vision](#g04-team-vision-fog-of-war-and-gameplay-relevance) | MOBAs, RTS games, tactical shooters | M/L | Connects game visibility to authoritative information disclosure. |
| 10 | [R01: baked lighting and probes](#r01-baked-lighting-reflection-probes-and-a-deliberate-gi-path) | Indoor shooters, RPGs, low-end clients | L | Offers controllable indirect light without starting with a full real-time GI system. |
| 11 | [W02: world cell streaming](#w02-world-cell-streaming-with-explicit-activation) | Open worlds, MMOs, survival games | XL | Needs lifecycle and content contracts, not just distance culling. |
| 12 | [W07: multiplayer fault and load scenes](#w07-repeatable-multiplayer-fault-scenes-and-load-profiles) | All multiplayer development | M/L | Proves the features together under latency, churn, and content pressure. |

## Gameplay, navigation, and animation

<a id="g01-character-intent-shared-movement-and-prediction-parity"></a>
### G01. Character intent, shared movement, and prediction parity

P1. Extension and planned. Owners: [character system](future-work/character-system.md)
and [input system](future-work/input-system.md).

The character plan also defines player-independent NPC characters, player roster
and possession ownership, and the lease boundary between a human driver and AI.
The [navigation and AI plan](future-work/navigation-ai-system.md) specifies the
correlated named-action request and acknowledgement seam.

The native client already predicts movement, reconciles authoritative state,
replays unconfirmed move inputs, and retains visual correction for smoothing.
Extract a player-independent CharacterIntent and controller so players, bots,
scripts, and replay can use the same rules. Extend that controller with
capsule-based collision, explicit grounding/steps/slopes, moving-platform
behaviour, and game-defined modes such as climbing or swimming. Keep the server
authoritative and extend collision-aware parity between client replay and server
simulation.

First useful slice: exercise existing input acknowledgment, replay of
unconfirmed commands, and visual smoothing through the improved controller.
Prove it with two clients, stairs, a moving platform, loss, and variable
latency. Report correction size and replay cost. Arbitrary scripted movement
needs an explicit prediction policy; reproducible fixed ticks do not establish
bitwise determinism across machines.

<a id="g02-historical-hitboxes-and-authoritative-combat-queries"></a>
### G02. Historical hitboxes and authoritative combat queries

P1 for shooters. Extension. Build on the current position rewind and query APIs.

Store a bounded history of collision poses, hitbox dimensions, stance, relevant
animation state, and moving blockers. Validate firing time, origin, fire rate,
ammunition, and permitted rewind before querying that history. Run queries
against historical data without temporarily changing the live world.

First useful slice: hitscan against moving capsules and a moving door. Add
skeletal hit regions only when needed. Show the firing view, accepted server
query, and hit result in a debug replay. Include expired history, respawn, and
portal traversal. Unity documents [collision-world history](https://docs.unity3d.com/Packages/com.unity.netcode@1.10/manual/physics.html);
Riot explains why [bounded rewind](https://www.riotgames.com/en/news/peeking-valorants-netcode)
matters to actual gunplay.

<a id="g03-reusable-abilities-attributes-effects-and-gameplay-cues"></a>
### G03. Reusable abilities, attributes, effects, and gameplay cues

P1 for MOBAs, RPGs, and hero shooters. Extension; a complete shared contract was
not found. Existing [MagicCore examples](../mono.engine/examples/assets/scripts/libraries/MagicCore)
already implement substantial spell, projectile, and status-effect behaviour.

Extract only the reusable needs: typed attributes, resource costs, cooldowns,
stacking policies, targeting, cancellation, authoritative activation, and
deduplicated presentation cues. Keep individual spells and balance rules in
game-owned data/scripts. Stable gameplay tags can describe blocked states and
immunities without hard-coding every combination.

First useful slice: a projectile spell with mana cost, cooldown, stun, and
interrupt. Two clients must agree after duplicate requests and denied casts;
predicted sound/VFX must not play twice. Unreal's [ability framework](https://dev.epicgames.com/documentation/en-us/unreal-engine/understanding-the-unreal-engine-gameplay-ability-system)
is a useful behavioural reference. Copy the useful contract, not its class tree.

<a id="g04-team-vision-fog-of-war-and-gameplay-relevance"></a>
### G04. Team vision, fog of war, and gameplay relevance

P1 for MOBAs/RTS; P2 elsewhere. Extension; a complete gameplay vision service
was not found. Replication already accepts interest predicates and priorities.

Add authoritative vision sources, occluders, team masks, revealed/explored
state, and explicit detection rules. Use that result for replication,
targeting, minimaps, and the rendered fog mask. Camera occlusion alone cannot
decide whether a team knows an enemy's position. Define what happens to a
previously known enemy when it becomes hidden.

First useful slice: two teams, walls, brush, and a temporary ward. Hidden units
must stop receiving protected updates while allies remain visible. Inspect the
actual outgoing data. League's [fog-of-war account](https://www.riotgames.com/en/news/story-fog-and-war)
is a concrete reference for the visual side; the disclosure policy is a proposed
engine/game contract, not a claim about that article's network implementation.

<a id="g05-tiled-navigation-meshes-and-bounded-route-queries"></a>
### G05. Tiled navigation meshes and bounded route queries

P1. Extension and planned: [navigation and AI](future-work/navigation-ai-system.md).

The current [path query](../mono.engine/physics/include/engine/physics/Query.hpp)
uses a small set of authored horizontal box tops. Extend beyond that with
walkable mesh baking, agent radius/height profiles, area costs, stacked rooms,
tile references, and explicit partial/unreachable results. Reuse collision
geometry and publish navigation changes at a defined tick boundary.

First useful slice: a bot follows routes through ramps, stairs, and two floors
without cutting through a ceiling. Then add a closed door and a changed tile.
Show the corridor and reason for failure in Studio. Study Roblox's
[pathfinding workflow](https://create.roblox.com/docs/characters/pathfinding)
and Godot's [navigation-server lifecycle](https://docs.godotengine.org/en/stable/tutorials/navigation/navigation_using_navigationservers.html).

### G06. Crowd steering and explicit traversal links

P2; promote for MOBAs and large NPC scenes. Planned in the navigation plan.

Separate global routing from local avoidance. Add bounded neighbour queries,
arrival/queue behaviour, and authored links for ladders, jumps, climbs, doors,
and portals. A link requests a movement action through G01; it does not teleport
the agent past unsolved movement. Flow fields are a later option for many agents
sharing destinations, after a measured need appears.

First useful slice: two groups pass through a narrow gate without permanent
deadlock, then reserve and cross one ladder link. Test stacked floors so agents
on different levels do not avoid each other. Godot explicitly documents that
[avoidance does not change pathfinding](https://docs.godotengine.org/en/stable/tutorials/navigation/navigation_using_navigationagents.html).

### G07. Perception, inspectable behaviours, and tactical queries

P2. Planned in the [navigation and AI plan](future-work/navigation-ai-system.md).
Useful for shooter bots, MMO enemies, pets, and village simulations.

Add sight/hearing/proximity facts, named hierarchical AI states, Boolean
transition guards, and query scoring for cover, attack range, and reachable
positions. Expose the selected state, failed condition, remembered stimulus,
and chosen destination. The plan keeps state transitions separate from
same-tick behaviour dataflow and uses one bounded behaviour representation.
Registered typed condition/action/task nodes provide programmable extensions
without worker-thread callbacks. Export the canonical state tree and labelled
transitions to SVG and self-contained HTML, with an optional revision-matched
active path and reason snapshot.

First useful slice: a guard patrols, hears a noise, investigates, chases, and
returns home. A ranged enemy chooses reachable cover. Unreal's
[StateTree](https://dev.epicgames.com/documentation/en-us/unreal-engine/overview-of-state-tree-in-unreal-engine)
and [EQS example](https://dev.epicgames.com/documentation/unreal-engine/environment-query-system-quick-start-in-unreal-engine)
provide useful references for state inspection and candidate scoring.

<a id="g08-animation-graphs-masks-synchronisation-and-event-markers"></a>
### G08. Animation graphs, masks, synchronisation, and event markers

P1. Extension and planned in the [character system plan](future-work/character-system.md).
Clips and weighted tracks already play; adding another clip player would
duplicate existing work. AI requests named character actions through the
[navigation and AI plan](future-work/navigation-ai-system.md), which does not
own animation state or bones.

Add locomotion blend spaces, state transitions, upper/lower-body masks,
additive aim/recoil, sync groups, and event markers. Define looping, reverse
playback, interruptions, and event deduplication. Root-motion properties already
exist, but authority-side movement consumption needs a real controller contract.
Gameplay hit windows must be available headlessly instead of depending on a
client-only pose evaluator.

First useful slice: run and strafe while reloading, then interrupt with a stun.
Footstep and weapon events must remain correct across blends and reconciliation.
Godot's [AnimationTree](https://docs.godotengine.org/en/stable/tutorials/animation/animation_tree.html)
shows the relevant blend-tree and state-machine workflows.

### G09. Retargeting, procedural rig controls, and avatar deformation

P2. Extension and planned in the character plan. Useful for RPG avatars,
shooter weapon grips, and reducing animation-authoring cost.

Start with a humanoid mapping and retarget preview, then two-bone IK for feet
and hands, aim/look controls, and explicit attachment sockets. Treat facial
morph targets, equipment skinning, and character customisation as later slices
with their own import/runtime contracts. These were not found as a complete
workflow in the reviewed animation/import path.

Prove one locomotion set on two differently proportioned rigs, a hand staying
on a weapon grip, and feet following stairs. Unity's
[humanoid retargeting](https://docs.unity3d.com/Manual/Retargeting.html) is a
useful reference. Animation sharing needs compatible rig semantics, not merely
matching bone counts.

### G10. Spectators, killcams, and replay presentation

P2. Extension. Universe recording/replay already exists, so the missing feature
is a game-facing replay product.

Add spectator permissions, target following, a presentation timeline, bounded
seek checkpoints, and a killcam view. Decide whether a shipped replay stores
authoritative snapshots, versioned events, or requires the original executable.
The existing same-binary, same-machine recorder is not a promise of portable
match playback across releases.

First useful slice: replay a short combat encounter from attacker and victim
views without altering the live match. Restrict team-hidden information during
live spectating. Use this to inspect G02 and G04 before building a broadcast UI.

## Physics and physical gameplay

<a id="p01-solve-the-existing-generic-constraints"></a>
### P01. Solve the existing generic constraints

P1. Extension and planned: [physics expansion](future-work/physics-expansion.md).

The saved, script-visible [six-axis constraint](../mono.engine/scene/include/engine/scene/Constraints.hpp)
is not currently solved. Connect it to the existing contact/impulse pipeline,
including limits, drives, break thresholds, sleeping, and assembly ownership.
Unify the relevant weld path as the plan specifies.

First useful slice: a hinged door, driven sliding platform, pendulum, and
breakable joint all interact with ordinary bodies. Inspect impulses and limit
error. This is a better foundation for physical gameplay than independently
adding a vehicle solver and a ragdoll solver.

### P02. Contact policies and complete physical events

P1/P2 by game. Extension and planned in the physics plan. Layers, material
properties, triggers, and internal contact transitions already exist. Named
collision groups, the C++ pairwise collision matrix, and the
`BasePart.CollisionGroup` property are also present. Script or project
configuration of matrix pairs is still a gap; `PhysicsService`-style matrix
editing was not found. Friction and restitution already use fixed combine
rules, so the proposed work is author-selectable combine/contact overrides.

Add script/project configuration for the existing collision matrix, explicit
material-combine overrides, bounded contact/trigger event delivery, and
complete hit information. Define collision-group lifecycle and keep query
filtering consistent with solver filtering while allowing a query to ask for a
different purpose, such as aim selection or camera blocking.

First useful slice: configure the existing matrix through a project or script
surface, then verify projectiles ignore their owner and entering a hazard
produces one ordered enter/exit pair. Changing a group, destroying an
overlapping body, and disabling a collider must have defined event behaviour.

### P03. Complete fast-body collision and batched world queries

P2. Extension and planned in the physics plan. Sweeps, CCD-related counters,
shape casts, and batched occupancy/signed-distance queries already exist in
[Query.hpp](../mono.engine/physics/include/engine/physics/Query.hpp).

Add exact time-of-impact cases where the current conservative path is
insufficient, configurable body modes, batched raycasts/shape casts, richer
query filters, and consistent overflow/results. Choose cases from a failing
projectile or vehicle scene. Do not replace the collision system on an
unmeasured suspicion.

Prove a small fast projectile crossing a thin moving blocker, a rotating body,
and a burst of gameplay queries. Record missed contacts, solver cost, candidate
counts, and query latency. Keep authoritative results within the tick contract.

### P04. Vehicles and physical character rigs

P2. Planned in the physics/character plans. Unreal's
[physics overview](https://dev.epicgames.com/documentation/en-us/unreal-engine/physics-in-unreal-engine)
provides examples of separate vehicle and ragdoll capabilities.

Build a suspension/traction model on the shared bodies and constraints. Build
ragdoll joints and an animated-to-physical transition separately, on that same
solver. Add buoyancy only with an explicit water-volume query. Decorative
debris can remain client-side; gameplay vehicles and carried bodies need an
authority and correction policy.

First useful slices: a four-wheel vehicle on rough ground, then a character
falling down stairs and recovering. Verify against moving platforms and
networked ownership changes before adding tuning-heavy specialist models.

### P05. Bounded destruction and breakable assemblies

P2 for destruction games; P3 otherwise. Planned in the physics plan.

The existing [BreakGroup](../mono.engine/scene/include/engine/scene/BreakGroup.hpp)
already releases authored anchored parts into simulation. Extend it with
fracture authoring, break conditions, fragment budgets, and coordinated
collision/navigation updates. Replicate authoritative breaks and gameplay-relevant
pieces; permit cosmetic fragments to use cheaper local simulation. Arbitrary
runtime mesh cutting is a separate, larger feature.

First useful slice: break a wall that opens a navigation route and blocks vision
before destruction. A late join sees the same opening. Prove bounded debris
count, released resources, and persistence of the structural change. Chaos
destruction in Unreal's physics documentation is a reference, not a reason to
start with its entire fracture pipeline.

### P06. Cloth, soft bodies, and simulated fluids

P3. A complete runtime was not found. Useful for clothing, ropes, banners,
water interaction, and specialised simulation games.

Choose one visual feature first, such as a cape or rope with collision and
distance-based quality. Define attachment, bounds, reset, and CPU fallback.
Authoritative fluid or soft-body gameplay has a much larger replication and
replay contract than decorative GPU motion.

Prove a banner attached to a moving object, including teleport and reset, before
proposing a general multiphysics framework. Existing particle/field/weather
systems should supply inputs where appropriate.

## Rendering and content scale

<a id="r01-baked-lighting-reflection-probes-and-a-deliberate-gi-path"></a>
### R01. Baked lighting, reflection probes, and a deliberate GI path

P1/P2. Extension; a complete light-baking/probe workflow was not found. Existing
ambient estimates and portal radiance should remain explicitly described.

Start with baked indirect lighting for static geometry and sampled lighting for
moving characters. Add authored reflection probes and predictable blending.
Offer bake diagnostics, invalidation, streaming ownership, and clear limitations
for moving/emissive objects. Decide later whether dynamic GI needs probes,
distance fields, or geometry tracing.

First useful slice: an indoor room with an off-screen coloured bounce source
and a character walking from outside to inside. Compare against a reference
capture and inspect leaks. Unity's [Adaptive Probe Volumes](https://docs.unity3d.com/Manual/urp/probevolumes-concept.html)
and Godot's [GI comparison](https://docs.godotengine.org/en/stable/tutorials/3d/global_illumination/introduction_to_global_illumination.html)
show why several lighting tiers can be useful.

### R02. Many-light selection and local-shadow budgets

P2. Extension; a complete clustered many-light path was not found in the reviewed
lighting implementation. Point/spot lights and lighting passes already exist.

Add tile/cluster light lists only after a representative scene demonstrates
that light evaluation is costly. Budget shadow updates by visible importance,
cache unchanged results, and expose overflow. A village with many lamps and a
MOBA team fight need different priorities from an empty benchmark grid.

First useful slice: a night street plus moving ability lights. Match the
existing lighting result within a declared tolerance, show affected lights per
tile and shadow-update decisions, and measure total GPU time including list
construction. More queues or fewer draw calls alone are not proof of a win.

<a id="r03-material-graphs-instances-and-permutation-control"></a>
### R03. Material graphs, instances, and permutation control

P1/P2. Extension. Materials, map channels, runtime fragment shaders, and cooked
shader variants already exist. A complete visual material authoring workflow
was not found.

Build typed material nodes and named parameters over the existing shader ABI.
Include texture slots, parameter instances, reusable functions, previews,
compile diagnostics, and bounded permutation generation. Keep the render-pipeline
graph distinct: it schedules passes; a material graph defines surface shading.

First useful slice: author a dissolve material, make two parameter variants,
save/bake/publish them, and retain the last good shader after an invalid edit.
Study Unity's [Shader Graph](https://docs.unity3d.com/Manual/shader-graph.html).
Extend the existing compiler/library rather than adding another shader path.

### R04. Projected decals and mesh-particle effect authoring

P2. Extension and planned: [VFX system](future-work/vfx-system.md). Face-bound
decals, tiled textures, particles, beams, trails, and GPU fields already exist.

Add pooled world-projected decals with receiver layers, depth/normal fading,
bounded lifetime, and PBR channels where supported. These support bullet holes,
scorches, footprints, puddles, and MOBA area indicators. Extend effect authoring
with mesh-particle outputs and coordinated events as separate useful slices.

First useful slice: repeated impacts on curved geometry with a hard pool limit;
transparent receivers and portals have an explicit policy. Godot documents
[projected decals](https://docs.godotengine.org/en/stable/tutorials/3d/using_decals.html);
Unity's [VFX Graph](https://docs.unity3d.com/Packages/com.unity.visualeffectgraph@17.0/manual/index.html)
is an authoring reference, not evidence that Atomic lacks GPU particles.

### R05. HLOD, impostors, and foliage authoring

P2. Extension. Automatic mesh LOD, resident cluster pages, GPU selection, and
occlusion already exist. The remaining workflow is aggregation and residency
across large authored regions, plus vegetation authoring.

Generate region proxies, merged material atlases or impostors, foliage
placements, and stable transitions. Keep distant visual proxies separate from
collision, AI, and replication fidelity. Add wind and interaction only through
the existing field inputs where they fit.

First useful slice: a forest and village remain visible when their detailed
cells are unloaded. Measure draw/instance work, residency, and visible popping.
Unreal's [World Partition HLOD](https://dev.epicgames.com/documentation/en-us/unreal-engine/world-partition---hierarchical-level-of-detail-in-unreal-engine)
shows a distinct benefit beyond per-mesh triangle reduction.

### R06. View-driven mip residency before virtual texturing

P2; virtual texturing P3. Extension. Texture delivery, mip chains, budgets,
owner scoping, and runtime updates already exist in the
[texture table](../mono.engine/render/include/engine/render/TextureTable.hpp).
The reviewed upload path uploads the supplied levels together.

Add requested/resident mip ranges, priorities across cameras and portals,
bounded uploads, eviction, and prewarming. Begin with ordinary mip streaming.
Introduce sparse tiled virtual textures only if terrain/material scale needs
them and backend support justifies the added machinery.

Prove rapid travel through a textured town under a fixed GPU memory budget,
including a second camera and a teleport. Inspect requested versus resident
detail and upload spikes. References: Unity's [mipmap streaming](https://docs.unity3d.com/Manual/TextureStreaming.html)
and Unreal's [virtual texturing](https://dev.epicgames.com/documentation/en-us/unreal-engine/virtual-texturing-in-unreal-engine).

### R07. Temporal upscaling and frame-budget quality control

P2. Extension. Temporal AA, motion-vector features, history, reduced-size
resources, and GPU measurements already provide parts of the foundation.

Add separate render/display resolutions, deliberate history invalidation,
disocclusion handling, reactive treatment of transparent effects, and an
upscaler adapter. Dynamic resolution needs a stable GPU-time controller and
minimum quality bounds. Keep UI at display resolution and handle portal and
multi-viewport histories independently.

First useful slice: moving foliage, thin geometry, particles, a camera cut,
and a resize. Compare image sequences, latency, and frame time against native
resolution. Unreal's [Temporal Super Resolution](https://dev.epicgames.com/documentation/en-us/unreal-engine/temporal-super-resolution-in-unreal-engine)
documents this as a complete temporal workflow. No particular vendor upscaler
or licensing arrangement is assumed here.

### R08. Real geometry ray queries and reference path tracing

P3. Extension beyond the current screen-space/heuristic passes. A geometry
acceleration structure and tracing backend were not found in the reviewed path.

Start with a small static-scene BVH and validated ray/triangle queries, then
instance transforms and an explicit update budget. A reference path tracer
also needs surface sampling, material/light evaluation, random sampling,
accumulation, and defined convergence. Hardware tracing is a separate capability
adapter, not a scene feature flag alone.

Prove an off-screen reflected object, a shadow from non-visible geometry, and
multi-bounce colour transfer. Keep these distinct from screen-space reflection
tests. A Nanite-like geometry system is another separate program: Unreal's
[Nanite description](https://dev.epicgames.com/documentation/en-us/unreal-engine/nanite-virtualized-geometry-in-unreal-engine)
includes hierarchical clusters, fine-grained streaming, and a specialised draw
path. Existing cluster pages do not establish equivalent functionality.

### R09. Water surfaces, water volumes, and underwater rendering

P2 for open worlds; P3 elsewhere. A complete integrated workflow was not found.
Existing refraction, reflection, volumes, and fields are reusable foundations.

Separate the visual surface from authoritative water-volume queries. Start
with a plane/river mesh, depth-based colour/absorption, shoreline fading,
underwater camera state, and a simple current/buoyancy query. Swimming consumes
G01's movement modes. Full fluid simulation is not required for useful water.

First useful slice: enter a river, swim, float a rigid object, and move the
camera below the surface. Verify mirrored/portal views and stream-out behaviour.
Give caustics, waves, and fluid interaction separate quality and cost budgets.

## Graphs, compute, and diagnosis

### C01. General authored buffer compute and reusable kernels

P2. Extension. Graph buffer descriptors, authored image-oriented dispatch,
specialised particle/weather buffers, noise jobs, and readback already exist.

The useful gap is an end-to-end authoring contract for named typed buffer
inputs/outputs, binding validation, dispatch bounds, ownership, dependencies,
and completion. The reviewed generic dispatch binds sampled/storage images and
an optional engine instance buffer; it is not a general arbitrary-buffer API.
Add scan, compaction, reduction, sort, or noise kernels only when a consumer
needs them.

First useful slice: compact an authored buffer, feed it to another GPU pass,
and optionally retrieve a bounded result asynchronously. Compare against a CPU
oracle for empty, odd-sized, and oversized inputs. Godot's
[compute guide](https://docs.godotengine.org/en/stable/tutorials/shaders/compute_shaders.html)
is a useful API reference and explicitly warns about immediate synchronisation.

### C02. Simulation and animation levels of detail

P2 for MMOs, MOBAs, and battle scenes. Extension; a complete coordinated policy
was not found. Mesh LOD and an animator throttling property already exist.

Introduce explicit update tiers for perception, behaviour, animation, and
non-critical effects, with ownership and maximum staleness per subsystem.
Distance alone is insufficient: a far-away projectile, combatant, or player-owned
object may still require full gameplay fidelity. Lower-frequency simulation
must run at defined tick multiples with deterministic scheduling.

First useful slice: a populated town with a few active combatants. Inspect why
each agent is in its tier, then provoke combat during promotion. Measure tick
cost, promotion spikes, and stale decisions. Keep combat authority and gameplay
animation events separate from visual pose throttling.

### C03. Reusable graph assets, versioning, and live replacement

P2. Extension. Typed documents, validation, execution, and graph editors already
exist. A complete cross-asset subgraph/version/override workflow was not found.

Add reusable subgraphs with named typed ports, immutable asset revisions,
instance parameters, migrations, diagnostics mapped to the source node, and
transactional replacement. Define retained-state/history compatibility before
swapping a live graph. Reuse existing document, validation, and editor
infrastructure where compatible. Keep domain execution and timing rules
explicit; audio, image baking, and frame scheduling need different contracts.

First useful slice: two effects use one authored subgraph. Update it, show the
affected instances, and reject an incompatible update without losing the
previous working output. Do not turn this into a universal gameplay language
before material/VFX/behaviour users establish their actual common needs.

### C04. Physical GPU queue overlap and parallel recording

P3 until a profile justifies it. Extension. The current
[execution plan](../mono.engine/graph/include/engine/graph/ExecutionPlan.hpp)
already classifies graphics/compute/transfer work. The SDL adapter advertises
`UnifiedQueue = true`; separate command buffers do not prove simultaneous work.

A backend with separate physical queues needs fence/semaphore dependencies,
resource ownership transitions, measured scheduling, and reliable fallback.
CPU command recording can also be parallelised where ownership permits. Neither
change is automatically faster on a bandwidth-saturated frame.

First useful slice: one independent compute workload overlaps one raster branch
on supported hardware. Compare serial and overlapping modes, including transfers
and synchronisation. Show actual GPU overlap in a capture. Unreal's
[RDG](https://dev.epicgames.com/documentation/en-us/unreal-engine/render-dependency-graph-in-unreal-engine)
documents queue scheduling, resource lifetimes, and diagnostics worth studying.

### C05. Reproducible renderer captures and resource inspection

P2. Extension. Frame Graph profiling, graph diagnostics, image previews,
readback, structured capture, and world recording already exist.

Add a diagnostic bundle tying a world snapshot to pipeline revision, shader
artifacts, camera/history state, settings, and content hashes. Provide
pass-by-pass resource inspection and a bounded way to reproduce a problematic
frame. Engine-level reproducibility is distinct from native GPU command capture;
use an external GPU debugger where that is the actual need.

First useful slice: capture a bad portal/transparent-effect frame, restore its
inputs, and inspect the first incorrect resource. Name backend/build restrictions
and unavailable history. Extend existing tools rather than adding a second
unrelated profiler. RDG's resource-lifetime/overlap views are a useful comparison.

### C06. Procedural content graphs with reproducible outputs

P2. Planned: [procedural generation](future-work/procedural-generation.md).
Noise jobs, editable meshes, image graphs, and scripted terrain examples already
provide parts of the foundation.

Add seeded recipes, dependency tracking, bounded spatial queries, spline
inputs, scatter/exclusion rules, and immutable generated artifacts. Separate
editor generation from runtime generation. Use domain-owned adapters so a
road generator and a dungeon generator do not each invent content delivery.

First useful slice: a seeded forest and village road network with exclusion
around entrances. Rebuild one edited region and leave unrelated outputs intact.
Bake and publish the same result. Unreal's
[PCG overview](https://dev.epicgames.com/documentation/en-us/unreal-engine/procedural-content-generation-overview)
is a useful reference for authoring a procedural workflow.

## Worlds, reusable content, and online services

### W01. Production terrain and landscape authoring

P2; P1 for a terrain-centred game. Extension and planned:
[terrain system](future-work/terrain-system.md).

Saved `Workspace.Terrain` data and the scripted
[TerrainCore](../mono.engine/examples/assets/scripts/libraries/TerrainCore)/[TerrainRuntime](../mono.engine/examples/assets/scripts/libraries/TerrainRuntime/init.luau)
already provide height fields, chunked greedy box generation, edits, pooled
parts, and staged rebuilds. The larger gap is a reusable landscape asset and
Studio workflow with terrain-specific mesh/collision outputs, seams, material
layers, brushes, and durable edits. Extend that foundation; add voxel/SDF terrain
when caves, digging, or planets actually require it.

First useful slice: edit terrain, stream its neighbour, save, reload, and walk
across the boundary. Collision, navigation, and visible terrain must agree.
Define rebuild budgets and last-good output while a chunk changes. Large
planets additionally need a coordinate/precision contract across physics,
rendering, networking, and asset placement.

<a id="w02-world-cell-streaming-with-explicit-activation"></a>
### W02. World cell streaming with explicit activation

P1 for MMOs/open worlds. Extension and planned:
[world streaming](future-work/world-streaming.md).

Asset delivery, interest filtering, multiple worlds, portals, and chunk indexes
are real foundations. Add cell manifests, staged load/activate/deactivate,
reference resolution, state persistence, pinned dependencies, and multiple
streaming sources. Rendering visibility, client replication interest, and
server simulation residency need related but distinct decisions.

First useful slice: cross a town boundary while a projectile, quest NPC, and
portal remain relevant. Teleport to a cold region, disconnect during loading,
and revisit modified state. Inspect CPU/GPU residency and activation spikes.
Unreal's [World Partition](https://dev.epicgames.com/documentation/en-us/unreal-engine/world-partition-in-unreal-engine)
and Roblox's [instance streaming](https://create.roblox.com/docs/workspace/streaming)
show useful author-facing concepts; they do not solve cross-server MMO placement.

<a id="w03-prefabs-variants-and-versioned-content-packages"></a>
### W03. Prefabs, variants, and versioned content packages

P1. Extension and planned:
[prefab/package system](future-work/prefab-package-system.md).

Clone/subtree documents and project archives already exist. Add stable member
identity, prefab definitions, instance overrides, nested prefabs, variants,
dependency versions, and explicit update/conflict handling. A ZIP project
package is not this authoring workflow.

First useful slice: reuse an enemy with a rig, behaviour, effects, and UI. Change
the source health/mesh while retaining one instance's deliberate override.
Verify save/load and undo after a deleted or renamed member. Unity's
[Prefab documentation](https://docs.unity3d.com/Manual/Prefabs.html) provides a
useful comparison for reuse and overrides.

### W04. Parties, matchmaking, and cross-server travel

P1 for hosted multiplayer; P2 for local/private games. Extension and planned:
[session/social system](future-work/session-and-social.md).

Discovery, rendezvous, session keys, presence, local world travel, and
cross-process portal handoff already exist. The authenticated, resumable
handoff gates source retirement on destination acknowledgment; a durable
prepare/commit journal is wired when a DataStore root is configured. See
[portal transfer](../mono.engine/script/src/PortalTransfer.cpp) and
[portal session](../mono.engine/game/include/engine/game/PortalSession.hpp).
A hosted control plane still needs account identity, party membership, lobby
and match allocation, reconnect orchestration, and broader travel policies.
Region placement and host recovery need a real multi-host deployment to validate.
They cannot be proved with only several worlds in one executable.

First useful slice: a party enters a reserved match, reconnects after a dropped
connection, and returns together. Name what happens when a destination fails.
Roblox's [TeleportService](https://create.roblox.com/docs/reference/engine/classes/TeleportService)
and shared ephemeral [MemoryStoreService](https://create.roblox.com/docs/reference/engine/classes/MemoryStoreService)
illustrate the user-facing needs. Keep provider adapters separate from engine
simulation and game rules.

### W05. Durable gameplay transactions and ownership transfer

P1 for MMOs/economies; P2 elsewhere. Extension. Persistent stores and several
adapters already exist, so the proposal is not to add another generic key/value
service.

Define game-facing contracts for idempotent purchases/rewards, revision
conflicts, inventory transfer, session leases, schema migration, and crash
recovery. Verify what each existing backend can guarantee before exposing a
stronger promise. Atomic trade across multiple records or servers needs an
explicit protocol; a collection of independent successful saves is insufficient.

First useful slice: award loot, retry the request, transfer an item, and kill
the host between writes. The chosen durable record/protocol must prevent
duplication and preserve ownership. Roblox's persistent
[DataStoreService](https://create.roblox.com/docs/reference/engine/classes/DataStoreService)
is a reference for the service category, not evidence of identical transaction
guarantees. Keep the engine adapter separate from a game's economy policy.

### W06. Data-driven item, equipment, quest, and encounter kits

P2. Extension; complete reusable kits were not found. Tools, health, script
reflection, data tooling, and MagicCore are existing foundations.

Provide optional script/data packages for inventories, equipment slots, loot
tables, quest objectives, dialogue state, and encounters. Support stable
definition names, validation, content references, and migrations. These should
exercise the engine APIs while keeping MMO-specific progression and balance
out of the low-level runtime.

First useful slice: pick up/equip an item, complete a quest objective, receive
one reward, reconnect, and retain the result. Inspect the chain from definition
to authoritative state to UI. This is a practical integration test for G03,
W03, and W05, even if no generic quest framework is ultimately needed.

<a id="w07-repeatable-multiplayer-fault-scenes-and-load-profiles"></a>
### W07. Repeatable multiplayer fault scenes and load profiles

P1/P2. Extension. Existing tests, stress scenes, observations, automation,
profilers, and Studio play links already do substantial work.

Add a convenient repeatable profile combining delay, jitter, loss, bandwidth
limits, joins/leaves, cold content, and host failure with gameplay scenarios.
Use existing injection/harness paths where available; the gap is a cohesive
author workflow and curated genre scenes, not an assertion that networking
has no tests. Capture seeds/settings and show per-client correction, tick cost,
traffic, and convergence.

First useful slice: rerun the same two-player combat/door scenario under a
saved adverse-network profile. Then run bots through a crowded objective and
a cell transition. Report the real player/entity counts and hardware. Do not
advertise an MMO player capacity from an empty-world connection test.

## Input, sound, and presentation workflows

### U01. Project-owned gameplay actions and device profiles

P1/P2. Extension and planned: [input system](future-work/input-system.md).

Raw devices, IME, GUI actions, and named `ContextActionService` callbacks
already exist. Add serialisable typed game actions, contexts, composite axes,
rebind/conflict UI, saved profiles, device glyphs, and local-player routing.
Touch needs a real adapter and authored controls; the current callback API's
touch-button argument does not create a touch surface.

First useful slice: the same move/aim/interact actions work with keyboard/mouse
and gamepad, survive reconnect/rebind, and respect modal UI capture. Add a touch
profile only with device verification. Build this into the character/ability
samples rather than introducing a second movement input path.

### U02. Audio effects, spatial filtering, and acoustic zones

P2. Extension and planned: [audio system](future-work/audio-system.md).

The current graph has Player, Fader, Emitter, Bus, and Output nodes. Extend it
with filters, sends, reverb, limiting, ducking, voice prioritisation, and
virtualisation. Spatial placement currently returns stereo gains without an
occlusion filter or head model. Add obstruction/room effects before treating
an observation field as implemented acoustic behaviour.

First useful slice: move a sound behind a closed door, open it, and cross from
a small room to outside. A dialogue bus ducks combat audio. Check audio-thread
allocation/blocking, peaks, and audible transitions. HRTF binaural rendering is
a later quality option with its own listening and device tests.

### U03. Streaming media, adaptive music, and voice transport

P2/P3. Extension and planned in the audio/session plans. Complete streaming
music/voice and video playback workflows were not found.

Treat these as separate slices: buffered music decoding with seek/loop
behaviour; music state driven by gameplay; capture/encode/jitter buffering for
voice; and decoded video frames with audio synchronisation. Voice also needs
per-user permission, mute, and routing policy. A video widget alone cannot
provide a decoder and streaming clock.

First useful slice: music loops while content arrives slowly without starving
the mixer. For voice, prove two remote clients under jitter and reconnect.
For video, prove seek/pause and bounded decoded residency. Existing deferred
`VideoFrame` work should keep its single owner in [DEFERRED.md](DEFERRED.md).

### U04. Camera rigs, sequences, and cinematic authoring

P2. Extension and planned:
[camera/cinematics](future-work/camera-and-cinematics.md).

Follow cameras, first-person modes, portal views, and scripted cameras already
exist. Add reusable rigs, blending, rails, target framing, shake channels, and
an authored timeline for camera/animation/audio/events. Establish authority for
sequence events and use existing animation/audio clocks rather than wall-time
callbacks that drift independently.

First useful slice: a boss introduction blends from gameplay to a rail camera,
plays animation/sound, can be skipped, and returns control correctly. Test a
late join, player death, and scene unload during the sequence. Do not make a
cinematic timeline the required way to implement ordinary gameplay.

## Choose a genre-sized vertical slice

| Game target | Build first | Playable proof | Add after the proof |
|---|---|---|---|
| Competitive shooter | G01, G02, G08, U01, W07 | Two players move, aim, fire, reload, and contest a moving door under adverse network conditions. | G10 killcams, R04 impact decals, G07 bots, U02 spatial filtering. |
| MOBA comparable in needs to League | G03, G04, G05, G06, C02 | Two teams, lanes, minions, wards, towers, and a fight with stacked effects and interrupts. | Replay/spectating, skin/material workflows, hosted matches, richer tactical AI. |
| MMORPG | G01, G03, G05, W02, W04, W05 | Two authored regions, a party, an encounter, persistent loot, travel, and reconnect without duplication. | Larger populations, social systems, simulation LOD, richer avatars and terrain. |
| Co-op action RPG | G03, G08, G09, W03, W06 | A reusable enemy encounter with synced attacks, gear, rewards, and a saved quest objective. | Procedural dungeons, richer animation, voice, and cinematic tools. |
| Survival/building/physics game | P01, P03, P05, W01, W02, W03 | A constructed mechanism, breakable wall, editable ground, and persisted changes across a cell transition. | Vehicles, water, procedural worlds, and selected soft-body effects. |
| Large battle or village simulation | G05, G06, G07, C02, R05 | Agents navigate, make inspectable decisions, fight or work, and change fidelity without losing state. | Flow fields, shared animation, procedural population and settlement tools. |

These are proposed demonstrations, not claims that Atomic currently meets the
corresponding production scale. A small coherent game is more informative than
many disconnected feature demos.

## Implementation guardrails and acceptance evidence

Keep reusable engine systems under `mono.engine`. Studio owns authoring and
inspection, products own adapters, and game packages own game-specific rules.
New module placement must follow the existing layer stack and target graph.
Do not use this document as permission to create upward dependencies.

Keep authoritative facts in ECS, stable names in durable formats, and copied
messages across worlds. Extend the current graph, content, replication,
profiling, and replay paths. Cosmetic GPU work can finish asynchronously;
authoritative simulation must honour the fixed-tick/join contract. A GPU result
becoming ready on an arbitrary future frame cannot silently steer gameplay.

For each selected slice, agree on evidence before expanding it:

| Evidence | What it must answer |
|---|---|
| Playable scene | Can an author use the feature end to end through the intended script/editor/content path? |
| Focused headless tests | Are rules, rejection cases, interruption, lifecycle, and replication/persistence behaviour correct? |
| Visual or audible verification | Do animation, shading, decals, sound, and transitions actually work in the real client? |
| Release profile | What are CPU tick time, GPU time, tail latency, allocations, residency, uploads, and network cost in the named scene? |
| Failure/recovery exercise | What happens on capacity refusal, stream-out, disconnect, duplicate request, restore, or interrupted publication? |
| Inspectable state | Can the author see why an agent, graph, server, or renderer made its decision? |

Keep measurements under the build output and add a `just` job for any new
benchmark. Record preset, backend, hardware, scene size, and settings with each
performance result. Existing tests were inspected as implementation evidence
for this research; no runtime checks or performance results are claimed here.

The most useful next delivery is one complete slice from the genre table.
Build the shared systems that slice actually needs, then use the result to
choose the next feature.
