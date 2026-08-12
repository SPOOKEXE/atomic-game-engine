#include <engine/core/Profiling.hpp>
#include <engine/core/types/Color3.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Controls.hpp>
#include <engine/scene/DrawInstance.hpp>
#include <engine/scene/Enums.hpp>
#include <engine/scene/SurfaceCameras.hpp>
#include <engine/scene/Visibility.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

namespace engine::scene {

	namespace {
		using core::CFrame;
		using core::Vector3;
		using ecs::Entity;
		using ecs::NULL_ENTITY;
		using ecs::Store;

		// The half-extent along a face's own axis.
		//
		// **Derived from `NormalOf` rather than switching on the face again.**
		// A face normal is axis-aligned and unit, so a dot with the half-extent
		// picks exactly the matching axis and `abs` drops the sign — which means
		// this cannot disagree with `NormalOf` about which axis a face lives on.
		// It was a second switch, twenty lines away and in another file from the
		// first, and a seventh face would have had to be added to both.
		//
		// **Half, not the full size**, which is the distinction `Bounds` exists
		// to keep: a plane placed at the full extent sits a whole part outside
		// the part it belongs to.
		float ReachOf(const Bounds &bounds, NormalId face) {
			return std::abs(NormalOf(face).Dot(bounds.HalfExtent));
		}

		// The cameras to place, collected before anything is written.
		//
		// **Not for safety — `Each` already makes writing inside it safe — but
		// to stay off the deferred path.** The first version of this comment said
		// a `Set` would move the row the walk was standing on. That is false
		// twice over: `Transform` is a query term so it is always present, which
		// takes `SetComponent`'s "already present, nothing moves" branch, and
		// `Each` opens a `DeferScope` anyway, so a structural change would be
		// queued rather than applied underneath the iteration.
		//
		// The real cost is that same `DeferScope`: a `Set` inside the loop is
		// queued as a command with a **heap-allocated copy of the component**,
		// once per camera per frame. Writing after the loop, where the defer
		// depth is back to zero, is a direct column assign and allocates
		// nothing. That is the whole reason for the two passes, and it is worth
		// stating correctly because the wrong reason would have justified
		// deleting them.
		struct Aim {
			Entity Camera;
			Entity Part;
			CFrame Frame;

			// The frustum fitted to the mapped pane, and the plane behind which
			// nothing draws. Replaces the near plane and field of view this
			// used to carry: neither can express a frustum that leans, and a
			// portal needs one that does.
			SurfaceLens Lens;
			// Handed out after the walk, by entity id. -1 is a camera past the
			// renderer's cap, whose pane is left as an ordinary part.
			int8_t Surface = -1;

			// Whether there is a reflection to draw at all this frame.
			//
			// False for a pane the viewer is in the plane of — see
			// `EDGE_ON_MARGIN`. Such an aim still travels through the rest of
			// this pass, because the *pane* has to be told to stop sampling; an
			// aim simply dropped on the floor would leave it showing the last
			// frame that was rendered into its slot.
			bool Renders = true;
		};

		// How close to a mirror's own plane the viewer may be before its surface
		// stops drawing.
		//
		// **A mirror's, and not a portal's.** The discontinuity below is
		// `facing` changing sign with nothing else changing, which is true of a
		// reflection and false of a hole: a portal's eye is carried through the
		// pane on the same frame, so the two flips cancel. `AimSurfaceCameras`
		// is where the exemption is applied and argued.
		//
		// **A pane seen edge-on subtends no pixels, and its reflection is
		// degenerate before it gets there.** This is the fix for D00027 and it is
		// a decision about what a surface view *means* rather than a correction
		// to the arithmetic: at the crossing there is no continuous answer to aim
		// for. `facing` is +1 on one side of the plane and -1 on the other, both
		// are right, and no path connects them — so the reflected camera turns
		// 180 degrees between two frames, which is the flash.
		//
		// **Skipping the crossing and holding the last transform was tried and
		// does not work**: it moves the flip one frame later rather than removing
		// it. What removes it is that nothing is drawn across the band, so the
		// two orientations either side are never seen in consecutive frames.
		//
		// The same 0.3 as the near-plane margin below, and deliberately the same
		// number: inside it the reflected camera is nearer its own glass than the
		// margin that exists to stop the near plane z-fighting the pane, so every
		// part of this construction is already degenerate there.
		//
		// **What it does not cover, stated rather than discovered later:** a
		// viewer crossing the plane fast enough to step over the whole band
		// between two frames still sees the flip. The band is a distance and the
		// step is a speed, so no width closes that for every speed.
		constexpr float EDGE_ON_MARGIN = 0.3f;

		std::vector<Aim> &Pending() {
			static thread_local std::vector<Aim> pending;
			return pending;
		}

		// A little wider than the pane exactly needs.
		//
		// **Because the edge of a frustum is not a safe place to sample.** The
		// projection is read back per fragment and the surface texture is
		// filtered, so a corner landing precisely on `u = 1` samples half a texel
		// of whatever `CLAMP_TO_EDGE` hands back. A couple of percent of slack
		// costs a couple of percent of texels and puts the pane's edge inside the
		// image rather than on its boundary.
		constexpr float FIT_MARGIN = 1.02f;

		// The closest a corner may be treated as being, in studs.
		//
		// **A floor rather than a rejection**, for the reason `FitExtents`
		// gives: a corner at zero depth asks for an unbounded extent, and the
		// useful answer is "very wide" arrived at *continuously*. A viewer
		// walking into the glass is asking to see a plane they are standing on,
		// which no frustum covers — the floor is what turns that into a
		// reflection that stops covering the far corners rather than a matrix
		// full of infinities.
		//
		// **Unlike the field of view it replaces, nothing is clamped on the way
		// out.** The old fit needed a ceiling just under 180° because it took a
		// tangent, and that ceiling made it a step function — which is what read
		// as the mirror flashing once per orbit. An extent has no such limit, so
		// there is nothing to saturate against.
		constexpr float MINIMUM_DEPTH = 1e-3f;

		// The narrowest frustum worth building, at the near plane.
		//
		// A pane seen exactly edge-on projects to a line, and a rectangle with
		// no area is a projection matrix full of infinities. `EDGE_ON_MARGIN`
		// catches that case first; this is the floor for everything the margin
		// is too narrow to have caught.
		constexpr float FIT_MINIMUM_SPAN = 1e-4f;

		// Half a turn, for the rotation that makes a portal a hole.
		constexpr float PI = 3.14159265358979323846f;

		// How far past the destination's plane a crosser is put down, in studs.
		//
		// **A body that lands on a plane is a body that can cross it again.** The
		// map takes the source pane onto the destination exactly, so a crosser
		// arrives at whatever depth past the far plane its own step happened to
		// end at — which can be a float's worth of nothing. One tick of jitter
		// then changes its side back and it is returned through the hole it came
		// out of, once per tick, which reads as the portal spitting people back
		// and forth rather than as a rounding error.
		//
		// **This is the hysteresis, and it is the only one.** CodeParade's demo
		// bumps twice — the landing *and* the plane the test is made against —
		// and the second half is not taken here on purpose. Offsetting the test
		// plane means a body that begins a tick inside the offset never sees a
		// sign change at all, and at 500 Hz with a 2 mm offset nothing can start
		// there; at 60 Hz, where a character covers a quarter of a stud a tick, a
		// body can begin a tick anywhere and the offset becomes a band you walk
		// through without crossing. A landing that is always clear gives the same
		// guarantee — nothing can come to rest within the bump of a plane because
		// it crossed one — with no band to fall into.
		//
		// Twenty times the solver's own penetration slop, and a small fraction of
		// any tick's travel or any body's reach, so it can neither be lost to
		// float error nor take a crosser outside the seam its clone is drawn
		// through — see `SeamStraddled`, whose reach is a whole body's radius.
		constexpr float LANDING_CLEARANCE = 0.01f;

		// How far a viewpoint is kept from a pane's plane, in studs.
		//
		// **Much wider than a body's landing clearance, and for a different
		// reason.** A body one hundredth of a stud past a plane is simply on
		// that side, and nothing about it renders badly. A *camera* that close
		// is degenerate: the oblique clip has no half-space left to keep, the
		// fitted extents run away, and the pane comes out as a vertical smear of
		// stretched texels — which reads as a corrupt texture rather than as an
		// eye standing somewhere it should not.
		//
		// The same 0.3 as `EDGE_ON_MARGIN`, and deliberately the same number:
		// that constant is the width inside which every part of a surface
		// camera's construction is already degenerate, and this is the rule that
		// stops a viewpoint being there at all. A mirror stops drawing across
		// it; a portal is walked through, so the eye is moved instead.
		constexpr float VIEWPOINT_CLEARANCE = EDGE_ON_MARGIN;

		// How close a viewpoint may come to a pane it can be carried through, in
		// studs.
		//
		// **Twice the smallest near plane, which is CodeParade's bump exactly.**
		// The recursive portal pass builds nothing that runs away as the eye
		// approaches — only an oblique clip, which needs the eye off the plane
		// and nothing more — and `PortalNearPlane` shrinks the near plane to half
		// whatever this leaves. So the rule is only "not *in* the glass", and the
		// margin is the smallest one float arithmetic can still tell apart.
		constexpr float SEAM_TOUCH = 2.0f * PORTAL_NEAR_MIN;

		// How close a far-side copy may land to its own original before it is
		// not worth drawing, in studs.
		//
		// **A copy on top of its original is a duplicate rather than a far
		// half**, and two coplanar surfaces at one depth is a stripe of
		// flickering colour. That happens when the map is the identity for the
		// thing being copied — a pane paired with itself, or a pair arranged so
		// the map fixes whatever stands beside them.
		//
		// **Tiny rather than the body's own reach, which is what this was
		// first.** Measuring against the reach reads as "the copy overlaps the
		// original" and is far too strong: two rooms laid out next to each other
		// with a hole between them move a body a few studs, which is a real
		// crossing into a real other room, and refusing that clone puts the
		// artefact back. What has to be caught is the degenerate map, and a
		// degenerate map moves nothing at all.
		constexpr float COINCIDENT_COPY = 0.05f;

		// An up vector a `LookAt` can actually use for this direction.
		//
		// **A different up for a floor or a ceiling.** `LookAt` builds its
		// rotation against an up vector and cannot when the two are parallel — a
		// mirror in the floor faces straight up, which is the one case the
		// default cannot resolve, and it produces a NaN rotation that spreads
		// into the frame, the near plane and every bound derived from them.
		//
		// Its own function because a portal needs it three times where a mirror
		// needed it once — the source face, the destination face and the camera
		// — and three copies of a guard is three places for one of them to be
		// forgotten.
		Vector3 UpFor(const Vector3 &forward) {
			return std::abs(forward.Dot(Vector3::YAxis)) > 0.99f ? Vector3::ZAxis : Vector3::YAxis;
		}

		// How much bigger one pane is than another, as a single number.
		//
		// **The square root of the area ratio.** For two rectangles of the same
		// shape that is exactly the ratio of their sides, which is the answer
		// anybody expects and the only case a sane author builds. For two of
		// different shapes there is no such number at all, and this returns the
		// one that treats the two axes alike rather than the one that depends on
		// which axis `FaceAxes` happened to call first — a choice that would
		// make the same pair scale differently on a face turned ninety degrees.
		//
		// **One for a degenerate pane rather than an infinity or a zero.** A
		// part flattened to nothing on one of a face's two axes is a pane with no
		// area, and every consumer of this multiplies a length by it — so the
		// answer that leaves the world alone is the only safe one, and it is the
		// same answer this gives for a matched pair.
		//
		// @param first  The source pane's half-axes.
		// @param second The source pane's other half-axis.
		// @param farFirst  The destination pane's, measured off the same face.
		// @param farSecond The destination pane's other one.
		// @return The scale, which is 1 whenever the two are the same size.
		float ScaleBetween(
			const Vector3 &first, const Vector3 &second, const Vector3 &farFirst, const Vector3 &farSecond
		) {
			const float here = first.Magnitude() * second.Magnitude();
			const float there = farFirst.Magnitude() * farSecond.Magnitude();
			if (!(here > 0.0f) || !(there > 0.0f)) {
				return 1.0f;
			}
			return std::sqrt(there / here);
		}

