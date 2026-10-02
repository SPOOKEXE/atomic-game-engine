#pragma once

// Pure byte-to-planar conversion for host-owned whole audio clips.

#include <engine/imagegraph/Document.hpp>

#include <cstddef>
#include <span>

namespace engine::imagegraph {
	// Native mixer formats and pinned source PCM formats intentionally have different sample semantics.
	enum class WavClipPolicy : uint8_t { Native, PixelComposer };

	// Bounds decoder workspace and planar output before allocation. Output is unchanged on failure.
	Status DecodeWavClip(
		std::span<const std::byte> bytes,
		WavClipPolicy policy,
		uint64_t byteBudget,
		AudioBit &clip,
		Diagnostic &diagnostic
	);
}
