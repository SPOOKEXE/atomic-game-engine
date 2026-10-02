#pragma once
#include <engine/assets/ContentPolicy.hpp>
#include <engine/imagegraph/HostCapture.hpp>

#include <chrono>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace engine::imagegraphexport {
	struct GraphVideoGrant {
		std::string NodeId;
		std::filesystem::path File;
	};
	struct GraphVideoSettings {
		std::span<const GraphVideoGrant> Grants;
		std::filesystem::path Decoder;
		engine::assets::ContentPolicy Content;
		std::chrono::milliseconds Timeout{30000};
	};
	// Decodes granted clips once, then samples immutable source frames using authored animation controls.
	class GraphVideoHost final : public engine::imagegraph::HostNodeProvider {
	  public:
		explicit GraphVideoHost(GraphVideoSettings settings);
		bool Capture(
			const engine::imagegraph::HostNodeInvocation &invocation,
			engine::imagegraph::HostNodeCapture &output,
			std::string &failure
		) override;

	  private:
		struct Clip {
			std::string NodeId;
			std::filesystem::path Path;
			std::vector<engine::imagegraph::Image> Frames;
		};
		GraphVideoSettings Settings;
		std::vector<Clip> Clips;
	};
}
