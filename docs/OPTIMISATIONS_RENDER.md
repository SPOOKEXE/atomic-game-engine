# Rendering optimisations

## Status

Research notes, not committed work. Each entry says whether we already have
it (`we have`), have part of it (`partial`), or do not (`candidate`). A
`candidate` becomes work only when a release-preset measurement names the
scene that pays for it.

Ideas are written whole and self-contained so any entry can be picked up
without reading anything else.

---

## Shadows

### Cascaded shadow maps with exponential split scheme

Candidate. We fit one directional shadow map to the whole scene.

Partition camera depth into N cascades (4 is standard, 1 on weak targets).
Compute far bounds exponentially: `base = pow(maxDist / firstCascadeFar,
1/(N-1))`, `bound[i] = firstCascadeFar * pow(base, i)`; a first far bound
near 10 units and maximum near 150 are proven starting points. Each cascade
gets an orthographic projection fitted to that slice's frustum corners in
light space, all cascades sharing one pixel size (2048 typical). Store
per-view per-light cascade matrices separately from camera matrices so they
stabilize independently. In the fragment shader pick cascade by view depth,
blend across boundaries using an overlap proportion (about 0.2 of each
cascade overlaps the previous), fade to lit past maximum distance. Cull the
shadow set against each cascade frustum expanded along the light direction,
never the eye frustum; casters outside view still matter. Our instance
buffer's two-range split already encodes that instinct.

Costs N shadow passes over casters, mitigated by per-cascade culling and by
rendering coarser cascades less often (see update scheduling). Pays off
immediately once outdoor directional shadows look soft up close: cascades
recover resolution a whole-scene fit gives away.

### Texel-grid stabilization of moving shadow frustums

Candidate. Frustum fitting is explicitly deferred until a stability pass
exists; this is that pass. Do it on day one of cascade fitting.

Three cooperating tricks. First derive the light view from the light's
rotation only (orthonormal basis, zero translation) so nothing depends on
where the light entity sits. Second make the fit rotation-invariant: take
the maximum of frustum-slice body diagonal and far-plane diagonal lengths,
ceil to whole world units, size the ortho box from that diameter rather
than a per-axis AABB (a tight AABB changes shape as the camera rotates;
a sphere-derived extent does not). With power-of-two map size, texel size
becomes an exact power-of-two multiple and floating-point exact. Third
snap: floor the light-space centre to whole multiples of texel size before
building the matrix, and build light-from-world directly (transpose of an
orthogonal basis, translation column from floored centre) instead of
composing and inverting. Equivalent formulation: project new centre through
last frame's matrix, round x/y to texel boundaries, unproject. Quantizing
the scale itself (`quantizer / ceil(quantizer / scale)`) removes the last
wobble source.

Snapping gives up a fraction of a texel of resolution; without it every
refit shimmers and users blame the filter.

### Shadow filter menu

Candidate. We currently sample the shadow map directly. Ship compile-time
selected filters behind one sampling function, chosen by quality tier.

- Baseline: hardware 2x2 percentage-closer filtering through a comparison
  sampler, one fetch.
- Mid: fixed 9-tap separable Gaussian exploiting bilinear weights inside
  the comparison sampler (weights from fractional position within the
  texel, normalization constant 1/144), approximating a much wider kernel.
  Best quality per sample for fixed penumbra.
- Soft/temporal: 8 spiral-distributed taps rotated per pixel by a random
  angle from interleaved gradient noise seeded with pixel coordinates plus
  frame count, radius scaled by desired blur size plus an empirical fudge
  widening the pattern in coarse cascades. Trades noise for wide soft
  shadows at 8 taps.
- Contact hardening (PCSS): blocker search first, 8 taps spread over a
  search radius equal to light angular size averaging depths closer than
  receiver; penumbra width is `max((zBlocker - zReceiver) * lightSize /
  zReceiver, 0.5 texels)`; then jittered sampling at that width. Doubles
  cost and needs real authored light size.
- Cubemaps: build an orthonormal basis around the light direction and apply
  the 2D pattern in tangent space, since cubemap bilinear filtering does not
  behave like 2D.

### Depth bias and normal offset scaled by cascade texel size

Candidate. Nearly free, biggest visual-quality lever after resolution.