		// The two half-axes of a face, in the world.
		//
		// **The axes the normal is not on**, picked the way `MarkerExtent` picks
		// them and for the same reason: a face normal is axis-aligned and unit,
		// so the component above a half names the axis it lies on and the other
		// two are what is left. Returned already scaled by their half-extents and
		// rotated into the world, so `centre ± first ± second` is the pane's four
		// corners and nothing downstream has to know which axes they were.
		void FaceAxes(
			const CFrame &placement,
			const Vector3 &local,
			const Vector3 &half,
			Vector3 &first,
			Vector3 &second
		) {
			if (std::abs(local.X) > 0.5f) {
				first = placement.VectorToWorldSpace(Vector3{0.0f, 1.0f, 0.0f}) * half.Y;
				second = placement.VectorToWorldSpace(Vector3{0.0f, 0.0f, 1.0f}) * half.Z;
				return;
			}
			if (std::abs(local.Y) > 0.5f) {
				first = placement.VectorToWorldSpace(Vector3{1.0f, 0.0f, 0.0f}) * half.X;
				second = placement.VectorToWorldSpace(Vector3{0.0f, 0.0f, 1.0f}) * half.Z;
				return;
			}
			first = placement.VectorToWorldSpace(Vector3{1.0f, 0.0f, 0.0f}) * half.X;
			second = placement.VectorToWorldSpace(Vector3{0.0f, 1.0f, 0.0f}) * half.Y;
		}

		// Where a surface camera stands, and the rectangle it has to cover.
		//
		// **One shape for a mirror and a portal, because they differ in one
		// step.** Both take the pane, map it somewhere, put the camera in front of
		// the mapped rectangle and fit a frustum to it. A mirror's map is the
		// reflection through its own plane — which fixes that plane, so the mapped
		// rectangle is the pane itself and the arithmetic collapses back to what
		// planar reflection always was. A portal's map takes it to the far side.
		//
		// **The rectangle is the *mapped source* pane and never the destination
		// part**, and that is the one thing to get right. `opaque.frag` shades a
		// fragment of the *source* pane by projecting it through this camera's
		// matrix; that only lines up because the camera and the rectangle were
		// moved by the same map. Fitting to the destination part instead would be
		// correct exactly when the two panes are the same size and silently
		// wrong — an image sliding across the hole — whenever they are not.
		struct Placement {
			// Where the camera goes.
			Vector3 Eye;

			// Which way it looks. Unit, and **also the clip normal**: everything
			// behind the mapped pane is what has to stop drawing, so the plane to
			// keep the far side of is the one this points out of.
			Vector3 Forward;

			// The mapped pane's middle.
			Vector3 Centre;

			// Its half-axes, so `Centre ± First ± Second` is the four corners.
			//@{
			Vector3 First;
			Vector3 Second;
			//@}

			// What moved the pane here from where it stands.
			//
			// **Carried out of this struct rather than recomputed**, because the
			// pane has to be mapped by *exactly* this transform again when it
			// samples the image — `SurfaceLens::Mapping` says why, and a second
			// derivation of the same matrix is a second chance to disagree with
			// the placement about a sign.
			//
			// Identity for a mirror. See the component's comment for why that is
			// the reflection's equal on the only points that are shaded with it.
			SeamTransform Map;

			// The viewer's own frustum corners, as directions, put through the
			// same map as everything else here.
			//
			// **What stops a close pane being drawn at a hundredth of the
			// resolution it is shown at.** The fit covers the whole pane, and up
			// against the glass almost all of the pane is off screen — a pane
			// subtends nearly half a turn from a point on its own surface, and
			// the viewer sees perhaps a tenth of that. Every texel spent on the
			// other nine tenths is a texel not spent on the part in front of
			// them, so the image goes blocky exactly when it is largest.
			//
			// Intersecting the fit with these four directions keeps the texels
			// where the viewer is looking. It is exact rather than a heuristic:
			// the map takes the eye and the pane together, so the eye's frustum
			// stands in the same relation to the *mapped* pane as it does to the
			// real one, and what it cannot see through the pane is what does not
			// need drawing.
			//
			// Empty — a zero count — when there is no eye frustum to be had, in
			// which case the fit is used unclamped.
			Vector3 EyeCorners[4];
			size_t EyeCornerCount = 0;
		};

		// The frustum extents that just cover a rectangle, at the near plane.
		//
		// **The bug this exists for, because it is not obvious from a still
		// frame.** The image is projected back onto the pane per fragment and
		// `opaque.frag` tests the projected coordinate against the texture's 0..1
		// rectangle, falling back to the plain lit pane outside it. So a frustum
		// that does not cover the whole pane does not stretch or fade — it draws a
		// hard-edged rectangle of reflection floating on a grey wall.
		//
		// No constant covers it, and the reason is the whole of planar reflection:
		// the camera stands as far behind the glass as the viewer stands in front,
		// so the pane subtends *the same angle from the camera as from the
		// viewer*, and walking towards a mirror grows that without bound.
		// `Mirrors-1-world.luau` authored 70° with a comment calling it "wide
		// enough", which is the thing no constant can be: that wall needs 24° from
		// across the room and 127° from five units away.
		//
		// **Off-axis, which is what replaced the fitted field of view.** The four
		// edges are independent, so a viewer off to one side gets a frustum that
		// leans rather than one widened symmetrically about the view axis — the
		// same coverage on twice the texels. The symmetric fit had to pay for the
		// far edge of the pane on both sides.
		//
		// **And there is no clamp any more, which is a consequence rather than an
		// omission.** The old fit took a *tangent* of a half-angle, which grows
		// without bound as a corner approaches the camera's plane, so it needed a
		// floor on the depth and a ceiling just under 180° — and the ceiling made
		// the fit a step function, which read as the mirror flashing once per
		// orbit. These extents are a min and a max over four projected positions.
		// A corner approaching zero depth still sends one of them off to infinity,
		// so the depth floor stays; but nothing is being clamped back afterwards,
		// so nothing can step.
		//
		// @param placement Where the camera is and what it must cover.
		// @param up        The camera's up vector, matching the frame the caller
		//        builds — the two must agree or the extents describe a different
		//        camera from the one that renders.
		// @param near      The plane the extents are measured at.
		// @param lens      Filled with `Left`, `Right`, `Bottom` and `Top`.
		void FitExtents(const Placement &placement, const Vector3 &up, float near, SurfaceLens &lens) {
			const Vector3 right = placement.Forward.Cross(up).Unit();
			const Vector3 above = right.Cross(placement.Forward).Unit();

			bool first = true;

			for (int alongFirst = -1; alongFirst <= 1; alongFirst += 2) {
				for (int alongSecond = -1; alongSecond <= 1; alongSecond += 2) {
					const Vector3 corner = placement.Centre +
										   placement.First * static_cast<float>(alongFirst) +
										   placement.Second * static_cast<float>(alongSecond);
					const Vector3 toCorner = corner - placement.Eye;

					// **A floor rather than a rejection.** A corner level with the
					// camera, or behind it, asks for an unbounded extent; the useful
					// answer is "very wide" arrived at continuously, because a
					// discontinuity once per orbit is what a flash is.
					const float depth = std::max(toCorner.Dot(placement.Forward), MINIMUM_DEPTH);
					const float scale = near / depth;

					const float x = toCorner.Dot(right) * scale;
					const float y = toCorner.Dot(above) * scale;

					if (first) {
						lens.Left = x;
						lens.Right = x;
						lens.Bottom = y;
						lens.Top = y;
						first = false;
						continue;
					}

					lens.Left = std::min(lens.Left, x);
					lens.Right = std::max(lens.Right, x);
					lens.Bottom = std::min(lens.Bottom, y);
					lens.Top = std::max(lens.Top, y);
				}
			}

			// A little wider than the pane exactly needs, because the edge of a
			// frustum is not a safe place to sample: the projection is read back per
			// fragment and the texture is filtered, so a corner landing precisely on
			// `u = 1` samples half a texel of whatever `CLAMP_TO_EDGE` hands back.
			const float middleX = (lens.Left + lens.Right) * 0.5f;
			const float middleY = (lens.Bottom + lens.Top) * 0.5f;

			lens.Left = middleX + (lens.Left - middleX) * FIT_MARGIN;
			lens.Right = middleX + (lens.Right - middleX) * FIT_MARGIN;
			lens.Bottom = middleY + (lens.Bottom - middleY) * FIT_MARGIN;
			lens.Top = middleY + (lens.Top - middleY) * FIT_MARGIN;

			// **Then cut down to what the viewer can actually see through the
			// pane, which is the whole of the close-up sharpness.** See
			// `Placement::EyeCorners`. An intersection rather than a
			// replacement: far from the pane the fit is already much tighter
			// than the eye's frustum and nothing here changes it, which is the
			// case that must not regress.
			if (placement.EyeCornerCount == 4) {
				float eyeLeft = 0.0f;
				float eyeRight = 0.0f;
				float eyeBottom = 0.0f;
				float eyeTop = 0.0f;

				for (size_t corner = 0; corner < 4; corner++) {
					const Vector3 &direction = placement.EyeCorners[corner];

					// **The same floor the pane's corners get, and for the same
					// reason.** A guard that switched the clamp off when a corner
					// went behind the camera was tried and is what a flash is:
					// the fit jumps from "the screen" to "the whole pane" between
					// two frames, which the smoothness suite measured at nearly
					// half a radian. Flooring the depth instead sends the extent
					// off towards infinity as the corner swings past, so the
					// clamp stops binding *continuously* and there is no step to
					// see.
					const float depth = std::max(direction.Dot(placement.Forward), MINIMUM_DEPTH);
					const float scale = near / depth;

					const float x = direction.Dot(right) * scale;
					const float y = direction.Dot(above) * scale;

					if (corner == 0) {
						eyeLeft = x;
						eyeRight = x;
						eyeBottom = y;
						eyeTop = y;
						continue;
					}

					eyeLeft = std::min(eyeLeft, x);
					eyeRight = std::max(eyeRight, x);
					eyeBottom = std::min(eyeBottom, y);
					eyeTop = std::max(eyeTop, y);
				}

				// **Clamped without letting the box invert, rather than skipped
				// when the two do not overlap.** A pane entirely outside the
				// viewer's frustum collapses to a sliver here and the degenerate
				// guard below widens it to something finite — which is right,
				// because that pane is about to be skipped for being invisible
				// anyway. An `if` around the whole clamp would be one more
				// switch to step across.
				lens.Left = std::min(std::max(lens.Left, eyeLeft), lens.Right);
				lens.Right = std::max(std::min(lens.Right, eyeRight), lens.Left);
				lens.Bottom = std::min(std::max(lens.Bottom, eyeBottom), lens.Top);
				lens.Top = std::max(std::min(lens.Top, eyeTop), lens.Bottom);
			}

			// A rectangle with no area cannot be a frustum. Widening to something
			// tiny keeps the matrix finite, which is what lets the surface render
			// nothing rather than render infinities into every derived bound.
			if (!(lens.Right - lens.Left > FIT_MINIMUM_SPAN)) {
				lens.Left = middleX - FIT_MINIMUM_SPAN * 0.5f;
				lens.Right = middleX + FIT_MINIMUM_SPAN * 0.5f;
			}
			if (!(lens.Top - lens.Bottom > FIT_MINIMUM_SPAN)) {
				lens.Bottom = middleY - FIT_MINIMUM_SPAN * 0.5f;
				lens.Top = middleY + FIT_MINIMUM_SPAN * 0.5f;
			}
		}

