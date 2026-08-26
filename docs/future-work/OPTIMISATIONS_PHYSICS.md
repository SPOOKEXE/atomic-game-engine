# Physics and collision optimisations

## Status

Research notes, not committed work. Each entry says whether we already have
it (`we have`), have part of it (`partial`), or do not (`candidate`). A
`candidate` becomes work only when a release-preset measurement names the
scene that pays for it.

Ideas are written whole and self-contained so any entry can be picked up
without reading anything else.

---

## Broadphase

### Fat AABB margins with incremental refit

Candidate. Our dynamic grid is rebuilt wholesale every tick, which is O(n)
even when almost nothing moved.

Store each proxy twice: a tight world AABB recomputed from the transform
each tick, and a fat AABB written when the proxy was last inserted. On the
sync pass, if the tight box is still contained in the fat box, do nothing.
Only proxies whose motion escaped their fat box are re-inserted. Grow the
fat margin by a constant padding plus a term proportional to last tick's
velocity, so a slow crate rarely triggers work and a bullet always does.
Periodically shrink margins back toward tight so the structure does not
bloat permanently.

This needs an editable index (per-proxy slot with remove and insert),
which the current uniform grid deliberately refuses to be. Land it either
as a second structure beside the grid or as a replacement justified by a
release measurement.

Tradeoffs: turns per-tick cost from O(n) into O(moved); fat boxes admit a
few percent extra candidate pairs near margins; an editable structure has
history-dependent iteration order, so the deterministic pair sort must
stay downstream of insertion order regardless. Pays off once the ratio of
static plus sleeping to awake bodies is high. Sleeping bodies are already
excluded from our rebuild by the archetype move, so measure against
awake-only populations before committing.

### Bounding-volume hierarchy with branch-and-bound insertion

Candidate alternative to the grid, gated on the measurement the spatial
module already demands.

Build a balanced binary tree of AABBs over proxy bounds. Insertion walks
from the root choosing at each internal node the child whose union with
the new box adds least surface area, ties broken deterministically. The
high-quality version scores a candidate sibling by direct cost (area of
the new parent box) plus inherited cost (summed area increase of every
ancestor refitted on the way up), pruning subtrees whose lower-bound
inherited cost exceeds the best sibling found. After inserting, refit
ancestors back to the root, and at each ancestor consider tree rotations:
swap one child with a grandchild from the other side when doing so lowers
total surface area. Rotations are local operations that repair sorted-input
pathologies and animation drift without global rebalance. Track summed
internal-node surface area as the quality number in the benchmark.

Tradeoffs: pointer-chasing walks beat the grid's flat array only when node
storage is pool-allocated contiguously; rotations must be bounded per tick
to keep worst-case time bounded and results deterministic. Grids win at
bounded scene densities, trees win when object size variance is extreme or
worlds become unbounded and streamed. Do not adopt without a scene that
exhibits the failure mode.

### Lock-free broadphase modification with alternating roots

Candidate. Relevant only if queries ever contend with updates.

Three cooperating mechanisms. First, a moving proxy never rewrites its
ancestors' boxes destructively: it widens them with atomic min/max
operations, so concurrent readers always see a correct-but-loose box, and
tightening happens later in a controlled pass. Second, structural changes
build a fresh subtree off to the side and swap the root pointer atomically;
two root slots alternate, and the discarded root survives until the next
update so queries still walking the old tree finish safely. Third,
modifications are batched: collect changed proxy ids during the tick, hand
them to a prepare job that runs concurrently with anything else, finalize
with a brief root swap. A per-node changed bit marks subtrees containing
movement so rebuilds touch only those paths.

Tradeoffs: atomic min/max over six floats per node costs cache bandwidth;
loose intermediate boxes produce transient false positives, which narrow
phase absorbs anyway. Today our queries are reads over an immutable
between-ticks grid, so there is nothing to unlock yet.

### Wide SIMD box tests and traversal child ordering

Candidate, aimed at tree traversal and mesh-triangle midphases rather than
the hash grid itself.

Two pieces. Layout: store four child boxes of a node as six packed 4-float
vectors (all min-x together, all max-x together, and so on) so one slab
test covers four children with vector compares; a quadtree node then fits
exactly 128 bytes, two cache lines fully used. Traversal: evaluate children
front-to-back along the ray direction but push them onto the stack in
reverse order so the nearest child pops first, letting an early hit prune
the rest. Batch queries gain more than single ones: pack eight ray origins
and directions in structure-of-arrays form and intersect one node against
all eight lanes.

