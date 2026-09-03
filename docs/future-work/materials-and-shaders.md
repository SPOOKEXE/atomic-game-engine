# Materials and shaders plan

## Status

This document defines future work. It does not claim that the complete material
and shader system exists today.

The plan restores the repository architecture decision that authored shaders
compile while content is cooked. Studio may compile for preview and hot reload,
but a shipped client does not carry a GLSL compiler. A running game selects
published modules and changes bounded parameters. It does not turn arbitrary
source text into device code.

## Product goal

Authors can build reusable material assets, derive lightweight material
instances, write shader modules, preview changes immediately in Studio, and
publish validated variants for every supported device tier. A client receives
signed content, chooses a compatible cooked variant, creates bounded GPU state,
and falls back visibly when a requested path is unavailable.

The finished system provides:

- reusable PBR and custom-shader material assets;
- typed scalar, vector, colour, matrix, texture, sampler, and enum parameters;
- sparse per-instance overrides without copying the material definition;
- explicit texture roles, colour spaces, channel packing, and sampler state;
- separate vertex, fragment, and compute shader modules with declared entry
  points and interfaces;
- deterministic shader variants built from declared features;
- capability-based selection with authored fallbacks;
- Studio diagnostics, previews, and hot reload through the cook path;
- signed CDN delivery of source-independent cooked artifacts;
- content-addressed CPU and GPU caches with exact invalidation keys;
- render-pipeline integration without a second material resolver;
- matching Luau and JavaScript bindings;
- hostile-content bounds before allocations or device calls;
- headless format, compiler, reflection, and selection tests;
- image tests for the final device-facing behaviour.

## Current foundation

### Published materials

`assets::MaterialData` and the `AMT1` format currently carry seven optional
published texture names: colour, normal, roughness, occlusion, height,
metalness, and emissive. Versions one through four remain readable. Asset names
are bounded, and a material with no maps is a valid authored state.

`assetc` reads `.mat` source files, resolves their relative texture paths to
baked names, and emits `.amat`. Runtime code parses no source material text.
This source-to-cooked split remains.

`scene::MaterialCatalogue` resolves published material names to interned
texture names for one world. It is derived, is not saved, and clears entity
handles on load. `scene::MaterialRef` saves the `.amat` asset name and an
optional shader name. A material is currently an Instance child of the part it
affects.

### Textures and editable content

`assets::TextureData` carries a validated image and a derived mip layout.
`render::TextureTable` uploads textures by stable name, owns shared linear and
nearest samplers, tracks source dimensions, flipbook metadata, and uploaded
bytes, and caps content-owned device memory at 512 MiB.

`scene::EditableImage` and `scene::EditableMesh` are world-owned authoring
surfaces. A mutation increments a revision. Render and collision consumers
compare revisions and rebuild only changed content. Their generated
`editable-image://` and `editable-mesh://` names are process-local content
names, not publication identities.

These revision and transaction rules should be reused for procedural material
inputs. They should not be replaced by per-frame byte comparisons.

### Authored shaders

`ShaderScript` is currently an Instance containing `scene::ShaderSource` with
GLSL text and a write-count revision. Material, GUI, and post-processing demand
shader names from the world. `render::ShaderLibrary` resolves those names,
compiles changed source with `render::ShaderCompiler`, reflects capabilities,
and rebuilds changed pipeline variants.

Built-in shaders follow a different path. `Engine::resources` owns their GLSL,
the build compiles SPIR-V with `glslc`, and `shadercross` emits MSL from the same
module. `shadercheck` verifies SPIR-V structure and checks translated MSL
bindings. The device chooses SPIR-V or MSL at runtime.

The current live `ShaderScript` compiler is useful authoring machinery, but it
is in the wrong product. This plan moves it out of shipped clients and keeps it
in Studio and cook tools.

### Render pipeline

`graph::PipelineDocument` and render node declarations already describe pass
parameters, reads, writes, formats, requirements, and fallbacks. `render` owns
device handlers, targets, pipeline objects, capability probes, and per-pipeline
retirement. Authored raster and dispatch nodes currently accept inline GLSL or
a staged shader name.

Inline GLSL becomes an authoring convenience only. A cooked pipeline document
references a published shader module and variant. It never carries source that
a shipped client must compile.

### Delivery and trust

The asset system names content with BLAKE3-256, chunks it deterministically,
binds names and kinds into the manifest root, and signs that root once. Delivery
fetches and verifies bytes. The CDN origin has no signing key and does not parse
materials or shaders.

Material and shader publication uses this unchanged trust chain. There are no
per-shader signatures and no special shader download protocol.

## Non-negotiable rules

1. `assets` remains content addressing, format validation, and bytes. It does
   not decide render behaviour, parameter meaning, device support, or fallback
   policy.
