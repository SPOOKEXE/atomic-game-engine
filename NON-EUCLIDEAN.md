# Non-Euclidean worlds

Portals, and what it takes for one to be a **hole** rather than a picture of one.

This file is three readings of the same problem, merged, plus the design that
comes after them:

1. **The investigation** (v0.14) — filed by `ROADMAP.md` against CodeParade's
   *Non-Euclidean Worlds Engine*, whose sources are in `temp/NonEuclidean/`. It
   asked whether the approach was right and what each missing piece would cost.
2. **The re-read** (v0.15) — the demo read again once there was something to
   compare it against, and the four things it does better.
3. **The pivot and the port** (v0.15) — the discovery that a portal is not a
   surface camera, and the recursive pass that followed.
4. **The wormhole contract** (this version) — what is still missing for a hole
   that carries *everything* through it: bodies, contacts, light and shadow.
   Parts I to IV are what was built and why; **Part V is the design of what is
   not built**, stated as contracts rather than as intentions.

The one-line summary of the maths, which has not changed across any of the four:
**the eye, the body, the ray and the photon are all mapped by
`destination · half-turn · source⁻¹`, and nothing constrains the two frames to
describe one space.** That is the whole of the non-Euclidean part. Everything
else in this file is schedule, budget and the seams between passes.

---

# Part 0 — the model

Stated first because Parts I to IV kept re-deriving it, and because Part V is
nothing but this rule applied to four more things.

## The rule

A portal pair **identifies two half-spaces**. For a pane with map `M`
(`scene::SeamTransform`), a quantity `q` observed on the near side is the same
quantity as `M(q)` observed on the far side. There is **one `M` per pane**; the
far pane's map is its exact inverse, so a round trip closes from either face.

`M` is a *similarity*, not a rigid motion: a rotation, a translation and a
scale, taken about the source pane's centre. It exposes four applications
because four kinds of thing go through a hole and they are not the same kind of
thing:

| Application | For | Scales |
|---|---|---|
| `Point` | a position | yes |
| `Carry` | a velocity, an offset, a half-extent | yes |
| `Rotate` | a unit direction, a normal, a clip plane's normal, a ray's aim | no |
| `Place` | a placement | position only |

Every consumer names which it wants. Getting `Rotate` and `Carry` the wrong way
round in `Query.cpp` would make a portal-crossing ground ray report a floor at
the wrong range, which is a character falling through a floor it is visibly
standing on — the exact class of bug the naming exists to make visible in
review.

## The five transports

Everything a hole has to carry, as separate mechanisms, because they are
separate passes with separate budgets:

| | What crosses | Where it lives | State |
|---|---|---|---|
| **T1 view** | the camera | `render::PortalView`, the recursive pass | built, v0.15 |
| **T2 body** | placement, velocity, size, yaw | `scene::CrossPortals` | built, v0.14–v0.15 |
| **T3 image** | the far half of anything standing in the seam | `AppendPortalClones` / `AppendPortalGhosts` | built, **uncut** — Part V.1 |
| **T4 contact** | the far room's floor and walls, under a straddler | `physics::GhostPortalBodies` | **named in two headers and does not exist** — Part V.2 |
| **T5 light** | direct light, local lamps, shadow | nothing | **not built** — Part V.3 |

A hole is *seamless* exactly when all five agree about where the seam is. Every
artefact in this file is two of them disagreeing.

## What must not happen

**A second renderer.** Said in the investigation, said again in `D00112`, and it
still holds with one correction: the recursive portal pass *is* a pass of its
own, and that turned out to be right — but it draws the same instance ranges,
through the same pipeline, with the same lighting uniforms as the screen pass.
It is a different **camera schedule**, not a different renderer. Anything that
grows its own material model, its own light set or its own draw list will drift
from the screen on the first lighting change.

---

# Part I — the investigation, v0.14

The demo's claim is that impossible spaces need no exotic maths, only portals: a
camera at the other end of a hole, drawn into the hole, recursively. Rooms
bigger on the inside, corridors that loop, a cube with five outsides.

The roadmap's own guess was "cameras plus portal parts". That guess was right,
and the repository was further along than it looked: **the camera half already
existed.**

## What already existed

`SurfaceCamera`, since v0.13. An instance parented to a face of a part, placed
every frame by `scene::AimSurfaceCameras`, rendered into a texture by the
renderer's surface pass, and sampled by `opaque.frag` where the pane projects
it. Five properties of it mattered:

- **The pane samples by projection, not by UV.** The fragment divides
  `inSurfacePosition` and tests the result against the 0..1 rectangle, falling
  back to the plainly-lit pane outside it. A portal wants exactly this: what is
  seen through the hole is a window onto another camera's image, aligned to the
  eye rather than to the geometry.
- **Recursion was already free, and already lagged.** The surface pass drew the
  *other* surfaces using the textures they had last frame. Two facing mirrors
  built their corridor over frames rather than in one.
- **Sixteen surfaces per world.** `scene::MAX_SURFACES`, each with its own
  target and its own resolution.
- **Per-surface visibility already existed.** `SurfaceCamera::TagFilter` is a
  tag mask applied per instance in the draw loop, so a surface can be told to
  draw *some* of the world. That is the mechanism an impossible space needs, and
  it was already there for an unrelated reason.
- **The near plane was already pushed off the pane** — the header called it the
  poor man's oblique clip — so the idea that the view has to be clipped at the
  surface was built in, just approximately.

## What a portal is that a mirror is not

### 1. The transform. Small.

A mirror derives its camera by reflecting the eye through its own plane. A
portal derives it by mapping the eye through `destination · source⁻¹`, with a
half-turn so the camera looks *out* of the far side.

**This is where the non-Euclidean part lives.** Nothing constrains the pair of
frames to be consistent with a single space. A destination scaled, rotated or
placed anywhere gives a room bigger on the inside or a corridor that turns more
than four right angles — with no separate feature and no maths beyond a matrix
multiply. That is the whole insight of the demo.

**"Scaled" was the one word this document oversold for a version.** The map was
a `CFrame` — a position and a quaternion — so a mismatched pair rendered a
source-sized window onto a full-sized room and a body walked out of it the size
it went in. It is a `scene::SeamTransform` since v0.15.

### 2. A real oblique near-plane clip. The pinch point.

The old approximation moved the near plane parallel to the face. On a mirror
that costs a little over-clipping at grazing angles, which nobody notices. **On
a portal it is fatal**: the destination is set into a wall, and everything
between the destination camera and the far room — the wall itself, its back
face, whatever sits behind it — is inside the frustum and draws over the view.
The hole would show the back of the wall it leads through.

