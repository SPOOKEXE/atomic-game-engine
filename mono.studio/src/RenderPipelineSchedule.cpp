#include <engine/graph/ExecutionPlan.hpp>
#include <engine/graph/Schedule.hpp>

#include <array>
#include <imgui.h>
#include <studio/Editor.hpp>
#include <studio/RenderPipelineGraph.hpp>

namespace studio {

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
			if (ImGui::CollapsingHeader(title.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
				for (const engine::graph::PlannedInvocation &invocation : wave.Invocations) {
					const engine::graph::ScheduledNode &scheduled = invocation.Scheduled;
					const engine::graph::Node *node = graph.Find(scheduled.Node);
					if (node == nullptr) continue;
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
					if (scheduled.Queue == engine::graph::ExecutionQueue::Compute)
						ImGui::TextDisabled(
							"dispatch %u x %u x %u  async %s",
							scheduled.GroupsX,
							scheduled.GroupsY,
							scheduled.GroupsZ,
							engine::graph::Describe(scheduled.Async)
						);
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
}