		// How thick the face marker is, in studs, on the two axes it is not
		// spanning. Absolute rather than a fraction of the pane: a marker scaled
		// to its part is invisible on a small one and a plank on a large one,
		// and what it has to be is legible at whatever size the pane happens to
		// be.
		constexpr float MARKER_THICKNESS = 0.03f;

		// How much of the face's longer in-plane half-axis the bar covers.
		// **A third rather than all of it**, because a bar spanning the whole
		// face reads as a frame around the mirror — which is the one thing in
		// `Mirrors-1-world.luau` it must not be mistaken for.
		constexpr float MARKER_SPAN = 0.35f;

		// The marker's own half-extent, in the part's local axes.
		//
		// **Along the longer of the two axes that lie in the face.** A 16x9 pane
		// has a long axis and a short one, and a bar across the short one is a
		// dash somebody has to look for; across the long one it is a line. Which
		// two axes those are follows from the face's own — the normal is
		// axis-aligned and unit, so the component above a half picks it and the
		// other two are what is left.
		Vector3 MarkerExtent(const Vector3 &local, const Vector3 &half) {
			constexpr float THIN = MARKER_THICKNESS;

			if (std::abs(local.X) > 0.5f) {
				const bool tall = half.Y >= half.Z;
				return Vector3{THIN, tall ? half.Y * MARKER_SPAN : THIN, tall ? THIN : half.Z * MARKER_SPAN};
			}
			if (std::abs(local.Y) > 0.5f) {
				const bool wide = half.X >= half.Z;
				return Vector3{wide ? half.X * MARKER_SPAN : THIN, THIN, wide ? THIN : half.Z * MARKER_SPAN};
			}
			const bool wide = half.X >= half.Y;
			return Vector3{wide ? half.X * MARKER_SPAN : THIN, wide ? THIN : half.Y * MARKER_SPAN, THIN};
		}

		// The face a surface camera projects off, in the world.
		//
		// **One derivation, because there are two readers of it.** The aim pass
		// needs the plane to mirror the eye through and the marker pass needs
		// the same plane to lay a bar on; two walks that each worked out where
		// a face is would be `ReachOf`'s complaint one level up — a marker
		// drawn on a face the camera was not actually projecting off is a
		// debugging aid that lies, which is worse than none.
		struct Face {
			// The part the face belongs to, which is what gets told which
			// surface it shows.
			Entity Part;

			// The part's own placement, for the marker's rotation.
			CFrame Placement;

			// The face's outward normal, rotated into the world. Unit.
			Vector3 Normal;

			// The middle of the face, in the world.
			Vector3 Centre;

			// The part's half-extent, in its own axes.
			Vector3 HalfExtent;
		};

		// Finds it, or says there is not one.
		//
		// @param store  The world.
		// @param camera The surface camera.
		// @param face   Which side of the parent it projects off.
		// @param out    Filled when this returns true; untouched otherwise.
		// @return Whether the camera is parented to something with a face.
		bool FaceOf(Store &store, Entity camera, NormalId face, Face &out) {
			const Entity parent = store.ParentOf(camera);
			if (parent == NULL_ENTITY) {
				// Parented to the world rather than to a part. Left exactly
				// where it was put, which is the script-authored arrangement
				// and still a legal way to build a mirror.
				return false;
			}

			const Transform *placement = store.Get<Transform>(parent);
			const Bounds *bounds = store.Get<Bounds>(parent);
			if (placement == nullptr || bounds == nullptr) {
				// A parent that is not a part in space — a `Model`, a
				// service, a folder. There is no face to project off, so
				// this is not an error either.
				return false;
			}

			// The face, rotated into the world, so a rotated pane reflects
			// along the direction it actually faces.
			//
			// **`VectorToWorldSpace`, not a `CFrame` composition.** This
			// was `(frame * CFrame(local)).Position - frame.Position`, which
			// runs a whole quaternion multiply and a discarded normalise to
			// reach what one rotate gives — and loses precision for a pane
			// far from the origin, by adding the position and subtracting it
			// again. `RightVector`/`UpVector`/`LookVector` in the same header
			// are this call.
			//
			// The result is already unit: `NormalOf` returns unit vectors and
			// a `CFrame`'s quaternion is kept normalised on construction. The
			// `sqrt`, the zero-length guard and the reciprocal multiply that
			// used to follow could never change the answer.
			const CFrame &frame = placement->Frame;
			const Vector3 unit = frame.VectorToWorldSpace(NormalOf(face));

			out.Part = parent;
			out.Placement = frame;
			out.Normal = unit;

			// The middle of the face: the part's centre pushed out to the
			// surface along that normal.
			out.Centre = frame.Position + unit * ReachOf(*bounds, face);
			out.HalfExtent = bounds->HalfExtent;
			return true;
		}

		// Scratch for the passes that gather seams every tick, kept between them
		// so a scene with a portal in it stops allocating. Thread-local for
		// `Ordered()`'s reason.
		std::vector<PortalSeam> &Seams() {
			static thread_local std::vector<PortalSeam> seams;
			return seams;
		}

		// Every linked portal in the world, as a rectangle and a destination
		// frame. The public `GatherPortalSeams` is this into a caller's vector.
		void GatherSeams(Store &store, std::vector<PortalSeam> &seams) {
			seams.clear();

			store.Each<const Portal, const SurfaceCamera>([&](Entity entity,
															  const Portal &portal,
															  const SurfaceCamera &camera) {
				if (portal.Destination == NULL_ENTITY || !store.Alive(portal.Destination)) {
					// An unlinked portal falls back to a mirror, and a mirror
					// is a wall. Walking into one is walking into a wall.
					return;
				}

				// **The pane is the camera's parent, not the camera.** A
				// `SurfaceCamera` is a `PVInstance` — it has a placement and
				// no size at all — so a pass that read the camera's own
				// `Bounds` found none and every portal in the world quietly
				// had no hole in it. `FaceOf` is what `AimSurfaceCameras`
				// uses and is the one answer to "which rectangle is this
				// camera projecting off".
				Face face;
				if (!FaceOf(store, entity, camera.Face, face)) {
					return;
				}

				const Transform *far = store.Get<Transform>(portal.Destination);
				const Bounds *farBounds = store.Get<Bounds>(portal.Destination);
				if (far == nullptr || farBounds == nullptr) {
					return;
				}

				// The far pane's own face, chosen the way the source's was:
				// `SurfaceCamera::Face` names one side of a part, and the far
				// end of a hole is the matching side of the part it leads to.
				const Vector3 local = NormalOf(camera.Face);
				const Vector3 farNormal = far->Frame.VectorToWorldSpace(local).Unit();
				const Vector3 farCentre = far->Frame.Position + farNormal * ReachOf(*farBounds, camera.Face);

				PortalSeam seam;
				seam.Centre = face.Centre;
				seam.Normal = face.Normal;
				FaceAxes(face.Placement, local, face.HalfExtent, seam.First, seam.Second);
				seam.Destination = CFrame::LookAt(farCentre, farCentre + farNormal, UpFor(farNormal));

				// **How much bigger the far end is, from the two rectangles
				// themselves.** The destination's face is measured exactly
				// the way the source's was — same face id, same `FaceAxes` —
				// so a pair that is the same size cannot come out as
				// anything but one, and nothing has to be authored.
				Vector3 farFirst;
				Vector3 farSecond;
				FaceAxes(far->Frame, local, farBounds->HalfExtent, farFirst, farSecond);
				seam.Scale = ScaleBetween(seam.First, seam.Second, farFirst, farSecond);

				// **The pane, which is the camera's parent and not the
				// camera.** Everything that reads a seam back has to be able
				// to leave the surface itself alone, and only the walk that
				// found it knows which entity that was.
				seam.Pane = store.ParentOf(entity);
				seam.Far = portal.Destination;
				seam.Surface = camera.Surface;
				seam.TagFilter = camera.TagFilter;
				seam.Crosses = portal.DestinationWorld.IsValid();

				seams.push_back(seam);
			});
		}

		// Whether a segment goes through one hole, and what carries it there.
		//
		// @param hole    The pane to test against.
		// @param was     Where the segment starts.
		// @param now     Where it ends.
		// @param through The map from this side to the far side, written only
		//                when the answer is true.
		bool CrossingOf(
			const PortalSeam &hole,
			const Vector3 &was,
			const Vector3 &now,
			SeamTransform &through,
			float &share
		) {
			// Signed distance either side of the pane's plane. **A crossing is a
			// change of side and not a place**, which is what makes the test
			// work at speed: a character walks a quarter of a metre a tick, so
			// "is it inside the pane now" misses the tick it was on either side
			// of.
			//
			// **Either direction.** A pane is a hole rather than a one-way door,
			// and which side is "outward" is a question about the crosser —
			// `AimSurfaceCameras` answers the same question about the viewer, in
			// the same way, in the same file.
			const float from = (was - hole.Centre).Dot(hole.Normal);
			const float to = (now - hole.Centre).Dot(hole.Normal);
			if ((from > 0.0f) == (to > 0.0f)) {
				return false;
			}

			// Where the segment met the plane. The denominator cannot be zero:
			// the two signs differ, so they differ by something.
			share = from / (from - to);
			const Vector3 at = was + (now - was) * share;

			// Inside the rectangle, measured along its own half-axes.
			// **Normalised by the square of each axis' length**, which is the
			// projection without a square root — `a·b / b·b` is how far along
			// `b` the point is, in units of `b`.
			const Vector3 offset = at - hole.Centre;
			const float alongFirst = offset.Dot(hole.First) / hole.First.Dot(hole.First);
			const float alongSecond = offset.Dot(hole.Second) / hole.Second.Dot(hole.Second);
			if (std::abs(alongFirst) > 1.0f || std::abs(alongSecond) > 1.0f) {
				return false;
			}

			// The crosser's own side, which is what `facing` is in
			// `AimSurfaceCameras` — a pane walked into from behind maps through
			// the frame that faces backwards, and whatever went in comes out of
			// the destination the same way round.
			through = SeamMapping(hole);
			return true;
		}

		// Which hole a segment goes through, out of all of them.
		//
		// **The nearest, and not the first one gathered.** A segment that meets
		// two panes has to go through the one it reaches first, and the gather
		// order is archetype order — which moves the moment anything in the
		// world changes a component set. So "the first that matches" is not a
		// rule at all: it is whichever pane the store happened to list first
		// this frame, and it can differ between two frames in which nothing
		// moved.
		//
		// **One function because two disagreeing is what a flipped camera is.**
		// The body's crossing and the third-person camera arm's are the same
		// question asked about two ends of the same rig, and they used to be two
		// loops — this one picking the nearest and `CrossPortals` picking the
		// first. On any frame the two chose differently, the body went through
		// one hole and the eye through another: the view snapped somewhere the
		// character was not, on some crossings and not others, with nothing
		// about the crossing to distinguish them. Intermittent by construction,
		// because the thing that decided it was archetype order.
		//
		// @param holes The gathered seams.
		// @param from  Where the segment starts.
		// @param to    Where it ends.
		// @param hop   Filled when one is found, untouched otherwise.
		// @return The seam that was crossed, or null. Returned rather than
		//         looked up again from `PortalHop::Pane`, because a caller that
		//         needs the rectangle — a body does, to be put down clear of it
		//         — would otherwise search for the answer this already had.
		const PortalSeam *NearestCrossing(
			const std::vector<PortalSeam> &holes, const Vector3 &from, const Vector3 &to, PortalHop &hop
		) {
			const PortalSeam *found = nullptr;
			float nearest = 1.0f;

			for (const PortalSeam &seam : holes) {
				// **A cross-world pane is nobody's to move anybody through
				// here.** Its `Destination` is a stand-in that tells the camera
				// where to look, so a segment through one has no far end in this
				// store to be continued onto — and moving a body to the stand-in
				// would put them a metre behind the pane they were walking into.
				if (seam.Crosses) {
					continue;
				}

				SeamTransform candidate;
				float at = 1.0f;
				if (!CrossingOf(seam, from, to, candidate, at)) {
					continue;
				}

				if (found == nullptr || at < nearest) {
					nearest = at;
					found = &seam;
					hop = PortalHop{candidate, at, seam.Pane, seam.Far};
				}
			}

			return found;
		}
	}