Tradeoffs: the packed layout spreads a representation concern through
every place boxes are tested; scattered rays waste lanes.

---

## Narrowphase

### Persistent pair cache with manifold reuse

Partial: the begin/end diff exists; manifold reuse is the candidate.

Key a persistent map by the ordered body pair (pairs are already sorted by
entity id, so the key is free). Each entry stores last tick's manifold,
both colliders' world AABBs at the time it was made, and the touching
flag. Each tick, after broadphase emits candidates, merge-sort the new
pairs against cached ones (both sorted, linear merge). For a pair present
in both sets: if neither body's box moved more than a small epsilon and the
cached manifold's points still lie within tolerance of both surfaces, reuse
the manifold verbatim and mark the pair skipped; otherwise re-run narrow
phase. Pairs only in the old set are end events, only in the new set are
began events. Our existing `TouchingLast` / `TouchingNow` sorted lists
already implement the diff for events; the extension stores the manifold in
the cache and skips the pair function when the geometric precondition holds.

Tradeoffs: skipped manifolds carry stale normals if bodies rotate in
place, so guard with a rotation-delta check or accept shallow error inside
the penetration slop. Invalidate explicitly on shape or layer edits. Saves
nearly everything for settled scenes, nothing for scenes where everything
moves every tick.

### GJK warm starting with cached simplex and metric gate

Candidate. Our general convex pair starts cold every call.

Persist, per body pair, the last terminating simplex as vertex
correspondence pairs (index of the vertex used from each body; for implicit
primitives a canonical feature index such as sphere centre or rim point).
On the next call, re-evaluate support at the cached correspondences under
current transforms, rebuild the simplex from them, and compute a
degeneracy metric proportional to its length, area or volume. If the metric
collapsed or exploded versus its stored value (a factor-of-two band is
standard), discard the cache and start from a default direction; otherwise
the rebuilt simplex is usually optimal and termination confirms in one or
two support calls. Also cache the last separating direction and use it as
the initial search direction for separated pairs, which often proves
immediate separation on the first support call for slowly drifting bodies.
Normalize directions before support evaluation exactly as the current
implementation does.

Tradeoffs: degenerate caches give wrong answers unchecked, hence the cheap
metric gate. Cache storage is a few dozen bytes per tracked pair and
belongs in the same sorted merged structure as the impulse cache. Gains
concentrate in hull-heavy scenes.

### Four-point manifold reduction by maximized projected area

Candidate alignment: we have a budget of four; this is the concrete
selection rule.

Project every clipped contact point onto the plane through body one's
centre of mass perpendicular to the penetration axis. Pick point one
maximizing (projected distance from centre squared) times (penetration
depth squared), clamping both terms away from zero, preferring deep
contacts with long torque arms. Pick point two maximizing the same product
measured from point one, giving a line segment. Build the perpendicular of
that segment within the plane; pick point three minimizing and point four
maximizing signed dot against that perpendicular. The surviving tetrad
spans the patch in both planar directions weighted toward load-bearing
contacts.

Cost is one pass plus three scans over at most eight points. Determinism
is exact because all comparisons are strict inequalities computed in fixed
order. Changing an existing reduction churns recorded replays, so pin the
new rule against the tower drift and sink benchmarks before swapping it in.

### Contact persistence by proximity in body-local space

Candidate companion to the pair cache; warm-start keys currently rely on
feature ids, which have no natural analogue on arbitrary hulls.

When generating this tick's manifold for a cached pair, take last tick's
points transformed into each body's local frame. For each new point find
the nearest old point in local space; within a matching tolerance (a few
millimetres, scaled by contact size), inherit that old point's accumulated
impulse and identity. Unmatched new points start at zero; old points left
unmatched die. Both frames' transforms are known at match time, so no extra
storage is needed beyond last tick's local-space points, which the pair
cache already holds.

Matching is O(old times new), trivially small at four by four. Tolerance
too large inherits impulses across genuinely different contacts (ghost
stickiness); too small loses warm starts constantly. Matters most once
hulls or compound shapes join the closed shape set.

