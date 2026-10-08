# ROADMAP

## Editing

Deferred items are for items that need significant systems we cannot do now.
If you can do the item now, do NOT add to the deferred list.

Do NOT add new deferred work as a roadmap item. Place it in
`docs/DEFERRED.md`. If a TODO item is not FULLY completed, split the
TODO item, keeping the concise short dash point list with block infront and
split it as another item under the same version.

For example, if we complete A and B but not C and D:
```
### v0.5
- [x] do: A1, A3, A5
- [_] do: A, B, C, D
- [x] do: G, H, I
- [x] do: K, K2, K3
```

becomes:
```
### v0.5
- [x] do: A1, A3, A5
- [x] do: A, B
- [x] do: G, H, I
- [x] do: K, K2, K3
- [_] do: C, D
- [_] deferred `D0001` for later.
```

or defer to another version.

## VERSIONS

Milestone labels describe development scope, not release versions.

### v0.25

Priority: Finish the imagegraph fundamental system first. Pause separate audio, mesh and animation work until it is complete, then revisit those tracks.

- [_] Add custom profiles such as RenderPipeline: let RunProfileWorkflow select a workflow, expose its inputs and outputs as node ports, and open its inspector popup on double-click.

Pixel Composer foundations:

- [x] Retain the source licence, pinned metadata, enum choices and typed shader inputs; preserve static selectors and source records; save instance ports, graph selectors and cooked-shader references.
- [x] Evaluate bounded image, data, path and palette kernels, including polar mapping, surface/buffer conversion, Julia sets, Gabor noise, tile patterns and heightmap projection; preserve mapped inputs, surface formats and instance routes.
- [x] Replay animation, sequences, Gradients, Time Remap, FLIP/Verlet, Strand and rigid actors with bounded history; clear and refill mixed owned image cycles, retain random state, resolve FFT windows and rasterize trails/VFX.
- [x] Edit graphs, groups, keys and tiles atomically; scope Dopesheet Delete and canvas shortcuts to their panels; refresh HLSL sockets, cook shaders and retain Lua observations; preserve moved inputs, animator aliases and refused selections; insert Gradient/Matrix keys and save rigid meshes with metadata.
- [x] Read granted assets through bounded hosts; reload changed WAV files on the host frame clock.
- [x] Run bounded Lua and recorded-random export helpers; publish prepared image, sequence, WAV, CSV and tilemap outputs with rollback.
- [x] Evaluate bounded volume projection; bind rigid/3D outputs, mesh availability and skyboxes; render Composer shaders from cooked binaries and queue camera/SDF jobs. Keep live ShaderScript compilation. Cooked Composer Vulkan tests pass; broader GPU verification remains open.
- [x] Package source projects and authored scenes; profile scene, simulation, atlas, palette, filter, FFT, path and Strand CPU workloads. Visual acceptance remains open.
- [x] Add typed physics, replication and portal observations with optional trace IDs, bounded per-world records and server JSONL export (physics and replication: `1dd7743d`).
- [x] Finish the measured engine stress pass and matched 200-client `RecoverRows` profiling. Retain the 4 m physics grid default; cell-size results vary by scene. The recovery candidate was rejected without a demonstrated gain. See the [stress audit](docs/ENGINE_STRESS_AUDIT_2026-09-22.md).
- [x] Validate the same-world follow-camera repair and extended tunnel walk with native presentation and render tests (`04315bcd`). Passed native dev/Vulkan validation on 2026-10-04: 32 presentation cases, the 64-frame tunnel sweep and 5 portal GPU cases.

Pixel Composer [M0 to M7](docs/to-delete/v026-pixel-composer.md):

