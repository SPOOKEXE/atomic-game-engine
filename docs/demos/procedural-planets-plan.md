# Procedural Planets: implementation plan

Feature set derived from the Sebastian Lague *Procedural Planet Generation* series
(E01 to E07, transcripts in `transcripts/`), re-targeted onto the Atomic Game Engine
at `~/Documents/GitHub/atomic-game-engine` (v0.22.0).

## Status and positioning

`ROADMAP.md` already carries this work as **v0.23.1: "quadsphere, quadtree planet"**,
listed beside the new-demo block. v0.26 wants it again at product scale ("space
engineers asteroids + planets full demo"), and the FUTURE band names the
"(procedural, node-based) terrain generator" that this eventually feeds.

Two documents govern anything built here and this plan defers to both:

- `docs/future-work/procedural-generation.md`, which owns request identity, seeds,
  scheduling, cache keys and publication envelopes across every generating domain.
- `docs/future-work/terrain-system.md`, which proposes a `mono.engine/terrain`
  module at **L8, shared** and owns field, meshing and chunk semantics.

A planet is a terrain domain with a different topology, not a competing system.
`AGENTS.md` calls two ways to do one job the most expensive debt in the monorepo,
so this plan lands the quadsphere *inside* that lineage rather than beside it.

## House rules this document obeys

- **No em-dashes.** `just em-dash-check` runs inside `just check`.
- **No AI co-author credits or generated footers**, per `CLAUDE.md`.
- A new module is an architectural change: it needs a row in
  `mono.tools/architecture/expected_graph.json` with a `layer` and a `tier`, and it
  must pass `just test-architecture` and configure-time tier enforcement.
- Every module carries `AGENTS.md`, `docs/` and `tests/`. Read the module's
  `AGENTS.md` before touching it.
- One test file per public header, in that module's own `tests/`. A header needing
  a GPU gets no unit suite and is checked by running the client.
- Benchmarks get a `just` job and never write to file.
- `just check` is the gate. `/run-checklist` before a pull request.

## Reading order

Layers below are the engine's real heights from `docs/CODE_ARCH.md` §4.1, bottom
up. Inside every layer, **Fundamentals** land before **Additions**. Creative work
is quarantined in the last section so the core pipeline is finished and provable
first. `(E0n)` traces a feature to its source episode; untagged items are
engine-port work with no video equivalent.

---

## What the engine already provides

The single largest change from the first draft of this plan: most of the
infrastructure exists, and the work is mostly composition rather than invention.

| Video concept | Engine machinery that already exists | Where |
|---|---|---|
| `Mesh.vertices/triangles` assignment | `scene::EditableMeshGeometry`, `PrepareEditableMesh`, `CommitEditableMesh` | `scene` L7 |
| `mesh.Clear()` before reassign | `ReplaceEditableMesh`, revision-checked, no-op on identical content | `scene` L7 |
| Six `MeshFilter` children | `MeshPart` + `EditableMeshContentName` assigned to `MeshId` | `scene` L7 |
| Gradient asset | `core::ColorSequence::Evaluate`, `constexpr` | `core` L1 |
| Gradient baked to `Texture2D` | `scene::EditableImage`, row-major RGBA8, revision-tracked | `scene` L7 |
| Simplex noise script | a 3D Perlin already exists, `math.noise` compatible | `script` L9, private |
| Threaded generation | `script::EditableMeshJobs`, fork-joined, ticket-ordered commit | `script` L9 |
| Async noise grids | `script::ComputeJobs::SubmitNoise` | `script` L9 |
| Shader Graph PBR shader | `ShaderScript` holding GLSL, named by `Material.Shader` | `scene` L7 + `render` L12 |
| Settings as a shared asset | `scene::Terrain`, a recipe resource of seed plus generator name | `scene` L7 |
| Deterministic seeding | `core::Random`, indexed and pure, safe from `EachParallel` | `core` L1 |
| Change detection | `ecs::DirtyBits`, one bit per component column | `ecs` L3 |

---

## Corrections the engine forces on the Unity design

These are not preferences. Each one is a place where copying the video produces
something that compiles and is wrong.

- **Winding flips.** `render/AGENTS.md` pins `CULLMODE_BACK` with
  `FRONTFACE_COUNTER_CLOCKWISE`, so triangles are counter-clockwise seen from
  outside. The video's clockwise order draws the planet inside out, and the symptom
  is faces appearing and vanishing as it turns rather than anything that reads as a
  winding bug. *(inverts E01)*
- **There is no uniform bag, so `_elevationMinMax` has no home.**
  `scene::SurfaceAppearance` carries named maps and a shader name, not arbitrary
  material parameters. Normalise elevation on the CPU while writing UVs instead, and
  the shader needs no uniform at all. This deletes the whole `MinMax` to
  `SetVector` path. *(replaces E05)*
- **`ShaderScript` is fragment-stage only**, because a vertex shader would have to
  agree with the renderer's private instance layout. Every per-vertex quantity the
  shader needs must ride a channel the built-in vertex stage already forwards, which
  makes the video's "store it in the UV" trick mandatory rather than clever.
  *(hardens E06, E07)*
- **`ecs::DirtyBits` is one bit per component column, marked automatically by
  `GetMutable`.** Do not invent a bespoke bitfield of named stages. Split the
  settings into separate components and the per-component bit gives the shape-versus-colour
  invalidation split for free. *(supersedes the first draft's Layer A design; still
  delivers E02's split)*
- **`EachBatch` and `EachBatchParallel` set no dirty bit**, by design. A consumer
  needing row granularity over batch-written data folds a content signature, which
  is exactly what `EditableMesh::Signature` is.
- **Vertex colours will not substitute for the LUT.** `render::BuildMeshData`
  averages each triangle's three corners, so per-vertex colour is effectively
  per-triangle. The gradient texture is the correct path, not an optional one.
- **`scene::LevelOfDetail` is a four-level discrete mesh ladder** selected by
  `TargetQuadArea`, and decision 19 rules out virtualized geometry. A quadsphere
  quadtree owns its own chunk selection; it may use the ladder per chunk, but it is
  not the LOD system. *(corrects the first draft's D.2)*
- **Meshes are never replicated.** `scene::Terrain` states the rule: a derived
  artifact is regenerated from its recipe on both ends, because sending a conclusion
  instead of its input hands an attacker the half they choose. Replicate seed and
  settings only, and lean on decision 14's strict IEEE arithmetic for agreement.
- **The noise is Perlin, not simplex, and it is in the wrong place.** The kernel in
  `mono.engine/script/src/ComputeJobs.cpp` is private to `script`, has no seed
  parameter, and wraps its inputs with `fmod(coord, 256.0)` so the field has period
  256. Any planet sampler must lift and seed that one kernel rather than vendor a
  second. *(reshapes E03)*
- **The editor half is Studio, and Studio is client-only.** `mono.studio/nodegraph`
  is in the program band precisely so no engine module can link it. E02's custom
  inspector is Studio work, and it must not keep a second mutable copy of authored
  recipe state.

---

## Layer L1 `core`: values and determinism

### Fundamentals

- **Lift the Perlin kernel to a shared home.** Move the `Noise`, `Fade`, `Gradient`
  and `PERLIN_HASH` block out of `script/src/ComputeJobs.cpp` so a generator below
  L9 can call it. `ComputeJobs` then calls the shared one, and the duplicate is
  deleted rather than left beside it. *(E03)*
- **Seed by offsetting the sample point, not by permuting the table.** The hash
  table is what makes the kernel `math.noise` compatible, and Luau scripts depend on
  that. Derive a per-layer `Vector3` offset from the seed through `core::Random` and
  add it to the sample point. *(generalises E03's noise centre)*
- **Document the period.** `fmod(coord, 256.0)` means the field repeats every 256
  units. A planet of radius `r` sampled at frequency `f` must keep `r * f` inside
  that or the terrain visibly tiles, and the constraint belongs in a comment beside
  the sampler rather than in somebody's memory.
- **Reuse `core::Random` for every derived value.** It is indexed and pure, so
  `Float(index, salt)` is safe to call from `EachParallel` and gives the same answer
  whether the loop ran or one entity spawned alone. A stateful generator would look
  equivalent and quietly not be.
- **`core::ColorSequence` is the gradient type.** `Evaluate` is `constexpr` and
  already the engine's answer for a colour ramp. Do not mint a planet-local gradient.
  *(E05, E06)*

### Additions

- **Analytic derivative alongside the value.** An optional gradient return enables
  exact normals without a mesh-wide recalculation pass, and it is cheap to add while
  the kernel is being moved.
- **A benchmark job for the sampler.** `just` recipe, output under the build
  directory, measured on the `release` preset since first-party code is `-O0` by
  default.

## Layer L2 `parallel` and L3 `ecs`: work and change

### Fundamentals

- **One component per independently invalidated concern.** `PlanetShape`,
  `PlanetNoise`, `PlanetColour` and `PlanetBiomes` as four components, so the
  per-column dirty bit answers "did the shape change or only the colour" with no
  extra machinery. This is E02's split expressed the way the engine already tracks
  change. *(E02, E06)*
- **Stage ordering contract.** Sample, then elevation range, then mesh, then UV,
  then LUT, then upload. Later stages read only outputs of earlier ones. The range
  stage sits before the UV stage because normalising a height needs the whole
  planet's extremes, which is a genuine two-pass dependency the video hides inside
  its material upload. *(E05)*
- **Parallelise per face, per row.** The face grid is regular and every vertex is
  independent, so `Jobs::For` over rows is the natural grain. `DEFAULT_GRAIN` is
  4096; below the crossover parallel is slower, so measure on `release` and put the
  number in a comment, per rule 5.
- **Work inside a tick may be parallel, across ticks may not.** Decision 23. A
  planet rebuild that spans ticks is legal only because the mesh is derived data;
  the moment collision depends on it, publication has to land at a barrier.

### Additions

- **Content signature for batch-written state.** If any stage writes through
  `EachBatch`, fold a signature the way `EditableMesh::Signature` does, because
  those paths set no dirty bit.
- **Generation budget.** A cap on vertices rebuilt per barrier so a resolution-256
  planet spreads across several rather than stalling one.

## Layer L4 `world`: barriers and replication

### Fundamentals

- **Replicate the recipe, never the geometry.** Seed plus the four settings
  components go on the wire; every host runs the same generator and gets the same
  planet. `scene::Terrain` makes this argument at length and a planet is that with
  more vertices on it.
- **Generated state enters a world at an authority-approved barrier.**
  `procedural-generation.md` decision 8. Visual-only chunks may appear as they
  finish; anything physics reads may not.
- **Nothing crossing a world boundary is a pointer.** Rule 3. Every generation
  request and result is an owned copy, which `EditableMeshJobs` already enforces by
  taking ownership of the geometry at submit.

## Layer L7 `scene`: what a planet is

`scene` may see only `core`, `ecs`, `spatial` and `collision`, and holds no device
data and generates nothing. Everything here is storage plus resolvers.

### Fundamentals

- **`scene::Planet` component: the recipe.** `Seed` (u64, widest first),
  `Radius`, `FaceResolution`, `Enabled`. Modelled field for field on `scene::Terrain`,
  including the `Enabled` flag rather than an invalid generator name, so switching a
  planet off does not lose its settings. *(E01, E02)*
- **`scene::PlanetNoise` component: the layer table.** Bounded array of layer
  records rather than an array of objects, so the sampler walks one contiguous
  allocation. Per layer: `Enabled`, `FilterKind`, `Strength`, `BaseRoughness`,
  `Roughness`, `Persistence`, `Centre`, `MinValue`, `OctaveCount`,
  `UseFirstLayerAsMask`. *(E03, E04)*
- **A declared ceiling on layers and octaves**, clamped on read the way
  `TerrainSettings` clamps chunk extent. The cost is linear in their product and the
  failure is a frame nobody expected. *(hardens E03's 1 to 8 range)*
- **`scene::PlanetColour` component.** LUT width, the ocean `ColorSequence`, and
  the smoothness the ocean mask drives. *(E05, E07)*
- **`scene::PlanetBiomes` component.** Bounded array of `{ Gradient, StartHeight,
  Tint, TintPercent }`, ordered by start height, plus `BlendAmount`, `NoiseOffset`
  and `NoiseStrength`. *(E06)*
- **`Planet` authoring class.** Parents six `MeshPart` faces, one `Material`, one
  `EditableImage` for the LUT and optionally one `ShaderScript`. Register it through
  the ordinary class machinery so `Instance.new("Planet")` works and the schema dump
  picks it up.
- **Resolution ceiling.** The video's 2 to 256 came from Unity's 65k vertex limit,
  which does not apply here. Pick the ceiling from the geometry transaction's own
  cost and state the number: six faces at 256 is 393,216 vertices per rebuild.
  *(revises E01)*
- **Faces are `MeshPart`s named by `EditableMeshContentName`.** A generated mesh and
  a published one are interchangeable from the renderer's point of view, which is
  what `examples/EditableMesh.luau` exists to prove.

### Additions

- **`scene::PlanetFace` component** on each face entity: `LocalUp`, face index,
  and the quadtree node it represents once chunking lands. Keeps face identity out
  of child order.
- **Bounds from the tracked maximum elevation**, so culling gets a correct radius
  without walking vertices. Note that `Bounds` deliberately caches nothing derived,
  so this is written by the generator at publish and not recomputed on read.

## Layer L8: the generator module

### Fundamentals

- **Answer `CODE_ARCH.md` §8's six questions before minting anything.** The honest
  answers today: `terrain` would own this noun, it does not exist yet, the highest
  layer needed is `scene` at L7, every program that draws or simulates needs it, and
  it is not a leaf. That points at **joining `mono.engine/terrain` at L8, shared**,
  not at a fourth procedural module.
- **The cube-sphere generator.** Six planar grids inflated to a sphere. Triangles
  stay near-uniform, resolution is finely controllable, and faces subdivide cleanly
  for the quadtree the roadmap actually asked for. *(E01)*
- **Face basis derivation.** From `localUp`, `axisA` by component swizzle and
  `axisB` as `cross(localUp, axisA)`. An orthogonal basis per face with no lookup
  table. *(E01)*
- **Vertex grid mapping.** `percent = (x, y) / (resolution - 1)`, then
  `point = localUp + (percent.x - 0.5) * 2 * axisA + (percent.y - 0.5) * 2 * axisB`.
  *(E01)*
- **Flat index addressing.** `i = x + y * resolution`, so no running counter and
  every row is independently computable. *(E01)*
- **Triangle emission, counter-clockwise from outside.** Two triangles per cell,
  skipping the right and bottom edges whose triangles fall outside the grid. The
  index pairs are the video's with the winding reversed. *(E01, inverted)*
- **Exact buffer sizing.** `resolution^2` positions and `(resolution - 1)^2 * 6`
  indices, allocated once. *(E01)*
- **Spherified-cube mapping, not plain normalisation.** The video flags the even
  distribution as the better method and skips it; there is no reason to inherit the
  worse one at the point the code is first written. *(E01, noted but not implemented
  there)*
- **fBm filter.** `OctaveCount` octaves; frequency multiplies by `Roughness`,
  amplitude by `Persistence`, each octave remapped from `[-1, 1]` to `[0, 1]` before
  weighting. *(E03)*
- **Min-value floor.** `max(0, value - MinValue)` sinks low terrain into the base
  sphere, giving flat ocean floors and separated continents. *(E03)*
- **Ridged filter.** `1 - abs(noise)`, squared, for sharp peaks with valleys
  between. *(E04)*
- **Ridge detail weighting.** Each octave multiplied by a running weight seeded from
  the previous octave's value and clamped to `[0, 1]` through `WeightMultiplier`, so
  detail concentrates on high ground. *(E04)*
- **Filter dispatch by enum, not by interface.** The video's `INoiseFilter` plus
  factory becomes a switch on `FilterKind`, keeping the hot loop allocation-free and
  branch-predictable. *(E04, restructured)*
- **Layer masking.** Layers flagged `UseFirstLayerAsMask` multiply by layer zero's
  value so mountains grow only on continents. *(E03)*
- **Evaluate layer zero once.** Cache it as both the elevation base and the mask,
  and start the accumulation at index one. This is the video's own optimisation and
  it matters more here, where the sampler runs on a worker under a budget. *(E03)*
- **Split unscaled from scaled elevation.** The raw signed value is what ocean
  shading needs; clamping at zero and applying `Radius * (1 + elevation)` happens
  only in the scaled form. Filters must not clamp. *(E07)*
- **Elevation range tracked during sampling**, into the generator's own state.
  Needed by the UV stage, and never stored on a component beside its inputs, per
  `scene`'s standing rule about derived facts going stale. *(E05)*
- **Strict IEEE throughout.** Decision 14. No fast-math, no reassociation, no
  reliance on FMA contraction, or two hosts disagree about where the ground is.
- **Publish through the geometry transaction.** Build an `EditableMeshGeometry` per
  face, `PrepareEditableMesh` on a worker, `CommitEditableMesh` with the revision
  observed at submit. Identical content is a no-op, so an idle planet disturbs
  neither collision nor GPU residency. *(replaces E01's clear-before-reassign)*

### Additions

- **Face render mask.** Generate one face only, as `All`, `Top`, `Bottom`, `Left`,
  `Right`, `Front` or `Back`. Roughly a sixfold iteration speedup at high resolution
  and the single most valuable authoring affordance in the whole series. *(E04)*
- **Skip inactive faces**, so the masked-out five cost nothing rather than being
  generated and hidden. *(E04)*
- **Seam normal repair.** Normals disagree along shared cube edges, giving lit
  seams. `EditableMesh` gives one normal per vertex and cannot express a split, and
  the faces are separate meshes regardless, so the fix is to sample one ring beyond
  each face border and compute edge normals from that skirt. *(E01, explicitly
  deferred by the video)*
- **Shared index buffers.** Indices depend only on resolution, never on the noise.
  Build once per resolution and share across faces and planets.
- **Quadtree chunking.** Subdivide each face by camera distance, which is what
  `ROADMAP.md` v0.23.1 names. Chunks are derived, never stored and never sent, per
  `scene::Terrain`. Selection is the quadtree's own; `LevelOfDetail` is a per-chunk
  mesh ladder at most.
- **Collision through `scene::CollisionShapes`.** A decimated surface published by
  the one conversion that already exists, so client, studio and headless server
  agree about where the ground stops. Never build a hull in a property setter.
- **Domain warping** on the sample point, breaking up fBm's repetitive signature.

## Layer L9 `script`: scheduling and the script surface

### Fundamentals

- **Submit faces through `EditableMeshJobs`.** Six geometry values prepared as one
  fork-joined batch, committed in ticket order at the script barrier. It is
  deterministic, it already exists, and it is the third documented resume source
  beside bus replies and child waiters. *(replaces E01's synchronous rebuild)*
- **`editableMesh:SetGeometry(vertices, indices)` is the script-facing path.** One
  VM crossing for a complete face; the call yields and resumes when its ticket
  commits. A Luau planet demo needs nothing else to publish geometry.
- **Respect `ComputeJobs`' declared limits** if noise grids are dispatched through
  it: 1,048,576 samples maximum, eight pending requests, 4,096 samples served per
  heartbeat. Six faces at resolution 256 is 393,216 samples, which fits the cap but
  not one heartbeat.
- **Script property surface on `Planet`.** `Radius`, `FaceResolution`, `Seed`,
  `Enabled`, `FaceRenderMask` as scriptable writable properties, following the
  existing `class_property` conventions and the capability-parity requirement across
  both VMs. *(E02)*
- **`Planet:Generate()`** for scripts that batch many writes and want one rebuild.
  *(E02)*
- **`Planet:GetElevationAt(direction)`** returning scaled elevation along a unit
  vector, so content is placed on the surface without a raycast.

### Additions

- **`Planet:GetBiomeAt(direction)`** returning the resolved biome index and blend
  weights, for gameplay that reacts to terrain type. *(E06)*
- **Completion signal** when an async regeneration publishes, so scripts can place
  content afterwards rather than guessing.
- **Capability parity across Luau and JavaScript**, which decision 4 makes
  CI-enforced rather than optional.

## Layer L12 `render`, client tier: shading

### Fundamentals

- **The planet shader is a fragment `ShaderScript`**, named by `Material.Shader`
  and resolved first against a `ShaderScript` in the world, then against a built-in.
  An author's shader is an override rather than a separate mechanism. *(E05)*
- **Height percent arrives already normalised, in `UV.y`.** Because there is no
  uniform bag, the generator does the `inverseLerp(min, max, elevation)` on the CPU
  during the UV stage. The shader samples the LUT and does no range arithmetic.
  *(replaces E05)*
- **Biome coordinate in `UV.x`.** Computed per vertex and interpolated for free.
  *(E06)*
- **The LUT is a `scene::EditableImage`**, bound as `SurfaceAppearance::ColourMap`
  through `EditableImageContentName`. Row-major RGBA8, uploaded by
  `client::UpdateEditableImages` when the revision moves. *(E05)*
- **LUT layout: width `2 * resolution`, height `biomeCount`.** Ocean ramp in the
  first half, land in the second, one row per biome. One texture, one bind, one
  sampler, which is the video's own packing and is still the right call. *(E06, E07)*
- **Mipmaps off.** Filtered lower-resolution mips smear adjacent biome rows
  together and produce visible banding at distance. The video hits this bug on
  camera and the fix is the same here. *(E07)*
- **Clamped sampling.** Wrapping bleeds the top of the height range into the
  bottom. *(E05)*
- **Split-range remap in the fragment shader.** Ocean depth to `[0, 0.5]`, land
  height to `[0.5, 1]`, selected by a `floor()` of the shoreline test and combined
  with a weighted add rather than a branch. *(E07)*
- **Ocean smoothness mask.** The same shoreline value raises smoothness on water
  only, so the ocean catches a sun specular and the land does not. *(E07)*
- **Biome tint.** Blend each biome's gradient toward a solid tint by `TintPercent`,
  so one gradient serves several biomes. *(E06)*

### Additions

- **Biome boundary noise.** Perturb the latitude percent before biome selection,
  scaled by `NoiseStrength` and shifted by `NoiseOffset`. Straight latitude bands
  read as obviously artificial. *(E06)*
- **Biome blending.** Weight per biome as
  `inverseLerp(-blendRange, +blendRange, distanceFromStartHeight)`, accumulated with
  the running index scaled by `1 - weight` first so it cannot overshoot. *(E06)*
- **Blend-range epsilon.** Add `0.001` so a blend amount of zero does not collapse
  the inverse lerp. *(E06)*
- **LUT rebuild conditions.** Rebuild only when the image is missing, the width
  changed, or the biome count changed. `ResizeEditableImage` clears, so a resize is
  a full repaint and not an edit. *(E05, E06)*
- **Colour updates never rebuild geometry.** The UV and LUT stages are separate
  systems from the mesh stage, which is the whole reason the video splits
  `UpdateUVs` out of `ConstructMesh`. *(E06)*
- **Triplanar detail and slope shading**, so the surface holds up closer than the
  series ever goes.

## Layer L12 `examples` and the program band: demos and authoring

### Fundamentals

- **`examples/Planet.luau`.** The demo the roadmap actually asks for, in the shape
  `examples/EditableMesh.luau` established: a header comment saying what it proves
  and cannot be proven otherwise, then the scene. Run with
  `client --script Planet.luau`.
- **Update `docs/DEMOS.md`**, which is currently a TODO, and attach a GIF as
  v0.23.1's demo block asks.
- **Studio owns the authoring UI.** E02's nested inspector, change-check scope,
  auto-update toggle and manual generate button are Studio features over the
  recipe components. Studio must not keep a second mutable copy of authored state.
  *(E02)*

### Additions

- **A planet node set for the node graph editor**, once v0.23.1's `NodeCanvas`
  lands. Engine modules cannot link `mono.studio/nodegraph`, so the nodes are Studio
  side and the evaluator is the shared generator.
- **Face and quadtree overlay** through the existing `gui.Adornment` handles, to
  see chunk boundaries and the active render mask.
- **Per-stage timing readout** so the expensive stage is obvious while tuning.
- **Seed randomise**, for surveying the space one settings asset covers.

---

## Expressive additions, last

Only after everything above is complete and passing.

- **Crater filter**, a third `FilterKind` producing impact craters with raised
  rims, for airless bodies. Feeds v0.26's asteroid demo directly.
- **Gas giant variant**, banded and animated in the fragment shader with no
  displacement, driven by latitude and flow noise.
- **Polar caps by axial tilt**, deriving the biome axis from the planet's own
  orientation rather than world Y.
- **Rivers and erosion**, a post-pass carving drainage into the height field so the
  terrain reads as weathered rather than merely additive.
- **Vegetation scatter**, density-driven and filtered by biome, slope and altitude,
  published through the placement domain in `procedural-generation.md`.
- **Night-side city lights** as an emissive mask where the surface faces away from
  the sun.
- **Atmosphere binding.** `scene::AtmosphereProcedural` already carries
  `PlanetRadius`, `AtmosphereHeight`, `Rayleigh`, `Mie` and `Samples`; drive them
  from the planet's own radius and elevation range so the shell always matches the
  surface.
- **Cloud shell** through `scene::Clouds` and `scene::CloudCompute`.
- **Planet-relative gravity and orientation**, so a character walks the surface.
  This is the point at which chunk collision becomes a simulation input and the
  barrier rule stops being theoretical.
- **Ring systems**, a textured shadow-receiving plane with the planet's own shadow
  across it.
- **Seeded variant generator**, randomising the whole settings space under
  plausibility constraints to populate a system in one call.

---

## Milestones, mapped to the roadmap

| # | Scope | Target |
|---|---|---|
| M1 | Perlin kernel lifted and seeded, `ComputeJobs` calls the shared one, duplicate deleted | prerequisite |
| M2 | Cube-sphere geometry, correct winding, spherified mapping, published via `EditableMeshJobs` | v0.23.1 |
| M3 | fBm and ridged filters, masking, min-value floor, elevation range | v0.23.1 |
| M4 | LUT in `EditableImage`, fragment `ShaderScript`, normalised height in `UV.y` | v0.23.1 |
| M5 | Biomes, boundary noise, blending, packed ocean ramp, ocean smoothness | v0.23.1 |
| M6 | `examples/Planet.luau`, `DEMOS.md` entry with GIF, face render mask | v0.23.1 |
| M7 | Quadtree chunking, seam repair, collision, barrier publication | v0.23.1 to v0.26 |
| M8 | Script surface, both VMs, capability parity | v0.24 |
| M9 | Expressive additions, selectively | v0.26 and after |

M1 through M6 are what "quadsphere, quadtree planet" needs to be checkable in
v0.23.1. M7 is where the quadtree half actually lands and is the largest single
piece of work in the plan.

---

## Open questions for a human

These change the shape of the work and are not mine to settle.

1. **Module placement.** Join `mono.engine/terrain` at L8 as this plan assumes, or
   land the quadsphere first as a Luau demo over the existing `SetGeometry` surface
   and lift it into C++ when `terrain` arrives? The roadmap lists the planet in
   v0.23.1's demo block and `terrain` in v0.26, which argues for the second; "finish
   the thing" argues for the first.
2. **Decision 11 versus the runtime shader compiler.** The decision table still
   reads "no shader compiler ships on the client", while `ShaderScript` exists
   precisely so a world's GLSL compiles at runtime. If the planet shader ships as a
   built-in rather than a `ShaderScript`, this question does not arise.
3. **Resolution ceiling.** Unity's 65k limit does not apply. What is the actual cap
   on one `EditableMeshGeometry`, and does it want a declared constant beside
   `MAXIMUM_EDITABLE_IMAGE_PIXELS`?
4. **Noise period.** Is a 256-unit repeat acceptable for planet-scale sampling, or
   does the kernel need a wider domain, which would break `math.noise` compatibility
   for scripts?