### Collision dispatch as a registered function table

Pattern candidate. Current code selects pairs through explicit branching.

Hold a two-dimensional table indexed by shape subtype on each axis,
initialized to a cell that asserts and reports nothing. Register exact
specialized implementations for one symmetric half of the matrix; register
a generic reversed wrapper on the other half that swaps arguments, calls
the specialized function, and swaps the result back (normal flip, point
side flip) so each pair function is written once. Register the general
GJK/EPA path as fallback wherever no specialization exists. Dispatch is one
indexed load per pair. Indirect call versus predictable branch is noise at
this granularity; the real value is auditability, because the table is the
coverage matrix and an unregistered cell is visible rather than implicit.
Adopt opportunistically if the shape set grows past the closed enum; do not
rewrite six working pairs for it alone.

### Midphase acceleration for triangle meshes and heightfields

Candidate. Mesh colliders currently answer by linear triangle scan.

Keep the pure-geometry module exactly as it is (points in, shape out,
per-triangle bounds stored). Add the index in the physics layer, which
links spatial and owns the measurements: at collider build time construct
a private index over the mesh's triangle bounds. A coarse uniform grid in
mesh space suffices and never rebuilds since the mesh is immutable; a BVH
if triangle sizes vary hugely. Queries transform into mesh space, walk the
index to candidate triangles, and test exactly. Contacts report triangle id
as the feature, doubling as the warm-start key. Heightfields get the
cheaper variant: column lookup by horizontal coordinate yields the handful
of quads under the query footprint directly, no index structure at all.

Index construction happens once at bake or load, memory proportional to
triangle count. Placement respects layer rules: geometry below, index
beside its consumer above. Pays off the moment any single mesh collider
exceeds a few thousand triangles or sits under many simultaneous queries.

---

## Solver

### Substepped solving with anchor tracking

Candidate. The clock supports faster rates; the solver does not exploit it.

Split the tick into n equal substeps and run one to two solver iterations
per substep instead of sixteen per tick, integrating velocities between
substeps. n small steps beat n iterations of one large step at equal total
work, with quadratic error reduction. What makes it affordable is not
re-running broad or narrow phase per substep: store each contact anchor in
both bodies' local frames at detection time, and at each substep reconstruct
world anchors, lever arms and separation by transforming the local anchors
out with current transforms. Separation updates reflect corrections earlier
substeps made, so the solver sees fresh constraints without fresh detection.

Anchor reconstruction is approximate under large rotation between substeps
and excellent under the small motions substepping exists for. Friction
benefits especially: friction anchors persist across the whole tick and
resist tangential drift far better than per-tick re-derivation. Cost
profile shifts toward per-substep overhead, so substeps want cheap
iterations. Fits cleanly: the clock already decouples solver rate from
world tick, and substep count is a configured property like any rate, so
recorded-run replay is unaffected.

### Warm-start impulse rescaling on step-length change

Small candidate, essentially free.

Cached impulses carry units of impulse, so when effective step length
changes (rate reconfiguration, adoption of substepping) last tick's
impulses are wrong by the ratio of lengths, and the first tick after the
change over- or under-corrects visibly. Track previous step length beside
the impulse cache; on the first step after a change scale every
warm-started normal and tangent impulse by (new length / old length) before
applying. The ratio should live next to the cache it governs.

### Island building by union-find over active bodies

Candidate complement or replacement for chunk-coloured grouping.

During constraint setup link bodies: for each contact and joint, union the
two movable bodies' sets with path-compressed union-find over indices into
the gathered-body array (deterministic because the array is sorted by
entity). Unlinked bodies are singleton islands. Finalize by walking active
bodies in index order appending each to its island's list and recording
each contact under its root's island; islands emerge as contiguous runs
with end offsets. Sort island indices by descending constraint count so
the biggest island starts first. Solve islands independently and in
parallel, serially within each, preserving sequential impulse semantics
exactly because an island is the dependency closure. Sleep an island when
every member is below thresholds; wake floods instantly because membership
is explicit.

Union-find over the gathered set is microseconds. Islands smaller than
chunks parallelize better (no border rows) and enable true island sleeping.
Worst case is one giant island, which no partition fixes; see splitting
below. Island visit order must remain a function of contents, which
index-ordered union-find guarantees. The chunk scheme is measured and
mutation-tested, so replacing it needs the same evidence trail.