2. Authored shader source compiles during cooking. Studio may invoke the same
   compiler for previews. Shipped clients contain no shaderc front end.
3. Built-in and user shaders pass through the same validation, reflection,
   optimization, backend translation, and interface checks before publication.
4. A stable string name crosses a save file, manifest, world, or process
   boundary. Interned ids, descriptor indexes, pipeline handles, and GPU
   pointers remain local.
5. A material definition is immutable published content. A material instance
   is a reference plus sparse typed overrides, not a copied definition.
6. One parameter declaration is authoritative. Studio widgets, script types,
   cooked layout, validation, and device packing derive from it.
7. Static features create variants. Ordinary numeric, colour, texture, and
   sampler changes use data and do not compile another shader.
8. Variant counts, source size, module size, resources, textures, samplers,
   buffers, workgroup sizes, and instruction estimates are bounded before a
   device object is created.
9. Capability failure is resolved before a pass records. There is no silent
   node skip in the middle of a frame.
10. The renderer owns GPU objects and retirement. Scene, Studio, delivery, and
    scripts never hold device handles.
11. Hot reload is transactional. A failed cook leaves the last accepted
    preview active and attaches a diagnostic to the attempted revision.
12. No cache key omits an input that can change generated bytes or device
    compatibility.

## Ownership and layer cuts

The compiler and reflection code should leave `render`. A new engine library,
provisionally `mono.engine/shading` with the `engine::shading` namespace, owns
device-independent shader source compilation, SPIR-V validation, optimization,
reflection, interface schemas, and cooked shader container formats. It has no
SDL dependency and opens no device. Its exact layer and edges must be added to
`mono.tools/architecture/expected_graph.json` and pass both architecture
checks.

`engine::msl` remains the one SPIR-V-to-MSL translator. Cook tools call it for
Apple variants. `render` may read cooked MSL, but no longer translates authored
source on a shipped path. Built-in shader staging should eventually call the
same cook worker rather than maintain a parallel command chain.

| Owner | Responsibility |
|---|---|
| `scene` | Material references, material-instance overrides, shader asset references, revisions, and saved authored state |
| `assets` | Content hashes, chunks, manifests, signatures, asset kinds, and bounded byte containers |
| `shading` | Shader schemas, source compilation, reflection, validation, optimization, interface matching, and portable cooked records |
| `msl` | Deterministic SPIR-V-to-MSL translation and SDL binding-index rules |
| `bake` and `assetc` | Source discovery, dependency resolution, variant expansion, cooking, reports, and publication inputs |
| `delivery` | Fetch and verify material, texture, and shader bytes without interpreting them |
| `graph` | Pass declarations, resource formats, requirements, fallback names, and pure acceptance decisions |
| `render` | Device caps, material residency, texture and sampler residency, pipeline creation, binding, retirement, and draw submission |
| `resources` | Engine-authored shader source and staging names only |
| Studio | Material, shader, and pipeline editors, previews, cook jobs, diagnostics, and hot-reload transactions |
| script adapters | Matching Luau and JavaScript views over scene-owned material data and cook tickets where allowed |

The source compiler may be linked by Studio and content tools. It must not be
linked by the release client or server. A build check should inspect the release
target graph and staged files so this is enforced rather than remembered.

## Asset model

### Stable identities

Use separate names for separate facts:

- `MaterialAsset` names an immutable cooked material definition;
- `MaterialInstance` names a saved reference plus overrides in a world or
  another published asset;
- `ShaderModuleAsset` names one authored shader family and its cooked variants;
- `TextureAsset` remains the existing cooked texture;
- `SamplerPreset` names a reusable sampler declaration when it is not stored
  inline;
- `MaterialLayout` and `ShaderInterface` are content signatures, not authoring
  identities.

Renaming an authoring file changes its published name through an explicit asset
move. It must not silently create an unrelated material while old worlds still
resolve the previous spelling.

### Material definition

A cooked material definition contains:

- format magic and monotonically increasing version;
- shader-module name and preferred technique;
- fallback material or fallback technique name;
- stable parameter declarations in canonical name order;
- default parameter values;
- texture bindings with role, expected dimension, colour space, and fallback;
- sampler declarations or sampler preset references;
- static feature selections;
- transparency mode, alpha cutoff, two-sided state, and shadow policy;
- render-state requirements that the shader interface permits;
- dependency names for textures, shader modules, and parent material;
- a layout signature derived from canonical declarations;
- optional author-facing labels and groups that do not affect device layout.

The existing seven-map `.amat` reads as a legacy standard-PBR material. Its
maps translate to well-known parameter names. Existing content does not need a
second legacy renderer.

### Material instances

A material instance contains:

- one parent material asset name;
- optional parent material-instance name for published instance chains;
- sparse overrides keyed by stable parameter name;
- a revision for world-owned live edits;
- optional local display metadata that is excluded from render signatures.

