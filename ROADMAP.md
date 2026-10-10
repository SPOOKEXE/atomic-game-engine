
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

The milestone headings below are development labels. Not in line with project versioning.

### v0.25

- [x] Add typed physics, replication and portal observations with optional trace IDs, bounded per-world records and server JSONL export (physics and replication: `1dd7743d`).
- [x] Finish the measured engine stress pass and matched 200-client `RecoverRows` profiling. Retain the 4 m physics grid default; cell-size results vary by scene. The recovery candidate was rejected without a demonstrated gain. See the [stress audit](docs/ENGINE_STRESS_AUDIT_2026-09-22.md).
- [x] Validate the same-world follow-camera repair and extended tunnel walk with native presentation and render tests (`04315bcd`). Passed native dev/Vulkan validation on 2026-10-04: 32 presentation cases, the 64-frame tunnel sweep and 5 portal GPU cases.
- [x] Fix Studio widgets that start unsnapped from their viewport slots.
- [x] Allow Studio UI elements to move from the Studio screen to other screens.
- [x] Carry forward completed portal traversal and camera fixes.
- [x] Carry forward scheduler timing and Metrics fixes, plus physics, world, GUI and script fixes.
- [x] Carry forward Studio dock and window fixes.
- [x] Carry forward audio decode and mixer fixes.
- [x] Carry forward texture format, material shader and sequence fixes.
- [x] Bound PNG inflation before excess output allocation; preserve accepted output on malformed streams.
- [x] Carry forward benchmark reports and jobs.

### v0.26

- [x] Create bounded static 2D images with Source, Solid, Resize, Crop, Transform, Flip and Blend nodes, durable projects and named outputs.
- [x] Add Studio Image Composer editing, cached previews, undo/redo, save/open and ordinary `.atex` export; add `.imagegraph` baking to assetc.
- [x] Verify exported images through ordinary signed content delivery in `ParticleEmitter` and `ImageLabel`, plus CPU, editor and sanitizer checks.

