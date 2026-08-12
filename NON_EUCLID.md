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

### What did not change

**In-frame recursion.** Still `D00112`, still the render-graph work behind a
prototype project. Everything above was reachable without it, which is most of
why this document exists: the seam had become an excuse for leaving cheaper
things undone.
