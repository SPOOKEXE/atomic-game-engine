# Non-Euclidean worlds, and what this engine would need

An investigation, filed by `ROADMAP.md` v0.14 against CodeParade's *Non-Euclidean
Worlds Engine* — the demo whose claim is that impossible spaces need no exotic
maths, only portals: a camera at the other end of a hole, drawn into the hole,
recursively. Rooms bigger on the inside, corridors that loop, a cube with five
outsides.

The roadmap's own guess was "cameras plus portal parts". That guess is right, and
this repository is further along than it looks: **the camera half already
exists.** What follows is what is there, what a portal is that a mirror is not,
and what each missing piece actually costs.

---

## What already exists

`SurfaceCamera`, since v0.13. An instance parented to a face of a part, placed
every frame by `scene::AimSurfaceCameras`, rendered into a texture by the
renderer's surface pass, and sampled by `opaque.frag` where the pane projects it.
Five properties of it matter here:

- **The pane samples by projection, not by UV.** The fragment divides
  `inSurfacePosition` and tests the result against the 0..1 rectangle, falling
  back to the plainly-lit pane outside it. A portal wants exactly this: what is
  seen through the hole is a window onto another camera's image, aligned to the
  eye rather than to the geometry.
- **Recursion is already free, and already lagged.** The surface pass draws the
  *other* surfaces using the textures they had last frame — `Renderer.cpp` says
  so where it mixes every camera's matrix into one signature. Two facing mirrors
  build their corridor over frames rather than in one. A portal through a portal
  behaves the same way, and this is the single most expensive property to change.
- **Sixteen surfaces per world.** `scene::MAX_SURFACES`, each with its own target
  and its own resolution.
- **Per-surface visibility already exists.** `SurfaceCamera::TagFilter` is a tag
  mask applied per instance in the draw loop, so a surface can be told to draw
  *some* of the world. That is the mechanism an impossible space needs, and it is
  already there for an unrelated reason.
- **The near plane is already pushed off the pane** — `SurfaceCameras.hpp` calls
  it the poor man's oblique clip — so the idea that the view has to be clipped at
  the surface is built in, just approximately.

---

## What a portal is that a mirror is not

### 1. The transform. Small.

A mirror derives its camera by reflecting the eye through its own plane. A portal
derives it by mapping the eye through `destination · source⁻¹`, usually with a
half-turn so the camera looks *out* of the far side.

That is one branch in `AimSurfaceCameras` and one component — a `Portal` carrying
the entity it is linked to. Everything downstream is unchanged: the camera is
still a `SurfaceView`, the pane still samples the projection, the tag filter
still works.

**This is also where the non-Euclidean part lives.** Nothing constrains the pair
of frames to be consistent with a single space. A destination scaled, rotated or
placed anywhere gives a room bigger on the inside or a corridor that turns more
than four right angles — with no separate feature and no maths beyond a matrix
multiply. That is the whole insight of the demo.

**"Scaled" was the one word this document oversold for a version.** The map was
a `CFrame` — a position and a quaternion — so a mismatched pair rendered a
source-sized window onto a full-sized room and a body walked out of it the size
it went in. It is a `scene::SeamTransform` since v0.15: the same rigid product,
plus the ratio of the two panes, taken about the source pane's centre. The
camera, the pane's sampling matrix, a crossing body, a clone, a ghost, the
camera arm and a portal-crossing ray all go through it.

### 2. A real oblique near-plane clip. The pinch point.

Today's approximation moves the near plane parallel to the face. On a mirror that
costs a little over-clipping at grazing angles, which nobody notices. **On a
portal it is fatal**: the destination is set into a wall, and everything between
the destination camera and the far room — the wall itself, its back face, whatever
sits behind it — is inside the frustum and draws over the view. The hole would
show the back of the wall it leads through.

