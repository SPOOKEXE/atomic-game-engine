#pragma once

#include <engine/core/Name.hpp>
#include <engine/core/types/AABB.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
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
	enum class ResourceImageKind : uint8_t { Colour, DirectionalShadow };
	// Transfer allocations retained by all capture slots share this bound.
	inline constexpr size_t MAX_RESOURCE_IMAGE_STAGING_BYTES = 32 * 1024 * 1024;
	struct ResourceShadowCapture {
		// Empty sources have canonical zero SourceBounds, not the native fitting fallback.
		bool SourceEmpty = false;
		core::AABB SourceBounds{};
		core::AABB DomainBounds{};
		// Exact column-major matrix used by the producing shadow pass.
		std::array<float, 16> LightViewProjection{};
	};

	struct ResourceImage {
		ResourceImageRequest Request;
		core::Name Resource;
		ResourceImageStatus Status = ResourceImageStatus::Failed;
		ResourceImageKind Kind = ResourceImageKind::Colour;
		std::optional<ResourceShadowCapture> Shadow;
		// Renderer-local frame that actually executed the capture, zero if it did
		// not run. Compare within one live renderer only, never as a world tick.
		uint64_t CaptureFrame = 0;
		uint32_t Width = 0;
		uint32_t Height = 0;
		uint32_t RowStride = 0;
		// Owned top-left, tightly packed linear RGBA16F, in little-endian order.
		std::vector<std::byte> Pixels;
		// Optional declared R32F capture input, copied in the same submission.
		// DirectionalShadow instead owns only this plane: exact D32F device depth,
		// with RowStride == Width * 4 and matching Shadow metadata.
		// Tight top-left little-endian float32 rows; sample semantics belong to
		// the named resource. The built-in linear-depth resource clears to FarPlane.
		core::Name DepthResource;
		std::vector<std::byte> Depth;
		// Optional retained ambient inputs, present together with depth. Normal is
		// packed RGB10A2_UNORM; response and baseline are tight little-endian RGBA32F.
		core::Name NormalResource;
		core::Name AmbientResponseResource;
		core::Name LightingBaselineResource;
		core::Name DirectionalResponseResource;
		std::vector<std::byte> Normal;
		std::vector<std::byte> AmbientResponse;
		std::vector<std::byte> LightingBaseline;
		// Optional directional-light derivative and original shadow visibility.
		std::vector<std::byte> DirectionalResponse;
	};
}
