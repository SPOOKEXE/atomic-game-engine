#pragma once
#include <engine/imagegraph/HostCapture.hpp>

#include <optional>

namespace engine::imagegraph {
	// One pending evaluation owns exact capability observations, so retrying its
	// immutable frame does not repeat upstream host side effects.
	struct PendingHostObservations {
		std::vector<HostNodeCapture> Captures;
		uint64_t Bytes = 0, Tick = 0, Seed = 0;
		double Subframe = 0;
		bool NegativeFrame = false, Active = false;
		void Clear();
		bool Capture(const HostNodeInvocation &, HostNodeProvider &, HostNodeCapture &, std::string &);
	};
}