- [x] Add selected-key easing, dynamic tracks and focused Composer Undo/Redo.
- [x] Preserve source keys and metadata through processor resets and quaternion display-mode edits.
- [x] Resume pending exports, cancel safely and publish atomically.
- [x] Sample Mirror paths with source-compatible local animator ratios and input capture.
- [x] Capture and replay bounded frame caches across CPU, client, Studio and exports.
- [x] Expose the granted WAV File Watcher control and validate reloads and refused edits.
- [x] Decode bounded font bytes into glyph bitmaps and metrics.
- [x] Decode real outline and bitmap font distance fields with bounded tracked storage.
- [x] Integrate Flow and Bubble noise generators.
- [x] Add noise Generator/Computed modes with separate 1D/2D/3D coordinate and Scalar/Vector2/Vector3 output dropdowns.
- [x] Add union ports, darken incompatible sockets while connecting, and display compatible port IDs.
- [x] Extend source noise generators and their consumers to selectable typed field outputs; retain inherited choices through PXC saves.
- [x] Integrate bounded Cristal noise generation and typed input projection.
- [x] Integrate bounded Gradient Cube image and cross-section outputs; pass joined CPU validation.
- [x] Integrate bounded Surface Project 3D outputs and source getters; pass joined CPU validation.
- [x] Integrate bounded Refract controls, sampling and PXC edits; pass joined CPU validation.
- [x] Integrate bounded XDoG controls, pixels and mapped PXC edits; pass joined CPU validation.
- [x] Integrate bounded Kuwahara controls, pixels and mapped PXC edits; pass joined CPU validation.
- [x] Integrate bounded Blobify controls, pixels and mapped PXC edits; pass joined CPU validation.
- [x] Profile all nine Kuwahara and Blobify variants with pixel fixtures, allocation and byte counters; pass joined CPU tests.
- [x] Integrate camera capture routing and row receipts; pass joined CPU validation.
- [x] Integrate Studio camera rows and sequence previews; pass joined CPU validation.
- [x] Integrate path extension, flattening and smoothing.
- [x] Add bounded Path Bake with native save/reopen and downstream sampling; validate all 21 CPU workload profiles.
- [x] Add bounded Mirror Polar CleanEdge sampling with source-derived pixel fixtures; validate all 22 CPU workload profiles.
- [x] Add bounded source Spiral Path sampling with owned state and PXC edit, group and save/reopen checks.
- [x] Add bounded planar Repeat Path with all controls, local vector getters and native/PXC save/reopen checks.
- [x] Add bounded Tile Random with source dimensions, nearest CPU pixels and native/PXC save/reopen checks.
- [x] Add bounded Wave Path with source controls, owned sampling state and native/PXC save/reopen checks.
- [x] Add bounded Smooth Path with source handles, shared sample cache and native/PXC save checks.
- [x] Add bounded Repeat Texture with all three modes, source controls and native/PXC save/reopen checks.
- [x] Add bounded Draw Cross Section with source controls, fixed RGBA8 output and native/PXC save/reopen checks.
- [x] Add bounded Markov Gradient with ordered palette replacement, global frame seeds and native/PXC save support.
- [x] Preserve typed Any group ports and dynamic sockets; retain defaults, fanout, nesting and image pixels after ungroup and save/reopen.
- [x] Verify grouped Cache Clear through the real Studio host across all eight source gates.
- [x] Preserve native Collection instances, disabled-group lifecycle and ordinary tagged boundary routes.
- [x] Preserve reserved source selectors, common Update destinations and opaque routes through PXC save/reopen; validate captured common-socket lifecycle.
- [_] Execute all reserved source trigger and metadata routes with ordered native project-step state, including unsupported callbacks, wrapper profiles and required host observations.
- [x] Verify audited native common lifecycle and PXC owner save/reopen with bounded work and atomic refusal.
- [x] Expose authored group boundary sockets for canvas connections; preserve routes through save and reopen.
- [x] Preserve canonical cold Nine Slice state through frozen groups, native saves and logical drawing.
- [x] Preserve expression-backed Combine keys through PXC save/reopen and re-separation for supported vector ports.
- [_] Finish Draw Line 2 Points with captured shared shader state and matched primitive coverage; see the [source-state audit](mono.engine/imagegraph/docs/source-line2points-state.md).
- [x] Profile bounded source-family workloads in an optimized build with allocation and byte counters.
- [x] Save completed image previews as PXC thumbnails with atomic publication.
- [x] Preserve separate Mirror X/Y animators through group replay and PXC edits.
- [x] Integrate typed arguments across client, Studio and exports; pass joined CPU validation.
- [x] Select adjacent Dopesheet keys by double-clicking their gap, with one-step undo.
- [x] Add Dopesheet copy/paste, duplication and time actions with atomic undo; pass headless gesture tests.
- [x] Add animation-region creation, selection, settings and resizing with atomic undo; pass headless gesture and joined CPU tests.
- [x] Add bounded native wrapped-text sizing with real font metrics, source integer controls and exact observation overrides; pass joined CPU tests.
- [x] Add saved-image cache playback and Studio Cache, Remove and Match Length actions; pass PXC, pixel export, headless gesture and joined CPU tests.
- [x] Integrate ASE, ORA and Krita artwork actions and preserve their edits through PXC saves.
- [x] Integrate bounded font hosts and Text rendering across client, Studio and exports; pass joined CPU validation.
- [x] Integrate source timeline normalization, seeking and WAV length edits in Studio; pass joined CPU validation.
- [x] Preserve original PXC region records through edits and bind range exports to their owned project cursor.
- [x] Preserve project-clock cache observations and refresh captured inputs without replacing cached frames; pass joined CPU validation.
- [x] Preserve source timeline endpoints through native saves, PXC edits and integral-frame exports; pass joined CPU validation.
- [x] Verify real font decoding, Unicode Text pixels and font artifacts; profile all seven boundary workloads.
- [x] Add separate/combine X/Y controls with processed input receipts, numeric tuple playback and atomic undo; pass headless gesture and save/reopen checks. Live Studio verification remains open.
- [x] Round source hexadecimal arguments once; preserve prefix parsing and atomic overflow refusal.
- [x] Convert Number argument tuple carriers to source caught-array zero; preserve String values.
- [x] Convert Area and Curve Number arguments to caught-array zero; preserve exact enum integers and raw String values.
- [_] Add source-compatible argument handling.
- [_] Bind cache playback and loading to selected animation-region bounds.
- [_] Complete remaining Composer GPU checks.
- [_] Establish exact parity with a licensed reference build and matched captures.
- [_] Finish Composer font loading.
- [x] Verify shared-alias Dopesheet drag, Delete, copy, fractional scale, collisions, cancellation and undo/redo in headless gestures.
- [_] Verify shared-alias Dopesheet workflows in live Studio.
- [_] Finish Studio host workflows.
- [_] Finish audio workflows.
- [_] Finish cache groups.
- [_] Finish export formats.
- [_] Finish feedback seek/reset and fractional-key playback.
- [x] Preserve signed observed Cache clocks, Cache Array ranges and source resize ordering; pass joined checks.
- [_] Finish fractional cache playback.
- [_] Finish large-sequence playback.
- [_] Finish mesh consumers and bindings.
- [_] Finish remaining PXC edits.
- [x] Finish selected-cache Clear.
- [_] Finish text layout and rendering.
- [x] Add bounded source 3D recipe, geometry and projected raster stages.
- [x] Render bounded Draw Shape 3D surface, depth and rim outputs with explicit native projection and texture-array scheduling.
- [_] Integrate remaining 3D executors and verify licensed renderer parity.
- [x] Add bounded weighted Points Triangulate and preserve downstream corner weights.
- [_] Implement remaining catalogue executors.
- [_] Integrate Studio camera previews.
- [_] Integrate camera rendering.
- [_] Load saved frame caches.
- [_] Preserve X/Y animation through groups and PXC edits.
- [x] Profile native wrapped-text measurement workloads.
- [x] Validate Hilbert and Kisrhombille generators, grouping, image formats and bounded execution.
- [_] Validate all integrated changes with joined CPU tests.
- [x] Validate signed Text trimming and whole-batch work limits with joined CPU tests.
- [x] Verify Transform GPU pixels.
- [_] Verify catalogue controls, errors, animation and parity evidence.
- [_] Verify live Studio workflows.
- [_] Verify modified-project compatibility.
- [x] Verify skybox GPU pixels.

