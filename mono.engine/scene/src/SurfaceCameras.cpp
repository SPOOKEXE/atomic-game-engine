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
			// Handed out after the walk, by entity id. -1 is a camera past the
			// renderer's cap, whose pane is left as an ordinary part.
			int8_t Surface = -1;
		};

		std::vector<Aim> &Pending() {
			static thread_local std::vector<Aim> pending;
			return pending;
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
				const Vector3 reflected = eye - unit * (2.0f * distance);

				// **Aimed, not merely placed.** An identity rotation looks down
				// -Z, so a camera put behind a pane faces away from it and
				// renders empty space. That was the first version of the script
				// this replaces, and the mirror came out showing the clear
				// colour.
				Aim aim;
				aim.Camera = entity;
				aim.Part = face.Part;
				aim.Frame = CFrame::LookAt(reflected, centre);

				// The near plane at the glass, which is the poor-man's oblique
				// clip: everything between the reflected camera and the pane
				// would otherwise occlude the reflection. A small margin,
				// because a near plane exactly on the surface z-fights it.
				aim.NearPlane = std::abs(distance) + 0.3f;

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
			pending[index].Surface = index < MAX_SURFACES ? static_cast<int8_t>(index) : int8_t{-1};
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
			const Transform *placed = store.Get<Transform>(aim.Camera);
			if (placed == nullptr || std::memcmp(&placed->Frame, &aim.Frame, sizeof(CFrame)) != 0) {
				store.Set(aim.Camera, Transform{aim.Frame});
			}

			if (const Camera *lens = store.Get<Camera>(aim.Camera);
				lens != nullptr && lens->NearPlane != aim.NearPlane) {
				store.GetMutable<Camera>(aim.Camera)->NearPlane = aim.NearPlane;
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

		return pending.size();
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
