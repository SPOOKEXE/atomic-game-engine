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
	// Presentation-bus channel for topology requests.
	inline constexpr std::string_view PORTAL_TOPOLOGY_REQUESTS = "portal-topology-requests";
	// Presentation-bus channel for topology replies.
	inline constexpr std::string_view PORTAL_TOPOLOGY_REPLIES = "portal-topology-replies";
	// Versioned portal seams for one destination world.
	struct PortalTopologySnapshot {
		// Changes when the destination's seam set changes.
		uint64_t Revision = 0;
		// Copied seam descriptions, independent of source ECS handles.
		std::vector<scene::PortalSeam> Seams;
	};
	// Presentation-owner adapter. One destination cache serves all viewports.
	// Trusted host setup owns route authentication. Universe outlives this host.
	class PortalTopologyHost {
	  public:
		// Monotonic clock used to expire topology requests and cached replies.
		using Time = std::chrono::steady_clock::time_point;
		// Borrows the universe that owns all producer and destination worlds.
		explicit PortalTopologyHost(world::Universe &universe);
		~PortalTopologyHost();
		PortalTopologyHost(const PortalTopologyHost &) = delete;
		PortalTopologyHost &operator=(const PortalTopologyHost &) = delete;
		// Registers a producer address for one local world.
		world::PresentationAddress Serve(world::WorldId world);
		// Requests fresh topology for a destination from a source world.
		bool Request(world::WorldId source, world::WorldId destination, Time now);
		// Advances request timeouts and consumes completed topology replies.
		void Pump(Time now);
		// Borrow until the next mutation. Missing, expired and retired data return null.
		const PortalTopologySnapshot *Snapshot(world::WorldId destination, Time now) const;
		// Retain unexpired topology while replacing requests lost with a transport.
		void RestartRequests();
		// Drops topology and pending requests involving a retired world.
		void RemoveWorld(world::WorldId world);
		// Drops all cached topology and pending requests.
		void Clear();

	  private:
		struct Impl;
		std::unique_ptr<Impl> State;
	};
}
