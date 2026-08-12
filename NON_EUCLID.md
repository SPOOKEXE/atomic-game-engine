# Non-Euclidean, measured against the demo it came from

A second reading of CodeParade's *Non-Euclidean Worlds Engine* — the sources in
`temp/NonEuclidean/` — against what this engine actually built at v0.14, and
what should change as a result.

`docs/NON-EUCLIDEAN.md` is the investigation that decided to build portals and
the record of what building them taught. **This document is the other half: the
demo re-read after the fact, once there was something to compare it with.** The
first reading could only ask "is the approach right"; this one can ask "where is
theirs better, and what is it costing us".

The answer, in one line: **the maths is the same and the schedule is not.** Both
engines map the eye by `destination · source⁻¹`, clip obliquely at the far
plane, and projective-texture the pane. CodeParade resolves the chain *inside
one frame, depth-first*; we resolve it *across frames, one bounce per frame*.
Nearly every other difference follows from that.

---

## Visually

| | NonEuclidean demo | atomic |
|---|---|---|
| Recursion | In-frame, depth-first, `GH_MAX_RECURSION = 4` — `Portal::Draw` re-enters `Engine::Render` through an FBO | Frame-lagged, one bounce per frame, ping-pong slots (`Renderer.cpp:3861`) |
| Chain terminus | A pink quad (`DrawPink`, `pink.frag`) | Last frame's texture — plausible rather than obviously wrong |
| Sub-camera projection | **The screen's own projection, unchanged**, into a 2048² FBO | Off-axis frustum fitted to the pane's four corners (`FitExtents`, `SurfaceCameras.cpp:285`) |
| Oblique clip | Lengyel, `-1..1` form (`Camera::ClipOblique`) | Lengyel, `0..1` form — `C/(C·Q)`, nothing subtracted (`ActiveCamera.cpp:87`) |
| Sampling | `gl_Position.xy / w * 0.5 + 0.5` (`portal.frag`) | The same projective idea through the surface camera's matrix, tested against 0..1 with a lit fallback outside it |
| Culling | GPU occlusion queries, per portal per level (`Engine.cpp:233`) | Content signature — skip if nothing that affects the image moved |
| Near the glass | Main camera near plane collapses: `clamp(nearestPortalDist * 0.5, 1e-3, 1e-1)` (`Engine.cpp:117`) | The pane **stops drawing** inside `EDGE_ON_MARGIN = 0.3` studs |
| A body in the seam | Nothing — objects pop across the plane | Clones and ghosts on both sides (`AppendPortalClones`, `AppendPortalGhosts`) |
| Pane orientation | **Yaw only** — `Portal::Draw` asserts `euler.x == 0 && euler.z == 0` | Arbitrary, with `UpFor` guarding a floor or ceiling pane |
| Budget | 16 portals × 3 FBOs each at 2048² | 16 surface slots per viewport, each sized to its own pane |

Three rows are worth expanding.

### Their sub-camera needs no fit at all, and that is not laziness

The portal view is rendered with the *identical* projection matrix as the
screen — only skewed. So the pane's clip-space position in the main view is by
construction the same coordinate as the sub-render's normalised position, and
the projective lookup is exact in two lines of shader: no `FIT_MARGIN`, no
`MINIMUM_DEPTH` floor, no degenerate-rectangle guard, no edge-on band.

Our whole `FitExtents` apparatus buys something they gave up: a portal covering
2% of the screen still gets the *entire* texture, where theirs burns a full
2048² render on 2% coverage. **The trade is right and it should stay** — but it
is the honest accounting for why a hundred and fifty lines of corner cases live
in `SurfaceCameras.cpp` and none live in `Portal.cpp`.

### Their occlusion culling has no counterpart here

They draw each portal as a depth- and colour-masked proxy under
`GL_SAMPLES_PASSED`, then skip the recursive render entirely when zero samples
pass. That is **visibility** culling. Our `Refresh` flag is **temporal**
culling — "did anything change". The two are orthogonal, and we only have one
of them: a scene with eight mirrors and a walking player redraws all eight every
frame, including the ones behind the camera and the ones a wall is in front of.

### Their near-glass decision is the opposite of ours, and theirs is right for holes

We blank a pane when the viewer is within 0.3 studs of its plane. That band
exists to kill the 180° flip when a viewer crosses a **mirror's** plane, where
`facing` is +1 on one side and -1 on the other and no continuous path joins
them.

