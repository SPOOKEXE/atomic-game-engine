#pragma once
#include <engine/imagegraphexport/GraphFileHost.hpp>

#include <vector>
namespace engine::imagegraphexport {
	struct GraphDirectoryGrant {
		std::string NodeId;
		std::filesystem::path Root;
	};
	struct GraphDirectoryEntry {
		std::filesystem::path File;
		bool Directory = false;
	};
	struct GraphDirectoryListing {
		std::filesystem::path Directory;
		std::vector<GraphDirectoryEntry> Entries;
	};
	// Owns the actual filesystem order observed once per traversed directory.
	struct GraphDirectoryObservation {
		std::string NodeId;
		bool Recursive = false;
		std::filesystem::path Root;
		std::vector<GraphDirectoryListing> Listings;
	};
	// Returns a bounded owning-record charge without reading the filesystem.
	std::optional<uint64_t> GraphDirectoryObservationBytes(const GraphDirectoryObservation &observation);
	bool ObserveGraphDirectory(
		const engine::imagegraph::HostNodeInvocation &invocation,
		std::span<const GraphDirectoryGrant> directories,
		GraphDirectoryObservation &observation,
		std::string &failure
	);
	bool CaptureGraphDirectory(
		const engine::imagegraph::HostNodeInvocation &invocation,
		const GraphDirectoryObservation &observation,
		std::span<const GraphFileGrant> files,
		const engine::assets::ContentPolicy &policy,
		engine::imagegraph::HostNodeCapture &output,
		std::string &failure
	);
	class GraphDirectoryHost final : public engine::imagegraph::HostNodeProvider {
	  public:
		GraphDirectoryHost(
			std::span<const GraphDirectoryGrant> directories,
			std::span<const GraphFileGrant> files,
			engine::assets::ContentPolicy policy
		)
			: Directories(directories), Files(files), Policy(policy) {}
		// Discards this node's owned directory order so the next request observes it again.
		void Refresh(std::string_view nodeId);
		bool Capture(
			const engine::imagegraph::HostNodeInvocation &in,
			engine::imagegraph::HostNodeCapture &out,
			std::string &failure
		) override;

	  private:
		std::span<const GraphDirectoryGrant> Directories;
		std::span<const GraphFileGrant> Files;
		engine::assets::ContentPolicy Policy;
		std::vector<GraphDirectoryObservation> Observations;
		uint64_t RetainedBytes = 0;
	};
}
