#pragma once

// Reaching `PhysicsWorld`'s storage. Private to this module.
//
// The two indexes, the proxy and record arrays and the pair list are written by
// the systems in this directory and read by the suites and benchmarks beside
// them - and not one of those is another module. A set of public setters would
// turn the layout into an API somebody outside could depend on, so this is a
// friend instead. `spatial/src/GridInternals.hpp` does the same for the same
// reason, and the symmetry is deliberate.

#include <engine/ecs/Entity.hpp>
#include <engine/physics/Contacts.hpp>
#include <engine/physics/PhysicsWorld.hpp>
#include <engine/spatial/HashGrid.hpp>

#include <cstdint>
#include <vector>

namespace engine::physics {

	// Reaches the resource's storage, and nothing else.
	struct PipelineInternals {
		// The index over colliders that can move, rebuilt every tick.
		static spatial::HashGrid &DynamicIndex(PhysicsWorld &world) {
			return world.DynamicIndex;
		}

		// The index over colliders that cannot, rebuilt when the set changes.
		static spatial::HashGrid &StaticIndex(PhysicsWorld &world) {
			return world.StaticIndex;
		}

		// The same four, for a reader. The collider queries take a `const
		// Store &` because several workers may run one each at the same time,
		// and a mutable accessor would be the one line that quietly ended that.
		static const spatial::HashGrid &DynamicIndex(const PhysicsWorld &world) {
			return world.DynamicIndex;
		}

		static const spatial::HashGrid &StaticIndex(const PhysicsWorld &world) {
			return world.StaticIndex;
		}

		static const std::vector<ColliderRecord> &DynamicRecords(const PhysicsWorld &world) {
			return world.DynamicRecords;
		}

		static const std::vector<ColliderRecord> &StaticRecords(const PhysicsWorld &world) {
			return world.StaticRecords;
		}

		// The dynamic proxies, parallel to `DynamicRecords` by index.
		static std::vector<spatial::Proxy> &DynamicProxies(PhysicsWorld &world) {
			return world.DynamicProxies;
		}

		// What the broad phase knows about each dynamic collider.
		static std::vector<ColliderRecord> &DynamicRecords(PhysicsWorld &world) {
			return world.DynamicRecords;
		}

		// The static proxies, parallel to `StaticRecords` by index.
		static std::vector<spatial::Proxy> &StaticProxies(PhysicsWorld &world) {
			return world.StaticProxies;
		}

		// What the broad phase knows about each static collider.
		static std::vector<ColliderRecord> &StaticRecords(PhysicsWorld &world) {
			return world.StaticRecords;
		}

		// The pair list, cleared and refilled by `BroadPhase`.
		static std::vector<CandidatePair> &Pairs(PhysicsWorld &world) {
			return world.PairList;
		}

		// The manifold list, cleared and refilled by `NarrowPhase`.
		static std::vector<ContactManifold> &Manifolds(PhysicsWorld &world) {
			return world.ManifoldList;
		}

		// The event list, cleared by `NarrowPhase` and filled by `Publish`.
		static std::vector<ContactEvent> &Events(PhysicsWorld &world) {
			return world.EventList;
		}

		// The solver's compact body array, refilled by `Solve`.
		static std::vector<SolverBody> &Bodies(PhysicsWorld &world) {
			return world.BodyList;
		}

		// The per-point constraint rows, refilled by `Solve`.
		static std::vector<ContactRow> &Rows(PhysicsWorld &world) {
			return world.RowList;
		}

		// The gather's sort buffer, and the body indices it resolves.
		static std::vector<ecs::Entity> &BodyOwners(PhysicsWorld &world) {
			return world.BodyOwners;
		}

		static std::vector<std::pair<uint32_t, uint32_t>> &ManifoldBodies(PhysicsWorld &world) {
			return world.ManifoldBodies;
		}

		// Last tick's accumulated impulses, which this tick warm-starts from.
		static std::vector<ContactImpulse> &ImpulseCache(PhysicsWorld &world) {
			return world.ImpulseCache;
		}

		// This tick's accumulated impulses, for the tick after it.
		static std::vector<ContactImpulse> &ImpulseNext(PhysicsWorld &world) {
			return world.ImpulseNext;
		}

		// How long each body has been still, carried across ticks.
		static std::vector<RestingBody> &Resting(PhysicsWorld &world) {
			return world.RestingList;
		}

		// Where the next tick's resting list is merged before the two swap.
		static std::vector<RestingBody> &RestingNext(PhysicsWorld &world) {
			return world.RestingNext;
		}

		// The pairs that were touching at the end of the previous tick.
		static std::vector<CandidatePair> &TouchingLast(PhysicsWorld &world) {
			return world.TouchingLast;
		}

		// The pairs touching at the end of this one.
		static std::vector<CandidatePair> &TouchingNow(PhysicsWorld &world) {
			return world.TouchingNow;
		}

		// Scratch for an overlap query's answer, reused across every query in a
		// tick.
		static std::vector<uint64_t> &CandidateBuffer(PhysicsWorld &world) {
			return world.CandidateBuffer;
		}

		// Whether the static index is known to be out of date.
		static bool &StaticStale(PhysicsWorld &world) {
			return world.StaticStale;
		}

		// The store change counter as of the last time the static set was
		// examined. Equal to `Store::ChangeVersion()` means nothing has been
		// written through `Set` since, so there is nothing to re-examine.
		static uint64_t &StaticChangeVersion(PhysicsWorld &world) {
			return world.StaticChangeVersion;
		}

		// How many times the dynamic index has been rebuilt.
		static uint64_t &DynamicRebuildCount(PhysicsWorld &world) {
			return world.DynamicRebuildCount;
		}

		// How many times the static index has been rebuilt.
		static uint64_t &StaticRebuildCount(PhysicsWorld &world) {
			return world.StaticRebuildCount;
		}

		// The cell size the world was constructed with, for a reader that has
		// to reconstruct the grids - a snapshot, above all.
		static float CellSize(const PhysicsWorld &world) {
			return world.DynamicIndex.CellSize();
		}
	};
}
