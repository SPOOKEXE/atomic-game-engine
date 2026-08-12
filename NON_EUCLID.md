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

## The work, in order

### 1. A visibility gate on the surface pass

**What.** A surface does not re-render when its pane is not visible from the
main camera and not visible from any other refreshing surface camera.

**Why.** It is the one thing the demo does that we have no equivalent of, and
the cost we pay for its absence grows with the number of panes — which is the
direction every one of these scenes goes. Eight mirrors in a room means eight
full scene renders a frame for as long as anything in the world is moving,
including the ones behind the viewer.

**Where.** `mono.engine/render/src/Renderer.cpp`. The cull at 3262 already
produces `State->Visible` against the main camera's frustum; the refresh
decision is at 3461. `AcceptedView` gains a `Visible` flag; `Refresh` becomes
`Visible && (!state.Ready || state.Signature != surfaceSignature)`.

**How.**
- Each pane is a `DrawInstance` whose `Surface` names its slot, so main-camera
  visibility is a sweep over `State->VisibleInstances` marking a bitmask of
  slots — no new geometry and no new bound.
- Then one pass over `accepted[]`: for each surface `i` not yet marked, test
  pane `i`'s world box against every *marked* surface's frustum
  (`graph::Frustum::FromViewProjection(accepted[j].ViewProjection)`), and mark
  it if any accepts.
- A slot that has never rendered (`!state.Ready`) refreshes regardless, so the
  first frame is unchanged.

**Test.** `mono.engine/render` — a world with two panes, one behind the camera:
`FrameResult` reports one surface pass rather than two, and the off-screen
pane's slot keeps its `ViewProjection`. Then the mirror-in-mirror case: pane B
faces away from the camera but is visible in pane A, and B still refreshes.

**Risk.** Low. The failure mode is a stale reflection, which is the same failure
the signature check already risks and the same one-frame budget.

### 2. `EDGE_ON_MARGIN` applies to mirrors, not to holes

**What.** In `AimSurfaceCameras`, move the edge-on blank inside the mirror
branch.

**Why.** The flip it prevents cancels itself for a linked portal, and the band
sits exactly where somebody walking through a hole spends the crossing. It is a
plausible part of `D00112`'s seam and it is three lines.

**Where.** `mono.engine/scene/src/SurfaceCameras.cpp:667-674`. The check
currently runs before `const Portal *portal = store.Get<Portal>(entity)` at 691,
so the resolution of `linked` moves above it.

**How.** Resolve `portal` and `linked` first; blank only when `!linked`. The
`Aim` still travels through the rest of the pass with `Renders = false` for a
mirror, exactly as now. Nothing about slot numbering changes — the comment at
863 about a blanked pane holding its place stays true and stays load-bearing.

**Test.** `mono.engine/scene/tests/SurfaceCameras.cpp` — a linked portal with
the eye on its plane still produces a lens with a finite, non-degenerate
frustum and a `Surface >= 0`; the same geometry with `Portal` removed still
blanks. And an eye walked *through* a portal in small steps produces a camera
frame that moves continuously across the crossing, which is the property the
band was standing in for.

**Risk.** Medium, and it is the one item here whose result has to be looked at
rather than asserted. The claim is that the cancellation holds; the test above
is what turns that from an argument into a fact. If a flip does survive, the
fallback is to keep a much narrower band for portals — the geometry degenerates
long before 0.3 studs.

### 3. A traversal bump

**What.** Offset the crossing test plane toward the crosser's side, and land the
crosser clear of the far plane.

**Why.** `CrossingOf` treats a body exactly on the plane as behind it, so a body
the solver parks there can change sides on alternate ticks. The demo's margin is
two lines and removes the case.

**Where.** `mono.engine/scene/src/SurfaceCameras.cpp` — `CrossingOf` for the
test, `CrossPortals` for the landing. `PortalCrossing`'s callers (the camera arm
in `Controls.cpp`, `RaycastThroughPortals` in `physics`) test but do not move,
so they take the offset and not the landing.

**How.** A named constant beside `EDGE_ON_MARGIN` — the demo uses
`2 · GH_NEAR_MIN`, which is millimetres; ours should be derived from nothing
finer than the solver's own contact slop, and stated as such. The landing is
`placement.Frame.Position` pushed along the destination's outward normal by the
same amount, after the map.

**Test.** `mono.engine/scene/tests/SurfaceCameras.cpp` — a body placed exactly
on a pane's plane crosses at most once over ten ticks of no motion, and a body
walked through at a tick step larger than the pane is thick still crosses
exactly once.

**Risk.** Low, with one thing to watch: the bump must be smaller than the
`SeamStraddled` reach used by the clone and ghost passes, or a body can land
outside the seam it just came out of and lose its far-side copy on the crossing
frame — which is the artefact those passes exist to remove.

### 4. Scale-carrying portals

**What.** A portal pair whose panes are different sizes rescales what goes
through it.

**Why.** It is what turns "a room bigger on the inside" from a rendering trick
into something the simulation agrees with, and it is the mechanism behind the
demo's best two levels. It is also the item that makes
`docs/NON-EUCLIDEAN.md:60` true.

**Where.** `SeamMapping` gains a scale alongside its `CFrame`; `CrossPortals`,
`AppendPortalClones`, `AppendPortalGhosts`, `PlaceCamera` and
`RaycastThroughPortals` all consume the pair rather than the frame alone. The
physics side needs a body scale that gravity, walk speed and the character rig
read — which is new, and is the actual size of this item.

**How — not yet decided, and this is the part to design before writing.** Two
shapes, and the choice is the work:

- **A scale on the body.** Closest to the demo: one float per physical thing,
  multiplied into every length and speed it has. Honest and invasive — every
  place that reads a distance has to know.
- **A scale on the world region.** The portal does not rescale the crosser; it
  moves them into a region whose units are different. Cleaner in principle,
  and it needs a concept this engine does not have.

**Test.** Nothing to specify until the shape is chosen. What must be true either
way: a body that goes through a pair and comes back is the size it started, to
within float error, after any number of round trips.

**Risk.** High, and it is the reason this is fourth rather than first. It should
not be started as a side effect of the other three, and it should not be started
without the design question above being answered on its own.

---

## What is deliberately not being taken

**In-frame recursion.** Still the right answer and still `D00112`, and still
gated on the render-graph work `ROADMAP.md` files behind a prototype project.
The three items above are all things that were never blocked on it, which is
most of why this document exists — the seam had become an excuse for leaving
cheaper things undone.

**Pink at the bottom of the chain.** Our terminus is last frame's texture, which
is a plausible image rather than a marker. That is better for a shipped world
and worse for a developer, and if it ever needs to be visible it belongs behind
a debug flag beside `AppendSurfaceFaceMarkers` rather than in the pass.

**Yaw-only panes.** Their `assert(euler.x == 0)` is a constraint we do not have
and do not want. `UpFor` costs one dot product and a branch, and a portal in a
floor is a thing people will build the first day they find the feature.
