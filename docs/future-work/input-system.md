# Input system plan

## Status

This document describes future work. It extends the current SDL translator,
world input resources, script services, client intent path, Studio keybinds,
replication input channel, and recording tools. It does not claim that named
game actions, touch controls, input profiles, local multiplayer, pen input, or
XR input exist today.

The first production target is deliberately smaller than the complete device
matrix: keyboard, mouse, and mapped gamepads feeding named and typed actions,
with deterministic fixed-tick samples, rebinding, context stacks, accessibility
transforms, character intent, scripting parity, Studio authoring, and useful
latency diagnostics. Touch follows on the same action path. Pen and XR add new
physical control kinds only after the action model has proved it can carry
them.

## Product goal

An author declares what a game can do, binds those actions to controls, and
reads the same named values from native systems, Luau, JavaScript, local bots,
recordings, and client intent builders. Game code does not ask whether W or the
left stick is held. It asks for `Move`, `Jump`, `Look`, or another stable action
name.

The system must provide:

- one normalized physical event path for keyboard, mouse, gamepad, touch, pen,
  and future XR adapters;
- stable runtime device identities with explicit reconnect and hotplug rules;
- named actions with boolean, scalar, vector, and pose value kinds;
- chords, composites, sequences, interactions, deadzones, curves, and bounded
  processors;
- per-local-player context stacks with explicit priority and consumption;
- user rebinding, conflict reporting, accessibility transforms, and local
  profiles;
- correct text input, input method composition, GUI focus, pointer capture, and
  window focus loss;
- local multiplayer with deterministic device assignment;
- a fixed-tick sample suitable for character intent, networking, prediction,
  and replay;
- a script surface shared by Luau and JavaScript;
- project-owned action map assets separate from machine-owned user profiles;
- a Studio action editor, live monitor, event trace, conflict view, and latency
  counters;
- bounded queues and hostile-input limits at every device and asset boundary;
- headless builds that understand input intent without linking SDL or a window.

Input translates local human activity into game intent. It does not own
characters, players, cameras, GUI widgets, networking authority, or Studio
commands.

## Current foundation

### Physical translation

`mono.engine/input` is an L12 client module. `input::Translator` receives events
from a caller-owned SDL event pump and currently produces:

- `scene::InputState` for keyboard levels and edges, three mouse buttons,
  pointer position and delta, wheel delta, focus, last input source, cursor
  behaviour, and cursor visibility;
- `scene::ControllerState` for eight gamepad slots, standardized buttons, six
  axes, connection edges, and tick-latched button presses;
- one frame of composed UTF-8 text retained by the translator rather than by a
  serializable component;
- focus-loss releases so a key released outside the window cannot remain held;
- normalized mapped gamepad and raw joystick input with a fixed 0.12 deadzone.

SDL instance ids stay inside the adapter. Worlds currently see first-free
`Gamepad1` through `Gamepad8` slots. A disconnect clears a slot and a reconnect
may occupy another one. That is a usable script-facing slot scheme, but it is
not a persistent device identity and is not enough for saved user profiles or
local multiplayer.

The program owns the event loop. This remains required because several window
systems require event pumping on the thread that created the window. No future
input module API may add a hidden polling thread.

### Shared world state

`scene::InputState` is a shared, trivially copyable resource with a pinned
56-byte layout. Its current fields include tick-latched key and mouse presses.
`scene::ControllerState` is separate so controller support did not rewrite that
established layout.

The pinned resource remains the Roblox-compatible raw input view. Dynamic
actions, arbitrary device records, text, and profile data must not be appended
to it. They require separate resources with their own versioning and ownership.

`scene::ReadMoveIntent` currently maps W, A, S, D, arrow keys, the active left
stick, Space, and the active gamepad A button into camera-relative movement and
jump. `scene::ReadAimIntent` maps the live camera plus the primary mouse button
or right trigger into an aim ray and fire edge. Both hosts reuse these helpers,
but the control names are still hard-coded gameplay policy inside the raw input
reader.

### Client, replication, and recording

`client::Client` owns the event pump and copies translator state into each
eligible world. `Client::WriteInput` preserves script-written cursor settings
and tick latches across the frame copy. It sends a tick-numbered `MoveInput`
containing a direction and jump edge. The server decodes finite values,
normalizes movement itself, checks which player sent the input, and applies it
to the current character.

The replication layer already treats client input as copied opaque bytes tied
to a server tick. Prediction and rollback retain input history by tick. World
recording captures authoritative barriers. The future action sampler should
feed these seams rather than adding a second network transport or a raw SDL
recording beside them.

### Script input

`UserInputService` is shared through `scene::InputState` and
`scene::ControllerState`. Luau and JavaScript receive the same neutral
`InputReport` facts through their own adapters. Current methods cover keyboard,
mouse, gamepad polling, last input type, focus, cursor behaviour, and several
signals.

`ContextActionService` already has a VM-local priority stack. Bindings are named,
replace by name, sort stably by priority, and let a handler return `Pass` or
sink lower handlers. It currently accepts key codes, not typed named action
values. Its callable storage cannot be serialized or placed on the world.

GUI processing already distinguishes pointer capture from keyboard focus. A
focused `TextBox` marks keyboard reports as processed, and the client starts
SDL text input only while a box is focused. This distinction must survive the
action system. A physical key event and committed text are different facts.

### Studio commands

Studio has a separate `studio::Keybinds` table. Commands use stable string ids,
single keyboard chords, exact modifier matching, scopes, saved user overrides,
conflict detection, and a guard that prevents the rebind press from firing the
new command. Studio Play uses its own `input::Translator` adapter for the
running world.

Studio command bindings are not game actions. They remain process-owned editor
commands even after both paths share lower normalized platform events.

`client::Actions` is another program-owned diagnostic command table for panels,
settings, wireframe, snapshots, and quit. It is likewise not a game action map.

## Non-negotiable design rules

1. Platform event pumping stays on the host thread that owns the window.
2. SDL types, instance ids, window pointers, and device handles do not cross
   into shared world state, saves, scripts, recordings, or network messages.
3. `scene::InputState` keeps its pinned layout and raw compatibility meaning.
   Named action state lives separately.
4. A game action is identified by a stable name in assets, profiles, scripts,
   recordings, and diagnostics. Dense runtime handles never leave one compiled
   action map.
5. Text input is never reconstructed from key presses. Committed UTF-8,
   composition text, and physical keys remain separate paths.
6. Authoritative simulation reads one fixed-tick action sample. It does not
   inspect frame edges or wall-clock event arrival directly.
7. A tap between ticks is retained. Multiple transitions in one tick are
   counted or queued within a bound rather than collapsed accidentally.
8. Equal-priority contexts, bindings, and processors use stable authored order
   as their tie-break. Hash order and device discovery order do not decide
   gameplay.
9. Every queue, device set, action map, context stack, binding list, sequence,
   touch set, and event trace has a named upper bound and visible overflow
   counter.
10. User profiles are local preferences. Game action assets are project
    content. Neither silently overwrites the other.
11. The server validates a client's current player possession and the semantic
    intent range. It never trusts a client-supplied character handle by itself.
