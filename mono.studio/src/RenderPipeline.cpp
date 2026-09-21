#include <engine/core/FrameGraph.hpp>
#include <engine/graph/ExecutionPlan.hpp>
#include <engine/graph/PipelineCatalogue.hpp>
#include <engine/graph/PipelineProfile.hpp>
#include <engine/graph/Schedule.hpp>
#include <engine/ui/Metrics.hpp>
#include <engine/ui/Theme.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <imgui.h>
#include <string_view>
#include <studio/Editor.hpp>
#include <studio/RenderPipelineGraph.hpp>

namespace studio {

	namespace {
		bool ContainsInsensitive(std::string text, std::string wanted) {
			for (char &letter : text) {
				letter = static_cast<char>(std::tolower(static_cast<unsigned char>(letter)));
			}
			for (char &letter : wanted) {
				letter = static_cast<char>(std::tolower(static_cast<unsigned char>(letter)));
			}
			return text.find(wanted) != std::string::npos;
		}

		bool IsImage(engine::graph::ResourceKind kind) {
			using engine::graph::ResourceKind;
			return kind == ResourceKind::Colour || kind == ResourceKind::Depth ||
				   kind == ResourceKind::Texture || kind == ResourceKind::Storage;
		}

		const engine::graph::ProfileResource *
		ProfileResourceOf(const engine::graph::PipelineProfile &profile, engine::graph::ResourceId wanted) {
			for (const engine::graph::ProfileResource &resource : profile.Resources) {
				if (resource.Id == wanted) {
					return &resource;
				}
			}
			return nullptr;
		}

		template <typename Timings>
		double PassTiming(const Timings &timings, const engine::graph::ProfilePass &pass) {
			if (const auto named = timings.find(pass.Name.Id()); named != timings.end()) {
				return named->second;
			}
			// The renderer records the display name, but the node kind is the
			// stable fallback for an older installed pipeline whose names differ.
			if (const auto kind = timings.find(pass.Kind.Id()); kind != timings.end()) {
				return kind->second;
			}
			return 0.0;
		}

		std::string RequirementsText(const engine::graph::NodeRequirements &needs) {
			std::string text;
			const auto append = [&text](std::string_view name) {
				if (!text.empty()) {
					text += ", ";
				}
				text += name;
			};
			if (needs.Compute) {
				append("compute");
			}
			if (needs.StorageTextures) {
				append("storage image");
			}
			if (needs.IndirectDraws) {
				append("indirect draw");
			}
			for (const engine::graph::ResourceFormat format : needs.Formats) {
				append(engine::graph::Describe(format));
			}
			if (needs.TimestampsUseful) {
				append("timestamps optional");
			}
			return text.empty() ? "none" : text;
		}

		template <typename Draw>
		bool DrawStageImages(
			const engine::graph::PipelineProfile &profile, const engine::graph::Node &node, Draw draw
		) {
			const std::vector<engine::graph::ResourceId> &images =
				node.Writes.empty() ? node.Reads : node.Writes;
			bool drewImage = false;
			for (const engine::graph::ResourceId resourceId : images) {
				const engine::graph::ProfileResource *resource = ProfileResourceOf(profile, resourceId);
				if (resource == nullptr || !IsImage(resource->Kind)) {
					continue;
				}
				drewImage = true;
				draw(*resource);
			}
			return drewImage;
		}
	}