- [x] Define strict document v2 named parameters and bindings, backward v1 reads, pure input resolution and bounded selected-output preparation for the seven 2D nodes.
- [x] Add the keyed L7 per-world `ImageGraph` owner and Luau `SetInput`, `GetInput` and `GetImage` paths using stable imagegraph references.
- [x] Add L12 GPU evaluation, dirty-cone caching and transactional publication through ordinary `TextureTable` images; preserve last-good output on refusal.
- [x] Add Studio live inputs, bindings, GPU preview and publish/apply to ordinary image slots.
- [x] Add bounded raw RGBA8, PNG and JPEG imports, raw Base64 export, `ContentId` consumers and the `ImageBuffers` sample.
- [x] Add a staged demo runner that captures each top-level script and world, checks client diagnostics and validates BMP output.
- [x] Verify the encrypted 1080p EditableImage snapshot and observed update preserve dimensions, full byte payload, colour space, `ContentId` and unrelated replica rows across default chunk and pacing limits (18,302 assertions).
- [x] Verify viewport checks (29 cases, 449 assertions) and final native viewport drag/resnap; repeat Play/Stop coverage passed 228 assertions in the strict-CI binary.
- [x] Pass `just preset=dev docs-check` with zero malformed comments and no undocumented public entities.
- [x] Finish three consecutive interactive Studio Play/Stop checks; restored orange graph output appears in ImageLabel and ParticleEmitter.
- [x] Verify isolated signed GUI Publish/Apply and mismatched-key refusal; six SHA values stayed unchanged, the candidate was absent and the previous Apply remained enabled.
- [x] Initial minimal ImageGraph baseline: strict CI passed 664 suites, 0 skipped and 0 failed; architecture had 50 modules, 6 programs and 36 layered modules.
- [x] Keep LocalScript-created objects and `Workspace` camera descendants local, unsaved and unreplicated; give each player private `StarterGui` copies with independent `ResetOnSpawn`.
- [x] Add protected generated `Workspace.Terrain` with helper and migration paths, opt-in `AuthoredAffordance`, cameras without a `SurfaceCamera` facet, `RenderEffects` None-default dropdowns and an authored portal/mirror grid.
- [x] Build the Tornado demo in pure world Luau over generic GPU field, particle, volume and force systems; verify the 50M-particle server/client run through the Studio HUD button and Bladeborne's character-following local camera with HUD visible.
- [x] Capture and pass all 77 top-level demos; pass GPU renderer checks (9 cases, 902 assertions) and consumer checks (4 cases, 617 assertions), and verify native viewport float and resnap behavior.
- [x] Keep GPU timing averages sample-aware and separate from CPU placement: sparse device spans average over returned samples, and late query durations stay on the GPU timing axis (`82ba4842`, `da06e334`).
- [x] Reuse drawable-source membership during presentation updates and hoist label-name lookup out of per-instance loops (`443e4588`).
- [x] Bound mirror captures by authored bounce depth; the one-bounce GPU fixture renders all 320 mirror-ball facets independently of portal depth (`956e76e9`, `cf75b379`).
- [x] Preserve complete instance births through transport pressure and loss; the stress test checks all 1,536 instances converge to the final changing value after one join (`813fd045`).
- [x] Ground the ReplicationStress player before admission; verify all 20,000 authored block states, grounded client camera visibility and final position convergence through the actual Studio link.
- [x] Group all built-in reflected properties with inherited `PropertiesTag` metadata and an `Unassigned` fallback; put collection tag add, rename, remove and undo in the Properties footer. See [Studio properties](docs/STUDIO_PROPERTIES.md).
- [x] Freeze paused Frame Graph counters and draw data, stop recorder collection while paused unless another profiling consumer needs it, reuse retained clipped flame indices and theme palette, and fit structural averages by actual parent relationships so child order does not hide gaps.
- [x] Admit tick exchange workers using each stage's measured cost, wake only workers selected for the active batch and report worker bodies only when they own tasks.
- [x] Replace inactive DOF, god rays, bloom, lenses, sky, fog and empty overlays with output-preserving copies or clears; skip empty G-buffer shader draws, depth linearisation, SSAO and deferred lighting only when cleared texture contents prove the result. Retain disabled environment outputs without compute, copy matching presentation images and omit cleared game UI blends. GPU fixtures cover active transitions, authored writes, retained UI, lighting captures and authored zero-depth exports.
- [x] Preserve cleared component write epochs for skipped presentation consumers, rebuild cached portal cuts and omit capture-label scans when no attribute rows exist.
- [x] Reuse render-batch scratch and dirty ranges, step shared GPU particle fields once per frame and keep inactive inputs out of environment cache keys.
- [x] Index capture surfaces by slot, avoid redundant ribbon restoration uploads and omit zero-strength ground-grid draws.
- [x] Preserve opaque depth in transparent passes when empty surface overlays skip rendering, and refuse failed image publications or pass starts in transparent geometry, mirror and portal capture paths without exporting retained stale images.
- [x] Retain Output filtering and text extents, clip visible rows and cache canonical Explorer searches while keeping computed-property searches live; preserve multiline layout and selection behavior.
- [x] Resolve built-in and runtime image references through the correct content owner before portal promotion, and refresh readiness when renderer resource revisions change.
- [x] Fit mandatory nested portal images to their existing fair pixel budget and regenerate their camera keys after sizing; keep optional seam radiance separate.
- [x] Hide only the displayed local player's body in Studio first-person and restore it in third-person; the real viewport GPU fixture also checks another player's visibility.
- [x] Preserve depth-tested SurfaceGui pixels through oblique captures with a bounded decal depth tolerance; GPU checks cover ordinary and authored shaders, close occluders and portal adoption.
- [_] Non-acceptance follow-up: audit aggregate replica snapshot and staged-byte ceilings for large EditableImage rows. The current path has a `uint32_t` total cap but no explicit assembled-image budget; per-image bounds and windowed staging are tested.
- [_] Security follow-up: audit dynamic schema and component-codec acceptance, plus client-local provenance and descendant filtering across replication and authored game saves.

The accepted implementation is `v0.26.0-imagegraph-minimal`. The preserved full Composer work remains on `v0.26.5-imagegraph-full` for user review.