Keep slope-scaled depth bias in rasterizer state for the geometric
component, apply the bulk as world-space offset of receiver position along
its surface normal before transforming into light space, sized in world
texels: `normalOffset = baseNormalOffset * texelWorldWidth`, recomputed per
cascade. When cached levels of different resolutions coexist, scale each
level's normal bias by its own texel width relative to finest level, leave
depth bias alone. Extend ortho depth range generously behind the camera
(several times cascade radius, or fit scene bounds along light axis) so
casters behind the eye occlude. Too much normal bias bleeds light under
contact points; inspect acne and peter-panning per cascade separately.

### Point light shadow cube arrays with tangent-space filtering

Candidate. No point-light shadows exist yet.

Allocate one depth cube array (or 2D array addressed as six layers per
light, `lightIndex*6 + face`). Each face renders with 90-degree perspective,
square aspect, near plane at configured shadow near, infinite reverse-Z.
Faces are ordinary graph views so culling, instance buffer and timestamps
work unchanged. Sampling uses comparison sampler on the cube array indexed
by light slot; because 2D patterns cannot wrap across faces, build tangent
basis from normalized direction to fragment, offset sample directions by
`position.x * basisX + position.y * basisY` scaled by distanceToLight, reuse
directional filters. A shadow LOD origin (usually active camera) lets far
lights drop updates or resolution.

Six faces per light dominates quickly: cap simultaneous shadowed point
lights to a handful, consider round-robin face updates for static scenes.

### Shadow update scheduling: alternate cascades, cached levels, budgets

Candidate, natural extension once cascades land.

Split levels into two classes. Dynamic near levels render every frame and
consume no budget. Coarse levels render on a refresh budget (one or two per
frame) with ages staggered at init so they never expire together, a maximum
cache age forcing eventual refresh, and targeted invalidation: an event
moving geometry marks only levels whose footprint intersects the change.
Publish committed centres from last completed render rather than desired
centres so the shader containment box never drifts from map content.
Cross-fade adjacent levels by accumulating shadow factors finest to
coarsest weighted by boundary band, sampling every level's comparison
texture unconditionally and multiplying by weight afterward; divergent
comparison sampling produces undefined derivatives and flicker.

Stale coarse shadows for a few frames during bursts is the price. A moving
sun forces frequent coherent refreshes or accepted lag. Composes with our
retained-image caching discipline: a shadow level is another retained image
with signatures.

### Shadow atlas packing and tile reuse

Candidate. The portal-beam quadrant arithmetic shows per-tile viewport,
scissor and lookup-window handling exists; generalizing is the work.

One large power-of-two depth texture subdivided into tiles; each light face
or cascade owns one rectangle struct used three ways: render viewport and
scissor, shader lookup window, clear region. Tile allocation is a shelf or
bin packer over requested resolutions; distant or small lights get smaller
tiles. Simpler robust alternative: fixed-size texture array, every layer
same size, no packer, no bleed. Reserve padding or clamp lookups against
edge bleed. Never let one rectangle's expression diverge into three copies
(the portal-beam bug class). Start with arrays; move to an atlas only when
measured waste from uniform layer size matters.

---

## Lighting

### Forward+ clustered light assignment

Candidate. Lighting is currently authored lights plus seam captures; this
is the scaling path while staying forward.

Partition view frustum into X by Y by Z clusters (16x9x24 example). XY
tiles divide screen dimensions; Z slices follow `slice(i) = near *
(far/near)^(i/N)` so slices stay near-cubic in perspective. Two-level data:
per-cluster word packing offset, point count and spot count into bitfields
(9 bits per count fits uniform-buffer case), pointing into one tightly
packed global light index list; light data lives in flat float4 arrays.
Assignment runs once per frame in compute (one thread per cluster, sphere
versus cluster AABB) or CPU. Fragments locate their cluster with
`u32(log(viewZ) * a + b)` and iterate only their list. Cap lights per
cluster and surface overflow with debug colour rather than silent drop.

Exponential slicing leaves big far clusters hoarding lights: cap clustering
far plane below render far plane and route distant lights through a cheap
separate path. Forward+ keeps transparency and MSAA trivial and bandwidth
low; deferred wins only at very high light counts with heavy overdraw.

### Tight light-to-cluster assignment

Candidate, adopt when clustered lighting lands, not after.

Naive sphere-versus-AABB assigns a light to every cluster its bounding box
touches, inflating lists 20-30 percent. For spheres find the index range
spanned, refine iteratively: loop Z planes first projecting the sphere onto
each plane outside centre slice (shrinking), then Y, scan X from both ends
until shrunken sphere misses; fill only covered cells. Spot cones narrow
against six cluster planes via cone-plane tests. Exact alternative:
rasterize the light's convex shell conservatively in view space for precise
coverage of arbitrary convex volumes at cost of a geometry pass. Frustum-
cull the light set first. More ALU for fewer shaded lights; pointless below
a couple hundred lights.

