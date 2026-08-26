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
#include <engine/scene/CollisionShapes.hpp>
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
		// `Proxy::Id` is the index the two arrays share, not the entity - see
		// `PhysicsWorld`, which explains why. The entity is on the record, so
		// resolving a candidate is a subscript and never a store lookup.
		void AppendEntry(
			std::vector<spatial::Proxy> &proxies,
			std::vector<ColliderRecord> &records,
			std::vector<PlacedCollider> &shapes,
			const scene::CollisionShapes *baked,
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

			// **The placed shape, resolved here because this is where the two
			// components are already in hand.** The narrow phase used to do this
			// once per *pair* - twenty-five thousand times for ten thousand
			// colliders - and every one of those was a re-read of the
			// `Transform` this loop has just read. See `PlacedCollider`, which
			// carries the measurement.
			//
			// The table is looked up only for the kinds that name one, so a
			// world of boxes pays nothing for a feature it does not use.
			const collision::ConvexHull *hull = nullptr;
			const collision::TriangleMesh *mesh = nullptr;
			if (baked != nullptr) {
				if (collider.Shape == scene::ShapeKind::Hull) {
					hull = baked->FindHull(collider.Geometry);
				} else if (collider.Shape == scene::ShapeKind::Mesh) {
					mesh = baked->FindMesh(collider.Geometry);
				}
			}

			shapes.push_back(
				PlacedCollider{
					ShapeInstance{transform.Frame, collider.Extent, collider.Shape, hull, mesh},
					collider.Trigger,
				}
			);
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
		ENGINE_PROFILE_CAT("physics.sync-broadphase", core::ProfileCategory::Physics);

		PhysicsWorld *world = PreparedWorldMutable(store);
		if (world == nullptr) {
			return;
		}

		// Rebuilt whole, every tick. `spatial::HashGrid` is count-then-fill and
		// has no `Insert`; "only re-insert what moved" is therefore a decision
		// about which set to hand to `Rebuild`, and this is the set that moved.
		std::vector<spatial::Proxy> &dynamicProxies = PipelineInternals::DynamicProxies(*world);
		std::vector<ColliderRecord> &dynamicRecords = PipelineInternals::DynamicRecords(*world);
		std::vector<PlacedCollider> &dynamicShapes = PipelineInternals::DynamicShapes(*world);
		dynamicProxies.clear();
		dynamicRecords.clear();
		dynamicShapes.clear();

		// Resolved once for the walk rather than per collider, which is the same
		// decision the solver makes about `scene::SurfaceTable`.
		const scene::CollisionShapes *baked = scene::CollisionShapesOf(store);

		{
			ENGINE_PROFILE_CAT("physics.gather-dynamic", core::ProfileCategory::Physics);
			store.Each<const scene::Transform, const scene::Collider, const scene::Motion>(
				[&dynamicProxies, &dynamicRecords, &dynamicShapes, baked](
					ecs::Entity entity,
					const scene::Transform &transform,
					const scene::Collider &collider,
					const scene::Motion &
				) {
					AppendEntry(
						dynamicProxies, dynamicRecords, dynamicShapes, baked, entity, transform, collider
					);
				}
			);
		}

		{
			// The rebuild on its own, separate from gathering the proxies that
			// feed it. Both scale with the moving set and they scale
			// differently - the gather is a store walk and this is a count-then-
			// fill over cells - so one number covering both says a broadphase is
			// expensive without saying which half to go and look at.
			ENGINE_PROFILE_CAT("physics.index-dynamic", core::ProfileCategory::Physics);

			// **The grid is sized from the scene rather than left at the
			// default**, which is what `HashGrid::DEFAULT_CELL_SIZE` invites in
			// as many words: that number is twice the median extent of *this
			// repository's* demo colliders, and a world of bullets or a world of
			// buildings gets a grid tuned for something else.
			//
			// The swing is measured and it is large:
			// `engine.physics.bench.broadphase` puts four thousand colliders at
			// 499 us with 2 m cells and 184 us with 8 m, for the same scene and
			// the same answer. Nothing about the pair list changes - the walk is
			// exhaustive at any spacing - so this is speed and not behaviour.
			//
			// **Free when the answer is the same**, which it is on almost every
			// tick: `SuggestCellSize` quantises to a power of two and
			// `SetCellSize` returns without touching anything when the size did
			// not move. A scene has to change scale by a factor of two before
			// this costs a rebuild that was not already happening.
			spatial::HashGrid &index = PipelineInternals::DynamicIndex(*world);
			if (world->CellSizeMeasured()) {
				index.SetCellSize(spatial::SuggestCellSize(dynamicProxies));
			}
			index.Rebuild(dynamicProxies);
		}
		PipelineInternals::DynamicRebuildCount(*world)++;

		// Every collider, minus the ones that just went into the dynamic index.
		// `ecs::Store` has no "without this component" query term, so the count
		// is a subtraction and the pass below asks per row - which is affordable
		// exactly because it is not a per-tick pass.
		size_t colliders = 0;
		{
			ENGINE_PROFILE_CAT("physics.count-colliders", core::ProfileCategory::Physics);
			colliders = store.CountMatching<scene::Transform, scene::Collider>();
		}
		const size_t staticCount = colliders - dynamicRecords.size();

		if (!StaticSetChanged(store, *world, staticCount)) {
			return;
		}

		std::vector<spatial::Proxy> &staticProxies = PipelineInternals::StaticProxies(*world);
		std::vector<ColliderRecord> &staticRecords = PipelineInternals::StaticRecords(*world);
		std::vector<PlacedCollider> &staticShapes = PipelineInternals::StaticShapes(*world);
		staticProxies.clear();
		staticRecords.clear();
		staticShapes.clear();

		store.Each<const scene::Transform, const scene::Collider>(
			[&store, &staticProxies, &staticRecords, &staticShapes, baked](
				ecs::Entity entity, const scene::Transform &transform, const scene::Collider &collider
			) {
				// The per-row question the ECS cannot express as a query term.
				// It costs a sparse-set lookup per collider, and it is paid when
				// the static set changes rather than once a tick.
				if (store.Has<scene::Motion>(entity)) {
					return;
				}
				AppendEntry(staticProxies, staticRecords, staticShapes, baked, entity, transform, collider);
			}
		);

		{
			// Only on a tick where the static set actually changed, which is what
			// makes it worth its own row: a span that is absent from most frames
			// and 4 ms on one of them is exactly what the RMAX column is for.
			ENGINE_PROFILE_CAT("physics.index-static", core::ProfileCategory::Physics);

			// **Sized independently of the dynamic half, because the two hold
			// different things.** A scene's static geometry is walls and floors
			// and its dynamic set is crates and characters; one spacing chosen
			// for the union of them is chosen for neither. Both are queried by
			// the same dynamic proxies, and a query costs what the *index* it
			// walks costs rather than what the querying box is.
			spatial::HashGrid &index = PipelineInternals::StaticIndex(*world);
			if (world->CellSizeMeasured()) {
				index.SetCellSize(spatial::SuggestCellSize(staticProxies));
			}
			index.Rebuild(staticProxies);
		}
		PipelineInternals::StaticRebuildCount(*world)++;
		PipelineInternals::StaticStale(*world) = false;
		PipelineInternals::StaticChangeVersion(*world) = store.ChangeVersion();
	}
}