**For a linked portal the flip cancels itself.** The frame the viewer's
`facing` sign changes is the same frame `CrossPortals` / `PortalCrossing`
carries the eye through the pane, so `through · eye` stays continuous across the
crossing. The demo depends on exactly that cancellation — `Portal::Draw` picks
`front` or `back` by side, and the two warps are inverses of each other — and it
is why they can shrink the near plane to a millimetre and walk face-first into
the glass.

So we are applying a mirror's fix to a hole, in the one band where somebody
walking through it is guaranteed to be. That is plausibly a visible part of
`D00112`'s seam and it is much cheaper to remove than in-frame recursion is.

---

## Physically

`Physical::TryPortal` and our `CrossingOf` (`SurfaceCameras.cpp:539`) are the
same four steps, arrived at independently: signed distance either side, reject
on matching signs, interpolate `da / (da - db)` to the plane, then test the hit
against the rectangle by projecting onto its half-axes normalised by their own
squared length. Two people solving "do not miss a fast body" wrote the same
function.

Where they differ:

- **They bump, we do not.** `TryPortal` offsets the test plane by
  `2 · GH_NEAR_MIN · p_scale` toward the side the body came from, and lands it
  at `pos - bump · 2` on the far side. That is a deliberate anti-re-cross
  margin. Our test treats `from == 0` as behind, so a body the solver parks
  exactly on the plane can in principle oscillate.
- **They carry scale, we cannot.** `p_scale *= warp->deltaInv.XAxis().Mag()`
  physically rescales the crosser, and gravity, walk speed, bob, hit-sphere
  radius and near-plane margins are all multiplied by it. That is the mechanism
  behind the tunnel that shrinks you. Our `SeamMapping` returns a `CFrame` —
  position and a quaternion, no scale — so scaled pairs render correctly, each
  pane fitted to its own rectangle, and a body crossing keeps its size.
  **`docs/NON-EUCLIDEAN.md:60` says "a destination scaled, rotated or placed
  anywhere", and "scaled" is the one word the implementation does not back.**
- **They step at 500 Hz.** `GH_DT = 0.002`, up to 30 substeps a frame. A
  segment test is trivially safe at a quarter-centimetre a tick. Ours has to be
  right at 60 Hz — it is, but it is the harder version of the same problem.
- **They have no seam problem because they have no body.** First person, no
  drawn character, and levels laid out so both rooms' floors line up at the
  doorway. `OpenPortals`, `RaycastThroughPortals` (`Query.cpp:206`) continuing a
  ground ray out of the far side, and the camera arm carried through in
  `PlaceCamera` (`Controls.cpp:188`) are all answers to a question they never
  had to ask.
- **Yaw fix-up is the same idea with different plumbing.** They write
  `euler.y = -atan2(newDir.x, -newDir.z)` straight onto the object. We record
  `PortalTransit{Serial, Turn}` and apply it in `FollowPortalTransit`, because
  the simulating host is not the looking host. Both share the limitation: only
  yaw is turned, so a floor-to-ceiling pair leaves the view wrong in either
  engine.
- **Cross-world portals are ours alone.** `Portal::DestinationWorld` — a hole
  into a different simulation — has no counterpart there.

---

## The decisions, self-played

**Q. Should we adopt their no-fit sub-camera and delete `FitExtents`?**
No. It is simpler and exact, and it costs a full-resolution render per portal
per level regardless of screen coverage. Our fit is what makes sixteen surfaces
affordable at sane texture sizes, and the corner cases it needs are now written
down and tested. Simplicity that scales with screen coverage is not simplicity
we can afford in an engine that also has to run mirrors.

**Q. Is the visibility gate a renderer change or a scene change?**
Renderer. `graph::CullAndBound` already runs against the main camera's frustum
at `Renderer.cpp:3262`, well before the refresh decision at `Renderer.cpp:3461`
and the surface pass at `Renderer.cpp:3882`. The gate is one more condition on
`Refresh`, computed from data the frame already has. Nothing in `scene` needs to
know.

**Q. What breaks if a pane is only visible inside another mirror?**
Gating on the main camera alone would freeze it. The fix is a union rather than
a special case: a surface refreshes when its pane is visible from the main
camera **or** from any other surface camera that is itself refreshing. That is
at most 16 panes against 17 frustums — 272 box tests, once a frame, on data
already in `accepted[]`. It must be a fixed-point of one pass, not an iteration:
compute main-camera visibility first, then one sweep over the surface frustums,
and accept that a pane visible only inside a *stale* mirror resolves a frame
later. That is the same one-frame budget the whole surface pass already runs on.