---

## Culling

### Two-phase Hi-Z occlusion culling hardening

We have the shape (CPU-chosen early-phase occluders, pyramid reduction,
late-phase box tests, indirect argument buffers, candidate-count fallback);
three hardening items are candidates.

Recap of the mechanism: phase one draws only objects visible last frame into
depth, reduces into hierarchical Z where each mip stores depth farthest from
camera. Phase two tests remaining candidates' world AABBs: project corners,
choose mip where footprint covers roughly four texels, cull when box nearest
depth exceeds sampled farthest plus epsilon. Survivors feed next frame's
early phase; compact survivors via prefix-sum over visibility bitmask so
indirect arguments carry no dead entries.

Hardening candidates: sparse-distributed-downsample pyramid builder removing
precision bugs at non-power-of-two sizes; per-candidate mip selection rather
than fixed level; run tests against shadow views too. Measure the occlusion
funnel (candidates, early survivors, late survivors, drawn), not just frame
time.

### CPU software-rasterized occluders

Candidate.

GPU two-phase answers one frame late and needs compute; a CPU rasterizer
answers before submission, suiting a host that decides the entire draw list
before touching the device. Maintain small curated occluder proxies (large
closed boxes, walls, terrain patches) front-to-back sorted. Rasterize with a
SIMD masked-software rasterizer into tiled hierarchical depth where each 8x4
tile stores near/far depths plus coverage mask, coverage and depth updates
decoupled so tiles compress naturally. Interleave drawing with queries:
after a few big occluders land, test candidate boxes against partial buffer,
skipping their proxies too when hidden. Multi-thread by binning triangles to
screen regions so threads never share tiles.

Burns CPU per frame, budgeted like any job workload. Imperfect proxies leak
toward drawing more, the safe direction. Best in deep interior scenes with
CPU headroom; coexists with GPU path as phase-one picker.

### GPU-driven indirect draws with binned batch sets

Partial: indirect draws exist for the occlusion late phase; full opaque path
is candidate.

CPU bins visible items by batch key (pipeline, material binding, indexedness)
into per-phase batch sets and uploads compact per-item records. Compute pass
expands them: each invocation frustum-culls and occlusion-tests its item,
appends surviving indices to work list, bumps batch instance counter
atomically. Second compute converts counts to indirect draw parameters
(index count, instance count, base offsets); counters reset between phases by
tiny reset dispatch. One multidraw-indirect call per batch set, vertex shader
fetches instance record by indirection. Platforms without compute fall back
to CPU-built parameter buffers, supported structurally from the start.

Moves sort-and-cull to GPU and collapses binds but complicates visibility
debugging and buffer lifetime. Payoff arrives above roughly a thousand
visible instances; below that CPU submission is simpler and fast enough. Our
single vertex/index pair plus three-integer draw identity is exactly the
layout this wants.

---

## Submission and materials

### Bindless material slabs and pipeline-count discipline

Candidate.

One descriptor set per material means one rebind per state change, which
drivers cannot batch; pipeline proliferation multiplies again. Pack many
materials into few large bind groups ("slabs") holding bound arrays of
textures, samplers and buffer views; empty slots carry dummy resources so
nothing is ever unbound (we already own a fallback-texel resource). A
material becomes slab-plus-slot stored in instance data; shaders index arrays
with non-uniform qualifiers. Rebinding happens once per slab change, ideally
never within a frame. Pair deliberately with ubershader stance: material
variation lives in data, not pipeline permutations, keeping pipeline counts
in hundreds rather than tens of thousands.

Needs descriptor-indexing features with update-after-bind semantics
(feature-detect and fall back to per-material binds); capture tools see less
so engine diagnostics must name slots. Watch platform limits (roughly a
million active descriptors, couple thousand samplers). Payoff grows with
material variety; a seventeen-name enum needs none yet.

### Buffer slab suballocation for meshes

Candidate. Single packed vertex/index pair is ours; growth by full re-upload
is the gap.

Manage the existing single vertex and index buffers as slabs owned by an
offset allocator: fixed-size bin classes, free-list merging, O(1)
alloc/free. Batch allocations and frees once per frame through stage/commit
transactions, copying only new elements' bytes through staging. Objects over
a threshold bypass with private buffers so one giant mesh cannot fragment the
slab. Slabs grow geometrically remembering high-water marks; shrink only
after sustained lower demand.

