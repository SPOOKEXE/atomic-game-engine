#pragma once
#include <engine/assets/Texture.hpp>
#include <engine/core/Name.hpp>
#include <engine/render/SourceCamera3D.hpp>
#include <engine/render/SourceSdf.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>

namespace engine::render {
	class Renderer;
	enum class SourceTextureStatus : uint8_t { Absent, Pending, Ready };
	struct SourceTexturePublication {
		core::Name Name;
		uint64_t Generation = 0;
	};
	namespace imagegraph {
		using SourceSkyboxFace = std::variant<assets::TextureData, SourceCamera3DRequest, SourceSdfRequest>;
		struct SourceSkyboxRequest {
			core::Name Owner, StagingOwner;
			std::array<SourceTexturePublication, 6> Targets;
			std::array<SourceSkyboxFace, 6> Faces;
		};
		enum class SourceSkyboxStatus : uint8_t {
			Invalid,
			OverLimit,
			Pending,
			ReadyToPublish,
			Published,
			Failed
		};
		inline constexpr uint64_t MAXIMUM_SOURCE_SKYBOX_OUTPUT_BYTES = 96ull * 1024 * 1024;
		struct SourceSkyboxGroup {
			std::optional<SourceSkyboxRequest> Request;
			std::array<bool, 6> Admitted{}, Ready{};
			bool Failed = false, Published = false;
		};
		struct SourceSkyboxFaceInfo {
			uint64_t RetainedBytes = 0, OutputBytes = 0;
		};
		SourceSkyboxStatus
		InspectSourceSkyboxFace(const SourceSkyboxFace &, SourceSkyboxFaceInfo &, std::string &diagnostic);
		// Source requests stay owned until their group completes; admission precedes moving into retained
		// state.
		SourceSkyboxStatus ValidateSourceSkybox(const SourceSkyboxRequest &, std::string &diagnostic);
		// The same completion rule is used by the real queue adapter and the headless suite.
		SourceSkyboxStatus ObserveSourceSkybox(
			std::array<bool, 6> admitted,
			std::array<bool, 6> ready,
			std::array<SourceTextureStatus, 6> observed
		);
		SourceSkyboxStatus
		BeginSourceSkybox(Renderer &, SourceSkyboxGroup &, SourceSkyboxRequest &&, std::string &diagnostic);
		SourceSkyboxStatus RefreshSourceSkybox(Renderer &, SourceSkyboxGroup &, std::string &diagnostic);
		void CancelSourceSkybox(Renderer &, SourceSkyboxGroup &);
	}
}