**Q. Should `EDGE_ON_MARGIN` be deleted or made conditional?**
Conditional. The flip it removes is real for a mirror and there is no
cancellation to rely on there — `docs/NON-EUCLIDEAN.md` and D00027 both record
what it looked like. A linked portal has the cancellation, so the band should
apply to the mirror branch of `AimSurfaceCameras` and not to the `linked` one.

**Q. If the band goes, what replaces it for a portal?**
Nothing has to. `FitExtents` already has `MINIMUM_DEPTH` and
`FIT_MINIMUM_SPAN`, which are exactly the "corner at zero depth" and "rectangle
with no area" guards the demo does not need because it never fits. The extents
grow without bound as the viewer reaches the plane, which is the correct
answer — a pane subtends half a turn from a point on its own surface — and the
oblique clip degenerates gracefully because `SurfaceProjection` already returns
the unskewed frustum when the camera is within `ON_THE_PLANE` of it.

**Q. Should the main camera's near plane collapse near a pane, as theirs does?**
Yes, and it is a smaller change than it looks: `Engine.cpp:117` is
`clamp(nearestPortalDist * 0.5, near_min, near_max)` and we have
`SeamOffset` / `GatherPortalSeams` to compute the same distance. But it belongs
*after* the `EDGE_ON_MARGIN` change, because on its own it fixes nothing — the
pane is not drawing in that band anyway — and because it touches the eye's
projection, which every pass in the frame reads.

**Q. Where does the traversal bump go — `CrossingOf` or `CrossPortals`?**
`CrossingOf` tests and `CrossPortals` moves, so the bump is two halves in two
places, exactly as the demo has it: the *test* plane is offset toward the
crosser's side, and the *landing* is pushed clear of the far plane. Putting both
in `CrossingOf` would make `PortalCrossing`'s camera-arm caller — which does not
move anything — pay for a margin it has no use for.

**Q. Can portal scale be added without a scale in `CFrame`?**
Yes, and it must be. `CFrame` is rigid on purpose and every transform in the
engine assumes it. Scale would be a second return value from `SeamMapping` — a
float — applied separately to `Bounds::HalfExtent`, `Motion::Linear`, humanoid
speeds and the character's own rig. That is a real feature with a real blast
radius, and it is the one item here that should not be started as a side effect
of the other three.

**Q. What must not happen?**
A portal renderer. `docs/NON-EUCLIDEAN.md` and `D00112` both say it and the
reasoning has not changed: anything beginning with a pass of its own would be a
copy of the surface pass under another name, and the two would drift on the
first lighting change. Every item below is a change to a pass that already
exists.

---

## The work, as built

All four landed. What follows is what each turned out to be, and — where it
differs from the plan above — why.

| | Landed as | Test |
|---|---|---|
| Visibility gate | `graph::VisibleSurfaces` | `graph/tests/Frustum.cpp`, three cases |
| Portals exempt from `EDGE_ON_MARGIN` | one `!linked &&` | `scene/tests/SurfaceCameras.cpp`, two cases |
| Traversal bump | `LANDING_CLEARANCE`, landing only | `scene/tests/SurfaceCameras.cpp`, one case |
| Scale-carrying portals | `SeamTransform`, `PortalSeam::Scale` | `scene/tests/SurfaceCameras.cpp`, three cases |

### 1. The visibility gate moved out of the renderer

Written in `Renderer::Render` first, exactly where the plan put it, and then
moved to `graph::VisibleSurfaces` — because `Renderer::Render` needs a GPU and
the decision does not. It is boxes against frusta over a draw list, which is
what `graph` already is, so the render pass is now nine lines that fill a
`SurfaceEye` array and call it. Three cases cover the pane off screen, the pane
visible only inside another surface, and the slot with no pane in the list at
all; none of them need a device.

The union sweep is the part that would have been wrong without writing it down.
Gating on the main camera alone freezes a mirror seen only inside another
mirror, which is a much louder artefact than the redraw it saves.

### 2. The band is a mirror's, and the header said so before the code did

Three lines: resolve `Portal` before the edge-on test and guard it with
`!linked`. Nothing replaced it — the three floors that keep the matrix finite at
the plane (`MINIMUM_DEPTH`, `FIT_MINIMUM_SPAN`, and `SurfaceProjection` refusing
to skew against a plane the camera is on) were all already there for the mirror
case just outside the band.

