#include <engine/core/Profiling.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Enums.hpp>
#include <engine/scene/SurfaceCameras.hpp>

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
			int8_t Surface = 0;
		};

		std::vector<Aim> &Pending() {
			static thread_local std::vector<Aim> pending;
			return pending;
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
				const Entity parent = store.ParentOf(entity);
				if (parent == NULL_ENTITY) {
					// Parented to the world rather than to a part. Left exactly
					// where it was put, which is the script-authored arrangement
					// and still a legal way to build a mirror.
					return;
				}

				const Transform *placement = store.Get<Transform>(parent);
				const Bounds *bounds = store.Get<Bounds>(parent);
				if (placement == nullptr || bounds == nullptr) {
					// A parent that is not a part in space — a `Model`, a
					// service, a folder. There is no face to project off, so
					// this is not an error either.
					return;
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
				const Vector3 unit = frame.VectorToWorldSpace(NormalOf(target.Face));

				// The middle of the face: the part's centre pushed out to the
				// surface along that normal.
				const Vector3 centre = frame.Position + unit * ReachOf(*bounds, target.Face);

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
				aim.Part = parent;
				aim.Frame = CFrame::LookAt(reflected, centre);
				aim.Surface = target.Surface;

				// The near plane at the glass, which is the poor-man's oblique
				// clip: everything between the reflected camera and the pane
				// would otherwise occlude the reflection. A small margin,
				// because a near plane exactly on the surface z-fights it.
				aim.NearPlane = std::abs(distance) + 0.3f;

				pending.push_back(aim);
			}
		);

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

			// **The part is told what it shows, so a mirror is a camera parented
			// to a part and nothing else.** Requiring `Surface` to be set by hand
			// as well is one fact recorded twice, and its failure mode is a
			// camera rendering perfectly into a texture nothing samples — which
			// looks exactly like a mirror that does not work.
			if (const Visual *visual = store.Get<Visual>(aim.Part);
				visual != nullptr && visual->Surface != aim.Surface) {
				store.GetMutable<Visual>(aim.Part)->Surface = aim.Surface;
			}
		}

		return pending.size();
	}
}
