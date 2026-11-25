#include <engine/core/FrameGraph.hpp>
#include <engine/graph/ExecutionPlan.hpp>
#include <engine/graph/PipelineCatalogue.hpp>
#include <engine/graph/PipelineProfile.hpp>
#include <engine/graph/Schedule.hpp>
#include <engine/ui/Metrics.hpp>
#include <engine/ui/NodeGraphPainter.hpp>
#include <engine/ui/Theme.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <functional>
#include <imgui.h>
#include <string_view>
#include <studio/Editor.hpp>
#include <studio/RenderPipelineGraph.hpp>

namespace studio {

	namespace {
		// Microseconds for a pass from a renderer timing table, by name then kind.
		template <typename Timings>
		double RenderPassTiming(const Timings &timings, const engine::graph::ProfilePass &pass) {
			if (const auto named = timings.find(pass.Name.Id()); named != timings.end()) {
				return named->second;
			}
			if (const auto kind = timings.find(pass.Kind.Id()); kind != timings.end()) {
				return kind->second;
			}
			return 0.0;
		}

		bool ContainsInsensitive(std::string text, std::string wanted) {
			for (char &letter : text) {
				letter = static_cast<char>(std::tolower(static_cast<unsigned char>(letter)));
			}
			for (char &letter : wanted) {
				letter = static_cast<char>(std::tolower(static_cast<unsigned char>(letter)));
			}
			return text.find(wanted) != std::string::npos;
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
		if (RenderPipelineNeedsLoad(RenderPipelineTriedWorld, Active.Index)) {
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
					RenderPipelineHistory.Clear();
					RenderPipelineCanvas->Select({});
					RenderPipelineCanvas->GraphChanged();
					RenderPipelineFrameWanted = true;
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
			if (ImGui::BeginMenu("Edit")) {
				if (ImGui::MenuItem("Undo", nullptr, false, RenderPipelineHistory.CanUndo())) {
					UndoRenderPipeline();
				}
				if (ImGui::MenuItem("Redo", nullptr, false, RenderPipelineHistory.CanRedo())) {
					RedoRenderPipeline();
				}
				ImGui::Separator();
				if (ImGui::MenuItem(
						"Group Selection", nullptr, false, !RenderPipelineCanvas->Selection().empty()
					)) {
					RenderPipelineCanvas->GroupSelection("group");
				}
				ImGui::EndMenu();
			}
			if (ImGui::MenuItem("Fit")) {
				RenderPipelineCanvas->Frame();
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
			std::unordered_map<uint64_t, void *> textures;
			std::unordered_map<uint64_t, float> aspects;
			std::unordered_map<std::string, uint64_t> handles;
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
				const auto canvasNodes = RenderPipelineGraph.Nodes();
				for (size_t index = 0; index < canvasNodes.size(); index++) {
					const engine::ang::NodeInstance &canvasNode = canvasNodes[index];
					const std::string previewPort = RenderPipelinePreviewPort(canvasNode.Def);
					const engine::graph::Node *renderNode =
						previewGraph.Find(engine::graph::NodeId{static_cast<uint32_t>(index + 1)});
					if (previewPort.empty() || renderNode == nullptr) {
						continue;
					}
					const engine::ang::Value enabled = RenderPipelineParam(canvasNode, "preview.enabled");
					if (enabled.Get<bool>() == nullptr || !*enabled.Get<bool>()) {
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
							return output.Name.Text() == previewPort;
						}
					);
					const size_t output = static_cast<size_t>(port - kind->Outputs.begin());
					if (port == kind->Outputs.end() || output >= renderNode->Writes.size()) {
						continue;
					}
					const engine::graph::ResourceDesc *resource =
						previewGraph.FindResourceDesc(renderNode->Writes[output]);
					if (resource != nullptr && refreshPreviews && previewSourceReady) {
						const engine::ang::Value reverse =
							RenderPipelineParam(canvasNode, "preview.reverse-spectrum");
						Renderer.RefreshResourcePreview(
							RenderPipelineInstalledName,
							resource->Name,
							previewSlot,
							reverse.Get<bool>() != nullptr && *reverse.Get<bool>()
						);
					}
					void *texture = resource == nullptr
										? nullptr
										: Renderer.ResourcePreviewTexture(
											  RenderPipelineInstalledName, resource->Name, previewSlot
										  );
					if (texture != nullptr) {
						// Handles are stable per node so the painter can resolve them every frame.
						const uint64_t handle = index + 1;
						handles[canvasNode.Id] = handle;
						textures[handle] = texture;
						aspects[handle] = Renderer.ResourcePreviewAspect(
							RenderPipelineInstalledName, resource->Name, previewSlot
						);
					}
				}
				if (refreshPreviews) {
					engine::graph::CompiledGraph compiled;
					engine::core::Name compileOffender;
					if (previewGraph.Compile(compiled, compileOffender) == engine::graph::GraphStatus::Ok) {
						const uint32_t width = WorldTarget.IsValid() ? WorldTarget.Width : 1920;
						const uint32_t height = WorldTarget.IsValid() ? WorldTarget.Height : 1080;
						engine::graph::PipelineProfile profile =
							engine::graph::ProfilePipeline(previewGraph, compiled, width, height);
						for (engine::graph::ProfilePass &pass : profile.Passes) {
							pass.Elapsed = RenderPassTiming(Renderer.PassTimings(), pass);
							pass.Wall = RenderPassTiming(Renderer.PassWallTimes(), pass);
						}
						engine::ang::Plan plan;
						engine::ang::RunReport report;
						RenderPipelineFeedback(RenderPipelineGraph, previewGraph, profile, plan, report);
						RenderPipelineCanvas->SetRunReport(plan, report);
					}
				}
			}
			// A picture appearing, leaving or changing shape changes node sizes.
			if (handles != RenderPipelinePreviewHandles || aspects != RenderPipelinePreviewAspects) {
				RenderPipelinePreviewHandles = std::move(handles);
				RenderPipelinePreviewAspects = std::move(aspects);
				RenderPipelineCanvas->VisualsChanged();
			}
			RenderPipelinePreviewTextures = std::move(textures);
			const ImVec2 canvasSize = ImGui::GetContentRegionAvail();
			RenderPipelineCanvas->SetViewport({canvasSize.x, canvasSize.y});
			if (RenderPipelineFrameWanted) {
				RenderPipelineCanvas->Frame();
				RenderPipelineFrameWanted = false;
			}
			engine::ui::NodeGraphCanvas(
				"##render-pipeline-graph", *RenderPipelineCanvas, {0.0f, 0.0f}, [this](uint64_t handle) {
					const auto found = RenderPipelinePreviewTextures.find(handle);
					return found == RenderPipelinePreviewTextures.end()
							   ? ImTextureID{}
							   : reinterpret_cast<ImTextureID>(found->second);
				}
			);
			std::vector<engine::ang::EditRecord> records = RenderPipelineCanvas->TakeEdits();
			if (!records.empty()) {
				RenderPipelineHistory.Record(std::move(records));
				RenderPipelineEditing.clear();
				RenderPipelineDirty = true;
			}
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
				const std::string def = "render.pass." + std::string(spec.Kind.Text());
				const std::string made = RenderPipelineGraph.NewNodeId(def);
				const engine::core::Vector2 centre =
					RenderPipelineCanvas->ToGraph(RenderPipelineCanvas->Viewport() * 0.5f);
				if (ApplyRenderPipelineEdit(
						engine::ang::edit::AddNode{{.Id = made, .Def = def, .Position = centre}}
					)) {
					RenderPipelineCanvas->Select({made});
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
