# VR system plan

## Status

This document describes future work. It does not claim that the engine has an XR runtime,
headset support, eye tracking, optical focus control, or a VR input path today. The Rift S is a
requested validation target. Its current runtime availability and compatibility must be checked
when implementation begins.

The plan separates headset integration, per-eye rendering, player intent, comfort, local vision
preferences, and shared-room safety. A client adapter owns XR session and device details. Shared
engine code receives copied poses, view descriptions, and action samples. A headless server has
no headset or runtime dependency.

Far Cry VR and Warhammer 40,000: Space Marine VR - Defenders of Avarax at Zero Latency Adelaide
are reference experiences supplied for this plan. First-person POVs are expected later and have
not been reviewed here. Official pages describe group play and safety guidance for Far Cry VR,
and squad encounters and difficulty choices for Space Marine VR. These are study references, not
claims about this engine or verified Adelaide venue inventory ([Far Cry VR game information](https://zerolatencyvr.zendesk.com/hc/en-us/articles/360003503315-Far-Cry-VR-Game-Information), [Space Marine VR standard mode information](https://zerolatencyvr.zendesk.com/hc/en-us/articles/10613665200527-Space-Marine-VR-Standard-Mode-Game-Information)).

## Product goal

Build a comfortable, inspectable VR path that uses the engine's existing world, camera, input,
character, audio, physics, and rendering systems.

The system should support:

- headset session and frame lifecycle through a client platform adapter;
- runtime-provided view pose and asymmetric projection parameters per eye;
- head, controller, tracker, and hand data translated into the common input and character paths;
- arbitrary input devices and pose sources through capability-reporting provider adapters;
- positional body and limb tracking, plus optical or glove hand joints mapped onto authored character rigs;
- configurable comfort and accessibility options;
- optional per-eye perceived-focus adjustments for users who need them;
- a separate, clearly named virtual magnifier for interface or world content;
- profile reset, calibration preview, and safe return to the game;
- shared-room sessions with physical origin and boundary safety rules;
- headless authoritative simulation and copied network messages;
- a small multiplayer experience that exercises movement, aim, audio, and recovery from tracking
  or session interruptions.

VR presentation does not own gameplay authority. The server commits gameplay on fixed ticks from
validated action intent. The client may predict visual motion and late-latch view pose for
rendering, but render-time pose updates do not write authoritative transforms or bypass movement
validation.

## Existing foundation and references

Extend the camera, input, character, navigation, audio, and session paths linked below.

| Plan | VR connection |
|---|---|
| [Camera and cinematics](camera-and-cinematics.md) | Per-view camera output, tracked head pose, lens ownership, and comfort-aware transitions |
| [Input system](input-system.md) | XR action samples, focus loss, rebinding, and fixed-tick intent |
| [Virtual device service](virtual-device-service.md) | Bounded external and scripted device ingress through the shared device and action path |
| [Character system](character-system.md) | Character intent, possession, aim, and locomotion |
| [Navigation AI](navigation-ai-system.md) | Shared-room agents and bounded collision queries |
| [Audio system](audio-system.md) | Listener pose, spatial audio, and copied presentation state |
| [Session and social](session-and-social.md) | Joining, leaving, copied player state, and recovery |

OpenXR supplies view poses and projection fields through `xrLocateViews`. Use those per-view
values when building and submitting projection views. Keep the poses and FOV data coherent with
the matrices and layer metadata for that frame ([OpenXR rendering specification](https://github.com/KhronosGroup/OpenXR-Docs/blob/main/specification/sources/chapters/rendering.adoc)).

OpenXR core provides view and projection state, not a general setter for a headset's physical
lens focus. Treat physical per-eye focus as a vendor or device capability only when a runtime
exposes it. Do not imply all headsets support it or invent an OpenXR core call.

## Non-negotiable design rules

1. XR runtime headers and handles stay behind the client platform adapter.
2. No headset, controller, or runtime pointer crosses a world boundary.
3. The server and headless tools can run without an XR runtime or GPU.
4. Runtime view poses and FOV values are the baseline for each submitted view.
5. Submitted projection metadata must describe the projection used to render.
6. Per-eye preferences are local presentation settings, never game authority.
7. A game cannot silently change a player's vision profile.
8. User scale controls are not prescription or diopter measurements.
9. The runtime boundary is safety input, not a visual avatar or collision mesh.
10. Recenter and origin changes are explicit, versioned session events.
11. Tracking loss and session focus loss release held actions exactly once.
12. Render prediction never commits simulation state.
13. Query and report device features; unsupported options say unavailable.
14. Calibration and comfort checks must work before gameplay.
15. Bound every queue, device set, view family, and action sample.

## Ownership and layer placement

Start with a client adapter around the selected XR runtime. Keep reusable view, input, and
player-intent data in existing engine layers where their current owners fit. Do not add an
engine XR module before implementation has a concrete dependency set. If new modules or
cross-layer edges are needed, update and pass the repository architecture graph checks in the
implementation change.

| Owner | Responsibility |
|---|---|
| Client platform adapter | Runtime discovery, instance/session lifecycle, frame timing, event pump, swapchains, and device capability reporting |
| `scene` and camera path | Copied tracked poses, per-view camera inputs, and presentation configuration |
| Input | XR action mapping, local device handles, focus-loss release, and fixed-tick samples |
| Character and physics | Validated locomotion intent, semantic tracking targets, rig and pose integration, collision, ground state, and authoritative movement |
| Render | Per-eye target setup, projection, submission, and diagnostics |
| Audio | Listener pose and spatial output from the final presentation view |
| Server | Fixed-tick authority, intent validation, room membership, and copied state |
| Studio | Optional authoring and diagnostics; no runtime ownership |

Create the instance, session, reference space, and supported swapchains after
discovering a compatible runtime and view configuration. Session start and stop
are event-driven. Continue polling runtime events while idle. On `READY`, call
`xrBeginSession` before entering the frame loop. On `STOPPING`, leave that loop,
call `xrEndSession`, then return to event polling. On `EXITING` or
`LOSS_PENDING`, unwind session resources and exit or recover according to the
reported state ([OpenXR 1.1 specification](https://registry.khronos.org/OpenXR/specs/1.1-khr/html/xrspec.html)).

Inside the active frame loop:

1. wait for frame timing and begin the frame;
2. if rendering is requested, locate view poses and FOV for the predicted display time;
3. acquire and wait for images only when rendering is requested;
4. render and prepare projection views using the same pose/FOV contract, then complete GPU
   access according to the graphics binding;
5. release acquired images, then end the frame. If rendering is not requested, skip image work
   and still end the frame;
6. return to event polling before the next frame and handle focus, exit, or device loss.

Every failure path must release acquired resources and move the session to a known state. The
exact API calls and graphics binding belong in the adapter implementation after runtime and
platform support are verified.

## Device and tracking source contract

Treat a device, a tracked source, a semantic body role, and a character bone as four different
things. A device is an input endpoint that may expose buttons, axes, haptics, a pose, or a joint
set. A `TrackedSource` is one logical pose or joint-set endpoint. Its identity does not imply that
it is a waist, hand, or any other body part. A semantic role is assigned by local calibration. A
character bone name comes from the authored rig profile.

Keep XR runtime handles, vendor objects, and operating-system device identifiers inside the
client provider. `InputDevice` retains a generation-checked local handle for input routing; a
disconnect invalidates that generation, so queued samples for the old handle are rejected.
`TrackedSource` identity is local and opaque outside the provider. Never serialize device handles,
runtime paths, hardware serials, or declaration-order numbers as durable identities.

The common input and tracking path allows zero, one, or many devices, with independent capability
sets. It does not assume two controllers, left and right slots, gamepad slot numbers, or a vendor
role enumeration. It should accommodate gamepads and raw joysticks, 3DoF and 6DoF controllers,
pose-only trackers, gloves and optical hand tracking, optional body skeletons, tracked props, and
cameras. A custom provider may offer any subset and can operate without an HMD when that provider
supports the configuration. Input and output capabilities are separate; haptics or other output
are available only when reported by the provider and requested by a supported action.

OpenXR action paths and extensions expose only devices and features implemented by the active
runtime. For example, the HTC tracker extension has runtime-specific persistent and role paths,
and a role may be unset or change during an instance. Its roles do not define a universal tracker
contract ([HTC Vive Tracker interaction extension](https://github.com/KhronosGroup/OpenXR-Docs/blob/main/specification/sources/chapters/extensions/htcx/htcx_vive_tracker_interaction.adoc)).
OpenXR hand tracking can expose individual joint locations when the runtime supports the
extension ([OpenXR hand tracking extension](https://github.com/KhronosGroup/OpenXR-Docs/blob/main/specification/sources/chapters/extensions/ext/ext_hand_tracking.adoc)).
The FB body tracking extension describes an optional provider-specific skeleton path
([FB body tracking extension](https://github.com/KhronosGroup/OpenXR-Docs/blob/main/specification/sources/chapters/extensions/fb/fb_body_tracking.adoc)).
These references establish examples of capability-shaped integration, not a promise that every
runtime exposes them. USB presence alone does not provide universal pose discovery. A device
without an implemented provider is unavailable and must be reported as such.

Providers publish bounded inventories: maximum device count, source count per device, joint
count per source, action count, and sample size. Define limits in provider configuration and
reject or explicitly truncate overflow with a diagnostic. Unknown feature identifiers remain
unknown and unavailable; do not reinterpret them as a nearby known feature. A provider adapter
normalizes its reports and copies samples into the common contract. Headless tests use a
simulated provider through the same seam, with no runtime library or GPU dependency.

Represent each tracking read as a copied, bounded `TrackingSample`. It contains source identity,
source schema revision, sample sequence, provider timestamp, converted engine timestamp, origin
generation, and either one pose or a bounded set of named joints. Each pose or joint carries its
own position-valid, orientation-valid, position-tracked, and orientation-tracked flags. Confidence
and linear or angular velocity are optional fields with explicit availability. A provider that
does not supply them leaves them absent; the adapter does not estimate values and label them as
measured.

Transforms use the engine's declared coordinate convention and units after provider conversion.
Accept only finite positions and finite, normalized orientations. Joint names are stable strings
within a documented provider schema, never wire ordinals. Skeleton schema revision and source
revision change when the provider's meaning or joint set changes. Samples use a monotonic provider
clock converted through an explicit clock mapping, plus a sequence number to detect stale,
duplicate, or reordered data. Do not compare timestamps from different clock domains before
conversion.

Every source declares its coordinate space, handedness, units, and origin generation. The adapter
converts positions and orientations into the session's selected tracking space before publishing
samples. Two sources may be combined only when their coordinate transforms, clocks, and origin
generations are known to agree. Otherwise the source remains uncalibrated and cannot contribute to
the same character solution. Recenter or origin remap publishes one coherent generation change;
old-generation samples are rejected, and any device mount calibration tied to the old origin is
marked for review or invalidated according to its declared reference space.

Device discovery and reassignment are explicit state changes. On disconnect, invalidate the local
handle generation and mark its sources unavailable immediately. On reconnect, a reused runtime
role or device path is not enough to silently restore an ambiguous assignment. Preserve local
calibration only when its stable provider identity and schema still match; otherwise require the
user to confirm the source. Let users swap or clear role assignments, then bump the calibration
revision. Multiple local people use separate session-scoped inventories, calibration state,
origins, and sample queues; never share a role assignment implicitly.

## Luau providers and computer vision

Luau-defined controllers are a normal production input source. LocalScripts
can declare virtual buttons, axes, poses, and named joint streams and publish
them through the [input plan's virtual controller API](input-system.md#luau-defined-virtual-controllers).
The host owns producer lifetime, source registration, and validation. The
script owns how its declared controls are computed. Published values use the
same action maps, calibration, fixed-tick sampling, and character rig mapping
as native devices. A pure Luau or recorded provider requires no headset,
camera, OpenXR runtime, or inference dependency.

Virtual controller creation must be useful before a computer vision backend
ships. Start with a procedural hand or replayed landmark fixture controlled by
a Luau module. Use it to exercise pointing, pinch/grab actions, finger posing,
source selection, and partial loss. This is a production extension point as
well as a headless test seam. Scripts that already produce semantic character
intent can continue using that path directly.

### Optional vision adapters and built-in libraries

Keep capture, inference, and input publishing separate:

```text
camera frames or recorded input
  optional native vision backend -> timestamped landmarks and model metadata
  Luau library -> joint normalization, calibration, filtering, and gestures
  virtual controller Publish -> action and tracking input barriers
  semantic intent and targets -> character controls and rig pose
```

Provide versioned first-party Luau modules for common controller composition,
named hand landmarks, joint-frame construction, gesture thresholds and
hysteresis, calibration, and recorded/synthetic feeds. They accept ordinary
landmark data, so a project can replace a model or supply its own detector.
Filtering exposes its added latency and resets on source loss or origin
change. Degenerate landmark segments produce unavailable orientations rather
than non-finite joint frames. The shared producer contract also supports
JavaScript consumers without duplicating tracking storage or solving policy.

Offer camera capture and hand/body inference through optional engine adapters
with thin script wrappers. Native inference and capture dependencies belong
behind engine interfaces, not in character code or public vendor types. A
project can use only the pure Luau helpers, only a native tracker, or both.
Verify backend platform support, model and library licenses, distribution,
memory cost, and measured latency before selecting a built-in backend. This
plan does not claim an inference library or a model is already integrated.

MediaPipe Hand Landmarker is one candidate to evaluate, not a selected engine
dependency. Its documented inputs include images, video, and live streams, and
its output includes hand landmarks and handedness
([Google's Hand Landmarker guide](https://developers.google.com/edge/mediapipe/solutions/vision/hand_landmarker)).
Adapters must describe the actual model's coordinate frame, scale, confidence
semantics, and timestamp behavior instead of passing its output through as a
runtime-tracked world pose.

Image-space landmarks can drive pointer or gesture controls. Local 3D hand
shape can drive finger articulation without establishing a wrist's position
in the room. A world-space wrist or body root needs an explicit calibrated
positioning method or another aligned source. Declare inferred channels and
unavailable channels honestly. Do not treat a library's use of the word
"world" as evidence of alignment with the engine's tracking origin. Mirroring,
handedness, and detection-to-person association are explicit adapter policies;
detector list order never reassigns one person's hands to another.

Camera access follows the host's camera capability and permission policy.
Creating a virtual controller does not grant camera access or start capture.
Raw frames and model intermediates remain local unless a separate feature
explicitly needs them; normal avatar replication carries semantic targets.
Stopping capture or losing access invalidates the affected sources and
cancels dependent controls through the ordinary loss path.

Capture and inference are external input work. They must not block the fixed
simulation tick or keep a simulation job running across ticks. Completed
results enter a bounded owner-thread inbox with capture time, completion time,
sequence, and source revision. Discard stale or out-of-order results before
publication. The next input barrier accepts a complete snapshot; late results
never alter a consumed tick. Record the accepted semantic tick samples for
replay rather than expecting inference to finish identically on every machine.

## Mapping tracked sources to character rigs

Keep reusable rig authoring separate from per-player calibration. The planned `TrackingRigProfile`
references the `RigRetargetProfile` described by the future character foundation for semantic
joint-to-bone and rest-pose mapping. It adds named tracking slots, target-chain and constraint
policy, masks, weights, and tracking-specific reference corrections. It does not declare a
competing skeleton or a second set of bone names. Its revision changes when slot names, chain
policies, masks, weights, or corrections change.

Local calibration assigns available `TrackedSource` endpoints to named tracking slots and stores
mount offsets and the accepted tracking reference. It does not edit the author profile or infer a
semantic role from a controller label. Store calibration locally with a revision and fingerprints
for provider identity, source schema, rig profile, and origin generation. Do not put personal
dimensions or exact mount offsets into shared telemetry.

Calibration should let a user assign or confirm waist, chest, elbows, knees, feet, and wrists,
including multiple unlabelled sources. Show the source pose and expected virtual slot together.
For a mounted tracker, capture its offset from the intended body segment, reference pose, axes,
and rest orientation. Ask the user to stand or sit in a known reference pose, perform a short
verification movement, then explicitly accept, cancel, or reset. Keep mount correction, player
proportions, virtual locomotion scale, and world optical scale as separate values. Revalidate
after a rig, device, provider schema, or origin change; never quietly apply stale calibration.

An example mapping is a guide for profile authors, not an engine-wide skeleton naming convention:

| Input source | Semantic target | Example rig target |
|---|---|---|
| Headset head pose | Head | `Head` or head chain in `RigRetargetProfile` |
| Waist tracker | Pelvis | `Hips` or pelvis chain in `RigRetargetProfile` |
| Foot tracker | Foot position constraint | Ankle or foot IK target |
| Wrist tracker or controller grip pose | Wrist and hand | Wrist joint plus hand target |
| Optical or glove finger joints | Finger joint targets | Named thumb and finger chains |

The planned character foundation consumes neutral semantic targets through `TrackingBinding` and
the planned `RigRetargetProfile`, `BoneController` / `BoneControl`, `PoseBuffer`, and `PoseBinding`
path. These names describe future interfaces in [the character system plan](character-system.md),
not implemented engine features. `TrackingBinding` owns the inspectable enable state, weight, and
profile reference. It never consumes provider handles. The adapter and calibration layer produce
semantic targets; the character owns how its rig, animation layers, and constraints use them. This
plan adds no parallel pose storage system.

Sparse tracking does not uniquely determine a full skeleton. A head and two controllers cannot
recover shoulders, spine, elbows, legs, and fingers without assumptions. Resolve available
targets with bounded inverse kinematics, authored chain lengths, anatomical limits, and declared
fallback animation. Define source precedence, masks, weights, and blend behavior in the rig
profile or character configuration. Resolve ties deterministically, and expose the chosen source
and validity in diagnostics. Missing sources leave their targets unconstrained or use an explicit
animation fallback; they do not create inferred authoritative motion.

Hand tracking is an optional richer input than a grip pose. When named finger joints are
available, map them onto matching rig chains through the same semantic target path. When only a
controller pose or a provider's hand aim is available, use a declared grip or hand pose fallback.
Do not fabricate individual fingers from a pose and mark them tracked. Skeletons with fewer or
different joints remain usable through explicit profile mappings and bounded interpolation.

## Tracking calibration and loss behavior

Calibration UI should list detected devices and capabilities, then guide assignment without
assuming role labels exist. A user can confirm automatic suggestions, assign a source manually,
swap two assignments, or leave a slot unassigned. Require deliberate confirmation when a
reconnect could map one of several sources to a previously used slot. Include a live validity
indicator, a short movement check for each assigned limb, and an explicit finish action. Cancel
leaves prior valid calibration active; reset clears the selected mapping and its offsets.

When a target loses tracking, retain validity per component and per semantic target. A missing
position with a valid orientation may still support orientation-only use if the profile allows
it. Loss fallback is configured per target: fade tracking influence to animation, preserve a last
pose only for brief presentation continuity while marking it invalid, or disable a dependent
action. Never replace an invalid pose with the world origin. Never invent missing finger joints or
promote predicted display motion to measured tracking.

Pose-driven actions such as grabbing, pointing, or contact interaction cancel once when their
required targets become invalid. Release held input exactly once and stop associated haptics when
the owning provider is no longer valid. Death, ragdoll, possession changes, or driver-lease release
clears the character's tracking overrides. Reacquisition requires valid current samples and a new
accepted action state; it must not replay stale presses or snap the authoritative character to an
old pose.

## View and camera model

Each eye is a render view with its own runtime pose and asymmetric FOV. The world camera remains
a shared logical view. The renderer combines that view with the tracked eye offset and runtime
projection parameters for submission.

The camera plan's target-specific output remains the shared camera seam. The XR adapter supplies
copied view data; it does not create a second camera controller or write the authored camera
lens. The camera pose for gameplay is the declared head or aim policy, while eye projections
stay per-eye render data.

Rules for projection and pose:

- use runtime-located poses for the target display time;
- preserve the runtime's per-view asymmetric FOV values;
- keep rendered projection and submitted projection metadata coherent;
- treat display coverage and content magnification as separate choices;
- do not fake IPD, world scale, or head pose to imitate prescription optics;
- do not replace runtime eye separation with a mono camera copied twice;
- keep spectator and desktop views as separate consumers of camera output.

An interface or region magnifier enlarges selected content while leaving the outer eye projection
and submitted FOV on the runtime baseline. Narrowing the submitted FOV alone changes display
coverage, not content magnification. An advanced full-view projection change must render with
and submit the same validated FOV and preserve compositor geometry and depth semantics.

Portal and mirror views may use engine projection. Headset submissions follow runtime view
state.

## Per-eye perceived-focus controls

Calibration may offer separate left and right perceived-clarity controls for users who find that
one image appears softer or differently scaled. The UI must name these as software presentation
adjustments. Neutral strength is zero. Provide a linked-eyes control by default, a preview for
each eye, and an explicit binocular confirmation step.

The first version should prefer a bounded clarity filter and readable UI options. A user may
disable depth of field or request a stable text treatment for accessibility. If an adjustment
changes image scale, label it as a visual fit control and show the binocular preview. Per-eye
unequal scale is an advanced option that stays off by default because it can impair binocular
fusion.

Software sharpening, blur, depth of field, crop, zoom, or projection changes cannot move a
headset's fixed optical focal plane or replace prescription lenses. HoloLens guidance describes
fixed display distance and virtual convergence as separate factors. Its numerical advice is
device-specific and is not a universal VR setting ([Microsoft mixed-reality comfort guidance](https://learn.microsoft.com/en-us/windows/mixed-reality/design/comfort)).

If a future headset exposes physical lens adjustment through a supported runtime extension or
vendor API, the adapter may report that capability and offer its documented control. Otherwise
the interface must say physical lens adjustment is unavailable. Do not convert subjective slider
values into diopters or infer a prescription from a user's calibration response.

## Magnification and calibration

Keep interface zoom and world magnification as different controls. Interface zoom may enlarge
menus, captions, reticles, and text while preserving their screen placement rules. World
magnification changes apparent scene scale and has greater comfort and aim risks.

Initial values and behavior:

- linked binocular magnification is the default;
- a dimensionless value of `1.0` means neutral magnification;
- store validated finite values within product-tested bounds;
- provide a one-action reset to the neutral profile;
- show the actual effect during calibration before saving;
- avoid claims that a value matches a physical prescription.

Full-world stereo magnification is experimental. Gate it behind testing of head movement,
reprojection, vergence, depth judgment, and hand-to-world alignment. Do not commit to a
tangent-scaling formula or claim full-world magnification works before the render path proves it. A
bounded magnifier for UI or a small view region is the fallback if world scaling causes
discomfort or breaks interaction.

Calibration should be reachable from the in-game menu and return to the previous game state
without losing the session. Provide a stationary grid, high-contrast text, and near, middle, and
far markers. The player previews the left eye, previews the right eye, then confirms the
binocular image. Controls remain linked unless the player enables independent adjustment.

The flow must include cancel, reset, save, and restore behavior. Keep the player stationary
during visual calibration. Give clear IPD and headset-fit guidance, and point to runtime or
device setup for those settings. Do not fake IPD changes through world scale or camera movement.

Calibration captures input through a local modal action context. Its confirm, cancel, reset, and
preview actions do not leak into gameplay; calibration haptics use that same context and remain
bounded. It does not pause the multiplayer world. The player moves to a safe lobby or room
according to the game's policy, while server authority and other players continue.

## Local profile and persistence

Store vision preferences in a local, versioned user profile. Keep values separate from save
games, world assets, replicated player state, and network messages. A multiplayer game may query
whether a player is ready, but it must not read or alter detailed vision settings.

The profile may contain:

- schema version and profile revision;
- linked or independent eye preference;
- validated per-eye clarity and optional scale values;
- interface magnification and world magnification choice;
- depth-of-field and text accessibility preferences;
- eyewear context selected by the user, if useful to the calibration flow;
- runtime and view-configuration fingerprint for stale-profile detection.

On a schema or runtime-view mismatch, retain the old profile safely and ask the player to
reconfirm or reset. Do not auto-apply an old calibration to a changed view configuration.
Corrupt, missing, or non-finite values fall back to neutral settings and produce a local
diagnostic.

## Input, comfort, and player control

XR controls enter through the action system as named actions. The adapter translates runtime
events into bounded action samples, while pose and joint reads use copied `TrackingSample` values.
Gameplay consumes validated action and semantic tracking input at fixed ticks. Device events,
runtime handles, local source identity, and render-time pose updates remain local to the client.

Use the shared device and action path described by the [input system](input-system.md) and
[VirtualDeviceService plan](virtual-device-service.md); XR must not add a parallel gameplay
input channel. Check runtime profile availability for these proposed [OpenXR interaction
profile paths](https://raw.githubusercontent.com/KhronosGroup/OpenXR-Docs/main/specification/sources/chapters/semantic_paths.adoc): HTC Vive wands
(`/interaction_profiles/htc/vive_controller`), Oculus Touch used by Rift S
(`/interaction_profiles/oculus/touch_controller`), Valve Index
(`/interaction_profiles/valve/index_controller`), and the Khronos simple-controller fallback
(`/interaction_profiles/khr/simple_controller`). These are unverified targets, not a claim of
working hardware support. Availability and bindings depend on the active runtime. Treat Vive
Cosmos and Focus as separate future extension checks, not as Vive wand compatibility.

Support explicit policies for:

- head-directed or controller-directed aiming;
- handedness and tracked controller offsets;
- snap turn and smooth turn, with a comfortable default;
- teleport or continuous locomotion when the game declares support;
- seated or standing play mode where valid for the experience;
- pause, recenter, and session focus behavior;
- haptics requested through action output with bounded duration and strength.

No cinematic may take control of the tracked head pose. A cutscene can move the virtual camera
subject or use a bounded transition, but it must preserve head tracking and offer a comfort-safe
skip path. The recenter action is explicit and records an origin generation so other systems can
react.

Session focus loss, headset removal, tracking loss, or controller disconnect must release held
gameplay actions, stop unneeded haptics, pause or safely degrade the experience, and explain how
to resume. Returning focus must not replay stale button presses. If a device pose is invalid,
preserve the last safe state only for display continuity and mark it invalid for gameplay use.

Acceptance checks cover profile discovery and bindings for buttons, axes and touchpad controls;
grip and aim poses with validity; haptics when exposed; tracking and session-focus loss; and
disconnect/reconnect without stuck actions or stale poses.

## Shared-room and network rules

Shared-room physical placement is separate from virtual presentation. A session owns an explicit
physical floor/origin mapping and a monotonically increasing origin generation. Recenter or room
remap updates that mapping as a copied event. Presentation offsets must never become authority
over physical player position.

Use runtime boundary or guardian information when the platform exposes it. The game may add
warnings and conservative collision checks, but cannot rely on a visible avatar or a guessed
room polygon as the safety system. Never move a physical player by teleporting their tracked
pose. Virtual locomotion changes the game-space mapping under a declared policy and must remain
distinct from physical movement.

Network messages carry copied, bounded intent and any pose data the game actually needs. The
server validates player ownership, possession or driver lease, profile and calibration revisions,
tick order, finite values, action ranges, target reach and rate limits, and movement against
authoritative rules. It does not accept a client pose as proof of collision, reach, or room safety.
Render-only late tracking never writes the capsule, network action history, or authoritative
character transforms. Gameplay pose uses fixed-tick sampled and validated intent or semantic
targets; a current lease and matching revision are required to submit them.

Replicate semantic targets and their validity only when another peer needs them for gameplay or
presentation. Keep packet size, target count, and update rate bounded, and use interpolation for
remote presentation without changing the authoritative tick at which an input was accepted.
Record the consumed tick samples for replay. Physical source identity, tracker serials, local role
assignment, mount calibration, and raw device fingerprints stay local. A client sample cannot
override server collision, reach, room boundary, or locomotion checks.

The first cooperative test experience should use an original small arena or range with two
players. It should exercise tracked aim, weapon offsets, handedness, reload actions, bounded
haptics, and basic spatial world audio. Add tracking interruption, room boundaries, and network
recovery. Record the chosen action and pose policy in the experience rather than hiding it in the
XR adapter.

## Diagnostics and performance

Expose runtime name, supported view configuration, session state, tracking validity, swapchain
dimensions and formats, frame timing, and dropped or skipped frame counts. Do not log personal
calibration values to shared telemetry. A local diagnostic view may show whether preferences are
neutral or active without exposing exact profile values to the game.

Tracking diagnostics should show bounded device and source counts, reported capabilities,
assignment state, source and schema revisions, origin generation, sample age and sequence,
component validity, and the selected semantic target source. Show overflow and stale-handle
rejections. Personal dimensions, exact mount offsets, persistent hardware identifiers, and raw
fingerprints remain private. Profile CPU time and copied sample bytes at the provider boundary;
report joint counts and update rates without inventing confidence or velocity data.

For script and vision sources, also show producer origin, script execution
cost, publish counts and bytes, capture-to-result latency, result age at input
acceptance, queue drops, and filtering delay. Keep model availability and
camera availability distinct from whether a virtual controller exists.

Measure CPU and GPU frame time percentiles on named hardware, runtime, resolution, refresh rate,
and render scale. Track time spent waiting on the runtime separately from engine work. Report
per-eye render cost and any reprojection or missed-frame signal the runtime exposes. Do not
invent a performance budget or claim a measured result before implementation.

## Delivery stages

Script-defined controllers depend on the common input scripting contract,
not on the headset stages below. Synthetic hand/joint sources additionally
need the shared tracking schema. Camera inference and XR hardware integration
are independent adapters that can follow those contracts separately.

1. Verify target operating systems, runtime availability, Rift S compatibility, graphics
   bindings, and license requirements. Record supported combinations.
2. Build an adapter prototype that opens a session, locates views, renders a neutral scene, and
   handles orderly shutdown and focus changes.
3. Connect view data to the existing camera and render paths; preserve each runtime pose and FOV
   through submission.
4. Map XR actions into the existing input and character intent path. Add tracking-loss release,
   recenter generation, and explicit comfort settings.
5. Integrate the independently deliverable common device and tracking contract with the XR backend:
   bounded feeds, generation-checked handles, normalized samples, origin conversion, and capabilities.
   The Luau virtual producer, including procedural or recorded hand/joint sources,
   belongs to the input scripting phase once its shared schema is ready. It can
   ship before headset stages 1-4 and requires no XR hardware.
6. Integrate a positional tracker provider and a joint-capable hand provider when a selected runtime
   or device backend exposes them. Record the tested runtime, provider, device, and operating-system
   combinations.
   Separately evaluate an optional camera/inference adapter and reusable Luau
   landmark/gesture modules. An unavailable camera or model must not block
   virtual controllers, recorded feeds, or native XR tracking.
7. Add `TrackingRigProfile` references to planned retarget profiles and character
   `TrackingBinding`, then validate semantic targets with a headless character fixture.
8. Add local source-to-slot calibration, mount correction, short movement verification, and
   reassignment after provider, device, rig, or origin changes.
9. Add the local vision calibration and profile flow with neutral defaults and per-eye preview.
   Keep physical lens control capability-gated.
10. After headless fixtures pass, complete final hardware verification on supported, available setups.
    Check device assignment, mount and reference calibration, rig bone or chain
    mapping, partial tracking loss, reconnect, and source switching. Report only tested combinations.
11. Add the cooperative range and validate copied fixed-tick intent, semantic target budgets,
    audio, boundary handling, and recovery under a headless authoritative server.
12. Evaluate advanced world magnification only after binocular and comfort tests pass. Keep it
    opt-in and removable if evidence is poor.

## Validation

Headless tests should cover profile validation, version migration, finite and bounded values,
stale-runtime detection, neutral fallback, action release, and copied message validation. Render
tests should check asymmetric per-eye projections, matching submitted metadata, and consistent
pose sampling.

Tracking checks should cover arbitrary device counts within declared bounds, overflow behavior,
unknown capabilities, stale `InputDevice` generations, ambiguous reconnect, role swaps, source and
skeleton schema revisions, finite normalized transforms, separate position and orientation
validity, optional confidence and velocity availability, clock conversion, origin-generation
rejection, and simulated-provider operation without an XR runtime. Verify that uncalibrated or
incompatible coordinate spaces cannot be combined.

Script-provider fixtures should drive the ordinary action path and an animated
rig from Luau-created controls and named joints without a headset. Cover atomic
publication, script stop/error/hot reload, freshness timeout, stale producer
handles, model/source revision changes, partial landmark loss, detector order
changes, mirrored input, unknown depth, and camera denial or removal. Replay
the accepted tick samples with capture and inference absent. Complete the
optional vision backend gate with a real camera check on named hardware and
report capture, inference, filtering, and input-acceptance latency separately.

Character checks should cover profile references, stable named slots, local calibration revision
mismatch, semantic target precedence, masks and weights, bounded IK constraints, sparse hand and
body inputs, and explicit animation fallback. Network and replay checks should verify possession
or driver lease, reach and rate validation, target count and packet limits, fixed-tick recording,
and rejection of client collision claims. Exercise source loss, one-time action cancellation,
haptic stop, and tracking override cleanup on death, ragdoll, and possession release.

Desktop and headless tests cannot verify optical comfort, physical focus, or binocular fusion.
Those require headset checks with the supported runtime. Validate the calibration grid and text,
per-eye adjustments, head motion, recenter, tracking loss, boundary warnings, and comfort
transitions with people using the target device. Do not treat a successful launch as evidence
that vision settings are safe or useful.

Complete final device verification after headless fixtures pass. Where supported hardware is available,
use at least one actual positional tracker provider and a joint-capable hand provider.
Check real device assignment, mount
and reference calibration, rig bone or chain mapping, partial loss, reconnect, and source switching.
Record the tested operating system, runtime and version, provider and version, device models,
enabled extensions, and each feature exercised. Mark all other combinations unverified. Do not
infer availability or support from another runtime or from USB enumeration alone.

Rift S is the named first validation target from the request. Before relying on it, verify that
a supported runtime and driver are available on the chosen development platform. Windows is the
first platform to investigate. Verify macOS and Linux runtime paths separately before listing
them as supported.

## Reference experiences

The intended reference list is:

| Experience | Study question | Status |
|---|---|---|
| Far Cry VR at Zero Latency Adelaide | How group coordination, movement, weapons, and safety cues are presented | POVs pending |
| Warhammer 40,000: Space Marine VR - Defenders of Avarax at Zero Latency Adelaide | How squad encounters, aiming, reloads, and difficulty choices are presented | POVs pending |

When POVs arrive, record timestamp, action, physical versus virtual movement, tracking cues,
comfort observations, proposed engine gap, and uncertainty. Mark observations separately from
official product facts, and record supplied venue and build context. A recording cannot
establish latency metrics or optical properties. Do not infer hidden implementation, headset
model, runtime, or lens controls from video alone.
