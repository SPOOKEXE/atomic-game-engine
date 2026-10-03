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

Pixel Composer foundations:

- [x] Retain the source licence, pinned metadata, enum choices and typed shader inputs; preserve static selectors and source records; save instance ports, graph selectors and cooked-shader references.
- [x] Evaluate bounded image, data, path and palette kernels, including polar mapping, surface/buffer conversion, Julia sets, Gabor noise, tile patterns and heightmap projection; preserve mapped inputs, surface formats and instance routes.
- [x] Replay animation, sequences, Gradients, Time Remap, FLIP/Verlet, Strand and rigid actors with bounded history; clear and refill mixed owned image cycles, retain random state, resolve FFT windows and rasterize trails/VFX.
- [x] Edit graphs, groups, keys and tiles atomically; scope Dopesheet Delete and canvas shortcuts to their panels; refresh HLSL sockets, cook shaders and retain Lua observations; preserve moved inputs, animator aliases and refused selections; insert Gradient/Matrix keys and save rigid meshes with metadata.
- [x] Read granted assets through bounded hosts; reload changed WAV files on the host frame clock.
- [x] Run bounded Lua and recorded-random export helpers; publish prepared image, sequence, WAV, CSV and tilemap outputs with rollback.
- [x] Evaluate bounded volume projection; bind rigid/3D outputs, mesh availability and skyboxes; render Composer shaders from cooked binaries and queue camera/SDF jobs. Keep live ShaderScript compilation. Cooked Composer Vulkan tests pass; broader GPU verification remains open.
- [x] Package source projects and authored scenes; profile scene, simulation, atlas, palette, filter, FFT, path and Strand CPU workloads. Visual acceptance remains open.

Pixel Composer [M0 to M7](docs/to-delete/v026-pixel-composer.md):

- [x] Add selected-key easing, dynamic tracks and focused Composer Undo/Redo.
- [x] Preserve source keys and metadata through processor resets and quaternion display-mode edits.
- [x] Resume pending exports, cancel safely and publish atomically.
- [_] Finish Mirror path sampling and separate X/Y animation.
- [x] Integrate Flow and Bubble noise generators.
- [_] Integrate path extension, flattening and smoothing.
- [_] Finish font loading, text layout and rendering.
- [_] Add source-compatible argument handling.
- [_] Implement remaining catalogue executors and verify controls, errors, animation and parity evidence.
- [_] Integrate frame caches, saved-cache loading and playback.
- [_] Finish audio workflows and feedback; verify seek/reset and fractional keys.
- [_] Integrate camera rendering and Studio previews.
- [_] Finish mesh consumers, remaining 3D nodes, bindings and large-sequence playback.
- [_] Finish Dopesheet gestures and Studio host workflows.
- [_] Add prepared thumbnails to PXC saving.
- [_] Finish remaining PXC edits, export formats and modified-project compatibility.
- [_] Validate all integrated changes with joined CPU tests.
- [_] Profile source-family workloads in an optimized build.
- [_] Finish GPU checks, including Transform and skybox pixels.
- [_] Verify live Studio workflows.
- [_] Establish exact parity with a licensed reference build and matched captures.

Isolated passes do not establish combined acceptance. See the [native validation ledger](docs/pixel-composer-m0/native-validation-2026-10-02.json) and [reference gate](docs/pixel-composer-m0/reference-gate.md). Retained integration evidence: `.cache/build/dev/evidence/`.

- [_] Finish measured engine stress optimizations and matched 200-client `RecoverRows` profiling. Retain the 4 m physics grid default; cell-size results vary by scene. See the [stress audit](docs/ENGINE_STRESS_AUDIT_2026-09-22.md).
- [_] Validate the same-world follow-camera repair and extended tunnel walk with native presentation and render tests (`04315bcd`).

### v0.26

- [_] Remove dead engine code and obsolete compatibility paths.
- [_] Consolidate render hooks, nodes, graphs and graph visualization.
- [_] Profile CPU, GPU and memory costs; evaluate channel packing, cheaper computation and reduced precision against output quality.
- [_] Finish typed render hook acceptance:

  1. Verify exact pipeline, revision, world, view, snapshot and node identity, including cancellation and stale requests.
  2. Keep stable names, typed immutable contexts and bounded nonblocking readback. `view.camera` remains the explicit typed pre-view mutation exception.
  3. Add a second real consumer; profile bytes, allocations, GPU work, readback latency and dropped records in release.

- [x] Add typed physics, replication and portal observations with optional trace IDs, bounded per-world records and server JSONL export (physics and replication: `1dd7743d`).

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