### Splitting oversized islands into parallel batches

Candidate, the Amdahl term for island parallelism.

One island containing thousands of constraints serializes the machine
under plain island parallelism. Threshold on island size (order of a couple
hundred items). For oversized islands assign contacts to up to thirty-two
splits greedily: process contacts in order, placing each into the lowest
indexed split where neither of its bodies has been claimed; a body claimed
by another split makes the contact a straddler, and straddlers land in a
reserved non-parallel split solved after all others. Each split carries an
atomic status word encoding iteration, split index and next item cursor so
workers fetch batches lock-free and mark completion by counted increments.
Iterations proceed barrier-style: every split completes round k before any
begins round k+1, news crossing splits once per round through straddlers.

Straddler fraction is a surface-area effect: larger splits mean fewer
straddlers and less parallelism, tune toward target count per worker.
Correctness rests on the non-parallel split absorbing every conflict,
which is assertable directly.

---

## Continuous collision

### Speculative contacts with corrected restitution

Candidate, explicitly listed absent today.

Generate contacts for pairs separated by less than a speculative distance,
roughly the distance the pair can close in one tick taken from relative
velocity magnitude. Such a row carries negative penetration (separation
s > 0). Its bias becomes s divided by step length, so the constraint reads
"relative closing speed along the normal may not exceed s/dt": bodies stop
exactly at the surface instead of inside it. Restitution needs care: if
predicted impact speed exceeds the bounce threshold apply restitution now,
accepting one frame of bouncing from slightly-inside position, and
compensate the gravity applied earlier in the step by subtracting the
gravity-induced normal component from rebound speed; otherwise fall back
to the pure stopping bias. Ghost collisions are the known failure mode: a
grazing pair picks up an inclined normal from a distant corner and slides
up an invisible ramp. Mitigate with a tight speculative distance and
face-derived normals.

Marginal cost per pair is near zero: same narrow phase, wider acceptance
radius, one different bias formula, acting at velocity level so it composes
with the correction-velocity design. Rows inflate for pairs that never
touch, so pair this with per-body flags letting only flagged bodies widen
acceptance.

### Time-of-impact casting with fraction-ordered resolution

Candidate extension of the existing fast-body sweep.

Flag eligible bodies (speed high relative to own thickness) with a motion
quality bit. For each, compute time-of-impact fraction against candidates:
advance conservatively using the distance between convex shapes (already
implemented) to bound how far either can safely rotate or translate,
iterating until the fraction converges or falls back to discrete. Collect
all impacted fractions for the step, sort ascending with body-id tiebreak,
process earliest first: at each impact apply the response, then shorten
remaining travel of any other flagged body sharing an involved body so
later impacts involving deflected bodies re-evaluate. Order is total, so
deterministic.

Conservative advancement iterates many times near-touching; cap iterations
and fall back. This is the most expensive collision path, hence eligibility
flags. Glancing rotational misses are accepted everywhere in practice.

---

## Character controllers

### Full kinematic capsule controller

Candidate. Grounding is one ray today; walls are handled by clipping
commanded velocity. This is the largest single gap and decomposes into
shippable slices: slide-with-skin first, snap-down second, autostep third,
platforms last.

Model the character as an upright capsule queried but not simulated.
Movement per tick: cast the capsule along desired displacement; on hit stop
at skin distance (a small margin kept to surfaces so casts never start
penetrating), project remaining motion onto the hit plane, repeat up to a
bounded iteration count. Autostep: only when grounded before the move, on
a wall hit whose contact point sits below knee height, cast up by step
height, forward by minimum ledge width, down by step height plus skin;
commit the up-and-over teleport only if the down cast lands within slope
limits and headroom existed throughout. Snap down: after a successful
grounded move, cast down a short snap distance and pull to the floor if
found, keeping contact downhill and over stair lips without triggering
airborne state. Platform carrying: sample the supporting body's velocity at
the contact point (linear plus angular cross lever), add it to desired
displacement, and express the character's own motion relative to the
platform so standing still rides along. Slope limit comes from ground
normal dotted against up; steep slopes cancel their downhill component
from input.

