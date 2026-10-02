#pragma once

#include <engine/assets/ContentPolicy.hpp>
#include <engine/imagegraph/HostCapture.hpp>

#include <filesystem>
#include <span>
#include <string>

namespace engine::imagegraphexport {
	struct GraphFileGrant {
		std::string NodeId;
		std::filesystem::path File;
		bool Write = false;
		// Named dependency from a room resource. Empty selects the primary exact file grant.
		std::string Resource{};
	};
	// A grant binds one authored node to one exact path and operation. Graph paths confer no access.
	class GraphFileHost final : public engine::imagegraph::HostNodeProvider {
	  public:
		GraphFileHost(std::span<const GraphFileGrant> grants, engine::assets::ContentPolicy policy);
		bool Capture(
			const engine::imagegraph::HostNodeInvocation &invocation,
			engine::imagegraph::HostNodeCapture &output,
			std::string &failure
		) override;

	  private:
		std::span<const GraphFileGrant> Grants;
		engine::assets::ContentPolicy Policy;
	};
}
