# Mobile client support

This document defines the work required to ship the game client on Android and
iOS. It is an implementation plan, not a claim that either platform is already
supported.

The mobile product is the player client. The studio, launcher, server, CDN and
developer tools remain desktop products unless they are scheduled separately.
Mobile must reuse `Mono::client`, the engine modules and SDL3. The production
implementation should reuse the render graph where mobile hardware supports it,
but the first prototype deliberately does not depend on that renderer. Separate
mobile render paths remain possible when physical-device evidence shows they are
needed. Mobile must not grow a second input model, content format or simulation
loop.

---

## 1. What already exists

The repository has useful platform-neutral foundations, but no mobile product
build or runtime yet.

| Area | Current state | Remaining gap |
|---|---|---|
| Platform source selection | `mono.build/MonoLibrary.cmake` recognises `android` and `ios` platform directories, with `posix` as their shared family | There are no Android or iOS product presets, packages or platform hosts |
| Rendering | SDL3 GPU is the renderer boundary. Built-in shaders are staged as SPIR-V and MSL | Android Vulkan capabilities need a defined floor, iOS must be exercised on Metal, and device suspension must be handled |
| Client structure | `Mono::client` is a library with a thin `mono.client/app/main.cpp` executable | The startup and frame loop are still owned by a desktop `main` function |
| Input | `engine::input::Translator` handles keyboard, committed text, mouse, focus and gamepad events | It has no touch contacts, composition text, safe area or mobile orientation state |
| Paths | `engine::core::Paths` centralises the executable-relative asset root | Mobile resources are read-only packaged data, while saves, preferences and caches require writable sandbox locations |
| Networking | Endpoints and transports already represent IPv4 and IPv6 | Hostname resolution, IPv6-only networks, suspend and resume, and local-network permissions need device validation |

The first mobile change should preserve these seams. Platform details belong in
private platform sources or thin product hosts, not in public headers.

---

## 2. Supported product and device floor

Before implementation, record the support policy in one place and enforce it in
the package metadata and renderer startup:

- Android ships as an arm64 game client. Add emulator ABIs only for development
  if they do not complicate the shipped package.
- iOS ships as an arm64 game client built on macOS with Xcode. SDL3 GPU does not
  support the iOS Simulator, so rendering acceptance requires physical devices.
- Vulkan through SDL3 GPU is the Android rendering path. Metal through SDL3 GPU
  is the iOS rendering path.
- Pick minimum operating-system versions from the oldest physical devices in the
  test matrix, not from the oldest value accepted by a toolchain.
- Landscape, portrait or both must be a game setting. The package manifest and
  runtime window policy must agree.

The minimum GPU feature set is a product decision. SDL3 allows optional Vulkan
features such as clip distance, depth clamping, indirect first instance and
anisotropy to be disabled for wider Android coverage. Audit every render pass
before choosing which features are required. A device that lacks the declared
floor must fail at startup with the missing capability named.

---

## 3. Early prototype without engine shaders

The first milestone is a mobile host, not a mobile rendering port. It proves the
parts that every later renderer depends on while keeping shader translation,
pipeline compatibility and render-graph debugging out of the first bring-up.

### 3.1 Prototype rendering boundary

Create a temporary minimal presentation implementation with no engine-authored
shaders, materials, scene passes or render graph. Its visual work is limited to:

1. Create the SDL window and mobile drawable.
2. Acquire a drawable or swapchain image.
3. Clear it to a solid colour.
4. Present it.

A changing clear colour may acknowledge touch, focus and lifecycle state. Keep
detailed state in device logs so the prototype does not need text, fonts or a UI
shader. The prototype does not render game geometry and is never a release
renderer.

Prefer a clear-only SDL GPU path because it exercises the same Vulkan and Metal
device and swapchain boundary the production renderer will use. If device
bring-up is blocked before SDL GPU can present, an SDL simple-renderer fallback
may be used to isolate packaging and lifecycle problems. Delete that fallback
when SDL GPU clear and present work on both platforms.

### 3.2 Essentials proved by the prototype

The prototype is complete when both a physical Android device and a physical
iOS device can:

- Install and launch from a clean build.
- Create a window at the correct drawable size and report display scale and safe
  area.
- Receive touch, text, gamepad and orientation events.
- Enter the background without presenting or ticking, then resume cleanly.
- Handle low-memory and termination callbacks without double shutdown.
- Read one packaged resource and write one preference and one disposable cache
  file to the correct locations.
- Open audio, make a direct network connection and report permission refusal as
  a normal result.
- Run repeated launch, suspend, resume and rotation cycles without leaking
  unbounded memory.

