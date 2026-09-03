#include "PipelineInternals.hpp"
#include "WorldResource.hpp"

#include <engine/core/FrameGraph.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/physics/Broadphase.hpp>
#include <engine/physics/PhysicsWorld.hpp>
#include <engine/physics/Shapes.hpp>
#include <engine/scene/CollisionShapes.hpp>
#include <engine/scene/Components.hpp>
#include <engine/spatial/HashGrid.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace engine::physics {

	namespace {
		// A world that had to fall back to the grid re-enters the tree after
		// several low-motion gathers. One frame is often the pause between two
		// bursts; three keeps the recovery deterministic without rebuilding both
		// indexes on every grid frame.
		// A fallback grid probes after at most 32 ticks, about half a second at
		// the usual 60 Hz. The interval keeps high-motion fallback shaped like
		// the pre-tree generated rebuild path rather than paying a second copy.
		constexpr size_t DYNAMIC_TREE_SETTLED_FRAMES = 32;
		void DispatchGridRanges(
			void *, size_t count, spatial::HashGrid::RangeDispatcher::Body body, void *bodyContext
		) {
			parallel::Jobs::For(
				count, 1, [body, bodyContext](size_t begin, size_t end) { body(bodyContext, begin, end); }, 1
			);
		}

		const spatial::HashGrid::RangeDispatcher GRID_DISPATCHER{nullptr, &DispatchGridRanges};
		// Writes one collider's proxy and appends its record, in row order.
		//
		// `Proxy::Id` is the index the two arrays share, not the entity - see
		// `PhysicsWorld`, which explains why. The entity is on the record, so
		// resolving a candidate is a subscript and never a store lookup.
		void WriteEntry(
			spatial::Proxy &proxy,
			uint64_t index,
			std::vector<ColliderRecord> &records,
			std::vector<PlacedCollider> &shapes,
			const scene::CollisionShapes *baked,
			ecs::Entity entity,
			const scene::Transform &transform,
			const scene::Collider &collider
		) {
			proxy = spatial::Proxy{index, ShapeWorldBounds(collider, transform.Frame), collider.Layer};
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

		// Gather once into retained rows, then choose exactly one dynamic index.
		// The grid remains rebuild-only; a tree may synchronise a stable row order
		// by reinserting only leaves that escaped their fat bounds.
		std::vector<core::AABB> &dynamicBounds = PipelineInternals::DynamicBounds(*world);
		std::vector<spatial::Proxy> &dynamicProxies = PipelineInternals::DynamicProxies(*world);
		std::vector<ColliderRecord> &dynamicRecords = PipelineInternals::DynamicRecords(*world);
		std::vector<PlacedCollider> &dynamicShapes = PipelineInternals::DynamicShapes(*world);
		std::vector<core::AABB> &previous = PipelineInternals::PreviousDynamicBounds(*world);
		std::vector<ecs::Entity> &previousOwners = PipelineInternals::PreviousDynamicOwners(*world);
		const bool treeWasActive = PipelineInternals::DynamicTreeActive(*world);
		dynamicBounds.clear();
		dynamicProxies.clear();
		dynamicRecords.clear();
		dynamicShapes.clear();

		// Resolved once for the walk rather than per collider, which is the same
		// decision the solver makes about `scene::SurfaceTable`.
		const scene::CollisionShapes *baked = scene::CollisionShapesOf(store);

		{
			// Gathering has one deterministic stream. That keeps the records, tight
			// bounds and either index on the same positional identity.
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
			spatial::DynamicBvh &tree = PipelineInternals::DynamicTree(*world);
			const size_t dynamicCount =
				store.Query<const scene::Transform, const scene::Collider, const scene::Motion>().Count();
			const bool recoveryProbe =
				!treeWasActive && PipelineInternals::DynamicTreeSettledFrames(*world) <= 1;
			const bool gatherProxies = treeWasActive || tree.ProxyCount() == 0;
			dynamicBounds.resize(dynamicCount);
			dynamicProxies.resize(gatherProxies ? dynamicCount : 0);
			const bool compareRecovery = treeWasActive || recoveryProbe;
			const bool previousCompatible = compareRecovery && previous.size() == dynamicCount;
			const bool previousOwnersCompatible = compareRecovery && previousOwners.size() == dynamicCount;
			size_t recoveryChanges = previousCompatible ? 0 : dynamicCount;
			bool topologyChanged = compareRecovery && !previousOwnersCompatible;
			if (compareRecovery) {
				previousOwners.resize(dynamicCount);
			}
			const bool directGridGather = !gatherProxies && !recoveryProbe && dynamicCount != 0;
			if (directGridGather) {
				spatial::HashGrid &grid = PipelineInternals::DynamicIndex(*world);
				const auto fillGrid = [&](std::span<spatial::Proxy> proxies) {
					size_t written = 0;
					store.Each<const scene::Transform, const scene::Collider, const scene::Motion>(
						[&](ecs::Entity entity,
							const scene::Transform &transform,
							const scene::Collider &collider,
							const scene::Motion &) {
							WriteEntry(
								proxies[written],
								static_cast<uint64_t>(written),
								dynamicRecords,
								dynamicShapes,
								baked,
								entity,
								transform,
								collider
							);
							dynamicBounds[written] = proxies[written].Bounds;
							written++;
						}
					);
				};
				if (parallel::Jobs::WorkerCount() != 0) {
					grid.RebuildGeneratedParallel(
						dynamicCount, fillGrid, GRID_DISPATCHER, world->CellSizeMeasured()
					);
				} else {
					grid.RebuildGenerated(dynamicCount, fillGrid, world->CellSizeMeasured());
				}
			}
			const auto gather = [&](auto &&writeProxy) {
				ENGINE_PROFILE_CAT("physics.gather-dynamic", core::ProfileCategory::Physics);
				size_t written = 0;
				store.Each<const scene::Transform, const scene::Collider, const scene::Motion>(
					[&](ecs::Entity entity,
						const scene::Transform &transform,
						const scene::Collider &collider,
						const scene::Motion &) {
						if (compareRecovery) {
							topologyChanged = topologyChanged ||
											  (previousOwnersCompatible && previousOwners[written] != entity);
							previousOwners[written] = entity;
						}
						spatial::Proxy proxy;
						WriteEntry(
							proxy,
							static_cast<uint64_t>(written),
							dynamicRecords,
							dynamicShapes,
							baked,
							entity,
							transform,
							collider
						);
						dynamicBounds[written] = proxy.Bounds;
						if (recoveryProbe && previousCompatible) {
							const core::AABB &before = previous[written];
							const core::AABB &current = dynamicBounds[written];
							const float margin = spatial::DynamicBvh::FAT_MARGIN;
							recoveryChanges +=
								std::abs(current.Minimum.X - before.Minimum.X) <= margin &&
										std::abs(current.Minimum.Y - before.Minimum.Y) <= margin &&
										std::abs(current.Minimum.Z - before.Minimum.Z) <= margin &&
										std::abs(current.Maximum.X - before.Maximum.X) <= margin &&
										std::abs(current.Maximum.Y - before.Maximum.Y) <= margin &&
										std::abs(current.Maximum.Z - before.Maximum.Z) <= margin
									? 0
									: 1;
						}
						writeProxy(proxy, written++);
					}
				);
			};
			if (directGridGather) {
			} else if (gatherProxies) {
				gather([&dynamicProxies](const spatial::Proxy &proxy, size_t index) {
					dynamicProxies[index] = proxy;
				});
			} else {
				spatial::HashGrid &grid = PipelineInternals::DynamicIndex(*world);
				bool recovered = false;
				grid.RebuildGenerated(
					dynamicCount,
					[&](std::span<spatial::Proxy> proxies) {
						gather([proxies](const spatial::Proxy &proxy, size_t index) {
							proxies[index] = proxy;
						});
						if (recoveryProbe && !topologyChanged &&
							recoveryChanges * spatial::DynamicBvh::MAXIMUM_INCREMENTAL_DENOMINATOR <=
								dynamicBounds.size()) {
							tree.Rebuild(proxies);
							recovered = true;
						}
					},
					world->CellSizeMeasured()
				);
				if (recovered) {
					PipelineInternals::DynamicTreeActive(*world) = true;
					PipelineInternals::DynamicTreeSettledFrames(*world) = 0;
				}
			}

			spatial::DynamicBvhPreflight preflight{};
			bool preflightSampled = false;
			bool useTree = PipelineInternals::DynamicTreeActive(*world) && !treeWasActive;
			if (directGridGather) {
				PipelineInternals::DynamicTreeSettledFrames(*world)--;
			} else if (!gatherProxies) {
				if (!useTree && recoveryProbe) {
					PipelineInternals::DynamicTreeSettledFrames(*world) = DYNAMIC_TREE_SETTLED_FRAMES;
				} else if (!useTree) {
					PipelineInternals::DynamicTreeSettledFrames(*world)--;
				}
			} else if (dynamicProxies.empty()) {
				if (tree.ProxyCount() != 0) {
					tree.Clear();
				}
				if (PipelineInternals::DynamicIndex(*world).ProxyCount() != 0) {
					PipelineInternals::DynamicIndex(*world).Clear();
				}
				PipelineInternals::DynamicTreeSettledFrames(*world) = 0;
			} else if (tree.ProxyCount() == 0) {
				tree.Rebuild(dynamicProxies);
				useTree = true;
			} else if (treeWasActive && !topologyChanged) {
				preflight = tree.Preflight(dynamicProxies);
				preflightSampled = true;
				const bool incremental =
					preflight.Compatible &&
					preflight.EscapedLeaves * spatial::DynamicBvh::MAXIMUM_INCREMENTAL_DENOMINATOR <
						dynamicProxies.size();
				if (incremental) {
					useTree = tree.Sync(dynamicProxies, preflight);
				}
				if (!useTree) {
					PipelineInternals::DynamicTreeSettledFrames(*world) = DYNAMIC_TREE_SETTLED_FRAMES;
				}
			} else if (treeWasActive) {
				PipelineInternals::DynamicTreeSettledFrames(*world) = DYNAMIC_TREE_SETTLED_FRAMES;
			} else if (!topologyChanged &&
					   recoveryChanges * spatial::DynamicBvh::MAXIMUM_INCREMENTAL_DENOMINATOR <=
						   dynamicBounds.size()) {
				tree.Rebuild(dynamicProxies);
				useTree = true;
				PipelineInternals::DynamicTreeSettledFrames(*world) = 0;
			} else {
				PipelineInternals::DynamicTreeSettledFrames(*world) = DYNAMIC_TREE_SETTLED_FRAMES;
			}
			if (useTree && !tree.Stats().PairCacheAvailable) {
				useTree = false;
				PipelineInternals::DynamicTreeSettledFrames(*world) = DYNAMIC_TREE_SETTLED_FRAMES;
			}

			if (!useTree && !dynamicProxies.empty()) {
				if (world->CellSizeMeasured()) {
					PipelineInternals::DynamicIndex(*world).SetCellSize(
						spatial::SuggestCellSize(dynamicProxies)
					);
				}
				spatial::HashGrid &grid = PipelineInternals::DynamicIndex(*world);
				if (parallel::Jobs::WorkerCount() != 0) {
					grid.RebuildParallel(dynamicProxies, GRID_DISPATCHER);
				} else {
					grid.Rebuild(dynamicProxies);
				}
			}
			PipelineInternals::DynamicTreeActive(*world) = useTree;
			if (compareRecovery) {
				previous = dynamicBounds;
			}
			if (treeWasActive != useTree) {
				core::Metrics::SetGauge("physics.dynamic_index.mode", useTree ? 1.0 : 0.0);
			}
			if (treeWasActive || recoveryProbe || useTree) {
				const spatial::DynamicBvhStats treeStats = tree.Stats();
				core::Metrics::SetGauge(
					"physics.dynamic_bvh.preflight_escaped", static_cast<double>(preflight.EscapedLeaves)
				);
				core::Metrics::SetGauge(
					"physics.dynamic_bvh.preflight_sampled", preflightSampled ? 1.0 : 0.0
				);
				core::Metrics::SetGauge(
					"physics.dynamic_bvh.refitted",
					preflightSampled && useTree ? static_cast<double>(treeStats.RefittedLeaves) : 0.0
				);
				core::Metrics::SetGauge(
					"physics.dynamic_bvh.reinserted",
					preflightSampled && useTree ? static_cast<double>(treeStats.ReinsertedLeaves) : 0.0
				);
				core::Metrics::SetGauge(
					"physics.dynamic_bvh.full_rebuilds", static_cast<double>(treeStats.Rebuilds)
				);
				core::Metrics::SetGauge(
					"physics.dynamic_bvh.support_retained_bytes",
					static_cast<double>(
						treeStats.RetainedBytes + dynamicProxies.capacity() * sizeof(spatial::Proxy) +
						previous.capacity() * sizeof(core::AABB) +
						previousOwners.capacity() * sizeof(ecs::Entity)
					)
				);
			}
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

		std::vector<ColliderRecord> &staticRecords = PipelineInternals::StaticRecords(*world);
		std::vector<PlacedCollider> &staticShapes = PipelineInternals::StaticShapes(*world);
		staticRecords.clear();
		staticShapes.clear();

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
			index.RebuildGenerated(
				staticCount,
				[&store, &staticRecords, &staticShapes, baked](std::span<spatial::Proxy> proxies) {
					size_t written = 0;
					store.Each<const scene::Transform, const scene::Collider>(
						[&](ecs::Entity entity,
							const scene::Transform &transform,
							const scene::Collider &collider) {
							// The per-row question the ECS cannot express as a query term.
							// It costs a sparse-set lookup per collider, and it is paid when
							// the static set changes rather than once a tick.
							if (store.Has<scene::Motion>(entity)) {
								return;
							}
							WriteEntry(
								proxies[written],
								static_cast<uint64_t>(written),
								staticRecords,
								staticShapes,
								baked,
								entity,
								transform,
								collider
							);
							written++;
						}
					);
				},
				world->CellSizeMeasured()
			);
		}
		PipelineInternals::StaticRebuildCount(*world)++;
		PipelineInternals::StaticStale(*world) = false;
		PipelineInternals::StaticChangeVersion(*world) = store.ChangeVersion();
	}
}
