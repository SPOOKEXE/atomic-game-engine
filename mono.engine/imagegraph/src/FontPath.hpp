#pragma once
#include <engine/imagegraph/SourceFont.hpp>

namespace engine::imagegraph::detail {
	// Resolves only the captured source namespace and lexical prefixes; never opens a path.
	Status ResolveSourceFontPath(
		std::string_view,
		const SourceFontContext *,
		uint64_t maximumBytes,
		std::string &,
		std::string &failure
	);
}