Resolve a parent chain once at admission. Reject cycles, excess depth, missing
parents, type mismatches, and overrides for unknown parameters. Flatten the
accepted result into one immutable resolved row and cache it by the complete
chain of content roots plus the local revision.

Limit published inheritance depth to a small constant. Deep inheritance is not
a feature worth turning every draw into a graph walk. World-owned instances may
reference one published parent and hold local overrides without becoming new
CDN assets.

### Parameter types

The closed parameter type set begins with:

- boolean;
- signed and unsigned integer;
- float;
- two-, three-, and four-component float vector;
- linear RGB colour and linear RGBA colour;
- matrix only where a declared shader interface requires one;
- texture reference with declared dimension and sample type;
- sampler reference or inline sampler value;
- stable enum token from a declared closed set.

Each numeric declaration may specify finite minimum, maximum, and editor step.
Every value has one canonical little-endian encoding. NaN and infinity are
refused unless a future parameter type explicitly defines them. Colours state
their storage and working spaces. They do not rely on a widget label to imply
sRGB conversion.

Parameter names are unique within a material layout and have fixed maximum
lengths. Unknown future parameter types cause that material version to be
refused, not partially loaded.

### Device packing

Cook reflection produces an explicit material uniform layout with offsets,
sizes, alignment, and descriptor bindings. Runtime code validates the cooked
layout signature against the selected shader variant before allocating.

The renderer packs resolved values directly into its device row. Scene does not
keep a second byte-packed copy. A packing format is versioned, and changing it
invalidates the cooked artifact and GPU cache key.

Small scalar parameters should use one bounded uniform block per resolved
material. Large arrays and runtime-sized buffers are not ordinary material
parameters. They require a declared render-node resource and separate limits.

## Texture sets and samplers

### Texture binding declarations

Every texture binding declares:

- stable binding name;
- semantic role such as base colour, normal, ORM, emissive, mask, or custom;
- dimension: 2D, 2D array, cube, or volume;
- sample type and channel expectations;
- colour space;
- whether a missing texture is allowed;
- fallback texture;
- whether editable images are accepted;
- expected mip policy;
- optional channel swizzle.

Packed ORM textures are explicit. A material does not guess that red means
occlusion because a file happens to have three channels.

Normal-map conventions are declared and normalized at cook time where possible.
The runtime does not branch per pixel on whether a source was OpenGL or DirectX
style.

### Sampler state

Sampler values include minification, magnification, mip filtering, address
modes, anisotropy, comparison mode, and a bounded LOD range. Unsupported values
fall through declared capability selection. They are not silently clamped into
a materially different result.

Sampler objects are deduplicated by the complete normalized sampler descriptor
and device identity. The current shared linear and nearest samplers become two
built-in presets rather than special cases each pass knows.

Do not create one sampler per material instance when many descriptors are
identical. Do not permit scripts to create an unbounded stream of almost-equal
samplers. Admission canonicalizes values and applies a per-device sampler cap.

### Editable textures

An editable image may override a texture parameter through its generated stable
runtime name. Its revision participates in the residency key. Changing pixels
reuploads that texture, not the shader and not unrelated material uniforms.

Editable images are never published by copying their process-local name. Baking
one writes a normal texture asset, gives it a manifest name, and changes the
material reference in one transaction.

## Shader modules

### Source model

The first source language remains GLSL. Each module declares:

- stage and entry point;
- include dependencies resolved inside an approved source root;
- named feature switches permitted to create variants;
- specialization constants permitted by policy;
- expected vertex inputs or fullscreen contract;
- fragment outputs and their formats;
- resources by set, binding, kind, and access;
- material parameters exposed to instances;
- required capabilities;
- authored fallback module or technique;
- source language and language version.

Includes are resolved, canonicalized, and hashed during cooking. Absolute paths,
parent traversal, symlink escape, network includes, and generated include names
are refused. Diagnostics map expanded lines back to source files.

Material shaders remain fragment-only until a public mesh and instance vertex
contract is designed. A custom vertex shader must not bind against private
renderer layouts by accident.

### Cooked module bundle

One published shader asset contains a bounded table of variants. Each variant
contains:

- canonical feature key;
- stage and entry point;
- optimized SPIR-V 1.0 words;
- translated MSL where supported;
- future backend forms only after their toolchains exist;
- reflected interface and capability requirements;
- resource counts and minimum buffer sizes;
- optimization report;
- compiler, translator, ABI, and container versions;
- source-dependency content roots for rebuild tracking;
- byte hashes for each backend payload.

The client validates the container, chooses one backend form, and creates a
device shader. It never reconstructs or recompiles source.

