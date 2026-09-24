#pragma once

// Source-backed PXCX projection into an authored native image graph.

#include <engine/bake/Pxcx.hpp>
#include <engine/imagegraph/Document.hpp>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::imagegraphio {
	struct PxcxReferencePreview {
		uint32_t Width = 256;
		uint32_t Height = 256;
		// Borrowed from PxcxImport::Source, not evaluated graph pixels.
		std::span<const uint8_t> Rgba;
		uint64_t Hash = 0;
	};

	struct PxcxImport {
		// Native nodes are mapped only when all relevant controls are understood.
		imagegraph::Document Graph;
		// Checked archive remains the lossless source for unknown nodes and metadata.
		bake::PxcxArchive Source;
		// Each opaque node or unsupported source feature has an explicit reason.
		std::vector<imagegraph::Diagnostic> Diagnostics;
		size_t NativeNodes = 0;
		// The saved PXCX thumbnail, when present. The view lives as long as Source.
		std::optional<PxcxReferencePreview> ReferencePreview() const;
	};

	struct PxcxSubgraph {
		// An independent native document ending at one mapped image node.
		imagegraph::Document Graph;
		// Incoming opaque dependencies omitted from this partial graph.
		std::vector<imagegraph::Diagnostic> Cuts;
		imagegraph::Status Compilation = imagegraph::Status::InvalidValue;
		imagegraph::Diagnostic Diagnostic;
	};

	// Accepts only a checked in-memory PXCX archive. On failure, `out` is unchanged.
	bool ImportPxcxImageGraph(const bake::PxcxArchive &archive, PxcxImport &out, std::string &failure);

	// Extracts the mapped upstream closure and validates it with the native compiler.
	// An opaque dependency is cut and reported. A required cut leaves Compilation non-Ok.
	PxcxSubgraph ExtractPxcxNativeSubgraph(const PxcxImport &imported, std::string_view nodeId);

}