	SeamTransform SeamMapping(const PortalSeam &seam) {
		const CFrame source = CFrame::LookAt(seam.Centre, seam.Centre + seam.Normal, UpFor(seam.Normal));

		// `destination · half-turn · source⁻¹`, the same product the camera goes
		// through — see `AimSurfaceCameras`' `linked` branch for why the
		// half-turn is what makes it a hole rather than a window onto a copy.
		//
		// **One map, and picking it by which side the crosser is on was a bug.**
		// The source frame used to flip with the side while the destination
		// stayed put, which makes two maps that are not each other's inverse:
		// both of them send a crosser to the *same* side of the far pane, so a
		// body that walks in the back of a hole comes out where one that walked
		// in the front does, and walking back through returns it to the front.
		// A round trip that starts from behind therefore lands somewhere else,
		// turned by whatever angle the pair turns through — which is the report
		// about a character snapping to a heading it never entered from.
		//
		// With the source fixed, the product is one rigid map that carries this
		// pane's front hemisphere to the far pane's back one and its back to the
		// far pane's front, and the far pane's own map is exactly its inverse.
		// CodeParade's `Portal::Connect` writes one `delta` per side and they
		// are the same matrix, for this reason; `Portal::Draw` re-picks `front`
		// or `back` only to know which hole to skip.
		//
		// **The scale is about the source pane's centre**, which this rigid
		// product already sends to the destination's centre — so scaling before
		// the map and scaling after it are the same operation, and only the
		// centre this pass already has is needed to state it.
		SeamTransform through;
		through.Frame = seam.Destination * CFrame::Angles(0.0f, PI, 0.0f) * source.Inverse();
		through.Origin = seam.Centre;
		through.Scale = seam.Scale;
		return through;
	}

	float SeamOffset(const PortalSeam &seam, const Vector3 &at) {
		return (at - seam.Centre).Dot(seam.Normal);
	}

	float
	RectangleDistance(const Vector3 &centre, const Vector3 &first, const Vector3 &second, const Vector3 &at) {
		const Vector3 offset = at - centre;

		// How far along each half-axis, in units of that axis, clamped to the
		// edge. **`a·b / b·b` rather than a normalise**, which is the projection
		// without a square root and is what makes the clamp read as `-1..1`.
		const float firstSquared = first.Dot(first);
		const float secondSquared = second.Dot(second);
		const float alongFirst =
			firstSquared > 0.0f ? std::clamp(offset.Dot(first) / firstSquared, -1.0f, 1.0f) : 0.0f;
		const float alongSecond =
			secondSquared > 0.0f ? std::clamp(offset.Dot(second) / secondSquared, -1.0f, 1.0f) : 0.0f;

		const Vector3 closest = first * alongFirst + second * alongSecond;
		return (offset - closest).Magnitude();
	}

	float SeamDistance(const PortalSeam &seam, const Vector3 &at) {
		return RectangleDistance(seam.Centre, seam.First, seam.Second, at);
	}

	float NearestSeamDistance(ecs::Store &store, const Vector3 &at) {
		std::vector<PortalSeam> &seams = Seams();
		GatherSeams(store, seams);

		float nearest = std::numeric_limits<float>::infinity();
		for (const PortalSeam &seam : seams) {
			nearest = std::min(nearest, SeamDistance(seam, at));
		}
		return nearest;
	}

	float PortalNearPlane(float authored, float nearestSeam) {
		if (!(authored > 0.0f)) {
			// A camera with no usable near plane is not one this can rescue, and
			// substituting one would hide the authoring mistake behind a portal.
			return authored;
		}
		if (!(nearestSeam < std::numeric_limits<float>::infinity())) {
			// No holes in this world, which is nearly every world.
			return authored;
		}

		return std::clamp(nearestSeam * 0.5f, PORTAL_NEAR_MIN, authored);
	}

	float PortalClipBias(float nearestSeam) {
		// The same halving as the near plane, and capped by the same margin that
		// says how wide a band around a pane is degenerate. Beyond that the
		// camera is far enough away that a fixed slab is invisible.
		if (!(nearestSeam < std::numeric_limits<float>::infinity())) {
			return 0.0f;
		}
		return std::min(nearestSeam * 0.5f, EDGE_ON_MARGIN);
	}

	bool SeamStraddled(const PortalSeam &seam, const Vector3 &at, float reach) {
		const float firstLength = std::sqrt(seam.First.Dot(seam.First));
		const float secondLength = std::sqrt(seam.Second.Dot(seam.Second));
		if (firstLength <= 0.0f || secondLength <= 0.0f) {
			return false;
		}

		// **No size rule here, and the one that was here was a bug.** It refused
		// anything whose reach exceeded the pane's shorter half-axis, which
		// sounds like "bigger than the hole" and is not: a person is very nearly
		// as big as the doorway they walk through. A five-stud character has a
		// reach of about three and a four-by-five doorway a shorter half-axis of
		// two, so every character in every hole was refused — no clone in the
		// far room, no half-body in the picture, which is exactly the artefact
		// the pass exists to remove.
		//
		// What that rule was standing in for is "is this the room rather than a
		// thing in it", and the answer to that is which query found it:
		// `CloneThroughSeams` walks bodies that can move and character limbs, so
		// a floor is never a candidate. `AppendPortalGhosts` reads a draw list
		// and cannot ask, so it keeps a size guard of its own — a much looser
		// one, stated against the pane's diagonal.
		if (std::abs(SeamOffset(seam, at)) >= reach) {
			return false;
		}

		// Inside the rectangle, measured along its own half-axes and widened by
		// the body's own reach — the same projection `CrossingOf` makes, plus
		// the slack a body has that a point does not. **Widened rather than
		// exact**, because a clone that appears a moment early is invisible
		// (it is behind the pane) and one that appears a moment late is a body
		// visibly cut in half, which is the artefact this exists to remove.
		const Vector3 offset = at - seam.Centre;

		const float alongFirst = std::abs(offset.Dot(seam.First) / firstLength);
		const float alongSecond = std::abs(offset.Dot(seam.Second) / secondLength);

		return alongFirst < firstLength + reach && alongSecond < secondLength + reach;
	}

	size_t GatherPortalSeams(Store &store, std::vector<PortalSeam> &seams) {
		GatherSeams(store, seams);
		return seams.size();
	}