12. Studio command consumption and game action consumption are separate. A
    game binding cannot suppress Save, and an editor command cannot appear in
    `UserInputService`.
13. Focus loss releases every level, cancels every pointer or touch capture,
    ends composition, and produces at most one visible cancellation path.
14. Accessibility transforms operate on declared actions or bindings. They do
    not rewrite committed text, operating-system shortcuts, or network
    authority.
15. Device output such as rumble and adaptive trigger control is a separate
    bounded output service. It is not encoded as fake input.
16. Headless builds decode and validate game intent but do not link the input
    device module.

## Architecture and ownership

The existing module split is mostly correct. The first implementation should
extend it rather than add a second input stack.

| Owner | Responsibility |
|---|---|
| `core` | stable names, bounded byte formats, vectors, transforms, and monotonic timestamps |
| `scene` | raw compatibility resources, fixed-tick action samples, local-player input assignment facts, and neutral character intent |
| `input` | platform normalization, device registry, action-map compilation, frame evaluation, fixed-tick sampling, profile application, and diagnostics |
| `gui` | pointer routing, keyboard focus, text editing, and processed-state evidence |
| `script` | raw compatibility services, named action services, signals, and VM-local callback stacks |
| `game` | semantic client intent codecs and server-side validation helpers |
| `replication` | tick-numbered opaque intent delivery, prediction history, ownership gates, and rollback input retention |
| `client` | event pumping, window focus, device opening, local-player routing, action sampling, camera-relative intent building, and submission |
| `world` | recording and replay of authoritative tick inputs and barriers |
| Studio | project action editor, user profile editor, editor commands, Play routing, diagnostics, and test injection |

The exact target graph must be checked before code lands. The intended
direction is unchanged: client-only input writes shared plain values that
scripts and gameplay can read. Shared modules do not reach upward into SDL.

### Separate resources

Do not turn `InputState` into a variant container. Add separate, purpose-shaped
resources:

| Resource | Scope | Purpose |
|---|---|---|
| `InputState` | world and compatibility | existing keyboard, mouse, focus, and cursor state |
| `ControllerState` | world and compatibility | existing eight script-facing gamepad slots |
| `ActionFrameState` | local player and frame | values, frame deltas, and processed flags for presentation and scripts |
| `ActionTickState` | local player and tick | deterministic levels, transitions, counters, and accumulated deltas consumed by simulation |
| `InputAssignmentState` | client world | local player to device assignment and active action-map revision |
| `InputDiagnostics` | client-local | bounded counters and timestamps, never saved or replicated |

If several local players share one world, one singleton action resource is not
enough. Store action state in local-player keyed rows or in a bounded client
resource whose entries name local player slots. Do not copy it onto character
entities. The Players service decides which character each local player drives.

### One low event feed, distinct consumers

The platform adapter emits normalized `PhysicalEvent` values into an owner
thread buffer. The same immutable event may be observed by:

1. the Studio command router, when the editor owns keyboard focus;
2. the active Play client, when its viewport owns game focus;
3. GUI and text routing inside that game world;
4. the game action evaluator;
5. the raw compatibility translator and script signals;
6. diagnostics and optional recording taps.

Observation is not shared consumption. Each consumer receives an explicit
routing decision. Studio command scope cannot be changed by a game's context
stack, and a Studio keybind does not become a game-processed input report.

## Physical device model

### Runtime identity

Introduce a generation-checked `DeviceHandle` valid only inside one input
runtime. It contains a dense slot and generation, not an SDL instance id. A
stale handle resolves to no device after disconnect.

Each live `InputDevice` record contains:

| Field | Meaning |
|---|---|
| `Handle` | generation-checked runtime identity |
| `Kind` | keyboard, mouse, gamepad, touch, pen, or XR controller |
| `Capabilities` | bounded bitset of available controls and output features |
| `DisplayName` | sanitized local display text |
| `MappingGuid` | backend mapping identity where available |
| `VendorId` and `ProductId` | optional hardware match fields |
| `ConnectionOrdinal` | monotonic local diagnostic number, never persisted |
| `LocalFingerprint` | optional salted machine-local reconnect key |
| `Battery` | optional bounded presentation reading |
| `PlayerSlot` | current local assignment or unassigned |
| `Connected` | current level with frame and tick connection edges |

`LocalFingerprint` must not contain a raw device path, serial number, Bluetooth
address, or other identifier in logs, saves, analytics, or network traffic. A
backend may hash stable local fields with an installation salt. Where a
platform exposes no stable identity, reconnect matching is best effort and the
profile falls back to kind, mapping GUID, vendor, product, then assignment
order.

### Device discovery and reconnect

Device enumeration runs before the first frame so already-connected gamepads
do not depend on a later hotplug event. Add and remove events are idempotent.
Duplicate backend notifications update one live record rather than creating a
second device.

Reconnect rules are deterministic:

1. exact local fingerprint reclaims its previous assignment;
2. an unambiguous mapping GUID plus vendor and product match reclaims it;
3. a user-confirmed profile match reclaims it;
4. otherwise the device remains unassigned until join or assignment policy
   chooses it.

A disconnect synthesizes releases, clears axes and pointer captures, records
one connection edge, and preserves the local player's assignment reservation
for a bounded grace period. Gameplay receives neutral values immediately. A
stuck level is never retained while waiting for reconnect.

### Control paths

Bindings name semantic physical controls with stable strings, for example:

```text
keyboard/space
keyboard/scancode/w
mouse/button/primary
mouse/delta
mouse/wheel/y
gamepad/button/south
gamepad/stick/left
gamepad/trigger/right
touch/contact/primary/position
pen/tip/pressure
xr/hand/left/trigger
xr/hand/right/pose
```

Keyboard bindings must declare whether they mean a logical key or a physical
scancode. Logical keys follow the active layout and are suitable for text-like
shortcuts. Physical keys preserve location and are suitable for movement. The
display layer shows the current localized legend without changing the saved
control path.

Control paths are parsed and interned when a map is compiled. Per-event
evaluation compares dense handles. A malformed or unsupported path is a
validation message, not a binding that silently never fires.

### Normalized event

`PhysicalEvent` is a copied plain value. It carries:

- monotonic host timestamp for latency diagnostics;
- per-runtime sequence number for total ordering;
- device handle;
- control handle or unresolved stable path during diagnostics;
- event kind such as connect, disconnect, press, release, value, pointer,
  contact, text commit, composition, focus, or cancellation;
- typed value and previous value where meaningful;
- pointer or contact id with generation;
- window or surface route as a host-local handle;
- backend flags such as repeat or synthesized release.

The timestamp does not decide authoritative simulation. Sequence order and the
fixed-tick sampler do. Backend timestamps may use different epochs, so the
adapter converts them to the engine's monotonic clock at ingestion.

### Keyboard and mouse

Keyboard support expands the current key table to every control the backend and
script enum promise. Unmapped controls remain visible in the input debug log.
Media keys and operating-system reserved shortcuts are exposed only when the
platform delivers them and the application is permitted to consume them.

Mouse support adds side buttons, horizontal wheel, raw versus accelerated
motion capability, pointer capture state, and high-resolution wheel deltas.
Action bindings consume normalized values. `UserInputService` keeps its Roblox
compatible three-button surface until compatible enum members are deliberately
added.

