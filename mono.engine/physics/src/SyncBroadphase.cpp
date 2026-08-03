#include "PipelineInternals.hpp"
#include "WorldResource.hpp"

#include <engine/core/FrameGraph.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/physics/Broadphase.hpp>
#include <engine/physics/PhysicsWorld.hpp>
#include <engine/physics/Shapes.hpp>
#include <engine/scene/Components.hpp>
#include <engine/spatial/HashGrid.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace engine::physics {

	namespace {
		// Appends one collider's proxy and record, in the order rows are
		// visited.
		//
		// `Proxy::Id` is the index the two arrays share, not the entity — see
		// `PhysicsWorld`, which explains why. The entity is on the record, so
		// resolving a candidate is a subscript and never a store lookup.
		void Append(
			std::vector<spatial::Proxy> &proxies,
			std::vector<ColliderRecord> &records,
			ecs::Entity entity,
			const scene::Transform &transform,
			const scene::Collider &collider
		) {
			proxies.push_back(
				spatial::Proxy{
					static_cast<uint64_t>(proxies.size()),
					ShapeWorldBounds(collider, transform.Frame),
					collider.Layer,
				}
			);
			records.push_back(ColliderRecord{entity, collider.Layer, collider.Mask});
		}

		// Whether a collider that cannot move has been created, destroyed or
		// written since the static index was last built.
		//
		// Two gates, and the outer one is what makes this cheap. `Changes` only
		// moves for a write through `Set` to an observed component, so an
		// unchanged counter means nothing authored has happened and there is
		// nothing to walk. The inner gate is the walk itself, which asks which
		// of the changed rows were static ones.
		//
		// **A consumer calling `Store::MarkAllChanged<Transform>` every tick
		// defeats the outer gate**, because that claim covers the anchored rows
		// too. The static index then rebuilds every tick: correct, and the cost
		// the second index exists to avoid. It is why `IntegrateMotion` does not
		// make that claim.
		bool StaticSetChanged(ecs::Store &store, PhysicsWorld &world, size_t staticCount) {
			if (PipelineInternals::StaticStale(world)) {
				return true;
			}
			if (staticCount != PipelineInternals::StaticRecords(world).size()) {
				return true;
			}

			const uint64_t version = store.ChangeVersion();
			if (version == PipelineInternals::StaticChangeVersion(world)) {
				return false;
			}
			PipelineInternals::StaticChangeVersion(world) = version;

			// A row with a `Motion` is in the other index and its change is
			// nothing to do with this one.
			bool moved = false;
			store.EachChanged<scene::Transform>([&store, &moved](ecs::Entity entity, scene::Transform &) {
				moved = moved || !store.Has<scene::Motion>(entity);
			});
			if (moved) {
				return true;
			}

			store.EachChanged<scene::Collider>([&store, &moved](ecs::Entity entity, scene::Collider &) {
				moved = moved || !store.Has<scene::Motion>(entity);
			});
			return moved;
		}
	}

	void SyncBroadphase(ecs::Store &store) {
		ENGINE_PROFILE_CAT("physics.sync-broadphase", core::ProfileCategory::ECS);

		PhysicsWorld *world = PreparedWorldMutable(store);
		if (world == nullptr) {
			return;
		}

		// Rebuilt whole, every tick. `spatial::HashGrid` is count-then-fill and
		// has no `Insert`; "only re-insert what moved" is therefore a decision
		// about which set to hand to `Rebuild`, and this is the set that moved.
		std::vector<spatial::Proxy> &dynamicProxies = PipelineInternals::DynamicProxies(*world);
		std::vector<ColliderRecord> &dynamicRecords = PipelineInternals::DynamicRecords(*world);
		dynamicProxies.clear();
		dynamicRecords.clear();

		store.Each<const scene::Transform, const scene::Collider, const scene::Motion>(
			[&dynamicProxies, &dynamicRecords](
				ecs::Entity entity,
				const scene::Transform &transform,
				const scene::Collider &collider,
				const scene::Motion &
			) { Append(dynamicProxies, dynamicRecords, entity, transform, collider); }
		);

		PipelineInternals::DynamicIndex(*world).Rebuild(dynamicProxies);
		PipelineInternals::DynamicRebuildCount(*world)++;

		// Every collider, minus the ones that just went into the dynamic index.
		// `ecs::Store` has no "without this component" query term, so the count
		// is a subtraction and the pass below asks per row — which is affordable
		// exactly because it is not a per-tick pass.
		const size_t colliders = store.CountMatching<scene::Transform, scene::Collider>();
		const size_t staticCount = colliders - dynamicRecords.size();

		if (!StaticSetChanged(store, *world, staticCount)) {
			return;
		}

		std::vector<spatial::Proxy> &staticProxies = PipelineInternals::StaticProxies(*world);
		std::vector<ColliderRecord> &staticRecords = PipelineInternals::StaticRecords(*world);
		staticProxies.clear();
		staticRecords.clear();

		store.Each<const scene::Transform, const scene::Collider>(
			[&store, &staticProxies, &staticRecords](
				ecs::Entity entity, const scene::Transform &transform, const scene::Collider &collider
			) {
				// The per-row question the ECS cannot express as a query term.
				// It costs a sparse-set lookup per collider, and it is paid when
				// the static set changes rather than once a tick.
				if (store.Has<scene::Motion>(entity)) {
					return;
				}
				Append(staticProxies, staticRecords, entity, transform, collider);
			}
		);

		PipelineInternals::StaticIndex(*world).Rebuild(staticProxies);
		PipelineInternals::StaticRebuildCount(*world)++;
		PipelineInternals::StaticStale(*world) = false;
		PipelineInternals::StaticChangeVersion(*world) = store.ChangeVersion();
	}
}