Keep this host runnable while production rendering is introduced. It is the
smallest reproduction for mobile lifecycle, package, input and swapchain bugs.

### 3.3 Reintroducing rendering

After the essentials work, bring rendering back in increasing slices:

1. Load and create one staged shader in the native format for the device.
2. Draw one untextured triangle through one production pipeline.
3. Draw one textured GUI element and verify safe-area hit testing.
4. Render the simplest built-in scene without post-processing, particles or
   shadows.
5. Enable production passes one at a time, recording unsupported capabilities,
   visual differences, frame time and memory.
6. Select mobile quality defaults only after sustained physical-device runs.

The default outcome is one render graph with capability-selected nodes and
mobile quality settings. Add a separate Android or iOS path only when a physical
device demonstrates one of these reasons:

- The platform cannot express a required pass or resource layout.
- A shared pass has a measured cost that misses the device budget and a
  platform-specific algorithm fixes it.
- A confirmed driver defect needs a contained workaround.
- Metal and Vulkan require materially different resource or synchronisation
  strategies that cannot be hidden behind the existing renderer boundary.

A separate path belongs at the narrowest render node or backend boundary that
contains the difference. It consumes the same ECS visual state and produces the
same graph output as the shared path. Do not fork scene extraction, simulation,
assets or the whole client. Record the device, driver, measurement and removal
condition beside every platform-specific path.

---

## 4. Build and package

### 4.1 Cross-compilation presets

Add checked-in configure and build presets for:

- Android arm64 release and development builds using the NDK CMake toolchain.
- iOS device release and development builds using the Xcode generator or an
  equivalent CMake toolchain on macOS.
- An optional Android emulator build for fast lifecycle and input checks. It is
  not a rendering acceptance target.

Each preset builds only the client and the shared modules it needs. It must not
try to build desktop-only programs such as the studio, launcher, server or CDN.
Tests that can run on the build host remain host builds. Mobile-specific tests
that require SDL, packaging or a device become device test targets.

### 4.2 Host build tools

`glslc` and `mono.tools/shadercross` run during the build. A cross build cannot
execute copies compiled for Android or iOS.

Make host tools an explicit build input:

1. Build or locate native-host `glslc` and `shadercross` executables.
2. Pass their absolute paths into the mobile configure.
3. Keep target shader libraries separate from those executable tools.
4. Make configuration fail early if either tool is a target binary or cannot be
   executed by the host.
5. Continue staging SPIR-V and MSL from the same source shaders so desktop and
   mobile do not acquire different shader pipelines.

The Windows cross build already demonstrates the `glslc` half of this split.
Mobile should generalise that mechanism for every build-time executable.

### 4.3 Android package

Add an Android application project that owns only product packaging and the SDL
Activity seam. It should contain:

- A package identifier, version code, application label and icons.
- A manifest with the SDL Activity, orientation policy and only the permissions
  required by enabled features.
- Gradle configuration that invokes the repository's CMake build and packages
  its assets.
- A native shared-library target in the form expected by SDL's Android host.
- Debug APK install and launch targets for development.
- Release Android App Bundle output, signing inputs supplied outside the
  repository, and symbol files retained for crash decoding.

Do not fork SDL's Java host unless the engine needs behaviour that cannot be
expressed by subclassing it. Any local Java or Kotlin code should stay a thin
bridge from platform services to copied engine messages.

### 4.4 iOS package

Add an iOS application bundle target that contains:

- Bundle identifier, version, icons, launch screen and supported orientations.
- `Info.plist` usage descriptions for every enabled protected capability.
- The SDL entry-point integration and linked Apple frameworks selected by SDL.
- Packaged bootstrap assets and both MSL source or metallib output, according to
  the shader packaging decision.
- Development signing selected locally or in CI, with credentials kept outside
  the repository.
- An archive path that produces an installable build and retains dSYM files.

The unsigned or development-signed build must be reproducible from a clean
checkout before store automation is added.

---

## 5. Application lifecycle

Mobile operating systems own the process lifecycle. The current client owns a
continuous polling loop, so its host must be split into lifecycle-sized calls.

Refactor the product boundary into these operations:

- Initialise configuration, logging, jobs, SDL and `client::Client` once.
- Handle one SDL event immediately.
- Advance at most one frame when the app is active.
- Suspend presentation, simulation, audio and input without destroying saved
  state.
- Resume the window, swapchain, audio device and network-facing services.
- Release memory on a low-memory notification.
- Shut down exactly once after partial or complete initialisation.