Relative mouse mode uses motion deltas and never derives them from a recentered
position. Focus loss exits capture through the existing window owner and emits
one cancellation sequence.

### Gamepads and joysticks

Mapped gamepads expose semantic buttons and axes. Raw joysticks expose bounded
numbered controls only through explicit raw control paths. A raw button ordinal
must not masquerade as a semantic south button.

Deadzone calibration moves out of the translator's one fixed constant and into
binding processors or a device calibration profile. The raw normalized state
keeps the full bounded range. Default gamepad bindings may apply a radial 0.12
stick deadzone and a 0.05 trigger threshold, but these are asset defaults that
Studio displays and tests.

Gamepad output is separate:

- low and high frequency rumble;
- bounded duration and gain;
- optional trigger or light controls behind capability flags;
- automatic cancellation on disconnect, focus loss, world exit, or owner
  destruction;
- per-device and global command limits.

### Touch

Touch is a set of contacts, not a mouse compatibility event. Each contact has a
generation-checked id, start position, current position, delta, pressure where
available, phase, surface, and capture owner. The runtime bounds active contacts
per surface and reports overflow.

The first touch delivery includes an authored `TouchLayout` asset whose virtual
sticks, buttons, swipe regions, and free look regions bind directly to named
actions. Safe areas, orientation, screen density, handedness, and UI scale are
inputs to layout. They do not alter gameplay action names.

Gesture recognition is engine-owned and deterministic over ordered contact
samples. The initial set is tap, hold, drag, swipe, pinch, and rotate. Every
gesture has declared distance and tick thresholds. Platform gesture recognizers
may be used only as presentation hints, not as authoritative gameplay events.

`ContextActionService` can create a touch button only after touch layout
instances, labels, icons, position, and lifecycle exist. Until then the current
accepted-but-ignored flag remains documented rather than producing an invisible
button.

### Pen

Pen input reuses pointer position and adds tip, eraser, pressure, tilt, twist,
barrel buttons, hover, and device capability flags. Pressure and tilt are
scalar or vector controls suitable for actions and Studio tools.

Pen events do not synthesize mouse input inside the engine. If a platform also
sends a compatibility mouse event, the adapter marks the related sequence so a
consumer can deduplicate it. Studio drawing tools may prefer pen data while a
game continues to use primary pointer actions.

### XR seam

XR support is a future adapter, not part of the first runtime. The action value
model reserves:

- boolean and scalar hand controls;
- 2D thumbsticks and touchpads;
- 3D linear or angular values;
- poses with position, orientation, validity, and tracking confidence;
- device role paths for head, left hand, right hand, and optional trackers.

The XR runtime owns late-latched presentation poses. Fixed-tick gameplay reads a
sampled pose with its tick and validity. Rendering may use a newer local pose
without rewriting authoritative action history. Haptics remain an output path.

## Named and typed actions

### Action declaration

An `InputActionMap` is a versioned project asset. It contains stable action
names, contexts, bindings, processors, interaction rules, default device
schemes, and display metadata. Runtime ids are compiled per asset revision.

Each action declares:

| Field | Meaning |
|---|---|
| `Name` | stable project-wide name such as `Move` or `Interact` |
| `ValueKind` | boolean, scalar, vector2, vector3, or pose |
| `Aggregation` | highest magnitude, latest, sum and clamp, or ordered composite |
| `Phases` | started, performed, changed, cancelled, and completed as applicable |
| `SimulationUse` | presentation only, gameplay sampled, or both |
| `ConsumeMode` | never, on performed, or while active |
| `DefaultValue` | typed neutral value |
| `DisplayName` | localizable text key rather than stable identity |
| `Category` | authoring and rebinding group |
| `Rebindable` | whether user profiles may override it |
| `NetworkPolicy` | local only or included in a named intent schema |

Action names are case-sensitive canonical strings at validation time and
`core::Name` values at runtime. Duplicate names are refused. Renames use an
explicit alias migration table; silently treating a new spelling as the old
action would make user profiles and recordings lie.

### Value kinds

- `Boolean` represents a button or thresholded interaction.
- `Scalar` represents triggers, throttle, zoom, and one-axis movement.
- `Vector2` represents movement, look, scroll, and pointer position.
- `Vector3` represents spatial controls and future motion sensors.
- `Pose` represents tracked position and orientation with validity flags.

Text is not an action value. Arbitrary strings create unbounded per-frame state
and confuse key activity with text composition. Text stays in the focused text
path.

An action never changes value kind through a user profile. A project asset
revision may migrate it only with an explicit compatibility rule and tests.

### Binding sources

A binding may use one of these bounded source shapes:

- one physical control;
- a 1D positive and negative composite;
- a 2D up, down, left, and right composite;
- a 3D six-direction composite;
- a chord whose modifier controls must be active;
- an ordered sequence of press and release steps;
- one action feeding another higher-level action;
- a touch virtual control;
- a script or test-injected virtual device control.

Composites normalize by declared policy. A digital 2D movement composite uses
unit-circle normalization by default so diagonals are not faster. Opposing
directions cancel unless the asset chooses latest-input priority.

Action-to-action bindings must form an acyclic graph. Compilation reports the
cycle with names. Evaluation order is topological and stable.

### Processors

Processors are a closed set of small, typed, allocation-free transforms:

- axial or radial deadzone;
- normalize;
- invert axes;
- scale and sensitivity;
- clamp;
- exponent or authored piecewise response curve;
- digital threshold with hysteresis;
- axis swap;
- screen-density and viewport normalization for pointers;
- smoothing for presentation-only values.

Each processor declares accepted input and output kinds. Invalid chains fail
asset compilation. Processor count per binding is bounded. Gameplay smoothing
uses fixed-tick coefficients and saved state. Frame-time smoothing is permitted
only on presentation-only actions and may not feed network intent.

NaN and infinity are refused at the first adapter or virtual-device boundary.
Every processor clamps its declared output. A response curve has a bounded key
count and monotonic input domain.

### Interactions

Interactions decide action phase from processed values:

- press;
- release;
- tap;
- hold;
- slow press;
- multi-tap;
- repeat;
- threshold crossing with hysteresis;
- value change.

Gameplay interaction deadlines are stored as tick counts. Presentation-only UI
interactions may use monotonic durations. A project asset must say which clock
applies. The same interaction cannot switch clocks when replay begins.

Multi-tap and sequences retain a bounded number of steps. A timeout resets the
state without a late callback. Autorepeat from the operating system never
counts as another physical press.

### Aggregation

Several bindings may feed one action. Aggregation happens after per-binding
processors and interactions:

- boolean actions OR active performed bindings unless the action declares
  latest-input priority;
- scalar and vector actions use their declared aggregation policy;
- equal magnitudes choose stable binding order;
- inactive devices contribute neutral values;
- a device disconnect cancels its binding contribution before aggregation.

The diagnostic view shows every contribution and the winning binding. It must
be possible to explain why an action has its current value.

## Context stacks and consumption

### Context declaration

Contexts are stable named rows in an action map. Common examples are
`Gameplay`, `Vehicle`, `Menu`, `Chat`, `Inventory`, `Spectator`, and
`PhotoMode`. A local player has a stack of active context entries.

