#pragma once

// grug prepare source project append in memory. the host still owns callback order and undo admission.

#include <engine/imagegraphio/PxcxImport.hpp>

namespace engine::imagegraphio {
	// grug choose stable identities, placement and operation payload cap.
	struct PxcxAppendOptions {
		// caller supplies fresh durable text; no declaration-order number becomes a saved identity.
		std::string Namespace;
		// empty appends on the root canvas; otherwise this names an existing destination group.
		std::string Context;
		// grug move only root nodes; child positions stay local to their saved group.
		imagegraph::Vector2 Offset{};
		// grug pass preview policy through the native import.
		bool SavePreviewSettings = true;
		// grug cap retained payload and reserved adapter/codec scratch, including old result.
		uint64_t MaximumOperationBytes = imagegraph::Limits::MaximumEvaluationBytes;
	};
	// grug keep saved identities beside fresh identities in source declaration order.
	struct PxcxAppendedNode {
		// identity in incoming source.
		std::string SourceId;
		// fresh identity in merged source.
		std::string NodeId;
		// saved parent was root, before placement inside destination context.
		bool TopLevel = false;
		bool operator==(const PxcxAppendedNode &) const = default;
	};
	// grug give host one complete candidate without changing old state on refusal.
	struct PxcxAppendResult {
		// checked merged source and native projection, ready for host admission.
		PxcxImport Project;
		// source declaration order, used by the host's constructor callbacks.
		std::vector<PxcxAppendedNode> Nodes;
		// incoming collection metadata is applied by the host after APPENDING ends.
		std::string MetadataJson;
	};
	// grug accept checked bake::ReadPxcx archives and append complete saved node records.
	// remap only source-backed identity fields.
	// names, expressions, cache membership strings and timeline references follow pinned APPEND policy.
	// destination globals, settings, animation regions and thumbnail stay owned by destination.
	// failure preserves result. this prepares files, not source runtime callbacks or licensed parity.
	[[nodiscard]] bool AppendPxcxProject(
		const bake::PxcxArchive &destination,
		const bake::PxcxArchive &incoming,
		const PxcxAppendOptions &options,
		PxcxAppendResult &result,
		imagegraph::Diagnostic &diagnostic
	);
}
