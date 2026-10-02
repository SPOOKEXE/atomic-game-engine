#pragma once

#include <engine/assets/Texture.hpp>
#include <engine/core/Name.hpp>
#include <engine/imagegraph/SourceSdf.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace engine::render {
	class Renderer;
}
namespace engine::render::imagegraph {
	enum class SourceSdfStatus : uint8_t { Ok, InvalidObject, InvalidControl, OutputLimit, GpuUnavailable };
	enum class SourceRaymarchMode : uint8_t { Render, Scatter, Cloud, Terrain };
	struct SourceScatterControls {
		float Seed = 0;
		std::array<float, 2> Grid{4, 4}, PositionOrigin{}, PositionOffset{}, ObjectScale{1, 1};
		std::array<float, 3> RotationOrigin{}, RotationMinimum{}, RotationMaximum{};
		bool operator==(const SourceScatterControls &) const = default;
	};
	struct SourceCloudControls {
		engine::imagegraph::Gradient Gradient;
		uint32_t GradientBlend = 0;
		std::optional<engine::imagegraph::Image> GradientMap;
		std::array<float, 4> GradientMapRange{0, 0, 1, 1};
		std::array<float, 3> Position{}, Rotation{};
		float ObjectScale = 1, Fov = 30;
		std::array<float, 2> ViewRange{0, 10};
		uint32_t Type = 0, Iteration = 10;
		float Density = 1, Threshold = .5f;
		bool Fog = false;
		float DetailScale = 1, DetailAttenuation = .5f;
		bool operator==(const SourceCloudControls &) const = default;
	};
	struct SourceTerrainControls {
		// Packed source atlases retain the original 4192 square layout.
		std::array<std::optional<engine::imagegraph::Image>, 4> Textures;
		std::array<float, 2> AtlasUvScale{1, 1};
		bool UseTexture = false;
		uint32_t Shape = 0, Tile = 0;
		float Thickness = 1, Time = 0;
		std::array<float, 3> Position{}, Rotation{};
		float ObjectScale = 1, Fov = 30;
		std::array<float, 2> ViewRange{0, 10};
		float DepthIntensity = 0;
		std::array<float, 4> Background{0, 0, 0, 1}, Ambient{1, 1, 1, 1};
		std::array<float, 3> SunPosition{0, 0, 1};
		float Shadow = 0;
		bool operator==(const SourceTerrainControls &) const = default;
	};
	// Source raymarch controls and owned inputs captured before GPU admission.
	struct SourceSdfRequest {
		SourceRaymarchMode Mode = SourceRaymarchMode::Render;
		engine::imagegraph::SdfValue Object;
		SourceScatterControls Scatter;
		SourceCloudControls Cloud;
		SourceTerrainControls Terrain;
		uint32_t Width = 0, Height = 0;
		// Source temporary atlas size with fixed 1024 cells and the source shader UV divisor.
		uint32_t TextureAtlasSize = 1024;
		// Primitive/combine previews use an 8192 shape atlas independently of environment size.
		uint32_t ShapeTextureAtlasSize = 0;
		assets::TextureFormat Format = assets::TextureFormat::RGBA8_LINEAR;
		std::array<float, 3> CameraRotation{30, 45, 0};
		float CameraScale = 1;
		uint32_t Projection = 0;
		float Fov = 30, OrthoScale = 5;
		std::array<float, 2> ViewRange{3, 6}, DepthRange{1, 10};
		float DepthIntensity = 0;
		bool DrawBackground = false;
		bool Render = true, TextureFiltering = false;
		std::array<float, 4> Background{0, 0, 0, 1};
		std::optional<engine::imagegraph::Image> Environment;
		bool EnvironmentInterpolation = false;
		float AmbientIntensity = .2f;
		bool UseLight = true;
		std::array<float, 3> LightPosition{-.4f, -.5f, 1};
		float LightIntensity = 1;
		std::array<float, 4> LightColor{1, 1, 1, 1};
		bool operator==(const SourceSdfRequest &) const = default;
	};
	struct SourceSdfResult {
		uint32_t Width = 0, Height = 0;
		assets::TextureFormat Format = assets::TextureFormat::RGBA8_LINEAR;
		std::vector<std::byte> Pixels;
	};
	struct SourceSdfLiveRequest {
		core::Name Owner, Name;
		uint64_t Generation = 0;
		SourceSdfRequest Request;
	};
	SourceSdfStatus ValidateSourceSdfRequest(const SourceSdfRequest &request);
	// Blocking export uses the same source shader and texture packing as live publication.
	SourceSdfStatus
	ExecuteSourceSdf(Renderer &renderer, const SourceSdfRequest &request, SourceSdfResult &result);
}