Each entry contains:

- context name;
- priority;
- stable push sequence;
- optional owner token;
- enabled action groups;
- modal or overlay mode;
- consumption policy;
- activation tick and revision.

Higher priority evaluates first. Equal priority uses push sequence. Context
changes requested during a callback or system pass commit at a named barrier
and affect the next frame or tick. Evaluation never mutates the stack it is
walking.

Owner tokens allow a menu, tool, vehicle seat, or script lifetime to remove its
own context. Destroying an owner releases all of its entries. A leaked context
must be visible in diagnostics with its owner name and age.

### Consumption

Consumption applies to physical contributions, not to arbitrary action names
after evaluation. A higher context may consume a key, pointer, touch region, or
gamepad control so lower contexts do not also use it. It may also leave the
control visible and consume only when its action performs.

The order is:

1. host focus and surface routing;
2. text composition and focused GUI processing;
3. per-local-player context evaluation;
4. raw compatibility signals marked with processed evidence;
5. fixed-tick action sample;
6. gameplay intent builders.

GUI processing does not destroy the raw event. `UserInputService.InputBegan`
still fires with `gameProcessedEvent = true`. Named gameplay actions exclude
the consumed contribution unless their binding explicitly opts into processed
input for a narrow debug or accessibility case.

### `ContextActionService`

Keep the existing VM-local callback stack for Roblox compatibility. It remains
a transient script event layer. Its `Pass` or sink result orders handlers in
that VM and does not silently rewrite project action-map state.

Extend it in compatible steps:

- accept supported `UserInputType` controls as well as key codes;
- create real touch buttons once touch layouts exist;
- expose action title, description, image, and position only when those values
  have a visible runtime consumer;
- make keyboard, pointer, and controller processed state agree with named
  context consumption;
- preserve stable priority and bind order across both script runtimes.

Add a separate `InputActionService` for project named actions. Do not overload
`ContextActionService:BindAction` with an asset reference and change its
callback semantics.

## Rebinding and conflicts

### Rebinding session

A rebind is an explicit state machine:

1. choose a local player, action, and binding slot;
2. enter capture with a timeout and allowed device kinds;
3. suppress the capture event from game and Studio command dispatch;
4. collect a valid control or composite;
5. normalize modifiers and control paths;
6. detect conflicts in relevant contexts and schemes;
7. apply replace, swap, keep-both, or cancel policy;
8. validate the whole map plus profile overlay;
9. commit atomically and persist once.

Escape may cancel only if it is not itself an allowed captured control, or the
UI provides a separate cancel button. A modifier alone is not a completed chord
unless the binding explicitly permits modifier-only controls.

The press that completes capture cannot perform the new action in the same
frame or tick. Studio already has this rule for command rebinds and the game
profile path must keep it.

### Conflict model

A conflict is not merely two equal control paths. Compare:

- device scheme and local-player assignment;
- contexts that can be active together;
- control path and modifiers;
- interaction phase and consumption mode;
- composite parts;
- user accessibility transforms;
- Studio focus route where the editor hosts Play.

Hard conflicts mean two actions in simultaneously active consuming contexts
would perform from one control. Soft conflicts mean contexts are normally
exclusive or both actions are non-consuming. Studio reports both and requires
an explicit choice only for hard conflicts.

Some duplicates are intentional, such as confirm on Enter and gamepad south.
Conflict identity is per physical control within a device scheme, not across
unrelated schemes.

## Accessibility transforms

Accessibility is a first-class profile overlay, not a separate input path.
Initial transforms include:

- hold versus toggle conversion for declared actions;
- sticky modifier chords;
- configurable hold and multi-tap windows;
- repeat delay and rate for UI actions;
- stick deadzone, sensitivity, inversion, and response curve;
- mouse sensitivity and axis inversion;
- one-handed binding schemes;
- simultaneous-press tolerance for chords;
- optional digital alternatives for analog actions;
- vibration gain and disable controls;
- touch target scale, spacing, opacity, and handedness.

An action asset declares which transforms are safe. A toggle transform is not
automatically valid for `Fire`, and a slow hold is not automatically valid for
a time-critical rhythm action. Unsafe requests are refused with a reason.

Accessibility state participates in the fixed-tick sampler and action recording
through the resolved action value. Replays do not need the original user's
profile to reproduce a run.

## Local multiplayer and device assignment

### Local player slots

The client owns a bounded set of `LocalInputSlot` records. Each names:

- a local Players service slot or local player entity;
- assigned device handles or device match rules;
- action map and user profile revisions;
- active contexts;
- frame and tick action state;
- join state and last active device.

Keyboard and mouse are a paired virtual device by default. A project may allow
split keyboard regions or shared devices, but that is explicit asset policy.
Gamepads are exclusive to one local slot by default.

### Join and leave

An unassigned device may perform a project-declared `Join` action. Assignment
commits at the next input barrier. The same press does not also perform the
joined player's confirm or attack action.

Leaving clears action levels, releases contexts owned by that slot, stops
device output, and informs the Players service. It does not destroy a character
directly. The Players service decides whether that character idles, transfers
to a bot, or is removed.

### Character routing

The input system produces neutral action state. A client intent builder maps
the current local player's named actions to `CharacterIntent`, for example:

| Action | Character intent field |
|---|---|
| `Move` | desired planar movement vector |
| `Look` | desired facing or camera delta, according to game policy |
| `Jump` | bounded press count or edge |
| `Sprint` | held level |
| `PrimaryAction` | named gameplay action request or fire edge |
| `Interact` | bounded interaction request |

The Players service validates which character is actively possessed by that
local player. Switching possession is atomic. The old character receives
neutral intent in the same operation, and the next tick routes input to the new
character. Character code does not read `InputState`, a device, or a Player
reference.

Bots, AI, scripts, and replays can write the same neutral intent without an
input device. They do not create fake gamepads or fake `InputState` rows.

## Fixed-tick sampling

### Frame accumulation

Physical events arrive during host frames. For each local slot, the evaluator
maintains:

- current action levels;
- ordered start, perform, cancel, and complete transitions;
- saturated press and release counts;
- accumulated relative deltas;
- latest absolute values;
- interaction state with tick deadlines;
- source device and binding for diagnostics.

Frame state drives UI and script frame signals. It is not the simulation
sample.

### Tick sample

At the fixed-tick input phase, the owner produces one immutable
`ActionTickState` per local slot:

- levels sampled at the barrier;
- accumulated relative values since the previous sample;
- bounded transition counts and ordered event summaries;
- action-map, profile, context, and assignment revisions;
- simulation tick;
- focus and cancellation flags;
- source masks for diagnostics, not authority.

After the consumer accepts the sample, accumulated transitions and deltas are
cleared. A refused network send retains required edge counts until accepted or
until a named maximum age expires and records a drop. Continuous levels are
sent every tick so a lost release cannot leave movement active.

### Multiple events between ticks

A bit is sufficient for one jump request, but not for double-tap, rhythm input,
or press then release then press within one tick. The generic sample therefore
stores saturated transition counts and a small bounded ordered event ring for
actions that declare sequence sensitivity.

