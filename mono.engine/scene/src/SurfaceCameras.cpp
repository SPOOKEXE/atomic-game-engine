#include <engine/core/Profiling.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Enums.hpp>
#include <engine/scene/SurfaceCameras.hpp>

#include <cmath>
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
		// **Half, not the full size, and the distinction has bitten this
		// repository once already.** `Bounds::HalfExtent` is half of a full
		// extent — the whole reason `Size` is a conversion rather than a member
		// pointer — so a plane placed at the full extent sits a whole part
		// outside the part it belongs to, and the reflection lands nowhere near
		// the pane.
		float ReachOf(const Bounds &bounds, NormalId face) {
			switch (face) {
			case NormalId::Right:
			case NormalId::Left:
				return bounds.HalfExtent.X;
			case NormalId::Top:
			case NormalId::Bottom:
				return bounds.HalfExtent.Y;
			case NormalId::Back:
			case NormalId::Front:
				return bounds.HalfExtent.Z;
			}
			return bounds.HalfExtent.Z;
		}

		// The cameras to place, collected before anything is written.
		//
		// **Collected first because placing one is a write and the walk is a
		// read.** `Set<Transform>` may move the row the walk is standing on, and
		// setting the parent's `Visual::Surface` can move *its* row too — an
		// iteration that wrote as it went would be walking a table moving
		// underneath it. Same argument `Store::FlushSignals` makes about
		// collecting the changed set before firing.
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

				// The face, rotated into the world. `CFrame` composition rather
				// than a matrix by hand, so a rotated pane reflects along the
				// direction it actually faces.
				const CFrame &frame = placement->Frame;
				const Vector3 local = NormalOf(target.Face);
				const Vector3 normal = (frame * CFrame(local)).Position - frame.Position;

				const float length = std::sqrt(normal.Dot(normal));
				if (length < 1e-6f) {
					return;
				}
				const Vector3 unit = normal * (1.0f / length);

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

		for (const Aim &aim : pending) {
			store.Set(aim.Camera, Transform{aim.Frame});

			if (Camera *lens = store.GetMutable<Camera>(aim.Camera); lens != nullptr) {
				lens->NearPlane = aim.NearPlane;
			}

			// **The part is told what it shows, so a mirror is a camera parented
			// to a part and nothing else.** Requiring `Surface` to be set by hand
			// as well is one fact recorded twice, and its failure mode is a
			// camera rendering perfectly into a texture nothing samples — which
			// looks exactly like a mirror that does not work.
			if (Visual *visual = store.GetMutable<Visual>(aim.Part); visual != nullptr) {
				visual->Surface = aim.Surface;
			}
		}

		return pending.size();
	}
}
