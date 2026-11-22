#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/ecs/Instance.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/WorldMembership.hpp>

#include <vector>

namespace engine::scene {

	namespace {
		using ecs::Entity;
		using ecs::Hierarchy;
		using ecs::NULL_ENTITY;
		using ecs::Store;

		// Past this many changed tree rows a full walk is the cheaper answer.
		constexpr size_t LOCAL_SYNC_LIMIT = 4096;

		// Scratch only; cleared on entry. Per thread because whichever worker
		// claimed a world ticks it.
		std::vector<Entity> &Pending() {
			static thread_local std::vector<Entity> pending;
			return pending;
		}
		std::vector<Entity> &Marks() {
			static thread_local std::vector<Entity> marks;
			return marks;
		}
		std::vector<Entity> &Clears() {
			static thread_local std::vector<Entity> clears;
			return clears;
		}

		// Whether `entity` is Workspace or under it. A world with no Workspace
		// has no storage to tell apart, so everything in it is in the world.
		bool InWorld(const Store &store, Entity entity, Entity workspace) {
			if (workspace == NULL_ENTITY) return true;
			for (Entity at = entity; at != NULL_ENTITY; at = store.ParentOf(at)) {
				if (at == workspace) return true;
			}
			return false;
		}

		// Queues `entity` to carry or lose the marker. Structural writes wait
		// until the walk is over.
		void Decide(const Store &store, Entity entity, bool inWorld) {
			const bool marked = store.Has<OutsideWorld>(entity);
			if (inWorld && marked)
				Clears().push_back(entity);
			else if (!inWorld && !marked)
				Marks().push_back(entity);
		}

		// Walks `root` and everything under it, which all share its answer.
		void DecideSubtree(Store &store, Entity root, bool inWorld, WorldMembershipMemo &memo) {
			std::vector<Entity> &pending = Pending();
			pending.clear();
			pending.push_back(root);
			while (!pending.empty()) {
				const Entity current = pending.back();
				pending.pop_back();
				memo.Parents[current.Id] = store.ParentOf(current).Id;
				Decide(store, current, inWorld);
				store.EachChild(current, [](Entity child) { Pending().push_back(child); });
			}
		}

		void Apply(Store &store) {
			for (const Entity entity : Marks())
				store.Set(entity, OutsideWorld{});
			for (const Entity entity : Clears())
				store.Remove<OutsideWorld>(entity);
		}

		void FullWalk(Store &store, WorldMembershipMemo &memo, Entity workspace) {
			ENGINE_PROFILE_CAT("world membership.walk", core::ProfileCategory::ECS);
			memo.Parents.clear();
			store.Each<const Hierarchy>([&](Entity entity, const Hierarchy &hierarchy) {
				if (hierarchy.Parent != NULL_ENTITY) return;
				DecideSubtree(store, entity, workspace == NULL_ENTITY || entity == workspace, memo);
			});
			// A marker left on a row that is no longer an instance.
			store.Each<const OutsideWorld>([&](Entity entity, const OutsideWorld &) {
				if (!store.Has<Hierarchy>(entity)) Clears().push_back(entity);
			});
			memo.ParentsKept = memo.Parents.size();
			core::Metrics::Count("scene.world_membership.walks", 1);
		}

		// Re-decides only subtrees whose root changed parent. False, having
		// changed nothing, when the change records cannot say what moved.
		bool LocalSync(Store &store, WorldMembershipMemo &memo, Entity workspace) {
			if (store.ComponentClearedChangeVersion<Hierarchy>() > memo.HierarchyWrites ||
				store.ComponentChangeVersion<ecs::InstanceClass>() != memo.ClassWrites ||
				store.ComponentMembershipVersion<ecs::InstanceClass>() != memo.ClassMembership ||
				workspace.Id != memo.Workspace)
				return false;
			std::vector<Entity> moved;
			bool overflow = false;
			store.EachChanged<const Hierarchy>([&](Entity entity, const Hierarchy &hierarchy) {
				if (overflow) return;
				const auto known = memo.Parents.find(entity.Id);
				if (known == memo.Parents.end() || known->second != hierarchy.Parent.Id) {
					moved.push_back(entity);
					overflow = moved.size() > LOCAL_SYNC_LIMIT;
				}
			});
			if (overflow) return false;
			for (const Entity root : moved) {
				if (!store.Alive(root) || !store.Has<Hierarchy>(root)) continue;
				// Moving Workspace or one of its ancestors re-roots everything.
				if (workspace != NULL_ENTITY && (root == workspace || InWorld(store, workspace, root)))
					return false;
			}
			for (const Entity root : moved) {
				if (!store.Alive(root) || !store.Has<Hierarchy>(root)) continue;
				DecideSubtree(store, root, InWorld(store, root, workspace), memo);
			}
			// Destroyed rows leave entries behind.
			if (memo.Parents.size() > memo.ParentsKept * 2 + LOCAL_SYNC_LIMIT) {
				std::erase_if(memo.Parents, [&store](const auto &entry) {
					return !store.Alive(Entity(entry.first));
				});
				memo.ParentsKept = memo.Parents.size();
			}
			return true;
		}
	}

	size_t SyncWorldMembership(Store &store) {
		ENGINE_PROFILE_CAT("world membership", core::ProfileCategory::ECS);
		store.Observe<Hierarchy>();
		store.Observe<ecs::InstanceClass>();
		if (!store.HasResource<WorldMembershipMemo>()) store.SetResource(WorldMembershipMemo{});
		WorldMembershipMemo &memo = *store.ResourceMutable<WorldMembershipMemo>();

		const uint64_t writes = store.ComponentChangeVersion<Hierarchy>();
		const uint64_t membership = store.ComponentMembershipVersion<Hierarchy>();
		if (memo.Fresh && writes == memo.HierarchyWrites && membership == memo.HierarchyMembership &&
			store.ComponentChangeVersion<ecs::InstanceClass>() == memo.ClassWrites &&
			store.ComponentMembershipVersion<ecs::InstanceClass>() == memo.ClassMembership)
			return store.CountMatching<OutsideWorld>();

		const Entity workspace = WorkspaceOf(store);
		Marks().clear();
		Clears().clear();
		if (!memo.Fresh || !LocalSync(store, memo, workspace)) {
			Marks().clear();
			Clears().clear();
			FullWalk(store, memo, workspace);
		}
		Apply(store);

		// Re-read: `Apply` and `SetResource` above may have moved the memo.
		WorldMembershipMemo &kept = *store.ResourceMutable<WorldMembershipMemo>();
		kept.Fresh = true;
		kept.Workspace = workspace.Id;
		kept.HierarchyWrites = store.ComponentChangeVersion<Hierarchy>();
		kept.HierarchyMembership = store.ComponentMembershipVersion<Hierarchy>();
		kept.ClassWrites = store.ComponentChangeVersion<ecs::InstanceClass>();
		kept.ClassMembership = store.ComponentMembershipVersion<ecs::InstanceClass>();
		return store.CountMatching<OutsideWorld>();
	}
}