The action asset opts into ordered transition retention. Ordinary movement
actions pay only for level and edge counts. Overflow saturates, sets an
overflow bit, and increments a diagnostic counter. It never allocates.

### Determinism

Deterministic input means the same tick samples produce the same simulation. It
does not mean two operating systems deliver physical events at identical times.

Rules:

- one owner thread assigns total event sequence numbers;
- gameplay deadlines use ticks;
- frame rate does not decide tap or hold classification;
- worker jobs may evaluate independent local slots, but all join before sample
  publication;
- device discovery order does not break binding ties;
- maps, profiles, and contexts change only at named barriers;
- replay injects recorded action or intent samples at their original ticks;
- presentation-only pointer and pose updates may run more often without
  changing gameplay samples.

## Networking, prediction, and replay

### Network payloads

Do not send the whole dynamic action map or arbitrary action dictionary every
tick. Define versioned semantic intent schemas for game systems. The character
schema is one example. A vehicle or UI request can define another.

Each payload includes:

- schema tag and version;
- server tick being answered;
- possession or authority revision when required;
- quantized bounded values;
- saturated edge counts where more than one matters;
- action-map compatibility signature when the server requires it.

The server rejects unknown versions, wrong lengths, non-finite decoded values,
out-of-range values, stale authority revisions, impossible transition counts,
and requests for a character the sender does not possess. It normalizes and
clamps again after decoding.

### Prediction

Prediction retains the semantic tick input that drove predicted state, not SDL
events and not pointers into action state. Reconciliation rewinds to the
authoritative tick and reapplies the copied samples in tick order.

Changing a user binding does not alter already-retained prediction samples.
Changing an action map revision flushes or versions incompatible predicted
history at a declared barrier.

### Recording levels

Support three deliberately different recordings:

| Recording | Purpose | Contents |
|---|---|---|
| authoritative replay | reproduce gameplay | accepted semantic intent by tick plus world barriers |
| action trace | debug maps and interactions | named typed action samples, contexts, revisions, and source ids scrubbed to local aliases |
| physical trace | input subsystem tests | normalized physical events and device capability fixtures, never raw OS handles |

Shipping replay uses the first. The latter two are diagnostic formats with
strict size limits and explicit user action to record. A replay never needs the
original keyboard layout, gamepad GUID, or accessibility profile.

## Script API

### Preserve raw compatibility

`UserInputService` keeps its current raw input role. Planned additions should be
added only when the runtime can produce them honestly:

- complete mouse buttons and supported key names;
- touch and pen signals;
- composition and text-input lifecycle where compatible;
- device capability queries using stable script-facing slots;
- pointer capture state;
- accurate processed flags from GUI and action contexts;
- connected and disconnected device signals;
- gamepad mapping and vibration requests behind capability checks.

Raw device methods answer local presentation state. They do not run on a
headless server and do not imply network authority.

### `InputActionService`

Add one neutral service surface used by both Luau and JavaScript. Initial shape:

```text
GetActionValue(name, localPlayer?) -> typed value
GetActionPhase(name, localPlayer?) -> named phase
IsActionActive(name, localPlayer?) -> boolean
GetActiveContexts(localPlayer?) -> ordered records
PushContext(name, priority?, owner?) -> context token
PopContext(token) -> boolean
EnableContext(name, enabled) -> boolean
GetBindingDisplay(name, scheme?) -> localized display record
BeginRebind(name, bindingIndex?, options?) -> rebind session
ActionStarted(name) -> signal
ActionChanged(name) -> signal
ActionPerformed(name) -> signal
ActionCancelled(name) -> signal
DeviceAssigned(localPlayer, device) -> signal
DeviceUnassigned(localPlayer, device) -> signal
```

Values cross through an explicit tagged report type. Do not return a generic
table whose shape depends on `ValueKind` without a discriminator. Both script
runtimes build their own wrapper from one shared fact, following the existing
`InputReport` pattern.

Server scripts may inspect accepted semantic character or gameplay intent
through the owning gameplay service. They do not see a client's local
`InputActionService` state as if it were authoritative.

### Virtual devices and tests

Scripts do not receive an unrestricted API to impersonate hardware. Test and
trusted tooling may create a bounded virtual input device through a permissioned
host API. Ordinary game scripts can drive characters or game systems through
their semantic APIs instead.

Virtual device events use the same normalization, map, context, and sampling
path. This is the basis for headless input tests and Studio's event injector.

## Action-map asset and user profile

### Project asset

`InputActionMap` is project content. It is saved with the game, published
through the asset pipeline, content-addressed, validated on import, and
available to every relevant client. It contains:

- schema version;
- stable map name and action declarations;
- context declarations;
- default device schemes and bindings;
- processors and interactions;
- touch layouts;
- local multiplayer assignment policy;
- rebinding permissions and display metadata;
- explicit migration aliases;
- deterministic compiled signature.

The source form is readable and diffable. Runtime may use a compiled binary
with dense ids and prevalidated processor plans. Both carry the same canonical
signature.

### User profile

`InputProfile` is local machine or account preference data. It contains only
overrides:

- action map canonical signature or compatible map name and version;
- per-device-match binding overrides;
- accessibility transforms;
- calibration values;
- local-player assignment preferences;
- touch layout adjustments;
- presentation options such as prompt scheme.

Profiles save stable names and control paths, never enum ordinals, dense action
ids, SDL ids, device handles, callbacks, or raw device paths. Writes are atomic.
Unknown future fields are preserved when practical or skipped with a visible
compatibility warning.

Game saves may name the active project action map. They do not embed the user's
private profile. Universe replication may publish which map revision is
required. It does not replicate personal bindings to other players.

### Compatibility

Profile overlay resolution is:

1. exact map signature;
2. compatible map version with declared action aliases;
3. stable action and binding slot match;
4. default binding for anything unresolved.

An orphaned override stays visible in the profile editor with a reason. It does
not attach itself to the next action that happens to reuse a dense id.

## Studio tools

### Input Actions editor

Add a project editor with an information-dense table and focused detail pane.
It supports:

- action creation, rename with alias, type, phase, and network policy;
- context creation, ordering, compatibility, and default activation;
- device scheme and binding editing;
- composites, chords, sequences, processors, and interactions;
- touch layout editing over safe-area and orientation previews;
- validation with direct links to the invalid row;
- conflict matrix by context and device scheme;
- profile overlay preview without modifying project defaults;
- canonical source diff and compiled signature;
- undo and redo through Studio's command log;
- import and export of action maps and local profiles.

Do not build the first version as a general node graph. Actions and bindings
are ordered records, and a table exposes conflicts and types better. A graph
view may later visualize action-to-action dependencies if real maps become hard
to read.

### Live monitor

The monitor shows, per local player:

- connected and assigned devices;
- active contexts in evaluation order;
- every action's type, value, phase, source binding, and consumed state;
- raw versus processed axis values;
- tick latch counts and overflow flags;
- action map and profile revisions;
- last event, sample, submit, server apply, and visible-response timestamps;
- focus, pointer capture, text composition, and GUI processed state.

Rows update from revision counters or new event sequence numbers. A hidden
monitor returns before formatting rows. It does not repaint a graph continuously
when nothing changed.

