# Full Planet Demo

## Goal

Build a single `SpaceEngine.luau` demo that generates and renders a complete procedural star system. The player flies freely from deep space to an orbital view and down to a planet surface without loading a separate scene. Pressing `Tab` opens a system map, lets the player select a body or landmark, warms the destination, then teleports the free camera there.

The target experience has three views:

1. A low orbital horizon with a glowing atmosphere, night side, stars and a distant sun.
2. A system map with complete spherical planets, orbit paths and selectable destinations.
3. A close surface view with cratered terrain, visible geological scale, atmospheric haze and fast six degree of freedom flight.

Everything authored for the demo lives in one Luau script. The engine supplies general rendering, mesh, task, input and UI primitives. The script defines every celestial body, terrain graph, atmosphere, cloud layer, orbit, landmark and visual rule.

## Non-goals

This first demo does not need survival mechanics, vehicles, procedural creatures, multiplayer travel, seamless landing gear physics, a voxel editor or planet destruction. It is a rendering and world streaming demonstration with a script controlled free camera.

The first system should contain one star, one terrestrial planet, one cratered moon and a sparse asteroid belt. More body types become configuration work after the underlying paths are proven.

## One-script structure

`SpaceEngine.luau` contains local tables that act as internal modules. It does not use `require`, load external Luau packages or need C++ demo code.

```text
SpaceEngine.luau
  Config              body definitions, seeds, generation rules and bookmarks
  Math                deterministic hashing, cube-sphere conversion and orbit math
  Graphs              terrain, biome, cloud and city-light graph definitions
  BodyFactory         builds a CelestialBody from Config
  SurfaceStreamer     cube-sphere quadtree, patch requests and eviction
  PatchBuilder        builds mesh, material weights and collider data
  AtmosphereRuntime   LUT state, clouds, sun and eclipse inputs
  SystemRuntime       orbital transforms and body activation
  FreeCamera          mouse, keyboard, gamepad and speed scaling
  SpaceMap            Tab UI, body selection and travel requests
  Diagnostics         LOD, queue, GPU memory and timing overlays
  Main                setup and frame loop
```

This boundary is deliberately inside one file. It keeps the demo easy to run while keeping each concern local enough to split into reusable script packages later, after the intended API has been exercised.

## Script authored data

The body definition is data first. A body composes small features instead of becoming a subtype with special case rendering code.

```luau
local System = {
	seed = 918273,
	sun = {
		id = "helios",
		radiusMetres = 696_340_000,
		colour = Color3.fromRGB(255, 224, 167),
		intensityLux = 127_000,
	},
	bodies = {
		{
			id = "aurelia",
			kind = "terrestrial",
			radiusMetres = 6_371_000,
			orbit = Orbit.static(Vector3.zero),
			rotationPeriodSeconds = 86_164,
			surface = {
				seed = 1101,
				graph = "continental-world",
				maxHeightMetres = 9_000,
				seaLevelMetres = 0,
			},
			atmosphere = {
				heightMetres = 100_000,
				rayleighColour = Color3.fromRGB(90, 150, 255),
				mieColour = Color3.fromRGB(255, 210, 175),
				density = 1.0,
			},
			features = {
				Feature.Continents.new({ ... }),
				Feature.CraterField.new({ ... }),
				Feature.Ocean.new({ ... }),
				Feature.Biomes.new({ ... }),
				Feature.CityLights.new({ ... }),
			},
			bookmarks = {
				{ id = "low-orbit", altitudeMetres = 120_000 },
				{ id = "crater-rim", latitude = -22.3, longitude = 41.8, altitudeMetres = 450 },
			},
		},
		{
			id = "khepri",
			kind = "rocky-moon",
			parent = "aurelia",
			radiusMetres = 1_737_400,
			orbit = Orbit.circular(384_400_000, 27.32 * 86_400),
			surface = { seed = 2202, graph = "cratered-moon" },
		},
	},
}
```

Graphs use the same style. A graph has a stable string identifier, a seed and a small typed node set. It produces height, biome masks, material weights, cloud density, city-light masks or other named outputs. Graphs are pure functions of planet-space coordinates and seed, so generation remains deterministic after streaming, teleporting or recreating a body.

## Coordinates and scale

Planets require two simultaneous coordinate systems.

| Space | Purpose | Precision |
|---|---|---|
| System space | Orbits, interplanetary distances and bookmarks | Float64 metres |
| Body space | Terrain graph input, spherical positions and landmark coordinates | Float64 metres |
| Render space | Camera-relative mesh and lighting transforms | Float32 metres |
| Patch space | Vertex positions inside one cube-sphere patch | Float32 metres |