The test that matters is not the one that checks a portal renders in the band.
It is the sweep: the eye walks the last sixty centimetres at the pane in
one-centimetre steps and the camera is required to move less than five
centimetres per step. A flip would move it by twice its distance from the pane.

### 3. Only half the demo's bump was taken, and the other half would have been a bug

CodeParade bumps twice: the landing *and* the plane the crossing test is made
against. The second half is hysteresis at 500 Hz and a trap at 60. Offsetting
the test plane means a body that begins a tick inside the offset never sees a
sign change — at 2 mm and 6 mm per tick nothing can start there, but at a
quarter of a stud per tick a body can begin a tick anywhere, and the offset
becomes a band you walk through without ever crossing.

The landing clearance alone gives the same guarantee: nothing can come to rest
within a hundredth of a stud of a plane *because it crossed one*, so no jitter
smaller than that can send it back. One mechanism, in one place.

**What the test found, which is worth knowing and is not fixed.** A body whose
step ends *exactly* on the plane, arriving from the negative side of the
normal, never crosses — `from == 0` counts as behind, so both ends test the same
side. It is measure-zero in floats, it cannot arise from a crossing now that
landings are clear, and `OpenPortals` stops the solver parking anything there.
Named here rather than guarded, because the guard would be a second sign
convention.

### 4. Scale is a similarity, and the four applications had to be named

`SeamMapping` returns a `SeamTransform` — the rigid map, the source pane's
centre, and a scale — instead of a `CFrame`. The scale is the square root of the
two panes' area ratio, which is the ratio of their sides for any pair of the
same shape and the only definition that does not depend on which axis `FaceAxes`
called first.

The struct exposes four applications rather than one multiply, and that is the
part that stops this being a source of quiet bugs:

- `Point` — a position: moves and scales.
- `Carry` — a velocity, an offset, a half-extent: rotates and scales.
- `Rotate` — a unit direction, a clip normal, a ray's aim: rotates only.
- `Place` — a placement: `Point` for the position, the rotation for the rest.

Every consumer now says which it wants. Getting `Rotate` and `Carry` the wrong
way round in `Query.cpp` would make a portal-crossing ground ray report a floor
at the wrong range, which is a character falling through a floor it is standing
on — the exact class of bug the naming exists to make visible in review.

`SurfaceLens` grew two fields, because a `CFrame` cannot hold a scale and the
pane has to be read back through *exactly* the map the camera was fitted with.
`scene::SurfaceMapping` composes the three into the matrix the shader wants, in
one place, because `T(pos) · R · T(origin) · S · T(-origin)` has an order that
can be got wrong.

**What crossing resizes**, and the list is longer than the multiply suggests: a
crate is its `Bounds` and its `Collider`; a character is those on a root nobody
can see, a `Humanoid` on a third entity holding every figure that decides how it
moves, and five limbs that are their own rows with their own boxes and their own
rest offsets. Mass follows from the box through `PhysicsProperties` and is not
touched.

**Gravity is not scaled, and that is the one place this deliberately parts from
the demo.** CodeParade multiplies it by the crosser's accumulated scale so that
shrinking is imperceptible, which is right for a gag about a tunnel. Here there
is one world, `scene::Gravity` is its property, and a small thing in it should
fall the way a small thing falls — walk into the large end and the room becomes
vast and your jumps become small, which is the whole point of having gone
through.

### What did not change, at the time

**In-frame recursion** was left open here and is closed further down. Everything
above was reachable without it, which is most of why this document exists: the
seam had become an excuse for leaving cheaper things undone.

---

## Two bugs found by running it

Both were reported from a screenshot and a description rather than a test, and
both turned out to be older than any of the work above.

### A spare character standing near the hole

`client::CollectInstances` called **both** `AppendPortalClones` and
`AppendPortalGhosts` over the same world. They are the same answer reached two
ways — one walks the world for things that can move, the other walks the draw
list — so every straddling body got two far-side copies z-fighting each other.
Worse, the ghost pass reads the list it appends to, and the clones had just been
appended to it: a clone sits across the *far* pane by construction, so it was
mapped back again and a third copy landed on top of the original.

The clone pass is the one kept on that path, because it walks entities and can
therefore ask what a draw instance cannot — whether a thing is able to move at
all. `client::CollectReplicatedInstances` keeps the ghost pass and has to: a
replica has a draw list and no simulation behind it.

### The far room's floor drawn across the near room

The straddle test widens every check by the body's own reach, which is right for
a character and absurd for a slab. A floor fifty studs across has a reach of
seventy, so its centre is within reach of every plane in the building and inside
every rectangle in it — and what gets drawn is a copy of the far room's floor
laid over the near one, meeting it along a hard straight line through the middle
of the scene.

