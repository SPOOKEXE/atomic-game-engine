#pragma once

// Owner-thread admission and charging. Direct stages outside a scheduled tick
// retain their existing unlimited behavior, but never bypass a blocked store.

#include <engine/physics/WorkBudget.hpp>
#include <engine/spatial/DynamicBvh.hpp>
#include <engine/spatial/HashGrid.hpp>
#include <engine/spatial/Query.hpp>

#include <utility>

namespace engine::physics {
	struct ShapeInstance;
	inline constexpr const char *PHYSICS_WORK_BUDGET_COMPONENT = "physics.PhysicsWorkBudget";
	void RegisterPhysicsWorkBudget();
	void ReserveEditableMeshWork(ecs::Store &store);
	PhysicsWorkBudget *PreparedWorkBudgetMutable(ecs::Store &store);
	void BeginPhysicsWorkTick(ecs::Store &store);
	void FinishPhysicsWorkTick(ecs::Store &store);
	bool PhysicsWorkActive(const ecs::Store &store);
	void ChargePhysicsWork(ecs::Store &store, PhysicsWorkStage stage, uint64_t units);
	[[noreturn]] void FailPhysicsWork(ecs::Store &store, PhysicsWorkStage stage);
	void AdmitPhysicsCount(ecs::Store &store, PhysicsWorkStage stage, uint64_t count, uint64_t maximum);
	spatial::TraversalBudget PhysicsTraversalBudget(ecs::Store &store);
	size_t RemainingPhysicsGridEntries(ecs::Store &store);
	void FinishPhysicsTraversal(
		ecs::Store &store, PhysicsWorkStage stage, const spatial::TraversalBudget &budget, size_t initial
	);
	uint64_t PhysicsShapeWork(const ShapeInstance &shape);
	void ChargePhysicsPairWork(
		ecs::Store &store, PhysicsWorkStage stage, const ShapeInstance &first, const ShapeInstance &second
	);
	// Keep unlimited direct calls on their original overload. Scheduled builds
	// charge the actual spatial traversal and admit generated entries in spatial.
	template <class... Arguments>
	void RebuildPhysicsGrid(ecs::Store &store, spatial::HashGrid &grid, Arguments &&...arguments) {
		if (!PhysicsWorkActive(store)) {
			grid.Rebuild(std::forward<Arguments>(arguments)...);
			return;
		}
		auto budget = PhysicsTraversalBudget(store);
		const size_t initial = budget.Remaining;
		grid.Rebuild(std::forward<Arguments>(arguments)..., budget, RemainingPhysicsGridEntries(store));
		FinishPhysicsTraversal(store, PhysicsWorkStage::BroadphaseSync, budget, initial);
	}
	template <class... Arguments>
	void RebuildPhysicsGridParallel(ecs::Store &store, spatial::HashGrid &grid, Arguments &&...arguments) {
		if (!PhysicsWorkActive(store)) {
			grid.RebuildParallel(std::forward<Arguments>(arguments)...);
			return;
		}
		auto budget = PhysicsTraversalBudget(store);
		const size_t initial = budget.Remaining;
		grid.RebuildParallel(
			std::forward<Arguments>(arguments)..., budget, RemainingPhysicsGridEntries(store)
		);
		FinishPhysicsTraversal(store, PhysicsWorkStage::BroadphaseSync, budget, initial);
	}
	template <class... Arguments>
	void RebuildPhysicsGridGenerated(ecs::Store &store, spatial::HashGrid &grid, Arguments &&...arguments) {
		if (!PhysicsWorkActive(store)) {
			grid.RebuildGenerated(std::forward<Arguments>(arguments)...);
			return;
		}
		auto budget = PhysicsTraversalBudget(store);
		const size_t initial = budget.Remaining;
		grid.RebuildGenerated(
			std::forward<Arguments>(arguments)..., budget, RemainingPhysicsGridEntries(store)
		);
		FinishPhysicsTraversal(store, PhysicsWorkStage::BroadphaseSync, budget, initial);
	}
	template <class... Arguments>
	void RebuildPhysicsGridGeneratedParallel(
		ecs::Store &store, spatial::HashGrid &grid, Arguments &&...arguments
	) {
		if (!PhysicsWorkActive(store)) {
			grid.RebuildGeneratedParallel(std::forward<Arguments>(arguments)...);
			return;
		}
		auto budget = PhysicsTraversalBudget(store);
		const size_t initial = budget.Remaining;
		grid.RebuildGeneratedParallel(
			std::forward<Arguments>(arguments)..., budget, RemainingPhysicsGridEntries(store)
		);
		FinishPhysicsTraversal(store, PhysicsWorkStage::BroadphaseSync, budget, initial);
	}
	inline void RebuildPhysicsTree(
		ecs::Store &store, spatial::DynamicBvh &tree, std::span<const spatial::Proxy> proxies
	) {
		if (!PhysicsWorkActive(store)) {
			tree.Rebuild(proxies);
			return;
		}
		auto budget = PhysicsTraversalBudget(store);
		const size_t initial = budget.Remaining;
		tree.Rebuild(proxies, budget);
		FinishPhysicsTraversal(store, PhysicsWorkStage::BroadphaseSync, budget, initial);
	}
	inline bool SyncPhysicsTree(
		ecs::Store &store,
		spatial::DynamicBvh &tree,
		std::span<const spatial::Proxy> proxies,
		const spatial::DynamicBvhPreflight &preflight
	) {
		if (!PhysicsWorkActive(store)) return tree.Sync(proxies, preflight);
		auto budget = PhysicsTraversalBudget(store);
		const size_t initial = budget.Remaining;
		const bool synchronized = tree.Sync(proxies, preflight, budget);
		FinishPhysicsTraversal(store, PhysicsWorkStage::BroadphaseSync, budget, initial);
		return synchronized;
	}

	template <class... Arguments>
	spatial::QueryResult QueryPhysicsOverlap(ecs::Store &store, Arguments &&...arguments) {
		if (!PhysicsWorkActive(store)) return spatial::OverlapBox(std::forward<Arguments>(arguments)...);
		auto budget = PhysicsTraversalBudget(store);
		const size_t initial = budget.Remaining;
		const auto found = spatial::OverlapBox(std::forward<Arguments>(arguments)..., budget);
		FinishPhysicsTraversal(store, PhysicsWorkStage::Continuous, budget, initial);
		return found;
	}
	template <class... Arguments>
	spatial::QueryResult QueryPhysicsShapeCast(ecs::Store &store, Arguments &&...arguments) {
		if (!PhysicsWorkActive(store)) return spatial::ShapeCast(std::forward<Arguments>(arguments)...);
		auto budget = PhysicsTraversalBudget(store);
		const size_t initial = budget.Remaining;
		const auto found = spatial::ShapeCast(std::forward<Arguments>(arguments)..., budget);
		FinishPhysicsTraversal(store, PhysicsWorkStage::Continuous, budget, initial);
		return found;
	}

}
