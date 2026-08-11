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

| Step | Size | Why in this order |
|---|---|---|
| `SurfaceView` carries a projection rather than a field of view | small | unblocks both of the next two, and pays for itself in mirror texels |
| Off-axis frustum fitted to the pane's rectangle | small | already wanted; makes a portal a *window* rather than a cone |
| Oblique near plane at the destination | small | without it a portal shows the wall it leads through |
| `Portal` component pairing two parts, placed like a surface camera | small | the whole non-Euclidean trick is this matrix |
| Traversal — cross the plane, move the body, remap velocity | medium | needs the v0.15 character controller to exist |
| In-frame recursion, deepest first | large | the only way to remove the crossing seam |

The first four are one change of a few hundred lines and produce something
demonstrable: a wall you can see through into somewhere that is not behind it,
with a room bigger on the inside. The fifth needs v0.15. The sixth is a rendering
decision, not a feature, and belongs with the render-graph work `ROADMAP.md`
already files behind a prototype project.

---

## Recommendation

**Worth adding, after v0.15, and cheaper than it sounds — but not as its own
subsystem.** A portal here is a `SurfaceCamera` with a different rule for where
it stands, and the honest way to build it is to finish the surface camera's own
outstanding work (a projection it is handed, off-axis, obliquely clipped) and then
add the pairing. Anything that started with a new "portal renderer" would be a
second copy of the surface pass.

What must not be promised until the sixth row above is done is a portal somebody
walks through without seeing a seam. Filed as `D00112`.
