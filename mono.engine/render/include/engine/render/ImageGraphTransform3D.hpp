#pragma once

// Typed CPU-facing contract for the render-owned Transform Image 3D pass.
// GPU device handles remain private to render.

#include <engine/assets/Texture.hpp>
#include <engine/core/Name.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace engine::render {
	class Renderer;
}

namespace engine::render::imagegraph {
	inline constexpr uint64_t MAXIMUM_TRANSFORM_IMAGE_3D_OUTPUT_BYTES = 64ull * 1024 * 1024;
	inline constexpr uint64_t MAXIMUM_TRANSFORM_IMAGE_3D_SCRATCH_BYTES = 128ull * 1024 * 1024;

	enum class TransformImage3DStatus : uint8_t {
		Ok,
		InvalidSurface,
		InvalidControl,
		OutputLimit,
		GpuUnavailable
	};
	enum class TransformImage3DProjection : uint8_t { Perspective, Orthographic };
	// Display applies sRGB reads and writes to normalized RGBA8 and RGBA4 formats.
	// Linear selects UNORM storage there; float and single-channel formats stay numeric.
	enum class TransformImage3DColorSpace : uint8_t { Linear, Display };

	struct TransformImage3DSurface {
		uint32_t Width = 0, Height = 0;
		assets::TextureFormat Format = assets::TextureFormat::RGBA8;
		std::vector<std::byte> Pixels;
	};

	// The fixed plane is supplied with the result so a graph consumer can carry
	// a typed mesh output without depending on render's private GPU vertex layout.
	struct TransformImage3DMesh {
		std::array<float, 18> Positions{};
		std::array<float, 12> TextureCoordinates{};
	};

	struct TransformImage3DRequest {
		TransformImage3DSurface Front, Back;
		std::array<float, 3> Position{0, 0, 0}, Anchor{0, 0, 0}, Scale{1, 1, 1};
		std::array<float, 4> Rotation{0, 0, 0, 1};
		std::array<float, 2> TextureTiling{1, 1}, ViewRange{.001f, 10}, DepthRange{0, 1};
		TransformImage3DProjection Projection = TransformImage3DProjection::Orthographic;
		TransformImage3DColorSpace ColorSpace = TransformImage3DColorSpace::Linear;
		float FieldOfViewDegrees = 45;
	};

	struct TransformImage3DResult {
		uint32_t Width = 0, Height = 0;
		TransformImage3DMesh Mesh;
		// RenderedPixels preserves the input numeric format. DepthRgba8 is the
		// fixed encoded-depth output and is always RGBA8 UNORM.
		assets::TextureFormat RenderedFormat = assets::TextureFormat::RGBA8;
		std::vector<std::byte> RenderedPixels, DepthRgba8;
		std::vector<float> Depth;
	};
	struct TransformImage3DLiveRequest {
		core::Name Owner;
		core::Name Name;
		uint64_t Generation = 0;
		TransformImage3DRequest Request;
	};
	enum class TransformImage3DQueueResult : uint8_t { Queued, Replaced, Duplicate, Invalid, Full };

	TransformImage3DStatus ValidateTransformImage3D(const TransformImage3DRequest &request);

	// Executes one bounded, synchronous export pass using an initialized renderer.
	// Live requests use the frame scheduler and keep their output resident.
	TransformImage3DStatus ExecuteTransformImage3D(
		Renderer &renderer, const TransformImage3DRequest &request, TransformImage3DResult &result
	);
}
