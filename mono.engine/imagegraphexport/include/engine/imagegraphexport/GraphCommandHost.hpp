#pragma once
#include <engine/assets/ContentPolicy.hpp>
#include <engine/imagegraph/HostCapture.hpp>

#include <chrono>
#include <filesystem>

namespace engine::imagegraphexport {
	struct GraphProcessGrant {
		std::string NodeId;
		std::filesystem::path Executable;
		std::string Script;
		std::vector<std::string> Arguments;
		std::filesystem::path WorkingDirectory;
		std::chrono::milliseconds Timeout{30000};
	};
	struct GraphHttpGrant {
		std::string NodeId;
		std::string Address;
		bool Post = false;
		std::string Content;
		std::filesystem::path Client;
		uint64_t MaximumBytes = 10000;
		std::chrono::milliseconds Timeout{30000};
	};
	// Bounded standalone grants, supplied by the host rather than embedded graph text.
	bool ReadGraphCommandGrants(
		std::string_view text,
		std::vector<GraphProcessGrant> &processes,
		std::vector<GraphHttpGrant> &requests,
		std::string &failure,
		std::vector<std::string> *clockNodes = nullptr
	);
	// A host grant supplies the command argv or the exact HTTP request. Graph text cannot widen it.
	class GraphCommandHost final : public engine::imagegraph::HostNodeProvider {
	  public:
		GraphCommandHost(
			std::span<const GraphProcessGrant> processes,
			std::span<const GraphHttpGrant> requests,
			engine::assets::ContentPolicy policy =
				engine::assets::ContentPolicy::Process(engine::assets::ContentVerb::Handle),
			std::span<const std::string> clockNodes = {}
		);
		bool Capture(
			const engine::imagegraph::HostNodeInvocation &,
			engine::imagegraph::HostNodeCapture &,
			std::string &
		) override;

	  private:
		std::span<const GraphProcessGrant> Processes;
		std::span<const GraphHttpGrant> Requests;
		engine::assets::ContentPolicy Policy;
		std::span<const std::string> ClockNodes;
		std::chrono::steady_clock::time_point ClockStart = std::chrono::steady_clock::now();
	};
}
