#pragma once

#include <engine/core/Name.hpp>

#include <cassert>
#include <cstddef>
#include <cstdint>

namespace engine::render {
	// Identifies the rendered image that may satisfy a preview request. Resource
	// names repeat across profiles and viewport slots, so all three fields are
	// part of the identity.
	struct ResourcePreviewRoute {
		core::Name Pipeline;
		core::Name Resource;
		size_t Slot = 0;

		bool operator==(const ResourcePreviewRoute &) const = default;
	};

	// Chooses which retained image the interface may read and which the renderer
	// may overwrite. The two must differ after the first image is published: an
	// ImGui draw list already holds the readable texture when the render graph
	// records the next preview update later in that frame.
	struct ResourcePreviewSlots {
		uint8_t Visible = 0;
		bool Ready = false;

		uint8_t Writable() const {
			return Ready ? static_cast<uint8_t>(Visible ^ 1u) : 0u;
		}

		void Publish(uint8_t slot) {
			assert(slot < 2);
			Visible = slot;
			Ready = true;
		}

		void Reset() {
			Visible = 0;
			Ready = false;
		}
	};

	// The four floats consumed by image.frag. Kept device-free so the shader
	// contract is covered without needing a GPU in a unit suite.
	struct ImageUniformMode {
		float SingleChannel = 0.0f;
		float ReverseSpectrum = 0.0f;
		float Reserved[2]{};
	};

	inline ImageUniformMode ImageMode(bool singleChannel, bool reverseSpectrum) {
		return ImageUniformMode{
			.SingleChannel = singleChannel ? 1.0f : 0.0f,
			.ReverseSpectrum = reverseSpectrum ? 1.0f : 0.0f,
		};
	}
}
