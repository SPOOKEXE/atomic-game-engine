#include <engine/core/Profiling.hpp>
#include <engine/core/types/Color3.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Components.hpp>
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
			float NearPlane = 0.0f;
			float FieldOfView = 0.0f;
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

		// The widest frustum worth building, in radians — a whisker under 180°.
		//
		// **A limit of the projection rather than a policy.** A pane subtends
		// half a turn from a point on its own surface, so as the viewer walks
		// into the glass the frustum needed to cover it approaches 180° and its
		// tangent approaches infinity. There is no field of view that covers a
		// plane you are standing on, and clamping is what turns that into a
		// reflection that stops covering the far corners instead of a projection
		// matrix full of infinities. An oblique frustum does not escape this
		// either; it is the geometry, not the parameterisation.
		constexpr float FIT_MAXIMUM = 3.0f;

		// The closest a corner may be treated as being, in studs.
		//
		// **A floor rather than a rejection**, for the reason `FitFieldOfView`
		// gives at the clamp: a corner at zero depth needs an infinite frustum,
		// and the useful answer is "the widest one" arrived at *continuously*.
		// Small enough that a corner this close is already asking for more than
		// `FIT_MAXIMUM` allows, so the floor never changes an answer that was
		// not already saturated.
		constexpr float MINIMUM_DEPTH = 1e-3f;

		// And the narrowest, so a distant pane still has a frustum with a shape.
		constexpr float FIT_MINIMUM = 0.02f;

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

		// The vertical field of view that just covers a pane, in radians.
		//
		// **The bug this exists for, because it is not obvious from a still
		// frame.** The reflection is projected back onto the pane per fragment
		// and `opaque.frag` tests the projected coordinate against the texture's
		// 0..1 rectangle, falling back to the plain lit pane outside it. So a
		// frustum that does not cover the whole pane does not stretch or fade —
		// it draws a hard-edged rectangle of reflection floating on a grey wall.
		//
		// A fixed field of view cannot cover it, and the reason is the whole of
		// planar reflection: the camera stands as far behind the glass as the
		// viewer stands in front, so the pane subtends *the same angle from the
		// camera as it does from the viewer*. Walk towards a mirror and that
		// angle grows without bound. `Mirrors-1-world.luau` authored 70° with a
		// comment saying it was "wide enough to still cover the pane when the
		// viewer walks up to it", which is exactly the thing no constant can be:
		// at 48 units back the north wall needs 24°, and at 5 units it needs 127°.
		//
		// So it is fitted, every frame, to the four corners.
		//
		// **This needs the camera to look along the face normal, and the caller
		// does.** Every point of the pane is then at the same depth, so each
		// corner's constraint is finite and none can fall behind the camera. An
		// aim at the pane's centre puts the near corner behind the camera as soon
		// as the viewer is close and off to one side, and no angle covers that.
		//
		// The frustum is symmetric about the axis, so a viewer off to one side
		// pays for the far edge on both sides — an off-axis frustum would fit the
		// same corners with none of that waste, and is the better shape once
		// `SurfaceView` can carry a rectangle rather than a field of view.
		//
		// @param frame  Where the camera stands and which way it faces.
		// @param centre The middle of the pane.
		// @param first  One half-axis of the pane, in the world.
		// @param second The other.
		// @param aspect Width over height of the texture it renders into.
		// @return A vertical field of view in radians, clamped to something a
		//         projection can be built from.
		float FitFieldOfView(
			const CFrame &frame,
			const Vector3 &centre,
			const Vector3 &first,
			const Vector3 &second,
			float aspect
		) {
			// A texture with no height would divide the horizontal constraint by
			// zero and hand back a frustum of infinities.
			if (!(aspect > 0.0f)) {
				return FIT_MAXIMUM;
			}

			const Vector3 eye = frame.Position;
			const Vector3 forward = frame.LookVector();
			const Vector3 right = frame.RightVector();
			const Vector3 up = frame.UpVector();

			float tangent = 0.0f;

			for (int alongFirst = -1; alongFirst <= 1; alongFirst += 2) {
				for (int alongSecond = -1; alongSecond <= 1; alongSecond += 2) {
					const Vector3 corner = centre + first * static_cast<float>(alongFirst) +
										   second * static_cast<float>(alongSecond);
					const Vector3 toCorner = corner - eye;

					// **Level with the camera or behind it**, which happens as
					// the viewer approaches the plane of the pane: the reflected
					// camera approaches it too, and a corner at ninety degrees is
					// the 180° case arriving. Nothing finite covers that, so the
					// widest frustum is the answer.
					//
					// **Clamped rather than returned early, and that is the whole
					// of a bug that read as the mirror flashing.** This used to
					// bail to `FIT_MAXIMUM` the instant one corner reached zero
					// depth, which makes the fit a *step function*: orbiting the
					// camera at a constant distance sweeps the reflected camera
					// toward the pane's plane, a corner crosses the threshold,
					// and the field of view jumps from a fitted half-radian to
					// 172° between one frame and the next — then back. The
					// projection was never wrong; the fit was discontinuous, and
					// a discontinuity once per orbit is exactly what a flash is.
					//
					// A floor on the depth is continuous instead: as a corner
					// approaches the plane the tangent grows without bound and
					// the clamp at the bottom of this function saturates it at
					// `FIT_MAXIMUM` smoothly. A corner genuinely *behind* the
					// camera has a negative depth and lands on the same floor,
					// so it reaches the same answer from the same side rather
					// than by a different path.
					const float depth = std::max(toCorner.Dot(forward), MINIMUM_DEPTH);

					// The vertical constraint directly, and the horizontal one
					// divided by the aspect — because `ResolveCamera` builds a
					// projection from a *vertical* field of view and widens it by
					// the aspect, so `tan(h/2) = aspect * tan(v/2)` and a corner
					// off to the side asks for proportionally less of the vertical
					// angle than one above.
					tangent = std::max(tangent, std::abs(toCorner.Dot(up)) / depth);
					tangent = std::max(tangent, std::abs(toCorner.Dot(right)) / depth / aspect);
				}
			}

			return std::clamp(2.0f * std::atan(tangent * FIT_MARGIN), FIT_MINIMUM, FIT_MAXIMUM);
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
			[&](Entity entity, const SurfaceCamera &target, const Camera &, const Transform &) {
				Face face;
				if (!FaceOf(store, entity, target.Face, face)) {
					return;
				}

				const Vector3 unit = face.Normal;
				const Vector3 centre = face.Centre;

				// **Mirrored through the plane.** The same distance behind the
				// face as the eye is in front, on the other side — which is the
				// whole of planar reflection and is why the image lines up with
				// the pane instead of sliding across it as the viewer moves.
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

				const Vector3 reflected = eye - unit * (2.0f * distance);

				// **Aimed, not merely placed.** An identity rotation looks down
				// -Z, so a camera put behind a pane faces away from it and
				// renders empty space. That was the first version of the script
				// this replaces, and the mirror came out showing the clear
				// colour.
				//
				// **Square on to the pane, and not at its centre.** Aiming at the
				// middle sounds like the same thing and is not: it tilts the view
				// axis off the face normal by however far the viewer stands to one
				// side, and the pane then lies at an angle across the frustum.
				// Push that far enough — close to the glass and off to the side,
				// which is a metre from a wall in a room — and the nearest corner
				// of the pane goes *behind* the camera. Nothing covers a point
				// behind a camera, so no field of view could rescue it, and the
				// corner drew as bare wall.
				//
				// Looking along the normal puts every point of the pane at the
				// same depth, so the fit below is always finite and the corners
				// are always in front. It costs nothing in correctness: the image
				// is read back by projecting each fragment through this camera's
				// own matrix, so the orientation decides which texels the pane
				// lands on and never which part of the world it shows. The
				// *position* is what makes it a reflection, and that is unchanged.
				//
				// Which way along the normal follows from which side the viewer
				// is on, because a face can be looked at from behind — the sign of
				// `distance` is exactly that question, already answered.
				const float facing = distance >= 0.0f ? 1.0f : -1.0f;
				const Vector3 forward = unit * facing;

				// **A different up for a floor or a ceiling.** `LookAt` builds its
				// rotation against an up vector and cannot when the two are
				// parallel — a mirror in the floor faces straight up, which is the
				// one case the default cannot resolve, and it produces a NaN
				// rotation that spreads into the frame, the near plane and every
				// bound derived from them.
				const Vector3 up =
					std::abs(forward.Dot(Vector3::YAxis)) > 0.99f ? Vector3::ZAxis : Vector3::YAxis;

				Aim aim;
				aim.Camera = entity;
				aim.Part = face.Part;
				aim.Frame = CFrame::LookAt(reflected, reflected + forward, up);

				// The near plane at the glass, which is the poor-man's oblique
				// clip: everything between the reflected camera and the pane
				// would otherwise occlude the reflection. A small margin,
				// because a near plane exactly on the surface z-fights it.
				aim.NearPlane = std::abs(distance) + 0.3f;

				// **And a frustum fitted to the pane, because a constant one
				// cannot be.** See `FitFieldOfView`: the reflection is projected
				// back per fragment and clipped to the texture's rectangle, so a
				// frustum narrower than the pane draws a hard-edged rectangle of
				// reflection on a grey wall rather than a smaller or softer image.
				Vector3 first;
				Vector3 second;
				FaceAxes(face.Placement, NormalOf(target.Face), face.HalfExtent, first, second);

				// The texture's shape, which is what `client::CollectSurfaceViews`
				// hands the renderer and therefore what the projection is widened
				// by. Taking it from anywhere else would fit a frustum to a
				// rectangle nothing renders into.
				const float aspect = static_cast<float>(target.Width) /
									 static_cast<float>(std::max<uint16_t>(target.Height, 1));

				aim.FieldOfView = FitFieldOfView(aim.Frame, centre, first, second, aspect);

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

			// **Both clip and field of view, and both are the engine's now.** A
			// surface camera parented to a part has its placement written here
			// every frame; the frustum that placement implies is no more the
			// author's to choose than the placement is. A `FieldOfView` set by a
			// script is honoured on a camera parented to the world, which is the
			// same line `Transform` is already drawn on.
			if (const Camera *lens = store.Get<Camera>(aim.Camera); aim.Renders && lens != nullptr) {
				if (lens->NearPlane != aim.NearPlane) {
					store.GetMutable<Camera>(aim.Camera)->NearPlane = aim.NearPlane;
				}
				if (lens->FieldOfViewRadians != aim.FieldOfView) {
					store.GetMutable<Camera>(aim.Camera)->FieldOfViewRadians = aim.FieldOfView;
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
}
