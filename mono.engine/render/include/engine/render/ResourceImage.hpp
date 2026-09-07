#pragma once

#include <engine/core/Name.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace engine::render {
	enum class ResourceImageDelivery : uint8_t { CopiedPixels, Resident };
	// Process-local request for one declared capture node. Tokens must be unique
	// until the result is taken or cancellation completes. No GPU handle escapes.
	struct ResourceImageRequest {
		uint64_t Token = 0;
		core::Name Pipeline{};
		core::Name Node{};
		size_t ViewSlot = 0;
		ResourceImageDelivery Delivery = ResourceImageDelivery::CopiedPixels;
	};

	enum class ResourceImageStatus : uint8_t { Ok, Unsupported, Failed };

	struct ResourceImage {
		ResourceImageRequest Request;
		core::Name Resource;
		ResourceImageStatus Status = ResourceImageStatus::Failed;
		// Renderer-local frame that actually executed the capture, zero if it did
		// not run. Compare within one live renderer only, never as a world tick.
		uint64_t CaptureFrame = 0;
		uint32_t Width = 0;
		uint32_t Height = 0;
		uint32_t RowStride = 0;
		// Owned top-left, tightly packed linear RGBA16F, in little-endian order.
		std::vector<std::byte> Pixels;
		// Optional declared R32F capture input, copied in the same submission.
		// Tight top-left little-endian float32 rows; sample semantics belong to
		// the named resource. The built-in linear-depth resource clears to FarPlane.
		core::Name DepthResource;
		std::vector<std::byte> Depth;
	};
}
