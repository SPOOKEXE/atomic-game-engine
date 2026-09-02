# Camera and cinematics system plan

## Status

This document describes future work. It does not claim that the current engine
has camera rigs, cinematic sequences, replay cameras, a sequencer, depth of
field, or a general camera blending system.

The first useful delivery is deliberately narrow: separate authored cameras
from the controller that drives the local view, add reusable camera rigs and
deterministic blends, then let Studio author a sequence that drives a camera,
an animation, a sound, and a named presentation event from one clock. Later
features extend those foundations rather than adding another timeline or
another way to move the active camera.

## Product goal

Build one inspectable camera and cinematic system for gameplay, Studio, replays,
spectating, viewports, portals, and scripted presentation.

The system must support:

- reusable follow, orbit, first-person, shoulder, rail, fixed, and free camera
  rigs;
- explicit subjects, look targets, offsets, constraints, collision, and
  composition rules;
- deterministic cuts and blends between camera outputs;
- bounded shake and procedural noise without permanently moving authored data;
- physical and familiar lens controls, including focal length or field of view,
  focus distance, aperture, exposure intent, and depth of field;
- several cameras in one world, with one active camera per view rather than one
  global answer for every render target;
- `ViewportFrame`, mirror, portal, surface, thumbnail, and Studio views without
  duplicating camera math;
- cinematic sequences with typed tracks, clips, keyframes, events, markers,
  nested subsequences, and explicit binding slots;
- synchronised camera, animation, audio, VFX, visibility, property, and script
  presentation cues;
- predictable play, pause, seek, scrub, loop, skip, and reverse rules;
- gameplay cameras, cinematic cameras, replay cameras, and spectator cameras
  that share rig evaluation but keep their authority rules distinct;
- a generated Luau and JavaScript surface that exposes authored facts and safe
  playback commands;
- a Studio sequencer with a timeline, curve editor, camera preview, shot list,
  diagnostics, undo, and non-destructive recording;
- deterministic headless evaluation tests plus stable image comparisons for
  visual output;
- enough profiling and diagnostics to explain why a shot, view, event, or
  render target did not run.

Camera evaluation never owns gameplay state. It reads a frozen simulation
snapshot and produces presentation output. In particular, a cinematic callback
must not mutate gameplay while a render target samples a sequence. A portal,
thumbnail, split-screen view, or editor panel may sample the same time more
than once, and a callback fired from sampling would make gameplay depend on how
many views happened to render.

## Existing foundation

The engine already has useful pieces. This plan extends them instead of building
parallel copies.

| Existing piece | Current role | Required use |
|---|---|---|
| `scene::Camera` | Serializable lens component with vertical field of view, near and far planes, and image bounds | Remain the authored lens carried by every camera instance |
| `scene::ActiveCamera` | Names the live camera entity and stores the current consumer aspect ratio | Migrate toward per-view selection while preserving the single-view compatibility path |
| `scene::ResolveCamera` | Builds view, projection, and view-projection matrices for one target aspect | Stay the shared perspective camera math |
| `scene::ResolveSurfaceCamera`, `SurfaceProjection`, and `ObliqueProjection` | Build off-axis and clipped surface views | Remain the only surface and portal projection path |
| `scene::CameraController` | Local classic, first-person, shift-lock, and scriptable camera state | Become the compatibility gameplay rig and local input adapter, not the universal cinematic runtime |
| `scene::UpdateCameraControl` and `PlaceCamera` | Read copied input state and place the local camera | Feed a gameplay rig using fixed-tick input and keep headless tests possible |
| `scene::FollowOwnCharacter` | Assigns the local player character root as camera subject unless scriptable | Move to the Players client binding, which follows active possession rather than character ownership |
| `scene::AimSurfaceCameras` | Fits mirror and portal cameras from the active viewer | Consume an explicit source view and immutable evaluated camera output |
| `gui::Viewport` on `ViewportFrame` | Names a camera and supplies frame-specific lighting and tint | Become a first-class view consumer without changing the world active camera |
| Studio multi-viewports and `ViewerLensToWrite` | Give editor panels independent target sizes and avoid overwriting authored lens edits | Supply the first multi-view consumer and preserve author ownership rules |
| `scene::AnimationBuffer`, `AnimationClip`, and `AnimationTrack` | Store scriptable baked animation and fixed-tick playback state | Reuse clip sampling and curve interpolation where data formats align, without making cameras skeletons |
| `world::Recorder` and `world::Replayer` | Record and replay applied fixed-tick world barriers | Supply deterministic world history for replay cameras |
| `scene::AudioState` and `audio::ListenerPose` | Select the world listener and place the mixer ear | Follow the final composed view or an explicit object through one declared policy |
| `SurfaceCamera` and `Portal` | Render mirrors, remote panes, recursive views, and non-Euclidean seams | Reuse evaluated camera outputs and existing slot budgets |
| `render` view recording and image targets | Build target-specific camera matrices and submit views | Consume immutable evaluated outputs and enforce view budgets |
| Studio `nodegraph` | Generic Studio-only graph editing library | Remain available for camera rig graphs if needed, but not replace the sequencer timeline |
| generated Luau and JavaScript bindings | Reflect registered scene classes into both runtimes | Expose one matched camera and sequence API |
| fixed world ticks and mutation barriers | Give simulation a stable order | Advance authoritative sequence state and commit events at named phases |
| profiling, metrics, and image test support | Attribute CPU, GPU, memory, and rendered output | Gate runtime cost and visual stability |

`scene::ActiveCamera` is intentionally not a matrix cache. A swapchain, portal,
viewport frame, thumbnail, and Studio panel may have different sizes and near
plane needs. The future system keeps that rule. It caches target-independent
rig and lens output, then each view resolves matrices against its own target.

## Non-negotiable design rules

1. Simulation and presentation have separate clocks and separate side effects.
   A render sample may not mutate gameplay.
2. A sequence event that affects gameplay is committed once by the authoritative
   fixed-tick sequence runner. It is never fired by render sampling, scrubbing,
   thumbnails, image tests, or editor previews.
3. Presentation events may be sampled for a view, but their handlers may only
   write a bounded presentation command buffer consumed after sampling.
4. A camera output is plain copied data. Nothing crossing a world, render, or
   worker boundary is a pointer to a camera rig, subject, sequence, or track.
5. Persistent bindings use stable names and instance references. Dense track,
   clip, key, and cache numbers are local compiled handles only.
6. Each view selects its camera explicitly. The compatibility active camera is
   not a hidden global input to every offscreen target.
7. Target-independent evaluation happens once per view time and revision.
   Projection matrices remain target-specific.
8. Every sequence, event scan, nested sequence, camera stack, view family,
   render target, and cache has a declared bound and a visible overflow status.
9. Equal times and priorities use stable authored order and stable names as
   tie-breaks. Container or worker completion order never decides a shot.
10. Runtime state lives in ECS rows and resources. Compiled sequence data,
    sampled curves, and render matrices are derived caches.
11. Camera animation may reuse the existing animation curve decoder and sampler,
    but a camera is not a skeleton and does not gain bones or an `Animator`.
