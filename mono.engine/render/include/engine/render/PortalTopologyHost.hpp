#pragma once

#include <engine/scene/SurfaceCameras.hpp>
#include <engine/world/PresentationBus.hpp>
#include <engine/world/World.hpp>

#include <chrono>
#include <memory>
#include <vector>

namespace engine::world {
	class Universe;
}
namespace engine::render {
	inline constexpr std::string_view PORTAL_TOPOLOGY_REQUESTS = "portal-topology-requests";
	inline constexpr std::string_view PORTAL_TOPOLOGY_REPLIES = "portal-topology-replies";
	struct PortalTopologySnapshot {
		uint64_t Revision = 0;
		std::vector<scene::PortalSeam> Seams;
	};
	// Presentation-owner adapter. One destination cache serves all viewports.
	// Trusted host setup owns route authentication. Universe outlives this host.
	class PortalTopologyHost {
	  public:
		using Time = std::chrono::steady_clock::time_point;
		explicit PortalTopologyHost(world::Universe &universe);
		~PortalTopologyHost();
		PortalTopologyHost(const PortalTopologyHost &) = delete;
		PortalTopologyHost &operator=(const PortalTopologyHost &) = delete;
		world::PresentationAddress Serve(world::WorldId world);
		bool Request(world::WorldId source, world::WorldId destination, Time now);
		void Pump(Time now);
		// Borrow until the next mutation. Missing, expired and retired data return null.
		const PortalTopologySnapshot *Snapshot(world::WorldId destination, Time now) const;
		// Retain unexpired topology while replacing requests lost with a transport.
		void RestartRequests();
		void RemoveWorld(world::WorldId world);
		void Clear();

	  private:
		struct Impl;
		std::unique_ptr<Impl> State;
	};
}
