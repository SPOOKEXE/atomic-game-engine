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

	void Editor::LoadRenderPipeline(WorldId world, engine::core::Name wanted) {
		if (Universe == nullptr || !world.IsValid()) {
			RenderPipelineGraph.Clear();
			RenderPipelineWorld = {};
			RenderPipelineStatus = "no active world";
			return;
		}

		engine::graph::PipelineDocument document;
		engine::core::Name selected = wanted;
		if (!selected.IsValid()) {
			selected = Universe->SettingsOf(world).RenderingProfile;
		}
		if (RenderingProfiles.Count() == 0) {
			RenderingProfiles.Set(engine::core::Name("Default PBR"), engine::graph::DefaultPbrDocument());
		}
		if (RenderingProfiles.Find(selected) == nullptr) {
			selected = RenderingProfiles.Find(engine::core::Name("Default PBR")) != nullptr
						   ? engine::core::Name("Default PBR")
						   : RenderingProfiles.Names().front();
		}
		document = *RenderingProfiles.Find(selected);

		RenderPipelineWorld = world;
		RenderPipelineName = selected;
		RenderPipelineInstalledName = engine::core::Name(
			std::string(selected.Text()) + "#" + std::to_string(world.Index)
		);
		RenderPipelineBasis = document;
		RenderPipelineLoaded = engine::graph::Write(document);
		RenderPipelineDirty = false;

		if (!LoadRenderPipelineGraph(document, RenderPipelineGraph, RenderPipelineStatus)) {
			return;
		}
		RenderPipelineStatus = "loaded " + std::string(selected.Text());
		RenderPipelineCanvas.Select(nodegraph::NO_NODE);
		RenderPipelineCanvas.Fit(RenderPipelineGraph);
	}

	bool Editor::SaveRenderPipeline() {
		engine::graph::PipelineDocument saved;
		if (!SaveRenderPipelineGraph(RenderPipelineGraph, RenderPipelineBasis, saved, RenderPipelineStatus)) {
			return false;
		}
		if (Universe == nullptr || !RenderPipelineWorld.IsValid() || !RenderPipelineName.IsValid()) {
			RenderPipelineStatus = "no universe to save into";
			return false;
		}

		RenderingProfiles.Set(RenderPipelineName, saved);
		RenderPipelineInstalledName = engine::core::Name(
			std::string(RenderPipelineName.Text()) + "#" + std::to_string(RenderPipelineWorld.Index)
		);
		PipelineSelected.clear();
		MarkModified();

		RenderPipelineBasis = saved;
		RenderPipelineLoaded = engine::graph::Write(saved);
		RenderPipelineDirty = false;
		RenderPipelineStatus = "saved " + std::string(RenderPipelineName.Text()) + " into the universe";
		return true;
	}

	void Editor::DrawRenderPipeline() {
		if (!ShowRenderPipeline) {
			return;
		}
		if (!ImGui::Begin("Render Pipeline", &ShowRenderPipeline, ImGuiWindowFlags_MenuBar)) {
			ImGui::End();
			return;
		}

		if (!RenderPipelineCanvasReady) {
			RegisterRenderPipelineNodeTypes();
			RenderPipelineCanvas.Observe(&RenderPipelinePreviewEvaluator);
			RenderPipelineCanvas.Images(
				[this](uint64_t key, const std::function<bool(nodegraph::PreviewImage &)> &) {
					const auto found = RenderPipelinePreviewTextures.find(key);
					return found == RenderPipelinePreviewTextures.end() ? nullptr : found->second;
				}
			);
			RenderPipelineCanvas.Signals.Changed = [this] { RenderPipelineDirty = true; };
			RenderPipelineCanvasReady = true;
		}
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
				if (ImGui::MenuItem("Save Profile As...")) {
					RenderPipelineNewName[0] = '\0';
					RenderPipelineSaveAsWanted = true;
				}
				if (ImGui::MenuItem("Reset to Default PBR")) {
					RenderPipelineName = engine::core::Name("Default PBR");
					RenderPipelineBasis = engine::graph::DefaultPbrDocument();
					LoadRenderPipelineGraph(RenderPipelineBasis, RenderPipelineGraph, RenderPipelineStatus);
					RenderPipelineCanvas.Fit(RenderPipelineGraph);
					RenderPipelineDirty = true;
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
			const bool previewSourceReady =
				renderedSlot != RenderPipelineRenderedSlots.end() &&
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
						RenderPipelinePreviewTextures[nodegraph::PictureKey(
							RenderPipelinePreviewEvaluator.RanAt(canvasNode.Id), canvasType->PreviewPort
						)] = texture;
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

		for (const engine::graph::NodeKindSpec &spec : engine::graph::NodeCatalogue::All()) {
			const std::string title = spec.Label.empty() ? std::string(spec.Kind.Text()) : spec.Label;
			if (!wanted.empty() &&
				!ContainsInsensitive(
					title + " " + std::string(spec.Kind.Text()) + " " + spec.Summary, wanted
				)) {
				continue;
			}
			ImGui::PushID(static_cast<int>(spec.Kind.Id()));
			if (ImGui::Selectable(title.c_str())) {
				const nodegraph::NodeId made =
					RenderPipelineGraph.Add("render.pass." + std::string(spec.Kind.Text()), 0.0f, 0.0f);
				if (made != nodegraph::NO_NODE) {
					RenderPipelineCanvas.Select(made);
					RenderPipelineCanvas.Centre(RenderPipelineGraph, made);
					RenderPipelineDirty = true;
				}
			}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("%s", spec.Summary.c_str());
			}
			ImGui::PopID();
		}
	}

	void Editor::DrawRenderPipelineInspector() {
		const std::vector<nodegraph::NodeId> &selection = RenderPipelineCanvas.Selection();
		if (selection.size() != 1) {
			ImGui::TextDisabled(selection.empty() ? "select a pass" : "multiple passes selected");
			return;
		}
		const nodegraph::Node *node = RenderPipelineGraph.Find(selection.front());
		if (node == nullptr) {
			return;
		}
		const nodegraph::NodeType *type = nodegraph::NodeTypes::Find(node->Type);
		ImGui::TextUnformatted(node->Label.empty() ? node->Type.c_str() : node->Label.c_str());
		if (type != nullptr) {
			ImGui::TextDisabled("%s", type->Subtitle.c_str());
			ImGui::SeparatorText("Inputs");
			for (const nodegraph::PortSpec &port : type->Inputs) {
				const nodegraph::Link *link = RenderPipelineGraph.LinkInto(node->Id, port.Name);
				ImGui::BulletText("%s  %s", port.Name.c_str(), link == nullptr ? "unwired" : "connected");
			}
			ImGui::SeparatorText("Outputs");
			for (const nodegraph::PortSpec &port : type->Outputs) {
				size_t consumers = 0;
				for (const nodegraph::Link &link : RenderPipelineGraph.Links()) {
					consumers += link.From == node->Id && link.FromPort == port.Name ? 1 : 0;
				}
				ImGui::BulletText(
					"%s  %zu consumer%s", port.Name.c_str(), consumers, consumers == 1 ? "" : "s"
				);
			}
		}

		engine::graph::PipelineDocument document;
		std::string error;
		engine::graph::RenderGraph graph;
		engine::core::Name offender;
		engine::graph::CompiledGraph compiled;
		const auto canvasNode = std::find_if(
			RenderPipelineGraph.Nodes().begin(),
			RenderPipelineGraph.Nodes().end(),
			[node](const nodegraph::Node &candidate) { return candidate.Id == node->Id; }
		);
		if (canvasNode != RenderPipelineGraph.Nodes().end() &&
			SaveRenderPipelineGraph(RenderPipelineGraph, RenderPipelineBasis, document, error) &&
			engine::graph::Build(document, graph, offender) == engine::graph::PipelineDocumentStatus::Ok &&
			graph.Compile(compiled, offender) == engine::graph::GraphStatus::Ok) {
			const auto offset = static_cast<uint32_t>(canvasNode - RenderPipelineGraph.Nodes().begin());
			const engine::graph::Node *renderNode = graph.Find(engine::graph::NodeId{offset + 1});
			if (renderNode != nullptr) {
				const auto &gpuTimings = Renderer.PassTimings();
				const auto &wallTimings = Renderer.PassWallTimes();
				const auto gpu = gpuTimings.find(renderNode->Name.Id());
				const auto wall = wallTimings.find(renderNode->Name.Id());
				ImGui::SeparatorText("Profile");
				if (gpu != gpuTimings.end() && gpu->second > 0.0) {
					ImGui::Text(
						"GPU %.3f ms, wall %.3f ms",
						gpu->second / 1000.0,
						wall == wallTimings.end() ? 0.0 : wall->second / 1000.0
					);
				} else {
					ImGui::TextDisabled(
						Renderer.Timed() ? "GPU pending, wall %.3f ms" : "GPU unavailable, wall %.3f ms",
						wall == wallTimings.end() ? 0.0 : wall->second / 1000.0
					);
				}

				const uint32_t width = WorldTarget.IsValid() ? WorldTarget.Width : 1920;
				const uint32_t height = WorldTarget.IsValid() ? WorldTarget.Height : 1080;
				const engine::graph::PipelineProfile profile =
					engine::graph::ProfilePipeline(graph, compiled, width, height);
				const bool drewImage = DrawStageImages(profile, *renderNode, [this](const auto &resource) {
					DrawProfileImage(resource.Name, resource.Width, resource.Height, 240.0f);
				});
				if (!drewImage) {
					ImGui::TextDisabled("This stage does not produce an image.");
				}
			}
		}
		ImGui::Spacing();
		ImGui::TextWrapped(
			"Queue, async, culling, and compute dispatch controls live on the node body. Wires determine "
			"data order; box position does not."
		);
	}

	void Editor::DrawRenderPipelineSchedule() {
		engine::graph::PipelineDocument document;
		std::string error;
		if (!SaveRenderPipelineGraph(RenderPipelineGraph, RenderPipelineBasis, document, error)) {
			ImGui::TextWrapped("%s", error.c_str());
			return;
		}

		engine::graph::RenderGraph graph;
		engine::core::Name offender;
		if (engine::graph::Build(document, graph, offender) != engine::graph::PipelineDocumentStatus::Ok) {
			ImGui::TextWrapped("cannot build %s", std::string(offender.Text()).c_str());
			return;
		}
		engine::graph::ExecutionSchedule schedule;
		if (const auto status = engine::graph::CompileSchedule(graph, schedule, offender);
			status != engine::graph::ScheduleStatus::Ok) {
			ImGui::TextWrapped(
				"%s: %s", engine::graph::Describe(status), std::string(offender.Text()).c_str()
			);
			return;
		}

		const uint32_t width = WorldTarget.IsValid() ? WorldTarget.Width : 1920;
		const uint32_t height = WorldTarget.IsValid() ? WorldTarget.Height : 1080;
		const std::array<uint64_t, 1> worlds = {RenderPipelineWorld.Index};
		engine::graph::FrameExecutionPlan plan;
		if (const auto status =
				engine::graph::PlanFrame(graph, schedule, worlds, width, height, plan, offender);
			status != engine::graph::ExecutionPlanStatus::Ok) {
			ImGui::TextWrapped(
				"%s: %s", engine::graph::Describe(status), std::string(offender.Text()).c_str()
			);
			return;
		}

		uint32_t waveNumber = 0;
		for (const engine::graph::PlannedWave &wave : plan.Waves) {
			ImGui::PushID(static_cast<int>(waveNumber));
			const std::string title = "Wave " + std::to_string(waveNumber++) +
									  (wave.ConcurrentQueues ? "  async overlap" : "  ordered");
			if (ImGui::CollapsingHeader(title.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
				for (const engine::graph::PlannedInvocation &invocation : wave.Invocations) {
					const engine::graph::ScheduledNode &scheduled = invocation.Scheduled;
					const engine::graph::Node *node = graph.Find(scheduled.Node);
					if (node == nullptr) {
						continue;
					}

					ImGui::TextUnformatted(std::string(node->Name.Text()).c_str());
					ImGui::SameLine();
					ImGui::TextDisabled(
						"%s  %s  %s  %.2f MiB read  %.2f MiB write  timing unmeasured",
						engine::graph::Describe(scheduled.Queue),
						engine::graph::Describe(scheduled.Culling),
						engine::graph::Describe(invocation.Scope),
						static_cast<double>(invocation.ReadBytes) / (1024.0 * 1024.0),
						static_cast<double>(invocation.WriteBytes) / (1024.0 * 1024.0)
					);
					if (scheduled.Queue == engine::graph::ExecutionQueue::Compute) {
						ImGui::TextDisabled(
							"dispatch %u x %u x %u  async %s",
							scheduled.GroupsX,
							scheduled.GroupsY,
							scheduled.GroupsZ,
							engine::graph::Describe(scheduled.Async)
						);
					}
				}
			}
			ImGui::PopID();
		}

		ImGui::Separator();
		ImGui::TextDisabled(
			"%u x %u: %.2f MiB read, %.2f MiB write, %.2f MiB across %zu queue handoff%s.",
			width,
			height,
			static_cast<double>(plan.ReadBytes) / (1024.0 * 1024.0),
			static_cast<double>(plan.WriteBytes) / (1024.0 * 1024.0),
			static_cast<double>(plan.QueueTransferBytes) / (1024.0 * 1024.0),
			plan.Transfers.size(),
			plan.Transfers.size() == 1 ? "" : "s"
		);
		ImGui::TextDisabled(
			"GPU and wall-clock timings remain unmeasured until the backend submits this plan."
		);
	}

	void Editor::DrawWorldLighting() {
		if (!ShowWorldLighting) {
			return;
		}
		if (!ImGui::Begin("World Lighting", &ShowWorldLighting)) {
			ImGui::End();
			return;
		}
		if (Universe == nullptr || !Active.IsValid()) {
			ImGui::TextDisabled("no active world");
			ImGui::End();
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
			ShowRenderPipeline = true;
			LoadRenderPipeline(Active, settings.RenderingProfile);
		}
		ImGui::SameLine();
		if (ImGui::Button("Lighting Service Properties")) {
			engine::ecs::Entity lighting = engine::ecs::NULL_ENTITY;
			Universe->Enter(Active, [&](engine::ecs::Store &store) {
				lighting = store.FindFirstRoot("Lighting");
			});
			if (lighting != engine::ecs::NULL_ENTITY) {
				Selection = {lighting};
				SelectionWorld = Active;
				ClearRootSelection();
				ShowProperties = true;
				RevealSelection = true;
			}
		}

		ImGui::TextDisabled("Lighting values remain properties of this world's Lighting service.");
		ImGui::End();
	}

	void Editor::DrawPipelineProfile() {
		if (!ShowPipelineProfile) {
			if (Renderer.Inspecting().IsValid()) {
				Renderer.Inspect({});
			}
			return;
		}
		if (!ImGui::Begin("Pipeline Profile", &ShowPipelineProfile)) {
			ImGui::End();
			return;
		}

		engine::graph::PipelineDocument document;
		std::string error;
		engine::graph::RenderGraph graph;
		engine::core::Name offender;
		engine::graph::CompiledGraph compiled;
		if (!SaveRenderPipelineGraph(RenderPipelineGraph, RenderPipelineBasis, document, error) ||
			engine::graph::Build(document, graph, offender) != engine::graph::PipelineDocumentStatus::Ok ||
			graph.Compile(compiled, offender) != engine::graph::GraphStatus::Ok) {
			ImGui::TextWrapped(
				"The edited pipeline does not compile: %s",
				!error.empty() ? error.c_str() : std::string(offender.Text()).c_str()
			);
			DrawProfileWatch();
			ImGui::End();
			return;
		}

		const uint32_t width = WorldTarget.IsValid() ? WorldTarget.Width : 1920;
		const uint32_t height = WorldTarget.IsValid() ? WorldTarget.Height : 1080;
		engine::graph::PipelineProfile profile =
			engine::graph::ProfilePipeline(graph, compiled, width, height);
		const auto &gpuTimings = Renderer.PassTimings();
		const auto &wallTimings = Renderer.PassWallTimes();
		for (engine::graph::ProfilePass &pass : profile.Passes) {
			if (const auto found = gpuTimings.find(pass.Name.Id()); found != gpuTimings.end()) {
				pass.Elapsed = found->second;
			}
			if (const auto found = wallTimings.find(pass.Name.Id()); found != wallTimings.end()) {
				pass.Wall = found->second;
			}
		}
		const auto mib = [](uint64_t bytes) { return static_cast<double>(bytes) / (1024.0 * 1024.0); };

		ImGui::Text("%zu passes, %zu resources", profile.Passes.size(), profile.Resources.size());
		ImGui::SameLine();
		ImGui::TextDisabled(
			"%.2f MiB peak / %.2f MiB declared / %.2f MiB aliasable",
			mib(profile.PeakBytes),
			mib(profile.TotalBytes),
			mib(profile.TotalBytes - profile.PeakBytes)
		);
		ImGui::SeparatorText("Stage images and timings");
		for (const engine::graph::ProfilePass &pass : profile.Passes) {
			ImGui::PushID(static_cast<int>(pass.Node.Value));
			const std::string name(pass.Name.Text());
			const bool open = ImGui::TreeNodeEx(name.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth);
			ImGui::SameLine();
			if (pass.Elapsed > 0.0) {
				ImGui::TextDisabled("GPU %.3f ms  wall %.3f ms", pass.Elapsed / 1000.0, pass.Wall / 1000.0);
			} else {
				ImGui::TextDisabled(
					Renderer.Timed() ? "GPU pending  wall %.3f ms" : "GPU unavailable  wall %.3f ms",
					pass.Wall / 1000.0
				);
			}
			if (open) {
				const engine::graph::Node *node = graph.Find(pass.Node);
				const bool drewImage =
					node != nullptr && DrawStageImages(profile, *node, [this](const auto &resource) {
						DrawProfileImage(resource.Name, resource.Width, resource.Height, 320.0f);
					});
				if (!drewImage) {
					ImGui::TextDisabled("This stage does not produce an image.");
				}
				ImGui::TreePop();
			}
			ImGui::PopID();
		}

		if (ImGui::BeginTable(
				"resource-access",
				static_cast<int>(profile.Passes.size()) + 1,
				ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollX |
					ImGuiTableFlags_SizingFixedFit
			)) {
			ImGui::TableSetupScrollFreeze(1, 1);
			ImGui::TableSetupColumn("Resource", ImGuiTableColumnFlags_WidthFixed, 190.0f);
			for (const engine::graph::ProfilePass &pass : profile.Passes) {
				ImGui::TableSetupColumn(
					std::string(pass.Name.Text()).c_str(), ImGuiTableColumnFlags_WidthFixed, 76.0f
				);
			}
			ImGui::TableHeadersRow();

			for (size_t row = 0; row < profile.Resources.size(); row++) {
				const engine::graph::ProfileResource &resource = profile.Resources[row];
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				const std::string label(resource.Name.Text());
				const bool watched = ProfileWatched == resource.Name;
				if (ImGui::Selectable(label.c_str(), watched, ImGuiSelectableFlags_SpanAllColumns)) {
					ProfileWatched = watched ? engine::core::Name{} : resource.Name;
					Renderer.Inspect(ProfileWatched, 0);
				}
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip(
						"%s, %s, %ux%u, %.2f MiB%s",
						engine::graph::Describe(resource.Kind),
						engine::graph::Describe(resource.Format),
						resource.Width,
						resource.Height,
						mib(resource.Bytes),
						resource.External ? ", external" : ", transient"
					);
				}

				for (size_t column = 0; column < profile.Passes.size(); column++) {
					ImGui::TableSetColumnIndex(static_cast<int>(column) + 1);
					const engine::graph::Access access = profile.At(row, column);
					if (access != engine::graph::Access::None) {
						ImGui::TextUnformatted(engine::graph::Describe(access));
					} else if (resource.LiveAt(static_cast<uint32_t>(column))) {
						ImGui::TextDisabled("alive");
					}
				}
			}
			ImGui::EndTable();
		}

		DrawProfileWatch();
		ImGui::End();
	}

	void Editor::DrawProfileImage(
		engine::core::Name resource, uint32_t width, uint32_t height, float maximumWidth
	) {
		const std::string name(resource.Text());
		void *texture = Renderer.ResourceTexture(resource, 0);
		ImGui::TextUnformatted(name.c_str());
		if (texture == nullptr || width == 0 || height == 0) {
			ImGui::TextDisabled("No image is allocated for the current viewport.");
			return;
		}

		const float available = std::max(ImGui::GetContentRegionAvail().x, 64.0f);
		const float shown = std::min(available, maximumWidth);
		const float aspect = static_cast<float>(width) / static_cast<float>(height);
		const engine::render::SceneExtent extent = Renderer.ResourceTextureExtent(resource, 0);
		ImGui::Image(
			reinterpret_cast<ImTextureID>(texture),
			ImVec2{shown, shown / std::max(aspect, 0.01f)},
			ImVec2{0.0f, 0.0f},
			ImVec2{extent.U, extent.V}
		);
		if (ImGui::IsItemClicked()) {
			ProfileWatched = resource;
			ShowPipelineProfile = true;
			Renderer.Inspect(resource, 0);
		}
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("Click to inspect %s with its histogram.", name.c_str());
		}
	}

	void Editor::DrawProfileWatch() {
		if (!ProfileWatched.IsValid()) {
			ImGui::TextDisabled("Select a resource to inspect its output.");
			return;
		}

		ImGui::Separator();
		const std::string name(ProfileWatched.Text());
		void *texture = Renderer.ResourceTexture(ProfileWatched, 0);
		if (texture == nullptr || !WorldTarget.IsValid()) {
			ImGui::TextDisabled("%s has no allocated image in the current viewport.", name.c_str());
			return;
		}
		Renderer.Inspect(ProfileWatched, 0);

		const engine::render::Renderer::ReadbackImage readback = Renderer.Readback();
		const bool matching = readback.IsValid() && readback.Source == ProfileWatched && readback.Slot == 0;
		ImGui::Text("%s, %ux%u", name.c_str(), WorldTarget.Width, WorldTarget.Height);
		ImGui::SameLine();
		const bool histogramSupported = ProfileWatched == engine::core::Name("colour") ||
										ProfileWatched == engine::core::Name("display") ||
										ProfileWatched == engine::core::Name("window") ||
										ProfileWatched == engine::core::Name("albedo") ||
										ProfileWatched == engine::core::Name("material");
		if (matching) {
			ImGui::TextDisabled("histogram %llu frame(s) old", static_cast<unsigned long long>(readback.Age));
		} else if (!histogramSupported) {
			ImGui::TextDisabled("live GPU image, histogram unavailable for this format");
		} else {
			ImGui::TextDisabled("histogram waiting");
		}

		const float available = std::max(ImGui::GetContentRegionAvail().x, 64.0f);
		const float shown = std::min(available, 420.0f);
		const float aspect = static_cast<float>(WorldTarget.Width) / static_cast<float>(WorldTarget.Height);
		const engine::render::SceneExtent extent = Renderer.ResourceTextureExtent(ProfileWatched, 0);
		ImGui::Image(
			reinterpret_cast<ImTextureID>(texture),
			ImVec2{shown, shown / std::max(aspect, 0.01f)},
			ImVec2{0.0f, 0.0f},
			ImVec2{extent.U, extent.V}
		);

		if (!matching) {
			return;
		}

		const auto histogram = [](const char *label,
								  const engine::render::ChannelHistogram &channel,
								  ImU32 colour) {
			uint32_t peak = 1;
			for (const uint32_t count : channel.Buckets) {
				peak = std::max(peak, count);
			}
			ImGui::TextDisabled("%s", label);
			ImGui::SameLine();
			const ImVec2 origin = ImGui::GetCursorScreenPos();
			const float width = std::min(std::max(ImGui::GetContentRegionAvail().x - 72.0f, 64.0f), 280.0f);
			const float height = 28.0f;
			const float step = width / static_cast<float>(engine::render::HISTOGRAM_BUCKETS);
			for (size_t bucket = 0; bucket < engine::render::HISTOGRAM_BUCKETS; bucket++) {
				const float tall =
					height * static_cast<float>(channel.Buckets[bucket]) / static_cast<float>(peak);
				ImGui::GetWindowDrawList()->AddRectFilled(
					ImVec2{origin.x + step * bucket, origin.y + height - tall},
					ImVec2{origin.x + step * (bucket + 1) - 1.0f, origin.y + height},
					colour
				);
			}
			ImGui::Dummy(ImVec2{width, height});
			ImGui::SameLine();
			ImGui::TextDisabled("%u..%u", channel.Minimum, channel.Maximum);
		};

		histogram("R", readback.Histogram.Red, IM_COL32(230, 90, 90, 255));
		histogram("G", readback.Histogram.Green, IM_COL32(100, 215, 100, 255));
		histogram("B", readback.Histogram.Blue, IM_COL32(100, 140, 240, 255));
		histogram("A", readback.Histogram.Alpha, IM_COL32(205, 205, 205, 255));

		if (readback.Histogram.Uniform()) {
			ImGui::TextColored(
				ImVec4{0.95f, 0.75f, 0.35f, 1.0f}, "Every pixel is identical. The pass may not have run."
			);
		} else if (readback.Histogram.Alpha.Blank()) {
			ImGui::TextColored(
				ImVec4{0.95f, 0.75f, 0.35f, 1.0f}, "Alpha is blank. This format may be wasting a channel."
			);
		}
	}
}
