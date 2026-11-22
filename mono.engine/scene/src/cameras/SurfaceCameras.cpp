// Surface camera aiming: placing mirror and portal cameras against their panes,
// fitting their lenses, gathering panes and slots, and drawing face markers.

#include "cameras/SurfaceCamerasInternal.hpp"

#include <engine/core/Profiling.hpp>
#include <engine/core/types/Color3.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/DrawInstance.hpp>
#include <engine/scene/Enums.hpp>
#include <engine/scene/cameras/ActiveCamera.hpp>
#include <engine/scene/cameras/CameraContinuation.hpp>
#include <engine/scene/cameras/SurfaceCameras.hpp>
#include <engine/scene/cameras/ViewEye.hpp>
#include <engine/scene/characters/Characters.hpp>
#include <engine/scene/characters/Controls.hpp>
#include <engine/scene/environment/Sunlight.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

namespace engine::scene {

	namespace {
		using core::CFrame;
		using core::Vector3;
		using ecs::Entity;
		using ecs::NULL_ENTITY;
		using ecs::Store;
		using surface_camera_detail::EDGE_ON_MARGIN;
		using surface_camera_detail::Face;
		using surface_camera_detail::FaceAxes;
		using surface_camera_detail::FaceFrame;
		using surface_camera_detail::FaceOf;
		using surface_camera_detail::LinkedPortalOf;
		using surface_camera_detail::PI;
		using surface_camera_detail::ReachOf;
		using surface_camera_detail::ScaleBetween;
		using surface_camera_detail::UpFor;
	}

	namespace surface_camera_detail {
		// The half-extent along a face's own axis.
		//
		// **Derived from `NormalOf` rather than switching on the face again.**
		// A face normal is axis-aligned and unit, so a dot with the half-extent
		// picks exactly the matching axis and `abs` drops the sign - which means
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
	}

	namespace {
		// The cameras to place, collected before anything is written.
		//
		// **Not for safety - `Each` already makes writing inside it safe - but
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
			int16_t Surface = -1;

			// Whether there is a reflection to draw at all this frame.
			//
			// False for a pane the viewer is in the plane of - see
			// `EDGE_ON_MARGIN`. Such an aim still travels through the rest of
			// this pass, because the *pane* has to be told to stop sampling; an
			// aim simply dropped on the floor would leave it showing the last
			// frame that was rendered into its slot.
			bool Renders = true;
		};

		std::vector<Aim> &PendingAims() {
			static thread_local std::vector<Aim> pending;
			return pending;
		}

		// The parts this frame's aim put on a slot, sorted by entity id.
		//
		// Scratch beside `PendingAims()` and for the same reason: it is refilled
		// once per aim and would otherwise be an allocation per world per frame.
		std::vector<uint64_t> &AimedParts() {
			static thread_local std::vector<uint64_t> aimed;
			return aimed;
		}

		// Takes a pane off its surface slot when nothing aims at it any more.
		//
		// **The only place a deleted mirror can be noticed.** The walk in
		// `AimSurfaceCameras` visits live `SurfaceCamera` rows, so a pane whose
		// camera was destroyed - or whose `SurfaceCamera` was removed because
		// its `SurfaceSize` fell below a texel, which `Part.cpp` does - is never
		// visited again and keeps the slot it was last told. The renderer's
		// texture for that slot still holds the frame the camera drew, so the
		// pane goes on sampling it: a reflection of a room the viewer has walked
		// out of. That is the same frozen picture `EDGE_ON_MARGIN` blanks a pane
		// for, reached by a different route.
		//
		// **Counted first, cleared second, and the count is what makes this
		// affordable.** `Visual` is on every drawn part, so finding the holders
		// is a column scan. `EachBatch` reads that column with no per-row call
		// and no entity handle, which is one compare per part; the second walk
		// runs only when more panes hold a slot than this frame handed out, and
		// that mismatch is a mirror having gone.
		void ReleaseUnaimedSurfaces(Store &store, std::span<const Aim> aims) {
			ENGINE_PROFILE("release unaimed surfaces");

			// **Deduplicated, because two cameras aiming at one part hand out
			// two slots and leave one holder.** Counting the slots instead would
			// read that as a pane nobody claimed and run the sweep every frame.
			std::vector<uint64_t> &aimed = AimedParts();
			aimed.clear();
			for (const Aim &aim : aims) {
				if (aim.Surface >= 0) {
					aimed.push_back(aim.Part.Id);
				}
			}
			std::sort(aimed.begin(), aimed.end());
			aimed.erase(std::unique(aimed.begin(), aimed.end()), aimed.end());

			size_t holding = 0;
			store.EachBatch<const Visual>([&](size_t rows, const Visual *visuals) {
				for (size_t row = 0; row < rows; row++) {
					holding += visuals[row].Surface >= 0 ? 1u : 0u;
				}
			});

			if (holding <= aimed.size()) {
				return;
			}

			store.Each<const Visual>([&](Entity part, const Visual &visual) {
				if (visual.Surface < 0 || std::binary_search(aimed.begin(), aimed.end(), part.Id)) {
					return;
				}

				// `GetMutable` rather than `Set`: the row is already there, and
				// the guarded-write argument below applies here too - this is
				// reached only for a pane that is actually changing.
				store.GetMutable<Visual>(part)->Surface = -1;
			});
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
		// which no frustum covers - the floor is what turns that into a
		// reflection that stops covering the far corners rather than a matrix
		// full of infinities.
		//
		// **Unlike the field of view it replaces, nothing is clamped on the way
		// out.** The old fit needed a ceiling just under 180° because it took a
		// tangent, and that ceiling made it a step function - which is what read
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

	}

