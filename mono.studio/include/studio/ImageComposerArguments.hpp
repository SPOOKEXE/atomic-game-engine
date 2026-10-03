#pragma once
#include <engine/imagegraph/SourceArgumentHost.hpp>

namespace engine::render {
	class Renderer;
}
namespace studio {
	// Startup preparation is synchronous and must precede the first Composer draw.
	bool PrepareImageComposerArguments(
		const engine::imagegraph::SourceArgumentOptions &,
		engine::imagegraph::Diagnostic &,
		uint64_t maximumBytes = engine::imagegraph::Limits::MaximumEvaluationBytes
	);
	// A running replacement requires the actual renderer so admitted device work
	// can be retired before the new observation generation is used.
	bool PrepareImageComposerArguments(
		const engine::imagegraph::SourceArgumentOptions &,
		engine::render::Renderer &,
		engine::imagegraph::Diagnostic &,
		uint64_t maximumBytes = engine::imagegraph::Limits::MaximumEvaluationBytes
	);
}
