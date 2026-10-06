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
	// grug keep the source metadata manager as host data, not invented per-node archive fields.
	struct PxcxCollectionMetadata {
		// fresh durable Collection identity.
		std::string NodeId;
		// eleven manager fields from constructor defaults and the root deserialize callback.
		std::string MetadataJson;
		bool operator==(const PxcxCollectionMetadata &) const = default;
	};
	// grug stage metadata and the source path without changing the prepared append or live host.
	struct PxcxAppendPostLoad {
		// one manager per incoming top-level Collection, in source declaration order.
		std::vector<PxcxCollectionMetadata> Collections;
		// present only when the single top-level Collection's source-backed path changed.
		std::optional<bake::PxcxArchive> Source;
	};
	// grug prepare constructor defaults for every Collection-family node in a checked archive.
	// Root project metadata and Collection paths are deliberately ignored.
	[[nodiscard]] bool PreparePxcxCollectionMetadata(
		const bake::PxcxArchive &archive,
		std::vector<PxcxCollectionMetadata> &result,
		imagegraph::Diagnostic &diagnostic,
		uint64_t maximumBytes = imagegraph::Limits::MaximumEvaluationBytes
	);
	// grug call after APPENDING callbacks end, before source, canvas and history publication.
	// prepare default managers with non-null root overrides and the optional observed append path.
	// failure preserves result; the caller retains the original native projection and Group captures.
	[[nodiscard]] bool PreparePxcxAppendPostLoad(
		const PxcxAppendResult &append,
		std::string_view sourcePath,
		PxcxAppendPostLoad &result,
		imagegraph::Diagnostic &diagnostic,
		uint64_t maximumBytes = imagegraph::Limits::MaximumEvaluationBytes
	);
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