`SeamStraddled` now refuses anything whose reach exceeds the pane's shorter
half-axis. The rule is physical rather than tuned: a thing wider than the hole is
not standing in the hole, whatever its centre is doing. `AppendPortalGhosts` had
a *third* hand-rolled copy of the straddle test — the reason the rule reached
only half the passes — and now calls the public one.

### The camera flipping on some crossings and not others

`PlaceCamera` put the third-person arm through the **nearest** pane its segment
met. `CrossPortals` moved the body through the **first pane gathered**, which is
archetype order — and archetype order moves whenever anything in the world
changes a component set. On the frames the two chose differently, the body went
through one hole and the eye through another, and nothing about those frames
distinguished them from the ones that worked. It shows up in first person too,
because the yaw written into `PortalTransit` is measured off whichever map the
body used.

They are one function now — `NearestCrossing` — used by the body's crossing and
the camera's alike, so the two cannot disagree again. The regression test asserts
that the *farther* pane is gathered first, which is what makes it a guard rather
than a coincidence: a first-match rule passes the old geometry and fails this.

---

## Four more, from looking at it

### A body in a hole cast no shadow on the far side

`AppendPortalGhosts` forced `CastShadow` off, and the note explaining why said a
second shadow would follow a body that is not there. That is true of a copy
standing beside its original and false of this one: the map takes it to the far
pane, which is a different room. A character standing in a doorway has half its
shadow in each, and only the near half was ever drawn. The ghost keeps the
instance's own `CastShadow` now, so a part authored not to cast still does not.

### The character snapped for a frame or two after crossing

`CapturePreviousTransforms` records where a body was when the tick began, and
the renderer blends that against `Transform` by the tick's alpha.
`CrossPortals` runs in `PostSimulation` — *after* that capture — so a body it
teleports is interpolated **across the teleport**, and at three frames to a tick
it is drawn once or twice somewhere in the hundred units between the two panes.

`PreviousTransform` now goes through the same map as the placement and the
velocity. Mapped rather than collapsed onto the new position: CodeParade's demo
assigns `prev_pos = pos`, which removes the streak by removing the motion, and
mapping keeps the whole tick expressed in the room the body is now in.

### `SurfaceCamera::FPS`

A surface is a whole scene render and there was no reason it should keep the
screen's rate. 120 by default, zero for uncapped, honoured beside the content
signature and the visibility gate rather than instead of them — three
independent reasons to skip, and a surface has to clear all three.

Three ways to be uncapped and all of them mean "draw": a rate of zero, a
negative rate (a script computed something silly, and blacking the surface out
for that would be a bug nothing reports), and a frame clock that has not
advanced (a host that never calls `SetAnimationTime`, where capping would freeze
every surface after its first frame). The bias is culling's: when the answer is
not certain, do the work.

**The never-drawn case ignores the cap**, deliberately, or a pane walked up to
shows its own tint for up to an interval before the picture appears.

### The portal went blocky up close

Not the texture being too small. The fit covers the whole pane, and up against
the glass almost all of the pane is off screen — a pane subtends nearly half a
turn from a point on its own surface and a screen subtends seventy degrees. So
almost every texel was spent outside the frame, and the image went coarse exactly
when it was largest.

The fit is now intersected with the viewer's own frustum, carried through the
same map as everything else — exact rather than a heuristic, because the eye and
the pane were mapped together, so the mapped frustum stands in the same relation
to the mapped rectangle as the real one does to the real pane.

**Two things that had to be got right, and the suite caught both.** The clamp
must degrade *continuously*: a guard that switched it off when an eye-frustum
corner swung behind the camera moved the fit by nearly half a radian between two
frames, which the smoothness case measures and which is what a flash is. Flooring
the depth the way the pane's own corners are floored makes the clamp stop binding
smoothly instead. And the invariant the coverage case asserts had to change from
"every corner of the pane is inside the image" — which is stronger than the
shader needs and is what made this expensive — to "every part of the pane *the
viewer can see* is inside the image", which is precisely the condition
`opaque.frag` falls back on. The strict corner form is still asserted at ordinary
distances, as the guard that the clamp does not bite when it should not.

---

## Two more from the same session

### The camera could stand inside a pane

