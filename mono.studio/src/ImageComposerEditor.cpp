#include <engine/assets/ContentPolicy.hpp>
#include <engine/assets/LocalStore.hpp>
#include <engine/bake/ImageGraph.hpp>
#include <engine/core/Profiling.hpp>

#include <algorithm>
#include <assetc/ImageGraph.hpp>
#include <cstdio>
#include <imgui.h>
#include <studio/Assets.hpp>
#include <studio/Editor.hpp>
#include <studio/ImageComposer.hpp>
#include <studio/PropertySelection.hpp>
#include <unordered_set>

namespace studio {
	void Editor::ReleaseImageComposerPreview() {
		ComposerApply = {};
		ImageGraphInputEdits.clear();
		if (ImageComposer == nullptr) return;
		if (ImageComposer->PreviewOwner.IsValid()) Renderer.DropContentOwner(ImageComposer->PreviewOwner);
		ImageComposer->UploadedSources.clear();
		ImageComposer->PreviewHandle = nullptr;
		ImageComposer->PreviewWidth = ImageComposer->PreviewHeight = 0;
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
				engine::core::Name("studio.composer/graph"), engine::core::Name("studio.composer/output")
			};
		}
		ImageComposerHost host;
		host.BakedRoot = paths.Baked;
		if (Renderer.Backend().Device != nullptr)
			host.Gpu = [this](
						   const engine::imagegraph::Document &document,
						   std::string_view output,
						   const engine::imagegraph::TypedSourceResolver &sources,
						   ImageComposerPreview &preview,
						   engine::imagegraph::Diagnostic &diagnostic
					   ) {
				using namespace engine;
				ENGINE_PROFILE_CAT("image composer GPU preview", core::ProfileCategory::Render);
				auto &state = *ImageComposer;
				imagegraph::Plan plan;
				if (!imagegraph::Compile(document, plan, diagnostic)) return false;
				const auto selected =
					std::find_if(document.Outputs.begin(), document.Outputs.end(), [&](const auto &item) {
						return item.Name == output;
					});
				if (selected == document.Outputs.end()) {
					diagnostic = {{}, "select a graph output"};
					return false;
				}
				std::vector<bool> needed(document.Nodes.size(), false);
				needed[plan.Outputs[static_cast<size_t>(selected - document.Outputs.begin())]] = true;
				for (auto index = plan.Order.rbegin(); index != plan.Order.rend(); ++index)
					if (needed[*index])
						for (const auto input : plan.Inputs[*index])
							needed[input] = true;
				std::unordered_set<std::string> wanted;
				for (size_t index = 0; index < document.Nodes.size(); ++index)
					if (needed[index])
						if (const auto *source =
								std::get_if<imagegraph::Source>(&document.Nodes[index].Value))
							wanted.insert(
								std::to_string(static_cast<unsigned>(source->Interpretation)) + ":" +
								source->Path
							);
				std::vector<render::ImageGraphSourceBinding> bindings;
				std::unordered_set<std::string> bound;
				for (size_t index = 0; index < document.Nodes.size(); ++index) {
					if (!needed[index]) continue;
					const auto *source = std::get_if<imagegraph::Source>(&document.Nodes[index].Value);
					if (source == nullptr) continue;
					const auto key =
						std::to_string(static_cast<unsigned>(source->Interpretation)) + ":" + source->Path;
					if (!bound.insert(key).second) continue;
					imagegraph::Image image;
					std::string failure;
					if (!sources(*source, image, failure)) {
						diagnostic = {document.Nodes[index].Id, failure};
						return false;
					}
					auto held = std::find_if(
						state.UploadedSources.begin(), state.UploadedSources.end(), [&](const auto &entry) {
							return entry.Key == key;
						}
					);
					if (held == state.UploadedSources.end())
						held = std::find_if(
							state.UploadedSources.begin(),
							state.UploadedSources.end(),
							[&](const auto &entry) { return !wanted.contains(entry.Key); }
						);
					if (held == state.UploadedSources.end()) {
						if (state.UploadedSources.size() >= imagegraph::Limits::MaximumNodes) {
							diagnostic = {{}, "source texture budget exceeded"};
							return false;
						}
						state.UploadedSources.push_back(
							{{},
							 core::Name(
								 "studio.composer/source-" + std::to_string(state.UploadedSources.size())
							 ),
							 0}
						);
						held = std::prev(state.UploadedSources.end());
					}
					const auto version = state.SourceVersions.at(key);
					if (held->Key != key || held->Version != version) {
						assets::TextureData texture;
						if (!bake::ImageGraphTexture(image, texture, failure) ||
							!Renderer.AddTexture(held->Texture, texture, state.PreviewOwner)) {
							diagnostic = {
								document.Nodes[index].Id, failure.empty() ? "source upload failed" : failure
							};
							return false;
						}
						held->Key = key;
						held->Version = version;
					}
					bindings.push_back(
						{source->Path, held->Texture, state.PreviewOwner, source->Interpretation}
					);
				}
				if (!Renderer.SetImageGraph(
						state.PreviewOwner, state.PreviewNames[0], document, bindings, diagnostic
					))
					return false;
				const auto evaluation = Renderer.EvaluateImageGraph(
					state.PreviewOwner, state.PreviewNames[0], output, state.PreviewNames[1], diagnostic
				);
				if (evaluation == render::ImageGraphEvaluation::Refused) return false;
				preview.Handle = Renderer.TextureHandle(state.PreviewNames[1], state.PreviewOwner);
				if (preview.Handle == nullptr ||
					!Renderer.TextureSize(
						state.PreviewNames[1], preview.Width, preview.Height, state.PreviewOwner
					)) {
					diagnostic = {{}, "GPU output is unavailable"};
					return false;
				}
				for (auto &source : state.UploadedSources)
					if (!wanted.contains(source.Key)) {
						(void)Renderer.DropTexture(source.Texture, state.PreviewOwner);
						source.Key.clear();
						source.Version = 0;
					}
				if (evaluation == render::ImageGraphEvaluation::Updated) ++VisualResourceRevision;
				return true;
			};
		host.Register = [this](std::span<const std::byte> bytes, const std::string &name) {
			RegisterBakedAsset(bytes, name);
			RefreshStoreContents();
		};
		host.LivePublish = [this, paths](
							   const engine::imagegraph::Document &document,
							   std::string_view name,
							   std::string &accepted,
							   std::string &failure
						   ) {
			if (AssetSigningKey[0] == '\0') {
				ShowAssets = true;
				failure = "enter a signing key in Assets before publishing";
				return false;
			}
			engine::bake::CookedImageGraph cooked;
			const auto &state = *ImageComposer;
			const auto project = state.SourceRoot / ".imagegraph-source-context";
			if (!assetc::CookImageGraphProject(
					state.SourceRoot,
					project,
					document,
					name,
					engine::assets::ContentPolicy::Process(engine::assets::ContentVerb::Handle),
					cooked,
					failure
				) ||
				!assetc::PublishCookedImageGraph(paths.Baked, cooked, failure))
				return false;
			if (!PublishAssets(AssetSigningKey)) {
				failure = AssetStatus;
				return false;
			}
			for (const auto &source : cooked.Sources) {
				engine::core::ByteWriter encoded;
				if (engine::assets::Texture::Write(encoded, source.Texture))
					RegisterBakedAsset(encoded.Bytes(), source.Name);
			}
			RebuildContentClients();
			accepted = cooked.Name;
			return true;
		};
		if (Universe != nullptr && SelectionWorld.IsValid() &&
			AuthorityOf(SelectionWorld) == EditAuthority::Authoritative) {
			Universe->Enter(SelectionWorld, [&](engine::ecs::Store &store) {
				for (const auto &group : BuildPropertySelection(store, Selection))
					if (group.Applicable == Selection.size())
						for (const auto &row : group.Rows)
							if (row.Descriptor->Writable &&
								row.Descriptor->Type == engine::ecs::PropertyType::Name &&
								ContentKindOfProperty(group.Owner, row.Descriptor->Spelling) ==
									engine::assets::AssetKind::Texture)
								host.ImageSlots.emplace_back(row.Descriptor->Spelling);
			});
			host.Apply = [this](std::string_view graph, std::string_view output, std::string_view property) {
				ComposerApply = {
					SelectionWorld,
					Selection,
					engine::core::Name(property),
					engine::core::Name(graph),
					engine::core::Name(output)
				};
			};
		}
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