12. Studio owns authoring UI, preview state, selection, undo, and thumbnails.
    Engine modules own formats, validation, deterministic evaluation, and
    runtime semantics.
13. A headless server may advance authoritative sequence state without loading
    textures, audio samples, shaders, or a renderer.
14. Client scripts cannot use a cinematic camera to gain gameplay authority,
    inspect hidden replicated state, or emit authoritative events.
15. Camera collision queries use the shared scene and physics query surfaces.
    Camera code does not keep a private collider copy.
16. Replacing an existing camera path includes deleting its old writer. Two
    systems must not fight over the same transform.

## Ownership and layer placement

The first implementation should extend `scene` for serializable components and
add focused runtime code at the lowest layer that can evaluate it. Do not add a
large `cinematics` module until the sequence compiler and evaluator have a real
dependency set that justifies one. If a module is added, update the checked
architecture graph in the same change.

| Owner | Responsibility |
|---|---|
| `core` | Stable names, vectors, frames, colours, byte formats, and interpolation helpers |
| `ecs` | Saved rows, instance hierarchy, revisions, and mutation barriers |
| `scene` | Cameras, rig descriptions, sequence components, playback state, binding slots, and copied camera outputs |
| `physics` | Bounded collision and occlusion queries requested by camera rigs |
| `world` | Fixed ticks, recording, replay restoration, and authority barriers |
| `assets` | Immutable camera, sequence, animation, audio, and curve artifacts by content name |
| `script` | Registered methods, property access, callbacks, and permission checks |
| `audio` | Listener placement and sample-accurate mixer scheduling from presentation commands |
| `render` | Per-target projection, depth of field, exposure, view families, portals, surfaces, and GPU submission |
| `gui` | `ViewportFrame` camera consumption and view sizing |
| client | Local gameplay input, camera selection, spectator controls, replay controls, and final listener policy |
| server | Authoritative sequence clock, gameplay event commitment, and allowed replication |
| Studio | Sequencer, curve editor, recording, shot preview, diagnostics, undo, baking, and image capture |

The main data flow is:

1. fixed-tick systems advance authoritative gameplay and sequence playheads;
2. authoritative sequence runners scan crossed gameplay event intervals once;
3. committed gameplay events enter the normal world mutation barrier;
4. presentation builds an immutable interpolation snapshot;
5. each visible view requests a camera at a presentation time;
6. rig and sequence evaluation produce a target-independent `CameraOutput`;
7. modifiers such as shake and gameplay composition are applied in stable order;
8. each target resolves its own matrices and post-process settings;
9. presentation commands for audio, VFX, and local callbacks are deduplicated
   and consumed outside camera sampling;
10. render and audio use the copied results without writing simulation state.

## Public object model

### `Camera`

The existing `Camera` instance remains the public lens and authored frame. Add
properties only when the renderer can honour them.

Planned lens properties:

| Property | Type | Meaning |
|---|---|---|
| `ProjectionMode` | named enum | `Perspective`, with orthographic reserved until fully supported |
| `FieldOfView` | number | Vertical field of view in degrees at the script boundary |
| `FocalLength` | number | Optional physical-lens authoring value in millimetres |
| `SensorHeight` | number | Sensor height used to convert focal length to vertical field of view |
| `NearPlane` | number | Near clipping distance in metres |
| `FarPlane` | number | Far clipping distance in metres |
| `FocusDistance` | number | Distance from the lens to the focus plane |
| `Aperture` | number | F-number used by depth of field |
| `DepthOfFieldEnabled` | boolean | Enables physical depth-of-field output for this lens |
| `ExposureCompensation` | number | Authored exposure intent passed to the render pipeline |
| `ImageWidth` and `ImageHeight` | integer | Existing fixed capture request |
| `MaxImageWidth` and `MaxImageHeight` | integer | Existing allocation ceiling |

`FieldOfView` and `FocalLength` are two views of one lens fact. Setters update
the canonical vertical field of view using `SensorHeight`; they are not saved as
two independent values that may disagree. Invalid, non-finite, or degenerate
values are refused at the property boundary.

Depth of field is a render feature. A headless server saves and replicates lens
intent but does not evaluate blur. Unsupported backends render a sharp image
and expose a diagnostic counter rather than silently changing the focus model.

### `CameraRig`

A camera rig describes how to produce a camera frame from subjects and
parameters. It is not itself the active camera and does not own input.

Common properties:

- stable rig type name;
- primary subject and optional look target;
- local subject offset and camera offset;
- up-vector policy;
- position and rotation damping times;
- minimum and maximum distance;
- collision policy and collision padding;
- composition policy and screen-space target point;
- enabled state and priority;
- a bounded typed parameter set;
- a revision incremented by authored changes.

Initial built-in rig types:

- `Fixed`: use the camera instance frame;
- `Follow`: maintain an offset from a subject;
- `Orbit`: yaw, pitch, and distance around a subject;
- `FirstPerson`: place the camera at a subject attachment;
- `Shoulder`: orbit with a side offset and optional shoulder swap;
- `LookAt`: keep authored position and aim at a target;
- `Rail`: move along an authored spline with optional look target;
- `Free`: consume a local editor or spectator navigation intent;
- `ReplayFollow`: follow recorded subject samples with replay-time smoothing.

The existing `CameraController` becomes the compatibility adapter for `Orbit`,
`FirstPerson`, and `Shoulder`. Its public names may remain for scripts, but the
placement math must move behind the same rig evaluator used by sequences and
tests. Scriptable mode means the fixed camera or sequence output owns the view.

Rig parameters use a bounded, registered schema. Unknown parameter names are
retained in saved data for forward compatibility but ignored with a diagnostic.
They do not become an untyped dictionary consulted in the hot path.

### Subjects and binding slots

A rig or sequence references a stable binding slot such as `Camera`, `Hero`,
`LookTarget`, or `Vehicle`, then a playback instance resolves that slot to an
entity. This lets one sequence play for different characters without rewriting
the asset.

Bindings are validated before playback:

- the entity is alive;
- the required component or class is present;
- the binding belongs to the current world unless the track explicitly supports
  a named cross-world seam;
- the binding is visible to the current authority;
- required attachments or bones resolve by stable name;
- the binding count remains below the sequence limit.

A missing optional binding disables only its track. A missing required binding
puts playback in `Blocked` with a structured reason. It does not bind to the
first matching name found by walking the world.

### `CameraOutput`

Rig and sequence evaluation produce a copied target-independent value:

```text
CameraOutput
  Frame
  VerticalFieldOfView
  NearPlane
  FarPlane
  FocusDistance
  Aperture
  ExposureCompensation
  DepthOfFieldEnabled
  ListenerPolicy
  SourceRevision
  Flags
```

This value contains no matrix, aspect ratio, render target, raw pointer, event
callback, or mutable sequence state. A renderer resolves target-specific
matrices later. The output may be cached by sequence revision, binding revision,
sample time, and modifier revision.

### `CameraView`

