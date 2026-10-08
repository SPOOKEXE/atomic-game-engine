#include <engine/assets/LocalStore.hpp>

#include <cstdio>
#include <imgui.h>
#include <studio/Editor.hpp>
#include <studio/ImageComposer.hpp>

namespace studio {
	void Editor::ReleaseImageComposerPreview() {
		if (ImageComposer == nullptr) return;
		if (ImageComposer->PreviewOwner.IsValid()) Renderer.DropContentOwner(ImageComposer->PreviewOwner);
		ImageComposer->PreviewHandle = nullptr;
		ImageComposer->Attempt.clear();
		ReloadImageComposerSources(*ImageComposer);
	}
	void Editor::DrawImageComposer() {
		if (!ShowImageComposer) return;
		const auto paths = engine::assets::DefaultLocalPaths();
		if (ImageComposer == nullptr) {
			ImageComposer = std::make_unique<ImageComposerState>();
			ImageComposer->SourceRoot = paths.Raw;
			std::snprintf(
				ImageComposer->ProjectPath.data(),
				ImageComposer->ProjectPath.size(),
				"%s",
				(paths.Raw / "composer.imagegraph").string().c_str()
			);
			ImageComposer->PreviewOwner = engine::core::Name(
				"studio.composer/" + std::to_string(reinterpret_cast<uintptr_t>(ImageComposer.get()))
			);
			ImageComposer->PreviewNames = {
				engine::core::Name("studio.composer/preview-a"),
				engine::core::Name("studio.composer/preview-b")
			};
		}
		ImageComposerHost host;
		host.BakedRoot = paths.Baked;
		host.Upload =
			[this](const engine::assets::TextureData &texture, void *&handle, std::string &failure) {
				if (Renderer.Backend().Device == nullptr) {
					handle = nullptr;
					return true;
				}
				auto &state = *ImageComposer;
				const auto next = (state.PreviewSlot + 1) % state.PreviewNames.size();
				if (!Renderer.AddTexture(state.PreviewNames[next], texture, state.PreviewOwner)) {
					failure = "could not upload composer preview";
					return false;
				}
				handle = Renderer.TextureHandle(state.PreviewNames[next], state.PreviewOwner);
				state.PreviewSlot = next;
				return true;
			};
		host.Register = [this](std::span<const std::byte> bytes, const std::string &name) {
			RegisterBakedAsset(bytes, name);
			RefreshStoreContents();
		};
		host.Publish = [this] {
			if (AssetSigningKey[0] != '\0')
				PublishAssets(AssetSigningKey);
			else {
				ShowAssets = true;
				ImGui::SetWindowFocus("Assets");
			}
		};
		studio::DrawImageComposer(*ImageComposer, ShowImageComposer, host);
	}
}