### Event trace and injector

A bounded ring records normalized events, routing decisions, context winners,
action transitions, tick samples, and semantic intent encoding. Device names
are local aliases. The view supports filters by local player, device, action,
context, tick, and consumed state.

The injector creates permissioned virtual-device events. It can replay a saved
physical fixture, step one event, hold a control, move an axis, simulate focus
loss, disconnect a device, and advance fixed ticks. It cannot bypass the normal
translator or write `ActionTickState` directly in an end-to-end test.

### Studio command editor remains separate

The existing Keybinds page continues to edit editor commands. The Input Actions
editor edits game assets and user game profiles. They may share:

- normalized key display names;
- chord capture widgets;
- conflict presentation helpers;
- low platform event fixtures;
- profile-safe atomic file writing helpers.

They do not share command enums, context stacks, consumption, save files, or
dispatch. The UI labels each surface clearly so rebinding `Jump` cannot be
mistaken for rebinding Studio `Play`.

## Focus, text, and surface routing

### Window focus

On focus loss:

- synthesize releases for all held controls routed to that surface;
- cancel pointer capture and relative mode through the window owner;
- cancel touch and pen contacts;
- cancel active action interactions and contexts that require focus;
- stop text input and composition;
- neutralize the next fixed-tick sample;
- retain a focus-loss marker for raw script signals and diagnostics;
- stop rumble and other device output when policy requires it.

Focus gain does not restore old levels. Devices must speak again.

### Text focus

`gui::FocusedTextBox` remains the world authority for game text focus. The host
starts text input for the surface owning that world. Committed UTF-8 reaches the
focused box. Composition text, selection, candidate position, and composition
range require a new transient GUI edit state, not fields on `InputState`.

While text has focus:

- committed text goes only to the focused editor path;
- physical keys still produce raw reports marked processed;
- gameplay action bindings are consumed unless explicitly allowed;
- text navigation keys are handled by the text editor before game contexts;
- losing or destroying the focused object ends composition safely.

Studio text editors use Studio or ImGui focus, not the game world's
`GuiServiceState`. A Play viewport and a Studio script editor cannot both own
the same keyboard event.

### Multiple windows and surfaces

Every physical pointer, text, and focus event carries a host-local surface route.
The host maps that route to Studio, one game viewport, or another client window.
World resources do not store window handles.

Keyboard routing follows the focused surface. Gamepad routing follows local
assignment and may continue while a game window has focus according to project
policy. Background input is disabled by default and requires an explicit host
setting and platform capability.

## Failure handling and hostile limits

Initial limits must be constants or project settings with conservative hard
ceilings:

| Limit | Initial policy |
|---|---|
| live devices | 32 per process |
| script-facing gamepads | existing 8 slots |
| local player slots | 8 per client |
| active touch contacts | 16 per surface |
| action maps active per local slot | 8 layers |
| actions per compiled map | 1,024 |
| contexts per map | 128 |
| active context entries | 64 per local slot |
| bindings per action | 32 |
| processors per binding | 8 |
| sequence steps | 16 |
| ordered tick transitions | 32 per opted-in action |
| event buffer | fixed capacity sized from measured burst tests |
| diagnostic trace | byte and event caps, disabled by default |
| rebind sessions | one per local slot |

Exact capacities are measured before release and named in code. The plan values
are ceilings to design against, not performance claims.

Overflow policies:

- device and contact overflow refuses the new item and logs once per episode;
- event-buffer overflow synthesizes a full local release and marks the sample
  unreliable rather than leaving unknown held state;
- transition counters saturate and set an overflow flag;
- action-map validation refuses over-limit assets before allocation;
- context pushes past the limit fail and return no token;
- diagnostic traces overwrite oldest records only when explicitly configured
  as a ring;
- malformed profile rows are skipped individually and reported;
- a failed device backend leaves the client usable with keyboard, mouse, other
  devices, or a null input runtime.

Backend errors carry device alias, operation, and stable reason. They never log
private hardware identifiers or one line per frame.

## Save, replication, and lifecycle table

| Data | Saved with game | User profile | Replicated | Recorded | Owner |
|---|---:|---:|---:|---:|---|
| action declarations and defaults | yes | no | content revision only | signature | project asset |
| user binding overrides | no | yes | no | no | local client |
| accessibility transforms | no | yes | no | resolved sample only | local client |
| raw `InputState` | current snapshot rules | no | not as player authority | diagnostic only | world and client adapter |
| device handles and SDL ids | no | no | no | no | input runtime |
| device match descriptors | no | yes, privacy-safe | no | local aliases only | local client |
| active contexts | no, unless game state recreates them | optional profile defaults | no | action trace | local slot |
| action frame state | no | no | no | diagnostic trace | input runtime |
| action tick sample | no | no | semantic intent only | action trace | local slot |
| character intent | no | no | yes, validated | authoritative replay | Players and character seam |
| text and composition | no | no | resulting GUI property by normal rules | no | focused GUI editor |
| Studio commands | no | Studio preferences | no | no | Studio |

## Migration from current paths

Migration must delete old gameplay control paths as each replacement becomes
authoritative. Do not keep hard-coded movement and named actions both driving a
character.

1. Introduce normalized device identity and event records behind the current
   translator without changing `InputState` results.
2. Add action-map parsing, validation, compilation, and tests with no gameplay
   consumer.
3. Add separate action frame and tick state for one local slot. Compare a
   default compatibility map against current keyboard, mouse, and gamepad
   behaviour.
4. Move `ReadMoveIntent` and `ReadAimIntent` policy into a default game action
   map plus a semantic intent builder. Keep the existing helpers as temporary
   adapters calling the new path.
5. Switch Studio Play and the shipped client to the same intent builder.
6. Switch network submission and prediction history to the versioned semantic
   character intent. Preserve host normalization and ownership validation.
7. Change character control to consume neutral `CharacterIntent` only. Remove
   direct `InputState` reads and player ownership from the character subsystem.
8. Add script named-action APIs while retaining raw `UserInputService` and
   compatibility `ContextActionService`.
9. Add user profiles, rebinding, accessibility, and Studio editors.
10. Add local multiplayer assignment. Remove any assumption that one process
    has exactly one input owner.
11. Add touch on the same action path, then pen, then an XR adapter when the
    platform runtime exists.
12. Remove temporary adapters, duplicated deadzones, private tap latches, and
    obsolete hard-coded key tables only after their callers are gone.

During migration, a diagnostic parity mode may evaluate old and new movement
without applying the second result. It reports differences by tick. It must be
compiled out or disabled by default after the parity gate passes.

## Delivery phases and gates

### Phase 0: contracts and measurements

- inventory every current SDL event, raw resource reader, action table, Studio
  shortcut, script signal, intent codec, prediction history, and replay path;
- capture keyboard, mouse, mapped gamepad, raw joystick, focus, and text fixtures;
- measure event bursts, frame-to-tick gaps, device hotplug order, and current
  end-to-end latency;
- decide exact component or resource placement against the architecture graph;
- freeze the first action-map schema and compatibility map.

Gate: current input suites pass against captured fixtures and every current
consumer has one named migration owner.

