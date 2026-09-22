#include <engine/core/FrameGraph.hpp>
#include <engine/graph/ExecutionPlan.hpp>
#include <engine/graph/PipelineProfile.hpp>
#include <engine/graph/Schedule.hpp>

#include <algorithm>
#include <imgui.h>
#include <studio/Editor.hpp>
#include <studio/RenderPipelineGraph.hpp>

namespace studio {
	namespace {
		template <typename Timings>
		double ProfilePassTiming(const Timings &timings, const engine::graph::ProfilePass &pass) {
			if (const auto named = timings.find(pass.Name.Id()); named != timings.end()) return named->second;
			if (const auto kind = timings.find(pass.Kind.Id()); kind != timings.end()) return kind->second;
			return 0.0;
		}
		double ProfileFrameGraphWallTime(const engine::graph::ProfilePass &pass) {
			double microseconds = 0.0;
			for (const engine::core::FrameSpan &span : engine::core::FrameGraph::Spans())
				if (span.Name == pass.Name.Text() || span.Name == pass.Kind.Text())
					microseconds += static_cast<double>(span.Milliseconds) * 1000.0;
			return microseconds;
		}
		template <typename Draw>
		bool DrawStageImages(
			const engine::graph::PipelineProfile &profile, const engine::graph::Node &node, Draw draw
		) {
			const auto &images = node.Writes.empty() ? node.Reads : node.Writes;
			bool drew = false;
			for (const engine::graph::ResourceId id : images)
				for (const auto &resource : profile.Resources)
					if (resource.Id == id && (resource.Kind == engine::graph::ResourceKind::Colour ||
											  resource.Kind == engine::graph::ResourceKind::Depth ||
											  resource.Kind == engine::graph::ResourceKind::Texture ||
											  resource.Kind == engine::graph::ResourceKind::Storage)) {
						drew = true;
						draw(resource);
					}
			return drew;
		}
	}
	void Editor::DrawPipelineProfile() {
		if (!ShowPipelineProfile) {
			if (Renderer.Inspecting().IsValid()) {
				Renderer.Inspect({});
			}
			return;
		}
		if (RenderPipelineWorld != Active || RenderPipelineGraph.Nodes().empty()) {
			LoadRenderPipeline(Active, {});
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
		engine::graph::ExecutionSchedule schedule;
		engine::core::Name scheduleOffender;
		std::vector<engine::graph::PlannedCommandBuffer> commandBuffers;
		if (engine::graph::CompileSchedule(graph, schedule, scheduleOffender) ==
			engine::graph::ScheduleStatus::Ok) {
			commandBuffers = engine::graph::PlanCommandBuffers(schedule);
		}
		const auto &gpuTimings = Renderer.PassTimings();
		const auto &wallTimings = Renderer.PassWallTimes();
		for (engine::graph::ProfilePass &pass : profile.Passes) {
			pass.Elapsed = ProfilePassTiming(gpuTimings, pass);
			pass.Wall = ProfilePassTiming(wallTimings, pass);
			if (pass.Wall == 0.0)
				pass.Wall = ProfileFrameGraphWallTime(pass);
		}
		const auto mib = [](uint64_t bytes) { return static_cast<double>(bytes) / (1024.0 * 1024.0); };
		int profilingTier = static_cast<int>(Renderer.Profiling());
		const char *profilingTiers[] = {"Off", "CPU", "Full"};
		ImGui::SetNextItemWidth(110.0f);
		if (ImGui::Combo("Timing", &profilingTier, profilingTiers, 3)) {
			Renderer.SetProfiling(static_cast<engine::render::ProfilingTier>(profilingTier));
		}
		if (Renderer.DroppedProfileMarks() > 0) {
			ImGui::SameLine();
			ImGui::TextColored(
				ImVec4(1.0f, 0.55f, 0.2f, 1.0f),
				"%zu GPU marks dropped; timings are partial",
				Renderer.DroppedProfileMarks()
			);
		}
		if (profilingTier == static_cast<int>(engine::render::ProfilingTier::Off)) {
			ImGui::TextDisabled(
				"Passes and memory are available now. Select CPU or Full timing while a viewport runs "
				"to collect elapsed times."
			);
		}

		ImGui::Text("%zu passes, %zu resources", profile.Passes.size(), profile.Resources.size());
		ImGui::SameLine();
		ImGui::TextDisabled(
			"%.2f MiB peak / %.2f MiB declared / %.2f MiB transient allocation",
			mib(profile.PeakBytes),
			mib(profile.TotalBytes),
			mib(profile.AllocatedBytes)
		);
		if (!commandBuffers.empty()) {
			size_t gpuNodes = 0;
			for (const engine::graph::PlannedCommandBuffer &buffer : commandBuffers) {
				gpuNodes += buffer.Nodes.size();
			}
			ImGui::TextDisabled(
				"compile: %zu GPU nodes -> %zu command buffers (%zu fused boundaries), %u aliased targets",
				gpuNodes,
				commandBuffers.size(),
				gpuNodes > commandBuffers.size() ? gpuNodes - commandBuffers.size() : 0,
				profile.AliasedResources
			);
		}
		ImGui::SeparatorText("Stage images and timings");
		if (ImGui::BeginTable(
				"##pipeline-timings", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp
			)) {
			ImGui::TableSetupColumn("Pass", ImGuiTableColumnFlags_WidthStretch, 0.55f);
			ImGui::TableSetupColumn("GPU ms", ImGuiTableColumnFlags_WidthFixed, 80.0f);
			ImGui::TableSetupColumn("Wall ms", ImGuiTableColumnFlags_WidthFixed, 80.0f);
			ImGui::TableHeadersRow();
			for (const engine::graph::ProfilePass &pass : profile.Passes) {
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::TextUnformatted(std::string(pass.Name.Text()).c_str());
				ImGui::TableSetColumnIndex(1);
				if (pass.Elapsed > 0.0) {
					ImGui::Text("%.3f", pass.Elapsed / 1000.0);
				} else {
					ImGui::TextDisabled("unmeasured");
				}
				ImGui::TableSetColumnIndex(2);
				if (pass.Wall > 0.0) {
					ImGui::Text("%.3f", pass.Wall / 1000.0);
				} else {
					ImGui::TextDisabled("unmeasured");
				}
			}
			ImGui::EndTable();
		}
		for (const engine::graph::ProfilePass &pass : profile.Passes) {
			ImGui::PushID(static_cast<int>(pass.Node.Value));
			const std::string name(pass.Name.Text());
			const bool open = ImGui::TreeNodeEx(name.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth);
			ImGui::SameLine();
			if (pass.Elapsed > 0.0 || pass.Wall > 0.0) {
				if (pass.Elapsed > 0.0) {
					ImGui::TextDisabled(
						"GPU %.3f ms  wall %.3f ms", pass.Elapsed / 1000.0, pass.Wall / 1000.0
					);
				} else {
					ImGui::TextDisabled("GPU pending  wall %.3f ms", pass.Wall / 1000.0);
				}
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