Trigger is measurable: re-upload bytes per content arrival exceeding a frame
budget. Until streaming makes arrivals frequent, honest re-upload beats hand-
rolled device-memory allocators.

### Instance and vertex quantization

Partial: instance packing we have (snorm16 quaternion, packed colour,
forty-byte rows, decode mirrored in GLSL, pinned by tests); vertex
quantization candidate.

Instances: four-component 16-bit signed-normalized quaternions (smallest-
three and 10-bit variants rejected on measured angular error), colour four
bytes, translation floats, decoded with two unpack intrinsics. Vertices:
positions as 16-bit signed-normalized components relative to mesh bounding
box centre and extent (per-mesh scale in draw record), normals and tangents
octahedral 2x16 (2x8 for distant LODs), UVs snorm16 with tiling factors kept
outside data. All decodes pure arithmetic, testable headlessly beside the
C++ reference exactly as the instance suite does today.

Halves vertex fetch bandwidth, often the actual bottleneck behind "overdraw".
Pin error bounds in tests like the rotation suite does.

---

## Geometry and meshes

### Mesh compaction: smallest index type plus quantized vertices

Candidate. Mesh format is fixed float32 positions and normals, always u32
indices.

Choose u16 indices automatically whenever vertex count fits, upgrading past
the threshold, decided at bake and recorded in the header. Quantize normals
and tangents octahedral at eight to twelve bits, positions half-float or
unorm integers relative to mesh bounding box, UVs unorm relative to their own
bounds; let vertex fetch dequantize in hardware. Bake quantization offline,
never runtime; derive bounds from vertices rather than trusting stored ones.
Quantize before welding or weld on quantized values, else duplicated vertices
crack along mirrored seams. Octahedral encoding needs care packing tangent
handedness sign.

Expect a third to half off vertex bandwidth and file size.

### Bake-time triangle ordering and shared-vertex LOD chains

Candidate; neither exists, bake graph is the right place.

After welding, reorder triangles for post-transform cache hits, reorder
indices for sequential fetch, remap vertices accordingly, all offline.
Generate LOD chain by edge-collapse simplification sharing the original
vertex buffer for the first couple levels, appending each level's index range
after the previous so one allocation serves all; switch to independent vertex
data only for coarsest levels where attribute-preserving simplification
degrades. Simplification quality depends on attribute weights; normals and
UV seams constrain collapses, expose weights per material. Cache optimization
is hardware-sensitive but universally neutral-or-positive. LOD selection
itself stays a rendering concern, out of the bake.

### Meshlet virtual geometry (long-horizon overview)

Candidate, long-horizon.

Offline split meshes into clusters of 64 to 128 triangles, merge clusters
into simplification DAG annotated with bounding sphere, cone axis/angle for
backface culling, error metric; store coarse-to-fine for streaming. Runtime
all GPU: instance culling, screen-space-error walk selecting DAG cut,
per-cluster frustum/cone/Hi-Z culling, rasterization where clusters under a
projected-area threshold go through compute software rasterizer writing a
visibility buffer (instance id plus primitive id, atomically depth-compared)
and larger clusters through hardware rasterization; resolve pass shades once
per visible pixel fetching material and vertices through the visibility
buffer. Effectively one draw call for all opaque geometry with seamless LOD.

Demands a preprocessing tool, 64-bit atomic visibility buffers, higher base
overhead; wrong for sparse scenes, transformative for dense ones. Prototype
behind the existing node abstraction before committing.

### LOD selection refinements

We have projected-area ladder selection on CPU with named mesh levels;
refinements are candidates.

Add hysteresis: switch up at area A, down only below k*A (k around 0.85) so
objects straddling boundaries settle. Prefer screen-space-error threshold
when level metadata carries error values, since error predicts visual
difference better than area. Give shadow pass its own coarser ladder
(shadows tolerate one to two levels coarser than the eye) selected by same
function with different target. Early-out on squared distance before any
catalogue lookup; cache chosen level per instance until inputs change rather
than reselecting every frame. Cheap, incremental, testable headlessly.

---

## Textures and images

### Universal compressed texture interchange with per-target transcode

Candidate. Textures today are uncompressed R8 or RGBA8 with compression only
in transit.

Author and store textures in a supercompressed intermediate (ETC1S or UASTC)
inside a standard container, convert at install time, at bake time per target,
or load time to device preference: BC7/BCn desktop, ASTC modern mobile, ETC2
older-mobile floor, RGBA last resort. Target selection ordered by quality
probe. Transcoding is fast table-driven unpack, not recompression,
parallelizes per level. Carry sRGB flag beside data and apply consistently at
upload; decoding gamma-space into a linear pipeline or double-converting
shifts colours invisibly. Natural home is the bake graph: a compress node
immediately before write, like the mipmap-last convention.