SPIR-V remains the canonical intermediate representation. MSL binding indexes
are assigned by `Engine::msl` according to SDL's descriptor rules and checked by
an independent tool. The MSL entry point remains `main0` where required.

### Interface matching

Cook rejects a material whose parameter layout does not match its shader. It
also rejects a pipeline node whose declared reads, writes, local sizes, or
uniform contract disagree with reflection.

Interface signatures include all fields that affect compatibility. Names used
only for diagnostics are excluded only when ordinal binding and type remain
unambiguous. A signature mismatch at runtime refuses the material or node before
device creation and selects its declared fallback.

### Variants

Features that may create variants are declared in source metadata. The cook
expands only combinations requested by materials and pipeline profiles in the
publication, plus declared fallbacks. It does not build the Cartesian product
of every switch.

Variant keys are canonical sorted feature-name and value pairs. Duplicate keys
with different output are a deterministic-build failure.

Set hard limits for features per module, variants per module, total cooked
shader bytes, and variants admitted per device. A cook report lists which
material or pipeline demanded each variant so an explosion has an owner.

Numeric quality choices normally remain uniforms or graph divisors. A boolean
becomes a static variant only when profiling shows the branch or resource
layout justifies another pipeline.

## Capability and fallback selection

### Two capability records

Keep device and shader capabilities separate:

- device capabilities describe what the active backend can create;
- shader capabilities describe what one cooked module requires and exposes.

Selection compares them without opening a device object. Required texture
formats, storage access, compute support, binding counts, colour targets,
workgroup sizes, and shader form must all match.

### Selection order

For a material or graph node:

1. find the exact requested technique and feature key;
2. filter its cooked variants by device and graph requirements;
3. prefer the device's selected shader form, with SPIR-V first where both work;
4. follow the authored fallback technique or node kind;
5. follow the fallback material if the complete material is unsupported;
6. use the engine missing-material marker and emit one bounded diagnostic.

Fallback chains are bounded and cycle checked at cook and load. Selection is
deterministic for equal capabilities. It does not depend on unordered-map
iteration or driver-reported extension order.

Studio shows the selected tier and every rejected alternative with its reason.
The release client counts fallback use by stable reason without logging every
draw.

## Cooking and publication

### Source graph

The cooker builds an explicit dependency graph:

- material source to textures, parent, and shader module;
- material instance source to parent and overrides;
- shader source to includes and declared fallback;
- pipeline document to shader modules and formats;
- mesh submeshes to material names;
- publication profiles to required capability tiers.

Cycles are refused before work starts. Inputs and edges are sorted by canonical
name so two cooks of the same tree produce byte-identical artifacts and reports.

### Cook stages

1. discover and classify source files;
2. validate paths, sizes, encodings, and source policy;
3. parse material and shader metadata;
4. resolve dependencies and capability profiles;
5. derive only demanded variant keys;
6. compile GLSL to SPIR-V;
7. validate and optimize SPIR-V;
8. reflect interfaces and compare declarations;
9. translate supported Apple variants through `Engine::msl`;
10. run independent shader checks;
11. serialize deterministic material and shader containers;
12. add artifacts and dependencies to the ordinary asset manifest;
13. publish chunks and sign the manifest root once.

CPU compilation of independent modules and variants may use
`parallel::Jobs::For` after the complete work set is gathered. Each worker owns
its output slot. Filesystem reads occur before the fork, writes occur after the
join, and deterministic report order follows the sorted work set.

The crossover for parallel compilation must be measured in release. A single
small shader should stay inline if dispatch costs more than compilation.

### Incremental cooking

The incremental key includes:

- normalized source and include content roots;
- compiler and optimizer versions and flags;
- target SPIR-V environment;
- translator version and MSL options;
- stage, entry point, and feature key;
- declared interface and policy version;
- backend capability profile.

A cached result is accepted only after its container and dependency roots are
verified. Diagnostics are cached with failed authoring revisions for Studio,
but failed outputs are never published as valid shader assets.

Changing one material default must not recompile a shader when its interface and
feature key are unchanged. Changing a texture's pixels must not rebuild a
material container when the published texture name still resolves to a new
manifest root through the normal publication graph.

### Asset kinds and containers

Append material-instance and shader-module kinds to `AssetKind` without
renumbering existing entries. The publisher decides kind once. Readers use the
manifest value and do not derive it again from extensions.

Every container has magic, version, bounded counts, bounded strings, checked
offset arithmetic, canonical order, and no partial read result. Unknown newer
versions are refused with a useful diagnostic. Unknown future manifest kinds
remain `Unknown` under the current forward-compatibility rule.

## Studio editing and hot reload

### Material editor

The material editor presents the declaration-derived parameter groups, texture
slots, sampler settings, static features, technique, and fallback chain. It
does not contain a hand-written widget switch for each built-in material.