	namespace surface_camera_detail {
		// An up vector a `LookAt` can actually use for this direction.
		//
		// **A different up for a floor or a ceiling.** `LookAt` builds its
		// rotation against an up vector and cannot when the two are parallel - a
		// mirror in the floor faces straight up, which is the one case the
		// default cannot resolve, and it produces a NaN rotation that spreads
		// into the frame, the near plane and every bound derived from them.
		//
		// Its own function because a portal needs it three times where a mirror
		// needed it once - the source face, the destination face and the camera
		// - and three copies of a guard is three places for one of them to be
		// forgotten.
		Vector3 UpFor(const Vector3 &forward) {
			return std::abs(forward.Dot(Vector3::YAxis)) > 0.99f ? Vector3::ZAxis : Vector3::YAxis;
		}

		// Resolve the face in local axes first so rotation about its normal is
		// retained, including the local Z-up convention on top and bottom faces.
		CFrame FaceFrame(const CFrame &part, const Vector3 &centre, const Vector3 &localNormal) {
			const Vector3 normal = part.VectorToWorldSpace(localNormal).Unit();
			const Vector3 up = part.VectorToWorldSpace(UpFor(localNormal));
			return CFrame::LookAt(centre, centre + normal, up);
		}

		// How much bigger one pane is than another, as a single number.
		//
		// **The square root of the area ratio.** For two rectangles of the same
		// shape that is exactly the ratio of their sides, which is the answer
		// anybody expects and the only case a sane author builds. For two of
		// different shapes there is no such number at all, and this returns the
		// one that treats the two axes alike rather than the one that depends on
		// which axis `FaceAxes` happened to call first - a choice that would
		// make the same pair scale differently on a face turned ninety degrees.
		//
		// **One for a degenerate pane rather than an infinity or a zero.** A
		// part flattened to nothing on one of a face's two axes is a pane with no
		// area, and every consumer of this multiplies a length by it - so the
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

	}

	namespace {
		// Where a surface camera stands, and the rectangle it has to cover.
		//
		// **One shape for a mirror and a portal, because they differ in one
		// step.** Both take the pane, map it somewhere, put the camera in front of
		// the mapped rectangle and fit a frustum to it. A mirror's map is the
		// reflection through its own plane - which fixes that plane, so the mapped
		// rectangle is the pane itself and the arithmetic collapses back to what
		// planar reflection always was. A portal's map takes it to the far side.
		//
		// **The rectangle is the *mapped source* pane and never the destination
		// part**, and that is the one thing to get right. `opaque.frag` shades a
		// fragment of the *source* pane by projecting it through this camera's
		// matrix; that only lines up because the camera and the rectangle were
		// moved by the same map. Fitting to the destination part instead would be
		// correct exactly when the two panes are the same size and silently
		// wrong - an image sliding across the hole - whenever they are not.
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
			// samples the image - `SurfaceLens::Mapping` says why, and a second
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
			// against the glass almost all of the pane is off screen - a pane
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
			// Empty - a zero count - when there is no eye frustum to be had, in
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
		// that does not cover the whole pane does not stretch or fade - it draws a
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
		// leans rather than one widened symmetrically about the view axis - the
		// same coverage on twice the texels. The symmetric fit had to pay for the
		// far edge of the pane on both sides.
		//
		// **And there is no clamp any more, which is a consequence rather than an
		// omission.** The old fit took a *tangent* of a half-angle, which grows
		// without bound as a corner approaches the camera's plane, so it needed a
		// floor on the depth and a ceiling just under 180° - and the ceiling made
		// the fit a step function, which read as the mirror flashing once per
		// orbit. These extents are a min and a max over four projected positions.
		// A corner approaching zero depth still sends one of them off to infinity,
		// so the depth floor stays; but nothing is being clamped back afterwards,
		// so nothing can step.
		//
		// @param placement Where the camera is and what it must cover.
		// @param up        The camera's up vector, matching the frame the caller
		//        builds - the two must agree or the extents describe a different
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
				// guard below widens it to something finite - which is right,
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

