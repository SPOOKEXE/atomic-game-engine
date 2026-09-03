# Character system plan

## Goal

Build one inspectable character stack that supports the default R6 character,
artificial characters, bots, imported Roblox characters, custom controllers,
animation, clothing, accessories, and later graph-authored behaviour.

Characters do not know about players. Human input, bot AI, replay, and scripts
all produce the same bounded character intent. `Players` owns the player roster,
active possession, character swapping, client input routing, camera subject,
network permission, and player respawn policy. A player may own several
characters while actively controlling one, and a character may exist for its
whole life without a `Player` instance.

`Humanoid` remains the Roblox-facing compatibility class. It must forward to
the same public controller instances that a game can inspect and edit directly.
It must not hide a second movement or state system.

The first complete stack is an upright R6 character with one capsule collider,
ground and air movement, explicit character states, an animator, a bone
controller, rigid limb bindings, and accessory attachment. Later locomotion
modes and graph tools extend those cuts without replacing them.

## Existing foundation

The engine already has useful pieces. This work must extend them rather than
build parallel copies.

| Existing piece | Current role | Required change |
|---|---|---|
| `scene::Character` | Links the model, root, humanoid, and current player owner | Remove `Owner`; link only character-owned controller and rig instances |
| `scene::PlayerCharacter` | Implements one `Player.Character` reference | Replace with a `Players`-owned roster plus one active possession; keep `Player.Character` as the compatibility view of the active character |
| `scene::Humanoid` | Holds movement, health, and ground state | Reduce to health plus controller links, then expose movement through computed forwarding properties |
| `scene::MakeCharacter` | Builds the block character | Make it a player-independent factory, use a real capsule, and build the default controller tree |
| `scene::CharacterLimb` | Carries rigid limbs from the root | Replace with one general pose binding used by limbs, tools, and accessories |
| `physics::GroundCharacters` | Finds ground with a ray | Replace with the capsule controller's contact probe |
| `scene::StepCharacters` | Writes horizontal velocity and jump speed | Replace with motion-controller output |
| `physics::ClipCharacterVelocity` | Stops commanded velocity at walls | Replace with bounded capsule sweep and slide |
| `scene::Skeleton` and `scene::Bone` | Hold an inspectable rig and pose | Keep as the rig storage used by the bone controller |
| `scene::Animation`, `AnimationBuffer`, `Animator`, and `AnimationTrack` | Hold clips, procedural data, playback, and rig binding | Bind tracks to committed character states without adding another playback type |
| `render::EvaluateAnimations` | Samples and blends visible poses | Keep for visual-only animation, then extract shared sampling when root motion needs authority-side evaluation |
| `nodegraph` | Studio-only graph model and UI | Keep out of engine runtime; add a shared graph document only when state and animation graphs ship |
| `graph::EngineGraph` | Schedules engine frame work | Do not use it as a character behaviour graph |

The current default root is described as a capsule but uses a box. That is the
first mismatch to remove. `ShapeKind::Capsule` already exists in scene,
queries, narrow phase, solving, and Studio collider drawing.

## External design checks

The plan was checked against current first-party engine documentation. These
sources confirm useful cuts but do not override this engine's ECS and layer
rules.