The fix is standard (Lengyel's oblique frustum): skew the projection's near
plane onto the destination portal's plane so nothing behind it survives
clipping.

The obstacle was a type, not the maths. `SurfaceView::Lens` was a
`scene::Camera` — a field of view and two distances — so there was nowhere to
put a clip plane or a skewed matrix. That field had to become a projection the
renderer is handed rather than one it derives.

**The header already wanted this change for a different reason**: the fitted
frustum was symmetric about the face normal, so a viewer standing off to one
side wasted half the texels. An off-axis projection and an oblique near plane
are the same edit to the same type.

### 3. Traversal. Not a renderer problem.

Seeing through a hole is half the feature; walking through it is the other half.
That means detecting the eye crossing the portal's plane inside its rectangle,
applying the same transform to the body and its velocity, and doing it between
two ticks so nothing renders a frame of the wrong side.

At the time there was nowhere to put it — no player body, no controller, no
camera rig. The controller arrived early and `scene::CrossPortals` landed at
v0.14.

### 4. Overlapping space. Solved in principle, unproven in practice.

A room bigger on the inside is two regions of one world that would occupy the
same coordinates. Lighting, culling and physics all work in that one space, so
overlapping them means each region seeing the other's geometry.

`TagFilter` is the answer already in the engine: tag each region, give each
portal the mask of the region it looks into, and the pass draws only that.
**Physics has no such filter**, which is where an overlapping build breaks
first — two rooms in one place share one broadphase.

The cheap version avoids the problem entirely: place the regions *apart* in
world space and let the portals do the lying. That is enough for corridors that
loop, rooms bigger inside, and every hallway trick in the demo. Only literal
interpenetration needs more.

## What building it taught

**The four steps are one code path, not four.** Every one of them is a special
case of *take a placement transform, map the pane by it, and fit the camera to
the mapped rectangle*. A mirror's transform is the reflection through its own
plane — which **fixes** that plane, so the mapped corners are the pane's own
corners and the mapped plane is the pane's own plane. The reflection arithmetic
falls out of the general rule rather than sitting beside it.

**The rectangle must be the mapped *source* pane and never the destination
part.** `opaque.frag` shades a fragment of the source pane by projecting it
through the camera's matrix, and that lines up only because the camera and the
rectangle were moved by the same transform. Fitting to the destination is
correct exactly when the two panes are the same size and silently wrong — an
image sliding across the hole — whenever they are not. This is the one place the
design could have been plausibly wrong and passed a screenshot.

**The oblique clip has a depth-range trap.** Lengyel's published derivation maps
the near plane to `-1`, so it substitutes `C · 2/(C·Q)` and subtracts the `w`
row. `GLM_FORCE_DEPTH_ZERO_TO_ONE` is pinned engine-wide in `core`'s build,
where near is `0`, and the substitution is `C/(C·Q)` with nothing subtracted.
The wrong form compiles, runs, and reads as z-fighting rather than as a matrix
mistake.

**The one mistake the engine cannot catch is authoring, and the demo made it.**
`Face` is resolved on the destination as well as on the source, so a hole shows
whatever *that* face points at. Aim it at a wall whose matching face points out
of its room and every part of the machinery is satisfied: the camera is placed,
the frustum fits, the clip is built, the pane is given a slot, and the picture is
the empty space behind the far wall. The first `Portals-1-world.luau` paired each
room's north wall with the far room's *south* wall — which is what a corridor
looks like on paper — and all four holes rendered nothing at all.

The rule for an author is that **a destination must be a part whose matching
face points at the space the hole should show**. The test that catches it cannot
be about where the camera stands, because that was right: the invariant is that
**the half-space the oblique clip keeps contains the middle of the room the hole
names**. `examples/tests/Scene.cpp` asserts it per hole.

**Removing the field of view removed a clamp, and the clamp was a bug.** The old
symmetric fit took a *tangent* of a half-angle, so it needed a ceiling just under
180° — and that ceiling made the fit a step function, which is what read as the
mirror flashing once per orbit. Off-axis extents are a min and a max over four
projected positions; there is nothing to saturate against.

## The order it was built in, and it held

| Step | Size | Landed |
|---|---|---|
| `SurfaceView` carries a projection rather than a field of view | small | v0.14 |
| Off-axis frustum fitted to the pane's rectangle | small | v0.14 |
| Oblique near plane at the destination | small | v0.14 |
| `Portal` component pairing two parts | small | v0.14 |
| Traversal — cross the plane, move the body, remap velocity | medium | v0.14 |
| In-frame recursion, deepest first | large (it was small) | v0.15 |

---

# Part II — measured against the demo, v0.15

A second reading of `temp/NonEuclidean/`, against what this engine actually
built.

The answer, in one line: **the maths is the same and the schedule is not.** Both
engines map the eye by `destination · source⁻¹`, clip obliquely at the far
plane, and projective-texture the pane. CodeParade resolves the chain *inside one
frame, depth-first*; we resolved it *across frames, one bounce per frame*. Nearly
every other difference followed from that.

## Visually

| | NonEuclidean demo | atomic, at the time |
|---|---|---|
| Recursion | In-frame, depth-first, `GH_MAX_RECURSION = 4` | Frame-lagged, one bounce per frame |
| Chain terminus | A pink quad (`DrawPink`, `pink.frag`) | Last frame's texture |
| Sub-camera projection | **The screen's own projection, unchanged**, into a 2048² FBO | Off-axis frustum fitted to the pane's corners |
| Oblique clip | Lengyel, `-1..1` form (`Camera::ClipOblique`) | Lengyel, `0..1` form |
| Sampling | `gl_Position.xy / w * 0.5 + 0.5` (`portal.frag`) | The same projective idea through the surface camera's matrix |
| Culling | GPU occlusion queries, per portal per level | Content signature — skip if nothing that affects the image moved |
| Near the glass | `clamp(nearestPortalDist * 0.5, 1e-3, 1e-1)` | The pane **stopped drawing** inside `EDGE_ON_MARGIN` |
| A body in the seam | Nothing — objects pop across the plane | Clones and ghosts on both sides |
| Pane orientation | **Yaw only** — `Portal::Draw` asserts `euler.x == 0 && euler.z == 0` | Arbitrary |
| Budget | 16 portals × 3 FBOs each at 2048² | 16 surface slots, each sized to its own pane |

Three rows are worth expanding.

### Their sub-camera needs no fit at all, and that is not laziness

The portal view is rendered with the *identical* projection matrix as the
screen — only skewed. So the pane's clip-space position in the main view is by
construction the same coordinate as the sub-render's normalised position, and
the projective lookup is exact in two lines of shader: no `FIT_MARGIN`, no
`MINIMUM_DEPTH` floor, no degenerate-rectangle guard, no edge-on band.

This was written down as a trade worth keeping. **It was not**, and Part III is
why: the fit is right for a mirror and wrong for a hole, and the whole apparatus
went away for portals when the pass did.

### Their occlusion culling has no counterpart here

They draw each portal as a depth- and colour-masked proxy under
`GL_SAMPLES_PASSED`, then skip the recursive render entirely when zero samples
pass. That is **visibility** culling. The `Refresh` flag is **temporal**
culling — "did anything change". The two are orthogonal, and only one existed.

### Their near-glass decision is the opposite of ours, and theirs is right for holes

We blanked a pane when the viewer was within 0.3 studs of its plane. That band
exists to kill the 180° flip when a viewer crosses a **mirror's** plane, where
`facing` is +1 on one side and -1 on the other and no continuous path joins them.

**For a linked portal the flip cancels itself.** The frame the viewer's `facing`
sign changes is the same frame `CrossPortals` / `PortalCrossing` carries the eye
through the pane, so `through · eye` stays continuous across the crossing. The
demo depends on exactly that cancellation, and it is why they can shrink the near
plane to a millimetre and walk face-first into the glass.

So we were applying a mirror's fix to a hole, in the one band where somebody
walking through it is guaranteed to be.

## Physically

`Physical::TryPortal` and our `CrossingOf` are the same four steps, arrived at
independently: signed distance either side, reject on matching signs, interpolate
`da / (da - db)` to the plane, then test the hit against the rectangle by
projecting onto its half-axes normalised by their own squared length.

Where they differ:

- **They bump, we do not.** `TryPortal` offsets the test plane by
  `2 · GH_NEAR_MIN · p_scale` toward the side the body came from, and lands it at
  `pos - bump · 2` on the far side.
- **They carry scale, we could not.** `p_scale *= warp->deltaInv.XAxis().Mag()`
  physically rescales the crosser. Ours returned a `CFrame` — no scale. Fixed at
  v0.15; see Part II's fourth item below.
- **They step at 500 Hz.** `GH_DT = 0.002`, up to 30 substeps a frame. A segment
  test is trivially safe at a quarter-centimetre a tick. Ours has to be right at
  60 Hz — it is, but it is the harder version of the same problem.
- **They have no seam problem because they have no body.** First person, no drawn
  character, and levels laid out so both rooms' floors line up at the doorway.
  `OpenPortals`, `RaycastThroughPortals` continuing a ground ray out of the far
  side, and the camera arm carried through in `PlaceCamera` are all answers to a
  question they never had to ask.
- **Yaw fix-up is the same idea with different plumbing.** They write
  `euler.y = -atan2(newDir.x, -newDir.z)` straight onto the object. We record
  `PortalTransit{Serial, Turn}` and apply it in `FollowPortalTransit`, because the
  simulating host is not the looking host.
- **Cross-world portals are ours alone.** `Portal::DestinationWorld` — a hole into
  a different simulation — has no counterpart there.

## The decisions, self-played

**Q. Is the visibility gate a renderer change or a scene change?**
Renderer. `graph::CullAndBound` already runs against the main camera's frustum
well before the refresh decision, so the gate is one more condition on `Refresh`,
computed from data the frame already has.

**Q. What breaks if a pane is only visible inside another mirror?**
Gating on the main camera alone would freeze it. The fix is a union rather than a
special case: a surface refreshes when its pane is visible from the main camera
**or** from any other surface camera that is itself refreshing. At most 16 panes
against 17 frustums — 272 box tests, once a frame. It must be a fixed-point of
one pass, not an iteration.

**Q. Should `EDGE_ON_MARGIN` be deleted or made conditional?**
Conditional. The flip it removes is real for a mirror and there is no
cancellation to rely on there. A linked portal has the cancellation, so the band
applies to the mirror branch of `AimSurfaceCameras` and not to the `linked` one.

**Q. Where does the traversal bump go — `CrossingOf` or `CrossPortals`?**
`CrossingOf` tests and `CrossPortals` moves, so the bump is two halves in two
places. Putting both in `CrossingOf` would make `PortalCrossing`'s camera-arm
caller — which does not move anything — pay for a margin it has no use for.

**Q. Can portal scale be added without a scale in `CFrame`?**
Yes, and it must be. `CFrame` is rigid on purpose and every transform in the
engine assumes it. Scale is a second field on `SeamTransform`, applied separately
to `Bounds::HalfExtent`, `Motion::Linear`, humanoid speeds and the character's
own rig.

## The work, as built

| | Landed as | Test |
|---|---|---|
| Visibility gate | `graph::VisibleSurfaces` | `graph/tests/Frustum.cpp`, three cases |
| Portals exempt from `EDGE_ON_MARGIN` | one `!linked &&` | `scene/tests/SurfaceCameras.cpp`, two cases |
| Traversal bump | `LANDING_CLEARANCE`, landing only | `scene/tests/SurfaceCameras.cpp`, one case |
| Scale-carrying portals | `SeamTransform`, `PortalSeam::Scale` | `scene/tests/SurfaceCameras.cpp`, three cases |

### 1. The visibility gate moved out of the renderer

Written in `Renderer::Render` first and then moved to `graph::VisibleSurfaces`,
because `Renderer::Render` needs a GPU and the decision does not. It is boxes
against frusta over a draw list, which is what `graph` already is.

The union sweep is the part that would have been wrong without writing it down:
gating on the main camera alone freezes a mirror seen only inside another mirror,
which is a much louder artefact than the redraw it saves.

### 2. The band is a mirror's, and the header said so before the code did

Three lines. Nothing replaced it — the three floors that keep the matrix finite
at the plane (`MINIMUM_DEPTH`, `FIT_MINIMUM_SPAN`, and `SurfaceProjection`
refusing to skew against a plane the camera is on) were all already there.

The test that matters is not the one that checks a portal renders in the band. It
is the sweep: the eye walks the last sixty centimetres at the pane in
one-centimetre steps and the camera is required to move less than five
centimetres per step. A flip would move it by twice its distance from the pane.

### 3. Only half the demo's bump was taken, and the other half would have been a bug

CodeParade bumps twice: the landing *and* the plane the crossing test is made
against. The second half is hysteresis at 500 Hz and a trap at 60. Offsetting the
test plane means a body that begins a tick inside the offset never sees a sign
change — at 2 mm per tick nothing can start there, but at a quarter of a stud per
tick a body can begin a tick anywhere, and the offset becomes a band you walk
through without ever crossing.

**What the test found, which is not fixed.** A body whose step ends *exactly* on
the plane, arriving from the negative side of the normal, never crosses —
`from == 0` counts as behind, so both ends test the same side. It is measure-zero
in floats, it cannot arise from a crossing now that landings are clear, and
`OpenPortals` stops the solver parking anything there. Named here rather than
guarded, because the guard would be a second sign convention.

### 4. Scale is a similarity, and the four applications had to be named

`SeamMapping` returns a `SeamTransform` — the rigid map, the source pane's
centre, and a scale. The scale is the square root of the two panes' area ratio,
which is the ratio of their sides for any pair of the same shape and the only
definition that does not depend on which axis `FaceAxes` called first.

**What crossing resizes**, and the list is longer than the multiply suggests: a
crate is its `Bounds` and its `Collider`; a character is those on a root nobody
can see, a `Humanoid` on a third entity holding every figure that decides how it
moves, and five limbs that are their own rows with their own boxes and their own
rest offsets. Mass follows from the box through `PhysicsProperties`.

**Gravity is not scaled, and that is the one place this deliberately parts from
the demo.** CodeParade multiplies it by the crosser's accumulated scale so that
shrinking is imperceptible, which is right for a gag about a tunnel. Here there
is one world, `scene::Gravity` is its property, and a small thing in it should
fall the way a small thing falls.

## The bugs found by running it

### A spare character standing near the hole

`client::CollectInstances` called **both** `AppendPortalClones` and
`AppendPortalGhosts` over the same world. They are the same answer reached two
ways — one walks the world for things that can move, the other walks the draw
list — so every straddling body got two far-side copies z-fighting each other.
Worse, the ghost pass reads the list it appends to, so a clone was mapped back
again and a third copy landed on top of the original.

The clone pass is the one kept on the client path, because it walks entities and
can therefore ask what a draw instance cannot — whether a thing is able to move at
all. `client::CollectReplicatedInstances` keeps the ghost pass and has to: a
replica has a draw list and no simulation behind it.

### The far room's floor drawn across the near room

The straddle test widens every check by the body's own reach, which is right for
a character and absurd for a slab. A floor fifty studs across has a reach of
seventy, so its centre is within reach of every plane in the building — and what
got drawn was a copy of the far room's floor laid over the near one, meeting it
along a hard straight line through the middle of the scene.

The first fix was a size rule, and **the size rule was wrong in the direction
that removes the feature**: a person is very nearly as big as the doorway they
walk through, so a five-stud character with a reach of three was refused by a
four-by-five doorway with a shorter half-axis of two. Every character in every
hole was refused while the tests, which measured a floor slab against a
sixteen-by-nine pane, stayed green.

What the rule was standing in for is *is this the room, or a thing in it*, and
the answer to that is **which query found it**. `CloneThroughSeams` walks bodies
that can move and character limbs, so a floor is never a candidate there.
`AppendPortalGhosts` reads a draw list, where an instance is a frame and a box —
so the collector says instead: `DrawInstance::Movable` took the row's last byte
of explicit padding and is set from `Motion` and `CharacterLimb`. The size
heuristics are gone.

**The one byte of padding was carrying a test.** A case asserted that a signature
must *not* move with `Reserved`, because depending on padding is depending on
nothing. That byte means something now, so the case asserts the opposite.

### The other half of "render on both sides": the hole has to show it

A copy in the draw list is drawn by the *screen* pass, which puts the far half in
the far room. Somebody looking **at** the pane sees the surface texture instead,
so the copy has to reach the portal pass too — or the picture in the hole shows a
room with nobody in it while the body is visibly standing in the doorway.

It does, and it is asserted rather than assumed: `OrderScene` puts a copy in the
`Reflected` run — the opaque part of the scene range that is not itself a
mirror — which is exactly the range the surface and portal passes submit. A copy
lands there because it carries `Surface = -1` and the original's transparency,
and the ordering has no other opinion.

**This is the invariant Part V.1 must not break.** A seam run placed after the
surface runs would leave `plan.Reflected`, and a straddler would vanish from the
picture in the hole — which is the same artefact, arrived at from the other
direction.

### The camera flipping on some crossings and not others

`PlaceCamera` put the third-person arm through the **nearest** pane its segment
met. `CrossPortals` moved the body through the **first pane gathered**, which is
archetype order — and archetype order moves whenever anything in the world
changes a component set. On the frames the two chose differently, the body went
through one hole and the eye through another.

They are one function now — `NearestCrossing`. The regression test asserts that
the *farther* pane is gathered first, which is what makes it a guard rather than
a coincidence.

### A body in a hole cast no shadow on the far side

`AppendPortalGhosts` forced `CastShadow` off, and the note explaining why said a
second shadow would follow a body that is not there. That is true of a copy
standing beside its original and false of this one: the map takes it to the far
pane, which is a different room. The ghost keeps the instance's own `CastShadow`
now.

*(This fix is correct and incomplete. It gives the far half **a** shadow; Part
V.3 is about it being the **right** shadow.)*

### The character snapped for a frame or two after crossing

`CapturePreviousTransforms` records where a body was when the tick began, and the
renderer blends that against `Transform` by the tick's alpha. `CrossPortals` runs
in `PostSimulation` — *after* that capture — so a body it teleports is
interpolated **across the teleport**, and at three frames to a tick it is drawn
once or twice somewhere in the hundred units between the two panes.

`PreviousTransform` now goes through the same map as the placement and the
velocity. Mapped rather than collapsed onto the new position: CodeParade's demo
assigns `prev_pos = pos`, which removes the streak by removing the motion, and
mapping keeps the whole tick expressed in the room the body is now in.

### The camera could stand inside a pane

A third-person arm swung into a doorway could come to rest *within* the pane's own
plane. A surface camera built from a viewpoint there has no half-space for its
oblique clip to keep and no bounded fit, and the pane fills the screen with a
vertical smear of stretched texels.

`ClearOfPanes` is the body's landing-clearance rule for a viewpoint, pushed to the
side it was nearer, using the same tie-break `SeamMapping` uses so that an eye and
a body never disagree about which room they are in.

### A copy landing on its own original

A pair of panes can be arranged so the map is near enough the identity for
whatever stands beside them. Every clone then arrives on top of the thing it was
copied from, and two coplanar surfaces at one depth is a stripe of flickering
colour along the seam.

Refused within `COINCIDENT_COPY` — **a hair, five hundredths of a stud, and not
the body's own reach.** The stronger form refused a real crossing into a real
other room whenever two rooms were laid out near each other. What has to be
caught is a map that moves *nothing*.

### The character drifted on every round trip

`LANDING_CLEARANCE` was added to every crossing unconditionally, so a body that
walked through a hole and back came out a little beside where it started, and
again on the next trip. What was wanted is that nothing *rests* within the
clearance of a plane — a minimum, not an offset. It is a floor now.

### The portal went blocky up close

Not the texture being too small. The fit covers the whole pane, and up against
the glass almost all of the pane is off screen — a pane subtends nearly half a
turn from a point on its own surface and a screen subtends seventy degrees. So
almost every texel was spent outside the frame, and the image went coarse exactly
when it was largest.

Two mitigations landed — intersecting the fit with the viewer's own mapped
frustum, and doubling the target size with screen coverage — and **both are gone
for same-world holes**, because Part III removed the fit. They are still right for
mirrors and for cross-world panes.

### `SurfaceCamera::FPS`

A surface is a whole scene render and there was no reason it should keep the
screen's rate. 120 by default, zero for uncapped, honoured beside the content
signature and the visibility gate rather than instead of them — three independent
reasons to skip, and a surface has to clear all three. **The never-drawn case
ignores the cap**, or a pane walked up to shows its own tint for up to an interval
before the picture appears.

## In-frame recursion, which was the last row

**It is a loop, not a second renderer.** The surface pass samples the *other*
surfaces, and with one pass per frame it samples the textures they held last
frame. Part I estimated it "large" on the assumption that it
meant a recursive pass with its own budget. It did not:

- bounce zero draws every surface sampling last frame's neighbours;
- the flip at the top of bounce one makes bounce zero's output the read side;
- bounce one draws every surface sampling **this frame's** neighbours.

**The ping-pong pair is what makes it safe and it was already there.** Each
bounce writes `Slot` and reads `Slot ^ 1`. **Two by default** rather than
CodeParade's four, because their portals are the whole scene and ours share a
budget with mirrors, shadows and a world. **One surface takes one bounce whatever
the setting says**, because nothing samples itself.

---

# Part III — the pivot: a portal is not a surface camera

**Parts I and II both argued the opposite, at length, and both were wrong.** The
claim was that a portal is "a `SurfaceCamera` with a different rule for where it
stands", and that anything starting with a portal pass of its own would be a
second copy of the surface pass. That reasoning held for the *first* thing a
portal has to do — show somewhere else in a rectangle — and quietly failed for
everything after it.

## The line that settles it

`Renderer.cpp`, in the surface pass, where one surface draws another:

```cpp
const FrameUniforms mirrorFrame{
    state.ViewProjection,      // the camera this pass is rendering from
    lightViewProjection,
    shown.PreviousSampling,    // the *other* surface's own matrix
};
```

When surface A's pass draws pane B, it projects B's texture with **B's own
camera** — and every surface camera is placed from the *eye*. So the image of B
inside A is B-as-seen-from-the-eye, pasted into a view rendered from A's camera.

That is not a stale image. It is the wrong viewpoint, and no number of bounces
fixes it, because what is wrong is the camera and not the texture's age. For a
mirror the error is small; for a corridor of holes it is the whole picture.

CodeParade's demo does not have this problem because it never has it to have: the
sub-camera is derived from the **current** camera —

```cpp
Camera portalCam = cam;
portalCam.ClipOblique(pos - normal*extra_clip, -normal);
portalCam.worldView *= warp->delta;
```

— so the warps compose down the recursion by construction. Ours could not
compose, because a slot holds one camera and that camera is a function of the eye.

## What else falls out of the same mistake

| Symptom | Why it existed | Under a portal pass |
|---|---|---|
| Pixelation up close | The texture is fitted to the *pane*, so at wide angles the texels go where the viewer is not looking | Gone. The sub-view uses the screen's own projection and the quad samples by screen position: one texel per pixel, always |
| `FitExtents`, `FIT_MARGIN`, `MINIMUM_DEPTH`, `FIT_MINIMUM_SPAN` | A frustum has to be fitted to a rectangle | All gone. Nothing is fitted |
| The eye-frustum clamp | Recovering texels the fit wasted | Gone with the fit |
| Adaptive target resolution | Chasing the fit's resolution | Gone; the target is the viewport |
| `EDGE_ON_MARGIN` exemptions | A reflected camera flips at its own plane | Not a portal concern at all |
| Sixteen slots, shared with mirrors | One texture per surface | A depth-indexed pool, sized to recursion depth and what is visible |
| `SurfaceLens::Mapping` + `MappingOrigin` + `MappingScale` | Carrying the warp to a *sampling* matrix | The warp lives in the sub-camera. Nothing to carry |
| The bounce loop | Approximating recursion | Real recursion, deepest first |

## What survives, which is most of the work

**Everything about traversal and simulation is untouched**: `PortalSeam`,
`SeamTransform` and its four named applications, `SeamMapping`, `CrossPortals`,
`LANDING_CLEARANCE`, `NearestCrossing`, `PortalTransit`, `ClearOfPanes`,
`OpenPortals`, `RaycastThroughPortals`, the clone and ghost passes,
`DrawInstance::Movable`, and scale-carrying holes. None of it knows what a
`SurfaceCamera` is.

`scene::SurfaceProjection`'s oblique clip survives too — applied to the screen's
projection instead of a fitted one, which is what Lengyel's method is for.

**`SurfaceCamera` keeps mirrors and keeps every line written for them.**

## The shape of the pass, as built

1. `render::PortalView` — a pane rectangle, the warp as a `SeamTransform`, an
   optional tag filter, the slot its pane's draw run carries, and the slot of the
   hole at the far end so the level it opens can skip it. Handed to
   `Renderer::Render` beside the surfaces, not as one of them.
2. A recursive `fillLevel(camera, frame, level, skip)`:
   - for each portal whose pane is visible from `camera` (`graph::VisiblePane`),
     and while `level > 0`, recurse first;
   - build `sub` by applying the warp to the camera's frame and skewing the
     **unskewed screen projection** onto the mapped pane;
   - render `plan.Reflected` from `sub` into `bank.Portals[level][slot]`, then put
     back every pane this level can see, sampling `level - 1`'s targets;
   - the pool is per level *and* per slot, because level `L` needs all of level
     `L-1` live at once.
3. Termination at depth zero: the pane draws flat, in its own material rather
   than the demo's pink.
4. `opaque.frag` reads the sub-render **by screen position**
   (`gl_FragCoord.xy / textureSize`), never tested against 0..1 — it cannot be
   outside, because the target covers the same rectangle as the frame.

## Cross-world panes stay on the surface path

`Portal::DestinationWorld` shows another *world* through a pane. A recursive pass
composes cameras through a warp, and a warp into another world's coordinate space
is a stated frame rather than a derived one.

**Settled: a cross-world pane keeps its `SurfaceCamera` and does not recurse**,
and going *through* one is a teleport into the other world rather than a step
across a seam. A world change is a load, and a load wants somewhere to put a
screen. A hole you walk through without a frame of interruption is a same-world
hole by definition.

| | Same world | Another world |
|---|---|---|
| Drawn by | The recursive portal pass | A `SurfaceCamera`, projected as today |
| Recurses | Yes | No |
| Crossing | `CrossPortals`, continuous | A teleport, with a load screen if wanted |
| Resolution | Viewport, sampled by screen position | The pane's own surface target |

---

# Part IV — the full port, v0.15

The demo was read end to end — `Portal.cpp`, `Camera.cpp`, `Physical.cpp`,
`Engine.cpp`, `Level1`–`Level6` — and the four places this engine still differed
were closed together, because the half-implementations were what kept producing
new symptoms.

## One map per pane, not one per side

`scene::SeamMapping` took the crosser's side and flipped the *source* frame with
it while leaving the destination fixed. That makes two maps which are **not each
other's inverse**, and both land a crosser on the same side of the far pane:

- a body that entered a hole from behind came out where one that entered from the
  front does, so walking back through returned it to the front;
- the camera's map and the body's map agreed with each other but not with a second
  crossing, so the view snapped to a heading nobody entered from.

It is now one rigid `destination · half-turn · source⁻¹` per pane, which carries
the pane's front hemisphere to the far pane's back one and its back to the far
pane's front. This is what `Portal::Connect` does in the demo, where `a.delta` and
`b.delta` for the same pane are the same matrix and `front`/`back` exist only to
name which hole to skip. `render::PortalView::Front`/`Back` collapsed to `Warp`.

## The near plane follows the eye into the hole

The demo's single most important line for seamlessness was missing here:

```cpp
const float n = GH_CLAMP(NearestPortalDist() * 0.5f, GH_NEAR_MIN, GH_NEAR_MAX);
```

Without it a pane is sliced open by the near plane for the last hand's width of an
approach and you see through the wall beside the doorway. This engine hid that
behind `ClearOfPanes`, which shoved the eye a third of a stud out of any pane it
came near — a visible push at the one moment the illusion is judged.

`scene::PortalNearPlane(authored, nearestSeam)` is that clamp, **derived rather
than written back** so the authored value survives and returns the moment the eye
is clear. `scene::RectangleDistance` is `Portal::DistTo`: the distance to the hole
rather than to its plane, so an eye level with a doorway but beside it spends no
precision. The renderer applies it once to a `drawCamera` used by the cull, the
recursion and the opaque draw. `ClearOfPanes` now keeps a hair — twice
`PORTAL_NEAR_MIN` — for a same-world hole, and the old margin only for a
cross-world pane.

## The oblique clip is pulled back off the pane

`extra_clip` in the demo. Clipping exactly at the mapped pane leaves the far
room's own geometry fighting the pane for depth, which is the "parts poking
through" report. `scene::PortalClipBias` takes a sliver off the far room instead,
shrinking with distance so a nose against the glass does not lose most of what the
hole shows.

**The sign is the whole of it.** `clipNormal` is the way the sub-camera looks, so
adding along it pushes the plane deeper into the far room and removes a slab of
whatever is standing in the hole — a body straddling the seam loses its far half.
CodeParade subtracts for this reason.

## The turn is the crosser's, not the map's idea of north

`CrossPortals` measured `PortalTransit::Turn` by mapping a fixed north and taking
its yaw, which is right only while the composed rotation is a pure yaw. Tilt either
pane and it is an angle nothing turned through. It now maps the body's own facing
and reports the difference, wrapped.

## A bug the port exposed

A ray leaving a hole stands at exactly zero distance from the destination's glass,
because the map takes the near pane's plane onto the far one's. `PortalHop` now
carries `Far` and `RaycastThroughPortals` ignores it, or every portal ray reports
the destination pane as the first thing beyond the hole.

## `Hallway.luau`

CodeParade's Level 1, which is the smallest scene that proves a portal is a hole.
Two corridors on an empty plain, one thirty-two studs and one four, mouths paired
across. Walk into the small one and come out thirty-two studs later; walk into the
long one and come out after four.

The authoring rule the file records: **point each pane's face at the space a
traveller comes from, and the destination's face at the space they should arrive
in.** The long corridor's mouths therefore face out and the short one's face in.

---

# Part V — the wormhole contract: what is not built

Parts I to IV built T1 (the view) and T2 (the body). This part is the design for
the rest: **an object in the seam, a contact through the seam, light through the
seam, and nothing vanishing at any of them.**

Everything below is stated as a contract — an invariant, the type that carries
it, the pass that enforces it, and the test that would catch its absence — rather
than as an intention. Where a contract is expensive, the cost is stated and the
80/20 is named.

## V.0 The four requirements, restated as invariants

| # | Requirement | Invariant |
|---|---|---|
| **R1** | An object may sit in the middle of the seam | For every instance straddling a pane, the union of what is drawn on the two sides is **exactly one copy** of the object, cut at the plane, and the body is supported by whatever floor is under **each** half |
| **R2** | Lighting works through the portal | A light `L` on the near side contributes to a far-side fragment `p` exactly when the segment `M⁻¹(p) → L` passes through the pane rectangle |
| **R3** | Completely seamless, no gap | For every eye path crossing the pane, every drawn quantity is continuous in the eye's position — no frame blanks, no pane edge, no depth hairline, no resolution step |
| **R4** | Nothing disappears | An instance that is visible from the eye, or through any chain of holes the eye can see, is drawn — with any cull or budget that drops it either impossible or logged |

R1 and R4 are near-duplicates on purpose: R1 is about the *straddler*,
R4 is about *everything else*. They fail differently.

## V.1 The seam cut — R1, R4

### What is wrong today

`CloneThroughSeams` appends a whole copy of a straddling body at `M(body)`, and
the original stays whole. So a body in a hole is drawn **twice, in full**:

- in the near room, the original — including the part of it that has gone
  *through* the pane and is geometrically standing inside the far side's wall, or,
  for a free-standing pane, hanging visibly out of the back of it;
- in the far room, the clone — including the part of it that has *not* gone
  through, poking back out of the far pane toward the far room.

When the pane is set into a thick wall, the wall hides both overhangs and the
illusion holds. That is why `Portals-1-world.luau` looks right and why
`PortalShadow.luau`, whose panes are free-standing slabs, does not. It is also
precisely the report in `ROADMAP.md`: *"the entity is big enough to fit past the
portal, but we don't project it"*.

**The overhang is not a hole in the render; it is a duplicate.** No amount of
clone tuning fixes it, because both copies are correct and each is half wrong.

### The contract

> **C1.** An instance that straddles a pane is drawn as **two halves**, each with
> a world-space clip plane. The original keeps the half-space the *viewer's* side
> of the pane occupies; the clone keeps the complementary half-space at the far
> pane. Neither half is ever drawn outside its own half-space, in any pass.

The two planes are the same plane through `M`, so there is one number to get
right per straddler:

```
original: keep  dot(p, n)  >=  dot(centre, n)          for the pane's near side
clone:    keep  dot(p, M.Rotate(n)) <= dot(M.Point(centre), M.Rotate(n))
```

The sign is the crosser's side, exactly as `SeamMapping` takes it — **the same
tie-break**, so an eye, a body and a drawn half never disagree about which room
they are in.

### Why the near half's missing part is not missing

Cut at the plane, the original loses the piece that is through the hole — and the
picture in the pane supplies it, because the portal sub-render draws the *clone*
and the clone keeps exactly that piece. The two meet at the plane with no overlap
and no gap. This is the same argument the oblique clip already makes for the
room's own geometry; C1 extends it from the room to the things in it.

### Where it goes

The awkward part is that the cut is **per instance** and every uniform in this
pipeline is either per frame or per draw. Three options were weighed:

| | Cost | Verdict |
|---|---|---|
| `gl_ClipDistance` from a per-instance vertex attribute | 16 bytes per instance on every instance in the world, plus a pipeline change | No. Pays for the world to serve a handful of rows |
| A plane on `DrawInstance`, discarded in `opaque.frag` for every draw | A branch per fragment for the whole scene | No, for the same reason |
| **A seam run: straddling halves pulled out of the main runs and submitted one at a time, with the plane in `LightingUniforms`** | One extra run in `ScenePlan`, N tiny draws where N is the number of straddling halves, one `vec4` in a uniform that is already pushed per draw | **Yes** |

`LightingUniforms` is already pushed per draw call — `DrawSlots` has to write the
submesh's base colour into it — so the plane is free to carry there. There are at
most a handful of straddlers in a frame; a draw call each is nothing beside a
whole scene pass per hole per level.

### The types

```cpp
// scene/DrawInstance.hpp

struct DrawInstance {
    ...
    // The half-space this instance keeps, as a world plane: the unit normal
    // and the offset, keeping `dot(p, SeamNormal) >= SeamOffset`.
    //
    // **A zero normal means whole**, which is every instance in an ordinary
    // scene — the run this belongs to is chosen by `OrderScene` from exactly
    // this test, so a plain world never reaches the seam pass at all.
    //
    // **Two fields rather than one, because `core` has no `Vector4`** and
    // inventing one to carry a plane would be a type the whole engine then
    // has to have an opinion about. `core::types/` is `Vector2`, `Vector3`,
    // `CFrame`, `Color3` and the ranges, deliberately.
    core::Vector3 SeamNormal{0.0f, 0.0f, 0.0f};
    float SeamOffset = 0.0f;
};
```

This widens the row past its explicit padding, which
`scene/tests/DrawInstance.cpp` asserts out loud. That assert is the design
working: the row grows once, deliberately, with a test that says so.

```cpp
// scene/DrawInstance.hpp — ScenePlan

//     [0,                 ReflectedCasters)  opaque, no mirror, casts
//     [ReflectedCasters,  Reflected)         opaque, no mirror, no shadow
//     [Reflected,         Seam)              opaque, cut at a seam   <-- new
//     [Seam,              Opaque)            mirror, grouped by surface
//     [Opaque,            Opaque + Transparent)  blended, far to near
uint32_t Seam = 0;
```

**Between `Reflected` and the mirrors, not at the end**, because a seam half is
opaque world geometry that a mirror and a portal must both see. Putting it after
the surface runs would take it out of `plan.Reflected` — the exact range the
portal pass draws — and a straddler would vanish from the picture in the hole,
which is the artefact this whole section exists to remove.

```cpp
// render — LightingUniforms
// A fourth vec4 field, or Surface.zw + one more, depending on what fits:
vec4 SeamPlane;   // xyz normal, w offset; xyz == 0 disables
```

```glsl
// opaque.frag, immediately after the cut-out discard
if (dot(lighting.SeamPlane.xyz, lighting.SeamPlane.xyz) > 0.0 &&
    dot(inWorldPosition, lighting.SeamPlane.xyz) < lighting.SeamPlane.w) {
    discard;
}
```

**A discard rather than a clip plane**, because the pipeline has no clip-distance
slot and a discard on a run of a dozen instances costs nothing measurable. It
does defeat early-Z on those draws, which is the honest cost and is why the run
must stay small — see C4 below.

### The producer

`CloneThroughSeams` already computes everything needed. It gains three lines: set
the seam plane on the clone it appends, and set it on the **original's row** — which
means it must edit the list rather than only append to it. That is a change of
shape and is worth naming:

```cpp
// scene/SurfaceCameras.hpp

// Cuts every body standing in a seam in half, and appends the far half.
//
// **Two halves rather than two bodies, which is the whole difference from
// what this used to do.** Appending a copy and leaving the original whole
// draws the object twice: the original hangs out of the back of the pane
// into the room it is walking into, and the copy hangs out of the far pane
// back into the room it came from. With a thick wall around the pane both
// overhangs are hidden and the illusion holds, which is why this survived
// three scenes; with a free-standing slab it is two crates in a doorway.
//
// The original's row is **edited in place**, so this must be called after
// the instance that carries the body is already in `out`. `CollectInstances`
// orders it that way and the test asserts the order rather than the caller
// remembering it.
//
// @param store The world.
// @param out   The draw list. Rows in it may be edited as well as appended.
// @return How many halves were appended, which is how many bodies straddled.
size_t CutAndCloneSeams(ecs::Store &store, std::vector<DrawInstance> &out);
```

`AppendPortalClones` becomes this function's name; `AppendPortalGhosts` gains the
same edit against the draw list it walks, since it already reads and appends to
one.

### The physics half is not this

A cut is presentation. The body is still one rigid body in one place; nothing
about `Bounds`, `Collider` or the solver changes here. That is V.2.

### What would catch it

- `scene/tests/SurfaceCameras.cpp` — a crate whose half-extent exceeds the pane's
  thickness, straddling: assert both rows carry a seam plane, that the two planes
  are each other's image under `M`, and that a point one stud past the pane on
  the far side fails the original's test and passes the clone's.
- `scene/tests/DrawInstance.cpp` — the row's size assert, updated deliberately
  and with the reason in the case, which is the whole point of having it.
- `examples/tests/Scene.cpp` — `PortalShadow.luau` with a free-standing pane:
  assert no instance in the near room's draw range has geometry past the pane
  plane, which is the invariant a screenshot cannot state.

## V.2 The contact transport — R1

### What is wrong today

`physics::GhostPortalBodies` is named in two comments in
`scene/SurfaceCameras.hpp` — "a body half through and therefore standing on both
floors" — and **does not exist anywhere in the repository.** The header describes
a mechanism that was never written, which is the one kind of comment `AGENTS.md`
rule 6 calls documentation rather than a constraint.

So today: a body standing in a seam is supported by the near room's floor only.
Walk into a doorway whose far room's floor is a stud lower and the body hangs in
the air until its centre crosses; walk into one a stud higher and it clips into
the far floor and pops up on crossing. Both are the "objects pop across the plane"
row the demo has and this engine claimed not to.

The demo avoids it by construction — first person, no drawn body, and every level
laid out with both floors at the same height at the doorway. That is an authoring
constraint we should not inherit, because R1 is exactly the case it forbids.

### The contract

> **C2.** While a body straddles a pane, the solver sees a **second, kinematic
> proxy** of that body at `M(body)`, and every contact the proxy resolves is
> mapped back through `M⁻¹` and applied to the original. The proxy is never
> integrated, never published, never drawn and never saved.

The proxy is the physics twin of the clone: same map, same straddle test, same
one-seam-per-body rule. Two mechanisms rather than one because a picture and a
contact share nothing but the seam — a picture is a frame and a box in a draw
list, a contact is a shape in a broadphase.

### The shape of it

```cpp
// physics/include/engine/physics/Portals.hpp        (new)

// Puts a kinematic twin of every straddling body on the far side of its seam.
//
// **A twin rather than a shape swap, because the far room is real geometry
// in the same broadphase.** The body is not moved and its own collider is
// not touched; a second entity carrying `Collider`, `Bounds` and
// `PortalProxy` is placed at `M(body)` and integrated by nothing. The solver
// then resolves it against the far room exactly as it resolves anything.
//
// **Contacts come back through `M⁻¹`, as impulses on the original.** The
// twin has no mass of its own — `PhysicsProperties` is the original's — so
// what the far floor pushes on the twin is what the near body feels, rotated
// and scaled by the seam. Applying it to the twin instead would push a body
// nothing can see.
//
// Runs in `PreSimulation`, paired with `RetirePortalProxies` in
// `PostSimulation`, so a proxy never survives the tick that made it: a body
// that stopped straddling between two ticks must not leave a collider
// standing in the far room.
//
// @param store The world.
// @return How many proxies were placed. Zero on nearly every tick.
size_t GhostPortalBodies(ecs::Store &store);
```

```cpp
// scene/Components.hpp

// Marks a collider that exists only because its owner is standing in a hole.
//
// **Not saved, not replicated, not drawn.** `replication::LocalToTheClient`
// keeps it off the wire and `Rendered` is never added, so the only thing that
// can see one is the solver.
struct PortalProxy {
    ecs::Entity Owner = ecs::NULL_ENTITY;
    SeamTransform Through;   // owner -> proxy; invert for the impulse
};
```

### The four things that make it non-trivial, and what each costs

1. **A proxy must not collide with its own owner.** They are a hundred studs
   apart in an ordinary pair and coincident in a degenerate one. The existing
   `COINCIDENT_COPY` rule is the same guard — a map that moves nothing does not
   get a proxy — and the broadphase needs one filter: *a proxy never pairs with
   its owner or with another proxy of the same owner.*
2. **A proxy must not be ground-cast by `GroundCharacters`.** That pass casts
   from the character; the proxy is not a character. Nothing to do beyond not
   giving it `Humanoid`.
3. **Double support is correct, not a bug.** A body in a doorway standing on both
   floors is the point. What must not happen is *double gravity* or double
   friction: the proxy carries no `Motion` and no mass, so it contributes
   contacts and nothing else.
4. **Scale.** A proxy through a shrinking hole is the owner's box times
   `Through.Scale`, and the impulse comes back divided by it. This is
   `SeamTransform::Carry` for the box and the impulse, and `Rotate` for the
   contact normal. **Naming the two is what stops this being a source of quiet
   bugs**, exactly as in T2.

### The binary threshold, and what it is actually for

`ROADMAP.md`'s note on this suggests *"in the seam you can do a binary threshold
for which side to handle collisions on where >0.5 = B else A"*. That rule is
right and it is answering a different question from the proxy:

- **Which room owns the body** — one answer, chosen by side, so nothing is
  simulated twice. This already exists: `SeamMapping` takes the crosser's side
  and `ClearOfPanes` uses the same tie-break so an eye and a body never disagree.
  The threshold *is* that rule, and past the middle of the seam the body has
  effectively arrived.
- **What the body is standing on** — two answers, because a doorway has a floor
  on each side of it and a body in the doorway is on both. This is the proxy, and
  a threshold cannot supply it: whichever side it picks, the other side's floor
  stops existing for that body, which is the falling-between-worlds case the note
  is trying to avoid.

So: keep the threshold as the ownership rule it already is, and add the proxy for
support. They are not alternatives.

### The 80/20, if the full version is too much

**Ground rays only.** `physics::RaycastThroughPortals` already continues a ray out
of the far side of a hole, and `GroundCharacters` already casts down. Wiring the
ground cast through the portal gives a character standing in a doorway a floor on
whichever side is under it — which is the whole of the R1 failure a player
notices — without a proxy, a broadphase filter or an impulse map. It leaves walls
and ceilings unhandled, so a body can push its shoulders through a far-side wall.

Take the 80/20 first, and only build the proxy when a scene needs a far-side wall.

### What would catch it

- `physics/tests/Portals.cpp`, new — a box dropped into a seam whose far room's floor
  is one stud higher: assert it comes to rest on the *higher* floor, and that
  removing the seam makes it fall through to the lower one.
- The same box walked out of the seam: assert no `PortalProxy` survives the tick.

## V.3 Light through the hole — R2

The largest of the four, and the one with three separable layers. Build them in
order; each is worth having alone.

### What is wrong today

Three separate facts, and they compound:

1. **The sun is a constant.** `SUN_DIRECTION`, `SUN_AMBIENT` and
   `SHADOW_RESOLUTION` are `constexpr` in `Renderer.cpp`. There is no sun
   component, so there is nowhere for a scene — or a portal — to say anything
   about it.
2. **There is one shadow map, fitted once, to the whole scene, from one
   direction.** `graph::FitDirectionalLight(sceneBounds, SUN_DIRECTION)`.
3. **Local lights are world-space and unshadowed.** Sixteen of them in a uniform
   buffer, pushed once per pass, every fragment testing every light. A lamp
   already shines through walls.

The clone that R1 puts in the far room is therefore lit by the *world's* light
`L`, while its own geometry has been turned by `R`. The near half is lit by `L`
too. So the two halves of one body are lit by two directions that differ by `R`,
and the far half's shadow points ninety degrees away from the near half's.
`examples/PortalShadow.luau` is the scene that makes this unmistakable — a
quarter-turn pair, a checkered floor in two colours, an unanchored crate in the
seam and a control crate clear of it.

The arithmetic says this is a *model* rather than a mistake: it is exactly what
"every room has its own sun, all pointing the same way in world space" looks
like. Self-consistent, and not what anybody means by a portal.

### Layer 1 — the straddler is lit as one object

> **C3a.** A seam half drawn on the far side is shaded with the light direction
> `M.Rotate(L)` rather than `L`.

Because the clone's geometry is `R · original`, lighting it with `R · L` gives
shading identical to the original's, point for point. One body, one look, across
the seam. That is R2's most visible half and it is **four lines**, because the
seam run from C1 already submits these instances one at a time with their own
`LightingUniforms` — the `Direction` field is right there.

Cost: zero new passes, zero new textures, one extra field written on a uniform
that is already being pushed. **Do this first.**

It does not fix the *shadow* the clone casts, which is still the world map's,
still from `L`. That is Layer 3.

### Layer 2 — local lamps shine through the hole

> **C3b.** A `Light` within its own `Range` of a pane's rectangle emits a mapped
> copy at `M(position)`, with range `M.Length(range)`, direction `M.Rotate(dir)`
> and the same colour. One hop, never two.

This is the layer that makes a torch carried up to a portal light the far room,
which is what "lighting works through the portal" means to somebody looking at
it.

**Why it is honest despite ignoring the aperture.** A mapped lamp lights the
whole far room rather than only the beam of the hole — but a local light in this
pipeline already ignores every wall in the world, because local lights are
unshadowed. The transported copy is therefore *exactly as wrong as the light it
copies*, and no more. When local shadowing arrives, the transported copy inherits
it for free, because it is an ordinary entry in the same buffer.

Where:

```cpp
// client/Scene.cpp — CollectLights, after the walk and before the cut to
// MAX_SCENE_LIGHTS, so a transported lamp competes for a slot on the same
// nearest-to-the-eye rule as any other.

size_t MapLightsThroughSeams(
    Store &store,
    std::vector<engine::render::SceneLight> &lights
);
```

**In the client rather than in `scene`**, because `render::SceneLight` is a
render type and `scene` may not name one — the same tier rule that keeps
`DrawInstance` free of `mat4`. The arithmetic is `GatherPortalSeams` plus
`SeamMapping`, both of which `scene` already exports and both of which are
already tested.

Three rules the implementation must state:

- **One hop.** A copy is never itself copied through a second seam. Two hops is a
  geometric series in a sixteen-entry budget.
- **Range gate.** A lamp only transports when `SeamDistance(seam, position) <
  range`; anything else spends slots on lights that reach nothing.
- **Cross-world panes never transport.** Their `Destination` is a camera stand-in
  in *this* world, so a mapped lamp would light a spot a metre behind the pane.
  The same rule the clone pass already has.

Cost: one walk over the seams per light, capped by the same sixteen. No new
uniform, no new texture, no new pass.

### Layer 3 — the sun's occlusion crosses the hole

> **C3c.** A fragment `p` on the far side is shadowed by a near-side caster `c`
> exactly when the segment from `p` toward the sun passes through the pane
> rectangle and meets `M(c)`.

This is the layer that gives *one* shadow to a body in the seam and lets a caster
beside a hole darken the floor on the other side of it. It is a shadow-pipeline
change rather than a portal-pass change, and it is the expensive one.

**The arithmetic that settles the design.** For a rigid `M` and a directional
light, the image of a shadow is a shadow cast by the *mapped* caster:

> `M(proj_d(B) onto P)` = `proj_{R·d}(M(B))` onto `M(P)`

Equivalently, and this is the form to implement: **render a second shadow map in
the far room, along the world's own `L`, containing the near room's casters
mapped through `M`.** No second light direction is needed in the far room's chart
at all — the map has already done the turning.

**The frustum is the aperture mask, and that is the whole trick.** Fit the second
map to *the far pane's rectangle extruded along `L`* — a beam, not a scene. A
fragment outside that beam falls outside the map's 0..1 lookup and
`ShadowFactor`'s existing bounds test already returns "lit". So no rectangle test
is needed in the shader; the fit does it.

```cpp
// graph/include/engine/graph/Cull.hpp

// An orthographic light matrix covering exactly the beam of one hole.
//
// **The rectangle extruded along the light, and not the scene's bounds.**
// A portal shadow map only has to answer for the fragments the hole's beam
// reaches, and fitting it any wider both wastes its resolution and stops it
// being a mask — the aperture is the frustum.
//
// @param centre The pane's middle, already mapped to the far side.
// @param first  One mapped half-axis.
// @param second The other.
// @param toward The light's direction, in world space.
// @param depth  How far into the far room the beam is traced.
glm::mat4 FitPortalLight(
    const core::Vector3 &centre,
    const core::Vector3 &first,
    const core::Vector3 &second,
    const core::Vector3 &toward,
    float depth
);
```

**Combining is a minimum, never a sum.** `shadow = min(world, beam)`. A hole
transports *occlusion*, not illumination: both rooms already have the world's
sun, so adding a second contribution would double-light every floor near a
doorway. Saying it as a minimum is what makes this coherent with one global sun,
and it is why Layer 3 needs no answer to "which room's sun is this".

**How many, and where they live.** One beam per hole is unaffordable per draw —
a fragment cannot know which hole's beam it is in without a branch per hole. The
bounded form:

- one shadow texture, the same `SHADOW_RESOLUTION`, carrying a **2×2 atlas** of
  up to four beams;
- four matrices in `FrameUniforms`, and four `vec4` sub-rectangles;
- `opaque.frag` loops four, takes **one tap each** (a hard edge is acceptable for
  a beam that is already a hard-edged aperture) and mins them into the sun term;
- `MAX_PORTAL_LIGHTS = 4`, chosen by which holes are nearest the eye, and the
  ones dropped are **logged** — a silent cap here reads as "shadows through holes
  do not work" rather than as "you have five holes on screen".

Cost, honestly: one extra shadow render per beam over the caster list mapped
through `M`, one 2048² texture, four matrices, four taps per fragment. That is
the same order as the existing shadow pass, times four, and it is why this is
Layer 3 rather than Layer 1.

**The prerequisite nobody will enjoy.** `SUN_DIRECTION` is a `constexpr` in
`Renderer.cpp`. Layer 3 needs the direction in `scene` — a `scene::Sun`
component or a field on the world's `Lighting` — because the beams are fitted
from it and the fit belongs in `graph`, which cannot reach into the renderer's
anonymous namespace. **That refactor is worth doing on its own merits and should
not be smuggled in under a portal ticket.**

### What each layer is worth

| Layer | Fixes | Cost | Order |
|---|---|---|---|
| C3a — shade the seam half by `M.Rotate(L)` | A body in a hole looks like one body | 4 lines, no new pass | **first** |
| C3b — transport local lamps | A lamp near a hole lights the far room | ~40 lines in `client`, no new pass | second |
| C3c — beam shadow maps | One shadow across the seam; a caster darkens the far floor | A render target class, a uniform, a shader loop, and a sun that lives in `scene` | last |

### What would catch it

- `client/tests/Presentation.cpp` — a lamp two studs from a pane: assert a
  transported light appears at `M(position)` with `M.Length(range)`, and that
  moving the lamp out of range removes it.
- `examples/tests/Scene.cpp` — `PortalShadow.luau` under `_G.SHADOW_VIEW =
  "seam"`: assert the two halves' shading agrees to within a tolerance at the
  plane, which is what C3a buys and what no screenshot states.

## V.4 The seamlessness audit — R3

Every remaining discontinuity, with its status. A "seam" here means anything that
changes discontinuously as the eye moves, or any place two passes disagree about
where the plane is.

| # | Seam | Status |
|---|---|---|
| 1 | The near plane slices the pane on approach | **closed** — `PortalNearPlane`, halving with `RectangleDistance` |
| 2 | A hairline of background inside the hole, parts poking through | **closed** — `PortalClipBias`, pulled *toward* the camera |
| 3 | The pane blanks within `EDGE_ON_MARGIN` | **closed** — the band is a mirror's; holes are exempt |
| 4 | The eye comes to rest inside the pane | **closed** — `ClearOfPanes`, a hair for a hole and a hand for a window |
| 5 | The picture is a frame stale on the crossing frame | **closed for same-world** — the recursive pass; a cross-world pane is still one frame behind and is the only thing that is |
| 6 | The image goes coarse against the glass | **closed** — the sub-render is the screen's own projection, sampled by `gl_FragCoord` |
| 7 | The two halves of a straddler are two whole bodies | **open — V.1** |
| 8 | The two halves are lit differently | **open — V.3, layer 1** |
| 9 | The far half stands on nothing | **open — V.2** |
| 10 | **The pane's own thickness** | **open, small.** A `Part` is a box, and the portal image is applied to the *whole instance* — including its four edge faces. A one-stud-thick pane shows a one-stud band of the far room around its rim, at the wrong parallax. The contract is that a hole's pane is drawn as its **face** and not its box, or that thickness is authored below a stated fraction of the shorter half-axis and the engine warns above it. The cheaper half — the warn — is worth doing today |
| 11 | **The recursion terminus** | **open, small.** At depth zero the pane draws its own material, which is a flat panel at the end of a corridor of holes. The demo's answer is a pink quad, deliberately wrong; a shipped world wants the far room's fog or a plain shade, behind the same flag the face markers are |
| 12 | **The far room's own geometry may reach into the near room's half-space** | **open, authoring.** The oblique clip keeps the half-space beyond the mapped pane, so anything of the *near* room that happens to lie in that half-space is drawn inside the picture. Placing regions apart is the standing answer; `TagFilter` is the mechanism when they cannot be. This is the "overlapping space" limit from Part I, unchanged |
| 13 | **Blended geometry inside a hole is sorted for the wrong eye** | **open, known.** There is one scene range and its transparent tail is sorted once, from the first surface camera when there is one. A sort per sub-camera would be a sort per hole per level |

Seams 10 and 11 are the two that are small enough to take now and are not
blocked on anything above them.

## V.5 The disappearance audit — R4

Every mechanism in the current pipeline that can drop something from the picture,
whether it is a bug or a budget. **The rule is that every one of them is either
impossible, stated, or logged** — a silent drop is what makes a scene look
broken in a way nobody can grep for.

| # | Mechanism | Drops | Verdict |
|---|---|---|---|
| 1 | `graph::CullAndBound` against the eye frustum | Nothing from a portal — the scene passes read `State->SceneInstances`, which is the **uncalled** list, and `plan.Reflected` is a range of it. Only the screen's own draw is culled | **correct, and worth stating**: the portal pass costs the whole world per hole per level, which is the price of not culling. See #9 |
| 2 | `scene::KeepLoaded` | An instance naming a mesh that has not arrived | Correct — a wrong cube reads as a broken asset. Applied to the foreign range too, so a far world does not come up as a field of cubes |
| 3 | `CloneThroughSeams`' component requirement | An entity missing any of `Motion`/`CharacterLimb`, `Transform`, `PreviousTransform`, `Bounds`, `Visual`, `SurfaceAppearance`, `Tags`, `Rendered` | **Open.** A body without a `Tags` row is silently not cloned. The set should be the minimum the clone actually reads, and anything else defaulted |
| 4 | The `break` after one seam | A body inside two panes gets one far half | Stated. A body in two holes at once is at the line where two holes meet |
| 5 | `COINCIDENT_COPY` | A copy landing within a hair of its original | Correct — that is a duplicate, not a far half |
| 6 | `SeamStraddled`'s size rule in the **ghost** pass | Anything whose reach exceeds the pane's doubled diagonal | Correct **only** because `DrawInstance::Movable` now does the real work. The size rule is a second guard on a path that already has one and should be re-read when the ghost pass next changes |
| 7 | `scene::MAX_SURFACES` = 16, shared between mirrors, cross-world windows and holes | The seventeenth pane | Logged (`ENGINE_WARN` on a duplicate or out-of-range index). A hole that loses its slot draws flat |
| 8 | `MAX_PORTAL_DEPTH` = 4, `PortalDepth` = 2 by default | The fifth level of a chain | Stated; the terminus is seam #11 |
| 9 | `graph::VisiblePane` per hole per level | A hole not in the sub-camera's frustum | Correct, and it is the demo's occlusion query on the CPU. **The one thing it cannot see is a hole visible only in a mirror inside a hole**, which resolves a frame later |
| 10 | **Particles, ribbons and billboards do not cross a seam** | Every effect, at the plane | **Open, and the most visible of these.** `CollectParticleBatches` has no seam pass, so a torch's flame vanishes as it is carried through a doorway while the torch does not. The fix is `CloneThroughSeams` for a `ParticleBatch`, which is the same map applied to a different row |
| 11 | Anchored bodies are never cloned | A crate authored `Anchored` in a seam | Deliberate — the walk is `Motion` and `CharacterLimb`, which is what keeps a floor out of the room next door. `PortalShadow.luau` documents it as the reason its crate is unanchored |

Items 3, 10 and 11 are the open ones. **10 is the one a player sees.**

## V.6 Order of work

Ranked by what a player notices per line of code, and by what unblocks what.

| | Work | Fixes | Blocked on |
|---|---|---|---|
| 1 | **C1 — the seam cut** (the seam plane, the `Seam` run, the discard, `CutAndCloneSeams`) | R1, and the duplicate every free-standing pane shows | nothing |
| 2 | **C3a — shade the far half by `M.Rotate(L)`** | R2's most visible half; a body in a hole looks like one body | C1's seam run (it is where the uniform is pushed) |
| 3 | **C2's 80/20 — the ground cast through the hole** | R1's floor; a body in a doorway stands on whichever floor is under it | nothing |
| 4 | **C3b — transported lamps** | R2; a light carried into a hole lights the far room | nothing |
| 5 | **Seam #10 — particles across the seam** | R4's most visible drop | C1 (they want the same cut) |
| 6 | **Seam #11 and #12 — the terminus and the thickness warn** | R3's two small ones | nothing |
| 7 | **C2 in full — the kinematic proxy** | R1's walls and ceilings | a broadphase filter |
| 8 | **A `scene::Sun`** | Nothing on its own; unblocks 9 | nothing, and it is worth doing anyway |
| 9 | **C3c — beam shadow maps** | R2 completely; one shadow across a seam | 8 |

**Do not start 9 before 1 through 4 are in.** Every one of them is cheaper, and
three of the four remove artefacts that would otherwise still be in the frame
when the shadows land — which is how a large change gets blamed for a small one's
bug.

---

# Appendix A — the shutdown hang is not the portal pass

The recursive pass landed with a caveat: the client hangs at shutdown, far more
often with the pass on, main thread in a futex inside `SDL_WaitForGPUIdle`. The
diagnosis offered was a write-after-read hazard at depth two, and the mitigation
was to cap the depth.

**Measured again after merging, that diagnosis does not cover what happens.**

| Scene | Portals in it | Hangs |
|---|---|---|
| `Portals-1-world` | three | 7/8 |
| `Rings` | **none** | 2/6 |
| `Rings`, offscreen capture path | none | 3/6 |

`Rings` builds no `PortalView`s, so the pass never runs, the target pool is never
allocated and the release loop walks an empty list. The portal code is inert and
it still hangs two times in six. The offscreen path hangs too, so it is not the
swapchain.

So there is a **pre-existing shutdown race in the GPU teardown**, which more work
per frame makes more likely. Depth two may well have the additional hazard
described — that measurement was 20/20 against 0/13 and is far too clean to be the
same thing — but capping the depth does not make the client reliable.

**What this means for the default.** There is no safe configuration until the
teardown race is closed, and a comment implying otherwise would send the next
person looking in the wrong file. The next step is the shutdown path rather than
the pass: whether `AbandonFrame` can leave an acquisition outstanding, and whether
anything still holds a command buffer when `Renderer::Shutdown` takes the device.

---

# Appendix B — the demo, and where to look in it

`temp/NonEuclidean/` is CodeParade's *Non-Euclidean Worlds Engine*, MIT, and it
is the model for Parts III and IV. The five files worth reading:

| File | What it settles |
|---|---|
| `Portal.cpp` / `Portal.h` | `Connect` writing one `delta` into both warps; `Draw` recursing through an FBO; `DistTo` on the rectangle; `skipPortal` |
| `Camera.cpp` | `ClipOblique`, in Lengyel's `-1..1` form — **which is not ours**, see Part I |
| `Physical.cpp` | `TryPortal`: the segment test, the double bump, `p_scale` |
| `Engine.cpp` | `GH_MAX_RECURSION`, the near-plane clamp at line 117, the occlusion queries |
| `Level1.cpp` | The two corridors that `examples/Hallway.luau` is a port of |

The scenes in this engine that exercise each part:

| Scene | Shows |
|---|---|
| `examples/Hallway.luau` | The smallest proof a portal is a hole. `_G.HALLWAY_VIEW` names a still |
| `examples/Portals-1-world.luau` | Three rooms 300 apart and six holes, two of which no adjacency explains |
| `examples/PortalShadow.luau` | The straddler: two halves, two shadows, and no shadow crossing the seam. `_G.SHADOW_VIEW` names a still. **This is the scene Part V.1 and V.3 are measured against** |
| `examples/ImmersivePortals.luau` | The cross-world window, which does not recurse |
| `examples/PortalProbe.luau` | The authoring invariant: the half-space the clip keeps contains the middle of the room the hole names |