Block formats quantise gradients and alpha so UI-critical art may stay
uncompressed. Cook-per-target doubles storage unless intermediate ships and
conversion happens locally, trading disk for download size.

### Flipbook-aware mip chain generation

We have, including the stopping rule.

Naive halving averages neighbouring animation frames together producing
ghost frames at distance. Generate box-filtered levels downward only while
every destination pixel stays inside one grid cell, ending where a frame
reaches one pixel; sheets whose cells the grid divides unevenly get no chain
rather than approximate ones, single-frame sheets exempt. Apply same rule to
procedurally generated textures which otherwise reach samplers unchained and
shimmer. Rule lives beside the resampler so every producer shares one
definition of a half-size copy.

### Mip streaming under residency budget with recency eviction

Candidate. All textures and meshes currently upload eagerly; eviction
explicitly unsupported.

Ship whole mip chain in file, upload tail of small levels initially, promote
finer levels asynchronously based on observed screen footprint and required
level from projected texel density. Track per-resource bytes and
frame-last-used stamp; when promotion exceeds global budget evict highest
unused mip levels first, then whole resources oldest use first, never
anything used this frame. Load low levels first so pop-in resolves blurry to
sharp. Hysteresis mandatory: margin and cooldown around thresholds or
promote/demote thrashes. Requires residency bookkeeping currently absent;
SDL GPU re-uploads into new allocations, turning eviction into allocation
churn management. Budget promotions per frame or bursts hitch.

### Staging ring buffer for transfers with deferred mips

Partial: staging and transfer accounting exist, uploads immediate and
synchronous.

Keep few large mapped staging buffers reused as ring: write payloads into
next free region, copy at submit boundary, recycle regions only after frames
referencing them retire. Defer non-critical uploads, particularly upper mips,
into the queue so a texture is usable at low resolution the moment base level
lands. Cap bytes submitted per frame, queue the rest. Ring sizing must cover
worst burst; frame-retirement tracking adds lifecycle machinery. Deferring
mips delays sharpness a few frames, invisible in practice. Compounds with
streaming above.

---

## Particles and effects

### Pooled particle blocks with in-block swap retirement

We have. Recorded here because every later particle idea builds on it.

Each emitter owns one contiguous capacity block claimed once from a bump
allocator with free range list. Live particles are a prefix inside the
block; death swaps dying slot with last live slot and decrements count.
Blocks never resize, particles never cross blocks, dead emitter blocks
return ranges for exact-fit reuse. Jobs hand workers disjoint block sets so
the parallel step needs no atomics and produces identical order every run.
Fragmentation mitigated by exact-fit matching and hard ceiling per emitter.

### Split sim/render arrays with sampled curve tables

We have. Three parallel identically-indexed arrays: tiny mutable state row
for integration, 28-byte draw instance holding only what varies between two
particles of one emitter (position, packed sizes, rotation plus flipbook
cell, RGBA8 colour, emitter slot), and per-emitter blocks carrying shared
data. Authored curves resample once into fixed sixteen-entry lookups per
emitter whenever the authored row changes; step reads them with one multiply,
shift and lerp. Colour converts to eight bits sixteen times per emitter
instead of once per particle. Sixteen samples quantise hard edges onto
nearest sixteenth of lifetime; fix is per-emitter sample count, not bigger
constant for everyone.

### Deterministic emission from stateless seeded randomness

We have.

Every random draw is a pure function of seed tuple (emitter identity,
monotonic per-emitter spawn counter, purpose tag). Spawn counter feeds seed,
never recycled slot, else replacements duplicate predecessors and steady
emitters loop the same handful. Spin rates resolved at spawn stay in turn
units so integration is multiply-add against fixed-point rotation
accumulator. Weaker statistics than a real generator, fine for decoration;
new draw sites need new purpose tags or sequences collide.

### Device-resident compute stepping with retained host reference

We have, and the pairing is the part worth keeping.

When renderer owns the pool, host ageing stops and compute emits, integrates,
retires, draws from resident buffers; nothing crosses the bus unless
parameters changed. Emitter parameter blocks carry two independent revision
counters, transforms-and-forces versus sampled curves, so a moving emitter
re-sends forty-four bytes while static scenes send nothing. Host pass stays
alive as behavioural reference the tests pin, no-compute fallback, and
definition the shader must match. Every semantic change is a paired edit.
Compute buys little below roughly ten thousand particles where dispatch
overhead dominates, and forfeits trivial gameplay reads of positions.

