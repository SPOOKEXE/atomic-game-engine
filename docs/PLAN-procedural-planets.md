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

This is a proposed build plan, not a claim that the listed planet APIs exist.
Local source review on 2026-09-07 checked the two governing plans and the
editable-mesh preparation and commit path. The source episode transcripts are
not beside this document; episode tags retain the earlier draft's attribution.
Other engine facts below must be checked against the checkout before building.

The detailed contracts below refine the layer inventory. Proposed constants are
starting limits to measure, not existing engine limits or benchmark results.

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
  material parameters. Encode signed elevation around a fixed sea-level threshold
  on the CPU while writing UVs. No elevation uniform is needed, deleting the `MinMax` to
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
- **Document the period and units.** The kernel repeats every 256 noise-space
  units per axis. Planet noise samples a unit direction, so radius does not enter
  its frequency unless a recipe explicitly selects metre-space sampling. Check
  the full sampled span and every octave's frequency; `r * f < 256` alone cannot
  guarantee a pattern without visible repetition.
- **Reuse `core::Random` for every derived value.** It is indexed and pure, so
  `Float(index, salt)` is safe to call from `EachParallel` and gives the same answer
  whether the loop ran or one entity spawned alone. A stateful generator would look
  equivalent and quietly not be.
- **`core::ColorSequence` is the gradient type.** `Evaluate` is `constexpr` and
  already the engine's answer for a colour ramp. Do not mint a planet-local gradient.
  *(E05, E06)*

### Additions

- **Analytic derivative alongside the value.** An optional gradient return can
  support field normals when propagated through the full sampler. Keep it separate
  from the kernel move until parity and derivative checks pass.
- **A benchmark job for the sampler.** `just` recipe, executable under the build
  directory, measurements printed to the terminal. Measure on the `release` preset
  since first-party code is `-O0` by default.

## Layer L2 `parallel` and L3 `ecs`: work and change

### Fundamentals

- **One component per independently invalidated concern.** `Planet`,
  `PlanetNoise`, `PlanetColour` and `PlanetBiomes` as four components, so the
  per-column dirty bit identifies candidate concerns. Compare canonical recipe
  signatures per target before rebuilding: column dirtiness is not row-level
  proof of a semantic change. This retains E02's shape/colour split. *(E02, E06)*
- **Stage dependency contract.** Validate, sample, build positions and normals,
  encode surface UVs, prepare, then publish. LUT work depends on colour settings
  and can run independently. Shading uses recipe-stable height bounds; measured
  extrema are diagnostics, not changing inputs to every resident chunk. *(E05)*
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
- **Recipe ownership needs an explicit extension.** `scene::Terrain` is currently
  a one-per-world resource. The proposed per-body component above must become an
  instance of the same terrain recipe schema, not an independently owned copy of
  the world's seed and generator. Multi-body persistence and authority are an M0
  decision; one cannot obtain them just by registering a `Planet` class.
- **`scene::PlanetNoise` component: the layer table.** Bounded array of layer
  records rather than an array of objects, so the sampler walks one contiguous
  allocation. Per layer: `Enabled`, `FilterKind`, `Strength`, `BaseRoughness`,
  `Roughness`, `Persistence`, `Centre`, `MinValue`, `OctaveCount`,
  `UseFirstLayerAsMask`, `WeightMultiplier`, and a stable layer identity for seed
  derivation. *(E03, E04)*