		// The camera a mapped pane is rendered from: aimed square on to it, an
		// off-axis frustum fitted to it, the near plane skewed onto its plane, and
		// the map it was fitted through carried out for the pane to sample with.
		//
		// **The four steps a mirror and a hole share, written once.** The two
		// differ in where the pane was mapped to and in nothing else - `Placement`
		// *is* that difference - so this is the tail of both. It was inline in
		// `AimSurfaceCameras` while both maps were computed there; the mirror's
		// moved out to `ReflectCamera` so a recursion could reach it, and this is
		// what stopped that move from leaving two copies of the tail behind.
		//
		// @param placement Where the camera stands and what it must cover.
		// @param near      The author's near plane, which the extents are measured
		//                  at.
		// @param far       The author's far plane, carried through untouched.
		// @return The camera, always rendering: whether there is anything to show
		//         is a question about the *viewer* and is settled before a
		//         placement exists.
		MirrorEye AimAt(const Placement &placement, float near, float far) {
			// **Aimed, not merely placed.** An identity rotation looks down -Z, so
			// a camera put behind a pane faces away from it and renders empty
			// space. That was the first version of the script this replaces, and
			// the mirror came out showing the clear colour.
			//
			// **Square on to the rectangle, and not at its centre.** Aiming at the
			// middle sounds like the same thing and is not: it tilts the view axis
			// off the normal by however far the viewer stands to one side, and the
			// pane then lies at an angle across the frustum. Push that far enough
			// - close to the glass and off to the side, which is a metre from a
			// wall in a room - and the nearest corner goes *behind* the camera,
			// which nothing covers.
			//
			// Looking along the normal puts every corner at the same depth, so the
			// fit is always finite and the corners are always in front. It costs
			// nothing in correctness: the image is read back by projecting each
			// fragment through this camera's own matrix, so the orientation
			// decides which texels the pane lands on and never which part of the
			// world it shows. **Leaning is the frustum's job**, which is what an
			// off-axis fit is for and what a symmetric one could not do.
			const Vector3 up = UpFor(placement.Forward);

			MirrorEye aimed;
			aimed.Frame = CFrame::LookAt(placement.Eye, placement.Eye + placement.Forward, up);

			// **The near and far planes are the author's.** Pushing the near plane
			// out to the glass was the poor man's oblique clip, and there is a
			// real one below - so the engine has stopped overwriting a number it
			// no longer needs to borrow.
			aimed.Lens.NearPlane = near;
			aimed.Lens.FarPlane = far;

			FitExtents(placement, up, aimed.Lens.NearPlane, aimed.Lens);

			// **The real oblique clip, and on a portal it is not optional.** The
			// destination is set into a wall, so the wall, its back face and
			// whatever stands behind it are all inside the frustum and would draw
			// over the view - the hole would show the back of the wall it leads
			// through. A mirror wants the same thing for a smaller reason: the
			// frame and the back of the glass would otherwise occlude the
			// reflection.
			//
			// The normal is the look direction, so what is kept is everything
			// beyond the mapped pane and what is dropped is everything between it
			// and the camera.
			aimed.Lens.ClipNormal = placement.Forward;
			aimed.Lens.ClipDistance = placement.Forward.Dot(placement.Centre);

			// The map the pane is read back through, identity for a mirror.
			//
			// **Three fields rather than one, because the map is a similarity and
			// a `CFrame` is not.** A hole between panes of different sizes scales
			// what goes through it, and the pane has to be read back through
			// *exactly* the transform the camera was fitted with or the image
			// slides across the glass. `client::CollectSurfaceViews` composes the
			// three into the matrix the shader wants.
			aimed.Lens.Mapping = placement.Map.Frame;
			aimed.Lens.MappingOrigin = placement.Map.Origin;
			aimed.Lens.MappingScale = placement.Map.Scale;

			aimed.Renders = true;
			return aimed;
		}

		// How thick the face marker is, in studs, on the two axes it is not
		// spanning. Absolute rather than a fraction of the pane: a marker scaled
		// to its part is invisible on a small one and a plank on a large one,
		// and what it has to be is legible at whatever size the pane happens to
		// be.
		constexpr float MARKER_THICKNESS = 0.03f;

		// How much of the face's longer in-plane half-axis the bar covers.
		// **A third rather than all of it**, because a bar spanning the whole
		// face reads as a frame around the mirror - which is the one thing in
		// `Mirrors-1-world.luau` it must not be mistaken for.
		constexpr float MARKER_SPAN = 0.35f;