The script keeps body and camera positions in Float64 metres. Each frame, the engine chooses a floating render origin close to the camera and submits every visible transform relative to that origin. Rebase only the render representation. Do not rewrite body-space data, graph inputs, orbit state or saved bookmarks during a rebase.

The active body is the closest body whose gravity or surface influence range contains the camera. The script may render more than one body at a time. Only the active body's near surface quadtree receives high detail patch requests and collision work.

## Surface generation and LOD

Each planet is a cube sphere with six faces. Every face owns a quadtree of square patches. A patch has a face, a quadtree coordinate, a LOD, a fixed resolution and deterministic bounds.

```text
camera movement
  -> visible body selection
  -> visible cube-sphere patch selection
  -> screen-space error evaluation
  -> split, retain or merge patch nodes
  -> generation request queue
  -> mesh and material upload
  -> GPU culling and draw
```

Use a fixed mesh resolution per patch, initially 33 by 33 vertices. Refinement increases patch count instead of vertex density. This keeps seams, memory use and generation cost predictable.

Each patch evaluates the surface graph at cube-sphere sample positions. The graph produces displacement, biome weights, coarse normal information and material parameters. The vertex shader adds high frequency normal and detail data at render time, so small features remain visible without forcing high mesh density.

The LOD selector uses projected patch size with hysteresis. A patch splits after exceeding the refinement threshold and merges only after dropping below a lower threshold. Adjacent patches may differ by one level only. Stitch the fine edge to the coarse edge, or add a skirt as the initial safe implementation. Later, add vertex geomorphing to remove split and merge popping.

Generation priorities are:

1. Patches covering the camera's likely next second of flight.
2. The active body horizon and visible surface.
3. Teleport destination patches.
4. Other visible bodies at orbital LOD.
5. Prefetch work that can be cancelled immediately.

Each patch request carries its body identifier, graph revision, LOD, face, coordinate, generation epoch and cancellation token. A finished request may commit only if that exact request remains desired. This prevents an old body or stale LOD result from replacing a newer patch after a fast teleport.

## Rendering

### Planet surface

The surface material needs triplanar detail mapping, height and slope based biome blending, ocean masking, ice caps, roughness variation, night city lights and sun driven PBR lighting. Surface graph outputs become material inputs rather than unique shaders per biome.

At system scale, draw one low polygon sphere or baked impostor for each distant body. At orbital scale, draw a low density cube sphere. Cross-fade into procedural patches as the camera approaches. This avoids a visible hole while the near surface quadtree warms.

### Atmosphere and clouds

Every atmospheric body gets a reusable LUT based scattering material. The atmosphere requires transmittance, multi-scattering, sky-view and aerial perspective look-up textures. The renderer composites it after opaque geometry using the scene depth buffer, then renders the sun disk and limb glow.

Clouds are a separate spherical shell. Start with a weather texture and two scrolling procedural density layers. A later quality tier ray marches the shell, uses temporal reprojection and receives eclipses and cloud shadows. Clouds must be optional and independently quality-scalable.

### Star field, moons and shadows

The star field is an unlit camera-centered sky shell. It does not move with floating origin changes. The sun is both a directional light and an emissive disk. Bodies occlude the sun and cast eclipse masks that affect atmosphere, cloud and surface lighting.

The moon uses the same body pipeline with no atmosphere. Its surface graph emphasizes impact craters, ejecta, regolith colours and high contrast roughness.

## Free camera

The camera is scriptable and does not use character control. Mouse controls yaw and pitch. Keyboard or gamepad axes control forward, strafe, vertical rise and roll. The scroll wheel changes speed logarithmically from walking pace to interplanetary pace.

Near a surface, the default mode holds the camera above the terrain using a surface height query. Holding a modifier switches to unrestricted flight. The camera uses a far plane and depth mode appropriate for space scenes, while camera-relative rendering protects nearby terrain precision.

The diagnostics overlay shows active body, altitude, speed, render origin, visible patches, pending jobs, generated triangles, GPU memory, frame time and GPU pass time.

## Space map and teleport

`Tab` toggles a full-screen map. It shows the star, orbit paths, bodies, moons, camera position and bookmarks. It is a 3D system view rendered into the UI or a dedicated camera layer, with a simple flat fallback for the first milestone.

Selecting a body opens its bookmarks. Selecting a bookmark starts a travel transaction:

1. Resolve its body-space destination and desired camera orientation.
2. Make it the pending active body.
3. Request a minimum ring of destination patches, atmosphere state and collision data.
4. Wait for the configured readiness threshold or show a bounded loading state.
5. Move the camera in system space and select a new floating render origin.
6. Retire unneeded patches only after the destination has become visible.

