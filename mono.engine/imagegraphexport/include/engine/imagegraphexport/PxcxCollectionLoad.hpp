#pragma once

#include <engine/imagegraphio/PxcxAppend.hpp>

#include <filesystem>

namespace engine::imagegraphexport {
	// grug keep original collection texts beside their checked contextual archive.
	struct PxcxSourceRead {
		// grug checked native archive, derived only for legacy collection files.
		bake::PxcxArchive Archive;
		// full-size legacy PNG, empty when absent or explicitly skipped.
		imagegraph::Image Preview;
		// grug legacy bytes differ from the checked archive after contextual key loading.
		std::optional<imagegraphio::PxcxCollectionSave> Collection;
	};
	// grug read the granted file and, for .pxcc, its explicit .meta/.png siblings.
	// .pxz selects exactly basename.pxcc/.meta/.png in memory. no archive path is extracted.
	// PXCX bytes stay unchanged; legacy keys use supplied frame count, rate and playback context.
	// collection previews stay full size. append callers may skip unused preview decoding.
	// refusal preserves result. allowance bounds retained payload and format decode buffers.
	// bake JSON parser workspace keeps its independent format limits, not a full-process heap quota.
	[[nodiscard]] bool ReadPxcxSourceFile(
		const std::filesystem::path &path,
		const imagegraph::TimelineSettings &timeline,
		PxcxSourceRead &result,
		imagegraph::Diagnostic &diagnostic,
		uint64_t maximumBytes = imagegraph::Limits::MaximumEvaluationBytes,
		bool readPreview = true
	);
}