Use SDL main callbacks, or an equally thin SDL-supported host, for Android and
iOS. Keep the ordinary desktop entry point as an adapter over the same
operations. The client library must not learn Android Activity or iOS delegate
types.

Application events for backgrounding, foregrounding, low memory and termination
must be handled in the callback path where SDL delivers them. They cannot wait
for a later event poll. Once backgrounding begins:

- Stop acquiring swapchain textures and submitting presentation work.
- Stop simulation ticks and clear held input so resume cannot leave an action
  stuck down.
- Pause or close audio according to what the platform permits.
- Stop LAN discovery and suspend connection timers without advancing simulated
  time.
- Persist the small amount of local state needed to recover from process death.

On resume, treat the drawable size, safe area, orientation, audio route and GPU
surface as changed. Recreate only the resources the platform invalidated. A
resume must not reload the world or redownload verified content unless the
process was actually terminated.

---

## 6. Packaged resources and writable data

`Paths::Base()` currently derives everything from the executable directory.
That model does not describe either mobile sandbox.

Introduce platform-neutral locations with distinct contracts:

| Location | Contract | Examples |
|---|---|---|
| Packaged resources | Read-only and installed with the application | Built-in shaders, fonts, default textures, bootstrap game data |
| Persistent data | Writable and backed up according to product policy | Settings, saves, account-independent local state |
| Cache | Writable, disposable and bounded | Verified CDN content, compiled or decoded derivatives, temporary downloads |

The public API names the contract, never the operating system. Its Android and
iOS implementations may use SDL's platform paths and packaged-resource APIs.

Audit every `Paths::Assets()` consumer. Code that needs bytes must read through
one engine resource source rather than assuming a `std::filesystem::path` can
open an APK asset. Libraries that require memory input should receive the bytes
from that source. Keep a path adapter only where the platform truly provides a
normal file.

Content downloaded after installation belongs in the verified content cache,
not in the application bundle. Cache writes must be atomic, size-bounded and
recoverable after termination during a download. Clear-cache behaviour must not
remove saves or preferences.

---

## 7. Touch, text and display state

### 7.1 Touch contacts

Extend the shared input state with a fixed-capacity or otherwise bounded set of
touch contacts. Each contact needs a stable contact id, phase, logical position,
frame delta and pressure where available. Cancellation is distinct from release.
Several contacts in one frame must remain several contacts.

Do not globally turn touch into mouse input. The GUI hit-test layer may expose a
primary pointer compatibility path for existing buttons, but gameplay and
scripts need the original contacts for gestures and multi-touch controls.
Mouse, keyboard and gamepad input must continue to work on tablets and connected
controllers.

Add deterministic translator tests for:

- Press, move, release and cancel.
- Two contacts whose events interleave.
- Contact-id reuse after release.
- Focus loss or backgrounding while contacts are held.
- Primary-pointer compatibility without duplicate GUI activation.

### 7.2 Text input

Committed `SDL_EVENT_TEXT_INPUT` already reaches focused text boxes. Mobile also
needs composition state from text-editing events, on-screen keyboard show and
hide control, selection updates, return-key policy and keyboard occlusion.

The focused text control owns whether text input is active. The platform IME
owns composition. Do not manufacture key presses from committed text or store a
composition as final document text before it commits.

### 7.3 Safe area, scale and orientation

Expose logical window size, drawable pixel size, content scale, safe-area insets
and orientation as presentation state. GUI layout uses logical units and safe
areas. Render targets use drawable pixels.

Rotation and resize are normal runtime changes. They must update camera aspect,
GUI layout, touch coordinates and swapchain-dependent targets in one frame. A
notch or home indicator must not silently move gameplay coordinates; only UI
that opts into safe-area layout is inset.

---

## 8. Rendering on mobile GPUs

Start from one render graph and add a capability profile. Use the evidence gate
in section 3 before introducing a platform-specific render path.

The mobile profile must define:

- Required SDL GPU features and the fallback for each optional feature.
- Supported texture formats, dimensions, sampler modes and render-target counts.
- Limits for lights, shadows, particles, resident instances and transient
  attachments.
- A default frame rate and quality tier for low, medium and high device classes.
- Dynamic resolution inputs and hysteresis so resolution does not oscillate.
- Memory-pressure behaviour for transient pools, texture residency and decoded
  asset caches.

Android uses the staged SPIR-V. iOS uses the staged MSL or precompiled metallib.
Run shader validation for both formats in the host build. Physical device tests
must still create every production pipeline because offline translation cannot
prove device feature support or binding correctness.

