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
- head and controller actions translated into the common input path;
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
| Input | XR action mapping, focus-loss release, and fixed-tick samples |
| Character and physics | Validated locomotion intent, collision, ground state, and authoritative movement |
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
events into bounded samples. Gameplay consumes one authoritative sample per fixed tick. Device
events, runtime handles, and render-time pose updates remain local to the client.

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
server validates player ownership, tick order, finite values, action ranges, rate limits, and
movement against authoritative rules. It does not accept a client pose as proof of collision,
reach, or room safety. The renderer may predict and smooth remote poses, but simulation commits
only at its fixed tick.

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

Measure CPU and GPU frame time percentiles on named hardware, runtime, resolution, refresh rate,
and render scale. Track time spent waiting on the runtime separately from engine work. Report
per-eye render cost and any reprojection or missed-frame signal the runtime exposes. Do not
invent a performance budget or claim a measured result before implementation.

## Delivery stages

1. Verify target operating systems, runtime availability, Rift S compatibility, graphics
   bindings, and license requirements. Record supported combinations.
2. Build an adapter prototype that opens a session, locates views, renders a neutral scene, and
   handles orderly shutdown and focus changes.
3. Connect view data to the existing camera and render paths; preserve each runtime pose and FOV
   through submission.
4. Map XR actions into the existing input and character intent path. Add tracking-loss release,
   recenter generation, and explicit comfort settings.
5. Add the local calibration and profile flow with neutral defaults and per-eye preview. Keep
   physical lens control capability-gated.
6. Add the cooperative range and validate copied network intent, audio, boundary handling, and
   recovery under a headless authoritative server.
7. Evaluate advanced world magnification only after binocular and comfort tests pass. Keep it
   opt-in and removable if evidence is poor.

## Validation

Headless tests should cover profile validation, version migration, finite and bounded values,
stale-runtime detection, neutral fallback, action release, and copied message validation. Render
tests should check asymmetric per-eye projections, matching submitted metadata, and consistent
pose sampling.

Desktop and headless tests cannot verify optical comfort, physical focus, or binocular fusion.
Those require headset checks with the supported runtime. Validate the calibration grid and text,
per-eye adjustments, head motion, recenter, tracking loss, boundary warnings, and comfort
transitions with people using the target device. Do not treat a successful launch as evidence
that vision settings are safe or useful.

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
