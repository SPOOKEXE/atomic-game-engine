#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Controls.hpp>
#include <engine/scene/Input.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/SurfaceCameras.hpp>

#include <client/Replicated.hpp>
#include <client/Scene.hpp>
#include <cstring>
#include <optional>

namespace client {

	using engine::core::CFrame;
	using engine::core::Name;
	using engine::ecs::Entity;
	using engine::ecs::Phase;
	using engine::ecs::Scheduler;
	using engine::ecs::Store;
	using engine::replication::InterpolationSettings;
	using engine::replication::SnapshotBuffer;
	using engine::scene::AlphaMode;
	using engine::scene::Bounds;
	using engine::scene::DrawInstance;
	using engine::scene::SurfaceAppearance;
	using engine::scene::Tags;
	using engine::scene::Transform;
	using engine::scene::Visual;

	namespace {
		// Derives render poses from received ticks; interpolated poses never enter ECS.
		// Surface cameras are aimed from this client's viewer.
		void AimReplicatedSurfaces(Store &store) {
			(void)engine::scene::AimSurfaceCameras(store);
		}

		void CollectReplicated(Store &store) {
			auto *drawList = store.ResourceMutable<DrawList>();
			auto *buffer = store.ResourceMutable<SnapshotBuffer>();
			if (drawList == nullptr || buffer == nullptr) {
				return;
			}

			// Advance with the world's frame delta so stalls remain testable.
			buffer->Advance(store.Time().FrameDelta);

			// Entity joins are required here; retain draw-list capacity.
			drawList->Instances.clear();
			drawList->Instances.reserve(store.CountMatching<Transform, Bounds, Visual>());

			// The authority owns ancestry filtering; the replica only honors `Visible`.
			// Optional appearance and tag components must not be query requirements.
			store.Each<const Transform, const Bounds, const Visual>(
				[drawList, buffer, &store](
					Entity entity, const Transform &transform, const Bounds &bounds, const Visual &visual
				) {
					if (!visual.Visible) {
						return;
					}

					const std::optional<CFrame> interpolated = buffer->Sample(entity);

					const SurfaceAppearance *appearance = store.Get<SurfaceAppearance>(entity);
					const Tags *tags = store.Get<Tags>(entity);

					// Every replicated visual field, through the builder both
					// collectors share — see `scene::MakeDrawInstance`. The
					// optional components stay optional here: a replicated row
					// may arrive without an appearance, which is the difference
					// from the local collector that made these two drift.
					drawList->Instances.push_back(
						engine::scene::MakeDrawInstance(
							interpolated.value_or(transform.Frame), bounds, visual, appearance, tags
						)
					);
				}
			);

			engine::core::Metrics::Count(
				"replica.instances", static_cast<double>(drawList->Instances.size())
			);

			// **A client sees itself in the hole too, and this is where.** The
			// ghost is built from the list above, which holds interpolated
			// frames — the ones this machine actually draws — so the far half of
			// a body lines up with the near half rather than trailing it by
			// however far the character walked since the last tick. After the
			// metric for the reason `client::CollectInstances` gives.
			(void)engine::scene::CutAndCloneSeams(store, drawList->Instances);

			engine::core::Metrics::Count("replica.behind.ticks", buffer->Behind());
			engine::core::Metrics::Count("replica.stalls", static_cast<double>(buffer->Stats().Stalls));

			engine::core::Metrics::Count("replica.tickrate", buffer->MeasuredTickRate());
		}
	}

