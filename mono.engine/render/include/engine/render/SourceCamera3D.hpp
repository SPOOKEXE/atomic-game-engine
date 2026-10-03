#pragma once

#include <engine/assets/Texture.hpp>
#include <engine/core/Name.hpp>
#include <engine/imagegraph/HostCapture.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace engine::render {
	class Renderer;
}
namespace engine::render::imagegraph {
	enum class SourceCamera3DOutput : uint8_t {
		Rendered,
		Diffuse,
		Normal,
		ViewNormal,
		Depth,
		Shadow,
		AmbientOcclusion
	};
	enum class SourceCamera3DStatus : uint8_t {
		Ok,
		InvalidScene,
		InvalidControl,
		OutputLimit,
		GpuUnavailable
	};
	struct SourceCamera3DRequest {
		engine::imagegraph::SceneValue3D Scene;
		uint32_t Width = 0, Height = 0;
		assets::TextureFormat Format = assets::TextureFormat::RGBA8_LINEAR;
		// Column-major source matrices, with clip depth mapped to SDL's zero-to-one range.
		std::array<float, 16> View{}, Projection{};
		std::array<float, 3> CameraPosition{};
		float Near = 1, Far = 10;
		// Resolved shader: zero is source Phong, one is source PBR.
		uint32_t Shader = 0, CullMode = 2, BlendMode = 0, WireMode = 0;
		std::array<float, 4> AmbientLight{.25f, .25f, .25f, 1}, WireColor{0, 0, 0, 1},
			BackfaceBlending{1, 1, 1, 1};
		std::optional<engine::imagegraph::Image> Environment;
		bool ShowBackground = false, GammaAdjust = false, WireAntialias = false, WireShading = false,
			 WireOnly = false;
		bool AmbientOcclusion = false, SwapViewNormalX = false;
		float AlphaThreshold = 0, WireThickness = 1, AoStrength = 1, AoRadius = .25f, AoBias = .05f;
		uint32_t AoBlur = 5, RoundNormal = 0;
		SourceCamera3DOutput Output = SourceCamera3DOutput::Rendered;
		bool operator==(const SourceCamera3DRequest &) const = default;
	};
	inline constexpr uint64_t MAXIMUM_CAMERA_HOST_BYTES = 64ull * 1024 * 1024;
	std::optional<uint64_t> SourceCamera3DRetainedBytes(const SourceCamera3DRequest &);
	bool BuildSourceCamera3DRequest(
		const engine::imagegraph::HostNodeInvocation &,
		std::string_view outputPort,
		bool displayColorSpace,
		SourceCamera3DRequest &,
		std::string &failure
	);
	bool BuildSourceCamera3DRequest(
		const engine::imagegraph::Node &,
		const engine::imagegraph::EvaluationSnapshot &,
		const engine::imagegraph::SourceCameraEvaluationPolicy &,
		std::string_view outputPort,
		bool displayColorSpace,
		SourceCamera3DRequest &,
		engine::imagegraph::Diagnostic &,
		uint64_t maximumBytes = MAXIMUM_CAMERA_HOST_BYTES
	);
	struct SourceCamera3DResult {
		uint32_t Width = 0, Height = 0;
		assets::TextureFormat Format = assets::TextureFormat::RGBA8_LINEAR;
		std::vector<std::byte> Pixels;
	};
	SourceCamera3DStatus ValidateSourceCamera3D(
		const SourceCamera3DRequest &request, uint64_t maximumPreparationBytes = 128ull * 1024 * 1024
	);
	SourceCamera3DStatus ExecuteSourceCamera3D(
		Renderer &renderer, const SourceCamera3DRequest &request, SourceCamera3DResult &result
	);
	struct SourceCamera3DLiveRequest {
		core::Name Owner, Name;
		uint64_t Generation = 0;
		SourceCamera3DRequest Request;
	};
}
