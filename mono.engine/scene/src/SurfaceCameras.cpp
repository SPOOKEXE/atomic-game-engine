#include <engine/core/Profiling.hpp>
#include <engine/core/types/Color3.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Controls.hpp>
#include <engine/scene/DrawInstance.hpp>
#include <engine/scene/Enums.hpp>
#include <engine/scene/SurfaceCameras.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
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

		// How close to a pane's own plane the viewer may be before its surface
		// stops drawing.
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
			CFrame Map;
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

		// One end of one pairing, in the terms a segment test wants.
		//
		// **Shared by the body pass and the camera arm**, which is the whole
		// reason it is a type rather than four locals: a character that goes
		// through a hole and a camera that does not follow it is the same bug
		// twice, and the only way the two cannot disagree is for them to run the
		// same test over the same rectangle.
		struct Hole {
			// The pane's plane: a point on it and its face's unit normal.
			//
			// **The face's normal, not "the outward one".** Which side is
			// outward is a question about the *crosser*, exactly as it is a
			// question about the viewer in `AimSurfaceCameras` — a pane can be
			// walked into from either side and both answers are right. So the
			// sign is decided per crosser, below.
			Vector3 Centre;
			Vector3 Normal;

			// The pane's half-axes in the world, so `Centre ± First ± Second`
			// is the rectangle. Kept as vectors rather than as extents because
			// that is what the inside test needs.
			Vector3 First;
			Vector3 Second;

			// The far pane's face frame, looking out of itself. The half of the
			// mapping that does not depend on who is crossing.
			CFrame Destination;
		};

		// Every linked portal in the world, as a rectangle and a destination
		// frame.
		std::vector<Hole> GatherHoles(Store &store) {
			std::vector<Hole> holes;

			store.Each<const Portal, const SurfaceCamera>(
				[&](Entity entity, const Portal &portal, const SurfaceCamera &camera) {
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
					const Vector3 farCentre =
						far->Frame.Position + farNormal * ReachOf(*farBounds, camera.Face);

					Hole hole;
					hole.Centre = face.Centre;
					hole.Normal = face.Normal;
					FaceAxes(face.Placement, local, face.HalfExtent, hole.First, hole.Second);
					hole.Destination = CFrame::LookAt(farCentre, farCentre + farNormal, UpFor(farNormal));

					holes.push_back(hole);
				}
			);

			return holes;
		}

		// Whether a segment goes through one hole, and what carries it there.
		//
		// @param hole    The pane to test against.
		// @param was     Where the segment starts.
		// @param now     Where it ends.
		// @param through The map from this side to the far side, written only
		//                when the answer is true.
		bool CrossingOf(const Hole &hole, const Vector3 &was, const Vector3 &now, CFrame &through) {
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
			const float share = from / (from - to);
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
			const Vector3 outward = hole.Normal * (from > 0.0f ? 1.0f : -1.0f);
			const CFrame source = CFrame::LookAt(hole.Centre, hole.Centre + outward, UpFor(outward));

			// `destination · half-turn · source⁻¹`, the same product the camera
			// goes through — see `AimSurfaceCameras`' `linked` branch for why
			// the half-turn is what makes it a hole rather than a window onto a
			// copy.
			through = hole.Destination * CFrame::Angles(0.0f, PI, 0.0f) * source.Inverse();
			return true;
		}
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

				// **Edge-on renders nothing.** See `EDGE_ON_MARGIN`: this is the
				// one band where there is no continuous orientation to aim for,
				// and a pane the viewer is level with covers no pixels anyway.
				// Carried through as a non-rendering aim rather than returned
				// from, so the pane is told to stop sampling its slot.
				if (std::abs(distance) < EDGE_ON_MARGIN) {
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

				// **The one branch that makes a portal a portal.** Everything after
				// it is shared, because both cases have produced the same five
				// facts: where the camera stands, which way it looks, and the
				// rectangle it has to cover.
				const Portal *portal = store.Get<Portal>(entity);
				const bool linked = portal != nullptr && portal->Destination != NULL_ENTITY &&
									store.Alive(portal->Destination) &&
									store.Get<Transform>(portal->Destination) != nullptr &&
									store.Get<Bounds>(portal->Destination) != nullptr;

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

					// **`destination · half-turn · source⁻¹`, and the half-turn is
					// what makes it a hole rather than a window onto a copy.**
					// Without it the camera arrives at the far pane facing back the
					// way it came, so the portal shows the room the viewer is
					// already standing in.
					//
					// **Nothing here constrains the two frames to describe one
					// space, and that is the entire non-Euclidean feature.** A
					// destination turned, moved or placed anywhere gives a room
					// bigger on the inside or a corridor that turns through more
					// than four right angles — with no second mechanism and no
					// maths past this multiply. `docs/NON-EUCLIDEAN.md` is the
					// investigation that settled it.
					const CFrame through = destination * CFrame::Angles(0.0f, PI, 0.0f) * source.Inverse();

					placement.Eye = through.PointToWorldSpace(eye);
					placement.Centre = through.PointToWorldSpace(centre);
					placement.First = through.VectorToWorldSpace(first);
					placement.Second = through.VectorToWorldSpace(second);

					// **Negated, and this is the one sign in the file worth
					// deriving rather than trying.** `through` is rigid, so it
					// preserves which side of the pane a point is on: the eye
					// stands at `+outward` from the source, so the camera lands
					// at `+outward` from the *mapped* pane. A mirror's camera is
					// on the far side and looks along the outward normal; a
					// portal's is on the near side and has to look back through
					// the rectangle, which is the other way.
					//
					// Getting it wrong points the camera away from the hole, so
					// the portal shows whatever happens to be behind the
					// destination — which looks like a portal that works and
					// leads somewhere wrong.
					placement.Forward = through.VectorToWorldSpace(outward) * -1.0f;

					// And the pane is mapped by the same matrix when it reads
					// the image back, which is what makes the two line up.
					placement.Map = through;
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
				aim.Lens.Mapping = placement.Map;

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

	bool PortalCrossing(ecs::Store &store, const Vector3 &from, const Vector3 &to, CFrame &through) {
		for (const Hole &hole : GatherHoles(store)) {
			if (CrossingOf(hole, from, to, through)) {
				return true;
			}
		}

		return false;
	}

	size_t CrossPortals(ecs::Store &store) {
		// **The portals are gathered before anything is moved.** A body pushed
		// through one lands somewhere another might also claim, and a single
		// pass that moved as it walked would let one crossing feed the next
		// within a tick — a character could be bounced through three holes on
		// one step, which is neither what the author drew nor reproducible.
		const std::vector<Hole> holes = GatherHoles(store);

		if (holes.empty()) {
			return 0;
		}

		size_t crossed = 0;

		// **Anything with a velocity, not only a character.** A portal that
		// swallowed people and refused a thrown crate would be a portal with a
		// footnote, and the arithmetic does not care which it is. Anchored
		// scenery carries no `Motion` and is therefore never a candidate, which
		// is the archetype doing the filtering rather than a branch.
		store.Each<Transform, Motion, const PreviousTransform>(
			[&](ecs::Entity entity, Transform &placement, Motion &motion, const PreviousTransform &before) {
				const Vector3 was = before.Frame.Position;
				const Vector3 now = placement.Frame.Position;

				for (const Hole &hole : holes) {
					CFrame through;
					if (!CrossingOf(hole, was, now, through)) {
						continue;
					}

					// **The placement and the velocity, by the same transform.**
					// Forgetting the second is the bug that looks like physics:
					// the body arrives aimed the way it was aimed in the frame it
					// left, so it walks out of the destination sideways.
					placement.Frame = through * placement.Frame;
					motion.Linear = through.VectorToWorldSpace(motion.Linear);

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
					// **Measured off the map and not off the crosser**, so it
					// is the same number for everything that goes through this
					// hole this tick and does not depend on which way any of
					// them happened to be facing. North is the reference for no
					// reason but that a yaw of zero is north.
					const Vector3 north{0.0f, 0.0f, -1.0f};
					const Vector3 turned = through.VectorToWorldSpace(north);

					if (std::abs(turned.X) > 1e-6f || std::abs(turned.Z) > 1e-6f) {
						PortalTransit went;
						if (const PortalTransit *before_ = store.Get<PortalTransit>(entity)) {
							went = *before_;
						}

						went.Serial++;
						went.Turn = std::atan2(-turned.X, -turned.Z);
						store.Set(entity, went);
					}

					crossed++;

					// One hole per body per tick. See the gathering pass above.
					break;
				}
			}
		);

		return crossed;
	}

}
