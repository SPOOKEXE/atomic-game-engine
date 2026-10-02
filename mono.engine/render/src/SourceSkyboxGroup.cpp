#include <engine/core/Profiling.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/render/SourceSkyboxGroup.hpp>
namespace engine::render::imagegraph {
	void CancelSourceSkybox(Renderer &renderer, SourceSkyboxGroup &group) {
		if (group.Request) {
			renderer.DropTransformImage3DOwner(group.Request->StagingOwner);
			renderer.DropContentOwner(group.Request->StagingOwner);
		}
		group = {};
	}
	SourceSkyboxStatus BeginSourceSkybox(
		Renderer &renderer, SourceSkyboxGroup &group, SourceSkyboxRequest &&request, std::string &diagnostic
	) {
		ENGINE_PROFILE("imagegraph.skybox.begin");
		const auto status = ValidateSourceSkybox(request, diagnostic);
		if (status != SourceSkyboxStatus::Pending) return status;
		CancelSourceSkybox(renderer, group);
		renderer.DropTransformImage3DOwner(request.StagingOwner);
		renderer.DropContentOwner(request.StagingOwner);
		group.Request = std::move(request);
		return SourceSkyboxStatus::Pending;
	}
	SourceSkyboxStatus
	RefreshSourceSkybox(Renderer &renderer, SourceSkyboxGroup &group, std::string &diagnostic) {
		ENGINE_PROFILE("imagegraph.skybox.refresh");
		if (!group.Request) {
			diagnostic = "skybox group has not been admitted";
			return SourceSkyboxStatus::Invalid;
		}
		if (group.Failed) {
			if (diagnostic.empty())
				diagnostic = "skybox generation failed; its authored input has not changed";
			return SourceSkyboxStatus::Failed;
		}
		if (group.Published) return SourceSkyboxStatus::Published;
		auto &request = *group.Request;
		std::array<SourceTextureStatus, 6> observed{};
		for (size_t i = 0; i < 6; ++i) {
			const auto &target = request.Targets[i];
			if (group.Ready[i]) {
				observed[i] = SourceTextureStatus::Ready;
				continue;
			}
			if (group.Admitted[i]) {
				observed[i] =
					renderer.SourceOutputStatus(request.StagingOwner, target.Name, target.Generation);
				continue;
			}
			const auto queued = std::visit(
				[&](const auto &face) {
					using T = std::decay_t<decltype(face)>;
					if constexpr (std::is_same_v<T, assets::TextureData>) {
						if (!renderer.StageSourceTexture(request.StagingOwner, target, face))
							return TransformImage3DQueueResult::Invalid;
						group.Ready[i] = true;
						return TransformImage3DQueueResult::Queued;
					} else if constexpr (std::is_same_v<T, SourceCamera3DRequest>)
						return renderer.QueueSourceCamera3D(
							{request.StagingOwner, target.Name, target.Generation, face}
						);
					else
						return renderer.QueueSourceSdf(
							{request.StagingOwner, target.Name, target.Generation, face}
						);
				},
				request.Faces[i]
			);
			if (queued == TransformImage3DQueueResult::Invalid) {
				group.Failed = true;
				for (auto &face : request.Faces)
					face = assets::TextureData{};
				diagnostic = "skybox face queue or upload was refused";
				break;
			}
			if (queued != TransformImage3DQueueResult::Full) {
				group.Admitted[i] = true;
				observed[i] =
					group.Ready[i]
						? SourceTextureStatus::Ready
						: renderer.SourceOutputStatus(request.StagingOwner, target.Name, target.Generation);
			}
		}
		const auto status = group.Failed ? SourceSkyboxStatus::Failed
										 : ObserveSourceSkybox(group.Admitted, group.Ready, observed);
		if (status == SourceSkyboxStatus::Failed) {
			renderer.DropTransformImage3DOwner(request.StagingOwner);
			renderer.DropContentOwner(request.StagingOwner);
			group.Failed = true;
			for (auto &face : request.Faces)
				face = assets::TextureData{};
			if (diagnostic.empty()) diagnostic = "skybox face generation ended without a complete output";
			return status;
		}
		if (status != SourceSkyboxStatus::ReadyToPublish) return status;
		if (!renderer.PromoteTextureGroup(request.StagingOwner, request.Owner, request.Targets)) {
			renderer.DropTransformImage3DOwner(request.StagingOwner);
			renderer.DropContentOwner(request.StagingOwner);
			group.Failed = true;
			for (auto &face : request.Faces)
				face = assets::TextureData{};
			diagnostic = "skybox atomic texture transfer was refused";
			return SourceSkyboxStatus::Failed;
		}
		renderer.DropTransformImage3DOwner(request.StagingOwner);
		renderer.DropContentOwner(request.StagingOwner);
		group.Published = true;
		// Requests and their source surfaces can retire now. Completion metadata remains in the client cache.
		for (auto &face : request.Faces)
			face = assets::TextureData{};
		diagnostic.clear();
		return SourceSkyboxStatus::Published;
	}
}
