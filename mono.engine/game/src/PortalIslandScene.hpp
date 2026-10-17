#pragma once

#include <engine/physics/CopiedContacts.hpp>
#include <engine/physics/PortalIsland.hpp>

#include <span>
#include <vector>

namespace engine::ecs {
	class Store;
}

namespace engine::game {
	// Copies active canonical portal bodies into the chosen local chart. Contact
	// generation stays with the seam coordinator, so this routine never retains
	// a reference to another world's store.
	void CollectPortalIslandBodies(ecs::Store &store, std::vector<physics::PortalIslandBody> &out);
	// Appends finite far-side dynamic rows gathered during the preceding exchange
	// round. Both sides are values in the local chart before the global solve.
	void CollectPortalIslandContacts(ecs::Store &store, physics::PortalIslandPacket &packet);
	// Writes a solved velocity only when the persistent body incarnation still
	// matches, preventing an old barrier reply touching a reborn body.
	bool ApplyPortalIslandResults(ecs::Store &store, std::span<const physics::PortalIslandResult> results);
	// Adds one finite-aperture dynamic pair to a barrier packet. Both snapshots
	// must already be expressed in the same portal chart.
	bool AppendPortalIslandContact(
		const physics::CopiedDynamicContact &first,
		const physics::CopiedDynamicContact &second,
		physics::PortalIslandPacket &packet
	);
}