The readiness threshold matters. A teleport must never arrive at an untextured planet with no collision or a camera below the generated surface.

## Engine work already available

The demo can build on several current engine paths:

| Capability | Existing support | Demo use |
|---|---|---|
| Luau authored mesh data | `EditableMesh` and `MeshPart` are available to Luau | Build and replace procedural patch geometry |
| Editable mesh collision | Editable mesh changes can rebuild collision | Near surface flight and ground clearance |
| Fixed mesh LOD | Four authored or automatic mesh LOD levels, selected on GPU | Distant props and temporary body impostors |
| Mesh decimation | Automatic mesh LOD artifact generation | Static detail assets |
| Render graph | Typed render graph and compute scheduling exist in engine code | Basis for atmosphere, clouds and generation passes |
| Shader compiler | Shader source compiles through the engine asset and renderer path | Basis for runtime planet shader workflow |
| PBR renderer | PBR, mipmapping, emissivity, post-processing, occlusion culling and feature masks exist | Planet surface presentation |
| Spatial queries | Luau raycast, overlap and shape cast APIs exist | Surface following and selecting near geometry |
| Script camera and input | Scriptable `Camera`, `workspace.CurrentCamera`, task scheduling and input services exist | Free camera and map interaction |
| UI and instances | Luau can create and manage scene and UI instances | Map and diagnostics overlays |

# MISSING FEATURES

The engine does not currently expose the following capabilities to Luau as a complete supported path. These are required before a full planet demo can meet its flight, scale and rendering goals. Existing internal C++ types, shader tooling or fixed mesh LOD support do not satisfy the requirement until the script can safely create, control, observe and retire the resource.

## Build first

| Priority | Missing feature | Why the demo needs it | Required Luau surface |
|---|---|---|---|
| P0 | Large world transforms and floating origin | Float32 scene transforms lose usable surface precision at planetary distances | `LargeWorldPosition`, Float64 body and camera transforms, camera-relative submission, origin change signal and conversion helpers |
| P0 | Runtime cube-sphere patch LOD | Existing LOD chooses among four mesh artifacts. It does not own a view-dependent planetary quadtree, neighbor rules, request cancellation or patch residency | `PlanetSurface`, `PlanetPatch`, screen-space error policy, patch visibility result, split/merge control and patch lifetime statistics |
| P0 | Asynchronous procedural mesh generation and upload | Luau can author editable meshes, but generating and replacing thousands of patches on the frame thread will stall | bulk mesh write API, background job submission, cancellation, completion polling, upload fence, byte budget and safe stale-result refusal |
| P0 | Runtime shader and material authoring | Shader compilation and rendering exist in C++, but Luau has no supported API to create shaders, compile source or graphs, bind resources and receive diagnostics | `ShaderGraph`, constrained source compiler, `ShaderFuture`, compile diagnostics, `MaterialInstance`, named parameter and texture binding |
| P0 | Script render pass registration | The atmosphere and cloud composition need depth-aware custom passes. The render graph is currently engine authored | `RenderPipeline`, pass declaration, resource reads and writes, depth input, transient targets, pass enable state and pass timings |
| P0 | GPU compute dispatch | Terrain graph baking, LUT production and cloud work need GPU execution. Luau must not run pixel or texture loops itself | `ComputeKernel`, typed storage buffers and textures, dispatch dimensions, barriers, future, cancellation and validated resource limits |
| P0 | Runtime texture resources | Planet materials need generated height, biome, weather, LUT and cube textures with mips | create 2D, cube and 3D texture, upload or compute write, mip generation, residency state and release |

## Build next

| Priority | Missing feature | Why the demo needs it | Required Luau surface |
|---|---|---|---|
| P1 | Atmosphere resource and renderer pass | A convincing orbital horizon needs physically based scattering and depth-aware aerial perspective | `Atmosphere.new`, LUT generation status, sun inputs, body radius, density profile, quality tier and renderer binding |
| P1 | Cloud shell renderer | Volumetric or layered clouds establish planetary scale and visual motion | `CloudLayer`, weather graph inputs, shadow controls, quality tier, history policy and GPU budget |
| P1 | GPU driven procedural instance path | Rocks, vegetation, city lights and asteroids cannot become thousands of individual Luau parts | procedural instance buffers, culling, indirect draw submission and visible-count statistics |
| P1 | Planet surface query provider | The camera needs height and normal before full collision is available, including a target teleport point | graph height and normal query, async collider readiness, patch coverage query and nearest valid landing point |
| P1 | Teleport readiness contract | Moving a camera before critical assets are ready produces a blank or invalid destination | named readiness requirements, progress signal, timeout outcome and atomic camera handoff |
| P1 | Render profiling visible to Luau | The script needs to hold generation and cloud work within a frame and memory budget | per pass GPU timing, dispatch time, mesh upload bytes, VRAM resident bytes and culling counters |