The preview offers a small fixed set of meshes, lighting rigs, backgrounds,
exposure values, and capability tiers. The preview scene uses the actual render
pipeline, material resolver, and residency tables. A separate mock preview
renderer would become a second material implementation.

Edits update an authoring document. Applying a preview submits a cook ticket.
Only a successful, current ticket swaps the preview artifact. Undo and redo act
on authoring values, then schedule the same cook path.

### Shader editor

The shader editor provides:

- syntax and include diagnostics;
- compile errors mapped to source lines;
- reflected resources and interface mismatches;
- static instruction and module-byte estimates;
- optimization steps before and after counts;
- requested and built variant lists;
- per-tier compatibility and fallback explanations;
- a diff between the active preview revision and attempted revision;
- explicit compile and automatic debounced preview modes.

Static estimates are labelled as estimates. They are not GPU time, occupancy,
or bandwidth. The GPU profiler remains the evidence for runtime cost.

### Hot-reload worker

Studio owns a cancellable cook queue. Editing creates an immutable request with
source roots, target profiles, and revision. Work may run in another process so
a compiler crash or runaway memory use does not take Studio with it.

Results return owned bytes, diagnostics, dependency roots, timing, and the
request revision. Studio discards stale results. The render owner thread admits
accepted bytes at a frame boundary, creates replacement state, and retires old
GPU objects only after in-flight frames release them.

No worker writes into an ECS store or renderer. No compile continues across a
world tick as simulation work. Studio authoring jobs are outside deterministic
simulation.

### Shipping boundary

Saving an editable Studio world may preserve `ShaderScript` source for continued
authoring. Packaging a game must replace every runtime shader demand with a
published shader-module reference and verify that all demanded variants exist.

The release client rejects source-only `ShaderScript` execution with a clear
content diagnostic. A packaging check fails first, so this should only occur
for a malformed or manually edited game.

## Runtime residency and caching

### CPU asset caches

Delivery caches verified cooked bytes under the existing content root. Parsed
material and shader records are cached by asset content root plus container
reader version. Parsed records are immutable.

World resolution maps stable names to verified content roots from the active
manifest. A publication swap creates a new resolution generation. It does not
mutate parsed records held by a frame already in progress.

### Material residency key

A resolved material GPU row key includes:

- material definition content root;
- complete parent-instance roots;
- sparse override canonical bytes or world revision;
- selected technique and feature key;
- material layout and shader interface signatures;
- referenced texture roots or editable-image revisions;
- normalized sampler descriptors;
- active device identity and backend form;
- renderer ABI and packing version.

Do not use a material name alone. A name may resolve to different signed content
after publication changes.

### Shader and pipeline keys

A device shader key includes module content root, variant key, stage, backend
form, entry point, device identity, and render ABI. A graphics pipeline key adds
vertex layout, render-target formats, depth and blend state, sample count, and
specialization values. A compute pipeline key adds local sizes where they are
not fixed in the cooked module.

All key structs have equality tests that vary one field at a time. Missing one
field produces plausible stale pixels and is difficult to diagnose later.

### Budgets and eviction

Track separately:

- parsed material bytes;
- parsed shader bytes;
- texture device bytes;
- material uniform bytes;
- shader module device bytes where observable;
- graphics and compute pipeline counts;
- sampler count;
- pending upload and deferred-release bytes.

Eviction is least-recently-used among entries with no in-flight reference.
Release is deferred by the renderer's frame retirement mechanism. A single
artifact larger than a complete cache budget is refused rather than evicting
everything else for an entry that still cannot fit.

Cache misses, rebuilds, evictions, fallbacks, and refusals use bounded reason
names in metrics. Never generate a metric name from an asset or parameter.

## Render-graph integration

### Material consumption

The graph decides which pass and resource shape is required. The renderer
resolves the selected material technique into a cooked shader variant and
binding layout. The graph never receives GPU handles, and the renderer never
reinterprets the authored pipeline order.

Opaque, alpha-test, transparent, shadow, depth-only, and velocity techniques
are named entries in a material. A pass requests a technique by stable name.
The material supplies it or follows its declared fallback. Pass code does not
grow one switch per material family.

### Authored graph shaders

`raster` and `dispatch` nodes keep source fields in Studio documents for
authoring. Cooked pipeline documents replace them with shader-module names and
variant keys. Runtime install rejects a source field in a packaged document.

Node declarations and shader reflection must agree on:

- stage;
- read and write resource kinds;
- formats and access modes;
- sampler and storage bindings;
- uniform block layout;
- compute local size;
- required capabilities.

The independent graph validator can test this headlessly from cooked metadata.
Device creation remains the final backend check.

### Preview and visibility

Studio previews render only while their editor or graph tab is visible. A
closed panel, hidden dock, or inactive tab submits no preview work. A stale
preview keeps its last image and status without continuously repainting or
compiling.