### Phase 1: normalized devices

- add `DeviceHandle`, capability records, privacy-safe matching, and
  `PhysicalEvent`;
- adapt SDL keyboard, mouse, gamepad, and raw joystick events;
- preserve current `Translator` output byte for byte where applicable;
- enumerate existing devices and handle duplicate hotplug notifications;
- add disconnect cancellation and device diagnostics.

Gate: old raw input behaviour is unchanged, stale handles fail safely, and
hotplug stress leaves no stuck levels or leaked device handles.

### Phase 2: action asset and compiler

- add readable source schema and versioned compiled format;
- implement actions, contexts, device schemes, composites, processors, and
  interactions;
- add validation, stable signatures, alias migration, and hostile limits;
- add deterministic topological evaluation plans;
- provide a default compatibility map for current movement and aim.

Gate: malformed and hostile assets are refused, canonical builds are
byte-identical, and all compiled plans are allocation-free during evaluation.

### Phase 3: frame and tick evaluation

- add per-local-slot frame state and fixed-tick samples;
- implement levels, counts, ordered transitions, delta accumulation, and tick
  interaction deadlines;
- add context priority, owner tokens, consumption, and barrier updates;
- integrate GUI processed evidence and focus cancellation;
- add virtual-device test injection.

Gate: taps never disappear between ticks, double transitions follow declared
policy, and the same physical fixture at different frame rates produces the
same tick samples.

### Phase 4: character and network intent

- build semantic character intent from `Move`, `Look`, `Jump`, and declared
  actions;
- route it through Players possession rather than character ownership;
- version and quantize the payload;
- retain accepted samples for prediction and rollback;
- validate possession, revision, finite values, ranges, and counts on the host;
- switch Studio Play and client to one implementation.

Gate: local, Studio Play, networked, predicted, reconciled, bot-driven, and
replayed characters consume the same intent shape.

### Phase 5: scripting

- add `InputActionService` to the neutral service catalogue;
- implement typed values and signals in Luau and JavaScript from shared facts;
- extend processed-state and device reporting;
- bridge supported raw controls into `ContextActionService` without changing
  its VM-local callable ownership;
- add permissioned virtual input for tests and tools.

Gate: both languages pass the same end-to-end input fixtures and generated
declarations match the runtime surface.

### Phase 6: profiles, rebinding, and accessibility

- implement atomic local profiles and compatibility overlay;
- add rebind sessions, conflict analysis, device calibration, and accessibility
  transforms;
- preserve orphaned overrides visibly;
- add controller assignment and reconnect matching;
- add bounded device output.

Gate: a profile survives restart, device reorder, action aliases, and map
updates without rebinding an unrelated action.

### Phase 7: Studio tools

- add Input Actions editor, conflict matrix, profile preview, and touch layout
  preview;
- add live monitor, event trace, latency view, and virtual injector;
- share only low capture and display helpers with Studio Keybinds;
- integrate undo, save, import, publish, and validation workflows.

Gate: an author can build, test, diagnose, save, reload, and publish a complete
keyboard, mouse, and gamepad map without editing source text.

### Phase 8: local multiplayer

- add local slots, assignment policy, join and leave, per-slot contexts, and
  Players service bindings;
- test paired keyboard and mouse, exclusive gamepads, reconnect grace, and
  possession swaps;
- make cameras and GUI focus resolve per local player where the rendering and
  UI systems support it.

Gate: two local players can use different devices, swap possessed characters,
disconnect, reconnect, and replay without sharing action state.

### Phase 9: touch and pen

- add bounded contacts, capture, gestures, safe-area layouts, and virtual
  controls;
- create real ContextActionService touch buttons;
- add pen capabilities and duplicate-event suppression;
- add mobile keyboard and composition lifecycle coverage.

Gate: touch and pen feed the same named actions and fixed-tick samples without
mouse synthesis being required.

### Phase 10: XR adapter

- select and isolate a platform XR backend;
- add semantic control paths, capabilities, fixed-tick poses, late-latched
  presentation poses, and haptics;
- add tracking-loss cancellation and recenter events;
- validate headless builds remain backend-free.

Gate: XR adds no alternate gameplay intent path and a tracking loss cannot
leave an action active.

## Test plan

### Physical translation

- every named logical key and physical scancode maps correctly;
- autorepeat does not create extra presses;
- composed UTF-8 remains separate from physical keys;
- mouse deltas accumulate across events and relative mode does not depend on
  position;
- horizontal and high-resolution wheel values retain sign and magnitude;
- mapped and raw controller controls do not collide;
- radial and axial deadzones match reference vectors;
- device enumeration plus duplicate add events creates one record;
- disconnect releases buttons, axes, pointers, contacts, and output;
- stale generation handles cannot reach a replacement device;
- focus loss followed by key-up does not double-fire a release;
- touch and pen compatibility events deduplicate when marked related.

### Action compilation and evaluation

- stable names survive declaration reorder;
- duplicate names, cycles, type mismatches, and limit violations fail with the
  exact offending path;
- canonical compilation is byte-identical across runs;
- every composite handles opposing inputs and diagonal normalization;
- every processor handles zero, boundary, NaN rejection, and maximum values;
- equal priorities and magnitudes use authored order;
- chords enforce exact or declared modifier policy;
- sequences, taps, holds, multi-taps, and hysteresis use tick deadlines;
- a binding cannot mutate the context stack being evaluated;
- owner destruction removes its contexts at the barrier;
- GUI consumption marks raw signals and suppresses gameplay contributions;
- an unprocessed action remains visible through all lower contexts.

### Tick and replay

- a tap wholly between two ticks appears exactly once;
- press, release, and press within one tick produces the declared counts and
  order;
- transition overflow saturates and reports;
- 30, 60, 144, and uncapped presentation rates yield identical tick samples
  for one physical fixture;
- action-map changes occur at the declared tick;
- prediction reapplies copied semantic samples after rewind;
- action and authoritative recordings replay byte-identical results;
- a replay needs no physical device or user profile;
- a refused send retains required edges without repeating accepted ones.

### Focus and text

- focused text boxes receive composed multi-byte text;
- movement keys are marked processed while typing;
- clicking away transfers or clears focus once;
- destroying the focused box ends composition safely;
- window focus loss neutralizes the next tick;
- Studio script text and Play game text never both receive one key;
- on-screen keyboard start and stop follows game text focus;
- composition cancellation does not commit partial text.

### Profiles and rebinding

- rebind capture does not perform the new action;
- hard and soft conflicts are classified by contexts and schemes;
- replace, swap, keep, clear, cancel, and timeout paths are atomic;
- unknown profile rows do not erase valid rows;
- action alias migration preserves the intended override;
- deleted actions leave visible orphan overrides;
- device reorder and reconnect recover assignment where identity permits;
- privacy-sensitive backend ids never appear in profile fixtures or logs;
- accessibility transforms reject unsafe action policy.

### Local multiplayer and character intent

- one gamepad cannot drive two local slots without explicit sharing;
- a join press does not also activate gameplay;
- disconnect neutralizes only its assigned slot;
- reconnect reclaims the reserved slot deterministically;
- possession swap neutralizes the old character and drives the new one on one
  barrier;