The fix is standard (Lengyel's oblique frustum): skew the projection's near plane
onto the destination portal's plane so nothing behind it survives clipping.

The obstacle here is a type, not the maths. `SurfaceView::Lens` is a
`scene::Camera` — a field of view and two distances — so there is nowhere to put
a clip plane or a skewed matrix. That field has to become a projection the
renderer is handed rather than one it derives, which touches `SurfaceView`, the
surface pass, and the signature that decides whether a surface needs redrawing.

**`SurfaceCameras.hpp` already wants this change for a different reason**: the
fitted frustum is symmetric about the face normal, so a viewer standing off to one
side wastes half the texels, and the file names an off-axis frustum as what it is
waiting on. An off-axis projection and an oblique near plane are the same edit to
the same type. Doing both at once is most of the work of a portal.

### 3. Traversal. Not a renderer problem, and it has no home yet.

Seeing through a hole is half the feature; walking through it is the other half.
That means detecting the eye crossing the portal's plane inside its rectangle,
applying the same transform to the body and its velocity, and doing it between two
ticks so nothing renders a frame of the wrong side.

**There is nowhere to put that yet.** The character controller is `ROADMAP.md`
v0.15 — there is no player body, no controller and no camera rig to hand a
teleport to. `world::Postbox::Teleport` is a *cross-world* authority operation,
which is a different thing: it moves a player between simulations, not between two
places in one.

So a portal built today is one you can look through and not one you can use, and
that is an argument for ordering rather than against the feature.

### 4. Overlapping space. Solved in principle, unproven in practice.

A room bigger on the inside is two regions of one world that would occupy the same
coordinates. Lighting, culling and physics all work in that one space, so
overlapping them means each region seeing the other's geometry.

`TagFilter` is the answer already in the engine: tag each region, give each
portal camera the mask of the region it looks into, and the surface pass draws
only that. Physics has no such filter, which is where an overlapping build would
break first — two rooms in one place share one broadphase.

The cheap version avoids the problem entirely: place the regions *apart* in world
space and let the portals do the lying. That is enough for corridors that loop,
rooms bigger inside, and every hallway trick in the demo. Only literal
interpenetration needs more.

---

## The two limits worth knowing before starting

**One frame of lag per recursion level.** A portal's texture is last frame's, so
walking towards a portal shows a view that is one frame stale, and a portal seen
through a portal two. For a mirror this is invisible. For a hole somebody is
walking through, at 60 fps, on the frame they cross — it is a visible seam, and
the standard fix (render the portal chain inside the frame, deepest first) is
exactly the ordering the current design gives up in exchange for its cheapness.
Changing it means the surface pass becoming a recursive pass with its own budget.

**Sixteen surfaces, shared with mirrors.** A scene of portals is a scene with a
small, fixed number of them. That is a reasonable limit for the demo's kind of
level and not for a world dotted with them.

---

## What it would take, in order

**The first four rows landed at v0.14.** Left in place rather than rewritten,
because the ordering argument is the useful part of this document and it held:
each row genuinely unblocked the next, and the fourth turned out to be the small
one the table said it was.

| Step | Size | Status |
|---|---|---|
| `SurfaceView` carries a projection rather than a field of view | small | **done** |
| Off-axis frustum fitted to the pane's rectangle | small | **done** |
| Oblique near plane at the destination | small | **done** |
| `Portal` component pairing two parts, placed like a surface camera | small | **done** |
| Traversal — cross the plane, move the body, remap velocity | medium | **done**, at v0.14 — the controller arrived early |
| In-frame recursion, deepest first | large | open, and a rendering decision |

Four more landed at v0.15, out of a second reading of the demo once there was
something to compare against. `NON_EUCLID.md` at the repository root is that
comparison and what each of them cost; in short:

| Step | Size | Status |
|---|---|---|
| A visibility gate on the surface pass — the demo's occlusion query, on the CPU | small | **done** |
| The edge-on band applies to mirrors and not to holes | small | **done** |
| A landing clearance, so nothing rests on a plane it just crossed | small | **done** |
| Scale-carrying portals — a hole that changes what goes through it | medium | **done** |

### What building it actually taught

**The four steps are one code path, not four.** Every one of them is a special
case of *take a placement transform, map the pane by it, and fit the camera to
the mapped rectangle*. A mirror's transform is the reflection through its own
plane — which **fixes** that plane, so the mapped corners are the pane's own
corners and the mapped plane is the pane's own plane. The reflection arithmetic
that was already here falls out of the general rule rather than sitting beside
it, and the portal branch is a few lines choosing a different map.

**The rectangle must be the mapped *source* pane and never the destination
part.** `opaque.frag` shades a fragment of the source pane by projecting it
through the camera's matrix, and that lines up only because the camera and the
rectangle were moved by the same transform. Fitting to the destination is correct
exactly when the two panes are the same size and silently wrong — an image
sliding across the hole — whenever they are not. This is the one place the design
could have been plausibly wrong and passed a screenshot.

**The oblique clip has a depth-range trap.** Lengyel's published derivation maps
the near plane to `-1`, so it substitutes `C · 2/(C·Q)` and subtracts the `w`
row. `GLM_FORCE_DEPTH_ZERO_TO_ONE` is pinned engine-wide in `core`'s build, where
near is `0`, and the substitution is `C/(C·Q)` with nothing subtracted. The wrong
form compiles, runs, and reads as z-fighting rather than as a matrix mistake —
which `scene::CameraMatrices` already warned about for an unrelated reason.

**The one mistake the engine cannot catch is authoring, and the demo made it.**
`Face` is resolved on the destination as well as on the source — the far frame is
the portal's own `Face` applied to the destination's transform — so a hole shows
whatever *that* face points at. Aim it at a wall whose matching face points out
of its room and every part of the machinery is satisfied: the camera is placed,
the frustum fits the mapped rectangle, the oblique clip is built, the pane is
given a surface index, and the picture is the empty space behind the far wall.
The first version of `Portals-1-world.luau` paired each room's north wall with
the far room's *south* wall — which is what a corridor looks like on paper — and
all four holes rendered nothing at all.

Two things follow. The rule for an author is that a destination must be a part
whose matching face points at the space the hole should show, which for unrotated
rooms is the wall on the *same* side of the far room and in general is a rotation.
And the test that catches it cannot be about where the camera stands, because
that was right: the invariant is that **the half-space the oblique clip keeps
contains the middle of the room the hole names**, which needs nothing from the
scene but the room's own centre. `examples/tests/Scene.cpp` asserts it per hole.

**Removing the field of view removed a clamp, and the clamp was a bug.** The old
symmetric fit took a *tangent* of a half-angle, so it needed a ceiling just under
180° — and that ceiling made the fit a step function, which is what read as the
mirror flashing once per orbit. Off-axis extents are a min and a max over four
projected positions; there is nothing to saturate against, so the clamp is gone
rather than retuned. The regression test that measured this had to move from an
angle to an angle *derived from the extents*: measuring the raw extents instead
reports enormous steps near the crossing, because a span is a tangent and
legitimately grows without bound there.

## Recommendation

**Worth adding, after v0.15, and cheaper than it sounds — but not as its own
subsystem.** A portal here is a `SurfaceCamera` with a different rule for where
it stands, and the honest way to build it is to finish the surface camera's own
outstanding work (a projection it is handed, off-axis, obliquely clipped) and then
add the pairing. Anything that started with a new "portal renderer" would be a
second copy of the surface pass.

What must not be promised until the sixth row above is done is a portal somebody
walks through without seeing a seam. Filed as `D00112`.

**Taken, at v0.14, and the estimate held.** The recommendation said "after v0.15"
on the strength of traversal being the point; the first four rows turned out to
be worth having on their own, because looking through a hole into somewhere that
is not behind it is the part that demonstrates the idea. The pairing was indeed
one branch and one component, and the three rows in front of it were indeed what
cost the work — exactly as this document predicted, and for the reason it gave:
they are the surface camera's own outstanding debts, and a portal is what made
paying them urgent rather than tidy.

What is still not promised is a portal somebody walks through without a seam, and
that is now the whole of `D00112`.