	size_t AimSurfaceCameras(Store &store) {
		ENGINE_PROFILE("aim surface cameras");

		std::vector<Aim> &pending = Pending();
		pending.clear();

		// Where the scene is watched from. Without one there is nothing to
		// reflect: a mirror shows the viewer's world, so a world with no active
		// camera has no reflection to compute rather than a default one.
		const ActiveCamera *active = store.Resource<ActiveCamera>();
		if (active == nullptr || !store.Alive(active->Entity)) {
			return 0;
		}

		const Transform *eyeTransform = store.Get<Transform>(active->Entity);
		if (eyeTransform == nullptr) {
			return 0;
		}
		const Vector3 eye = eyeTransform->Frame.Position;

		// **The viewer's own frustum, as four directions.** What a surface has
		// to cover is not the pane, it is the part of the pane the viewer can
		// see — and up against the glass those are wildly different: a pane
		// subtends nearly half a turn from a point on its own surface and a
		// screen subtends seventy degrees. Fitting the whole pane there spends
		// almost every texel outside the frame, which is exactly why a portal
		// went blocky as you walked into it. `FitExtents` intersects with these.
		//
		// **Taken once, outside the walk**, because they are the eye's and not
		// each pane's, and rebuilding them per surface would be four rotations
		// per mirror per frame for one answer.
		//
		// A world whose active camera carries no `Camera` — which a bare
		// `Transform` used as an eye is — leaves the count at zero and every fit
		// unclamped, which is what this did before.
		Vector3 eyeCorners[4];
		size_t eyeCornerCount = 0;

		if (const Camera *eyeLens = store.Get<Camera>(active->Entity)) {
			const float aspect = active->AspectRatio > 0.0f ? active->AspectRatio : 1.0f;
			const float up = std::tan(eyeLens->FieldOfViewRadians * 0.5f);
			const float across = up * aspect;

			// **A little wider than the screen exactly needs**, for
			// `FIT_MARGIN`'s reason one level up: the edge of a frustum is not a
			// safe place to sample, and a clamp landing exactly on the screen
			// edge puts the pane's visible boundary on the texture's boundary.
			const float slack = FIT_MARGIN;

			int corner = 0;
			for (int x = -1; x <= 1; x += 2) {
				for (int y = -1; y <= 1; y += 2) {
					const Vector3 local{
						static_cast<float>(x) * across * slack, static_cast<float>(y) * up * slack, -1.0f
					};
					eyeCorners[corner++] = eyeTransform->Frame.VectorToWorldSpace(local);
				}
			}
			eyeCornerCount = 4;
		}

		store.Each<const SurfaceCamera, const Camera, const Transform>(
			[&](Entity entity, const SurfaceCamera &target, const Camera &lens, const Transform &) {
				Face face;
				if (!FaceOf(store, entity, target.Face, face)) {
					return;
				}

				const Vector3 unit = face.Normal;
				const Vector3 centre = face.Centre;

				// Which side of the face the viewer is on, and how far off it. Both
				// branches below need it: a mirror reflects across it, and a portal
				// uses its sign to decide which way out of the far side to look.
				const float distance = (eye - centre).Dot(unit);

				// **The one branch that makes a portal a portal.** Everything after
				// it is shared, because both cases have produced the same five
				// facts: where the camera stands, which way it looks, and the
				// rectangle it has to cover.
				//
				// **Resolved before the edge-on band because the band is not the
				// same question for the two.** See below.
				const Portal *portal = store.Get<Portal>(entity);
				const bool linked = portal != nullptr && portal->Destination != NULL_ENTITY &&
									store.Alive(portal->Destination) &&
									store.Get<Transform>(portal->Destination) != nullptr &&
									store.Get<Bounds>(portal->Destination) != nullptr;

				// **Edge-on renders nothing, and only a mirror is edge-on.** See
				// `EDGE_ON_MARGIN`: for a *mirror* this is the one band where
				// there is no continuous orientation to aim for, and a pane the
				// viewer is level with covers no pixels anyway. Carried through
				// as a non-rendering aim rather than returned from, so the pane
				// is told to stop sampling its slot.
				//
				// **A linked portal is exempt, and that is the whole of it.** The
				// discontinuity the band exists for is `facing` changing sign
				// with nothing else changing — true of a mirror, whose map is
				// the reflection through its own plane, and false of a hole. A
				// portal's camera is `through · eye`, and the frame `facing`
				// flips is the same frame `CrossPortals` — or `PortalCrossing`,
				// for the arm a third-person eye rides on — carries the eye
				// through the pane. The two flips cancel and the placement is
				// continuous across the crossing.
				//
				// Which matters because the band sits exactly where somebody
				// walking through a hole spends the crossing: 0.3 studs either
				// side of the plane, blanked, on the one frame they are in the
				// doorway. That is a picture going dark in the middle of the one
				// move a portal exists for, and it was a mirror's fix wearing a
				// hole's clothes.
				//
				// Nothing replaces it here. `FitExtents` already floors the
				// corner depth and the frustum span, and `SurfaceProjection`
				// already declines to skew when the camera is on its own clip
				// plane — the three guards a fit needs at the plane are the ones
				// that were already written.
				if (!linked && std::abs(distance) < EDGE_ON_MARGIN) {
					Aim blank;
					blank.Camera = entity;
					blank.Part = face.Part;
					blank.Renders = false;
					pending.push_back(blank);
					return;
				}

				// Which way along the normal the viewer is, because a face can be
				// looked at from behind — the sign of `distance` is exactly that
				// question, already answered.
				const float facing = distance >= 0.0f ? 1.0f : -1.0f;

				Vector3 first;
				Vector3 second;
				FaceAxes(face.Placement, NormalOf(target.Face), face.HalfExtent, first, second);

				Placement placement;

				if (linked) {
					// The two face frames, each looking *out* of its own pane. A
					// `LookAt` builds its rotation against an up vector and cannot
					// when the two are parallel, which is a portal in the floor —
					// the same case the mirror branch guards below, for the same
					// reason and with the same answer.
					const Vector3 outward = unit * facing;
					const CFrame source = CFrame::LookAt(centre, centre + outward, UpFor(outward));

					const Transform *farPlacement = store.Get<Transform>(portal->Destination);
					const Bounds *farBounds = store.Get<Bounds>(portal->Destination);

					// The destination's own face, chosen the same way the source's
					// was: `SurfaceCamera::Face` names one side of a part, and the
					// far end of a hole is the matching side of the part it leads
					// to.
					const Vector3 farNormal = farPlacement->Frame.VectorToWorldSpace(NormalOf(target.Face));
					const Vector3 farCentre =
						farPlacement->Frame.Position + farNormal * ReachOf(*farBounds, target.Face);
					const CFrame destination =
						CFrame::LookAt(farCentre, farCentre + farNormal, UpFor(farNormal));

					// Its rectangle, measured exactly as the source's was, which
					// is the only reason a matched pair scales by one.
					Vector3 farFirst;
					Vector3 farSecond;
					FaceAxes(
						farPlacement->Frame, NormalOf(target.Face), farBounds->HalfExtent, farFirst, farSecond
					);

					// **`destination · half-turn · source⁻¹`, and the half-turn is
					// what makes it a hole rather than a window onto a copy.**
					// Without it the camera arrives at the far pane facing back the
					// way it came, so the portal shows the room the viewer is
					// already standing in.
					//
					// **Nothing here constrains the two frames to describe one
					// space, and that is the entire non-Euclidean feature.** A
					// destination turned, moved, resized or placed anywhere gives a
					// room bigger on the inside or a corridor that turns through
					// more than four right angles — with no second mechanism and no
					// maths past this multiply. `docs/NON-EUCLIDEAN.md` is the
					// investigation that settled it.
					//
					// **Built the same way a crossing body's is**, rather than
					// composed here: `SeamMapping` is the one statement of what a
					// hole does to what goes through it, and a second derivation of
					// it in the pass that draws the picture is a second chance for
					// the picture and the simulation to disagree about a sign or a
					// size. The scale is why that stopped being cosmetic — a hole
					// between panes of different sizes moves the camera *and*
					// resizes the rectangle it is fitted to.
					SeamTransform through;
					through.Frame = destination * CFrame::Angles(0.0f, PI, 0.0f) * source.Inverse();
					through.Origin = centre;
					through.Scale = ScaleBetween(first, second, farFirst, farSecond);

					placement.Eye = through.Point(eye);
					placement.Centre = through.Point(centre);

					// **`Carry`, so the fitted rectangle is the destination pane's
					// size and not the source's.** This is where the scale earns
					// its place: the fit and the oblique clip are made against
					// these vectors, and `opaque.frag` projects the source pane
					// through the same map — so the three agree by construction
					// however different the two panes are.
					placement.First = through.Carry(first);
					placement.Second = through.Carry(second);

					// **Negated, and this is the one sign in the file worth
					// deriving rather than trying.** `through` is a similarity
					// with a positive scale, so it preserves which side of the
					// pane a point is on: the eye stands at `+outward` from the
					// source, so the camera lands at `+outward` from the *mapped*
					// pane, further out by the scale. A mirror's camera is
					// on the far side and looks along the outward normal; a
					// portal's is on the near side and has to look back through
					// the rectangle, which is the other way.
					//
					// Getting it wrong points the camera away from the hole, so
					// the portal shows whatever happens to be behind the
					// destination — which looks like a portal that works and
					// leads somewhere wrong.
					// **`Rotate` and not `Carry`**, because this is a direction
					// and has to stay unit: it is handed straight to `LookAt` and
					// used as the oblique clip's normal, and a clip plane whose
					// normal is twice as long is a plane twice as far away.
					placement.Forward = through.Rotate(outward) * -1.0f;

					// And the pane is mapped by the same matrix when it reads
					// the image back, which is what makes the two line up.
					placement.Map = through;

					// **The viewer's frustum goes through the same map**, which
					// is what makes clamping the fit against it exact rather
					// than a guess: the eye and the pane were moved together, so
					// the mapped frustum stands in the same relation to the
					// mapped rectangle as the real one does to the real pane.
					// `Rotate` and not `Carry`, because these are directions.
					for (size_t corner = 0; corner < eyeCornerCount; corner++) {
						placement.EyeCorners[corner] = through.Rotate(eyeCorners[corner]);
					}
					placement.EyeCornerCount = eyeCornerCount;
				} else {
					// **Mirrored through the plane.** The same distance behind the
					// face as the eye is in front, on the other side — which is the
					// whole of planar reflection and is why the image lines up with
					// the pane instead of sliding across it as the viewer moves.
					//
					// **The mapped rectangle is the pane itself**, because a
					// reflection fixes every point of the plane it reflects
					// through. That is why this branch looks like it is not mapping
					// anything: it is, and the map happens to be the identity on
					// exactly the four corners that matter.
					placement.Eye = eye - unit * (2.0f * distance);
					placement.Centre = centre;
					placement.Forward = unit * facing;
					placement.First = first;
					placement.Second = second;

					// **The same map applied to the viewer's frustum, and for a
					// mirror the map is the reflection rather than the
					// identity.** `Map` is left identity here on purpose — it is
					// the reflection's equal on the pane's own plane, which is
					// where the pane is shaded — but a *direction* is not on that
					// plane, so it has to be reflected properly or the clamp
					// would cut the wrong side of the image.
					for (size_t corner = 0; corner < eyeCornerCount; corner++) {
						const Vector3 &direction = eyeCorners[corner];
						placement.EyeCorners[corner] = direction - unit * (2.0f * direction.Dot(unit));
					}
					placement.EyeCornerCount = eyeCornerCount;
				}

				// **Aimed, not merely placed.** An identity rotation looks down
				// -Z, so a camera put behind a pane faces away from it and renders
				// empty space. That was the first version of the script this
				// replaces, and the mirror came out showing the clear colour.
				//
				// **Square on to the rectangle, and not at its centre.** Aiming at
				// the middle sounds like the same thing and is not: it tilts the
				// view axis off the normal by however far the viewer stands to one
				// side, and the pane then lies at an angle across the frustum. Push
				// that far enough — close to the glass and off to the side, which
				// is a metre from a wall in a room — and the nearest corner goes
				// *behind* the camera, which nothing covers.
				//
				// Looking along the normal puts every corner at the same depth, so
				// the fit is always finite and the corners are always in front. It
				// costs nothing in correctness: the image is read back by projecting
				// each fragment through this camera's own matrix, so the orientation
				// decides which texels the pane lands on and never which part of the
				// world it shows. **Leaning is now the frustum's job**, which is
				// what an off-axis fit is for and what a symmetric one could not do.
				const Vector3 up = UpFor(placement.Forward);

				Aim aim;
				aim.Camera = entity;
				aim.Part = face.Part;
				aim.Frame = CFrame::LookAt(placement.Eye, placement.Eye + placement.Forward, up);

				// **The near and far planes are the author's again.** Pushing the
				// near plane out to the glass was the poor man's oblique clip, and
				// there is a real one below — so the engine has stopped overwriting
				// a number it no longer needs to borrow.
				aim.Lens.NearPlane = lens.NearPlane;
				aim.Lens.FarPlane = lens.FarPlane;

				FitExtents(placement, up, aim.Lens.NearPlane, aim.Lens);

				// **The real oblique clip, and on a portal it is not optional.**
				// The destination is set into a wall, so the wall, its back face
				// and whatever stands behind it are all inside the frustum and
				// would draw over the view — the hole would show the back of the
				// wall it leads through. A mirror wants the same thing for a
				// smaller reason: the frame and the back of the glass would
				// otherwise occlude the reflection.
				//
				// The normal is the look direction, so what is kept is everything
				// beyond the mapped pane and what is dropped is everything between
				// it and the camera.
				aim.Lens.ClipNormal = placement.Forward;
				aim.Lens.ClipDistance = placement.Forward.Dot(placement.Centre);

				// The map the pane is read back through. Identity for a mirror,
				// which is what the default already is.
				//
				// **Three fields rather than one, because the map is a similarity
				// and a `CFrame` is not.** A hole between panes of different
				// sizes scales what goes through it, and the pane has to be read
				// back through *exactly* the transform the camera was fitted with
				// or the image slides across the glass. `client::CollectSurfaceViews`
				// composes the three into the matrix the shader wants.
				aim.Lens.Mapping = placement.Map.Frame;
				aim.Lens.MappingOrigin = placement.Map.Origin;
				aim.Lens.MappingScale = placement.Map.Scale;

				pending.push_back(aim);
			}
		);

		// **The slots, handed out here rather than authored on the camera.**
		// A pane is a mirror because a `SurfaceCamera` is parented to it — a
		// plain `Camera` projects nothing — so which texture it uses is the
		// engine's bookkeeping and never a number anybody has to type. It was a
		// `Surface` property on both classes, and that was Roblox's name for
		// something else entirely; what it actually held was a render-target
		// index that the author had to keep unique by hand, with two cameras
		// silently sharing a texture as the failure.
		//
		// **By entity id, which is creation order, and the sort is what makes it
		// deterministic.** `Each` walks archetypes in an order that moves the
		// moment anything changes a component set, so assigning in walk order
		// would shuffle which mirror owned which texture whenever an unrelated
		// component was added — a reflection that jumped between panes for no
		// reason a scene could show. Ids are stable across a snapshot and a
		// replica matches entities by index and generation, so both ends of a
		// wire hand out the same slots without sending them.
		std::sort(pending.begin(), pending.end(), [](const Aim &left, const Aim &right) {
			return left.Camera.Id < right.Camera.Id;
		});

		for (size_t index = 0; index < pending.size(); index++) {
			// **Past the cap is not a mirror, rather than a mirror sharing slot
			// zero.** The renderer has a texture pair per slot and only so many;
			// a scene with more surface cameras than it can draw gets the first
			// `MAX_SURFACES` of them and the rest render nothing. Pointing the
			// overflow at an existing slot would be worse than nothing — two
			// panes showing one camera's reflection, which reads as a projection
			// bug rather than as a budget.
			//
			// **A pane that is not drawing this frame still holds its place in
			// the numbering**, rather than being packed out of it. Compacting
			// would hand its slot to the next mirror along and take it back a
			// frame later, so every other reflection in the scene would swap
			// textures each time one viewer walked past the plane of one pane —
			// a much louder artefact than the one being fixed.
			const bool renders = pending[index].Renders && index < MAX_SURFACES;
			pending[index].Surface = renders ? static_cast<int8_t>(index) : int8_t{-1};
		}

		// **Every write is guarded on the value actually differing, and that is
		// not a micro-optimisation.** `Set` marks the row dirty and `GetMutable`
		// marks it by the act of handing out the pointer — it says so itself. So
		// an unconditional write here is three dirty marks per mirror per frame,
		// for ever, on a mirror nobody has moved.
		//
		// That is invisible in a client store that observes nothing and expensive
		// everywhere else: `mono.server` observes `scene::Transform`, so a static
		// mirror would emit a `Transform` delta for its camera and a `Visual`
		// delta for its pane on **every tick of the game**, and a script watching
		// the pane would get a `Changed` fan-out over every one of its properties
		// at the same rate. A derived value that has not changed is not a write.
		for (const Aim &aim : pending) {
			// **Bitwise, and that is the right comparison rather than a lazy
			// one.** `CFrame` is seven floats with no equality operator, and what
			// is being asked is "did this frame's arithmetic produce what last
			// frame's did" — the same inputs through the same code give the same
			// bits, so an exact compare answers exactly that. A tolerance would
			// be answering a different question and would let a slow drift
			// accumulate unreported.
			// **A non-rendering aim leaves the camera exactly where it was.**
			// There is nothing to point it at, and writing the identity or the
			// eye's own frame would be a dirty mark per mirror per frame for a
			// value nothing reads — the same argument the guarded writes below
			// are here for. The slot is what stops it drawing, not its placement.
			if (aim.Renders) {
				const Transform *placed = store.Get<Transform>(aim.Camera);
				if (placed == nullptr || std::memcmp(&placed->Frame, &aim.Frame, sizeof(CFrame)) != 0) {
					store.Set(aim.Camera, Transform{aim.Frame});
				}
			}

			// **The frustum is the engine's, and it has stopped borrowing the
			// author's fields to say so.** A surface camera parented to a part
			// has its placement written here every frame, and the frustum that
			// placement implies is no more the author's to choose than the
			// placement is — so it goes in `SurfaceLens`, which exists for it.
			//
			// **`Camera::FieldOfView` and `Camera::NearPlane` are no longer
			// overwritten**, which is a change an author can see. They used to
			// be, because there was nowhere else to put a fitted angle and a
			// near plane pushed out to the glass; a script that set either on a
			// parented camera had it taken back on the next frame. Now the near
			// and far planes are read as authored and the fit lives beside them,
			// so setting `FieldOfView` on a surface camera does nothing rather
			// than being reverted — the honest outcome for a field the surface
			// path no longer consults.
			if (aim.Renders) {
				const SurfaceLens *existing = store.Get<SurfaceLens>(aim.Camera);
				if (existing == nullptr || std::memcmp(existing, &aim.Lens, sizeof(SurfaceLens)) != 0) {
					store.Set(aim.Camera, aim.Lens);
				}
			}

			// **Both ends of the pairing, written from one number.** The camera
			// carries the slot it renders into — `client::CollectSurfaceViews`
			// reads it to fill `render::SurfaceView::Index` — and the pane
			// carries the slot it samples. They are the same slot seen from its
			// two ends, and writing them from one variable here is what makes
			// that true by construction rather than by two authors agreeing.
			//
			// The failure this replaces is worth naming: while the number was
			// authored, forgetting it on one of the two left a camera rendering
			// perfectly into a texture nothing sampled, which looks exactly like
			// a mirror that does not work.
			if (const SurfaceCamera *target = store.Get<SurfaceCamera>(aim.Camera);
				target != nullptr && target->Surface != aim.Surface) {
				store.GetMutable<SurfaceCamera>(aim.Camera)->Surface = aim.Surface;
			}

			if (const Visual *visual = store.Get<Visual>(aim.Part);
				visual != nullptr && visual->Surface != aim.Surface) {
				store.GetMutable<Visual>(aim.Part)->Surface = aim.Surface;
			}
		}

		// **What rendered, not what was walked.** A pane the viewer is level with
		// has been visited and told to stop sampling, which is work — but the
		// caller asks this to find out whether there is a reflection in the
		// scene, and counting a blank one would answer yes.
		return static_cast<size_t>(std::count_if(pending.begin(), pending.end(), [](const Aim &aim) {
			return aim.Renders;
		}));
	}