A third-person arm swung into a doorway, or a first-person eye walked up to one,
could come to rest *within* the pane's own plane. A surface camera built from a
viewpoint there has no half-space for its oblique clip to keep and no bounded
fit, and the pane fills the screen with a vertical smear of stretched texels —
which reads as a corrupt texture rather than as an eye standing somewhere it
should not.

A body already had this rule from its landing clearance. `ClearOfPanes` is the
same rule for a viewpoint: inside the band and inside the rectangle, it is pushed
to the side it was nearer, using the same tie-break `SeamMapping` uses so that an
eye and a body never disagree about which room they are in. Called after the
crossing rather than before, because it is the eye's final resting place that has
to be out of the seam.

### A copy landing on its own original

A pair of panes can be arranged so the map is near enough the identity for
whatever stands beside them — two rooms laid out adjacent, with the hole between
them agreeing with the geometry. Every clone then arrives on top of the thing it
was copied from, and two coplanar surfaces at one depth is a stripe of flickering
colour along the seam.

Both far-side passes now refuse a copy that lands within the body's own reach of
its original. A copy that overlaps its original is not a far half; it is a
duplicate, and it can only fight.

---

## In-frame recursion, which was the last row

**It is a loop, not a second renderer.** The surface pass samples the *other*
surfaces, and with one pass per frame it samples the textures they held last
frame — so a portal seen through a portal resolved one level per frame, and the
frame somebody crossed showed a seam. That was the whole of what remained in
`D00112`, and `docs/NON-EUCLIDEAN.md` estimated it "large" on the assumption that
it meant a recursive pass with its own budget.

It did not. Running the existing pass again is enough:

- bounce zero draws every surface sampling last frame's neighbours;
- the flip at the top of bounce one makes bounce zero's output the read side;
- bounce one draws every surface sampling **this frame's** neighbours.

After `n` bounces a chain `n` deep is resolved inside the frame, deepest first,
because that is what iterating to a fixed point does from the outside in.

**The ping-pong pair is what makes it safe and it was already there.** Each
bounce writes `Slot` and reads `Slot ^ 1`, and the flip between bounces swaps
which is which — so no pass ever samples a texture another pass is writing in the
same bounce, which is the exact self-reference the pair was built to prevent.
Nothing about the pass body changed; the loop wraps it.

**Two by default rather than CodeParade's four**, because their portals are the
whole scene and ours share a budget with mirrors, shadows and a world. The cost
is one scene pass per refreshing surface per bounce, which is linear and is
exactly why the visibility gate and `SurfaceCamera::FPS` were worth landing
first: a bounce only costs for panes something can actually see, and only when
they are due. `Renderer::SetSurfaceBounces` is the knob, floored at one.

**One surface takes one bounce whatever the setting says**, because nothing
samples itself — a lone mirror gains nothing from a second pass and should not
pay for one.

---

## The regression that made "render on both sides" look unfixed

The size rule added to `SeamStraddled` — *nothing bigger than the hole is
standing in the hole*, stated against the pane's shorter half-axis — was wrong,
and it was wrong in the direction that removes the feature.

**A person is very nearly as big as the doorway they walk through.** A five-stud
character has a reach of about three; a four-by-five doorway has a shorter
half-axis of two. So every character in every hole was refused: no clone in the
far room, no half a body in the picture, and the artefact the whole mechanism
exists to remove was back — while the tests, which measured a floor slab against
a sixteen-by-nine pane, stayed green.

What that rule was standing in for is *is this the room, or a thing in it*, and
the answer to that is which query found it. `CloneThroughSeams` walks bodies that
can move and character limbs, so a floor is never a candidate there and no size
rule is needed at all. `AppendPortalGhosts` reads a draw list, where an instance
is a frame and a box and nothing else — so the guard belongs there, and only
there. It is stated against the pane's **diagonal, doubled**, because what has to
be caught is a room, and a room is bigger than its own doorway by an order of
magnitude rather than by a factor of two.

The test that would have caught it is the one that now exists: a whole
`MakeCharacter` rig stood in a seam, asserting that its limbs are cloned and land
in the far room. A box was not enough, because the failing case was a body about
the size of the hole.

**And the coincident-copy rule was tightened for the same class of reason.** It
refused a copy landing within the body's own *reach* of its original, which reads
as "the copy overlaps the original" and is far too strong: two rooms laid out
near each other move a body a few studs, which is a real crossing into a real
other room. What has to be caught is a map that moves nothing, so it is a hair —
five hundredths of a stud — and nothing else.

### The other half: the hole has to show it