### v0.27

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
}``` move all character management code to a "PlayerModule" script called CameraController.
Same with movement system, needs to be server authoritive but pure-lua so it can be changed.
When you create a new world/scene, it auto appends the scripts in.

- [_] breakpoint history list per-script (show each iteration of breakpoint, can see change overtime)
- [_] expand breakpoint system to also include profilers like the heap allocation and timed flamegraph, you can see bottlenecks per iteration then (e.g. we can setup a "total compute", "total memory alloc", "total memory release", etc)
- [_] expose automation tools (AutomationService) like mouse clicks and keyboard inputs to luau scripts (so we can create ai that plays for you)

- [_] /docs/future-work/physics-expansion.md
- [_] /docs/future-work/world-streaming.md

### FUTURE

- [_] /docs/future-work/navigation-ai-system.md
- [_] pathfinding
- [_] more advanced pathfinding where you can specify wall climbing and stuff, like a "can climb" zone or stuff lik that for ai too

- [_] gtlf default character (unreal style)
- [_] project demos:
* space engineers asteroids + planets full demo (`docs/FULL-PLANET-DEMO.md`)
* huge medieval battle full ai war, ai magic battle with tons of particles and explosions and whatnot
* ai village with daily tasks, occupations, relationships, and things like that (dwarf fortress style - personality, occupation, etc).
* floating islands with village houses on them with bridges connecting them together, floating above clouds, minecraft-like

- [_] localization support
- [_] /docs/future-work/terrain-system.md
- [_] /docs/future-work/character-system.md
- [_] /docs/future-work/vfx-system.md
- [_] /docs/future-work/input-system.md
- [_] /docs/future-work/camera-and-cinematics.md
- [_] /docs/future-work/prefab-package-system.md
- [_] /docs/future-work/procedural-generation.md
- [_] /docs/future-work/session-and-social.md
- [_] /docs/future-work/audio-system.md
- [_] go through docs/future-work/REVISIT_IDEAS.md for things we can do sooner.
- [_] maybe consider converting a bunch of custom tools to plugins and have them built-in to studio, or make a plugin pack as a extra release file you can import to a plugins/ folder in ~/Documents/atomic-game-engine/studio/plugins
- [_] (procedural, node-based) terrain generator (refer to discord references) - editablemesh, greedymesh, noise layers, node graph with previews, chunk-based, etc. Add voxel mode (which separates cardinal facing direction Fnt/Bk/Lft/Rgt/Top/Bott faces into groups - only renders the two groups it can see). Expand with surfacecameras, portals, etc, so it culls, occulusion culls, etc.
- [_] full procedural terrain studio tools
- [_] unity porting tools / unity shop
- [_] consider adding C# as another scripting langauge?
- [_] constraints system
- [_] deferred `D00106` - JavaScript and TypeScript breakpoints. The vendored QuickJS exposes no line hook and no debugger API at all, so this is a submodule decision rather than a feature. Asking for one on a .js/.ts chunk is refused with the reason, at the service, the gutter and the panel alike. **The TypeScript half of the entry shipped at v0.15 and is not part of this** - source maps are emitted and read, so the lines a debugger would land on are already the right ones.
- [_] full audio DAW (digital audio workbench) system
- [_] built-in whiteboxing tools (planning) for building (plugin)
- [_] full ui feature buildout + custom
- [_] ui creation tool, full aspect ratio scaling, select how it scales, how panels scale, etc. easier version of tooling than manually building them out
- [_] html-based ui creation (html-script?) => auto handles aspect constraints and whatnot as well, css as well. "virtual container" that makes/simulates the instances?
- [_] figma import tools
- [_] import blender files in asset explorer natively (drag .blend files on engine)
- [_] rpg maker port tool
- [_] photoshop file reader and import tool
- [_] docs/MOBILE.md implementation
- [_] concept idea: setup a public mcp repository in python, add .mcp.json in project folder that loads it, it watches forums channels in the discord server for new/existing bugs. agent writes a message in the channel stating you're fixing it, other agents work on other bugs. agents can write that "this bug is a big rewrite" in the channel too which could be helpful. as a custom plugin? maybe just consider as a separate project.
- [_] could we try some minecraft shaders / pbr texture packs as test items? maybe upload to my cdn and then load it and ill check if it works
- [_] add modulescript boundaries between luau and javascript VMs. moving values between vms. add a container component flag to enable it. add a [experiment] marker.
- [_] VR support (oculus rift s)
- [_] setup a studio permissions system for: microphone, camera, etc
- [_] setup a example plugin for mocap with camera point track
- [_] ECS driven RL agent environments
- [_] Move "roblox files to atomic game files" to a external program - the port tool?

- [_] idea: for the LOD system, could we move mesh details into a normal map as part of the LOD? this way we can have a 'performance mode' that focuses on using this method instead of pure mesh data to show the details (even if it looks slightly uglier)