		// The marker's own half-extent, in the part's local axes.
		//
		// **Along the longer of the two axes that lie in the face.** A 16x9 pane
		// has a long axis and a short one, and a bar across the short one is a
		// dash somebody has to look for; across the long one it is a line. Which
		// two axes those are follows from the face's own - the normal is
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
	}

	namespace surface_camera_detail {
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
				// A parent that is not a part in space - a `Model`, a
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
			// reach what one rotate gives - and loses precision for a pane
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

		// The portal on a surface camera, if it names a destination this pass can
		// actually place a camera against.
		//
		// **One test, because two passes decide the same thing and disagreeing is
		// a pane nobody draws.** `AimSurfaceCameras` asks it to choose a branch
		// and `GatherSurfacePanes` asks it to choose a list; a pane that one
		// called a hole and the other called a mirror would either be drawn twice
		// or not at all. `Transform` and `Bounds` are part of the question rather
		// than a guard around it: a destination with neither is a link the
		// arithmetic cannot follow, and the fallback for that is the same as for
		// no link at all - a hole leading nowhere is a wall.
		const Portal *LinkedPortalOf(Store &store, Entity camera) {
			const Portal *portal = store.Get<Portal>(camera);
			if (portal == nullptr || !portal->Enabled || portal->Destination == NULL_ENTITY ||
				!store.Alive(portal->Destination)) {
				return nullptr;
			}
			if (store.Get<Transform>(portal->Destination) == nullptr ||
				store.Get<Bounds>(portal->Destination) == nullptr) {
				return nullptr;
			}
			return portal;
		}
	}

	size_t FrustumCorners(const CFrame &frame, float fieldOfViewRadians, float aspect, Vector3 (&out)[4]) {
		// A window reports zero height while it is minimised, which is an
		// ordinary frame rather than an error - `ResolveCamera` takes the same
		// view of it.
		const float width = aspect > 0.0f ? aspect : 1.0f;
		const float up = std::tan(fieldOfViewRadians * 0.5f);
		const float across = up * width;

		// **A little wider than the screen exactly needs**, for `FIT_MARGIN`'s
		// reason: the edge of a frustum is not a safe place to sample, and a
		// clamp landing exactly on the screen edge puts the pane's visible
		// boundary on the texture's boundary.
		const float slack = FIT_MARGIN;

		int corner = 0;
		for (int x = -1; x <= 1; x += 2) {
			for (int y = -1; y <= 1; y += 2) {
				const Vector3 local{
					static_cast<float>(x) * across * slack, static_cast<float>(y) * up * slack, -1.0f
				};
				out[corner++] = frame.VectorToWorldSpace(local);
			}
		}
		return 4;
	}

	size_t FrustumCorners(const CFrame &frame, const SurfaceLens &lens, Vector3 (&out)[4]) {
		// **Divided out to the same one-unit depth the other overload hands
		// back.** The extents are measured *at* the near plane, so dividing by it
		// is what turns them into directions rather than into points - and the
		// two overloads have to agree about that scale, because it cancels
		// everywhere in the fit except against the depth floor.
		//
		// A lens with no usable near plane cannot say how wide it is; taking one
		// leaves the directions finite and the clamp meaningless, which is the
		// same answer an unfitted lens already gets.
		const float near = lens.NearPlane > 0.0f ? lens.NearPlane : 1.0f;

		// **No second margin.** These extents already carry the widening the fit
		// put on them, and widening a widened frustum compounds once per level of
		// a recursion.
		int corner = 0;
		for (const float x : {lens.Left, lens.Right}) {
			for (const float y : {lens.Bottom, lens.Top}) {
				out[corner++] = frame.VectorToWorldSpace(Vector3{x / near, y / near, -1.0f});
			}
		}
		return 4;
	}