	size_t AppendSurfaceFaceMarkers(Store &store, std::vector<DrawInstance> &out) {
		ENGINE_PROFILE("mark surface faces");

		size_t appended = 0;

		store.Each<const SurfaceCamera, const Camera, const Transform>(
			[&](Entity entity, const SurfaceCamera &target, const Camera &, const Transform &) {
				Face face;
				if (!FaceOf(store, entity, target.Face, face)) {
					return;
				}

				DrawInstance marker;

				// **The part's rotation with the face's position**, so the bar
				// lies in the plane of the face rather than axis-aligned beside
				// it. `MarkerExtent` is written in the part's own axes for the
				// same reason: a half-extent means nothing without the frame it
				// is measured in, and taking both from the part is what keeps a
				// tilted pane's marker tilted with it.
				//
				// Pushed a thickness clear of the glass, because a marker
				// exactly on the surface z-fights it — the same margin, for the
				// same reason, as the near plane above.
				marker.Frame =
					CFrame(face.Centre + face.Normal * MARKER_THICKNESS, face.Placement.Rotation());
				marker.HalfExtent = MarkerExtent(NormalOf(target.Face), face.HalfExtent);

				// Cyan, which is the one hue `Mirrors-1-world.luau` has nothing
				// else in: the pane is white, the frame is brown, the floor is
				// grey and the casters are a scatter that avoids the corner of
				// the cube this sits in. A marker the colour of something else
				// in the scene is a marker somebody has to hunt for.
				marker.Tint = core::Color3{0.1f, 0.9f, 1.0f};

				// Half-transparent, and both halves of that matter. It has to be
				// see-through so it does not hide the reflection it is pointing
				// at, and it has to be *blended* so the surface pass — which
				// draws only the opaque head — never puts it inside a mirror.
				marker.Transparency = 0.5f;

				// Not a mirror itself, and it does not occlude the sun. A
				// debugging aid that cast a shadow would put a bar on the floor
				// of the scene it is describing.
				marker.Surface = -1;
				marker.CastShadow = false;

				out.push_back(marker);
				appended++;
			}
		);

		return appended;
	}

	// The body of both `AppendPortalClones` overloads.
	//
	// `surface` names the one pane to clone through, or is negative for "every
	// pane this world can answer for on its own" — which is every same-world
	// one. The split is the boundary and not a preference: a cross-world pane's
	// far side is a draw list only the host can reach, so the host is what asks
	// for those by name.
	static size_t CloneThroughSeams(Store &store, int surface, std::vector<DrawInstance> &out) {
		ENGINE_PROFILE("clone portal seams");

		std::vector<PortalSeam> &seams = Seams();
		GatherSeams(store, seams);

		// Erase the ones this call may not clone through, so the inner loop is a
		// plain walk. A world whose only pane is a cross-world one — which
		// `ImmersivePortals.luau` is — ends up with no seams and pays for one
		// gather.
		seams.erase(
			std::remove_if(
				seams.begin(),
				seams.end(),
				[surface](const PortalSeam &seam) {
					return surface < 0 ? seam.Crosses : seam.Surface != surface;
				}
			),
			seams.end()
		);

		if (seams.empty()) {
			return 0;
		}

		const float alpha = store.Time().Alpha;
		size_t appended = 0;

		// One body against every seam, appending a copy of it on the far side.
		//
		// **The interpolation is `CollectInstances`' and has to stay it.** A
		// clone built from the tick transform beside an original built from the
		// blended one is a body whose two halves are a frame apart — which at
		// three hundred frames against a sixty hertz tick is a visible seam
		// running down the middle of the character, and reads as the portal
		// itself being misaligned.
		const auto clone = [&](Entity entity,
							   const Transform &placement,
							   const PreviousTransform &previous,
							   const Bounds &bounds,
							   const Visual &visual,
							   const SurfaceAppearance &appearance,
							   const Tags &tags) {
			const CFrame frame = previous.Frame.NLerp(placement.Frame, alpha);

			// A radius rather than the box, which is the conservative side: the
			// test is only deciding whether to *offer* a clone, and one offered
			// early sits behind the destination pane where nothing can see it.
			const float reach = std::sqrt(bounds.HalfExtent.Dot(bounds.HalfExtent));

			for (const PortalSeam &seam : seams) {
				// **A pane straddles its own plane, and so does the part its far
				// end is on.** Cloning either is a portal drawn inside a portal:
				// the picture recurses and, worse, `CrossPortals` gets a second
				// copy of the very surface it decides crossings against.
				if (entity == seam.Pane || entity == seam.Far) {
					continue;
				}

				if (!SeamStraddled(seam, frame.Position, reach)) {
					continue;
				}

				// **The box goes through too.** A clone of a body standing in a
				// hole that changes size has to be the size of the room it is
				// showing up in, or the two halves meet at the pane as a step.
				const SeamTransform through = SeamMapping(seam);
				const CFrame carried = through.Place(frame);

				// **A copy that lands on its own original is not a far half, it
				// is a duplicate.** A pair of panes can be arranged so that the
				// map is near enough the identity for the things beside them —
				// two rooms laid out adjacent, with the hole between them
				// agreeing with the geometry — and every clone then arrives on
				// top of the thing it was copied from. Two coplanar surfaces at
				// the same depth is z-fighting, which is what a stripe of
				// flickering colour along a seam is.
				//
				// Measured against a hair rather than the body's own size: see
				// `COINCIDENT_COPY`. What has to be caught is a map that moves
				// nothing, not a hole between two rooms that happen to be near
				// each other.
				if ((carried.Position - frame.Position).Magnitude() < COINCIDENT_COPY) {
					continue;
				}

				DrawInstance ghost{
					carried,
					bounds.HalfExtent * through.Scale,
					visual.Tint,
					visual.Mesh,
					appearance.ColourMap,
					appearance.NormalMap,
					appearance.RoughnessMap,
					appearance.OcclusionMap,
					appearance.EmissiveMap,
					tags.Mask,
					visual.Transparency,

					// **Never a surface itself.** A cloned mirror would claim
					// the slot its original already writes, and the renderer
					// keeps the first camera to name an index — so the two would
					// fight over one texture from one frame to the next.
					static_cast<int8_t>(-1),
					visual.CastShadow,
					appearance.Mode,
				};

				out.push_back(ghost);
				appended++;

				// **One seam per body.** A body inside two panes at once is a
				// body in a corner case nobody has drawn, and cloning it through
				// both would put two of it on two far sides — the same rule, for
				// the same reason, that `CrossPortals` breaks after one hole.
				break;
			}
		};

		// **Two walks over two small sets, rather than one over the world.**
		// What goes through a portal is what can move: a dynamic body, or a limb
		// posed off one. Everything else is the room, and a room cloned through
		// its own doorway is a second floor inside the first.
		store.Each<
			const Motion,
			const Transform,
			const PreviousTransform,
			const Bounds,
			const Visual,
			const SurfaceAppearance,
			const Tags,
			const Rendered>([&](Entity entity,
								const Motion &,
								const Transform &placement,
								const PreviousTransform &previous,
								const Bounds &bounds,
								const Visual &visual,
								const SurfaceAppearance &appearance,
								const Tags &tags,
								const Rendered &) {
			clone(entity, placement, previous, bounds, visual, appearance, tags);
		});

		// **Anchored, which is why they need their own walk.** `MakeCharacter`
		// anchors every visible limb and poses it off the root each tick, so the
		// only part of a character carrying a `Motion` is the root — and the root
		// is invisible. A character is six boxes this query finds and none the
		// one above does.
		store.Each<
			const CharacterLimb,
			const Transform,
			const PreviousTransform,
			const Bounds,
			const Visual,
			const SurfaceAppearance,
			const Tags,
			const Rendered>([&](Entity entity,
								const CharacterLimb &,
								const Transform &placement,
								const PreviousTransform &previous,
								const Bounds &bounds,
								const Visual &visual,
								const SurfaceAppearance &appearance,
								const Tags &tags,
								const Rendered &) {
			// Whatever the first walk already took. Nothing in this engine
			// carries both today; a rig that did would otherwise be drawn
			// twice on the far side, exactly on top of itself.
			if (store.Has<Motion>(entity)) {
				return;
			}
			clone(entity, placement, previous, bounds, visual, appearance, tags);
		});

		return appended;
	}