A view selects what one consumer renders. It separates camera selection from
the camera instance and prevents one global `ActiveCamera` from serving targets
with different needs.

Properties include:

- source camera or active camera stack;
- target kind and stable target name;
- viewport rectangle and render scale;
- optional fixed width and height;
- post-process profile override;
- layer or collection tag filter;
- recursion and secondary-view budget;
- audio-listener eligibility;
- enabled and visible state;
- diagnostic status and last rendered revision.

The main window owns one primary `CameraView`. Each `ViewportFrame`, Studio
viewport, thumbnail request, mirror, and portal owns or derives another view.
The `ActiveCamera` compatibility resource maps to the primary view only.

## Rig evaluation

### Evaluation inputs

Evaluation reads one immutable input set:

- current and previous fixed-tick transforms for each bound subject;
- presentation interpolation alpha;
- rig parameters and revision;
- local camera intent already copied from the platform input layer;
- bounded physics query results from the declared camera collision phase;
- sequence sample result;
- replay sample result when replaying;
- deterministic noise state;
- view purpose, such as primary, viewport, portal, thumbnail, or editor preview.

Render sampling does not query mutable input devices, run arbitrary scripts, or
walk the ECS while workers may be changing it.

### Constraints and composition

Constraints apply in stable order:

1. resolve subject and authored offset;
2. evaluate base rig frame;
3. apply rail or positional constraints;
4. solve look target and up-vector constraint;
5. apply pitch, yaw, distance, and region limits;
6. apply camera collision and occlusion correction;
7. apply composition adjustment;
8. apply deterministic shake and local presentation modifiers;
9. resolve the final lens;
10. emit `CameraOutput`.

Initial constraints include bounded axis ranges, distance ranges, look-at,
world or subject-relative up, spline rails, volume confinement, horizon lock,
and screen-space composition. A constraint that cannot solve keeps the last
valid result for one sample, records a reason, then falls back to the base rig.
It never emits NaN transforms.

Composition adjustment should be conservative. It may shift or rotate the
camera within authored limits to keep a target near a screen point. It must not
run an unbounded iterative solver per view. Start with an analytic look-at and
bounded distance correction, then profile before adding more.

### Collision and occlusion

The existing poppercam idea remains: authored orbit distance is not overwritten
by the temporary collision-corrected distance.

The camera collision phase submits bounded sphere casts or raycasts from the
subject to candidate camera positions. Results are stable and joined before
presentation uses them. Policy selects among:

- `None`;
- `PullForward`;
- `Slide` along a hit plane within a small iteration bound;
- `FadeOccluder`, which emits local presentation fade commands;
- `Hybrid`, which pulls forward first and requests fading only inside a limit.

Camera collision changes presentation only. It never moves the subject or
changes gameplay collision. Portal transit remains an explicit transform of the
rig frame and yaw state, not a collision side effect.

### Damping and interpolation

Damping uses an analytic, frame-rate-independent response based on elapsed
presentation time. It is never a fixed fraction per rendered frame. Seeking,
cuts, binding changes, world changes, and large time discontinuities reset
damping explicitly.

Gameplay camera damping is local presentation state and is not replicated.
Cinematic camera movement is sampled from authored curves and gives the same
frame at the same sequence time. Optional cinematic smoothing must be baked or
defined by a pure analytic filter with a stated seek rule.

## Blending and camera stacks

### Camera stack

Each primary view owns a bounded stack of camera claims. Claims include source,
priority, blend policy, owner token, and lifetime. Typical sources are gameplay,
cinematic, death, spectator, replay, photo mode, and Studio preview.

The highest eligible claim wins. Equal priorities use stable claim type and
creation sequence. A claim requires an owner token so teardown can release only
its own camera. Destroying a sequence, switching worlds, or losing possession
cannot leave a hidden scriptable camera claim behind.

### Blend model

A blend stores:

- outgoing and incoming source ids;
- start time and duration;
- position curve;
- rotation curve;
- lens curve;
- cut flags;
- optional match target;
- interruption policy.

Position uses linear or cubic interpolation. Rotation uses shortest-path
quaternion interpolation, with squad or a baked curve available only where
authored continuity needs it. Lens values blend in a declared space. Field of
view blends in focal-length space when a physical lens is enabled, avoiding the
uneven visual speed of a raw angle blend.

Blend curves are named registered curves or baked scalar curves. Arbitrary
script functions are not called per sample.

Interruption policies are:

- `CutToNew`;
- `BlendFromCurrentOutput`;
- `FinishThenQueue`;
- `IgnoreLowerPriority`.

`BlendFromCurrentOutput` snapshots copied camera values, not a pointer to a
source rig. This gives continuity even if the outgoing camera is destroyed.

## Shake and procedural camera motion

Camera shake is a presentation modifier, not an edit to the camera instance.

A `CameraShake` request carries:

- stable source name and explicit seed;
- start time and duration;
- translation and rotation amplitude;
- frequency band;
- envelope curve;
- falloff origin and distance when spatial;
- priority, mix mode, and channel mask;
- view and audience filter.

Requests are bounded per view. Equal requests sort by priority and stable source
name. Noise is a pure function of seed and sample time, so seeking and image
tests produce the same result. No random generator advances once per rendered
frame.

Impulses, recoil, footsteps, vehicle vibration, and environmental sway use the
same modifier surface with different envelopes. Authoring may bake a procedural
shake to a camera curve when exact export is needed.

High-frequency shake is clamped by angular and positional acceleration limits,
with optional comfort scaling. Accessibility settings may reduce translation,
rotation, field-of-view pulses, and frequency independently without changing
gameplay.

## Lenses and render integration

### Lens model

Perspective remains the first supported projection. Orthographic projection is
added only with complete culling, pointer projection, surface-camera, shadow,
and Studio support.

The lens model keeps:

- vertical field of view as the canonical saved projection value;
- optional focal length and sensor metadata for authoring;
- near and far planes;
- focus distance and aperture;
- exposure compensation;
- optional focus target binding;
- depth-of-field quality tier and maximum blur radius.

Autofocus is an explicit rig modifier. It resolves a bound target or a bounded
raycast result and applies damped focus distance. It does not read the depth
buffer back synchronously.

### Depth of field

Depth of field runs only when the view is visible, the render profile supports
it, and the lens requests it. Portal, mirror, thumbnail, and `ViewportFrame`
views default it off unless the consumer opts in. This avoids multiplying an
expensive full-screen effect across secondary views.

The render graph receives physical lens parameters and target size. It owns
circle-of-confusion calculation, blur passes, transient targets, and quality
fallbacks. Camera runtime never names a shader or device texture.

### Multi-camera and view families

A view family groups views that share a simulation snapshot and presentation
time. It declares:

- primary view;
- secondary views;
- shared culling eligibility;
- target sizes and render scales;
- surface recursion budget;
- post-process policy;
- capture purpose;
- listener eligibility.

Split-screen views each have a primary camera and viewport. Only one declared
view drives the audio listener unless a future mixer explicitly supports more.
Picture-in-picture, security feeds, and `ViewportFrame` are secondary views and
never replace the primary active camera.