- bot handoff uses `CharacterIntent` without a fake Player or device;
- a malicious client cannot drive another player's character;
- server normalization prevents faster diagonal or out-of-range motion.

### Script parity

- Luau and JavaScript receive equal action values, phases, source names, and
  processed flags;
- callbacks fire once and in stable order;
- context tokens release on VM teardown;
- `ContextActionService` pass and sink behaviour remains compatible;
- raw `UserInputService` and named action services agree on device changes;
- generated declarations cover every reachable runtime member;
- headless server surfaces refuse local device methods clearly.

### Studio

- command keybinds and game action bindings remain isolated;
- undo and redo cover action, context, binding, processor, and interaction edits;
- hidden diagnostics perform no row formatting or continuous repaint;
- event filters do not change retained trace order;
- virtual injection traverses the real translator and action evaluator;
- publishing refuses invalid maps and names every issue;
- profile preview never modifies project defaults.

Fuzz targets cover action-map parsing, profile parsing, control paths, processor
chains, interaction state, network intent decoding, physical trace decoding,
and random hotplug or focus sequences under all hard limits.

## Performance and latency gates

### Budgets to measure

Profile release builds with keyboard and mouse, 1, 4, 8, and 32 devices, and
maps containing 32, 256, and 1,024 actions. Measure:

- platform pump time;
- event normalization time and event count;
- device lookup and hotplug cost;
- action-map evaluation by active binding count;
- context walk depth and consumed contributions;
- fixed-tick sample build time;
- allocations per frame and tick;
- script signal dispatch time separately from core evaluation;
- intent encode, queue, decode, validation, and apply time;
- diagnostics cost while closed and open.

The steady input and action paths allocate nothing after map or profile commit.
Text may append to a bounded transient buffer. Rebinding, hotplug, and asset
compilation are cold paths and still require explicit ceilings.

### End-to-end latency

Carry diagnostic timestamps or sequence correlations through these points:

1. backend event timestamp;
2. host ingestion;
3. normalized event;
4. action transition;
5. fixed-tick sample;
6. client intent submission;
7. server receipt and apply;
8. predicted local response;
9. authoritative correction;
10. presented frame.

Report median, p95, p99, and worst observed latency over a named run. Separate
local predicted response from network round trip. Do not subtract overlapping
worker spans or infer latency from average frame rate.

Profile at least Linux X11 or Wayland, Windows, and macOS because event pumps,
focus, raw mouse access, keyboard layouts, gamepad hotplug, and text input differ
by backend. Mobile and XR gates name their platform and device.

### Acceptance gates

- closed diagnostics add no measurable continuous presentation work;
- steady keyboard and mouse input allocate zero bytes;
- action evaluation cost scales with active compiled bindings, not every known
  platform control;
- one idle map with no changes can reuse its compiled and evaluated state;
- event overflow, late samples, saturated transitions, and dropped output are
  visible counters;
- input work inside one tick joins before the phase ends;
- the server build contains no SDL input or window backend dependency.

## Observability

Counters and gauges include:

- events by kind and device class;
- unmapped and refused controls;
- connected, assigned, and reconnecting devices;
- event-buffer high-water mark and overflow episodes;
- action transitions, consumed contributions, and active contexts;
- context push refusals and leaked-owner cleanup;
- transition saturation and retained edge age;
- profile rows applied, orphaned, migrated, and refused;
- hard and soft conflicts;
- input sends accepted, refused, retried, and expired;
- server intent decode and authority refusals;
- device output commands accepted, coalesced, and dropped;
- focus cancellation and synthesized release counts;
- text commit and composition cancellation counts without recording text;
- latency histograms for the points above.

Logs use an `input` category with device aliases, local slots, action names, and
tick or event sequence. Log levels are dynamic. High-frequency events are trace
level and rate-limited. Committed text, hardware serials, raw device paths, and
personal binding files are not logged.

## Explicit non-goals

The first production action system does not include:

- a universal visual scripting graph for input;
- cloud synchronization of personal profiles;
- kernel-level global hotkeys or input capture outside application focus;
- anti-cheat based on trusting client device metadata;
- arbitrary script-created hardware identities;
- recording personal typed text;
- platform gesture results as authoritative gameplay;
- XR hand skeletons, eye tracking, or body tracking before the base XR adapter;
- motion matching or animation selection inside input;
- character ownership, possession policy, or bot decision making;
- a replacement for Studio command keybinds;
- a second network channel dedicated to raw actions;
- guaranteed persistent identity on hardware whose platform exposes none.

## Open decisions

These choices need measurements or coordination with adjacent plans before the
corresponding phase begins. None blocks the normalized keyboard, mouse, and
gamepad foundation.

1. Choose the exact shared storage shape for per-local-player
   `ActionFrameState` and `ActionTickState`. A bounded resource table avoids
   thousands of entities, while keyed ECS rows make ownership and inspection
   more direct. The choice must preserve local-player isolation without copying
   action values onto characters.
2. Choose the source and compiled file suffixes for `InputActionMap`. The source
   must remain readable and diffable, while the published artifact needs a
   versioned canonical byte format and asset classification.
3. Measure event bursts and common action maps before fixing capacities below
   the hard ceilings in this plan. Keyboard rollover, high-rate mice, touch,
   reconnect storms, and diagnostic recording need separate fixtures.
4. Decide whether physical keyboard movement defaults use scancodes or logical
   keys per project locale. The asset model supports both, but the starter map
   needs one documented policy and localized display behaviour.
5. Define the precise ordering between named action signals and
   `ContextActionService` callbacks. Compatibility requires the VM-local stack,
   while gameplay requires action evaluation that does not depend on a callback
   finishing at a variable point in the tick.
6. Decide which privacy-safe gamepad fields each platform may use for reconnect
   matching. The fallback must remain useful when a stable device field is not
   available.
7. Decide whether gamepads may drive an assigned local player while another
   application has window focus. The default remains no background input.
8. Select the first mobile platform and device matrix before fixing touch safe
   area, on-screen keyboard, and gesture acceptance gates.
9. Select an XR runtime only when the render and platform plans have a matching
   backend. The input action model should not choose that vendor boundary by
   itself.

## Completion definition

The input system is complete for its first production scope when:

- keyboard, mouse, mapped gamepads, and raw joysticks enter one normalized
  device path with safe hotplug and focus cancellation;
- project action maps compile into bounded deterministic plans;
- named typed actions, contexts, consumption, rebinding, conflicts, and
  accessibility work per local player;
- simulation receives one deterministic tick sample and no longer reads raw
  keys for character control;
- Players routes semantic intent to the actively possessed character, while
  bots and replays use the same `CharacterIntent` without fake devices;
- prediction, reconciliation, and authoritative replay retain semantic tick
  inputs;
- Luau and JavaScript expose matching raw and named input behaviour;
- profiles survive restart, map migration, device reorder, and reconnect;
- Studio can author, validate, test, monitor, and publish maps while its command
  bindings remain isolated;
- hostile limits, fuzz tests, deterministic fixtures, latency profiles, and
  server-headless checks pass;
- the temporary hard-coded movement and duplicate input paths have been
  removed.