A copy in the draw list is drawn by the screen pass, which puts the far half in
the far room. Somebody looking *at* the pane sees the surface texture instead, so
the copy has to reach the surface pass too — or the picture in the hole shows a
room with nobody in it while the body is visibly standing in the doorway.

It does, and it is now asserted rather than assumed: `OrderScene` puts a copy in
the `Reflected` run — the opaque part of the scene range that is not itself a
mirror — which is exactly the range the surface pass submits. A copy lands there
because it carries `Surface = -1` and the original's transparency, and the
ordering has no other opinion. The surface camera's oblique clip then takes the
copy's near half, and the pane's own image covers the original's overhang, so the
two meet at the plane.

---

## Three from the next run

### The character drifted on every round trip

`LANDING_CLEARANCE` was added to every crossing unconditionally, so a body that
walked through a hole and back came out a little beside where it started, and
again on the next trip. What was wanted is that nothing *rests* within the
clearance of a plane — a minimum, not an offset. It is a floor now: a body whose
step already ended a metre past the pane gets nothing added, and only a step that
finished inside the band is nudged out to it. The tests that pinned the old
behaviour now pin the absence of the drift.

### The green patch: the ghost pass stopped guessing

The far-side pass reads a draw list, where an instance is a frame and a box and
nothing else, so it had to infer from *size* whether something straddling a pane
was a body worth copying or the room the pane is cut into. Every rule it could
infer was wrong somewhere. Against the pane's shorter half-axis it refused every
character, because a person is very nearly as big as the doorway they walk
through. Loose enough to admit one, it admitted the floor — and a floor copied
through a doorway is the far room's ground laid over the near one.

The collector knows and the pass does not, so the collector says.
`DrawInstance::Movable` took the row's last byte of explicit padding — the type
is the same width — and is set from `Motion` and `CharacterLimb`, the same pair
the clone walk uses. The size heuristics are gone entirely.

**The one byte of padding was carrying a test.** A case asserted that a
signature must *not* move with `Reserved`, because depending on padding is
depending on nothing. That byte means something now, so the case asserts the
opposite and says why.

### The pixelation: the target is sized to what the pane covers

Not the texture being too small in absolute terms, and not only the fit. A
surface camera is fitted to its pane, so its texture maps one-to-one onto the
pane's screen footprint: a pane covering half the screen wants half the screen's
pixels, and a fixed size whatever it covers spreads the same texels over a
rectangle several times larger.

`graph::VisibleSurfaces` already had every pane's world box, so it now reports
screen coverage alongside visibility, and the renderer doubles the target between
the authored size and the viewport as a pane grows. Powers of two, because a
continuous size would recreate two textures and a depth buffer every frame; a
step down costs a whole step, so standing on a threshold does not oscillate.

**The stop condition is "already at least the screen", not "the next step would
exceed it".** The second reads as safer and did nothing at all: a surface
authored at more than half the viewport could never take a step, which is every
surface in a scene that sized its panes sensibly. Overshooting by one doubling
costs texels nobody samples; stopping short costs the sharpness this is for.

---

# The pivot: a portal is not a surface camera

**This document and `docs/NON-EUCLIDEAN.md` both argued the opposite, at length,
and both were wrong.** The claim was that a portal is "a `SurfaceCamera` with a
different rule for where it stands", and that anything starting with a portal
pass of its own would be a second copy of the surface pass. That reasoning held
for the *first* thing a portal has to do — show somewhere else in a rectangle —
and quietly failed for everything after it.

## The line that settles it

`Renderer.cpp`, in the surface pass, where one surface draws another:

    const FrameUniforms mirrorFrame{
        state.ViewProjection,      // the camera this pass is rendering from
        lightViewProjection,
        shown.PreviousSampling,    // the *other* surface's own matrix
    };

When surface A's pass draws pane B, it projects B's texture with **B's own
camera** — and every surface camera is placed from the *eye*. So the image of B
inside A is B-as-seen-from-the-eye, pasted into a view rendered from A's camera.

That is not a stale image. It is the wrong viewpoint, and no number of bounces
fixes it, because what is wrong is the camera and not the texture's age. The
in-frame recursion added earlier removes the *lag* and leaves the *error*. For a
mirror the error is small and the file has always said so; for a corridor of
holes it is the whole picture.

CodeParade's demo does not have this problem because it never has it to have: the
sub-camera is derived from the **current** camera —

    Camera portalCam = cam;
    portalCam.ClipOblique(pos - normal*extra_clip, -normal);
    portalCam.worldView *= warp->delta;