	void
	BuildReplicatedWorld(Store &store, Scheduler &scheduler, const InterpolationSettings &interpolation) {
		// Register snapshot component names before applying one.
		engine::scene::RegisterSceneComponents();

		// Register client resources before their component ids are minted.
		RegisterClientComponents();

		// **And the replication module's own, which nothing was doing.** A
		// `SnapshotBuffer` is a resource, a resource is keyed by a component id,
		// and one minted from the compiler's spelling is a world `Store::Save`
		// refuses — so a replica could not be snapshotted, which is what the
		// studio does every time Play is pressed. `client::DrawList` two lines
		// down is the same fix for the same reason, one version earlier.
		engine::replication::RegisterReplicationComponents();

		store.SetResource(DrawList{});

		// Per-world state belongs in the store.
		store.SetResource(SnapshotBuffer{interpolation});

		// **The two resources that make a replica somewhere a player stands
		// rather than somewhere they watch.** Both are on
		// `replication::LocalToTheClient`'s list, so nothing arriving from the
		// server ever overwrites them — which is precisely what makes it safe
		// to keep this machine's keyboard and this machine's camera in a world
		// whose every other row is somebody else's answer.
		store.SetResource(engine::scene::InputState{});
		store.SetResource(engine::scene::CameraController{});

		// PreRender derives draw data and mirror aim; the replica does not simulate.
		//
		// **The camera is the one thing here that is driven and not derived**,
		// and it is not a simulation: turning the view moves no row the server
		// owns. `FollowOwnCharacter` between the two halves is what points it at
		// the body that arrived over the wire — a client never calls
		// `LoadCharacter`, so there is no spawn moment for it to hook.
		scheduler.Add("replica-camera", Phase::PreRender, [](Store &store) {
			(void)engine::scene::UpdateCameraControl(store);
			(void)engine::scene::FollowOwnCharacter(store);
			(void)engine::scene::PlaceCamera(store);
		});

		// **Posed here and never stepped here.** A character's limbs hang off a
		// root the *server* moved and this machine interpolated, so the product
		// that places them has to run wherever the picture is made. The step and
		// the ground query deliberately do not: this world simulates nothing.
		scheduler.Add("pose-characters", Phase::PreRender, [](Store &store) {
			(void)engine::scene::PoseCharacters(store);
		});

		scheduler.Add("aim-surface-cameras", Phase::PreRender, AimReplicatedSurfaces);
		scheduler.Add("collect-replicated", Phase::PreRender, CollectReplicated);
	}

	Entity AimReplicaViewer(Store &store, const CFrame &frame, const engine::scene::Camera &lens) {
		const auto *active = store.Resource<engine::scene::ActiveCamera>();
		Entity camera = active != nullptr ? active->Entity : engine::ecs::NULL_ENTITY;

		if (camera == engine::ecs::NULL_ENTITY || !store.Alive(camera)) {
			// **Predicted, not authoritative.** The high range is the client's
			// own and the authority never allocates from it, so this camera
			// cannot become the same entity as something the server made.
			camera = store.CreatePredicted("ReplicaViewer");
			if (camera == engine::ecs::NULL_ENTITY) {
				return camera;
			}

			store.Set(camera, engine::scene::Transform{frame});
			store.Set(camera, lens);

			engine::scene::ActiveCamera live;
			live.Entity = camera;
			store.SetResource(live);
			return camera;
		}

		// **A replica with a body of its own places its own camera**, and this
		// must not fight it. `BuildReplicatedWorld` installs `replica-camera`,
		// which turns with the mouse and sits behind the character the server
		// gave this client; the frame passed in is where the *local* world is
		// looking, which is the right answer only while there is nothing here to
		// look at. Two writers and the last one wins, so the condition is stated
		// rather than left to phase order.
		if (const auto *controller = store.Resource<engine::scene::CameraController>();
			controller != nullptr && store.Alive(controller->Subject)) {
			return camera;
		}

		// Guarded on the value differing, for `AimSurfaceCameras`' reason: a
		// `Set` marks the row dirty, and a viewer that has not moved is not a
		// write. A replica observes nothing today and that is not a reason to
		// emit changes it would have to.
		if (const auto *placement = store.Get<engine::scene::Transform>(camera);
			placement == nullptr || std::memcmp(&placement->Frame, &frame, sizeof(CFrame)) != 0) {
			store.Set(camera, engine::scene::Transform{frame});
		}

		if (const auto *current = store.Get<engine::scene::Camera>(camera);
			current == nullptr || current->FieldOfViewRadians != lens.FieldOfViewRadians ||
			current->NearPlane != lens.NearPlane || current->FarPlane != lens.FarPlane) {
			store.Set(camera, lens);
		}

		return camera;
	}

	void RecordReplicatedTick(Store &store, uint64_t tick) {
		auto *buffer = store.ResourceMutable<SnapshotBuffer>();
		if (buffer == nullptr || tick == 0 || buffer->Holds(tick)) {
			return;
		}

		store.Each<const Transform>([buffer, tick](Entity entity, const Transform &transform) {
			buffer->Record(tick, entity, transform.Frame);
		});
	}
}