The renderer must tolerate a temporarily unavailable swapchain and a changed
drawable size. Backgrounding cannot leave command buffers or presentation work
in flight indefinitely. GPU startup errors must report the backend, device and
missing capability in logs that can be collected from a device.

Measure sustained performance, not only a short launch capture. Record frame
time, GPU time, memory, battery state and thermal state over a representative
scene long enough for throttling to appear. Choose mobile defaults from those
measurements.

---

## 9. Networking and platform permissions

Mobile networking keeps the existing protocol and endpoint types. Add platform
behaviour around them:

- Resolve hostnames asynchronously so DNS cannot stall the frame callback.
- Test direct connections and content delivery on IPv4, dual-stack and
  IPv6-only networks with NAT64.
- Re-establish or clearly fail sessions after the operating system changes
  networks while the app is suspended.
- Stop LAN discovery while backgrounded.
- Request and explain local-network access only when the user chooses browsing.
- Keep Internet, local-network, Bluetooth controller, microphone and other
  permissions conditional on features that actually use them.

Permission refusal is a normal result. It must disable the relevant feature and
explain why without preventing offline or direct-connect play when those paths
remain valid.

---

## 10. Diagnostics, testing and release gates

### 10.1 Automated checks

Add host tests for lifecycle state transitions, path contracts, touch
translation, composition state, capability-profile selection and cache recovery.
They must use copied messages and explicit time so the existing determinism rule
still holds.

CI gates are:

- Android arm64 configures and builds from a clean checkout.
- The debug APK packages, installs and launches on a device job.
- iOS device configuration and archive build pass on a macOS worker.
- Every staged SPIR-V and MSL shader passes the existing shader checks.
- Desktop `dev`, `server` and `ci` gates remain green.

### 10.2 Physical device matrix

Keep a small named matrix instead of testing whatever phone happens to be near
the developer:

- Oldest supported Android OS and lowest supported GPU class.
- Current mid-range Android device.
- Current high-end Android device.
- Oldest supported iPhone or iPad GPU class.
- Current iPhone or iPad.

Each release candidate runs the same smoke scene and records device model, OS,
GPU backend, driver, quality tier, drawable size and result. The matrix may grow
when a real defect demonstrates a missing class.

### 10.3 Definition of done

Android and iOS support is complete only when all of these are true:

- A documented clean-checkout command produces an installable development
  package for each platform.
- The app launches into a real game, loads packaged and downloaded assets, and
  renders through the same graph as desktop.
- Touch can drive gameplay and every standard GUI control without duplicate
  activation.
- A user can enter, compose, select and delete non-ASCII text with the native
  keyboard.
- Rotation, safe areas and high-density displays produce correct rendering and
  hit testing.
- Backgrounding and resuming during rendering, audio playback, content download
  and a network session do not hang, corrupt data or advance simulation time.
- Low-memory handling releases disposable data and either recovers or exits with
  a useful diagnostic.
- Direct connect and content delivery work on IPv6-only mobile networks.
- A sustained representative scene meets the recorded frame-time, memory and
  thermal budgets on the lowest supported devices.
- Release packages retain symbols, use external signing credentials and pass
  the platform store's automated validation.

---

## 11. Implementation order

Build this in vertical slices so every stage leaves a runnable product:

1. Cross-compile the minimal mobile host and only the dependencies it needs.
2. Package and launch the clear-only prototype on one Android device and one iOS
   device.
3. Prove lifecycle, paths, touch, text, orientation, audio and networking with
   the minimal presentation implementation.
4. Cross-compile `Mono::client` and its dependencies with native-host shader
   tools.
5. Reintroduce one shader, one pipeline and then the simplest built-in scene.
6. Replace executable-relative resource assumptions and load packaged shaders,
   fonts and bootstrap content.
7. Enable production render passes individually and decide whether any need a
   separate mobile path from physical-device evidence.
8. Establish mobile GPU capability and quality profiles from sustained device
   measurements.
9. Validate permissions, cache recovery and network changes with the full
   client.
10. Add CI packaging, device smoke tests, crash symbols and release automation.

Do not begin store submission while the physical-device rendering, lifecycle and
data-recovery gates are still open. Packaging can hide an engine defect, but it
cannot repair one.

---

## References

- [SDL Android integration](https://wiki.libsdl.org/SDL3/README-android)
- [SDL iOS integration and lifecycle](https://wiki.libsdl.org/SDL3/README-ios)
- [SDL main callbacks](https://wiki.libsdl.org/SDL3/SDL_MAIN_USE_CALLBACKS)
- [SDL GPU platform requirements](https://wiki.libsdl.org/SDL3/CategoryGPU)
