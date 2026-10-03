#pragma once
#include "ComposerCookResidency.hpp"

#include <engine/imagegraph/HostCapture.hpp>
namespace engine::render::detail {
	// Completed image bytes are output, so they do not alter the immutable
	// invocation identity.
	inline bool SameComposerCaptureInputs(
		const engine::imagegraph::HostNodeCapture &cached,
		const engine::imagegraph::HostNodeCapture &requested,
		int64_t cachedInterpolation,
		int64_t requestedInterpolation
	) {
		if (cachedInterpolation != requestedInterpolation || cached.Authored != requested.Authored ||
			cached.Tick != requested.Tick || cached.Subframe != requested.Subframe ||
			cached.NegativeFrame != requested.NegativeFrame || cached.Inputs != requested.Inputs ||
			cached.InputImages.size() != requested.InputImages.size())
			return false;
		for (size_t index = 0; index < cached.InputImages.size(); ++index)
			if (cached.InputImages[index].Port != requested.InputImages[index].Port ||
				cached.InputImages[index].Hash != requested.InputImages[index].Hash)
				return false;
		return true;
	}
} // namespace engine::render::detail
