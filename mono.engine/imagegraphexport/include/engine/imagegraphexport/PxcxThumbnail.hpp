#pragma once

#include <engine/bake/Pxcx.hpp>
#include <engine/imagegraph/Document.hpp>

namespace engine::imagegraphexport {
	// Explicitly replaces THMB in a checked, fully serialized save candidate using an immutable preview.
	// The caller attests that the preview belongs to this candidate. No graph evaluation or files occur.
	// Native 256-square nearest sampling follows source cover-crop geometry and straight RGBA8 conversion.
	// Graph JSON and META facts are unchanged. Default source-preserving saves do not call this helper.
	// maxBytes admits aggregate retained payload and conservative codec staging, not process heap.
	// The bake codec's JSON/vendor workspace remains independently bounded by its own format limits.
	[[nodiscard]] bool WritePxcxPreparedThumbnail(
		const engine::bake::PxcxArchive &checkedCandidate,
		const engine::imagegraph::Image &preparedPreview,
		std::vector<std::byte> &out,
		engine::imagegraph::Diagnostic &diagnostic,
		uint64_t maxBytes = engine::imagegraph::Limits::MaximumEvaluationBytes
	);
}