### Surface and portal cameras

Surface cameras remain derived views. `AimSurfaceCameras` receives the source
view output instead of consulting hidden global state. It keeps its current
off-axis frustum, oblique clip plane, pane fit, stable slot order, FPS cap, and
resolution budget.

Rules:

- a mirror derives its frame from the source view and pane;
- a portal derives it through the declared seam transform;
- recursive portal views carry an explicit recursion depth and visited seam
  chain;
- a cinematic rendered in a portal samples the same immutable sequence time;
- surface sampling does not fire sequence callbacks;
- a surface view cannot become the audio listener;
- invisible, occluded, offscreen, closed-tab, or over-budget views do not render;
- stale slots are released or marked invalid by the existing ownership rules;
- a missing destination renders the declared fallback tint or texture.

### `ViewportFrame` and Studio views

`ViewportFrame.CurrentCamera` selects its own camera. It never changes
`Workspace.CurrentCamera`. Its lighting remains local to the frame.

Studio panels keep independent view size, render scale, visibility, camera
claim, and preview time. A hidden tab does not evaluate camera curves or submit
a render. Thumbnail generation uses an isolated preview context and cannot
advance the edited world's sequence playback.

## Cinematic sequence model

### Assets and instances

A `CinematicSequence` asset is immutable compiled content. A world plays it
through a `SequencePlayer` instance that holds bindings and runtime state.

The asset stores:

- stable asset name and format version;
- display rate and tick resolution;
- duration and playback range;
- binding slot declarations;
- typed tracks and clips;
- curve and keyframe data;
- markers and named presentation events;
- authoritative gameplay event declarations;
- nested subsequence references;
- source and compiled signatures;
- declared bounds and feature requirements.

The player stores:

- sequence asset reference;
- current time and previous committed time;
- playback state, direction, speed, and loop count;
- binding map;
- authority mode;
- camera stack claim;
- last committed gameplay event cursor;
- presentation event generation;
- sequence revision and diagnostic status.

Compiled content may live in a world-owned buffer during procedural authoring,
following `AnimationBuffer`, then publish to an immutable asset. The byte format
is versioned, bounded, validated before use, and opaque below its owning layer.

### Track types

Initial typed tracks are:

- `CameraCutTrack`: selects camera shots and blend policies;
- `CameraPropertyTrack`: animates rig and lens values;
- `TransformTrack`: animates a bound presentation transform;
- `AnimationTrack`: controls an existing animation clip or track;
- `AudioTrack`: schedules sound start, stop, gain, pitch, and fades;
- `VFXTrack`: emits named VFX presentation commands;
- `VisibilityTrack`: controls local presentation visibility;
- `PropertyTrack`: animates an allowlisted presentation property;
- `PresentationEventTrack`: emits local named events;
- `GameplayEventTrack`: commits authoritative fixed-tick requests;
- `SubsequenceTrack`: embeds another sequence with a mapped time range.

Do not start with a universal track that stores arbitrary property paths and
arbitrary values. Registered track schemas give bindings, validation, generated
script types, and clear failure messages.

### Keyframes and curves

Each key carries time, value, interpolation, tangent data where applicable, and
stable authored order. Supported interpolation starts with step, linear, and
cubic Hermite. Quaternion channels use a rotation-specific sampler.

Camera transform curves may reuse the same scalar, vector, quaternion, key
reduction, and interval lookup code as animation clips. They use camera channel
names and sequence bindings, not joint slots, rig ids, bones, or `Animator`.

Key lookup uses a compiled sorted array and cached interval cursor. Seeking may
binary search. Sequential playback advances the cursor. Editing invalidates the
compiled curve revision and never patches hot runtime arrays in place.

### Time representation

Authored time uses signed integer ticks with an asset-defined resolution. This
avoids float drift across long sequences and gives exact event boundaries.
Presentation converts to seconds only at sampler boundaries.

Display rate controls ruler labels and frame snapping. It does not reduce
internal tick resolution. NTSC-style rates use rational numerator and
denominator values rather than rounded decimals.

## Playback semantics

### Play, pause, stop, and loop

`Play` begins from the current position or an explicit range start. `Pause`
keeps position and claims. `Stop` releases the camera claim and resets according
to a declared stop policy. Destroying a player always releases claims and
presentation resources.

Looping defines a half-open playback range `[start, end)`. An event exactly at
the end belongs to the next loop only when the range wraps. This prevents an end
marker firing twice.

### Seek and scrub

Seeking evaluates state at the destination without replaying every crossed
presentation event. The caller chooses one policy:

- `Silent`: update sampled state only;
- `PresentationAtTarget`: emit idempotent presentation state at the target;
- `CommitAuthoritative`: authority-only, scans and commits allowed gameplay
  events through fixed-tick barriers.

Studio scrubbing is always `Silent` for gameplay. It may preview audio snippets,
VFX, and presentation events in an isolated preview command buffer. It never
calls live gameplay handlers.

Large seeks reset damping, shake accumulation, audio streaming cursors, temporal
render history, and interval caches. They do not leave motion blur or depth of
field history from the old time.

### Reverse playback

Curves and sampled state support reverse playback. Events require explicit
rules:

- presentation markers may opt into forward, reverse, both, or silent;
- audio clips seek or restart according to backend support;
- one-shot VFX reverse only when the effect has a reversible simulation or a
  baked cache;
- gameplay events do not automatically undo themselves;
- authoritative reverse is refused when the range contains non-reversible
  gameplay events, unless the game restores a recorded world snapshot.

Reverse replay uses world snapshots and applied barrier history rather than
inventing inverse gameplay callbacks.

### Skip

Skipping is a named operation, not a fast seek. A sequence asset declares:

- skippable ranges;
- destination marker;
- mandatory authoritative events to commit;
- final presentation state to apply;
- blend or cut out policy;
- whether all participants must agree in a networked session.

The authority commits mandatory gameplay effects once, sets the new playhead,
and replicates the resulting state. Clients clear presentation commands and
rebuild target state at the destination. No loop of accelerated frames runs.

### Event interval rules

An event fires when the committed playhead crosses its integer tick in the
declared direction. Scans use open and closed endpoints that are stated once:

- forward: `(previous, current]`;
- reverse presentation scan: `[current, previous)`;
- seek: policy-specific and silent by default;
- loop: split into two scans without including either boundary twice.

Equal-time events run by track priority, stable track name, then authored event
order. Every committed authoritative event carries sequence instance id, loop
generation, event id, and tick so deduplication is exact.

## Animation, audio, VFX, and scripts

### Animation sync

An animation clip controlled by a sequence remains an ordinary
`scene::AnimationTrack`. The sequence writes its playhead, weight, speed,
priority, and playing state at the fixed presentation phase.

Sync options include:

- absolute sequence time;
- clip-relative time;
- named animation marker alignment;
- loop phase;
- blend-in and blend-out ranges;
- pause and reverse support.

The animation renderer samples the resulting track normally. Camera sequences
do not own a second skeleton pose evaluator.

### Audio sync

