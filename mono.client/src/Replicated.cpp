#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Components.hpp>
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

					// Preserve every replicated visual field.
					drawList->Instances.push_back(
						DrawInstance{
							interpolated.value_or(transform.Frame),
							bounds.HalfExtent,
							visual.Tint,
							visual.Mesh,
							appearance != nullptr ? appearance->ColourMap : Name(),
							appearance != nullptr ? appearance->NormalMap : Name(),
							appearance != nullptr ? appearance->RoughnessMap : Name(),
							appearance != nullptr ? appearance->OcclusionMap : Name(),
							appearance != nullptr ? appearance->EmissiveMap : Name(),
							tags != nullptr ? tags->Mask : 0u,
							visual.Transparency,
							visual.Surface,
							visual.CastShadow,
							appearance != nullptr ? appearance->Mode : AlphaMode::Opaque,
						}
					);
				}
			);

			engine::core::Metrics::Count(
				"replica.instances", static_cast<double>(drawList->Instances.size())
			);

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

		// PreRender derives draw data and mirror aim; the replica does not simulate.
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