The [joined parallel checks](docs/pixel-composer-m0/native-parallel-composer-validation-2026-10-07.json) include Mirror Polar CleanEdge, cold Nine Slice and Combine PXC persistence: 5,848 C++ cases, 116 Python checks, 16 CLI checks and all 22 CPU workload profiles pass. Remaining catalogue executors, live Studio verification and licensed parity stay open.

The [imagegraph fundamentals batch](docs/pixel-composer-m0/native-imagegraph-fundamentals-validation-2026-10-08.json) passes all 6,080 C++ cases, including 3,650 core, 376 source IO, 1,159 Studio and 33 offscreen Vulkan cases, plus 116 Python checks, 16 CLI checks and all 26 CPU workload profiles. Supported native common lifecycle, PXC persistence, routing, comparison, Collection clones and editable group junctions pass joined checks. Previous listener failures now pass. Full catalogue callbacks, required host observations, live Studio and licensed parity remain open.

The [October 5 continuation checks](docs/pixel-composer-m0/native-validation-2026-10-05.json) record joined CPU suites, native wrapped-text profiling and scoped Vulkan checks for Composer, Transform, cameras and grouped skyboxes. Licensed reference parity and live Studio remain open.

Isolated passes do not establish combined acceptance. See the [native validation ledger](docs/pixel-composer-m0/native-validation-2026-10-02.json) and [reference gate](docs/pixel-composer-m0/reference-gate.md). Retained integration evidence: `.cache/build/dev/evidence/`.