	MirrorEye
	ReflectCamera(const SurfacePane &pane, const CFrame &viewer, std::span<const Vector3> viewerCorners) {
		const Vector3 &unit = pane.Normal;
		const Vector3 at = viewer.Position;

		// Which side of the face the viewer is on, and how far off it. A pane can
		// be looked at from either side and both answers are right, so the sign
		// is the answer rather than a case to reject.
		const float distance = (at - pane.Centre).Dot(unit);

		// **Edge-on renders nothing.** See `EDGE_ON_MARGIN`: this is the one band
		// where there is no continuous orientation to aim for, and a pane the
		// viewer is level with covers no pixels anyway. The frame and the lens
		// are left at their defaults, so a caller that renders anyway gets a
		// finite matrix rather than a fit full of infinities.
		if (std::abs(distance) < EDGE_ON_MARGIN) {
			MirrorEye blank;
			blank.Lens.NearPlane = pane.NearPlane;
			blank.Lens.FarPlane = pane.FarPlane;
			return blank;
		}

		const float facing = distance >= 0.0f ? 1.0f : -1.0f;

		// **Mirrored through the plane.** The same distance behind the face as
		// the viewer is in front, on the other side - which is the whole of
		// planar reflection and is why the image lines up with the pane instead
		// of sliding across it as the viewer moves.
		//
		// **The mapped rectangle is the pane itself**, because a reflection fixes
		// every point of the plane it reflects through. That is why this looks
		// like it is not mapping anything: it is, and the map happens to be the
		// identity on exactly the four corners that matter - which is also why
		// `Placement::Map` is left identity here and `SurfaceLens::Mapping` comes
		// out identity with it.
		Placement placement;
		placement.Eye = at - unit * (2.0f * distance);
		placement.Centre = pane.Centre;
		placement.Forward = unit * facing;
		placement.First = pane.First;
		placement.Second = pane.Second;

		// **The same map applied to the viewer's frustum, and here the map is the
		// reflection rather than the identity.** A *direction* is not on the
		// pane's plane, so it has to be reflected properly or the clamp would cut
		// the wrong side of the image.
		//
		// **Four or none**, because `FitExtents` clamps against a whole frustum
		// or none of one: a partial set of corners is not a frustum, and taking
		// the ones that arrived would clamp the fit to a wedge of the screen.
		if (viewerCorners.size() == 4) {
			for (size_t corner = 0; corner < 4; corner++) {
				const Vector3 &direction = viewerCorners[corner];
				placement.EyeCorners[corner] = direction - unit * (2.0f * direction.Dot(unit));
			}
			placement.EyeCornerCount = 4;
		}

		// The aim, the fit, the clip and the map, which are the four steps a hole
		// takes as well - `Placement` is the whole of the difference between the
		// two, and `AimAt` is what keeps that true.
		return AimAt(placement, pane.NearPlane, pane.FarPlane);
	}

	size_t GatherSurfacePanes(Store &store, std::vector<SurfacePane> &panes) {
		ENGINE_PROFILE("gather surface panes");

		panes.clear();

		store.Each<const SurfaceCamera, const Camera, const Transform>(
			[&](Entity entity, const SurfaceCamera &target, const Camera &lens, const Transform &) {
				// A disabled portal is neither a recursive hole nor a mirror. Its
				// pane is left as ordinary geometry and its old slot is cleared by
				// `AimSurfaceCameras`.
				if (const Portal *portal = store.Get<Portal>(entity); portal != nullptr && !portal->Enabled) {
					return;
				}

				// **A linked portal is `GatherPortalSeams`' pane and not this
				// one.** Its camera is a warp rather than a reflection, so a
				// caller handed both descriptions would have two answers for
				// where one camera goes.
				if (LinkedPortalOf(store, entity) != nullptr) {
					return;
				}

				Face face;
				if (!FaceOf(store, entity, target.Face, face)) {
					// Parented to the world rather than to a part, which is the
					// script-authored arrangement: there is no face to reflect
					// through and the camera keeps whatever it was given.
					return;
				}

				SurfacePane pane;
				pane.Centre = face.Centre;
				pane.Normal = face.Normal;
				FaceAxes(face.Placement, NormalOf(target.Face), face.HalfExtent, pane.First, pane.Second);
				pane.Part = face.Part;
				pane.Camera = entity;
				pane.Surface = target.Surface;
				pane.TagFilter = target.TagFilter;
				pane.NearPlane = lens.NearPlane;
				pane.FarPlane = lens.FarPlane;

				panes.push_back(pane);
			}
		);

		return panes.size();
	}

	int32_t SurfaceLimitOf(const Store &store) {
		const SurfaceLimit *authored = store.Resource<SurfaceLimit>();

		// **Never below zero, whatever is in the resource.** The property setter
		// refuses a negative, and a snapshot is hostile input that did not go
		// through it - a negative here would reach a `size_t` comparison in the
		// renderer as an enormous positive.
		if (authored == nullptr) {
			return DEFAULT_SURFACE_LIMIT;
		}
		return authored->Panes > 0 ? authored->Panes : 0;
	}

	int32_t SurfaceBouncesOf(const Store &store) {
		const SurfaceBounces *authored = store.Resource<SurfaceBounces>();
		return authored == nullptr ? AUTOMATIC_SURFACE_BOUNCES : authored->Levels;
	}

	uint32_t NextSurfaceBounces(const SurfaceBounceProbe &measured, uint32_t ceiling) {
		// **Floored at one rather than at zero**, because a frame that drew no
		// surface at all has measured nothing about mirrors - the ordinary case
		// in a scene with none - and the next frame has to be allowed to draw
		// the first level to find out.
		const uint32_t top = std::max(ceiling, 1u);
		const uint32_t wanted = measured.Resolved + (measured.Deeper ? 1u : 0u);
		return std::clamp(wanted, 1u, top);
	}