## Script surface

### Runtime material API

Luau and JavaScript expose equivalent operations:

- read a part's material asset and local material instance;
- create a world-owned material instance from a published material;
- get parameter declarations and current resolved values;
- set and clear allowed overrides by stable name;
- set texture references, including an `EditableImage` where permitted;
- clone override data without copying published definitions;
- query selected technique, variant, and fallback reason as diagnostics;
- observe a material revision change;
- bake a world-owned instance to a publication candidate in Studio only.

Setters validate type, range, enum membership, texture dimension, and authority
before changing the ECS. An invalid set returns an error and changes nothing.
Bindings do not expose descriptor indexes, uniform offsets, content-hash
internals, compiler pointers, or GPU handles.

### Shader authoring API

`ShaderScript.Source` remains writable only in Studio or an explicitly enabled
trusted authoring host. Its preview methods return cook tickets and diagnostics.
They do not compile synchronously on the UI or simulation thread.

Runtime scripts may select a published shader module and declared feature key,
subject to the material or graph schema. They may not supply source, includes,
arbitrary SPIR-V, MSL, descriptor layouts, or unbounded specialization values.

### Binding generation

Parameter declarations drive TypeScript and Luau documentation metadata, but
material-specific parameter names remain data rather than generated global API
members. Generic `GetParameter` and `SetParameter` calls use a tagged value
union and return explicit errors.

Both VMs share the same engine method implementation. Cross-VM parity tests run
the same fixture materials and compare values and errors.

## Save, replication, and networking

### Saved state

Save:

- published material asset names;
- material-instance parent names;
- sparse override names, types, and canonical values;
- shader-module references and declared feature keys;
- editable authoring source only in Studio documents;
- revisions only when a consumer needs them after load.

Do not save:

- resolved parent chains;
- texture or material catalogue entries;
- compiled shader words duplicated from published assets;
- GPU residency keys or handles;
- descriptor indexes;
- device capability decisions;
- cache timestamps or eviction state.

The reader validates override counts and bytes before allocation. Unknown
parameters may be preserved as opaque authoring data in Studio when the parent
asset is temporarily missing, but a packaged runtime cannot activate them.

### Replication

Replicate material instance references and bounded override deltas. Large
textures, shader modules, and material definitions travel through signed content
delivery, not world replication.

A delta includes entity, material-slot stable name, base revision, next
revision, parameter name, tagged value, and authority. The receiver applies it
only if the base revision matches. Otherwise it requests the current bounded
override snapshot.

Clients cannot ask the server to compile shader source. A client-selected
material or feature must exist in the server-approved publication and schema.
The server validates the stable name before replication fans it out.

Visual-only material overrides may be client-local when the game marks the slot
as such. Gameplay must never read a client-only visual material as authoritative
state.

## Hostile content and safety limits

Every material, shader, manifest entry, and Studio worker result is untrusted at
its reader. Refuse before allocating or creating a device resource when any of
these exceed policy:

- source bytes, include count, include depth, or expanded bytes;
- material parameter count or total default bytes;
- override count or encoded bytes;
- texture binding, sampler, buffer, or output count;
- string, enum, and diagnostic length;
- inheritance or fallback depth;
- feature count or cooked variant count;
- SPIR-V words, instruction count estimate, block count, or capability count;
- descriptor set and binding range;
- declared uniform or storage-buffer minimum bytes;
- compute local size or total invocations;
- render targets and bytes per pixel;
- cooked container section count, offset, or total bytes;
- per-frame material changes, uploads, and pipeline creations.

Validate SPIR-V structure with the existing independent checker before device
creation. Reject capabilities and extensions outside the engine allow-list.
Custom shader code receives only declared resources. It does not gain file,
network, host pointer, or arbitrary device-address access.

Compiler workers use time, memory, output-byte, and process-count ceilings.
Cancellation is cooperative first and process termination is the final Studio
escape hatch. A failed worker cannot leave a half-written artifact under a name
the publisher treats as complete.

Diagnostics escape paths and source text before rendering in Studio. Logs cap
source excerpts and never dump complete proprietary shaders by default.

## Migration from current paths

Migration keeps one working path at every phase and deletes each replaced path
before the next compatibility layer becomes permanent.

1. Freeze current `AMT1` and `ShaderScript` fixtures and add conversion tests.
2. Extract device-independent compilation, optimization, reflection, and
   cooked records from `render` into `Engine::shading` without changing output.
3. Add deterministic shader-module cooking to `assetc` and Studio.
4. Teach `ShaderLibrary` to load cooked modules first while retaining current
   built-ins and runtime source as temporary fallbacks.
5. Extend the material container with typed declarations, defaults, techniques,
   shader references, and fallback data. Translate legacy seven-map material
   reads to the standard PBR schema.
