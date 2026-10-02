#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <cstddef>
#include <span>

namespace engine::render::imagegraph {

	// SDL's Vulkan backend binds a 4096-byte range for each uniform slot. Keep source arrays intact
	// at these boundaries so every backend reads the same authored fields.
	inline constexpr size_t SOURCE_UNIFORM_SLOT_BYTES = 4096;
	inline constexpr std::array<size_t, 3> CAMERA_UNIFORM_CUTS{0, 2256, 5328};
	inline constexpr std::array<size_t, 3> SDF_UNIFORM_CUTS{0, 3872, 7712};

	constexpr bool SourceUniformBlocksFit(std::span<const size_t> cuts, size_t bytes) {
		if (cuts.empty() || cuts.front() != 0) return false;
		for (size_t i = 0; i < cuts.size(); ++i) {
			const size_t end = i + 1 < cuts.size() ? cuts[i + 1] : bytes;
			if (end <= cuts[i] || end - cuts[i] > SOURCE_UNIFORM_SLOT_BYTES) return false;
		}
		return true;
	}

	inline bool PushSourceFragmentUniforms(
		SDL_GPUCommandBuffer *command, std::span<const std::byte> bytes, std::span<const size_t> cuts
	) {
		if (!SourceUniformBlocksFit(cuts, bytes.size())) return false;
		for (size_t i = 0; i < cuts.size(); ++i) {
			const size_t end = i + 1 < cuts.size() ? cuts[i + 1] : bytes.size();
			SDL_PushGPUFragmentUniformData(
				command, uint32_t(i), bytes.data() + cuts[i], uint32_t(end - cuts[i])
			);
		}
		return true;
	}

	static_assert(SourceUniformBlocksFit(CAMERA_UNIFORM_CUTS, 6672));
	static_assert(SourceUniformBlocksFit(SDF_UNIFORM_CUTS, 10960));
	static_assert(SourceUniformBlocksFit(SDF_UNIFORM_CUTS, 11104));
}