Several casts per character per tick, bounded by iteration caps and by
characters being few relative to crates. Every cast excludes the
character's own colliders via the layer-mask route already in place. Skin
prevents stuck-on-seams.

### Multi-ray ground probe with relative-velocity damped support

Candidate soft-stance alternative where rigid capsule feel is wrong.

Cast five short downward rays per character: centre plus four inset corner
rays of the footprint rectangle in the facing frame. Accept a ray as
support only if the hit normal satisfies slope limit; choose nearest valid
hit. Damp vertical error toward hip height with a mass-scaled PD force:
stiffness from tuned frequency, damping measured against relative vertical
velocity (character minus support-point velocity), clamped to maximum force
and unilateral (zero force means gravity takes over). Relative-velocity
damping is what makes moving platforms ride smoothly: damping toward
absolute velocity injects platform motion back as oscillation, damping
toward relative velocity does not.

Five queries versus one is negligible at character counts. Spring
parameters belong in a tuning table, not constants. Corner rays double as
ledge-detection signal for grab mechanics.

---

## Chunked world interplay

The full chunk-border and streaming treatment lives in the render doc's
chunk section where it overlaps; the physics-specific pieces follow.

### Interior/border solver split with longest-first dispatch

We have, in the solve partition; the pattern generalizes to any per-chunk
phase.

Contacts spanning two chunks cannot run beside anything touching those
chunks. Classify each manifold: same-chunk goes to that chunk's group,
movable-versus-immovable goes to the movable body's chunk because immovable
bodies are read-only during the sweep, and movable-in-two-chunks goes to a
dedicated border slot placed last. Run all interior groups in one fork-join
dispatch, then the border group alone afterward. Dispatch interior groups
sorted longest-row-count-first so the critical-path pile starts while
workers are free; break ties by first-row index so ordering is a pure
function of contents. Reordering disjoint groups changes no arithmetic
because no two groups write one body; order within a group is untouched
and is the part that matters.

A dispatch costs roughly one microsecond per woken worker plus tens of
nanoseconds per range, so a scene whose border fraction exceeds about half
the work may run faster unpartitioned. Measure in release and keep the
number beside the constant.

### Ghost/halo borders for chunk-local simulation

Candidate.

A cell update whose stencil reaches past the chunk edge reads neighbor
data; reading directly couples chunk storage layouts and breaks the
copy-per-boundary rule, skipping corrupts borders. Allocate each chunk's
cell array with a halo of width r (stencil radius) on every side stored as
read-only copies. Each tick phase one: every chunk copies its r-wide border
shells into messages addressed to neighbors, landing in receivers' halos.
Phase two: simulate interior-plus-owned cells using halo values, never
writing halo cells. Invariants that make schedule irrelevant: each owned
cell updates exactly once per step, copied border values refresh between
uses, no thread updates a cell it holds only as ghost. Halos arrive as
messages carrying copies, keeping thread-per-world and process-per-world
interchangeable.

Halo memory is O(6rN^2) per N-cube. A deeper halo than correctness
requires allows exchanging every n steps instead of every step, trading
n-fold halo refresh for fewer synchronization points; worthwhile across
processes or when wake latency dominates. Halo contents are pure functions
of neighbor state at exchange, so determinism holds.

### Corner cells folded into axis waves

Candidate.

Corner cells touch up to eight neighbors in 2D (26 in 3D); exchanging with
every diagonal multiplies messages and races on corner ownership. Exchange
in waves along axes. Wave one: horizontal exchanges send border strips
padded to include adjacent corner blocks. Wave two: vertical exchanges send
strips wide enough to carry corners received in wave one. Two waves fill
every diagonal ghost having travelled at most two hops, cutting 8 exchanges
to 4 in 2D and 26 to 6 in 3D. Corners are smaller than main blocks, so the
extra payload is noise while saved latency is not.

Waves are sequential (a mini barrier between them), suiting our existing
phase-barrier shape. In-process, reading neighbor arrays directly during
simulation is faster but welds chunks into one address space and quietly
ends process-per-world. Prefer copies.

### Canonical border ownership resolves disagreements

Candidate.