	size_t AppendPortalClones(Store &store, std::vector<DrawInstance> &out) {
		return CloneThroughSeams(store, -1, out);
	}

	size_t AppendPortalClones(Store &store, int8_t surface, std::vector<DrawInstance> &out) {
		return CloneThroughSeams(store, static_cast<int>(surface), out);
	}

	bool ClearOfPanes(ecs::Store &store, Vector3 &at) {
		std::vector<PortalSeam> &seams = Seams();
		GatherSeams(store, seams);

		for (const PortalSeam &seam : seams) {
			// **A hole you walk through gets a hair, a picture gets a hand's
			// width.** A same-world pane is drawn by the recursive portal pass,
			// whose only construction is an oblique clip — degenerate exactly on
			// the plane and nowhere else — and the near plane now shrinks to meet
			// it, so an eye may stand as close to one as physics allows and the
			// pane still draws. Pushing it a third of a stud instead is a visible
			// shove at the one moment the illusion is judged, and it is what
			// stopped the approach from ever being seamless.
			//
			// A cross-world pane still goes through `AimSurfaceCameras`, which
			// fits extents to the rectangle from the viewpoint and runs away as
			// that viewpoint reaches the plane. That one keeps the old margin.
			const float clearance = seam.Crosses ? VIEWPOINT_CLEARANCE : SEAM_TOUCH;

			const float offset = SeamOffset(seam, at);
			if (std::abs(offset) >= clearance) {
				continue;
			}

			// **Inside the rectangle and not merely the plane**, which is
			// `CrossingOf`'s distinction: a plane is infinite and a pane is not,
			// and an eye level with a doorway but well to the side of it is
			// standing in a wall rather than in a hole.
			const Vector3 along = at - seam.Centre;
			const float first = along.Dot(seam.First) / seam.First.Dot(seam.First);
			const float second = along.Dot(seam.Second) / seam.Second.Dot(seam.Second);
			if (std::abs(first) > 1.0f || std::abs(second) > 1.0f) {
				continue;
			}

			// **The side it is nearer, and zero counts as behind** — the same
			// tie-break `SeamMapping` makes, so a viewpoint pushed out of a pane
			// and a body carried through one never disagree about which room
			// they are in.
			const float side = offset > 0.0f ? 1.0f : -1.0f;
			at = at + seam.Normal * (side * clearance - offset);
			return true;
		}

		return false;
	}

	size_t OpenPortals(ecs::Store &store) {
		// **Gathered before anything is written**, for the reason every other
		// pass in this file gives: `Store::Set` on a component the row already
		// has is a plain write, but the gather costs nothing and keeps the shape
		// the same as its neighbours — and a `Collider` added to a pane that had
		// none would be an archetype move under an `Each`.
		std::vector<ecs::Entity> panes;

		store.Each<const Portal, const SurfaceCamera>(
			[&](ecs::Entity camera, const Portal &, const SurfaceCamera &) {
				// **The pane is the camera's parent**, which is the same
				// resolution `CrossPortals` and `AimSurfaceCameras` make — a
				// `SurfaceCamera` is a `PVInstance` with a placement and no size,
				// so it is not the thing anybody can walk into.
				const ecs::Entity pane = store.ParentOf(camera);
				if (pane == NULL_ENTITY) {
					return;
				}

				// A pane with no collider is already something a body passes
				// through, and a pane already open is the every-tick case. Both
				// cost one lookup and write nothing.
				const Collider *collider = store.Get<Collider>(pane);
				if (collider == nullptr || collider->Trigger) {
					return;
				}

				panes.push_back(pane);
			}
		);

		size_t opened = 0;
		for (const ecs::Entity pane : panes) {
			const Collider *collider = store.Get<Collider>(pane);
			if (collider == nullptr) {
				continue;
			}

			// **`Set` rather than a write through the reference, because this
			// is a change the broad phase has to see.** `SyncBroadphase` reads
			// the row's stamp to decide whether static geometry moved, and a
			// collider that became a trigger without one would keep being solved
			// against until something else happened to touch the row. It stamps
			// once per portal for the life of the world, not once per tick —
			// the guard above is what makes that true.
			Collider opened_ = *collider;
			opened_.Trigger = true;
			store.Set(pane, opened_);
			opened++;
		}

		return opened;
	}

	bool PortalCrossing(ecs::Store &store, const Vector3 &from, const Vector3 &to, PortalHop &hop) {
		std::vector<PortalSeam> &seams = Seams();
		GatherSeams(store, seams);
		return NearestCrossing(seams, from, to, hop) != nullptr;
	}

	bool PortalCrossing(ecs::Store &store, const Vector3 &from, const Vector3 &to, SeamTransform &through) {
		PortalHop hop;
		if (!PortalCrossing(store, from, to, hop)) {
			return false;
		}
		through = hop.Through;
		return true;
	}

	size_t AppendPortalGhosts(ecs::Store &store, std::vector<DrawInstance> &out) {
		ENGINE_PROFILE("portal ghosts");

		if (out.empty()) {
			return 0;
		}

		// **The same seams the crossing uses**, so a body is ghosted through
		// exactly the rectangle it will be moved through — two answers to "which
		// pane is this" is the kind of disagreement that shows up as a ghost
		// standing somewhere its body never goes.
		std::vector<PortalSeam> seams;
		GatherSeams(store, seams);
		if (seams.empty()) {
			return 0;
		}

		size_t appended = 0;

		// **The list as it stands, not as it grows.** Every ghost is itself a
		// draw instance in `out`, and a walk that saw its own output would ghost
		// the ghosts — a body near two facing panes would fill the buffer.
		const size_t drawn = out.size();

		for (size_t index = 0; index < drawn; index++) {
			const DrawInstance &body = out[index];

			// **A pane never ghosts itself.** It is a hole rather than a thing
			// in the hole, and a copy of it mapped through its own pairing lands
			// exactly on the far pane — z-fighting a wall with a picture on it.
			if (body.Surface >= 0) {
				continue;
			}

			// **The room is not a thing standing in the hole, and this pass
			// stopped trying to work out which is which.** It reads a draw list,
			// where an instance is a frame and a box and nothing else, so every
			// rule it could infer from size was wrong somewhere: measured
			// against the pane's shorter half-axis it refused every character,
			// because a person is very nearly as big as the doorway they walk
			// through; loose enough to admit one, it admitted the floor — and a
			// floor copied through a doorway is the far room's ground laid over
			// the near one, meeting it along a hard straight line through the
			// middle of the scene.
			//
			// The collector knows and this does not, so the collector says. See
			// `DrawInstance::Movable`, which `CloneThroughSeams` answers by
			// walking `Motion` and `CharacterLimb` and a replica's collector
			// answers by asking for the same two.
			if (!body.Movable) {
				continue;
			}

			// A sphere around the instance rather than its oriented box, which
			// is conservative in the only direction that costs nothing: a false
			// positive is a ghost the oblique clip throws away, and a false
			// negative is the half a body that this whole pass exists to draw.
			const float radius = body.HalfExtent.Magnitude();

			for (const PortalSeam &seam : seams) {
				// A cross-world pane leads somewhere this store cannot draw. The
				// far half of a body in one belongs in the *other world's* list,
				// which is a host's to append — see the header.
				if (seam.Crosses) {
					continue;
				}

				// **`SeamStraddled` and not a third copy of the straddle test.**
				// This file had three: the segment one in `CrossingOf`, the
				// public state one, and a hand-rolled version here that agreed
				// with neither about its slack.
				if (!SeamStraddled(seam, body.Frame.Position, radius)) {
					continue;
				}

				const Vector3 offset = body.Frame.Position - seam.Centre;

				// **Which side the body's middle is on decides the map**, the
				// same question `CrossingOf` asks of a segment and
				// `AimSurfaceCameras` asks of a viewer. The half that has
				// crossed belongs in the space the other half is looking into.
				//
				// **`SeamMapping` and not a fourth copy of the product.** This
				// used to compose `destination · half-turn · source⁻¹` by hand,
				// which was the same three lines as the function beside it and
				// went out of date the moment the map gained a scale — a ghost
				// mapped rigidly through a hole that resizes is a body whose far
				// half is the wrong size, meeting its near half at a step in the
				// plane.
				const SeamTransform through = SeamMapping(seam);

				const CFrame carried = through.Place(body.Frame);

				// **A copy that lands on its own original is a duplicate rather
				// than a far half**, and two coplanar surfaces at one depth is
				// the stripe of flickering colour that appears along a seam. See
				// `CloneThroughSeams`, which refuses it for the same reason: a
				// pair of panes can be arranged so the map is near enough the
				// identity for whatever stands beside them.
				if ((carried.Position - body.Frame.Position).Magnitude() < COINCIDENT_COPY) {
					continue;
				}

				DrawInstance ghost = body;
				ghost.Frame = carried;
				ghost.HalfExtent = body.HalfExtent * through.Scale;

				// **Never a surface**, because a ghost that sampled a surface
				// slot would put the far room's picture on a copy of a
				// character.
				ghost.Surface = -1;

				// **But it does cast, and refusing to was wrong.** The note
				// here used to say a second shadow would follow a body that is
				// not there — which is true of a copy standing beside its
				// original and false of this one: the map takes it to the far
				// pane, which is a different room. A body half through a hole
				// has half its shadow in each, and the near half is the only one
				// anything was drawing. What that looks like is a character
				// standing in a doorway whose shadow stops dead at the seam.
				//
				// The instance's own `CastShadow` is kept rather than forced on,
				// so a part authored not to cast still does not.
				ghost.CastShadow = body.CastShadow;

				out.push_back(ghost);
				appended++;

				// One ghost per body. A body in two holes at once is a body at
				// the cone point where two panes meet, and the second copy would
				// land on top of the first.
				break;
			}
		}

		return appended;
	}

