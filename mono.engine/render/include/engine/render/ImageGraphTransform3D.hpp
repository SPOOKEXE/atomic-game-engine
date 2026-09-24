#pragma once

// Typed CPU-facing contract for the render-owned Transform Image 3D pass.
// GPU device handles remain private to render.

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

	struct TransformImage3DSurface {
		uint32_t Width = 0, Height = 0;
		std::vector<std::byte> Rgba8;
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
		float FieldOfViewDegrees = 45;
	};

	struct TransformImage3DResult {
		uint32_t Width = 0, Height = 0;
		TransformImage3DMesh Mesh;
		std::vector<std::byte> RenderedRgba8, DepthRgba8;
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
	// This is for headless export and tests. A live frame scheduler owns asynchronous
	// submission and resident outputs when that path is added.
	TransformImage3DStatus ExecuteTransformImage3D(
		Renderer &renderer, const TransformImage3DRequest &request, TransformImage3DResult &result
	);
}
