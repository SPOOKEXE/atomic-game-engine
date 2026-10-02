#include "SourceCamera3D.hpp"
#include "SourceSdf.hpp"

#include <engine/core/Profiling.hpp>
#include <engine/render/ImageGraphTransform3D.hpp>
#include <engine/render/LiveImagePublisher.hpp>
#include <engine/render/SourceSkyboxGroup.hpp>
#include <engine/render/TextureTable.hpp>

#include <limits>
namespace engine::render::imagegraph {
	SourceSkyboxStatus InspectSourceSkyboxFace(
		const SourceSkyboxFace &faceValue, SourceSkyboxFaceInfo &info, std::string &diagnostic
	) {
		uint32_t width = 0, height = 0;
		assets::TextureFormat format{};
		uint64_t bytes = 0;
		const bool valid = std::visit(
			[&](const auto &face) {
				using T = std::decay_t<decltype(face)>;
				width = face.Width;
				height = face.Height;
				format = face.Format;
				if constexpr (std::is_same_v<T, assets::TextureData>) {
					bytes = face.Pixels.capacity() + face.Mips.capacity() * sizeof(std::vector<std::byte>) +
							face.FlipbookFrameDurations.capacity() * sizeof(float);
					return face.IsValid() && face.LevelCount() == 1 && face.FlipbookSide == 0;
				} else if constexpr (std::is_same_v<T, SourceCamera3DRequest>) {
					if (ValidateSourceCamera3D(face) != SourceCamera3DStatus::Ok ||
						face.Output == SourceCamera3DOutput::Depth ||
						face.Output == SourceCamera3DOutput::Normal ||
						face.Output == SourceCamera3DOutput::ViewNormal ||
						face.Output == SourceCamera3DOutput::Shadow ||
						face.Output == SourceCamera3DOutput::AmbientOcclusion)
						return false;
					bytes = SourceCamera3DSourceBytes(face);
					return true;
				} else {
					if (ValidateSourceSdfRequest(face) != SourceSdfStatus::Ok) return false;
					bytes = SourceSdfSourceBytes(face);
					return true;
				}
			},
			faceValue
		);
		const auto stride = TextureUploadBytesPerPixel(format);
		if (!valid || !stride || width == 0 || height == 0 || width > LiveImagePublisher::MAXIMUM_SIDE ||
			height > LiveImagePublisher::MAXIMUM_SIDE || format == assets::TextureFormat::RGBA8_LINEAR ||
			format == assets::TextureFormat::RGBA4_UNORM || format == assets::TextureFormat::R8 ||
			format == assets::TextureFormat::R16_FLOAT || format == assets::TextureFormat::R32_FLOAT) {
			diagnostic = "skybox face must be a valid still display image";
			return SourceSkyboxStatus::Invalid;
		}
		info = {bytes, uint64_t(width) * height * (*stride)};
		diagnostic.clear();
		return SourceSkyboxStatus::Pending;
	}

	SourceSkyboxStatus ValidateSourceSkybox(const SourceSkyboxRequest &request, std::string &diagnostic) {
		if (!request.Owner.IsValid() || !request.StagingOwner.IsValid() ||
			request.Owner == request.StagingOwner) {
			diagnostic = "skybox staging owner must be distinct and valid";
			return SourceSkyboxStatus::Invalid;
		}
		uint64_t retained = 0, outputBytes = 0;
		for (size_t i = 0; i < 6; ++i) {
			const auto &target = request.Targets[i];
			if (!target.Name.IsValid() || target.Generation == 0) {
				diagnostic = "skybox faces need valid names and generations";
				return SourceSkyboxStatus::Invalid;
			}
			for (size_t j = 0; j < i; ++j)
				if (request.Targets[j].Name == target.Name) {
					diagnostic = "skybox face names must be distinct";
					return SourceSkyboxStatus::Invalid;
				}
			SourceSkyboxFaceInfo info;
			const auto status = InspectSourceSkyboxFace(request.Faces[i], info, diagnostic);
			if (status != SourceSkyboxStatus::Pending) return status;
			const uint64_t bytes = info.RetainedBytes, output = info.OutputBytes;
			if (bytes > MAXIMUM_TRANSFORM_IMAGE_3D_OUTPUT_BYTES - retained ||
				output > MAXIMUM_SOURCE_SKYBOX_OUTPUT_BYTES - outputBytes) {
				diagnostic = "skybox group exceeds retained source or output byte cap";
				return SourceSkyboxStatus::OverLimit;
			}
			retained += bytes;
			outputBytes += output;
		}
		diagnostic.clear();
		return SourceSkyboxStatus::Pending;
	}
	SourceSkyboxStatus ObserveSourceSkybox(
		std::array<bool, 6> admitted, std::array<bool, 6> ready, std::array<SourceTextureStatus, 6> observed
	) {
		bool complete = true;
		for (size_t i = 0; i < 6; ++i) {
			if (!ready[i] && admitted[i] && observed[i] == SourceTextureStatus::Absent)
				return SourceSkyboxStatus::Failed;
			complete &= ready[i] || observed[i] == SourceTextureStatus::Ready;
		}
		return complete ? SourceSkyboxStatus::ReadyToPublish : SourceSkyboxStatus::Pending;
	}
}