### Sorting strategy: blend partitioning now, depth buckets later

We have the partition; finer ordering is candidate.

Batches group by blend mode and orientation rule with stable index-list sort
putting blended groups before additive, so each pipeline binds once and
additive draws never sort. Candidate extension: inside blended batches,
bucket particles into handfuls of view-depth bands per emitter drawn far to
near, approximate correct ordering at O(n) instead of O(n log n), enough
that smoke self-overlap artifacts stop reading as bugs. Band seams can show
on large overlapping quads; tune band count against quad size. Full sorts
and depth peeling are the wrong end of the trade at this scale.

### Soft particles through depth fade

Candidate, opt-in per effect.

Particle fragment shader samples scene depth at its pixel, linearises both
depths against near/far, takes difference, multiplies alpha by smoothstep
ramp over tunable falloff so particles dissolve approaching surfaces. Needs
opaque depth available to translucent pass plus per-effect falloff constant.
Dependent depth sample hurts most where overdraw is worst; on tile-based
mobile GPUs reading depth during blend disables early rejection. Default off
for small sparks where artifact invisible.

### Emitter significance LOD and spawn-budget tiers

Candidate; claim hook already exists as activation predicate and refusal
counters.

Classify emitters into effect classes with per-class spawn-rate scales,
simultaneous-instance caps, maximum distance beyond which an emitter claims
no block. Rank live emitters per frame by significance roughly inverse
screen-projected size weighted by class priority; shrink rate, lifetime and
maximum size for low-significance emitters before refusing outright. Global
quality tier multiplies rates and pool cap. Prefer shrinking size curves over
cutting count: fill cost falls quadratically, simulation cost linearly.
Fade transparency out over short window instead of switching instantly.
Ranking reads compact block rows, never wide authored components.

### Cheap curl noise flow fields

Candidate; scalar procedural force module already stored on emitters.

Evaluate two or three offset copies of cheap gradient/value noise at particle
position, finite differences give pseudo-curl vector used as divergence-free
velocity. Scroll noise coordinates over time to animate field. Evaluate low
frequency, optionally every other tick interpolated, since fields change slow
relative to particle lifetimes. Gate behind strength-zero disable flag;
consider per-block-region evaluation for weak fields. Divergence-free is
approximate; nobody notices in smoke.

### Simulation-rate trail recording

We have.

Record trail points during simulation tick into fixed ring held on component;
render-side pass derives drawn ribbon geometry from ring. History lives in
memory only: save/load write authored fields and restore empty ring, verified
byte-for-byte by benchmark. Ring inflates component rows substantially,
mattering for hashing and replication paths, keep it out of wire and snapshot
formats by construction. Ring length caps visible trail duration regardless
of framerate, intended and documented behaviour.

---

## Chunked worlds feeding rendering

Full physics-side interplay lives in the physics doc. These entries cover
meshing, streaming and scheduling that produce render input.

### Neighbor-padded chunk buffers feeding binary greedy meshing

Candidate.

Meshing needs one voxel of neighbor data per face to cull hidden boundary
faces and merge quads across edges; lazy mid-algorithm fetch wrecks cache
behaviour and thread safety. Stage each meshing job with padded array of
size (n+2)^3 holding chunk plus one shell of neighbor voxels filled from six
neighbor border copies handed in as plain arrays. Build per-face-direction
binary masks: bit set where voxel solid and predecessor along face normal
air, computable word-wise with shifts and xors over padded array. Greedy
merge operates on bit planes: scan rows finding maximal runs of equal bits
horizontally, extend vertically while whole rows compare equal, clearing
consumed bits. Pack each emitted quad into single 64-bit word (face type,
height, width, z, y, x) so output is branch-light and sortable. Precompute
opaque/transparent masks once per chunk edit so remeshes skip voxel sweep.
Reuse scratch buffers; hot path allocates nothing.

Padded-buffer practice lands around two orders ahead of naive per-voxel
sampling. Fixed chunk size effectively required since bit-plane widths bake
in. Face merging stops at borders unless mesher receives extended context;
accept seam, hide with skirts or stitched indices at LOD boundaries rather
than paying for global merging.

### Vertex-baked ambient occlusion with anisotropy-correct triangulation

Candidate.