	size_t GatherSurfaceSlots(Store &store, std::vector<SurfaceSlot> &slots) {
		ENGINE_PROFILE("gather surface slots");
		slots.clear();
		store.Each<const SurfaceCamera, const Camera, const Transform>(
			[&](Entity entity, const SurfaceCamera &surface, const Camera &, const Transform &) {
				Face face;
				if (FaceOf(store, entity, surface.Face, face)) {
					slots.push_back({.Camera = entity, .Part = face.Part});
				}
			}
		);
		std::sort(slots.begin(), slots.end(), [](const SurfaceSlot &left, const SurfaceSlot &right) {
			return left.Camera.Id < right.Camera.Id;
		});
		for (size_t index = 0; index < slots.size(); ++index) {
			slots[index].Index = index < MAX_SURFACES ? static_cast<int16_t>(index) : int16_t{-1};
		}
		return slots.size();
	}

	size_t AimSurfaceCameras(Store &store) {
		ENGINE_PROFILE("aim surface cameras");

		std::vector<Aim> &pending = PendingAims();
		pending.clear();

		// Where the scene is watched from. Without one there is nothing to
		// reflect: a mirror shows the viewer's world, so a world with no active
		// camera has no reflection to compute rather than a default one.
		//
		// **These two returns deliberately leave the panes on their slots**,
		// unlike the sweep at the end. A world with no viewer draws nothing, so
		// there is no picture for a pane to be stuck showing - and a replica
		// between connecting and spawning its camera is exactly this state while
		// the authority is still sending it `Visual` rows carrying the slot. A
		// clear here would fight the wire once per snapshot for a frame nobody
		// renders. `RenderView` refuses to sample a slot nothing claimed, which
		// is what covers a world that never gets a viewer back.
		const ActiveCamera *active = store.Resource<ActiveCamera>();
		if (active == nullptr || !store.Alive(active->Entity)) {
			return 0;
		}

		CFrame eyeFrame;
		if (!ResolveViewEye(store, active->Entity, eyeFrame)) {
			return 0;
		}
		const Vector3 eye = eyeFrame.Position;

		// **The viewer's own frustum, as four directions.** What a surface has
		// to cover is not the pane, it is the part of the pane the viewer can
		// see - and up against the glass those are wildly different: a pane
		// subtends nearly half a turn from a point on its own surface and a
		// screen subtends seventy degrees. Fitting the whole pane there spends
		// almost every texel outside the frame, which is exactly why a portal
		// went blocky as you walked into it. `FitExtents` intersects with these.
		//
		// **Taken once, outside the walk**, because they are the eye's and not
		// each pane's, and rebuilding them per surface would be four rotations
		// per mirror per frame for one answer.
		//
		// A world whose active camera carries no `Camera` - which a bare
		// `Transform` used as an eye is - leaves the count at zero and every fit
		// unclamped, which is what this did before.
		Vector3 eyeCorners[4];
		size_t eyeCornerCount = 0;

		if (const Camera *eyeLens = store.Get<Camera>(active->Entity)) {
			eyeCornerCount =
				FrustumCorners(eyeFrame, eyeLens->FieldOfViewRadians, active->AspectRatio, eyeCorners);
		}

		store.Each<const SurfaceCamera, const Camera, const Transform>(
			[&](Entity entity, const SurfaceCamera &target, const Camera &lens, const Transform &) {
				Face face;
				if (!FaceOf(store, entity, target.Face, face)) {
					return;
				}

				// A disabled mouth keeps its deterministic place in the surface
				// ordering, but renders nothing. Carrying a non-rendering aim is what
				// clears a texture slot left by the previous enabled frame.
				if (const Portal *portal = store.Get<Portal>(entity); portal != nullptr && !portal->Enabled) {
					Aim aim;
					aim.Camera = entity;
					aim.Part = face.Part;
					aim.Renders = false;
					pending.push_back(aim);
					return;
				}

				const Vector3 unit = face.Normal;
				const Vector3 centre = face.Centre;

				Vector3 first;
				Vector3 second;
				FaceAxes(face.Placement, NormalOf(target.Face), face.HalfExtent, first, second);

				// **The one branch that makes a portal a portal**, and it is
				// resolved before anything is placed because the two do not
				// merely map the pane differently - they answer the edge-on
				// question differently as well.
				const Portal *portal = LinkedPortalOf(store, entity);

				if (portal == nullptr) {
					// **A mirror, and what a mirror does to a camera is
					// `ReflectCamera`.** Where the camera stands, which way it
					// looks, the frustum fitted to the pane, the oblique clip and
					// the identity map all live there rather than here, for the
					// reason `SeamMapping` is the one statement of what a hole
					// does to what goes through it: a second derivation is a
					// second chance to disagree about a sign.
					//
					// **And because a rule buried in this walk can only ever
					// answer for the eye.** This pass places every surface in the
					// world from the world's one `ActiveCamera`, so a pane
					// appearing inside *another* pane's picture was placed and
					// sampled from the eye at every depth - which is
					// mirror-in-mirror-in-mirror drawing its inner panes as flat
					// tint. `render`'s recursive pass asks the same question of
					// the camera one level up, and could not while this was the
					// only place the answer existed.
					SurfacePane pane;
					pane.Centre = centre;
					pane.Normal = unit;
					pane.First = first;
					pane.Second = second;
					pane.Part = face.Part;
					pane.Camera = entity;
					pane.Surface = target.Surface;
					pane.TagFilter = target.TagFilter;
					pane.NearPlane = lens.NearPlane;
					pane.FarPlane = lens.FarPlane;

					const MirrorEye reflected =
						ReflectCamera(pane, eyeFrame, std::span<const Vector3>(eyeCorners, eyeCornerCount));

					// **A pane the viewer is level with is carried through as a
					// non-rendering aim rather than returned from**, because the
					// *pane* has to be told to stop sampling its slot: an aim
					// dropped on the floor would leave it showing the last frame
					// that was rendered into it. The camera is left exactly where
					// it was for the guarded-write reason below - there is
					// nothing to point it at, and writing the identity would be a
					// dirty mark per mirror per frame for a value nothing reads.
					Aim aim;
					aim.Camera = entity;
					aim.Part = face.Part;
					aim.Renders = reflected.Renders;
					if (reflected.Renders) {
						aim.Frame = reflected.Frame;
						aim.Lens = reflected.Lens;
					}

					pending.push_back(aim);
					return;
				}

				// **A linked portal is exempt from the edge-on band, and that is
				// the whole of it.** The discontinuity the band exists for is
				// `facing` changing sign with nothing else changing - true of a
				// mirror, whose map is the reflection through its own plane, and
				// false of a hole. A portal's camera is `through · eye`, and the
				// frame `facing` flips is the same frame `CrossPortals` - or
				// `PortalCrossing`, for the arm a third-person eye rides on -
				// carries the eye through the pane. The two flips cancel and the
				// placement is continuous across the crossing.
				//
				// Which matters because the band sits exactly where somebody
				// walking through a hole spends the crossing: 0.3 studs either
				// side of the plane, blanked, on the one frame they are in the
				// doorway. That is a picture going dark in the middle of the one
				// move a portal exists for, and it was a mirror's fix wearing a
				// hole's clothes.
				//
				// Nothing replaces it. `FitExtents` already floors the corner
				// depth and the frustum span, and `SurfaceProjection` already
				// declines to skew when the camera is on its own clip plane - the
				// three guards a fit needs at the plane are the ones that were
				// already written.
				//
				// Which way along the normal the viewer is, because a face can be
				// looked at from behind - the sign of the offset is exactly that
				// question.
				const float distance = (eye - centre).Dot(unit);
				const float facing = distance >= 0.0f ? 1.0f : -1.0f;

				Placement placement;

				// Scoped, so the far pane's working set cannot be read by the aim
				// below - which is shared with a mirror, and a mirror has no far
				// pane at all.
				{
					// The two face frames, each looking *out* of its own pane. A
					// `LookAt` builds its rotation against an up vector and cannot
					// when the two are parallel, which is a portal in the floor -
					// the same case `UpFor` answers for a mirror, for the same
					// reason and with the same answer.
					//
					// **Built from the pane's own normal and never from the
					// viewer's side of it**, which is the whole of what
					// `SeamMapping` settled at v0.15 and what this pass was still
					// getting wrong. A source frame that flips with the side is
					// two maps for one pane, and they are not each other's
					// inverse: both send a viewer to the *same* side of the far
					// pane. A same-world hole stopped noticing when the recursive
					// pass took its picture over - but a **cross-world** pane
					// still draws from here, and `ImmersivePortals.luau` spawns
					// you *behind* its pane, which is exactly the side the two
					// maps differ on.
					//
					// What that looked like: a window onto the other world whose
					// camera stood forty studs from where a body crossing would
					// land, on the wrong side, showing the half of the far room
					// with nothing in it. The floor is everywhere so the floor
					// arrived; the pad, the markers and the people did not.
					const Vector3 outward = unit * facing;
					const CFrame source = FaceFrame(face.Placement, centre, NormalOf(target.Face));

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
						FaceFrame(farPlacement->Frame, farCentre, NormalOf(target.Face));

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
					// more than four right angles - with no second mechanism and no
					// maths past this multiply. `docs/retired/NON-EUCLIDEAN.md` is the
					// investigation that settled it.
					//
					// **Built the same way a crossing body's is**, rather than
					// composed here: `SeamMapping` is the one statement of what a
					// hole does to what goes through it, and a second derivation of
					// it in the pass that draws the picture is a second chance for
					// the picture and the simulation to disagree about a sign or a
					// size. The scale is why that stopped being cosmetic - a hole
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
					// through the same map - so the three agree by construction
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
					// destination - which looks like a portal that works and
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
				}

				// **The same four steps a mirror takes, and the same code.** A
				// hole and a reflection differ in where the pane was mapped to and
				// in nothing after it, so the aim, the fit, the oblique clip and
				// the map the pane samples through are `AimAt` for both.
				const MirrorEye aimed = AimAt(placement, lens.NearPlane, lens.FarPlane);

				Aim aim;
				aim.Camera = entity;
				aim.Part = face.Part;
				aim.Frame = aimed.Frame;
				aim.Lens = aimed.Lens;

				pending.push_back(aim);
			}
		);

		// A request camera and the simulation camera use the same numbering.
		// Non-rendering aims keep their position so other panes never swap slots.
		static thread_local std::vector<SurfaceSlot> slots;
		GatherSurfaceSlots(store, slots);
		std::sort(pending.begin(), pending.end(), [](const Aim &left, const Aim &right) {
			return left.Camera.Id < right.Camera.Id;
		});
		size_t slot = 0;
		for (Aim &aim : pending) {
			while (slot < slots.size() && slots[slot].Camera.Id < aim.Camera.Id) {
				++slot;
			}
			aim.Surface = aim.Renders && slot < slots.size() && slots[slot].Camera == aim.Camera
							  ? slots[slot].Index
							  : int16_t{-1};
		}

		// **Every write is guarded on the value actually differing, and that is
		// not a micro-optimisation.** `Set` marks the row dirty and `GetMutable`
		// marks it by the act of handing out the pointer - it says so itself. So
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
			// frame's did" - the same inputs through the same code give the same
			// bits, so an exact compare answers exactly that. A tolerance would
			// be answering a different question and would let a slow drift
			// accumulate unreported.
			// **A non-rendering aim leaves the camera exactly where it was.**
			// There is nothing to point it at, and writing the identity or the
			// eye's own frame would be a dirty mark per mirror per frame for a
			// value nothing reads - the same argument the guarded writes below
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
			// placement is - so it goes in `SurfaceLens`, which exists for it.
			//
			// **`Camera::FieldOfView` and `Camera::NearPlane` are no longer
			// overwritten**, which is a change an author can see. They used to
			// be, because there was nowhere else to put a fitted angle and a
			// near plane pushed out to the glass; a script that set either on a
			// parented camera had it taken back on the next frame. Now the near
			// and far planes are read as authored and the fit lives beside them,
			// so setting `FieldOfView` on a surface camera does nothing rather
			// than being reverted - the honest outcome for a field the surface
			// path no longer consults.
			if (aim.Renders) {
				const SurfaceLens *existing = store.Get<SurfaceLens>(aim.Camera);
				if (existing == nullptr || std::memcmp(existing, &aim.Lens, sizeof(SurfaceLens)) != 0) {
					store.Set(aim.Camera, aim.Lens);
				}
			}

			// **Both ends of the pairing, written from one number.** The camera
			// carries the slot it renders into - `client::CollectSurfaceViews`
			// reads it to fill `render::SurfaceView::Index` - and the pane
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

		ReleaseUnaimedSurfaces(store, pending);

		// **What rendered, not what was walked.** A pane the viewer is level with
		// has been visited and told to stop sampling, which is work - but the
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
				if (const Portal *portal = store.Get<Portal>(entity); portal != nullptr && !portal->Enabled) {
					return;
				}

				Face face;
				if (!FaceOf(store, entity, target.Face, face)) {
					return;
				}

				DrawInstance marker;
				marker.Source = entity.Id;
				marker.Variant = entity.Id;

				// **The part's rotation with the face's position**, so the bar
				// lies in the plane of the face rather than axis-aligned beside
				// it. `MarkerExtent` is written in the part's own axes for the
				// same reason: a half-extent means nothing without the frame it
				// is measured in, and taking both from the part is what keeps a
				// tilted pane's marker tilted with it.
				//
				// Pushed a thickness clear of the glass, because a marker
				// exactly on the surface z-fights it - the same margin, for the
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
				// at, and it has to be *blended* so the surface pass - which
				// draws only the opaque head - never puts it inside a mirror.
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
}