- Roblox's current
  [`ControllerManager`](https://create.roblox.com/docs/reference/engine/classes/ControllerManager/RootPart)
  separates moving direction, facing direction, up direction, active motion
  controller, and explicit ground and climb sensors. This plan adopts those
  visible cuts and also distinguishes the requested controller from the one
  that successfully activated.
- Roblox's
  [`ControllerPartSensor`](https://create.roblox.com/docs/reference/engine/classes/ControllerPartSensor/SensedPart)
  exposes its hit part, frame, normal, material, search distance, and mode. This
  plan uses scriptable sensor instances instead of burying ground and climb
  queries inside movement code.
- Roblox's current
  [`Humanoid.MoveDirection`](https://create.roblox.com/docs/reference/engine/classes/Humanoid/MoveDirection)
  is read-only while `ControllerManager.MovingDirection` is writable. The shim
  follows that split. `Humanoid:Move()` and the primary controller remain the
  intent-writing doors.
- Roblox rigid accessories match named attachments between an accessory handle
  and the character, as documented by
  [`Humanoid:AddAccessory()`](https://create.roblox.com/docs/reference/engine/classes/Humanoid/AddAccessory)
  and the
  [rigid accessory specification](https://create.roblox.com/docs/art/accessories/specifications).
  The accessory plan uses the same stable-name rule without adding a physics
  weld to the visual R6 body.
- Roblox keeps `Animator` useful without `Humanoid` through an
  `AnimationController`, as shown in
  [Use animations](https://create.roblox.com/docs/animation/using). This
  supports keeping animation independent and binding it to a character only
  through explicit references.
- Unity's current
  [Character Controller](https://docs.unity3d.com/6000.0/Documentation/Manual/class-CharacterController.html)
  documents the same kinematic capsule controls used here: slope limit, step
  offset, skin width, and collision-constrained movement without a rigid body.
  Its guidance that skin width be about ten percent of radius is a starting
  default to test, not a substitute for profiling this engine.
- Roblox's
  [Adaptive Animation](https://create.roblox.com/docs/characters/adaptive-animation)
  maps named joints plus a reference T-pose so clips can cross different body
  shapes. The future `RigRetargetProfile` below reserves this seam without
  putting retarget data in `Animator` or `Bone`.
- Unreal's current
  [StateTree overview](https://dev.epicgames.com/documentation/en-us/unreal-engine/overview-of-state-tree-in-unreal-engine)
  shows the useful part of hierarchical states: parent tasks and transitions
  stay active while one leaf state is selected. Character state blueprints may
  add that form later, but the first locomotion machine remains flat and easy
  to test.

## Non-negotiable design rules

1. The ECS owns every saved or replicated fact. No private controller table may
   mirror the store.
2. The root capsule is the only simulated body in the default character.
   Limbs, accessories, bones, and animation never become competing physics
   authorities.
3. Character intent, state choice, motion solving, animation choice, and pose
   output are separate steps with explicit rows between them.
4. `Humanoid` forwards to the modular stack. It never runs a hidden fallback
   controller beside it.
5. The authority commits movement and character state on the fixed tick.
   Presentation may interpolate or throttle poses but may not change gameplay.
6. A state is a semantic fact such as `Running` or `Freefall`. A motion
   controller is the algorithm used to move, such as ground or air movement.
   Several states may use one motion controller.
7. Node graphs are authoring documents. Runtime consumes a validated, bounded,
   immutable compile result and never walks editor nodes each tick.
8. Instance references and graph type names cross files and the wire as stable
   names or entity references. Declaration-order numbers remain process-local.
9. Work stays in `mono.engine`. Studio supplies editing views only.
10. The character system has no `Player` reference, `Players` include, local
    input rule, camera rule, or respawn policy. Higher systems may drive a
    character through intent, but the dependency never points back upward.
11. Character storage, simulation, animation selection, appearance, and
    ragdoll orchestration form one named engine subsystem even where the layer
    rule places physics, scene storage, and rendering in separate modules.
    `Players` is an adapter and consumer of that subsystem, never part of it.

## Character isolation, drivers, and possession

The generic character factory accepts a character description and transform.
It does not accept a player, choose a player name, create player containers, or
apply a player respawn policy. `Players` calls the same factory available to bot
spawners, NPC scripts, tests, and authored worlds.

### Neutral character intent

`CharacterIntent` is the only gameplay input consumed by
`CharacterController`. It contains:

- desired movement and facing directions
- jump press and release edges
- optional look or aim direction
- bounded named action requests
- source tick and monotonic sequence
- an intent revision

The row contains no pointer or player id. A human client adapter, server bot,
AI graph, replay reader, or script command validates its own source and writes
the same data. Missing or expired intent means neutral input, not reuse of the
last movement forever. Fixed-tick simulation consumes one committed snapshot,
so input cannot change halfway through state evaluation.

Only one source has the control lease for a character at a time. Lease storage
lives with the higher-level driver or possession system, not in the character
core. A future shared-control mixer must produce one resolved intent before the
character tick rather than letting multiple writers race on the component.

Bots run on the server and emit deterministic intent with explicit seeds and
bounded work. Replays emit recorded intent. Neither needs a fake `Player`, a
local client, or a different controller path.

### `Players` roster and active possession

`Players` owns one authoritative relation table instead of mirrored forward
and reverse links. For each player it records:

- an ordered, bounded roster of live character model references
- the active possessed character, or none
- the possession revision and input channel
- the player-specific spawn and respawn policy

The script surface exposes `GetCharacters()`, `AddCharacter(character)`,
`RemoveCharacter(character)`, and `SetActiveCharacter(character)`. Roblox
compatibility keeps `Player.Character` as the active character and routes an
assignment through the same validated possession transaction. Roster changes
and active-character changes have separate bounded event queues so scripts can
distinguish ownership from control.

A possession swap commits atomically at the store mutation barrier:

1. Validate that the target is a live character in the player's roster and
   that the caller has authority.
2. Revoke the old input route and write neutral intent to the old character.
3. Change the authoritative active relation and increment its revision.
4. Grant the new input route, then retarget the client camera if it is not
   scriptable.
5. Replicate the relation change before accepting intent for the new revision.

The old character remains alive. It may idle, receive a bot driver, or be
possessed again later. Removing an active character first performs the same
revocation and then chooses no replacement unless an explicit player policy
selects one. A character belongs to at most one player roster, and one character
cannot be actively possessed by two players. Bot control is a lease, not roster
membership, so a bot may drive an unpossessed character without changing who
may select it later.

Client move messages identify the possession revision and sequence. The server
resolves the sender through `Players` and rejects an intent whose target or
revision is not the sender's active possession. A client-supplied entity alone
is never authority. Camera follow and local input read the active possession
from `Players`; neither searches `Character::Owner`.

Health and death remain character facts. `Players` observes the death of an
active player character and applies that player's respawn policy. An artificial
character is respawned only when its own bot, spawner, or game script requests
it.

## Default R6 instance tree

The default spawn should produce this visible and scriptable shape:

```text
Character [Model, Character]
|-- HumanoidRootPart [Part, capsule Collider, simulated root, Skeleton]
|   `-- Root [Bone]
|       `-- Torso [Bone]
|           |-- Head [Bone]
|           |-- LeftShoulder [Bone]
|           |-- RightShoulder [Bone]
|           |-- LeftHip [Bone]
|           `-- RightHip [Bone]
|-- Head [Part, PoseBinding]
|-- Torso [Part, PoseBinding]
|-- Left Arm [Part, PoseBinding]
|-- Right Arm [Part, PoseBinding]
|-- Left Leg [Part, PoseBinding]
|-- Right Leg [Part, PoseBinding]
|-- Humanoid [Humanoid shim]
|   `-- Animator [Animator]
|-- CharacterController
|   |-- GroundSensor
|   |-- ClimbSensor
|   |-- GroundController
|   |-- AirController
|   |-- ClimbController [disabled until supported]
|   |-- SwimController [disabled until supported]
|   |-- SeatController [disabled until supported]
|   |-- PhysicsController
|   |-- CharacterStateController
|   |   |-- Idle
|   |   |-- Running
|   |   |-- Jumping
|   |   |-- Freefall
|   |   |-- Landed
|   |   |-- Climbing
|   |   |-- Swimming
|   |   |-- Seated
|   |   |-- Dead
|   |   `-- Physics
|   |-- BoneController
|   `-- CharacterAnimationSet
`-- Accessory instances
```

Disabled future controllers and states may be omitted from the first spawned
tree if their classes are registered and `EnsureCharacterStack` can add them
when requested. Do not create dead instances merely to make the tree look
complete.

### R6 dimensions

Use named constants and one derivation:

```text
R6 height                 5.0 m
R6 capsule radius         1.0 m
R6 capsule half segment   1.5 m
total height              2 * (radius + half segment) = 5.0 m
```

The capsule remains upright. `Collider::Extent` is the one source of its radius
and half segment. Controller ground probes derive the feet and total height from
that collider. Do not add a second radius or height to `CharacterController`.

The root part stays invisible. Its `Part` size should describe the capsule's
full bounds for selection and Studio handles. The six visible R6 parts remain
non-simulated and non-colliding.

## Public instance model

### `CharacterController`

This is the primary scriptable controller. It owns intent, tuning, links, and
observable output, but it does not own animation clip bytes or bone poses.

Required links:

- `RootPart`
- `StateController`
- `BoneController`
- `Animator`
- `AnimationSet`
- `GroundSensor`
- `ClimbSensor`
- `RequestedMotionController`
- `ActiveMotionController`

Required input and tuning:

- `MovingDirection`
- `FacingDirection`
- `UpDirection`
- `JumpRequested`
- `Enabled`
- `AutoRotate`
- `WalkSpeed`
- `JumpSpeed`
- `Acceleration`
- `BrakingAcceleration`
- `AirControl`
- `TurnSpeed`
- `MaxSlopeAngle`
- `StepHeight`
- `GroundSnapDistance`
- `SkinWidth`
- `MinimumMoveDistance`
- `MaxSlideIterations`

Required observable output:

- `Grounded`
- `GroundNormal`
- `GroundEntity`
- `MoveDirection`
- `Velocity`
- `ExternalVelocity`
- `CurrentState`
- `ActiveMotionController`

`Grounded`, support data, and velocity are written by the authority's physics
step. They are not writable script properties. Controller tuning and intent are
writable on an authority and refused on a replica through the existing store
rule.

`MovingDirection` is the requested unit direction. `MoveDirection` is the
read-only direction the controller actually committed after state, collision,
and support handling. `FacingDirection` is separate because strafing and
shift-lock face somewhere other than movement. `UpDirection` defaults to world
up and is normalised at the property boundary. A zero, NaN, or infinite value
is refused.

`ExternalVelocity` is the explicit door for knockback, moving supports, and
other motion not produced by character intent. A kinematic controller without this
door becomes immune to the rest of the world. Incoming impulses accumulate
here, then decay or are replaced according to a named policy.

Roblox import and compatibility use `ControllerManager` as a class alias over
the same controller component and properties. `Instance.new("ControllerManager")`
therefore creates the same storage and runs the same systems as
`CharacterController`. `ControllerPartSensor` similarly uses the same sensor
component as `CharacterSensor`. These are class-tree aliases, not adapters with
copied fields.

### Character sensor instances

`CharacterSensor` is an inspectable query result with these fields:

- sensor mode: floor, climb, water, or seat
- search distance and local search offset
- hit entity
- hit frame
- hit normal
- sensed material
- valid flag
- output revision

`GroundSensor` and `ClimbSensor` are default child instances referenced by the
controller. Physics updates the sensors needed by the requested motion
controller before activation. Scripts may read the output and may provide a
custom sensor instance, but output fields remain read-only on replicas and in
ordinary gameplay.

Sensors do not query lazily from a property getter. Lazy physics reads make the
result depend on which script happened to inspect it first. Active sensors run
once at the fixed point in the tick, write one result, and every consumer reads
that same result.

### Motion controller instances

All motion controller classes share one internal component. The class prototype
selects the algorithm, as the light classes already select their kind. The kind
is not a writable property because a `GroundController` that runs air movement
is a contradictory object.

| Class | Purpose |
|---|---|
| `GroundController` | Walk, brake, turn, sweep, slide, step, snap, and ride supports |
| `AirController` | Preserve vertical motion, apply air control, and detect landing |
| `ClimbController` | Move along a validated climb surface |
| `SwimController` | Move with buoyancy and three-dimensional intent in a water volume |
| `SeatController` | Follow a seat or vehicle driver while preserving character state |
| `PhysicsController` | Stop authored locomotion and accept external motion only |

The first shipping set needs ground, air, and physics. Climb, swim, and seat are
later slices with the same interface. The active instance receives a compact
input and writes a candidate displacement plus state signals. It never writes
the model or animation directly.

`RequestedMotionController` is author intent. `ActiveMotionController` is what
actually passed activation checks this tick. A ground controller with no valid
walkable support remains requested but cannot become active, so state policy
may select air without destroying the request. The two references must never be
collapsed into one field.

Shared controller tuning belongs on `CharacterController`. Per-mode multipliers
and response belong on each motion controller. Ground movement exposes speed
and turn factors, acceleration and deceleration time, friction weight, and
ground offset. Air, climb, and swim expose only the values their algorithms
read.

### `CharacterStateController`

This is the visible state machine. It holds:

- `CurrentState`
- `PreviousState`
- `RequestedState`
- `ActiveMotionController`
- `AutomaticTransitionsEnabled`
- `Revision`

Each child `CharacterState` instance has a stable name, an enabled flag, and a
motion-controller reference. Built-in code supplies the first transition
policy. Scripts may disable a state or request an enabled state. A request is
validated and committed at one fixed point in the tick.

State identity is a stable `core::Name`, not a closed runtime enum. Built-in
names map to Roblox `HumanoidStateType` values at the compatibility boundary.
Unknown custom names remain intact through save, replication, and import. A
custom state may choose an existing motion controller, `PhysicsController`, or
a registered bounded controller kind without modifying the built-in enum.

`CharacterParameters` is the one bounded typed parameter surface shared by
state policy and animation selection. It supports declared boolean, scalar,
vector, trigger, and stable-name values such as speed, grounded, aim, mood, and
game-defined inputs. Declarations provide defaults, replication policy, numeric
bounds, and writer authority. Runtime storage uses compiled slots, while files
and scripts use stable names. State graphs and animation trees may read the
same committed snapshot but may not keep shadow parameter tables.

The initial transition policy is data-shaped and ordered:

1. `Dead` when health reaches zero.
2. `Seated`, `Swimming`, or `Climbing` when the matching controller has a valid
   attachment or volume.
3. `Jumping` when a grounded jump is accepted.
4. `Freefall` when there is no walkable support.
5. `Landed` for one committed tick after an airborne-to-ground transition.
6. `Running` when grounded speed is above its threshold.
7. `Idle` otherwise.

`Physics` is explicitly requested and disables automatic locomotion until
another state is requested. Ties are resolved by this stated order, never by
archetype or hash iteration order.

Transition arbitration is explicit:

- death and invalid-controller recovery are non-interruptible system safety
  transitions
- an accepted manual request beats ordinary automatic locomotion for that tick
- each transition declares priority, minimum residence time, cooldown,
  re-entry permission, and whether it may interrupt the current state
- at most one transition commits per fixed tick
- requests that lose arbitration are consumed or retained according to their
  declared policy, never by incidental evaluation order

Gameplay state and animation state remain separate. `CharacterStateController`
owns semantic gameplay facts such as dead, seated, stunned, or airborne. The
animation selector may crossfade among locomotion poses, aim poses, and action
layers without changing gameplay state. Animation completion may enqueue a
future state request, but it cannot mutate state during pose sampling.

State entry and exit are recorded into a bounded `CharacterStateChanges` world
resource. The script layer drains it and fires `StateChanged` and compatibility
events after the store mutation barrier. Property changes still use the normal
property signal path.

### `Humanoid` compatibility shim

After migration, `Humanoid` stores only facts that remain humanoid concerns:

- `Controller`
- `Health`
- `MaxHealth`

Movement and state properties become computed forwarding properties. There is
one field on the controller and no shadow field on the humanoid.

| Humanoid surface | Forwards to |
|---|---|
| `RootPart` | `CharacterController.RootPart` |
| `MoveDirection` | `CharacterController.MoveDirection`, read-only |
| `WalkSpeed` | `CharacterController.WalkSpeed` |
| `JumpPower` | `CharacterController.JumpSpeed` |
| `AutoRotate` | `CharacterController.AutoRotate` |
| `Enabled` | `CharacterController.Enabled` |
| `FloorMaterial` | Material resolved from `GroundEntity`, read-only |
| `GetState()` | `CharacterStateController.CurrentState` |
| `ChangeState(state)` | Validated state request |
| `SetStateEnabled(state, enabled)` | Matching `CharacterState.Enabled` |
| `GetStateEnabled(state)` | Matching `CharacterState.Enabled` |
| `Move(direction)` | Writes `CharacterController.MovingDirection` |
| `TakeDamage(amount)` | Existing authority-only health path |
| `Died` | Health transition event |
| `StateChanged` | Drained state-change record |

Full shim coverage is tracked by a checked-in compatibility manifest, not by a
claim in prose. Every supported Roblox `Humanoid` property, method, and event is
classified as forwarded, implemented by another engine instance, deprecated
alias, or deliberately unsupported with a reason. The bindings check fails when
a manifest entry has no generated Luau and JavaScript surface.

The main groups are:

| Compatibility group | Route |
|---|---|
| Movement speed, jump, slope, facing, and root properties | `CharacterController` and active motion controller |
| `Jump`, `Move()`, `MoveTo()`, and walk goals | controller intent, with `MoveToFinished` from the goal tracker |
| `GetState()`, `ChangeState()`, enabled states, and `EvaluateStateMachine` | `CharacterStateController` |
| `PlatformStand`, `Sit`, and `SeatPart` | physics or seat state and its motion controller |
| `FloorMaterial`, relative floor velocity, and move velocity | controller sensor and committed output |
| Health, max health, damage, death, and health events | the retained humanoid health row |
| Rig type, body-part queries, scaling, and descriptions | rig profile and appearance system, not locomotion |
| Accessories | `Accessory` plus pose binding |
| Animation loading and playing | existing `Animator`, `Animation`, and `AnimationTrack` path |
| Display name, health display, and camera offset | presentation components outside the motion controller |

Deprecated aliases may forward when cheap. They must never grow duplicate
storage. Existing project decisions such as tool equipment remain routed
through `Tool`, `Backpack`, and the tool-grip path instead of being re-created
inside `Humanoid`.

`HipHeight` must stop meaning full capsule height. During migration, legacy
values are converted into the new controller's ground offset or the capsule
dimensions. The final property follows Roblox's root-to-ground offset meaning.

Compatibility methods that are only aliases call the modular objects. They do
not contain movement code. Unsupported Roblox behaviour must be listed in an
import compatibility table with a clear warning. It must not silently create a
third partial controller.

`EnsureCharacterStack` supplies the default controller when an authority owns a
humanoid with no live controller reference. It runs after load and before
character linking. It gathers missing rows first and creates them after the
iteration, so archetype movement cannot invalidate the walk. A replica never
mints substitute entity ids. It waits for the authority's replicated stack.

### `BoneController` and pose bindings

`BoneController` points to the root entity carrying `Skeleton` and to the
`CharacterController`. It applies procedural pose controls after clip blending
and before bone resolution.

`PoseBuffer` is the transient per-sample output shared by clip sampling,
blending, procedural controls, inverse kinematics, retargeting, and rendering.
It contains skeleton-local transforms and validity bits in skeleton order. It
is scratch data, is never an instance, and is not saved or replicated.
`AnimationBuffer` remains durable baked clip data with tracks, keys, markers,
curves, and timing. An `Animation` or `AnimationTrack` may point to an
`AnimationBuffer`; it never points to a transient `PoseBuffer`.

Child `BoneControl` instances expose one operation each:

- target `Bone`
- mode: additive or override
- local target frame
- weight
- priority
- enabled flag

Look, aim, recoil, simple inverse kinematics, and scripted offsets all use these
rows. No private list mirrors them. The default R6 controller may begin with no
active controls.

Future custom rigs use a `RigRetargetProfile`. It maps stable semantic joint
names to `Bone` instances and stores the reference-pose adjustment and optional
rotation limits for each mapping. R6 ships with a built-in six-joint profile.
R15 and imported custom rigs provide their own. Animation clips continue to
name their source rig, while the retarget profile is the explicit adapter to a
different target skeleton. Retargeting is compiled and cached by clip, source
profile revision, and target profile revision.

Replace `CharacterLimb` with `PoseBinding` once the bone path is ready.
`PoseBinding` contains a target entity and local offset. Its target may be a
root `Transform`, a resolved `Bone`, or an `Attachment`. One pre-render pass
resolves the target frame and places the bound part or model.

This one component serves three existing needs:

- rigid R6 limbs driven by bones
- tool handles driven by the hand attachment
- accessory handles driven by named attachments

The old root-offset path remains only as a load migration. After saved content
has been converted, delete its runtime branch and the old component.

## Physics controller

The final controller is a queried upright capsule, not a dynamic body whose
horizontal velocity is overwritten and then repaired by the contact solver.
The collider remains in the broad phase for queries, triggers, and other bodies,
but the character movement pass owns its displacement.

Start with `SkinWidth` at ten percent of capsule radius and
`MinimumMoveDistance` at zero. Reject a negative value, a step height greater
than total capsule height, and any non-finite tuning value at the property
boundary. Defaults remain named constants and must be tuned against the R6
physics cases rather than copied blindly from another engine.

One ground or air move does this:

1. Read the committed intent, active motion controller, support motion, and
   external velocity.
2. Build desired displacement for the fixed physics step.
3. Sweep the capsule along the displacement.
4. Stop at `SkinWidth` before the first solid hit.
5. Project the remaining displacement onto the hit plane.
6. Repeat up to `MaxSlideIterations`, with a small fixed default such as three.
7. If grounded and a low obstacle blocked the first move, test step-up,
   forward, and step-down sweeps with headroom checks.
8. If the character was grounded and did not jump, snap down within
   `GroundSnapDistance` onto a walkable surface.
9. Commit transform, velocity, support, and state signals together.

A walkable surface compares its normal with `MaxSlopeAngle`. A steep surface is
a wall for ascent and a slide surface for external downward motion.

Moving support uses velocity at the contact point:

```text
support velocity = linear velocity + angular velocity cross contact offset
```

The controller adds that displacement before its own intent. Standing still on
a platform therefore rides the platform without copying the platform transform
into private state.

Dynamic-body interaction has two explicit paths:

- a character hit may apply a bounded push impulse to a dynamic body
- an impulse applied to the character adds to `ExternalVelocity`

Both are authority-only and deterministic. Character-to-character contacts use
entity id as the stable tie break when two capsule moves conflict.

The old `GroundCharacters`, `StepCharacters`, and `ClipCharacterVelocity`
runtime path is deleted after capsule sweep parity tests pass. Leaving both
enabled would make system registration order decide movement.

## Tick and frame order

```text
fixed tick
  higher-level drivers resolve one neutral character intent
  ensure character links and default stack
  commit the typed parameter snapshot
  sample controller sensors
  choose requested state
  evaluate root motion, if enabled
  run active motion controller
  sweep and commit capsule movement
  commit state transition
  update animation-track intent
  advance tracks and fades
  publish replication changes

presentation frame
  sample and blend visible animation tracks
  apply procedural bone controls
  resolve bones
  resolve pose bindings for limbs, tools, and accessories
  build skinning palettes and draw
```

Root motion is disabled on the default R6 controller. When enabled, it must be
sampled on the authority at the fixed tick and converted into controller intent
before the capsule sweep. A render-only pose may never move the authoritative
root.

The existing render sampler can remain for visual-only tracks. Before root
motion ships, extract format decode and deterministic sampling into a shared
engine animation module that links `assets` and `scene`. Render then consumes
its pose result instead of owning the only sampler. Physics consumes only the
scene-side root-motion intent row, preserving the layer direction.

## Animation binding

`CharacterAnimationSet` is an instance with child `CharacterAnimationBinding`
instances. Each binding contains:

- character state name or reference
- `Animation` reference
- looped flag
- priority
- fade-in and fade-out time
- reference movement speed
- optional motion-controller restriction

`UpdateCharacterAnimations` reads the committed state and actual controller
velocity. It creates or reuses one `AnimationTrack` under the existing
`Animator` for each active binding, adjusts playback speed from movement speed,
and fades old state tracks out. Tracks are not created and destroyed every
tick.

Animation tracks support named markers, typed curves, and events stored in
`AnimationBuffer`. Event traversal defines forward play, reverse play, loops,
multi-loop time steps, and explicit seeks. Each crossed occurrence fires once
with a track generation and loop index. Seeking is silent by default and may
opt into marker emission. Gameplay consumes authority-side marker records from
a bounded queue, never callbacks fired inside the sampler.

Locomotion clips may join a named sync group. The group has one leader and a
normalised phase; followers phase-match without rewriting their assets. Blend
layers support per-bone masks, additive reference poses, and stable priority so
an upper-body action can play over lower-body locomotion. Invalid or empty masks
fail at binding validation rather than silently affecting the whole rig.

Action tracks started by a script remain independent and win through existing
animation priority and weight rules. The character driver owns only tracks it
created, identified by the binding instance that created them.

The default set contains idle, walk or run, jump, fall, and land bindings. It
may point to built-in clips or world-owned `AnimationBuffer` instances. The
procedural buffer path therefore works without special controller code.

`AnimationBuffer` is a scriptable instance in both Luau and JavaScript. Its
bounded authoring surface can clear a draft, declare duration and source rig,
set or remove a joint channel, append or replace transform keys, set curves,
set markers, validate, and bake. Bulk channel input uses the engine buffer type
so generated animation does not require one cross-language call per key. Edits
build a private draft owned by the script call, then atomically replace the ECS
component and increment its revision after validation. A playing track sees
either the old complete revision or the new complete revision, never a partly
written clip. Runtime playback treats each committed revision as immutable.

Limits cover clip duration, joints, channels, keys, markers, curve samples, and
total bytes. Times and transforms must be finite, keys are sorted with a stable
same-time rule, and invalid joint names or malformed packed buffers return a
named error without changing the active revision.

Markers, curves, source rig, and additive reference metadata require a
versioned extension of the existing AAN animation format. The asset reader
continues accepting the current version with empty defaults, the writer emits
only the new canonical version, and unknown future sections fail or remain
opaque according to an explicit section policy. Save, CDN, procedural bake,
and imported assets all pass through the same validator.

Visual pose sampling may be distance or visibility throttled. Track time,
marker traversal, root motion, gameplay curves, and authoritative events still
advance every fixed tick. When presentation resumes, it samples the current
time once rather than replaying every skipped visual pose.

### Procedural animation and baking

Procedural nodes read the committed parameter snapshot and write a
`PoseBuffer`. Randomised nodes require an explicit saved seed. Every node has a
bounded iteration or sample count, and runtime evaluation may not allocate or
compile.

`AnimationRecorder` bakes procedural work through this path:

1. Evaluate the procedural graph or scripted pose at a declared sample rate.
2. Capture local transforms and selected curves from each `PoseBuffer`.
3. Preserve named markers and root-motion policy.
4. Simplify keys against declared position, rotation, and scale tolerances.
5. Write a validated world-owned `AnimationBuffer` that ordinary animations
   and tracks can reference.

The bake is an authoring job and may run in parallel over independent bones or
sample ranges when merge order is fixed. Playback never bakes implicitly.

An animation tree later replaces only the binding-to-blend decision. It still
outputs ordinary track weights or a blend plan for the same animator and bones.

## Accessories

Add an `Accessory` class and component. An accessory normally owns one `Handle`
part and may own more visual descendants. The handle is non-simulated and
non-colliding by default.

Attachment resolves by stable name:

1. Find an `Attachment` below the accessory handle.
2. Find the matching attachment name below the character rig.
3. Create or update one derived `PoseBinding` on the handle.
4. Compose the handle attachment inverse with the character attachment frame.
5. Leave the accessory unattached, with a visible diagnostic, when no unique
   match exists.

The derived binding is rebuilt when the accessory, handle, attachment name,
character rig, or ancestry changes. Cache the resolved pair by the revisions of
those sources. Do not scan the whole character every frame.

Removing an accessory removes only its binding. Reparenting it to another
character resolves again. Deleting a target attachment leaves the accessory in
place rather than sending it to the origin.

Rigid accessories use `PoseBinding`. Skinned accessories share the character's
bone palette through an explicit rig reference.

## Appearance and clothing

`CharacterAppearance` is independent of players and owns the character's
visible description:

- body colours and body-part asset references
- body and rig scale values
- classic shirt, pants, and graphic references
- animation-set reference
- ordered equipped accessory and clothing references

Applying an appearance is an authority-side transaction. It validates all
assets first, then changes the rig and visible descendants at one mutation
barrier. A failed asset leaves the previous valid appearance active. The
appearance revision invalidates attachment, retarget, cage, and skinning caches
without scanning unchanged characters every frame.

Clothing has three explicit paths:

1. Classic clothing is material or texture data projected by the supported rig
   profile.
2. Rigid accessories attach by stable named attachments through `PoseBinding`.
3. Layered clothing stores an inner cage, outer cage, stable layer order,
   puffiness, and a reference to the character's shared bone palette.

Layered clothing deformation compiles a cage mapping when the body mesh,
clothing mesh, rig profile, scale, or layer order changes. The presentation pass
then applies the cached mapping and current skinning palette. Invalid cages or
ambiguous layer ordering produce a visible diagnostic and do not replace the
last valid compiled result.

The first layered-clothing slice is deterministic cage deformation, not cloth
simulation. Wind, collisions between cloth layers, tearing, and dynamic cloth
remain future systems behind the same clothing instance data. They must not add
physics bodies to the default character controller.

## Ragdoll seam

Ragdoll is a controller handoff, not a second always-active character body. A
`RagdollController` owns the generated joint-body references, activation state,
entry velocity, and recovery policy. Entering ragdoll disables capsule-authored
movement, transfers root and limb motion to authority-owned joint bodies, and
changes the semantic state to a named ragdoll or physics state. Animation may
continue only on bones not controlled by the ragdoll mask.

Exiting ragdoll chooses a validated capsule recovery pose, checks overlap and
ground support, copies the resolved root velocity, disables the joint bodies,
and restores the requested motion controller. If no safe capsule pose exists,
the character remains ragdolled. Joint bodies and their contacts are saved only
when snapshot policy explicitly requires an active ragdoll; ordinary place
files save the rig and ragdoll configuration, not a transient collapsed pose.

## State machines and future node graphs

The built-in state machine should ship as ordinary fixed code over explicit
state instances first. This gives a working character and proves the inputs a
graph actually needs.

The first machine is flat. The later state blueprint may group leaf states
under parents such as `Alive`, `Grounded`, and `Airborne`. Parent entry tasks and
transitions remain active with the chosen leaf, allowing one death transition
or one shared airborne task instead of copies on every child. Leaf-to-root
transition order and stable state id tie breaks must be part of the compile
format.

When graph authoring ships, add a shared, device-free behaviour graph document
and compiler under `mono.engine`. Do not link `scene`, `physics`, or any engine
module to Studio's `nodegraph` library. Studio adapts its canvas model to the
shared document at save time.

The shared graph layer supports three documents:

| Document | Runs | Output |
|---|---|---|
| `CharacterBlueprint` | Once when building or spawning | A validated instance template and controller links |
| `CharacterStateBlueprint` | Fixed tick | A requested state and motion-controller selection |
| `AnimationTree` | Fixed tick plus presentation sample | Track weights, playback speeds, and an optional root-motion sample |

Graph rules:

- node type ids are stable strings
- node and edge counts are bounded
- data ports are typed
- graph parameters resolve against the shared `CharacterParameters` declaration
- cycles are rejected unless a node kind explicitly owns bounded state
- compile happens after edits, never in the tick
- the compile result is immutable and revision-cached
- the document stores a revision and the compile result records which revision
  it covers; runtime refuses a stale plan and keeps the last valid plan active
- deterministic evaluation uses declaration order plus stable node ids as ties
- procedural randomness requires an explicit saved seed
- unknown node types remain in the document and make compilation fail with a
  named diagnostic
- editor position, folding, groups, and comments never affect the runtime hash
- scripts receive entry and exit events but arbitrary script callbacks do not
  run in the middle of the physics solver

Do not force one universal node runtime too early. State graphs and animation
trees are the first two consumers that justify a shared document and compiler.
The frame graph, bake graph, and Studio canvas remain separate because they
solve different runtime problems.

The seams above deliberately reserve later work without pulling it into the
first implementation. Full cloth simulation may consume layered-clothing cage
output, motion matching may become an animation-tree node backed by a bounded
search index, and facial rigs may add named face bones or blend channels to the
same pose and curve path. None of those may replace the ordinary clip sampler,
character state controller, or controller intent path. A universal graph VM is
not a prerequisite for any phase.

## Save, replication, and authority

| Data | Save | Replicate | Writer |
|---|---|---|---|
| Controller links and tuning | yes | yes | authority or authoring world |
| Character intent | no | input message, then authority row | current validated driver |
| Character parameters and declarations | declarations yes, transient triggers no | by declaration policy | declared authority |
| Sensor tuning and references | yes | yes | authority or authoring world |
| Sensor hit output | snapshot-safe, recomputed next step | yes when script-visible | authority physics |
| Ground and support output | snapshot-safe, recomputed next step | yes when script-visible | authority physics |
| Current and previous state | yes | yes | authority state controller |
| State definitions and animation bindings | yes | yes | authority or authoring world |
| Player roster and active possession | runtime snapshot only | yes | authority `Players` service |
| Character appearance and clothing | yes | yes | authority or authoring world |
| Bone rest pose and controls | yes | yes | author, animation, or controller by field |
| `AnimationBuffer` clips, markers, and curves | yes | asset or buffer reference | authority or authoring world |
| `PoseBuffer` samples | no | no | local animation evaluation |
| Resolved bone world frames | no | no | local presentation |
| Accessory definition | yes | yes | authority or authoring world |
| Derived pose binding cache | no | no | local resolver |
| Compiled cage and retarget caches | no | no | local compiler from saved inputs |
| Active ragdoll state | snapshot policy only | yes while active | authority physics |
| Compiled graph plan | no | no | local compiler from saved document |

The server remains authoritative. The first release may keep current
server-authoritative movement without local prediction. Prediction can later
run the same controller against an input sequence because the controller reads
explicit input and a fixed step. Reconciliation compares authoritative root,
velocity, and state. It does not compare visual bone poses.

Character replication contains no player owner. `Players` separately
replicates roster and active-possession relations. AI-only worlds can therefore
omit `Players` while retaining the complete character, animation, clothing,
state, and ragdoll stack.

## Migration

1. Add neutral `CharacterIntent`. Route current local and remote movement
   through it before changing movement behaviour.
2. Move `Character::Owner`, `PlayerCharacter`, `SetPlayerCharacter`, player
   spawning, player respawn, local camera follow, input permission, and network
   ownership into the `Players` service and client bindings. Replace the single
   character link with the roster and active possession relation. Delete the
   reverse owner field after migration tests pass.
3. Split `MakeCharacter` into a generic character factory plus the player spawn
   policy that calls it. Route bots and tests through the generic factory.
4. Add the new component serializers, class registrations, properties,
   generated bindings, and component documentation before runtime creates
   further character objects.
5. Change the default R6 root to `ShapeKind::Capsule` while retaining current
   movement. This isolates collider regressions.
6. Build the controller tree and make `Humanoid` properties forward to it.
   Read legacy humanoid movement fields into the new controller, but never
   write both forms.
7. Add the explicit state controller and preserve current walking and jumping
   through ground, air, and physics motion instances.
8. Switch movement to the kinematic capsule solver. Delete the old movement,
   ground-ray, and clip path in the same phase once parity passes.
9. Add bones and `PoseBinding`, migrate R6 limbs and tool grips, then remove
   `CharacterLimb` from live runtime use.
10. Add appearance, rigid accessories, and layered clothing on the shared rig
    and pose paths.
11. Add character animation bindings, marker traversal, sync groups, masks,
    additive layers, `PoseBuffer`, and procedural baking over existing animation
    instances and tracks.
12. Extract shared animation sampling before enabling authoritative root
    motion.
13. Add the ragdoll controller handoff after capsule and bone authority are
    stable.
14. Add graph documents, compiler, and Studio adapters only after the plain
    state and animation inputs are stable.

Each migration step leaves one active path. Temporary readers may accept the
old representation, but writers emit only the new representation.

## Work phases and acceptance gates

### Phase 0: isolate characters from players

- add neutral fixed-tick `CharacterIntent`
- make the character factory independent of `Player`
- move roster, active possession, swapping, input validation, camera routing,
  network ownership, and respawn policy into `Players` and client bindings
- keep `Player.Character` as the active-character compatibility property
- remove `Character::Owner` and the one-character `PlayerCharacter` storage
- route a headless bot and a replay through the same intent consumer

Gate: characters run in a world with no `Players` service; one player can keep
multiple characters and atomically swap among them; a bot can take over the
released character; stale client intent for the prior possession is rejected.

### Phase 1: real default R6 capsule

- derive capsule extents from named R6 dimensions
- make the root collider a capsule and its selectable bounds match
- keep six visible parts intangible
- update current character and collider comments that still call the box a
  capsule
- test spawn height, ground placement, save and load, raycasts, narrow-phase
  contacts, slopes, walls, portals, and Studio collider description headlessly

Gate: all current character, physics, replication, replay, and determinism tests
pass with a capsule root.

### Phase 2: controller instances and humanoid shim

- register `CharacterController` and motion controller classes
- register floor and climb sensor classes
- build `EnsureCharacterStack`
- make default spawns include the stack
- forward humanoid movement properties and methods
- migrate old humanoid data once
- expose equal Luau and JavaScript surfaces through the generated bindings

Gate: editing either the controller or humanoid view changes the same field,
and no legacy movement field remains live on `Humanoid`.

### Phase 3: state and full capsule movement

- add explicit sensor output, state instances, and queued state changes
- add ground, air, and physics movement algorithms
- add bounded sweep, slide, step, snap, slope, support, headroom, and external
  velocity handling
- delete old runtime movement functions after parity

Gate: headless tests cover idle, run, brake, jump, fall, land, death, disabled
states, forced physics, wall slide, corners, stairs, slopes, ceilings, moving
platforms, disappearing supports, knockback, triggers, two characters, and
portals. Sensor output must match the surface used by the controller.

### Phase 4: bones and rigid pose

- build the default R6 bone tree
- add `BoneController`, `BoneControl`, and `PoseBinding`
- drive limbs and tool grips through pose targets
- remove the old live limb pose path

Gate: tool grips follow animated attachments, invalid pose targets fail safely,
and the root remains the only simulated body.

### Phase 5: appearance, accessories, and clothing

- add `CharacterAppearance` and transactional application
- add rigid accessory attachment resolution and revision caching
- add classic clothing
- add deterministic layered-clothing cage compilation and deformation
- share rig, retarget, attachment, and bone-palette caches by revision

Gate: saved and replicated appearances apply without partial updates; rigid and
layered items follow an animated scaled rig; invalid assets preserve the last
valid appearance; no clothing path creates a default physics body.

### Phase 6: character animation binding

- add `CharacterAnimationSet` and bindings
- reuse persistent tracks under the existing animator
- drive idle, movement, jump, fall, land, and death clips from committed states
- support published animations and procedural `AnimationBuffer` clips equally
- keep action tracks independent by priority
- add exact marker, curve, loop, reverse, and seek traversal
- add sync groups, phase matching, per-bone masks, and additive layers
- keep gameplay state separate from animation state

Gate: a headless state sequence produces the expected track reuse, weights,
fades, speeds, marker occurrences, masked blends, and bone transforms in both
script languages.

### Phase 7: shared sampling, procedural baking, and root motion

- move deterministic decode, sample, and blend work out of render-only code
- add transient `PoseBuffer` scratch storage
- add seeded bounded procedural nodes and `AnimationRecorder`
- bake, simplify, validate, save, and replay generated `AnimationBuffer` clips
- produce a fixed-tick root-motion intent
- make the controller sweep that intent through collision
- keep visual interpolation separate
- throttle visual pose sampling without throttling time, events, or root motion

Gate: server, standalone client, and replay produce the same root displacement
for one canonical clip, including collision and portals; a procedural bake
round-trips within declared tolerances and fires identical markers.

### Phase 8: custom states and shared parameters

- make runtime state identity a stable name with built-in compatibility mapping
- preserve unknown custom states through save and replication
- add bounded typed `CharacterParameters`
- enforce manual and automatic transition priority, interruption, cooldown,
  residence, re-entry, and one-transition-per-tick rules

Gate: custom states and parameters round-trip, replicas refuse invalid writers,
and the same fixed input sequence chooses the same transition on every run.

### Phase 9: ragdoll handoff

- add `RagdollController`, joint-body generation, masks, and recovery policy
- transfer capsule motion to ragdoll bodies at one authority barrier
- find a safe recovery capsule before returning control
- preserve animation only on bones outside the ragdoll mask

Gate: death and scripted ragdoll enter without dual physics authority, recover
without overlap, replicate consistently, and replay deterministically.

### Phase 10: blueprints, state blueprints, and animation trees

- add the shared graph document and bounded compiler
- add state and animation node registries
- compile against typed parameters and stable custom state names
- adapt Studio's node canvas without moving ImGui into the engine
- compile on edits and cache by content revision
- add a character-construction blueprint that runs only at spawn

Gate: saved graphs survive unknown nodes, reject cycles and bad types, compile
deterministically, and produce the same result as the plain default policy.

## Test and profile plan

Every public header gets its module suite. Behaviour belongs mostly in these
existing suites or narrow successors:

- `engine.scene.characters` for player-independent construction, tree, shim,
  custom states, parameters, save, and migration
- the existing Players suites for rosters, possession swaps, respawn policy,
  `Player.Character` compatibility, and character removal
- `engine.physics.characters` for capsule movement and contacts
- `engine.scene.animation` and `engine.render.animation` for binding, markers,
  curves, sync, masks, additive layers, pose buffers, and procedural bake parity
- appearance and render suites for classic clothing, rigid attachments, layered
  cages, cache invalidation, scale, and invalid assets
- `engine.physics.characters` successors for ragdoll entry, contact, replication
  state, safe recovery, and deterministic replay
- `server.replication` for remote intent, possession revision, authority, state,
  appearance, and accessories
- `client.scene.tick` for active-character input and camera switching plus
  complete headless character and animation playback
- both script runtimes for the same properties, methods, buffers, and events

Add focused cases, not a second pile of generic smoke tests. Each error branch
needs a case: missing root, bad controller reference, disabled or unknown custom
state, stale possession revision, duplicate possession, bot handoff, marker at a
loop boundary, reverse seek, invalid mask, invalid cage, ambiguous accessory
attachment, unsafe ragdoll recovery, invalid capsule dimensions, and a replica
write.

Profile the release preset with idle, moving, and accessory-heavy crowds. Record:

- controller time per active character
- sweep count and slide iterations
- state transitions per tick
- animation tracks sampled and cache hits
- marker events, throttled visual samples, and procedural bake keys retained
- accessory rebinds versus cache hits
- cage and retarget compiles versus cache hits
- possession swaps and rejected stale intents
- active ragdoll bodies and recovery probes
- allocations per tick

Steady idle characters should do no graph compile, no accessory or clothing
scan, no track creation, and no heap allocation. Inactive characters may reduce
visual pose frequency without delaying gameplay events. Parallel work may be
considered only after a release profile identifies a crossover and all work
still joins inside the tick.

Finish with headless checks first. Ask the user whether to run live Studio
inspection for the R6 collider view, controller tree, properties, animation,
and accessory attachment. If deferred, record that visual check as a later
verification step rather than claiming it ran.

## Completion definition

This plan is complete in code when all of these are true:

- character simulation has no player dependency and runs from neutral intent
- `Players` owns multi-character rosters, active possession, swapping, client
  permission, camera routing, and player respawn policy
- human input, bots, scripts, and replay drive the same character intent path
- the default character is a real R6 capsule character
- `Humanoid` visibly forwards to a default modular controller stack
- controller state and motion are scriptable instances, not private objects
- custom state names and typed parameters save and replicate intact
- transition interruption, cooldown, re-entry, and priority are deterministic
- bones and rigid pose bindings drive limbs, tools, and accessories
- appearance, classic clothing, rigid accessories, and layered clothing share
  the rig without becoming movement authorities
- existing animation instances and tracks bind to controller state
- markers, curves, sync groups, masks, and additive layers have exact semantics
- transient pose buffers and procedurally baked animation buffers use the same
  sampling and binding path as asset clips
- visual throttling never delays markers, root motion, or gameplay events
- root motion, when enabled, passes through authoritative collision
- ragdoll performs one reversible authority handoff with safe recovery
- future state blueprints, character blueprints, and animation trees plug into
  stable document and compile cuts
- legacy runtime paths and duplicate fields have been removed
- save, replication, replay, determinism, Luau, JavaScript, server, client, and
  headless end-to-end checks pass