For each emitted quad vertex sample three neighbor cells diagonal to that
vertex in plane offset toward face normal (side1, side2, corner). Rule: both
sides solid means ao 0, otherwise ao = 3 minus (side1 + side2 + corner).
Store two bits per vertex packed alongside material id. When corner values
are asymmetric flip quad triangulation along diagonal connecting vertices
whose AO sum is larger, keeping shading continuous instead of showing hard
crease. AO depends on neighbor cells so border vertices need the padded
shell; border edits mark neighbor meshes dirty too (feeding scheduler below).

Three cell tests per vertex, amortized by computing from same padded masks
used for face culling. Pure function of voxel neighborhood so replays match.

### Derived-predicate chunk scheduler

Candidate complement to ECS dirty bits.

Replace lifecycle events with periodically evaluated predicate per resident
chunk: generate if data missing and inside load region; mesh if data ready,
required neighbors present, mesh absent or stale, no job in flight; upload if
mesh newer than resident copy. One scan per tick (or k ticks) walks resident
chunks coordinate-sorted evaluating cheap booleans, enqueues passes sorted by
priority then stable ticket. Correctness follows from state not signal
delivery; stale async results discarded comparing request identity against
current state. Order phases so borders remesh last: interior mesh jobs
first, second pass for chunks flagged by neighbor border change, rebuilding
each border once. Keep per-chunk state in packed structs; dirty-list
accelerator reconciled occasionally self-heals missed signals.

Scan is O(resident chunks); enqueue order coordinate-sorted so replay
matches. Model chunk state itself as ECS components with changed-component
bit columns (`VoxelsDirty`, `MeshDirty`, `CollisionDirty`) rather than
private vectors; systems clear the specific bit whose work finished at owning
barrier. Never model dirtiness as tag components, tags move rows between
archetypes turning writes into structural changes.

### Ticketed async generation committed at the world barrier

Pattern we have; terrain wiring pending.

Worlds submit immutable requests (graph identity, seed, tile coordinate,
LOD, outputs) to process-wide scheduler deduplicating by full request
signature, sorting ready work by priority then ticket. Workers produce owned
artifacts plus request echo touching no ECS state. At deterministic owner
barrier accept result only when target still exists, ticket matches, content
signature matches expectations, revision unchanged; everything else
discarded. Identical geometry published twice advances no revision so
consumers see no invalidation. Scripts awaiting tickets suspend themselves,
not the world. Dedup costs one hash per request, collapses multi-camera
duplication. Bounded result queues prevent memory ballooning; stale tickets
dying at barrier removes cancel protocols entirely. Acceptance order is
ticket order so replay matches regardless of worker completion order.

### Hysteresis streaming with distance priorities and hard budgets

Candidate.

Per-viewer load radius strictly inside unload radius (load at 3 chunks,
evict at 5) so oscillation near boundary neither loads nor unloads. Score
pending loads by distance decay plus frustum bonus, re-sorting when viewer
turns. Cap concurrent loads and work per tick twice: async bytes
(decompression CPU seconds per frame, chunked so one giant chunk respects
cap) and sync finalization (millisecond budget drained incrementally, tile
resumes next frame mid-population). Evict under byte budget not count: drop
least-recently-used residents first, refuse rather than evict incoming
chunk. Cancel queued unloads when chunk re-enters range. Priorities affect
arrival timing only, never simulation results.

### Sector-table region files for chunk persistence

Candidate.

Group fixed block of chunks (32x32 columns proven) into one region file
addressed by floored division of chunk coordinates. Header carries location
table mapping chunk slot to (sector offset, sector count) packed one 32-bit
word per chunk plus timestamp table recording last write. Payload in aligned
4 KiB sectors; chunk records carry own length and compression-type byte,
individually compressed so empty subvolumes collapse. Writes serialize chunk
outside any lock, take file lock only to allocate sectors and copy bytes;
growth appends when no free run fits. Per-chunk size ceiling routes outliers
to sidecar files. Open synchronously to survive crashes mid-write.
Timestamps enable save-only-dirty sweeps keyed off ECS dirty bits. Names
cross boundaries as strings per house rule, never derived dense ids.

### Chunk-phase DAG executed by dependency counting

Candidate, only above dozens of chunks; below that fixed sequence of
`Jobs::For` phases is cheaper and simpler.

Static DAG of phase nodes annotated with per-chunk readiness: unmet
dependency counters cloned fresh each tick, ready-set bitset, completion
events pushed to bounded queue; completing decrements dependents, enqueues
at zero. Conflicting-access pairs precomputed into bitsets so two nodes
writing one chunk never co-run; exclusive operations drain board before
running. Cache topological order, recompute on graph change not per tick.
Within phase, tasks fan out fork-join preserving tick-is-one-thing. Dynamic
per-chunk skipping beats ad hoc schedules. Dependencies gate execution
without reordering arithmetic; verify parallel-equals-serial.