- [_] fix on startup the studio widgets are unsnapped from studio viewport slots
- [_] fix so we can take ui elements out of the studio screen and onto other screens if possible

### v0.26

- [_] Check whether shaderc needs glslang's deprecated HLSL front-end; disable `ENABLE_HLSL` if unused.
- [_] Check whether shaderc needs glslang's deprecated HLSL front-end; disable `ENABLE_HLSL` if unused.
- [_] Remove dead engine code and obsolete compatibility paths.
- [_] Consolidate render hooks, nodes, graphs and graph visualization.
- [_] Profile CPU, GPU and memory costs; evaluate channel packing, cheaper computation and reduced precision against output quality.
- [_] Finish typed render hook acceptance:

  1. Verify exact pipeline, revision, world, view, snapshot and node identity, including cancellation and stale requests.
  2. Keep stable names, typed immutable contexts and bounded nonblocking readback. `view.camera` remains the explicit typed pre-view mutation exception.
  3. Add a second real consumer; profile bytes, allocations, GPU work, readback latency and dropped records in release.

### v0.27

- [_] Move camera and character control into a Lua `PlayerModule` with `CameraController`. Keep movement server-authoritative and install the scripts in new worlds.
- [_] Add per-script breakpoint history showing values across hits.
- [_] Show timing flame graphs and allocation/free totals for each breakpoint hit.
- [_] Expose mouse and keyboard automation to Lua through `AutomationService`.
- [_] Expand physics (`docs/future-work/physics-expansion.md`).
- [_] Add world streaming (`docs/future-work/world-streaming.md`).

### FUTURE

- [_] Implement the navigation plan (`docs/future-work/navigation-ai-system.md`).
- [_] Add pathfinding.
- [_] Support climbing and authored traversal zones in pathfinding.
- [_] Add a default glTF character with Unreal-style presentation.
- [_] Build example projects:

  1. Space Engineers-style asteroids and planets (`docs/FULL-PLANET-DEMO.md`).
  2. A medieval battle with AI armies, magic and effects.
  3. An AI village with jobs, personalities and relationships.
  4. Floating islands, villages, bridges and clouds.

- [_] Add localization.
- [_] Implement the terrain plan (`docs/future-work/terrain-system.md`).
- [_] Implement the character plan (`docs/future-work/character-system.md`).
- [_] Implement the VFX plan (`docs/future-work/vfx-system.md`).
- [_] Implement the input plan (`docs/future-work/input-system.md`).
- [_] Implement cameras and cinematics (`docs/future-work/camera-and-cinematics.md`).
- [_] Implement prefabs and packages (`docs/future-work/prefab-package-system.md`).
- [_] Implement procedural generation (`docs/future-work/procedural-generation.md`).
- [_] Implement sessions and social features (`docs/future-work/session-and-social.md`).
- [_] Implement the audio plan (`docs/future-work/audio-system.md`).
- [_] Review `docs/future-work/REVISIT_IDEAS.md` for work that can start sooner.
- [_] Package suitable tools as bundled Studio plugins or installable plugin packs.
- [_] Build a node-based terrain generator for meshes and voxels, with visible-face selection and portal-aware culling.
- [_] Add procedural terrain tools to Studio.
- [_] Add Unity import tools and asset-store support.
- [_] Evaluate C# scripting.
- [_] Add constraints.
- [_] Deferred `D00106`: add JavaScript and TypeScript breakpoints after choosing a QuickJS debugger backend. TypeScript source maps already exist.
- [_] Build an audio workstation.
- [_] Add a whiteboxing plugin.
- [_] Complete the UI feature set and customization.
- [_] Add visual UI authoring with aspect-ratio, panel and scaling controls.
- [_] Evaluate HTML/CSS-based UI authoring through virtual instances.
- [_] Add Figma imports.
- [_] Import Blender `.blend` files in the asset explorer.
- [_] Add RPG Maker imports.
- [_] Add Photoshop file imports.
- [_] Implement mobile support (`docs/MOBILE.md`).
- [_] Evaluate a Discord bug-triage MCP plugin with agent ownership, status updates and escalation for large changes.
- [_] Test Minecraft shaders and PBR texture packs using CDN-hosted assets.
- [_] Add experimental ModuleScript calls and value transfer between Lua and JavaScript VMs.
- [_] Add VR support, including Oculus Rift S.
- [_] Add Studio permissions for microphone and camera access.
- [_] Build a camera-based motion-capture example plugin.
- [_] Add ECS-based reinforcement-learning environments.
- [_] Move Roblox import tooling into the external port tool.
- [_] Evaluate baking removed LOD mesh detail into normal maps for a performance mode.