	void Editor::DrawRenderPipeline() {
		if (!ShowRenderPipeline) {
			return;
		}
		// A hidden dock tab makes Begin return false. Keep every preview evaluation,
		// graph build, texture lookup, and GPU refresh below this gate.
		if (!ImGui::Begin("Render Pipeline", &ShowRenderPipeline, ImGuiWindowFlags_MenuBar)) {
			ImGui::End();
			return;
		}

		PrepareRenderPipelineCanvas();
		if (RenderPipelineWorld != Active || RenderPipelineGraph.Nodes().empty()) {
			LoadRenderPipeline(Active, {});
		}

		if (ImGui::BeginMenuBar()) {
			if (ImGui::BeginMenu("Profile")) {
				for (const engine::core::Name name : RenderingProfiles.Names()) {
					if (ImGui::MenuItem(
							std::string(name.Text()).c_str(), nullptr, name == RenderPipelineName
						)) {
						LoadRenderPipeline(Active, name);
					}
				}
				if (RenderingProfiles.Count() > 0) {
					ImGui::Separator();
				}
				if (ImGui::MenuItem("New Profile")) {
					RenderPipelineName = {};
					RenderPipelineBasis = engine::graph::DefaultPbrDocument();
					LoadRenderPipelineGraph(RenderPipelineBasis, RenderPipelineGraph, RenderPipelineStatus);
					RenderPipelineCanvas.Fit(RenderPipelineGraph);
					RenderPipelineDirty = true;
					RenderPipelineNewName[0] = '\0';
					RenderPipelineSaveAsWanted = true;
				}
				if (ImGui::MenuItem("Save Profile As...")) {
					RenderPipelineNewName[0] = '\0';
					RenderPipelineSaveAsWanted = true;
				}
				ImGui::EndMenu();
			}

			ImGui::BeginDisabled(!RenderPipelineDirty);
			if (ImGui::MenuItem("Save")) {
				SaveRenderPipeline();
			}
			ImGui::EndDisabled();
			if (ImGui::MenuItem("Fit")) {
				RenderPipelineCanvas.Fit(RenderPipelineGraph);
			}
			ImGui::Separator();
			ImGui::TextUnformatted("Preview");
			ImGui::SameLine();
			ImGui::SetNextItemWidth(engine::ui::Scaled(90.0f));
			if (ImGui::SliderFloat(
					"##render-pipeline-preview-fps", &RenderPipelinePreviewFps, 1.0f, 60.0f, "%.0f fps"
				)) {
				RenderPipelinePreviewNext = 0.0;
			}
			if (RenderPipelineDirty) {
				ImGui::SameLine();
				ImGui::TextDisabled("modified");
			}
			ImGui::EndMenuBar();
		}

		if (RenderPipelineSaveAsWanted) {
			ImGui::OpenPopup("Save Rendering Profile As");
			RenderPipelineSaveAsWanted = false;
		}
		if (ImGui::BeginPopupModal("Save Rendering Profile As", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
			ImGui::SetNextItemWidth(engine::ui::Scaled(280.0f));
			ImGui::InputText("Name", RenderPipelineNewName, sizeof(RenderPipelineNewName));
			const bool named = RenderPipelineNewName[0] != '\0';
			ImGui::BeginDisabled(!named);
			if (ImGui::Button("Save")) {
				RenderPipelineName = engine::core::Name(RenderPipelineNewName);
				RenderPipelineDirty = true;
				if (SaveRenderPipeline()) {
					ImGui::CloseCurrentPopup();
				}
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			if (ImGui::Button("Cancel")) {
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}

		const float side = engine::ui::Scaled(280.0f);
		const ImVec2 room = ImGui::GetContentRegionAvail();
		if (ImGui::BeginChild("##render-pipeline-canvas", ImVec2(std::max(room.x - side, side), room.y))) {
			RenderPipelinePreviewEvaluator.Run(RenderPipelineGraph);
			RenderPipelinePreviewTextures.clear();
			RenderPipelinePreviewAspects.clear();
			const double previewNow = Clock.Now();
			const bool refreshPreviews = previewNow >= RenderPipelinePreviewNext;
			if (refreshPreviews) {
				RenderPipelinePreviewNext = previewNow + 1.0 / std::max(RenderPipelinePreviewFps, 1.0f);
			}
			engine::graph::PipelineDocument previewDocument;
			engine::graph::RenderGraph previewGraph;
			engine::core::Name previewOffender;
			std::string previewError;
			const auto renderedSlot = RenderPipelineRenderedSlots.find(RenderPipelineWorld.Index);
			const auto installedPipeline = PipelineSelected.find(RenderPipelineWorld.Index);
			const bool previewSourceReady = renderedSlot != RenderPipelineRenderedSlots.end() &&
											installedPipeline != PipelineSelected.end() &&
											installedPipeline->second == RenderPipelineInstalledName;
			const size_t previewSlot = previewSourceReady ? renderedSlot->second : 0;
			if (SaveRenderPipelineGraph(
					RenderPipelineGraph, RenderPipelineBasis, previewDocument, previewError
				) &&
				engine::graph::Build(previewDocument, previewGraph, previewOffender) ==
					engine::graph::PipelineDocumentStatus::Ok) {
				for (size_t index = 0; index < RenderPipelineGraph.Nodes().size(); index++) {
					const nodegraph::Node &canvasNode = RenderPipelineGraph.Nodes()[index];
					const nodegraph::NodeType *canvasType = nodegraph::NodeTypes::Find(canvasNode.Type);
					const engine::graph::Node *renderNode =
						previewGraph.Find(engine::graph::NodeId{static_cast<uint32_t>(index + 1)});
					if (canvasType == nullptr || canvasType->PreviewPort.empty() || renderNode == nullptr) {
						continue;
					}
					const auto previewEnabled = canvasNode.Widgets.find("preview.enabled");
					if (previewEnabled == canvasNode.Widgets.end() || !previewEnabled->second.Flag) {
						continue;
					}
					const engine::graph::NodeKindSpec *kind =
						engine::graph::NodeCatalogue::Find(renderNode->Kind);
					if (kind == nullptr) {
						continue;
					}
					const auto port = std::find_if(
						kind->Outputs.begin(),
						kind->Outputs.end(),
						[&](const engine::graph::PortSpec &output) {
							return output.Name.Text() == canvasType->PreviewPort;
						}
					);
					const size_t output = static_cast<size_t>(port - kind->Outputs.begin());
					if (port == kind->Outputs.end() || output >= renderNode->Writes.size()) {
						continue;
					}
					const engine::graph::ResourceDesc *resource =
						previewGraph.FindResource(renderNode->Writes[output]);
					if (resource != nullptr && refreshPreviews && previewSourceReady) {
						const auto reverse = canvasNode.Widgets.find("preview.reverse-spectrum");
						Renderer.RefreshResourcePreview(
							RenderPipelineInstalledName,
							resource->Name,
							previewSlot,
							reverse != canvasNode.Widgets.end() && reverse->second.Flag
						);
					}
					void *texture = resource == nullptr
										? nullptr
										: Renderer.ResourcePreviewTexture(
											  RenderPipelineInstalledName, resource->Name, previewSlot
										  );
					if (texture != nullptr) {
						const uint64_t key = nodegraph::PictureKey(
							RenderPipelinePreviewEvaluator.RanAt(canvasNode.Id), canvasType->PreviewPort
						);
						RenderPipelinePreviewTextures[key] = texture;
						RenderPipelinePreviewAspects[key] = Renderer.ResourcePreviewAspect(
							RenderPipelineInstalledName, resource->Name, previewSlot
						);
					}
				}
			}
			RenderPipelineCanvas.Draw(RenderPipelineGraph);
		}
		ImGui::EndChild();

		ImGui::SameLine();
		if (ImGui::BeginChild("##render-pipeline-side", ImVec2(0.0f, room.y), ImGuiChildFlags_Borders)) {
			if (ImGui::BeginTabBar("##render-pipeline-tabs")) {
				if (ImGui::BeginTabItem("Add")) {
					DrawRenderPipelineLibrary();
					ImGui::EndTabItem();
				}
				if (ImGui::BeginTabItem("Node")) {
					DrawRenderPipelineInspector();
					ImGui::EndTabItem();
				}
				if (ImGui::BeginTabItem("Schedule")) {
					DrawRenderPipelineSchedule();
					ImGui::EndTabItem();
				}
				if (ImGui::BeginTabItem("Lighting")) {
					DrawRenderPipelineLighting();
					ImGui::EndTabItem();
				}
				ImGui::EndTabBar();
			}
			ImGui::Separator();
			ImGui::PushStyleColor(
				ImGuiCol_Text, RenderPipelineDirty ? engine::ui::WarningColour() : engine::ui::MutedColour()
			);
			ImGui::TextWrapped("%s", RenderPipelineStatus.c_str());
			ImGui::PopStyleColor();
		}
		ImGui::EndChild();

		// A valid gesture updates the world's component immediately. Invalid and
		// half-wired states remain editable on the canvas and keep the last valid
		// world document installed.
		if (RenderPipelineDirty) {
			SaveRenderPipeline();
		}
		ImGui::End();
	}

	void Editor::DrawRenderPipelineLibrary() {
		ImGui::SetNextItemWidth(-1.0f);
		ImGui::InputTextWithHint(
			"##render-pipeline-filter", "filter passes", RenderPipelineFilter, sizeof(RenderPipelineFilter)
		);
		const std::string wanted = RenderPipelineFilter;

		if (!ImGui::BeginTable(
				"##render-pipeline-kinds", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp
			)) {
			return;
		}
		ImGui::TableSetupColumn("Pass", ImGuiTableColumnFlags_WidthStretch, 0.45f);
		ImGui::TableSetupColumn("Requirements", ImGuiTableColumnFlags_WidthStretch, 0.55f);
		ImGui::TableHeadersRow();

		for (const engine::graph::NodeKindSpec &spec : engine::graph::NodeCatalogue::All()) {
			const std::string title = spec.Label.empty() ? std::string(spec.Kind.Text()) : spec.Label;
			if (!wanted.empty() &&
				!ContainsInsensitive(
					title + " " + std::string(spec.Kind.Text()) + " " + spec.Summary, wanted
				)) {
				continue;
			}
			const engine::render::CapabilityCheck capability =
				engine::render::CheckCapabilities(Renderer.Capabilities(), spec.Needs);
			const std::string requirements = RequirementsText(spec.Needs);
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::PushID(static_cast<int>(spec.Kind.Id()));
			if (ImGui::Selectable(
					title.c_str(), false, capability.Accepted() ? 0 : ImGuiSelectableFlags_Disabled
				)) {
				const nodegraph::NodeId made =
					RenderPipelineGraph.Add("render.pass." + std::string(spec.Kind.Text()), 0.0f, 0.0f);
				if (made != nodegraph::NO_NODE) {
					RenderPipelineCanvas.Select(made);
					RenderPipelineCanvas.Centre(RenderPipelineGraph, made);
					RenderPipelineDirty = true;
				}
			}
			if (ImGui::IsItemHovered()) {
				if (capability.Accepted()) {
					ImGui::SetTooltip("%s", spec.Summary.c_str());
				} else if (capability.Status == engine::render::CapabilityStatus::MissingFormat) {
					ImGui::SetTooltip(
						"%s: %s",
						engine::render::Describe(capability.Status),
						engine::graph::Describe(capability.Format)
					);
				} else {
					ImGui::SetTooltip("%s", engine::render::Describe(capability.Status));
				}
			}
			ImGui::TableSetColumnIndex(1);
			ImGui::TextDisabled("%s", requirements.c_str());
			ImGui::PopID();
		}
		ImGui::EndTable();
	}

	void Editor::DrawRenderPipelineLighting() {
		if (Universe == nullptr || !Active.IsValid()) {
			ImGui::TextDisabled("no active world");
			return;
		}

		const engine::world::WorldSettings settings = Universe->SettingsOf(Active);
		ImGui::Text("World: %s", settings.Name.IsValid() ? settings.Name.Text().data() : "(unnamed)");
		ImGui::Separator();

		engine::core::Name selected;
		const char *current = settings.RenderingProfile.IsValid() ? settings.RenderingProfile.Text().data()
																  : "(renderer default)";
		ImGui::SetNextItemWidth(-1.0f);
		if (ImGui::BeginCombo("Rendering Profile", current)) {
			for (const engine::core::Name name : RenderingProfiles.Names()) {
				if (ImGui::Selectable(name.Text().data(), name == settings.RenderingProfile)) {
					selected = name;
				}
			}
			ImGui::EndCombo();
		}

		if (selected.IsValid() && selected != settings.RenderingProfile &&
			Universe->SetRenderingProfile(Active, selected) == engine::world::WorldStatus::Ok) {
			PipelineSelected.erase(Active.Index);
			MarkModified();
		}

		if (RenderingProfiles.Find(settings.RenderingProfile) == nullptr) {
			ImGui::TextColored(
				ImVec4{0.95f, 0.75f, 0.35f, 1.0f},
				"The selected profile is missing. The renderer will use its fallback."
			);
		}

		if (ImGui::Button("Edit Rendering Profiles")) {
			LoadRenderPipeline(Active, settings.RenderingProfile);
		}
		ImGui::TextDisabled("Lighting values remain properties of this world's Lighting service.");
	}

}