	// Grows or shrinks everything about one body that is a length.
	//
	// **What a hole between panes of different sizes does to what goes through
	// it, and it is a list rather than a multiply because a body is not one
	// row.** A crate is its `Bounds` and its `Collider`. A character is those on
	// a root nobody can see, a `Humanoid` on a *third* entity holding every
	// figure that decides how it moves, and five limbs that are their own rows
	// with their own boxes and their own rest offsets. Missing any one of them
	// is visible immediately: a character whose limbs did not scale is a body
	// standing inside a rig the size it used to be.
	//
	// **Multiplied rather than assigned, so the pair is its own inverse.** The
	// scale of a seam is the ratio of two measurements that a crossing does not
	// change, so going back through the other way multiplies by the reciprocal
	// and lands on the number it started with — which is what makes a corridor
	// of mismatched holes a place you can walk around in rather than a ratchet.
	//
	// **Not the mass, and not because it is forgotten.** `PhysicsProperties`
	// derives mass from the volume and the density, so a body whose box has
	// changed already weighs what a body that size weighs.
	//
	// @param store The world.
	// @param body  The row the solver moves, which for a character is its root.
	// @param scale What to multiply by.
	static void ResizeCrosser(Store &store, Entity body, float scale) {
		if (const Bounds *bounds = store.Get<Bounds>(body)) {
			store.Set(body, Bounds{bounds->HalfExtent * scale});
		}

		// **Its own extent and not the bounds'.** A collider may be a different
		// shape and a different size from the box a part draws as, which is
		// exactly what `Collider::Extent` is for — scaling one and not the other
		// is a body that looks right and collides at its old size.
		if (const Collider *collider = store.Get<Collider>(body)) {
			Collider resized = *collider;
			resized.Extent = resized.Extent * scale;
			store.Set(body, resized);
		}

		// The humanoid steering it, which is on the model rather than on the
		// part — `Character` exists to spare everything this a walk by name.
		// A scripted character carries both on one row and is found by the
		// fallback below.
		Entity steering = NULL_ENTITY;
		store.Each<const Character>([&](Entity, const Character &character) {
			if (character.Root == body) {
				steering = character.Humanoid;
			}
		});
		if (steering == NULL_ENTITY && store.Has<Humanoid>(body)) {
			steering = body;
		}

		if (const Humanoid *humanoid = steering == NULL_ENTITY ? nullptr : store.Get<Humanoid>(steering)) {
			Humanoid resized = *humanoid;

			// **Every figure here is a length or a length per second**, and the
			// two that are easy to leave out are the ones that show. A ground
			// tolerance that did not shrink is a character hovering a tenth of
			// its own height off the floor; a walk speed that did not is a body
			// a tenth the size crossing the room at the same rate, which reads
			// as the portal having made it fast rather than small.
			resized.Height *= scale;
			resized.Radius *= scale;
			resized.WalkSpeed *= scale;
			resized.JumpSpeed *= scale;
			resized.GroundTolerance *= scale;
			store.Set(steering, resized);
		}

		// **Gathered before writing**, because adding nothing and changing a
		// component are different costs and a walk that wrote inside itself
		// would queue a heap copy per limb. The same two phases every other pass
		// in this file uses, for the same reason.
		std::vector<Entity> limbs;
		store.Each<const CharacterLimb>([&](Entity limb, const CharacterLimb &carried) {
			if (carried.Root == body) {
				limbs.push_back(limb);
			}
		});

		for (const Entity limb : limbs) {
			// **The rest offset as well as the box.** `PoseCharacterLimbs` puts
			// a limb at `root · Offset` every tick, so a rig whose offsets kept
			// their old lengths is a character shrunk in the middle of a skeleton
			// that did not — arms at the old distance, boxes at the new size.
			if (const CharacterLimb *carried = store.Get<CharacterLimb>(limb)) {
				CharacterLimb moved = *carried;
				moved.Offset = CFrame{moved.Offset.Position * scale, moved.Offset.Rotation()};
				store.Set(limb, moved);
			}

			if (const Bounds *bounds = store.Get<Bounds>(limb)) {
				store.Set(limb, Bounds{bounds->HalfExtent * scale});
			}

			if (const Collider *collider = store.Get<Collider>(limb)) {
				Collider resized = *collider;
				resized.Extent = resized.Extent * scale;
				store.Set(limb, resized);
			}
		}
	}

	size_t CrossPortals(ecs::Store &store) {
		// **The portals are gathered before anything is moved.** A body pushed
		// through one lands somewhere another might also claim, and a single
		// pass that moved as it walked would let one crossing feed the next
		// within a tick — a character could be bounced through three holes on
		// one step, which is neither what the author drew nor reproducible.
		std::vector<PortalSeam> &holes = Seams();
		GatherSeams(store, holes);

		if (holes.empty()) {
			return 0;
		}

		size_t crossed = 0;

		// What went through a hole that changes size, and by how much. Empty in
		// every world whose panes match, which is every world with a mirror in
		// it and most with a portal.
		struct Resize {
			ecs::Entity Body;
			float Scale;
		};
		std::vector<Resize> resized;

		// **Anything with a velocity, not only a character.** A portal that
		// swallowed people and refused a thrown crate would be a portal with a
		// footnote, and the arithmetic does not care which it is. Anchored
		// scenery carries no `Motion` and is therefore never a candidate, which
		// is the archetype doing the filtering rather than a branch.
		store.Each<Transform, Motion, PreviousTransform>(
			[&](ecs::Entity entity, Transform &placement, Motion &motion, PreviousTransform &before) {
				const Vector3 was = before.Frame.Position;
				const Vector3 now = placement.Frame.Position;

				// **The nearest hole this step met, by the same rule and the
				// same code as the camera's.** This used to take the first seam
				// the gather happened to list, which is archetype order and
				// therefore not a rule — and `PlaceCamera` already took the
				// nearest for its arm. On any frame the two chose differently
				// the body went through one hole and the eye through another,
				// and what that looks like is the view flipping on some
				// crossings and not others with nothing to tell them apart.
				//
				// **One hole per body per tick**, which the single answer now
				// says rather than a `break` at the bottom of a loop: a body
				// bounced through three holes on one step is neither what the
				// author drew nor reproducible.
				PortalHop hop;
				const PortalSeam *met = NearestCrossing(holes, was, now, hop);
				if (met == nullptr) {
					return;
				}

				const PortalSeam &hole = *met;
				const SeamTransform &through = hop.Through;

				{
					// **Put down clear of the plane it is about to be on the far
					// side of.** Pushed before the map rather than after, so the
					// only normal involved is the source pane's — which this pass
					// already has — and the rigid map carries the clearance to the
					// destination as the same distance out of the far pane. See
					// `LANDING_CLEARANCE`.
					//
					// **A floor on the depth rather than a push, and the
					// difference is a drift you can see.** Adding the clearance
					// unconditionally moves every crossing by it, so a body that
					// walks through a hole and back comes out beside where it
					// started, and again on the next round trip. What is wanted
					// is that nothing *rests* within the clearance of a plane,
					// which is a minimum and not an offset: a body that ended
					// its step a metre past the pane already has it, and gets
					// nothing added.
					//
					// Away from the side it came in on, which is the direction it
					// was already travelling.
					// **Read before anything is mapped**, because the turn below
					// is the difference between this and its image and every
					// line between here and there overwrites one of them.
					const Vector3 facing = placement.Frame.VectorToWorldSpace({0.0f, 0.0f, -1.0f});

					const float side = SeamOffset(hole, was) > 0.0f ? -1.0f : 1.0f;
					const float depth = std::abs(SeamOffset(hole, now));
					const Vector3 clear = hole.Normal * (side * std::max(LANDING_CLEARANCE - depth, 0.0f));

					// **The placement and the velocity, by the same transform.**
					// Forgetting the second is the bug that looks like physics:
					// the body arrives aimed the way it was aimed in the frame it
					// left, so it walks out of the destination sideways.
					//
					// **`Carry` for the velocity, because a speed is a length.**
					// A body that comes out of the small end of a hole at the
					// speed it went into the large end crosses the far room in a
					// fraction of the time, which reads as the portal firing you
					// out rather than as a change of scale.
					placement.Frame =
						through.Place(CFrame{placement.Frame.Position + clear, placement.Frame.Rotation()});
					motion.Linear = through.Carry(motion.Linear);

					// **And where it started the tick, or the frames between now
					// and the next tick draw it halfway between two rooms.**
					// `CapturePreviousTransforms` records where a body was when
					// the tick began and the renderer blends the two by the
					// tick's alpha — so a body teleported after that capture is
					// interpolated *across the teleport*, and at three frames to
					// a tick it is drawn once or twice somewhere in the hundred
					// units between the two panes. What that looks like is the
					// character snapping, with a streak where it went.
					//
					// **Mapped rather than collapsed onto the new placement.**
					// CodeParade's demo assigns `prev_pos = pos`, which removes
					// the streak by removing the motion — the body stands still
					// for the rest of the tick and then jumps. Putting the old
					// position through the same map keeps the whole tick, just
					// expressed in the room the body is now in, so it walks out
					// of the far pane at the speed it walked into the near one.
					before.Frame = through.Place(before.Frame);

					// **And the body itself, when the two ends are not the same
					// size.** Gathered rather than applied here for the reason
					// every pass in this file gives about its own two phases: this
					// touches other entities — a character's limbs are their own
					// rows — and an archetype the walk is not standing on is still
					// a deferred command with a heap copy behind it.
					if (through.Scale != 1.0f) {
						resized.push_back(Resize{entity, through.Scale});
					}

					// **And the turn is written down, because the eye that has
					// to follow it is on another machine.** This is the same
					// bug as the velocity one wearing a different coat: a
					// player walks west through a hole whose pair turns a
					// corner, the body comes out walking north, and the view
					// keeps pointing west — so the character reads as spinning
					// on the spot and W stops meaning forward, because
					// `ReadMoveIntent` is relative to the camera's yaw.
					//
					// It cannot be fixed here by reaching for the camera.
					// `CameraController` is a resource on whichever host is
					// *looking*, and the host running this is whichever one is
					// *simulating* — in a studio Play or against a real server
					// those are two different worlds, and the authority's
					// controller is not the one the player sees through.
					// `scene::PortalTransit` is the fact; `FollowPortalTransit`
					// is the client end of it.
					//
					// **Measured off the crosser's own facing, which is what
					// CodeParade's `TryPortal` does and is the half this got
					// wrong.** Mapping a fixed reference — north — and calling
					// the result the turn is only right when the map is a pure
					// yaw. Give either pane any tilt at all, or a pair whose
					// faces are not both level, and the composed rotation has
					// pitch and roll in it: the yaw of the mapped north is then
					// not the yaw anything actually turned through, and it is
					// wrong by an amount that depends on the geometry rather
					// than on anything the player did. What that reads as is the
					// view snapping to an angle nobody entered from, on some
					// pairs and not others.
					//
					// The body's own forward, mapped, minus where it started, is
					// the angle that body turned. For a level pair it is the
					// same number the old rule gave.
					//
					// **`Rotate`, so a scaled hole still reports the angle it
					// turns through.** A yaw is not a length, and `Carry` here
					// would leave `atan2` measuring a vector scaled on both
					// components — the same angle, arrived at by luck — or a zero
					// vector for a hole that shrinks to nothing.
					const Vector3 turned = through.Rotate(facing);

					if ((std::abs(turned.X) > 1e-6f || std::abs(turned.Z) > 1e-6f) &&
						(std::abs(facing.X) > 1e-6f || std::abs(facing.Z) > 1e-6f)) {
						PortalTransit went;
						if (const PortalTransit *before_ = store.Get<PortalTransit>(entity)) {
							went = *before_;
						}

						// Wrapped, so a quarter turn is reported as a quarter
						// turn and never as seven quarters the other way — the
						// camera it reaches adds it to an angle it already has.
						float turn = std::atan2(-turned.X, -turned.Z) - std::atan2(-facing.X, -facing.Z);
						turn = std::remainder(turn, 2.0f * PI);

						went.Serial++;
						went.Turn = turn;
						store.Set(entity, went);
					}

					crossed++;
				}
			}
		);

		// **After the walk, where the defer depth is back to zero.** A resize
		// touches a character's limbs, which are rows the walk above never
		// visits, and `ResizeCrosser` walks the world twice to find them — an
		// `Each` opened inside another one is a great deal of machinery to run
		// per crossing when a crossing is already over.
		for (const Resize &change : resized) {
			ResizeCrosser(store, change.Body, change.Scale);
		}

		return crossed;
	}

}
