#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Ownership.hpp>
#include <engine/scene/Services.hpp>

#include <vector>

namespace engine::scene {

	bool SetNetworkOwner(ecs::Store &store, ecs::Entity instance, ecs::Entity player) {
		if (!store.Alive(instance)) {
			return false;
		}

		if (player == ecs::NULL_ENTITY) {
			// Removing rather than storing a null. See the header: absent and
			// "owned by nobody" may not be two spellings of one state.
			//
			// **Allowed whatever `instance` is**, including an anchored part
			// the check below would refuse: giving something back to the server
			// is always a legal thing to ask for, and a script that anchored a
			// part and then tidied up should not be told off for it.
			store.Remove<NetworkOwner>(instance);
			return true;
		}

		// **Nothing to simulate, nothing to own.** A static part carries no
		// `Simulated` tag - the decision is presence rather than a flag - and a
		// `Folder` or a service has no body either. See the header.
		//
		// A *sleeping* part still carries the tag and is still ownable, which is
		// the behaviour that matters here: a crate somebody was handed does not
		// change hands because it settled.
		if (!store.Has<Simulated>(instance)) {
			return false;
		}

		// **Checked, because the failure is otherwise silent and permanent.**
		// Handing a body to a `Folder` writes a row nothing will ever reclaim -
		// the reclaim below only fires for an owner that *was* alive and stopped
		// being - so a scene would carry a body owned by something that cannot
		// own it for the rest of the session.
		if (!store.Alive(player) || !ecs::Classes::IsA(store.ClassOf(player), PlayerClass())) {
			return false;
		}

		store.Set<NetworkOwner>(instance, NetworkOwner{player});
		return true;
	}

	ecs::Entity NetworkOwnerOf(const ecs::Store &store, ecs::Entity instance) {
		const NetworkOwner *owner = store.Get<NetworkOwner>(instance);
		return owner == nullptr ? ecs::NULL_ENTITY : owner->Player;
	}

	void ReclaimAbandonedOwnership(ecs::Store &store) {
		// **Gathered first and removed after**, because removing a component is
		// a structural change and a structural change during a walk is the one
		// thing `Store` will abort on. The vector is empty on every tick of
		// every game that has not handed anything out, which is all of them
		// today.
		std::vector<ecs::Entity> abandoned;
		store.Each<const NetworkOwner>([&store, &abandoned](ecs::Entity entity, const NetworkOwner &owner) {
			// The owner has gone, or the body has. Anchoring takes the
			// `Simulated` tag away, which would otherwise leave an owner
			// authorised to write the transform of something the world has just
			// declared immovable.
			if (!store.Alive(owner.Player) || !store.Has<Simulated>(entity)) {
				abandoned.push_back(entity);
			}
		});

		for (const ecs::Entity entity : abandoned) {
			store.Remove<NetworkOwner>(entity);
		}
	}

	void RegisterOwnershipSystem(ecs::Scheduler &scheduler) {
		scheduler.Add("scene.ownership", ecs::Phase::PreSimulation, ReclaimAbandonedOwnership);
	}
}