Two chunks computing something about a shared face (contact, merged quad,
flux, AO term) can disagree, double-simulate, or produce completion-order
dependent results. Define one total order on directed edges, lexicographic
chunk coordinate pair matching existing chunk ordering. The face belongs to
whichever chunk sorts first; the other treats it exterior. Any consumer of
the shared quantity asks the owner. Where both sides genuinely compute
(meshing wants both faces visible), each computes only its own outward side
so union covers the interface exactly once. Values that must agree derive
from world coordinates at the shared lattice position, never independently
accumulated local floats, so derivations return bit-identical results.

Ownership query is one comparison; cache the winner per face pair in a
small table invalidated on repartition. Removes the last schedule
dependence from shared-face outcomes.

### Chunk-local collision caches with owned border seams

Candidate.

Broadphase over every terrain triangle each tick is prohibitive, yet pairs
straddling chunk borders must not vanish or duplicate when chunks stream.
Per resident chunk publish one immutable collision artifact keyed by chunk
content signature: simplified heightfield or box soup chosen per chunk by
measured contact density. Flat terrain wants heightfield raycast-and-clamp;
built-up voxels want boxes. Physics consumes published artifacts without
knowing generation; a changed signature swaps the artifact in one atomic
publish at the barrier. Border queries consult owning artifact plus
neighbor's, resolved by canonical ownership so each candidate appears
exactly once. Keep the uniform HashGrid as the moving-body index and treat
chunk artifacts as static geometry behind per-chunk indirection refreshed
only on signature change; the standing refusal of per-chunk trees stays
until release measurement says otherwise.

Dual representation doubles the query surface, gate behind one interface
and test both against reference cases. Heightfields misrepresent
overhangs; detect and escalate those chunks to soup during generation.
Collision hides LOD seams by always testing the higher-detail side of a
boundary. Artifact identity is content-derived so replay regenerates
identical collision inputs regardless of load timing.

---

## Determinism discipline

Mostly we have. Three refinements are candidates.

Already enforced: no unordered-container iteration, no wall clock, no
address-derived keys, no thread-id dependence, total orders on pairs and
resting lists, fixed iteration counts chosen by scene properties, worker
output compacted deterministically through per-slot flags, input-order
quickhull.

Refinements:

- Floating-point contraction: compilers fuse multiply-add differently by
  optimization level and target ISA, so identical source produces different
  bits across builds. If cross-build replay ever matters, compile solver
  and integrator translation units with contraction disabled explicitly and
  record compiler flags in the recording header.
- Cross-platform honesty: transcendental functions differ by library. Keep
  quaternion normalization and integration trig to IEEE-specified bit-exact
  operations (add, multiply, divide, sqrt) wherever cross-platform promise
  is contemplated, isolating unavoidable library calls behind one swappable
  function.
- Quantization at boundaries: floats crossing processes in messages should
  serialize bit-preservingly, never through text.

Disabling contraction costs solver throughput; measure, apply per-file if
it bites.

---

## Memory and layout

Patterns worth extracting:

- Fixed-size free lists for per-tick variable-population structures (tree
  nodes, islands, contact rows): allocate blocks sized to historical
  high-water marks, recycle indices, never return memory to the OS during
  play. Extends the cleared-never-freed rule to element lifetimes shorter
  than a tick. Introduce only where per-element recycling within a tick is
  real (candidates: contact rows and manifold slots).
- Per-step temp allocator: one bump allocator owned by the step, freed
  wholesale at step end, used for scratch lists. Allocation cost becomes a
  pointer increment and no leak survives a step.
- Hot-field clustering: solver body layout at sixty-four byte alignment
  with sweep-touched fields first is established; apply the same treatment
  to new hot structures.
- Index-based handles everywhere: proxies, records and solver entries
  reference each other by dense index rather than pointer. Preserve as a
  review invariant whenever a new buffer appears.

---

## Priority ranking

1. Substepped solving and speculative contacts address the largest
   behavioural-cost items: stack convergence and tunneling, both standing
   on foundations the clock and distance function already provide.
2. Island building and oversized-island splitting convert the measured
   border-row tail into parallel work and deliver instant wake propagation.
3. Kinematic capsule controller is the largest gameplay gap, decomposable
   into slices.
4. Persistent pair and manifold caching plus GJK warm starting cut
   steady-state cost in settled scenes and share one cache structure.
5. Fat-AABB incremental broadphase and mesh midphase indexing pay off at
   scale thresholds our benchmarks can name precisely when crossed.