## Build after the vertical slice works

| Priority | Missing feature | Why it should wait | Required Luau surface |
|---|---|---|---|
| P2 | Vertex geomorph and skirt free patch transitions | Skirts make the first surface correct. Geomorphing improves finish later | per patch morph factors and neighbor edge metadata |
| P2 | True volumetric clouds | Layered clouds establish the visual direction with lower risk | temporal history, motion vectors, ray march settings and cloud shadow resources |
| P2 | Eclipses and multi-body shadows | A visual refinement after bodies and atmosphere render correctly | body occluder list and eclipse mask resource |
| P2 | Persistent patch cache | It improves repeat travel but does not prove the generator | cache keys, validated on-disk storage, eviction policy and versioned graph hashes |
| P2 | Full 3D map render target | A simple UI map proves navigation first | secondary camera, offscreen render target and UI texture binding |

## API design constraints

The new APIs must keep C++ engine behavior in the engine and game policy in Luau.

- Every long running mesh, texture, shader, LUT and compute operation returns a handle that can be polled, cancelled and released.
- Every submitted request copies its input. Editing a Luau table after submission cannot alter queued work.
- Every resource has a stable string name or content key. Numeric identifiers must not cross saved data, graph definitions or bookmarks.
- Resource budgets are explicit. The script receives refusal or eviction information instead of unbounded allocation.
- GPU work executes through validated resource declarations. Luau cannot access raw Vulkan device handles or issue untracked barriers.
- Script supplied shader code must compile in a restricted, versioned language surface with bounded source, includes and resource declarations.
- Background generation preserves deterministic outputs. Scheduling may change completion time, never the mesh or graph result for a given request.
- Each API has a headless validation path where possible. Rendering APIs also require a Vulkan fixture and image based integration coverage.

## Delivery sequence

### 1. Space flight vertical slice

Expose large world transforms, a scriptable free camera and a low polygon planet with atmosphere placeholders. Prove a camera can fly from one million kilometres to one kilometre altitude with stable surface precision.

Acceptance: one Luau script creates a star, planet and moon, supports free flight and rebases rendering without visible position jitter.

### 2. Procedural surface vertical slice

Expose asynchronous mesh jobs and runtime patch LOD. Generate one cratered moon from a small deterministic graph. Use skirts first, a fixed patch resolution and a bounded resident patch count.

Acceptance: the camera flies from orbit to 100 metres above the cratered moon. No frame allocates an unbounded patch set, no seams expose the void and stale jobs never replace current patches.

### 3. Planet renderer vertical slice

Expose material, shader graph, texture and render pass APIs. Add a terrestrial surface material, ocean mask, atmosphere LUTs and simple cloud layers.

Acceptance: the planet has a readable day and night boundary, blue atmospheric limb, terrain haze and a surface that remains stable through LOD transitions.

### 4. System travel vertical slice

Add orbit updates, the Tab map, bookmarks and readiness-gated teleporting. Add one moon and one asteroid field.

Acceptance: selecting a moon bookmark warms its destination and moves the camera there without a blank body, missing ground or a retained high detail patch set from the prior body.

### 5. Finish and measurement

Add city lights, vegetation or rock instances, higher quality clouds, visual polish and diagnostics. Measure in the release preset on a stated scene and record the workload, preset, device, resolution, active body, patch count and settings alongside each performance claim.

Acceptance: the system demonstrates all three target views, exposes actionable diagnostics and has scripted integration flights that cover space, map teleport and surface traversal.

## Tests

Tests should prove the boundaries that are easy to break:

- Deterministic graph output for a fixed body ID, seed, face, patch coordinate and LOD.
- Cube-sphere face adjacency, seam indices and patch bounds.
- Neighbor LOD difference never exceeds one.
- Patch request cancellation and stale result rejection.
- Floating origin conversion preserves body-space camera position and landmark placement.
- Teleport readiness refuses an incomplete destination and retires the old body only after a successful handoff.
- Shader graph validation rejects undeclared resources, invalid stage use and oversized input.
- Mesh and texture budgets reject excess allocations without corrupting resident state.
- Vulkan integration captures orbital horizon, system map and close terrain reference images.

The first implementation should add a narrow end-to-end script test that runs the demo through an orbital approach, opens the map, teleports to the moon and verifies the reported active body, ready patch count and camera altitude. This is the proof that the features work together rather than only in isolated engine tests.