---

## Frame graph, async compute and post

### Transient aliasing and automated barriers

Partial: graph validates reads, orders nodes, retains signed images;
explicit transient heaps likely low-value on SDL GPU backend.

From declared graph compute each transient's lifetime (first write to last
read), colour interval graph so non-overlapping transients share physical
memory (40 to 50 percent transient VRAM reduction reported elsewhere),
insert aliasing barrier between occupants. Derive access masks and layouts
from declared usages, merge transitions per pass, drop redundant
consecutive-read barriers. Cull passes by backward reachability from outputs
so disabled features vanish including allocations. Cross-frame pooling keeps
heaps alive, resizes on peak growth, releases after quiet frames. Compiler
is pure CPU integer work, highly testable. Genuinely useful pieces here:
lifetime-driven reuse of pooled targets (surface pairs, mirror pools) and
pass culling, expressible without fighting the abstraction. Measure live
logical bytes before building any of it.

### Async compute overlap

Partial: dedicated compute submission path and multiple command buffers
exist.

Identify passes with no data dependency on concurrent raster work: depth
pyramid for next frame's early phase, light clustering, SSAO, histogram.
Submit through dedicated compute path enqueuing while other views'
rasterization proceeds; synchronize only at consumption points via graph
edges. Unified queues time-slice rather than truly parallel; confirm overlap
by timestamp buckets, never assumption. Report overlapped producer spans as
reported spans attributable to producer, not spliced into frame timeline,
matching house profiling rules. Start with pyramid and clustering, expand on
evidence only; ownership transfers cost more than they return below certain
pass sizes.

### Post-chain cost discipline

Candidate; deferred lighting, ambient occlusion and overlay composition
exist, bloom/LUT tonemapping/upscale do not.

Bloom as dual-filter pyramid: downsample progressively with 13-tap filter
applying Karis weighted average on first step suppressing fireflies, cap
smallest mip dimension (512 plenty), upsample with tent filter adding into
level above, composite additively, compact HDR format throughout. Tonemap
through small 3D LUT baked offline per curve: per-pixel cost collapses to one
trilinear-ish fetch plus gamut mapping, curve swaps become texture swaps.
SSAO-class effects at half resolution with depth-aware bilateral upsample;
smooth by nature, hides halving. Temporal upscale (jittered subpixel
projection, history reprojection with neighbourhood clamps) is heavyweight,
treat as project; interface passes stay native always since text does not
survive upscaling. Half-res trades sharpness on high-frequency detail; Karis
dims legitimate emitters slightly; LUTs constrain dynamic exposure unless
rebuilt or blended. Each independently adoptable behind a graph node.

---

## Profiling contract

Instrumentation largely we have; the habit is the ask.

Buckets: per-pass GPU time from existing nonblocking timestamps (shadow,
geometry, lighting, transparent, post, overlay, occlusion phases) recorded
every frame with idle waits marked rather than absent. Counters: draw calls
and triangles submitted per pass; occlusion funnel (candidates, early
survivors, late survivors, finally drawn); instance and texture upload bytes
per frame; cascade or tile texel utilization written over allocated; cluster
light-count histogram; presentation-cache hit rate alongside traffic counters
where hit must mean zero uploads and zero transient allocations; live versus
peak versus cumulative GPU bytes from tracked wrappers, churn and residency
being different diseases. Read metrics to report them, never steer behaviour
mid-frame. Publish numbers with preset, backend, scene and settings named
beside them; keep dropped spans visible so partial flame graphs never read
complete.

---

## Priority ranking

1. Cascaded shadow maps with day-one texel stabilization recover resolution
   the current whole-scene fit gives away; the stability pass is a
   prerequisite, not a polish item.
2. Clustered forward+ lighting is the scaling path once light counts grow,
   with tight assignment included from the start.
3. Occlusion funnel measurement plus its three hardening items turn the
   existing two-phase system trustworthy at scale.
4. Texture compression interchange plus mip streaming under residency budget
   attack VRAM and bandwidth together; staging ring deferral compounds.
5. Mesh compaction and bake-time ordering cut vertex bandwidth by a third to
   a half for offline-only effort.
6. Particle significance tiers address fill rate, usually the true killer
   before simulation capacity ever binds.