Audio tracks compile to timestamped presentation commands with sample offsets.
The audio layer schedules them against its monotonic device clock after mapping
from the current sequence time. A seek cancels old scheduled commands by
generation and re-establishes active loops and fades at the destination.

Lip sync and beat-critical cuts may use audio markers stored beside the clip.
The fixed-tick gameplay clock is not stretched to chase an audio device. Drift
correction applies only to local presentation playback and stays within a
bounded rate adjustment.

### VFX sync

VFX tracks emit stable effect name, seed, binding, transform, start time,
duration, and parameter block. A deterministic effect can seek by age. A
non-seekable effect restarts from the nearest baked checkpoint or reports that
scrub preview is unavailable.

Skipping and hidden views do not simulate every missed particle step. They
restore final persistent presentation state and discard transient one-shots
unless the skip policy names them as mandatory.

### Script events

There are two separate script surfaces:

- presentation events run on the client after sequence sampling and receive a
  copied event payload;
- gameplay events run through the authoritative fixed-tick command path.

Presentation handlers cannot call mutation APIs while the sampler is active.
The script host either queues allowed presentation commands or returns an error
such as `mutation not permitted during cinematic sampling`. Gameplay event
handlers are never called from render code.

Payload schemas are registered and bounded. An event contains stable names,
numbers, booleans, vectors, colours, frames, and instance binding references.
It does not carry closures, raw pointers, arbitrary tables, or wall-clock time.

## Gameplay, cinematic, and presentation authority

### Gameplay camera

The local gameplay camera consumes input and active possession from client-side
Players bindings. The character system does not own it. Switching a player's
active character changes the gameplay rig subject atomically, unless a higher
camera claim is active.

Gameplay camera transform, damping, collision correction, shake, and comfort
settings are local presentation state. They are not authoritative gameplay
facts and usually do not replicate.

### Authoritative cinematic state

The server owns sequence start, stop, skip, authoritative bindings, gameplay
events, and shared playhead when a cinematic affects gameplay. Clients receive
a sequence id, start tick, range, speed, binding ids, and revision. They derive
presentation time from the replicated fixed-tick anchor.

The server does not replicate camera transforms every frame. It replicates the
small authored inputs needed for clients to evaluate them. A client that lacks
the asset or binding falls back cleanly and reports the content failure.

Purely local menu, tutorial, kill, photo, and accessibility cameras may be
client-owned when they cannot affect authoritative state.

### Camera versus gameplay queries

Gameplay code must not branch on the local camera transform, field of view,
visibility, depth of field, or which shot is active. If gameplay needs an aim
direction, the input or character intent owns it. If it needs a cutscene state,
the authoritative sequence runner exposes a named gameplay fact.

This rule prevents a client camera, spectator, portal view, or accessibility
setting from changing simulation.

## Replay and spectator cameras

### Replay source

The existing world recorder supplies authoritative state at fixed barriers.
Replay presentation interpolates between restored snapshots and may run on a
copy of the world. Camera bookmarks and local view choices are a separate
presentation recording.

Optional recorded camera data includes:

- active rig and subject binding;
- local orbit intent samples;
- cuts and blend claims;
- lens changes;
- shake requests with seeds;
- sequence ids and playheads;
- operator bookmarks.

The default replay camera may be reconstructed from recorded characters and
events without storing every camera frame. A broadcast camera can also be
authored after the match without changing the gameplay recording.

### Replay controls

Replay supports play, pause, fixed-step, variable speed, seek to snapshot,
bookmark, subject selection, camera mode change, and clip export. Reverse seeks
to a prior snapshot and replays barriers forward to the requested time. It does
not reverse simulation in place.

Temporal render history, audio scheduling, VFX age, camera damping, and event
generations reset on replay seek.

### Spectator modes

Initial spectator modes are:

- free camera within server-defined bounds;
- follow subject;
- first-person subject view where allowed;
- orbit subject;
- fixed authored cameras;
- automatic director driven by safe replicated facts.

The server validates what entities and data a spectator may observe. A free
camera does not expand replication interest into hidden areas unless the game
explicitly grants it. Competitive games may restrict first-person views,
delays, teams, and camera volumes.

An automatic director ranks bounded candidate shots from copied facts, applies
hysteresis and minimum shot time, then emits ordinary camera claims. It is not a
second renderer or sequence system.

## Save, replication, and content format

### Saved facts

Save:

- camera instances, lenses, transforms, and image limits;
- rig instances and registered parameter values;
- authored rails, constraints, and binding slot declarations;
- sequence asset references;
- sequence players when runtime persistence is requested;
- stable binding names and instance references;
- camera view authoring for world-owned screens and viewports;
- explicit listener policy;
- revision and format information needed to validate caches.

Do not save:

- matrices;
- damping velocity;
- collision-corrected distance;
- evaluated camera output;
- curve interval cursors;
- render targets;
- depth-of-field history;
- surface-camera slot allocation;
- Studio selection, hover, or preview time in the world document.

### Replication policy

Replicate only facts that another host needs:

| Data | Default policy |
|---|---|
| Authored camera and rig properties | Server to relevant clients |
| Authoritative sequence player state | Server to relevant clients |
| Gameplay event results | Through normal gameplay replication |
| Sequence assets | Content manifest and CDN, not component deltas |
| Local gameplay camera orbit and damping | Never |
| Local shake from accepted gameplay event | Replicate compact request or derive locally |
| Spectator subject and allowed mode | Validate server-side, replicate as needed |
| Viewport and surface camera authoring | Replicate with owning world content |
| Evaluated matrices and post-process history | Never |

A sequence start message names the asset by stable content name and expected
hash. The client refuses a mismatched asset, reports `ContentMismatch`, and uses
a declared fallback camera. It does not play a different local revision.

### Format validation

The compiled format validates:

- magic, version, endianness, and declared byte length;
- maximum tracks, clips, keys, bindings, events, and nesting depth;
- sorted integer times and valid playback ranges;
- finite values and normalisable quaternions;
- registered track, property, curve, and event schemas;
- asset dependency cycles;
- nested sequence recursion;
- content hashes and source signatures;
- no gameplay events on presentation-only assets;
- no unsupported reverse or seek promises.

Loading failure returns a structured code and leaves the previous valid compiled
asset in use. Partial corrupt content is never sampled.

## Script API

Luau and JavaScript receive the same generated classes and names.

### Camera API

Planned camera surface:

- `workspace.CurrentCamera` remains the primary-view compatibility property;
- `Camera.CFrame`, `FieldOfView`, clipping, image size, focus, aperture, and
  exposure properties;
- `Camera:WorldToViewportPoint` and `ViewportPointToRay` use an explicit view
  size or primary view, with clear behind-camera status;
- `CameraRig` properties and typed parameter setters;
- `CameraService:GetPrimaryView()`;
- `CameraService:GetView(name)`;
- `CameraService:PushCamera(cameraOrRig, options)` returns a claim handle;
- `CameraService:ReleaseCamera(claim)`;
- `CameraService:Shake(request)` returns a local presentation handle;
- `CameraService:GetDiagnostics()` returns copied bounded facts.

