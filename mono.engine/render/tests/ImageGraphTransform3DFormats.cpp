#include "../src/ImageGraphTransform3DFormats.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <vector>

TEST_SUITE_ID("engine.render.imagegraph_transform_3d_formats")
TEST_DEPENDS("engine.render.imagegraph_transform_3d_gpu")

TEST_CASE("Transform Image 3D color spaces select matching texture formats", "[render][imagegraph]") {
	using namespace engine;
	using namespace engine::render::imagegraph;
	CHECK(
		detail::TransformImage3DColourFormat(TransformImage3DColorSpace::Linear) ==
		SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM
	);
	CHECK(
		detail::TransformImage3DColourFormat(TransformImage3DColorSpace::Display) ==
		SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB
	);
	CHECK(detail::ValidTransformImage3DColorSpace(TransformImage3DColorSpace::Linear));
	CHECK(detail::ValidTransformImage3DColorSpace(TransformImage3DColorSpace::Display));
	CHECK_FALSE(detail::ValidTransformImage3DColorSpace(static_cast<TransformImage3DColorSpace>(255)));
	TransformImage3DRequest request;
	request.Front = {1, 1, assets::TextureFormat::RGBA8, std::vector<std::byte>(4)};
	request.ColorSpace = static_cast<TransformImage3DColorSpace>(255);
	CHECK(ValidateTransformImage3D(request) == TransformImage3DStatus::InvalidControl);
}

TEST_CASE("Transform Image 3D retains native numeric surface layouts", "[render][imagegraph]") {
	using namespace engine;
	using namespace engine::render::imagegraph;
	using detail::TransformImage3DBytesPerPixel;
	using detail::TransformImage3DFormat;
	CHECK(
		TransformImage3DFormat(assets::TextureFormat::RGBA16_FLOAT, TransformImage3DColorSpace::Display) ==
		SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT
	);
	CHECK(
		TransformImage3DFormat(assets::TextureFormat::RGBA32_FLOAT, TransformImage3DColorSpace::Linear) ==
		SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT
	);
	CHECK(
		TransformImage3DFormat(assets::TextureFormat::R16_FLOAT, TransformImage3DColorSpace::Display) ==
		SDL_GPU_TEXTUREFORMAT_R16_FLOAT
	);
	CHECK(
		TransformImage3DFormat(assets::TextureFormat::R32_FLOAT, TransformImage3DColorSpace::Linear) ==
		SDL_GPU_TEXTUREFORMAT_R32_FLOAT
	);
	CHECK(
		TransformImage3DBytesPerPixel(
			assets::TextureFormat::RGBA16_FLOAT, TransformImage3DColorSpace::Linear
		) == 8
	);
	CHECK(
		TransformImage3DBytesPerPixel(
			assets::TextureFormat::RGBA32_FLOAT, TransformImage3DColorSpace::Linear
		) == 16
	);
	CHECK(
		TransformImage3DBytesPerPixel(
			assets::TextureFormat::RGBA4_UNORM, TransformImage3DColorSpace::Display
		) == 4
	);
	CHECK(
		TransformImage3DFormat(static_cast<assets::TextureFormat>(255), TransformImage3DColorSpace::Linear) ==
		std::nullopt
	);
	CHECK(
		detail::ResolveTransformImage3DFormat(
			assets::TextureFormat::RGBA8, TransformImage3DColorSpace::Display
		) == assets::TextureFormat::RGBA8
	);
	CHECK(
		detail::ResolveTransformImage3DFormat(
			assets::TextureFormat::RGBA8, TransformImage3DColorSpace::Linear
		) == assets::TextureFormat::RGBA8_LINEAR
	);

	TransformImage3DRequest request;
	request.Front = {1, 1, assets::TextureFormat::RGBA16_FLOAT, std::vector<std::byte>(8)};
	CHECK(ValidateTransformImage3D(request) == TransformImage3DStatus::Ok);
	request.Front.Pixels.resize(4);
	CHECK(ValidateTransformImage3D(request) == TransformImage3DStatus::InvalidSurface);
}