6. Add material-instance components, resolution, canonical packing, and script
   bindings. Migrate `MaterialRef::Shader` into the material technique while
   keeping a read shim for old saves.
7. Move Studio `ShaderScript` edits through asynchronous cook tickets and feed
   accepted cooked bytes into preview residency.
8. Replace inline graph-node source in packaged pipelines with cooked module
   references.
9. Add release target checks proving shaderc, source includes, and source-only
   ShaderScripts do not ship.
10. Remove runtime source compilation and SPIR-V-to-MSL translation from the
    release renderer. Keep compiler code linked only by Studio and cook tools.
11. Convert built-in shader staging to the same validator and container format.
12. Delete old resolver branches, duplicate parameter tables, and temporary save
    shims after the supported migration window.

At no point may two independently authored material layouts or shader binding
rules remain active. Compatibility readers translate old data into the new
canonical structures and hand off to one runtime.

## Work phases

### Phase 1: contracts and extraction

- inventory all shader consumers and device binding contracts;
- specify material, instance, shader bundle, interface, and variant formats;
- extract `Engine::shading` and keep existing compiler tests passing;
- add release graph checks for compiler ownership;
- record baseline compile, reflection, pipeline-create, and steady-frame costs.

Gate: current images and shader diagnostics are unchanged, and no SDL or shaderc
type crosses a new public header.

### Phase 2: cooked shader assets

- implement source parsing, dependency roots, variant demand, and containers;
- compile SPIR-V, optimize, reflect, translate MSL, and run `shadercheck`;
- publish shader modules through the ordinary manifest;
- load cooked modules in render and select by device form;
- expose deterministic cook reports and failure diagnostics.

Gate: a packaged demo uses a user-authored shader with the runtime compiler
removed from its link graph.

### Phase 3: material definitions and instances

- add typed parameters, layouts, defaults, texture sets, and samplers;
- add sparse instances, parent resolution, overrides, and canonical packing;
- translate legacy `.amat` content into standard PBR definitions;
- add Luau and JavaScript bindings;
- add save and replication schemas.

Gate: legacy content renders through the new resolver, instance changes do not
compile shaders, and both VMs produce equal values and errors.

### Phase 4: Studio authoring

- build material and shader editors from declarations;
- add preview meshes, lights, tiers, and fallback explanations;
- run cooks in cancellable workers with revision checks;
- add undo, redo, dependency invalidation, and transactional preview swaps;
- stop preview work for hidden or inactive panels.

Gate: invalid source preserves the previous preview, reports exact diagnostics,
and causes no UI-thread compile hitch.

### Phase 5: graph integration and fallbacks

- cook authored raster and dispatch node shaders;
- validate graph declarations against reflected interfaces;
- add technique selection and bounded fallback chains;
- display material and shader compatibility in Render Pipeline tools;
- add missing-material and unsupported-tier diagnostics.

Gate: every enabled GPU node has a compatible cooked backend or refuses the
pipeline before a frame begins.

### Phase 6: budgets and release hardening

- add complete CPU and GPU cache keys;
- add budgets, eviction, deferred release, and metrics;
- fuzz every container and hostile shader boundary;
- run cross-platform image suites;
- profile large material sets, variant pressure, publication swaps, and hot
  reload;
- remove runtime compiler and obsolete compatibility code.

Gate: release clients ship no source compiler, all supported backends pass, and
budgets fail visibly without leaking or stalling indefinitely.

## Test plan

### Headless unit tests

- material and shader container round trips are byte stable;
- every older material version translates to the expected standard schema;
- malformed magic, version, count, offset, length, and order are refused;
- parameter values round trip for every type and reject invalid ranges;
- inheritance and fallback cycles, depth, and missing parents are refused;
- sparse overrides resolve without mutating their parent;
- layout signatures change for every compatibility-relevant field;
- variant keys are canonical and demand expansion avoids unused combinations;
- shader reflection matches declared parameters and resources;
- invalid shader source always returns a non-empty line diagnostic;
- invalid SPIR-V, forbidden capabilities, excess bindings, and workgroups are
  refused;
- SPIR-V-to-MSL bindings match the independent SDL rule checker;
- capability selection and fallback are deterministic;
- cache-key tests vary every field independently;
- stale Studio cook results cannot replace a newer preview;
- cancelled and crashed workers publish nothing;
- save and replication readers enforce all bounds;
- Luau and JavaScript fixtures return equal values and errors;
- release target inspection finds no shaderc dependency or source payload.

### Integration tests

- cook a material, textures, shader variants, and pipeline into one signed
  publication, fetch them through delivery, and resolve a draw;
- swap the publication while an old frame is in flight and verify both frames
  use internally consistent content;