Claim handles are instances or stable opaque script handles with deterministic
teardown. They do not expose C++ pointers.

### Sequence API

Planned sequence surface:

- `SequencePlayer.Sequence`;
- `SequencePlayer.TimePosition`;
- `SequencePlayer.PlaybackSpeed`;
- `SequencePlayer.Playing`, `Paused`, `Looped`, and `Status`;
- `SequencePlayer:SetBinding(slot, instance)`;
- `SequencePlayer:GetBinding(slot)`;
- `SequencePlayer:Play(options)`;
- `SequencePlayer:Pause()`;
- `SequencePlayer:Stop(options)`;
- `SequencePlayer:Seek(time, policy)`;
- `SequencePlayer:Skip(marker)`;
- marker, finished, blocked, and presentation-event signals;
- authority-only methods clearly refused on clients.

Script time values are seconds at the ergonomic boundary. The implementation
converts to integer sequence ticks with an explicit rounding rule. Studio and
content APIs may also expose exact frame and tick forms.

### Procedural authoring

A `SequenceBuffer` may mirror the `AnimationBuffer` pattern:

- scripts add typed tracks, bindings, keys, markers, and events through checked
  builders;
- `Bake()` validates and emits canonical compiled bytes;
- a `CinematicSequence` may point to a world-owned buffer or a published asset;
- revision changes invalidate decoded caches once;
- maximum bytes, tracks, keys, and nesting are enforced before allocation.

This supports procedural camera rails and generated cinematics without exposing
the compiled binary layout as a table scripts must construct by hand.

## Studio sequencer

### Main layout

Add a `Sequencer` dock widget with:

- sequence selector and active player selector;
- transport controls;
- current time, display rate, and playback range;
- hierarchical binding and track list;
- shot strip for camera cuts and blends;
- keyframe timeline;
- curve editor;
- event and marker lanes;
- diagnostics and status row;
- camera preview selector;
- record and auto-key controls.

The UI follows the project visual rules: true black background, white primary
text, dense rows, no decorative cards, and no continuously repainting animation.
A moving playhead redraws only while playback is active or the user scrubs.

### Editing model

Edits operate on a Studio sequence document with stable ids. Undo and redo are
document operations, not snapshots of the whole world. Multi-selection, ripple
edits, box selection, snapping, tangent editing, duplication, and clipboard
operations preserve stable order.

The editor compiles an immutable preview artifact after a debounce or explicit
commit. Runtime preview swaps revisions atomically. A compile failure keeps the
last valid preview and marks the failing track and key.

Studio must not rewrite a camera property every frame merely because a derived
preview value differs. The existing `ViewerLensToWrite` ownership rule applies:
authored values change only through explicit edits, recording, or auto-key.

### Recording and auto-key

Recording samples selected camera or property outputs at a fixed declared
sample rate into a temporary take. Stopping recording:

1. validates finite samples;
2. removes redundant keys within authored tolerances;
3. preserves cuts and marked frames;
4. writes one undoable document edit;
5. compiles the new preview revision.

Recording never captures a value by reading a render matrix back from the GPU.
It samples the copied rig or property output before projection.

Auto-key writes only armed tracks. Moving an object with no armed track remains
an ordinary world edit. This avoids accidental keys from every property change.

### Preview isolation

Studio preview has a mode:

- `PresentationOnly`, the default;
- `PreviewWorld`, using a disposable cloned world;
- `LiveAuthority`, available only during an explicit play session with warning
  and normal authority rules.

Scrubbing the edited world never fires gameplay events. Audio and VFX previews
use isolated generations that are cancelled on stop, seek, tab hide, document
close, and world switch.

### Curves, shots, and diagnostics

The curve editor displays exact key values and tangents. It can isolate channels,
frame selection, snap to keys, and show discontinuities. It does not evaluate a
separate curve implementation from runtime. Preview samples the compiled runtime
sampler.

The shot strip shows cuts, blends, missing cameras, binding failures, unsupported
effects, nested depth, and render cost hints. Diagnostics link to the exact
track, clip, key, or binding and include a stable code suitable for tests.

### Node graph relationship

The sequencer is a timeline, not a node graph. The existing Studio `nodegraph`
may later edit reusable camera rig flow or an automatic director decision graph.
Those graph outputs compile into a rig or director asset and appear as one
sequence source. Timeline clips and keyframes do not get represented as hundreds
of graph nodes.

## Caching and work scheduling

### Cache keys

Cache only derived data:

- compiled sequence by content hash and format version;
- decoded curves by asset and revision;
- binding resolution by player revision and hierarchy revision;
- interval cursor by sequence instance and playback generation;
- target-independent camera output by sample tick, interpolation fraction,
  binding revision, rig revision, and modifier revision;
- target matrices by camera output revision, target size, projection mode, and
  clip override;
- Studio waveform, thumbnail, and curve display data by asset revision;
- depth-of-field and temporal histories by view generation.

Changing a subject transform invalidates only outputs that bind it. Changing
one key invalidates its compiled sequence revision. Resizing a viewport
invalidates target matrices and render targets, not rig evaluation or sequence
curves.

### Visibility and throttling

A view is eligible only when its consumer is open, visible, non-zero in size,
within its update rate, and inside the declared budget. A hidden Studio tab,
closed menu, clipped `ViewportFrame`, occluded portal, and unused thumbnail do
not evaluate or render.

Throttling may skip visual camera samples for secondary views. It may not skip
authoritative playhead advance, gameplay event commitment, sequence completion,
or required audio scheduling. A secondary view sampled later evaluates the
current state directly rather than replaying missed frames.

### Parallel work

Independent curve blocks, bindings, and view outputs may be evaluated through
fork-joined jobs after profiling shows a crossover. Jobs read immutable input
and write disjoint output slots. Publication waits for all required outputs.

Do not dispatch one job per key, track, or camera by default. Small sequences are
faster on the owner thread. Record the measured release-build crossover beside
any threshold.

Asynchronous content decode, waveform generation, and thumbnail rendering are
authoring work. They carry cancellation, revision, and generation ids. A late
result from an old revision is discarded rather than installed.

## Failure handling and diagnostics

### Runtime statuses

Use stable status and failure names, including:

- `Ready`;
- `Playing`;
- `Paused`;
- `Stopped`;
- `Blocked`;
- `MissingAsset`;
- `ContentMismatch`;
- `MissingBinding`;
- `InvalidBinding`;
- `UnsupportedTrack`;
- `InvalidCurve`;
- `CycleDetected`;
- `NestingLimit`;
- `ViewBudgetExceeded`;
- `RenderTargetUnavailable`;
- `AuthorityDenied`;
- `ReverseUnsupported`;
- `Cancelled`.

Failures include sequence name, player instance, track name, binding slot, view
name, revision, and a bounded message. Logs use stable ids so a client, server,
and Studio session can correlate one playback.

### Fallback rules

- invalid active camera: fall back to the gameplay camera, then a safe fixed
  camera, then render no 3D view;
- destroyed subject: keep the last valid frame for one sample, then apply the
  rig's missing-subject policy;
