#pragma once

// Cache of admitted immutable documents, scoped by the host's signed content route.
// Live input values belong to ECS and are supplied as snapshots, never stored here.
// @tier L9 · shared
#include <engine/imagegraph/Document.hpp>

#include <string>
#include <string_view>
#include <unordered_map>

namespace engine::imagegraph {
	// One admitted graph and the signed-content metadata that identifies its source.
	struct ContentRecord {
		// Parsed authored document retained for host evaluation.
		Document Authored;
		// Verified signed root associated with this asset.
		std::string Root;
		// Monotonic revision assigned when this record is admitted.
		uint64_t Revision = 0;
		// Encoded document size counted against the cache budget.
		size_t EncodedBytes = 0;
	};
	// Bounded cache of graph documents accepted through a signed content route.
	class Content {
	  public:
		// Maximum number of distinct asset records retained.
		static constexpr size_t MaximumRecords = 256;
		// Caller admits only verified ImageGraph assets. Invalid runtime sources or
		// duplicate signed roots preserve the last good record and its revision.
		bool Admit(std::string_view asset, std::string_view root, std::string_view encoded, Diagnostic &);
		// Finds an admitted record by its runtime asset path.
		const ContentRecord *Find(std::string_view asset) const;

	  private:
		std::unordered_map<std::string, ContentRecord> Records;
		uint64_t Revision = 0;
		size_t RetainedBytes = 0;
	};
	// Exact runtime texture names or world-local editable references supplied by live controls.
	// No host-relative rewrite is allowed after signed content admission.
	// Empty selection validates every declared source. Otherwise only the union
	// of the selected output dependency cones is demanded by the host.
	bool RuntimeSources(
		const Document &,
		std::vector<std::string> &out,
		Diagnostic &,
		std::span<const std::string_view> outputs = {}
	);
}