- **A declared ceiling on layers and octaves.** The editor bounds controls;
  decoders refuse invalid counts before allocation. Do not silently clamp a
  replicated recipe into a different planet. Charge the product of sample count,
  layers and octaves at admission. *(hardens E03's 1 to 8 range)*
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
- **Published mesh bounds from tracked vertex extrema.** Unbuilt chunks instead
  need conservative field bounds for selection and culling. The host adapter
  writes derived publication bounds; property getters do not regenerate them.

## Layer L8: the generator module

### Fundamentals

- **Answer `CODE_ARCH.md` §8's six questions before minting anything.** Join the
  proposed **`mono.engine/terrain` at L8, shared**. Its governing plan explicitly
  excludes ECS, scene, render and VM dependencies. Accept owned values and return
  neutral artifacts; a higher host adapter reads scene state and publishes meshes.
  Do not turn permission to depend downward into a need to depend on `scene`.
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
- **Min-value threshold.** Preserve `value - MinValue` as signed field data.
  Clamp only the final visible radius at sea level for the initial ocean-covered
  surface. Clamping individual layers destroys depth before ocean shading can use
  it. *(E03, refined by E07)*
- **Ridged filter.** `1 - abs(noise)`, squared, for sharp peaks with valleys
  between. *(E04)*
- **Ridge detail weighting.** Each octave multiplied by a running weight seeded from
  the previous octave's value and clamped to `[0, 1]` through `WeightMultiplier`, so
  detail concentrates on high ground. *(E04)*
- **Filter dispatch by enum, not by interface.** The video's `INoiseFilter` plus
  factory becomes a switch on `FilterKind`, keeping the hot loop allocation-free and
  branch-predictable. *(E04, restructured)*
- **Layer masking.** Layers flagged `UseFirstLayerAsMask` multiply by the clamped
  first-layer mask specified below, so negative depth cannot invert them. *(E03)*
- **Evaluate layer zero once.** Cache it as both the elevation base and the mask,
  and start the accumulation at index one. This is the video's own optimisation and
  it matters more here, where the sampler runs on a worker under a budget. *(E03)*
- **Split unscaled from scaled elevation.** The raw signed value is what ocean
  shading needs; clamping at zero and applying `Radius * (1 + elevation)` happens
  only in the scaled form. Filters must not clamp. *(E07)*
- **Elevation range tracked during sampling** in derived diagnostics. It validates
  recipe bounds and supports the inspector. UVs use the fixed encoding contract
  below, so loading a mountain cannot recolour previously resident chunks. *(E05)*
- **Strict IEEE throughout.** Decision 14. No fast-math, no reassociation, no
  reliance on FMA contraction, or two hosts disagree about where the ground is.
- **Publish through the geometry transaction in the host adapter.** Convert neutral
  artifacts to `EditableMeshGeometry`, prepare on a worker, then commit with the
  observed revision. A single-mesh commit is not a multi-mesh transaction. Group
  validation and publication are specified below. *(replaces E01)*

### Additions

- **Face render mask.** Generate one face only, as `All`, `Top`, `Bottom`, `Left`,
  `Right`, `Front` or `Back`. Roughly a sixfold iteration speedup at high resolution
  and the single most valuable authoring affordance in the whole series. *(E04)*
- **Skip inactive faces**, so the masked-out five cost nothing rather than being
  generated and hidden. *(E04)*
- **Seam normal repair.** Evaluate normals from the same direction-space field on
  both sides of an edge. Halo samples are derivative inputs, not rendered skirts.
  Cross-face remapping and coarse/fine topology need separate rules below.
  *(E01, explicitly deferred by the video)*
- **Shared index buffers.** Indices depend only on resolution, never on the noise.
  Cache CPU templates by resolution and stitch pattern. The current geometry
  transaction owns its vectors, so copying a template is the initial bridge;
  actual shared GPU index storage needs a measured renderer change.
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

- **Adapt scripts to the shared generation request.** `EditableMeshJobs` supplies
  the existing single-mesh bridge and script resumption pattern. Headless native
  generation must not require a VM, and planet-wide atomic publication still needs
  a host-level batch path. *(replaces E01's synchronous rebuild)*
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

- **The proposed planet shader is a fragment `ShaderScript`**, named by `Material.Shader`
  and resolved first against a `ShaderScript` in the world, then against a built-in.
  An author's shader is an override rather than a separate mechanism. M0 must
  settle the shipping compiler policy and prove the material bindings. *(E05)*
- **Signed height arrives in `UV.y`, with sea level fixed at `0.5`.** Recipe-stable
  depth and land bounds encode each side separately on the CPU. The shader can
  recover the shoreline test without an unavailable elevation uniform. See the
  exact surface encoding contract below. *(replaces E05)*
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
- **Split-range selection in the fragment shader.** Decode the fixed shoreline
  threshold, then address texel centres inside the selected ocean or land half.
  Use a clear comparison; branchless syntax is not a performance requirement.
  *(E07)*
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
- **LUT allocation and repaint are separate.** Allocate or resize for missing
  images and dimension changes. Repaint when any gradient, tint or packed material
  value changes, even at identical dimensions. Resize clears the image. *(E05, E06)*
- **Colour updates reuse terrain samples and topology.** Gradient edits repaint
  the LUT; biome boundary edits update UVs. The current mesh transaction may still
  upload an entire changed mesh for a UV-only edit. Do not promise partial uploads
  before verifying or extending that path. *(E06)*
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

## Detailed build contracts

The layer inventory says where work belongs. This section says what a working
result must do. Keep the first vertical slice small, but settle the seams before
building more surface area. Grug has seen six pretty faces hide twelve cracks.

### Scope and ownership

The first planet is a radial heightfield: each direction from the centre has
one ground radius. Caves, overhangs and detached islands require the later SDF
domain. Do not promise those through a height query that cannot represent them.

The initial sea is the clamped outer surface of that field. It supplies colour
and smoothness, but no underwater ground mesh, refraction, buoyancy or fluid
simulation. A later separate ocean shell must state how seabed collision differs
from the visible water surface.

| Concern | Owner and boundary |
|---|---|
| Seed, generator identity, authored parameters | One terrain recipe schema, instanced for each body if M0 approves that extension |
| Planet radius, axis, noise and biome values | Planet-specific parameters in that recipe, exposed through scene properties |
| Canonical bytes and generation identity | Shared procedural-generation contract |
| Direction sampling and cube-face topology | Terrain domain, pure owned inputs and outputs |
| ECS reads, target revisions and accepted activation | Host adapter at the world mutation barrier |
| Job execution and cancellation | Existing shared request coordinator and fork-joined job system |
| Physical contact geometry | Collision artifact consumed by the ordinary physics path |
| GPU upload, material binding and retirement | Render/client consumers |
| Inspector, undo and preview state | Studio over canonical recipe operations |

Do not add a graph evaluator merely to generate the first sphere. A typed
generator recipe is already a supported form in the procedural-generation plan.
Later terrain graph nodes call the same samplers and mesher.

### Recipe fields and admission

Use named values with explicit units. Defaults below are proposed fixtures for
the first build. Engine ceilings may force lower limits; larger defaults need
release measurements and an updated resource estimate.

| Field or group | Proposed initial rule |
|---|---|
| Schema and generator version | Required, separate versions for encoding and numeric behaviour |
| Seed | Full unsigned 64-bit value, serialized without floating-point loss |
| Radius | Finite, strictly positive world metres; no physical maximum claimed until precision tests pass |
| FaceResolution | Samples per axis, default 33; full-face preview accepts 2 through 256 |
| ChunkResolution | 33 samples, hence 32 cells; alternate values must be `2^k + 1` |
| Noise layers | At most 8, stable identities, explicit order |
| Octaves per layer | 1 through 8; disabled layers incur no sample cost except the explicit first-layer mask case |
| Strength and frequency | Finite and nonnegative; reject overflow in octave frequency and amplitude products |
| Persistence | `[0, 1]` for the first supported filters |
| Roughness | Finite, at least 1; reject recipes exceeding the coordinate precision profile |
| Ridge WeightMultiplier | Finite and nonnegative; each resulting feedback weight clamps to `[0, 1]` |
| Biomes | 1 through 16; an empty authored list canonicalizes to a documented default biome |
| Biome StartHeight | `[0, 1]`, strictly increasing in serialized order |
| TintPercent and BlendAmount | Finite, `[0, 1]` |
| LUT ramp resolution | Default 256 per half, allowed 2 through 1024, with image pixel limits checked too |
| Depth and land encoding bounds | Finite, strictly positive dimensionless extents from sea level |
| Maximum quadtree depth | Initial safety ceiling 16, also limited by integer and numeric coordinate precision |
| Transform | Translation and rotation; positive uniform scale only for the first physical planet |

The editor may constrain a slider. A decoder or script API returns a structured
error for invalid input instead of silently producing different canonical bytes.
Canonicalization of defaults is versioned and runs before signatures are made.

Reject NaN, infinity, unknown filter kinds, duplicate layer identities, malformed
gradients and invalid transforms. Bounds checks happen before allocating arrays.
The same validation serves saves, replication, scripts and Studio imports.

An unchanged property assignment is a semantic no-op. An auto-update edit captures
one immutable recipe after the current edit transaction, not one request per
property setter. Dragging a slider coalesces superseded previews.

### Coordinate space and stable sampling

Noise uses planet-local unit directions. Changing radius scales the body without
moving continents. Rotation and translation change placement without changing
the planet's field signature; transforms still update world bounds and collision
placement through the existing instance path.

Keep these quantities distinct in APIs and diagnostics:

| Quantity | Meaning |
|---|---|
| Cube point | Coordinates on a canonical face before sphere mapping |
| Direction | Unit vector from planet centre, independent of radius |
| Raw elevation | Signed, dimensionless layered field value relative to sea level |
| Surface radius | `Radius * (1 + max(0, rawElevation))` in the initial ocean mode |
| Surface position | Direction multiplied by surface radius, in planet-local metres |
| World position | Transformed local position, used by physics and presentation |
| Biome latitude | `(dot(direction, biomeAxis) + 1) / 2`, before boundary noise |

Normalize a finite nonzero direction once at the query boundary. Reject zero,
nonfinite or overflowed length inputs. The internal sampler accepts only the
validated direction representation and performs no ECS access.

Compute chunk sample locations from integer face coordinates and dyadic ranges.
Avoid repeated floating-point accumulation across rows or down the quadtree.
Parent samples that coincide with child samples must take the same canonical
coordinate path, including identical operand ordering.

Define face order as `+X, -X, +Y, -Y, +Z, -Z`. Face indices are topology values,
not child creation order. Saved symbolic names must not depend on an enum's
declaration index or `core::Name::Id()`.

For a cube point `(x, y, z)`, the selected mapping is:

```text
s.x = x * sqrt(1 - y*y/2 - z*z/2 + y*y*z*z/3)
s.y = y * sqrt(1 - z*z/2 - x*x/2 + z*z*x*x/3)
s.z = z * sqrt(1 - x*x/2 - y*y/2 + x*x*y*y/3)
```

Pin the evaluation order and any final normalization in the generator version.
Clamp only tiny negative radicands caused by rounding inside the validated cube
domain. A materially negative radicand is an invalid coordinate, not a value to
hide with a clamp.

### Noise and layer semantics

Moving the shared kernel must preserve the old script result for positive,
negative, integer and period-boundary inputs. Seed offsets are an adapter around
that kernel, not a change to `math.noise` semantics. Huge integer seeds must not
first become a float.

Derive bounded seed offsets from stable layer identity and a named stream. Keep
them in a documented noise-space interval so large offsets do not erase sample
precision. Reordering layers may change accumulation and masking, but it must
not accidentally change each surviving layer's seed stream.

The initial filter contract is explicit:

1. Begin octave amplitude at 1 and frequency at `BaseRoughness`.
2. Sample direction times frequency, plus authored centre and seed offset.
3. For fBm, remap the kernel value to `[0, 1]` before amplitude weighting.
4. For ridges, square `1 - abs(noise)`, multiply by the previous ridge weight,
   and derive the next clamped weight from that weighted ridge value.
5. Multiply frequency by `Roughness` and amplitude by `Persistence` each octave.
6. Subtract `MinValue` after the octave sum, then multiply by `Strength`.

The initial recipe does not divide by the amplitude sum. Changing that convention
later changes terrain and requires a generator version. Zero strength produces
zero contribution and can skip its noise work unless an independently defined
mask needs the underlying sample.

Layer zero contributes its signed filtered value only when enabled. When another
enabled layer requests its mask, evaluate layer zero even if its direct
contribution is disabled. The mask is `clamp(firstFilteredValue, 0, 1)`; an empty
layer list produces zero elevation. This deliberately fixes mask range rather
than letting values above one amplify mountains without an explicit strength.

Sum enabled contributions in authored order. No parallel reduction across
layers. Worker parallelism distributes sample locations, preserving the scalar
sampler's arithmetic order at each location.

An analytic noise derivative is optional. It must include octave frequency,
strength, mask product and any later warp chain rule before it becomes a surface
normal. An isolated Perlin gradient is not the planet normal.

### Normals and geometric seams

Begin with direction-space finite differences using a deterministic tangent
basis. Sample nearby directions, construct displaced surface positions and take
an outward-oriented cross product. Choose and record a derivative step in the
numeric profile; it is independent of the currently visible LOD.

Identical directions use the same basis, step, field version and samples on both
faces. This gives shared edge normals without averaging across separately
published meshes. Validate the numerical error near basis-axis changes and at
the clamped shoreline, where the surface itself has a slope discontinuity.

The topology suite covers all 12 cube edges and all 8 corners. Each directed
edge needs a neighbour face, neighbour edge and parameter reversal flag. Derive
and verify this table from the face bases once; never guess orientation while
walking a live tree.

A one-ring halo must remap to the adjacent face or sample directions directly.
Applying the spherified formula blindly outside its cube-face domain is not a
halo policy. Rendered skirts cannot repair a normal mismatch and are not
collision geometry.

### Surface encoding and material contract

Let `e` be raw elevation, `D` the fixed depth extent, and `H` the fixed land
extent. Store the following scalar in `UV.y`:

```text
e < 0:  h = 0.5 * (1 - clamp(-e / D, 0, 1))
e >= 0: h = 0.5 + 0.5 * clamp(e / H, 0, 1)
```

Store the continuous biome row index `b` in `UV.x`, mapped to the row centre as
`(b + 0.5) / biomeCount`. The fragment shader uses `UV.x` for texture Y and
decodes `UV.y` for texture X. Names and axis swaps belong beside both encoder
and shader; an unlabelled `texture(lut, uv)` is wrong here.

For ramp width `W` and total width `2W`, map ocean `t = 2h` to
`(0.5 + t * (W - 1)) / (2W)`. Map land `t = 2h - 1` to
`(W + 0.5 + t * (W - 1)) / (2W)`. Select ocean for `h < 0.5` and land otherwise.
These coordinates sample texel centres and avoid accidental horizontal blending
across the two packed ramps. Exact sea level selects the land shoreline texel.

Linear interpolation between biome rows is intentional. Mips remain disabled,
and addressing clamps on both axes. Verify the actual material sampler supports
these settings; if not, add the narrow sampler support before marking M4 done.

Use recipe-stable `D` and `H`, authored or conservatively derived before chunk
sampling. Saturated samples increment a diagnostic counter. Never renormalize
all chunks from the extrema of whichever faces happen to be resident.

Proposed packing: RGB carries the gradient colour, alpha carries smoothness,
and the planet material is opaque. Ocean smoothness fills ocean texels; land
smoothness fills land texels. This is a proposed ShaderScript contract that must
be proved by the renderer probe. If alpha is consumed as opacity before the
custom shader, add a built-in material path or a supported property binding;
do not silently make the ocean transparent.

Specify the gradient's working colour space and editable-image upload decode in
M4. Encode and decode once. A two-colour known-value ramp checks against double
gamma conversion. No per-triangle vertex colour approximation substitutes for
this test.

Biome boundaries use local latitude plus bounded boundary noise. Clamp the
selection coordinate, compute the continuous row index in stable order, and
return the same normalized blend weights from gameplay queries. At an exact
weight tie, the lowest authored biome index wins the dominant-biome query.

The initial shoreline is interpolated across triangles. Very coarse triangles
can blend across a narrow island; this is a resolution limitation to measure in
the demo, not evidence that the shader can reconstruct unsampled terrain.

### Dependency and invalidation matrix

Each row describes the narrowest required rebuild. Existing whole-mesh upload
behaviour can make publication more expensive, but must not force resampling.

| Changed input | Reused | Rebuilt or updated |
|---|---|---|
| Seed, noise values, layer order | Index templates, unchanged LUT | Field, positions, normals, UVs, bounds, collision |
| Radius | Direction samples, raw elevation, LUT | Scaled positions, metric bounds, collision, LOD error scale |
| Translation or rotation | Object-space fields and meshes | Instance placement, world bounds, interests |
| Gradient, tint, smoothness | Fields, topology, positions, UVs | LUT pixels and material revision |
| Biome starts, axis or noise | Elevation and topology | Biome samples, UVs, gameplay biome data |
| Biome count or ordering | Elevation and positions | LUT dimensions/content and matching UV encoding together |
| Depth/land encoding bounds | Raw elevation, positions, collision | Encoded UVs |
| Chunk resolution or stitch pattern | Compatible field cache entries, LUT | Requested sampling grid or indices, mesh preparation |
| Camera movement | Recipe and resident artifacts | Visual interest and selected chunks |
| Collision interest movement | Existing canonical artifacts | Collision residency requests and activation set |
| Face preview mask | Full-world recipe and authoritative collision | Preview interest and visible debug faces |
| Enabled becomes false | Authored recipe | Cancellation and barrier-controlled removal of generated state |

Keep field, topology, material and collision signatures separate inside the
shared cache contract. Do not add a second private dirty-bit system to express
this table. A component dirtiness signal only triggers comparison of inputs.

### Request identity, lifecycle and publication

Use the shared generation envelope. The planet scope adds topology version,
face, depth, integer chunk coordinates, sample resolution and halo. Output keys
include field parameters, generator version, normal profile, encoding version
and requested output kind where each affects that output.

Target world, entity generation, ticket and expected publication revision are
lifetime checks, not content identity. Priority and camera distance do not change
the mesh. Hash canonical fields, never struct padding or process-local names.

```text
Captured -> Validated -> Queued -> Building -> Prepared -> Published
                     -> Rejected
Queued/Building/Prepared -> Cancelled | Superseded | Failed
Prepared -> DroppedStale
```

These are shared request states or an adapter view of them, not a new scheduler.
Every ticket reaches exactly one terminal outcome. A cached result still passes
the target and revision checks before publication.

Workers own snapshots and output buffers. They never retain an ECS pointer,
material pointer or script stack reference. Cancellation checks occur between
bounded rows or blocks and before each expensive downstream stage.

Before publishing a group, the world owner checks all destinations, entity
generations, recipe revisions, image revisions, counts, index ranges, finite
attributes and output signatures. `PrepareEditableMesh` currently checks matching
array sizes and indices; it does not prove finite coordinates or a planet budget.
Those checks belong in artifact validation before preparation.

The current geometry representation requires normals, UVs, colours and alphas
to match the position count. Fill neutral colours and opaque alphas even though
the shader's colour comes from the LUT. Empty optional-looking arrays would fail
the actual preparation contract.

Multi-object publication must have an all-or-none path. Prepare replacement
meshes, image data and collision artifacts before entering the mutation barrier;
preflight all destinations, then swap a complete generation through a group
primitive that cannot fail midway. Alternatively publish a fully staged hidden
generation by swapping its root references at the barrier. Neither guarantee
comes from simply calling `CommitEditableMesh` six times.

Whole-recipe replacement, changed biome row layout and a parent-to-children LOD
swap each define a publication group. Independent visual chunks of an already
accepted recipe can publish separately when coverage and edge constraints hold.

If a group fails, retain the prior accepted generation and report the failure.
If a target was destroyed or disabled, drop its results and release ownership.
On shutdown, cancel coordinators, join owned work and reject late completions.

Identical content emits a no-op receipt without changing mesh, material or
collision revisions. Do not repeatedly enqueue work just to discover this at
the final byte comparison.

### Quadtree identity and selection

Each of six roots covers one face. A node key is `(face, depth, x, y)`, with
`0 <= x,y < 2^depth`. Children are assigned a fixed quadrant order. The recipe
digest and topology version qualify this spatial key in caches.

Use 32 cells per chunk initially. Four children therefore share every other
boundary sample with their parent. Keep full-face preview resolution separate
from chunk resolution; 256 preview samples are not a stitchable 256-cell chunk.

Each chunk records conservative bounds and a geometric error estimate relative
to the finer field. A sampled maximum alone is not a proven bound on unsampled
noise. Use a documented conservative bound or keep refining with a clearly
labelled heuristic until the bounded estimator is ready.

Select visual LOD from projected geometric error using camera projection and
the nearest conservative chunk distance. Start with split above 2 pixels and
merge below 1 pixel as tunable defaults. Orthographic views need their own pixel
scale; a camera inside a bound uses the highest allowed demand, not division by
zero. Clip depth and demand against hard limits.

Merge demand from active main, portal, surface and editor views by taking the
finest requested level per region. Hidden views submit no interest. Apply
frustum culling first; add horizon culling only when relief-aware bounds prove it
safe for the current observer position.

Neighbour leaves differ by at most one level, including across cube faces.
Balance the selected tree before scheduling. Use fixed key order for ties and
reserve capacity for balancing neighbours, since a split may require more than
four new chunks.

### Crack-free transitions and residency

At equal LOD, shared edge positions must match exactly in canonical local
coordinates. At a one-level boundary, select a transition index pattern on the
fine edge so it joins the coarse edge's polyline. Simply sampling the same
smooth field at different densities still leaves T-junction cracks.

With four edges there are 16 coarse-neighbour masks. Verify each pattern and
corner combination at the fixed chunk resolution. Transition triangles must
retain outward winding and have no degenerate or duplicate triangles. Shared
positions, normals and biome encoding need separate checks.

Pin the parent while its children build. Once all four children, required
neighbour transitions and draw resources are ready, swap coverage as one group.
Merging reverses this: prepare and pin the parent before retiring its children.
Never draw both complete surfaces indefinitely and rely on depth testing to
hide overlap.

Initial split/merge swaps may pop while remaining watertight. Geomorphing is a
later visual improvement with its own seam and shadow checks. Skirts are an
explicit visual fallback for exceptional unavailable neighbours, with a bounded
depth and a visible diagnostic count. They cannot be the normal crack solution.

On a teleport, keep coarse coverage while high-priority chunks load. On memory
pressure, stop new refinements and merge only into ready parents. Eviction cannot
remove the only visible coverage, active collision or an in-progress transition
dependency. If even the minimum root set cannot fit, reject activation cleanly.

The scheduler merges identical chunk requests across views. Cache accounting
includes pinned, building, prepared, visible and retiring bytes. Cancellation of
one viewer must not cancel an artifact still required by another viewer.

### Collision, queries and authority

Visual demand follows cameras. Collision demand follows authoritative bodies,
their movement envelopes and the world's interest policy. A server without a
camera must still generate safe ground.

Choose a fixed collision sampling profile initially, independent of visual LOD.
Pin neighbouring collision chunks around moving bodies and prefetch enough for
the declared speed and generation latency budget. Teleports use an explicit
readiness gate; movement cannot proceed into a missing collision region.

The coarse render fallback is not permission to replace collision with a sphere.
Collision replacements and matching accepted recipe revisions activate at the
authority-selected tick. If preparation is late, retain the old accepted state
or delay activation according to the shared readiness policy. Worker completion
time cannot choose a simulation tick independently on each peer.

Triangle collision must agree on edges and winding, including cube corners.
Give simplification a maximum radial error in world metres. Character and ray
tests determine that bound; arbitrary decimation ratios do not prove safe ground.
No visual skirts enter collision output.

Split query semantics explicitly:

| Query | Contract |
|---|---|
| `GetElevationAt(direction)` | Proposed compatibility convenience returning surface radius in metres; document the name's ambiguity |
| `SampleSurface(direction, revision)` | Radius, local position, normal, signed raw elevation and biome weights for an identified accepted recipe |
| Active ground query or raycast | Uses currently active collision, returning its accepted revision |
| Preview sample | Uses the pending authored recipe and is explicitly labelled non-authoritative |

Analytic surface radius can differ from a decimated collision triangle between
vertices. Return source/revision information and state the approximation error;
gameplay needing contact uses active collision. Do not imply the two are exact.

Large query batches run through bounded native requests. Script callbacks never
execute once per sample. Both VMs expose the same limits and result meanings.

### Precision and reproducibility gates

Strict compiler arithmetic is necessary, but it alone does not establish
bit-identical `sqrt`, vector normalization or library behaviour on every target.
Declare `BitExact` only after canonical artifact vectors match across the
supported platform matrix. Otherwise adopt the shared `Quantized` profile with
an explicit grid before hashing and collision use.

Record the profile's numeric types, operation order, FMA policy, normalization,
noise offset algorithm and encoding rules. A profile change invalidates caches
and requires a generator compatibility version. Never fix a numeric mismatch
by suppressing the digest comparison.

Planet-local construction avoids distant-world translation error but does not
by itself solve a huge radius stored in floats. Before advertising walking on
planet-scale bodies, measure vertex and contact error versus radius and depth.
Chunk-relative render origins may help rendering; their support must be proved
in the existing instance layout. Physics needs its own supported origin policy.

Translation-only rebasing must not regenerate noise or change content signatures.
Test tangent motion, near-pole motion and cube-corner crossings at the largest
supported radius. State the tested radius and maximum positional error in the
demo documentation. Reject unsupported nonuniform or mirrored physical scale.

### Resource accounting and overload behaviour

Count allocations before submission using checked wide arithmetic. Admission
reserves output and scratch capacity, not only the final mesh's size.

For a full planet at `N` samples per face:

```text
vertices = 6 * N * N
triangles = 12 * (N - 1) * (N - 1)
indices = 36 * (N - 1) * (N - 1)
```

At `N = 256`, that is 393,216 vertices, 780,300 triangles and 2,340,900 indices.
Assuming 48 bytes per vertex across position, normal, UV, colour and alpha, plus
32-bit indices, the base CPU geometry payload is 28,237,968 bytes, about 26.93
MiB. This excludes vector overhead, field samples, halos, collision, staging,
GPU buffers and the previous live generation. Use actual `sizeof` values in the
implementation's estimator rather than assuming this layout.

For eight eight-octave layers, that full-face grid alone can require 25,165,824
kernel evaluations before normals or biome noise. A vertex cap by itself is not
a CPU budget. Finite-difference normals multiply sampling work substantially.

Start with the following configurable service policies, subject to measurement:

| Resource | Initial policy |
|---|---|
| Rebuilds per planet | One active recipe request plus one newest coalesced replacement |
| Expensive generation work | Charge estimated kernel evaluations, cells and output bytes |
| Worker cancellation unit | Bounded row/block with a measured worst-case duration |
| Owner-thread publication | Bounded bytes and groups per barrier, reserving a complete indivisible group |
| Visual refinement | Limited requests per frame with fair service across planets |
| Preview work | Lower priority than active collision and main-view coverage |
| CPU cache and scratch | Separate byte and entry ceilings, with live pinned bytes visible |
| GPU residency | Separate upload, live and retirement budgets |
| Collision | Reserved capacity independent of visual cache pressure |

Set hard byte ceilings and latency targets on named desktop and headless-server
hardware before M7 exits. Do not invent a universal millisecond promise in this
document. A request whose indivisible publication exceeds the maximum barrier
budget is refused or rebuilt with smaller chunks; it must not wait forever.

Backpressure yields an explicit queued, rejected or degraded-visual outcome.
No path silently exceeds a hard ceiling. Queue cancellation releases reservations
exactly once, and repeated edits cannot accumulate unbounded prepared buffers.

### Save, replication and API completion

Save canonical recipe parameters, generator/profile versions, stable body
identity, transform and supported edit references. Do not save GPU handles,
worker state, tickets or the current camera's selected quadtree as authored data.
Baked cache artifacts remain optional and verifiable against the recipe.

Replicate full-width seeds and stable names without lossy number conversion.
Luau and JavaScript need a shared full-width seed representation, such as the
engine's existing integer codec or canonical decimal text. Numbers above
`2^53 - 1` must not silently round through either scripting surface.

A late join receives the canonical recipe and active revision/activation state.
Version mismatch produces a clear unsupported-generator result. Do not let an
older peer approximate a newer physical field with whatever sampler it has.

`Generate()` returns or awaits the shared ticket contract. Completion reports
accepted revision and outcome, including unchanged, superseded and failed.
Setting `AutoUpdate = false` makes explicit generation the publication trigger;
queries must distinguish the authored draft from the last accepted recipe.

Studio undo/redo records recipe operations. It cancels superseded preview work
and restores canonical values without storing a second editable planet model.
Face masks and debug overlays are editor/view state unless the author explicitly
chooses a supported saved presentation setting. They never remove server ground.

The authoring view follows the project's visual defaults: true black background,
white primary text, dense controls and minimal copy. Show invalid fields beside
their controls, retain the last valid preview, and stop preview submissions when
hidden. Progress uses stage names and counters without continuously repainting
spinners, shimmer or pulse effects.

### Failure matrix

| Trigger | Required result |
|---|---|
| Invalid recipe or overflow estimate | Refuse before expensive allocation; retain accepted generation |
| Recipe changes during generation | Supersede old ticket; reject its late publication |
| One face or child fails preparation | Retain prior complete coverage; publish no partial group |
| Entity destroyed and handle index reused | Entity generation check drops old completion |
| LUT row count changes mid-build | Mesh and LUT revision checks prevent mismatched row encoding |
| Cache exhausted | Preserve pinned coverage and collision; stop refinement or refuse admission |
| Device lost | Keep CPU recipe/artifacts, rebuild residency through renderer recovery |
| World stops | Cancel, join owned work and retire resources without script callbacks into a dead world |
| Unsupported numeric or generator version | Fail explicitly before authoritative activation |
| Collision misses activation deadline | Keep previous accepted state or delay through authority policy |
| Finite field exceeds shading extent | Clamp encoding, report saturation; preserve true field and bounds |

### Verification and profiling

Tests belong with the public behaviour they prove. Use table-driven topology and
numeric cases in the relevant header suites, and a few integration scenarios for
publication, streaming and authority. Do not create dozens of one-line smoke
tests or tests that merely assert deleted implementation details stay absent.

| Suite area | Required evidence |
|---|---|
| Shared noise | Existing script vectors unchanged; seeded offsets stable; negative and period-boundary inputs covered |
| Recipe codec | Round trip, full-width seeds, canonical defaults, unknown versions, nonfinite values and checked overflow |
| Cube topology | Outward nondegenerate triangles, exact counts, all face adjacencies and corners |
| Field sampler | Empty layers, disabled first-layer mask, signed depth, ridges, stable layer order |
| Normals | Outward unit normals and bounded edge difference across faces and LOD |
| Surface encoding | Ocean-only, land-only, zero elevation, saturated extrema, one biome and row-centre addressing |
| Invalidation | Gradient edit performs zero noise sampling; radius change reuses dimensionless field |
| Publication | Partial failure, stale target, destroy/reuse, cancellation, no-op and one accepted revision |
| LOD topology | All 16 transition masks, cross-face balancing, split/merge coverage and winding |
| Collision | Body crosses face and chunk borders without falling, launch impulses or contact holes |
| Reproducibility | Serial/multiple worker counts, cold/warm cache, supported platforms and saved replay |
| Lifecycle | Repeated edits, world stop and constrained-cache traversal release all reservations |
| Script adapters | Same errors, ticket outcomes, seed values and query results in both VMs |

Use one curated visual scene with fixed seed and camera poses. Include a smooth
sphere, ocean planet, ridged planet, polar biome and deliberately sharp shoreline.
Inspect face edges under grazing light, transitions during motion and texture
filtering at distance. GPU behaviour is checked by running the real client.

Benchmark cold generation, warm cache, LUT-only edits, biome-UV edits and sustained
chunk traversal separately. Record release preset, hardware, worker count,
backend, resolution, layer/octave counts and cache state. Benchmark executables
live under the build output; measurements print to the terminal per house rule.

Use the engine's profiling scopes for validation, queue wait, sampling, normals,
meshing, preparation, collision, upload and publication. Report owner-thread
wall time separately from summed worker work. Expose stale drops, cancellations,
cache bytes, pinned chunks, kernel evaluations and upload bytes.

An idle accepted planet must submit no generation, upload no geometry or LUT
bytes, and dirty no collision state. A moving camera may update selection while
resident chunks remain unchanged. Measure those paths explicitly.

### Build slices and review boundaries

Each slice leaves a working demo or a directly callable headless path. Proposed
file names follow the module's conventions once those are read; do not create
public headers solely to mirror every paragraph in this plan.

| Slice | Concrete reviewable change |
|---|---|
| Contract alignment | Recipe owner decision, module graph edges, shader probe and numeric profile |
| Shared sampler | One moved noise kernel, compatibility tests, measured sampler job |
| Pure sphere | Neutral terrain mesh artifact, fixed six-face topology, headless numeric checks |
| Scene bridge | Owned conversion and publication adapter, neutral vertex attributes, no-op behaviour |
| Signed field | fBm, ridges, masks, bounds and surface queries using one sampler |
| Material path | Fixed UV encoding, LUT pixels, real shader/material binding and visual fixture |
| Full authored planet | Both VM adapters needed by the demo, save/load, face preview mask |
| Chunk topology | Integer keys, neighbour table, balanced tree and verified transition indices |
| Streaming | Shared requests, bounded cache, parent/child publication and multi-view demand |
| Physical activation | Camera-independent demand, collision groups and authority readiness |
| Authoring completion | Undo/redo, hidden-preview stop, diagnostics and demo documentation |

The scene bridge may begin with one mesh. It cannot claim planet-wide atomicity
until the group path lands. Likewise, a six-face render proves the sphere, while
the quadtree milestone requires actual chunk selection and transition evidence.

---

## Milestones, mapped to the roadmap

| # | Scope | Target |
|---|---|---|
| M0 | Ownership, shader transport, numeric profile, architecture edges and limits agreed through concrete probes | prerequisite |
| M1 | Perlin kernel lifted and seeded, `ComputeJobs` calls the shared one, duplicate deleted | prerequisite |
| M2 | Cube-sphere geometry, correct winding, spherified mapping, published via `EditableMeshJobs` | v0.23.1 |
| M3 | fBm and ridged filters, masking, signed threshold, measured extrema | v0.23.1 |
| M4 | LUT in `EditableImage`, proved material binding, sea-level-preserving height encoding | v0.23.1 |
| M5 | Biomes, boundary noise, blending, packed ocean ramp, ocean smoothness | v0.23.1 |
| M6 | `examples/Planet.luau`, `DEMOS.md` entry with GIF, face render mask | v0.23.1 |
| M7a | Quadtree selection, balanced neighbours, stitched transitions, bounded visual residency | required for v0.23.1 quadtree claim |
| M7b | Collision demand, physical seam checks, grouped authority-approved activation | before walkable planets, aimed toward v0.26 |
| M8 | Extended script queries and authoring tools; basic API parity lands with each earlier exposed feature | v0.24 |
| M9 | Expressive additions, selectively | v0.26 and after |

M1 through M6 deliver the procedural sphere demo. M7a is required before marking
"quadsphere, quadtree planet" complete. If the terrain roadmap cannot accommodate
that work in v0.23.1, update the release scope explicitly instead of calling six
fixed faces a quadtree. M7b gates physical use rather than an orbit-only demo.

| Milestone | Exit evidence |
|---|---|
| M0 | Decision records identify owners; a minimal shader probe proves channel and sampler behaviour; resource estimates exist |
| M1 | Old noise vectors pass; seeded vectors match across worker counts; release timing printed |
| M2 | Six faces have correct winding, counts, bounds and shared edges; headless and client publication work |
| M3 | One pure sampler drives vertices and queries; mask and signed-depth fixtures pass |
| M4 | Known LUT values render correctly; ocean threshold is fixed; no double colour-space conversion |
| M5 | One-biome and multi-biome fixtures pass; tint edits repaint; biome layout changes publish coherently |
| M6 | Reproducible saved demo, documented command and GIF; idle and face-preview behaviour measured |
| M7a | Recorded traversal crosses all face and LOD edges without holes; constrained-cache teleport retains coverage |
| M7b | Headless moving-body case and replay pass; late collision cannot activate opportunistically |
| M8 | Both VMs expose matching semantics; Studio undo, cancellation and hidden-preview checks pass |
| M9 | Each chosen extension has its own resource estimate, persistence semantics and acceptance evidence |

Run applicable module suites, architecture/tier checks and `just check` for code
delivery, followed by the repository checklist before a PR. This document edit
does not itself implement those milestones or require engine runtime tests.

---

## Decisions to close before implementation

These are proposed defaults and required evidence, not permission requests for
this document edit. Investigate each with a bounded probe before asking for an
architectural choice. Ordinary build details should not wait on a human.

| Decision | Proposed direction | Evidence or approval needed |
|---|---|---|
| Per-body recipe ownership | Reuse the terrain recipe schema for planet instances; retain the world terrain entry point | Resolve the governing plan's one-per-world rule, save identity and authority for multiple bodies |
| Module timing | Pure planet generator joins terrain at L8; a thin demo calls it | Reconcile early roadmap scope with terrain's later planet phase and approve dependency edges |
| Shader policy and transport | Prove fragment ShaderScript first; built-in material if shipping policy requires it | Inspect runtime compilation policy, alpha handling, UV precision and sampler controls |
| Mesh transaction groups | Host-level preflight and indivisible accepted-generation swap | Demonstrate failure of one destination leaves every live destination unchanged |
| Numeric compatibility | CPU canonical generation with a declared supported-platform profile | Compare canonical artifacts; choose explicit quantization if bit-exact parity is not achieved |
| Resolution and byte ceilings | Small fixed chunks, 256-sample full-face preview maximum | Measure preparation, upload, collision and peak overlapping generations |
| Noise period | Preserve existing kernel and use direction-space sampling | Inspect high-octave repetition; wider-period noise, if needed, is a separately versioned terrain mode |
| Large-body coordinates | Local geometry first, explicit tested radius ceiling | Prove render and physics precision before adding chunk-relative origins or advertising planetary walking |
| Physical ocean meaning | Initial clamped sea surface, no submerged gameplay | Separate seabed and water contracts before swimming, underwater placement or buoyancy |
| Collision simplification | Fixed profile with measured radial error bound | Walking/raycast traversal at intended speeds and chunk borders |

## Completion definition

The first full quadtree planet delivery is complete when an author can save a
recipe, regenerate it deterministically, orbit across every face and LOD boundary,
edit shape or colour through the appropriate narrow rebuild path, and recover
from cancellation or resource pressure without losing valid coverage.

It also requires bounded queues and memory, verified seam topology, coherent
mesh/material publication, both-VM parity for exposed APIs, an idle path doing
no generation, and the documented demo plus applicable engine checks.

Walkable planets additionally require M7b: authoritative collision readiness,
supported precision, consistent accepted revisions and contact tests across
chunk boundaries. Atmospheres, rivers and vegetation cannot substitute for that
evidence. Grug wants ground that stays under feet before adding more clouds.