- missing sequence asset: keep gameplay camera and expose `MissingAsset`;
- missing required binding: block before claiming the camera;
- failed optional track: disable the track and continue with a diagnostic;
- invalid lens: use clamped safe defaults and report the source value;
- unavailable depth of field: render sharp;
- exhausted secondary-view budget: preserve the previous image or fallback tint
  according to the consumer;
- script failure in a presentation event: isolate that handler, release its
  temporary commands, and continue playback;
- authority failure: reject the command without changing playhead or claims.

No failure path leaves a camera stack claim, audio generation, temporary render
target, or Studio preview worker alive after its owner ends.

## Migration from current camera paths

Migration stays buildable after every phase.

1. Characterise current `Camera`, `ActiveCamera`, `CameraController`, portal,
   `ViewportFrame`, Studio viewport, and listener behaviour with focused tests.
2. Introduce `CameraOutput` and make the current controller produce it without
   changing visible behaviour.
3. Route the main client view through an explicit primary `CameraView`, keeping
   `ActiveCamera` as its compatibility projection.
4. Move current classic, first-person, shift-lock, and scriptable placement into
   built-in rig evaluators.
5. Move player subject following into the Players client binding and remove the
   character-to-player camera dependency.
6. Change `ViewportFrame` and Studio viewports to select explicit views without
   mutating primary active state.
7. Pass explicit source camera output into surface and portal aiming, then remove
   hidden primary-view reads from secondary view construction.
8. Add camera claims and deterministic blends. Replace every direct scriptable
   ownership flag with a claim owner and teardown path.
9. Add compiled sequence assets and a headless player with camera cuts only.
10. Add camera property, animation, audio, VFX, and typed event tracks one at a
    time, with event interval tests before each new side effect.
11. Add Studio sequencer editing against the same compiled sampler.
12. Add replay and spectator adapters after explicit views and claims are stable.
13. Add depth of field and physical-lens controls behind render profile support.
14. Delete old placement, callback, timeline, and direct camera writers as their
    callers move. Do not preserve two public paths indefinitely.

Saved worlds load old camera fields through a versioned adapter. The adapter
creates the equivalent fixed or gameplay rig data, logs a bounded migration
notice, and saves only the new form on the next explicit author save.

## Implementation phases and gates

### Phase 0: contracts and measurements

- inventory every current camera transform writer and active camera reader;
- record baseline main-view, Studio, `ViewportFrame`, mirror, and portal images;
- measure camera CPU cost and per-view GPU cost in release;
- define `CameraOutput`, view purpose, failure codes, and ownership rules;
- add architecture and serialization assertions for new rows.

Gate: current behaviour is covered well enough to move one writer at a time.

### Phase 1: explicit views and rig core

- add primary `CameraView`;
- add target-independent `CameraOutput`;
- implement fixed, follow, orbit, first-person, shoulder, and free rigs;
- adapt current controls and Players possession binding;
- preserve portal transit and camera collision;
- add deterministic damping and reset rules.

Gate: gameplay and Studio camera images match the baseline within approved
tolerances, and headless placement tests pass at varied frame rates.

### Phase 2: claims, blends, and shake

- add bounded camera claim stack;
- add cut and blend runtime;
- add deterministic seeded shake;
- add comfort scaling;
- expose script claim handles and teardown diagnostics.

Gate: interruption, owner destruction, world switching, and nested claims never
leave a stale camera owner.

### Phase 3: multi-view integration

- move `ViewportFrame`, Studio panels, thumbnails, split views, and captures to
  explicit view selection;
- pass source outputs to mirrors and portals;
- add view-family budgets, visibility checks, and cache generations;
- choose one audio-listener view explicitly.

Gate: hidden and over-budget views do no camera or GPU work, while visible
portal and viewport images remain correct.

### Phase 4: sequence runtime

- define and validate compiled sequence format;
- add `CinematicSequence`, `SequenceBuffer`, and `SequencePlayer`;
- implement integer time, camera cuts, camera properties, bindings, nested
  sequences, and playback rules;
- implement authoritative versus presentation event queues;
- add save, replication, and content hash handling.

Gate: two machines and a replay commit identical authoritative event ids, while
arbitrary render sample counts do not change gameplay fingerprints.

### Phase 5: media and script tracks

- integrate existing animation tracks;
- add audio scheduling and seek generations;
- add VFX presentation commands;
- add typed property and script events;
- implement skip and reverse capability checks.

Gate: play, loop, seek, skip, and reverse satisfy the event endpoint rules and
leave no stale media commands.

### Phase 6: Studio sequencer

- build timeline, shot strip, track list, transport, curve editor, markers, and
  diagnostics;
- add undo, clipboard, snapping, recording, key reduction, and auto-key;
- add isolated preview worlds and preview command generations;
- add asset compile, publish, and dependency inspection.

Gate: editing is undoable, failed compiles keep the last valid preview, hidden
tabs stop work, and scrubbing never mutates live gameplay.

### Phase 7: replay, spectator, and director

- add replay view controls and bookmark recording;
- add allowed spectator modes and replication-interest policy;
- add fixed authored camera selection;
- add a bounded automatic director only after manual modes are stable.

Gate: replay seeks are deterministic, spectators cannot expand hidden state, and
director decisions use stable tie-breaks.

### Phase 8: lenses and release hardening

- add focal-length authoring, focus binding, autofocus, aperture, and exposure;
- integrate depth of field with render profiles and secondary-view defaults;
- profile CPU, GPU, memory, target churn, and script cost;
- run platform image suites and long sequence soaks;
- document final public surfaces through generated references.

Gate: release targets stay within agreed budgets and unsupported backends fall
back without breaking shots.

## Test plan

### Deterministic headless tests

Cover:

- every built-in rig at known subject frames;
- camera placement at zero delta, varied frame cadence, and long elapsed times;
- damping equivalence for different presentation frame partitions;
- blend endpoints, midpoints, interruptions, cuts, and destroyed sources;
- quaternion shortest-path interpolation;
- field-of-view and focal-length conversion;
- constraint limits and unsatisfiable fallback;
- collision correction using fixed query results;
- deterministic shake at known seed and sample times;
- binding validation and missing optional bindings;
- integer sequence time conversion and rational display rates;
- step, linear, cubic, vector, and quaternion curves;
- nested sequence time mapping and depth limit;
- forward, reverse, loop, seek, scrub, and skip event intervals;
- equal-time event ordering;
- authoritative event deduplication across reconnect and resync;
- render sampling a time zero, one, and many times without changing gameplay
  fingerprint;
- presentation callbacks refused when they try to mutate gameplay during
  sampling;
- animation track sync and marker alignment;
- audio and VFX generation cancellation on seek;
- save and load round trips;
- corrupt, oversized, cyclic, and version-mismatched content;
- replication permission and content hash mismatch;
- replay seek by snapshot plus forward barrier replay;
- spectator visibility and interest restrictions;
- cache invalidation by rig, binding, subject, target size, and sequence revision;
- teardown of claims, workers, audio generations, and targets.

