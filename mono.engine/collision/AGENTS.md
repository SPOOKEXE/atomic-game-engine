# collision - module invariants

L5, `shared` tier. Convex hulls, triangle soups, and the two questions a narrow
phase asks them. This file is what a reviewer should refuse.

## It exists because geometry has to enter the stack from below

`assets::MeshData` is L8 and `physics` is L8. `assets/AGENTS.md` and
`physics/AGENTS.md` both refuse an edge between them, on the grounds the root
`AGENTS.md` gives: two modules at one height must stay disjoint, because a
same-height edge is the one the layer rule cannot adjudicate.

So a mesh collider could not be `physics` reaching sideways for a mesh. It had to
be a module both of them can see, and "both" means below L8. That is the whole
reason for the height, and a change that moves this module up to sit beside
either of them re-opens the question it was created to answer.

**Refuse an edge to `assets`, `scene`, `ecs`, `spatial` or `physics`**, in
either direction. `Engine::core` is the entire dependency list and it should stay
that way. Nothing here knows what an entity is, what a `Collider` component is,
or which file the triangles came out of - a caller hands over points and gets
back a shape.

## `spatial` is above this on purpose, and that is not an accident to fix

The obvious next thing to want here is an index over a mesh's triangles, and
`spatial::HashGrid` at L6 is exactly that structure. It is out of reach by
construction.

That is the decision rather than the cost of it. An index is an *acceleration*
choice, and this repository's rule for one is a measurement taken in `release`
with a number written into a comment - `spatial/AGENTS.md` states it for the grid
and `physics/AGENTS.md` restates it for the cell size. The module that has the
measurement is the consumer, and the consumer is `physics` at L8, where `spatial`
is already linked.

What this module offers instead is the exact answer and a bound to reject
against: `TriangleMesh::TriangleBounds` is stored per triangle so a scan rejects
against six floats in the line it is already walking, and `OverlapTriangles`
tests the whole-mesh bound before any triangle. **Refuse a `HashGrid`, a BVH or
an octree in this module.** If a scan is measured too slow, the index belongs in
`physics` beside the measurement that says so.

## A support function does not need a hull, and that bounds the damage

Everything a general convex-convex test needs is "how far does this shape reach
along a direction, and at which point", and that is answerable from an unordered
point set. `SupportPoint` over a `ConvexHull` and over the cloud it was built
from return the same number.

Two consequences a reviewer should hold to:

- **`BuildConvexHull`'s failure mode is a worse contact manifold, never a missed
  contact.** A coarse hull, a hull that stopped at `MAXIMUM_HULL_POINTS`, a hull
  whose faces fell back to triangles - every one of them still answers overlap
  exactly. That is why the builder is allowed to be pragmatic about degeneracy.
- **The faces are for the two things a point set cannot do**: a manifold wider
  than a point, and being drawn. A change that drops `Faces` to "simplify" takes
  both, and the symptom of the first is a box that will not rest flat.

## The degenerate cases are the specification, not the hostile input

Real input is a baked mesh. Baked meshes are flat planes, single quads, and
models whose vertices were split three ways for texture seams. So "the points are
coplanar" and "two of the four seed points coincide" are the *ordinary* cases.

`BuildConvexHull` therefore returns a usable shape for every one of them:

- Fewer than four distinct points, or all of them on one plane, line or point:
  the points are kept, `Faces` is empty, `Solid()` is false, and support queries
  and the bound are still exact.
- Past `MAXIMUM_HULL_POINTS`: the build stops. The result is convex and contains
  every point it accepted and not the ones it did not - so a caller wanting a
  *bounding* hull has to check the count itself.
- A coordinate that is not finite: dropped before the build. One infinity makes a
  face's offset a NaN, every point then compares "not outside" against it, and
  the result is a hull that swallows the world with nothing having failed.

**Refuse a change that turns any of these into an error return or an assert.**
A collider that refuses to build is a part with no collision at all, which is the
same bug as a wrong hull and harder to see.

## Two builds of one cloud are identical

Quickhull is usually written to take the *furthest* outside point next, because
it converges in fewer rounds. This one takes them in input order, and the seed
points are chosen by extent with the input index breaking a tie.

That is a determinism requirement rather than a preference: the furthest-first
rule makes the result depend on a floating-point maximum, so two builds of one
cloud can differ between compilers and the shapes a client and a server collide
against stop being the same shape. Nothing here reads a clock, an address or a
hash, and `tests/ConvexHull.cpp` pins the property directly.

## A triangle mesh is a surface and has no inside

A body resting on terrain is held up by the triangles it touches. A body that has
been teleported *through* the surface is not pushed back out, because nothing
here can say which side it should have been on.

That is the standing difference from `ConvexHull` and it is why a mesh collider
is for static geometry. **Refuse a change that gives `TriangleMesh` an "inside"**
- a winding-number test, a closed-mesh flag, a ray-cast parity check. Any of them
is a solid, a solid is a different type, and half of one produces a body that is
pushed out of a wall it was standing beside.

## Not here yet, so do not add half of one

- **No convex decomposition.** A character mesh wants several hulls rather than
  one hull with three hundred corners, and that is a bake-time algorithm with its
  own quality knobs. `MAXIMUM_HULL_POINTS` is what stops the single-hull answer
  from being unbounded in the meantime.
- **No serialisation.** These are built from points and rebuilt from points.
  Writing a hull to disk means a file that can claim a bound, a winding and a
  face set that do not agree with each other - `assets::MeshData` states the same
  refusal about its own bound and for the same reason.
- **No shape library or name lookup.** Which shape a collider *uses* is a
  question about a world, and a world is an `ecs::Store`, which is not something
  this module knows about. The table lives with whoever owns the naming.
- **No distance or penetration query.** GJK and EPA read the support function
  this module exposes and are `physics`' business: they need two shapes in two
  different frames, and a frame is a thing in a world.
- **No `Vector2` and no 2D hull.** A flat cloud already builds here and answers
  every query; what it does not get is faces, which a 2D hull would also not
  give a 3D narrow phase.