- edit one shader include and rebuild only dependent variants;
- edit one material default without recompiling its shader;
- edit one texture without rebuilding unrelated materials or shaders;
- replace an editable-image texture and upload only its changed revision;
- exhaust each cache and prove live entries remain valid through eviction;
- select each capability tier and verify the declared fallback chain;
- package a source-only graph or ShaderScript and prove cooking fails;
- load a legacy world and prove it uses the canonical new resolver.

### Image tests

Use a fixed camera, fixed exposure, deterministic lights, and small canonical
meshes. Compare images with backend-specific tolerances for:

- every standard PBR map alone and in combination;
- sRGB and linear texture handling;
- normal orientation and channel-packed ORM;
- sampler filtering, wrap modes, mip selection, and anisotropy;
- opaque, alpha-test, transparent, two-sided, emissive, and shadow techniques;
- parent material and sparse instance overrides;
- authored shader, fallback shader, and missing-material marker;
- graph raster and compute material consumption;
- editable-image revision replacement;
- SPIR-V and MSL output on supported CI hardware.

A missing GPU does not become a fake unit test. Headless checks prove formats,
selection, and bindings. Device images prove pixels.

### Fuzz and soak tests

- fuzz material, instance, and shader containers from empty through maximum
  accepted size;
- fuzz SPIR-V reflection and MSL translation inputs behind process limits;
- repeatedly fail, cancel, and supersede Studio cooks;
- churn publication generations and material overrides under cache pressure;
- soak pipeline creation and retirement across many frames;
- verify live GPU bytes flatten after edits stop;
- verify diagnostic and cache tables remain bounded under unique hostile names.

## Profiling and budgets

Profile release builds and name the backend, GPU, driver, scene, publication,
material count, visible draw count, variant count, and cache state.

Record at minimum:

- source discovery, parse, compile, optimize, reflect, translate, check, and
  serialize time per module and variant;
- incremental cook hit rate and bytes avoided;
- Studio edit-to-preview latency split by queue, worker, admission, upload, and
  first visible frame;
- parsed material and shader resident bytes;
- texture, uniform, shader, sampler, and pipeline GPU residency;
- material resolution and uniform-pack time;
- pipeline cache hit, miss, create, and eviction counts;
- frame time with one, one thousand, and a deliberately high material count;
- draw-run splits caused by material, shader, sampler, and transparency changes;
- fallback and refusal counts by bounded reason;
- publication-swap retirement bytes and frames retained.

Add `ENGINE_PROFILE` scopes around meaningful owner-thread and worker units.
Worker durations are reported after join. GPU pipeline and upload timing uses
nonblocking device timestamps where available. Byte counters are measured at
allocation and transfer boundaries, not inferred from material counts.

Do not set final limits from guesses. Start with conservative safety ceilings,
profile real demos and hostile fixtures, then record measured crossovers beside
parallel or caching choices.

## Explicit non-goals

The first production release does not include:

- arbitrary shader compilation in shipped clients;
- downloading unsigned shader patches outside the active manifest;
- a general visual shader graph that emits unrestricted source;
- cross-vendor runtime binary translation beyond published forms;
- ray-tracing languages or vendor-specific shader extensions;
- material inheritance of unbounded depth;
- automatic generation of every feature combination;
- runtime introspection of private renderer vertex layouts;
- scripts creating raw descriptor sets, device buffers, or GPU pointers;
- dynamic runtime-sized arrays as ordinary material parameters;
- per-asset signatures beside the signed manifest root;
- cloth, terrain, VFX, UI, and post-processing ownership moving into the
  material system;
- a promise that static instruction estimates predict GPU time.

Shader graphs, procedural texture baking, bindless resources, ray tracing, and
pipeline archives may be designed later. Their future formats must still enter
through the same cook, signature, capability, residency, and fallback seams.

## Completion definition

This plan is complete when:

- authors can create, inherit, preview, save, script, cook, publish, and load
  typed materials and shader modules;
- legacy seven-map materials use the same runtime resolver;
- Studio hot reload is cancellable, transactional, and never blocks the UI
  thread on compilation;
- packaged pipeline documents and worlds reference cooked shader modules only;
- release clients contain no GLSL compiler or authored-source execution path;
- SPIR-V and MSL variants are validated, signed through the manifest, selected
  by capabilities, and covered on supported devices;
- material instances change data without creating shader variants;
- unsupported content follows a bounded authored fallback or a visible missing
  marker;
- save, replication, delivery, and script surfaces carry stable names and
  bounded values rather than device state;
- hostile inputs fail before large allocation or device creation;
- cache keys cover every compatibility input and retirement leaks no GPU state;
- headless, integration, fuzz, image, architecture, formatting, and release
  profiling gates pass;
- the old runtime shader compiler path and redundant material-resolution paths
  are deleted.