Tests use exact frames and integer ticks where possible. Floating comparisons
state tolerances based on the operation, not one broad epsilon.

### Integration tests

Add focused integration scenes for:

- local possession swap while a cinematic claim is active;
- blend from gameplay to a rail shot and back;
- two split-screen primary views with different cameras;
- `ViewportFrame` showing a camera without changing the main view;
- a cinematic seen through a mirror and a recursive portal;
- sequence camera plus character animation, sound, and VFX marker;
- server-authoritative skip with two clients;
- missing client content fallback;
- replay follow, free, and fixed cameras;
- Studio edit, compile, preview, undo, and reopen;
- hidden Studio sequencer and viewport tabs doing no work;
- device or render-target loss during a shot.

Prefer these meaningful cross-system tests over a large set of shallow smoke
tests.

### Image tests

Capture deterministic images for:

- fixed, orbit, shoulder, first-person, and rail rigs;
- cut and several blend fractions;
- look-at and screen composition near limits;
- collision pull-forward and occluder fade;
- focal-length conversion at matched framing;
- near and far depth-of-field focus targets;
- `ViewportFrame` at two aspect ratios;
- mirror and portal views with cinematic cameras;
- recursive portal clipping;
- split-screen target rectangles;
- sequence seek to an exact frame;
- Studio camera preview and shot thumbnails.

Image tests pin backend, resolution, render profile, scene asset hashes, exposure,
random seed, and temporal-history reset. Compare exact structural buffers where
possible, such as depth and object ids, and use bounded perceptual thresholds for
shaded colour. Store an approved failure diff rather than increasing a tolerance
until a regression disappears.

### Fuzz and property tests

Fuzz compiled sequence parsing, nested references, curve keys, event ranges,
binding maps, and script builder inputs. Assert bounded allocation, no NaNs, no
out-of-range reads, stable error codes, and no partial publication.

Property tests cover monotonic curve interval lookup, blend endpoint identity,
seek idempotence, loop event counts, time conversion round trips, and equivalent
frame partitions for analytic damping.

### Sanitizers and soak tests

Run address, undefined-behaviour, and thread sanitizers over headless evaluation,
content replacement, cancellation, and Studio compile jobs. Soak long looping
sequences with repeated world switches, view creation, portal recursion, seeks,
and asset revisions. Watch live allocations, target counts, audio voices, cache
entries, and script callback generations for a sustained slope.

## Profiling and release gates

Profile release builds with named scene, backend, resolution, view count, portal
depth, sequence size, track count, and active effects.

Required CPU scopes include:

- sequence advance;
- authoritative event scan;
- binding resolution;
- curve sampling;
- rig evaluation;
- camera collision query gather and reduce;
- camera modifier stack;
- view-family build;
- surface-camera aiming;
- Studio sequencer compile and draw;
- script presentation dispatch.

Required counters and gauges include:

- active views by purpose;
- evaluated and cache-hit camera outputs;
- sampled tracks, curves, and keys;
- committed and deduplicated events;
- active camera claims and shakes;
- collision queries and overflow;
- secondary views skipped by visibility, rate, and budget;
- render targets created, reused, and live bytes;
- depth-of-field pixels and passes;
- audio and VFX commands scheduled and cancelled;
- sequence decode bytes and cache residency;
- Studio visible rows, keys, and compile revisions.

Required GPU measurements separate main view, each secondary-view class, portal
recursion, depth of field, and post-process work. Report logical target bytes,
peak live bytes, cumulative allocation, and resource creation count. A flat
live heap with high churn is still a camera-system problem.

Initial release gates are measured before assigning numeric budgets. At minimum:

- one ordinary gameplay camera must add no allocation per frame;
- a static camera and paused sequence must hit caches and do no curve work;
- a hidden Studio sequencer or viewport must do no sampling or rendering;
- changing target size must not recompile sequence or rig data;
- secondary-view cost scales with views actually submitted, not cameras present
  in the world;
- camera evaluation must not introduce a tick-late gameplay result;
- authoritative fingerprints must match with different render frame counts;
- long soaks show no credible live-memory or resource-count slope.

Only parallelise after profiles show the release-build crossover. Record the
threshold and hardware beside the decision.

## Security and abuse limits

- validate client camera and spectator requests against allowed modes, subjects,
  teams, delays, and volumes;
- never expand replication interest solely because a local camera moved;
- cap sequence bytes, tracks, keys, events, nested depth, bindings, claims,
  shakes, views, captures, and callbacks;
- allowlist properties a sequence may animate;
- separate presentation events from authoritative gameplay requests in types and
  permissions;
- refuse filesystem paths, shader source, arbitrary native calls, and closures
  inside sequence content;
- rate-limit remote sequence starts, skips, spectator changes, and captures;
- verify content hashes before playback;
- discard stale asynchronous results by world, asset, revision, and generation;
- make Studio live-authority preview an explicit mode rather than the default.

Before implementation ships, audit sequence payloads, spectator visibility,
script event permissions, and capture APIs. Add any unresolved findings to the
active roadmap rather than hiding them in this plan.

## Explicit non-goals

The first complete system does not include:

- a video editor or non-linear media compositor;
- encoded video playback or export codecs;
- full film-production colour grading and raw camera models;
- cloth, facial, or body animation owned by the camera system;
- motion matching for cameras;
- machine-learned automatic directing;
- unbounded arbitrary script callbacks per keyframe;
- gameplay rewind by calling inverse events;
- per-portal gameplay worlds advanced by render sampling;
- one audio listener mixed independently for every split-screen view;
- a universal graph virtual machine;
- orthographic cameras before all consumers support them;
- ray-traced depth of field as a requirement;
- network replication of per-frame camera matrices;
- preserving every current direct camera writer after migration.

These may become later plans if real games require them. Their seams are stable
view selection, typed tracks, copied camera outputs, registered presentation
commands, and immutable sequence content.

## Completion definition

The camera and cinematics system is complete when:

- gameplay, cinematic, replay, spectator, Studio, viewport, mirror, and portal
  views use one camera output and target-resolution path;
- current controller behaviour is represented by built-in rigs;
- each view selects its camera explicitly;
- claims, cuts, blends, collision, damping, and seeded shake have deterministic
  semantics and complete teardown;
- sequences use integer time, typed tracks, stable bindings, bounded content,
  and exact event interval rules;
- render sampling and Studio scrubbing cannot mutate gameplay;
- animation, audio, VFX, and scripts stay synchronised across play, pause, seek,
  loop, skip, and supported reverse playback;
- authoritative sequence state saves, replicates, reconnects, and replays;
- Studio can author, compile, preview, record, undo, diagnose, and publish a
  sequence without a second runtime sampler;
- hidden or over-budget views perform no unnecessary work;
- generated Luau and JavaScript bindings agree;
- focused headless, integration, image, fuzz, sanitizer, and soak tests pass;
- release profiles name CPU, GPU, allocation, and view-scaling costs;
- legacy direct writers and replaced timeline paths are deleted;
- comments, component purposes, architecture graph, generated docs, and roadmap
  entries match the shipped design.