— so the warps compose down the recursion by construction. Ours cannot compose,
because a slot holds one camera and that camera is a function of the eye.

## What else falls out of the same mistake

Every awkward thing fought in this document is the mirror abstraction leaking:

| Symptom | Why it exists | Under a portal pass |
|---|---|---|
| Pixelation up close | The texture is fitted to the *pane*, so at wide angles the texels go where the viewer is not looking | Gone. The sub-view uses the screen's own projection and the quad samples by screen position: one texel per pixel, always |
| `FitExtents`, `FIT_MARGIN`, `MINIMUM_DEPTH`, `FIT_MINIMUM_SPAN` | A frustum has to be fitted to a rectangle | All gone. Nothing is fitted |
| The eye-frustum clamp | Recovering texels the fit wasted | Gone with the fit |
| Adaptive target resolution | Chasing the fit's resolution | Gone; the target is the viewport |
| `EDGE_ON_MARGIN` exemptions | A reflected camera flips at its own plane | Not a portal concern at all |
| Sixteen slots, shared with mirrors | One texture per surface | A depth-indexed pool, sized to recursion depth and what is visible |
| `SurfaceLens::Mapping` + `MappingOrigin` + `MappingScale` | Carrying the warp to a *sampling* matrix | The warp lives in the sub-camera. Nothing to carry |
| The bounce loop | Approximating recursion | Real recursion, deepest first |

## What survives, which is most of the last week

**Everything about traversal and simulation is untouched**, and that is the
larger half of the work: `PortalSeam`, `SeamTransform` and its four named
applications, `SeamMapping`, `CrossPortals`, `LANDING_CLEARANCE`,
`NearestCrossing`, `PortalTransit`, `ClearOfPanes`, `OpenPortals`,
`RaycastThroughPortals`, the clone and ghost passes, `DrawInstance::Movable`, and
scale-carrying holes. None of it knows what a `SurfaceCamera` is; all of it is
about where a body, a ray or an eye goes.

`scene::SurfaceProjection`'s oblique clip survives too — it is applied to the
screen's projection instead of a fitted one, which is what Lengyel's method is
for and what the demo does.

**`SurfaceCamera` keeps mirrors and keeps every line written for them.** The fit,
the off-axis frustum, the edge-on band, the slot budget, `FPS`, the visibility
gate: all of it is correct for a reflection and stays. What it stops being is the
thing a portal is built out of.

## The shape of the pass

Modelled on `temp/NonEuclidean/NonEuclidean/Portal.cpp` and `Engine.cpp`:

1. `render::PortalView` — a pane rectangle, the warp as a `SeamTransform`, an
   optional tag filter, and which foreign instance range it draws. Handed to
   `Renderer::Render` beside the surfaces, not as one of them.
2. A recursive `DrawPortals(camera, depth)`:
   - for each portal whose pane is visible from `camera`, and while `depth > 0`
   - build `sub = camera` with the warp applied to its view and Lengyel's skew
     onto the mapped pane
   - render the scene from `sub` into a target from a depth-indexed pool, calling
     `DrawPortals(sub, depth - 1)` inside it
   - draw the pane's quad sampling that target **by screen position**
3. Termination at depth zero: draw the pane flat. The demo uses pink; a shipped
   world wants the far room's fog or a plain shade, and it belongs behind the
   same flag the face markers are.
4. Visibility per portal per level, which is the demo's occlusion query and can
   start as the CPU frustum test `graph::VisibleSurfaces` already does.

The one genuinely new piece is a shader path that samples by screen position.
`opaque.frag` projects a fragment through a matrix and tests 0..1; a portal quad
divides its own clip position and reads there. That is four lines and is the
demo's whole `portal.frag`.

## The open question, which is not mine to settle

**Cross-world portals.** `Portal::DestinationWorld` shows another *world* through
a pane, and today it works because a surface is a texture and a host can render
anything into it — `client::AttachForeignSurfaces` hands the far world's
instances over as a range. A recursive pass composes cameras through a warp, and
a warp into another world's coordinate space is a stated frame rather than a
derived one. Two ways:

- **Keep cross-world panes on the surface path.** They are a *window* onto a
  second simulation rather than a hole in one space, and they do not recurse.
  Cheapest, and the split is defensible: same-world holes recurse, cross-world
  windows do not.
- **Give the pass a world per level.** Recursion carries which store to draw, so
  a hole into another world is a hole. More honest and more work, and it needs
  the far world's draw list available at each level rather than as one appended
  range.

The first is what I would do first.
