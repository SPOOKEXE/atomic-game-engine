#include "PhysicsProfiler.hpp"
#include "ProfilerFlame.hpp"

#include <engine/core/FrameGraph.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/physics/PhysicsWorld.hpp>

#include <algorithm>
#include <imgui.h>
#include <studio/Editor.hpp>

namespace studio {
	namespace {
		void CapturePhysics(std::vector<DiagnosticSpan> &into) {
			const auto &spans = engine::core::FrameGraph::Spans();
			std::vector<bool> include;
			PhysicsProfilerSpanMask(spans, include);
			into.clear();
			into.reserve(spans.size());
			std::vector<uint32_t> kept(spans.size(), engine::core::FrameGraph::NO_PARENT);
			for (size_t index = 0; index < spans.size(); ++index) {
				const engine::core::FrameSpan &source = spans[index];
				if (!include[index]) continue;
				uint32_t parent = source.Parent;
				while (parent < index && kept[parent] == engine::core::FrameGraph::NO_PARENT)
					parent = spans[parent].Parent;
				const uint32_t retainedParent =
					parent < index ? kept[parent] : engine::core::FrameGraph::NO_PARENT;
				into.push_back(
					DiagnosticSpan{
						.Name = std::string(source.Name),
						.Depth = retainedParent == engine::core::FrameGraph::NO_PARENT
									 ? 0
									 : into[retainedParent].Depth + 1,
						.Parent = retainedParent,
						.StartMilliseconds = source.StartMilliseconds,
						.Milliseconds = source.Milliseconds,
						.SelfMilliseconds = source.SelfMilliseconds,
						.IdleMilliseconds = source.IdleMilliseconds,
						.Category = source.Category,
						.Owner = source.Owner,
						.Reported = source.Reported
					}
				);
				kept[index] = static_cast<uint32_t>(into.size() - 1);
			}
		}

		void DrawPhysicsFlame(const std::vector<DiagnosticSpan> &spans, float capturedFrame) {
			if (spans.empty()) {
				ImGui::TextDisabled("no physics spans recorded yet");
				return;
			}
			const float frame = std::max(capturedFrame, 0.001f);
			std::vector<DiagnosticSpan> measured;
			MeasuredProfilerSpans(spans, measured);
			std::vector<uint32_t> rows;
			const uint32_t rowCount = LayoutDiagnosticRows(measured, rows);
			const ImVec2 origin = ImGui::GetCursorScreenPos();
			const float width = ImGui::GetContentRegionAvail().x;
			constexpr float HEIGHT = 20.0f;
			ImDrawList *draw = ImGui::GetWindowDrawList();
			for (size_t index = 0; index < measured.size(); ++index) {
				const DiagnosticSpan &span = measured[index];
				const float left = origin.x + std::clamp(span.StartMilliseconds / frame, 0.0f, 1.0f) * width;
				const float right =
					origin.x +
					std::clamp((span.StartMilliseconds + span.Milliseconds) / frame, 0.0f, 1.0f) * width;
				const float top = origin.y + static_cast<float>(rows[index]) * HEIGHT;
				const ImVec2 min(left, top);
				const ImVec2 max(std::max(left + 1.0f, right), top + HEIGHT - 2.0f);
				draw->AddRectFilled(min, max, IM_COL32(180, 94, 220, 220));
				draw->PushClipRect(min, max, true);
				draw->AddText(ImVec2(left + 3.0f, top + 2.0f), IM_COL32_WHITE, span.Name.c_str());
				draw->PopClipRect();
				if (ImGui::IsMouseHoveringRect(min, max))
					ImGui::SetTooltip("%s\n%.3f ms measured", span.Name.c_str(), span.Milliseconds);
			}
			ImGui::Dummy(ImVec2(width, static_cast<float>(rowCount) * HEIGHT));
			for (const DiagnosticSpan &span : spans)
				if (span.Reported)
					ImGui::TextDisabled(
						"reported worker: %s, %.3f ms measured", span.Name.c_str(), span.Milliseconds
					);
		}
	}

	void Editor::DrawPhysicsSolver() {
		if (!ShowPhysicsSolver) return;
		ImGui::SetNextWindowSize(ImVec2(900.0f, 500.0f), ImGuiCond_FirstUseEver);
		if (!ImGui::Begin("Physics Profiler", &ShowPhysicsSolver)) {
			ImGui::End();
			return;
		}
		if (!PhysicsProfiler.Paused) {
			CapturePhysics(PhysicsProfiler.Spans);
			PhysicsProfiler.FrameMilliseconds = engine::core::FrameGraph::FrameMilliseconds();
			PhysicsProfiler.UnmarkedMilliseconds = engine::core::FrameGraph::UnmarkedMilliseconds();
			PhysicsProfiler.Dropped = engine::core::FrameGraph::Dropped();
		}
		if (ImGui::Button(PhysicsProfiler.Paused ? "Resume" : "Pause"))
			PhysicsProfiler.Paused = !PhysicsProfiler.Paused;
		ImGui::SameLine();
		if (ImGui::Button("Snapshot")) {
			CapturePhysics(PhysicsProfiler.Spans);
			PhysicsProfiler.FrameMilliseconds = engine::core::FrameGraph::FrameMilliseconds();
			PhysicsProfiler.UnmarkedMilliseconds = engine::core::FrameGraph::UnmarkedMilliseconds();
			PhysicsProfiler.Dropped = engine::core::FrameGraph::Dropped();
		}
		ImGui::SameLine();
		ImGui::TextWrapped(
			"all-world frame stages%s; active-world topology remains live. frame %.3f ms, unmarked %.3f "
			"ms, %zu dropped",
			PhysicsProfiler.Paused ? " (paused)" : "",
			PhysicsProfiler.FrameMilliseconds,
			PhysicsProfiler.UnmarkedMilliseconds,
			PhysicsProfiler.Dropped
		);

		if (Universe == nullptr || !Active.IsValid())
			ImGui::TextDisabled("no active world");
		else
			Universe->Enter(Active, [&](engine::ecs::Store &store) {
				const auto *physics = store.Resource<engine::physics::PhysicsWorld>();
				if (physics == nullptr) {
					ImGui::TextDisabled("physics is not prepared for this world");
					return;
				}
				const size_t groups = physics->SolverGroupCount();
				const char *route = physics->UsesIslandSchedule()	? "independent islands"
									: physics->UsesColourSchedule() ? "contact colour waves"
									: groups > 0					? "spatial chunks"
																	: "serial";
				ImGui::Text("%s", route);
				ImGui::SameLine();
				ImGui::TextDisabled(
					"%zu rows, %zu groups, %zu islands, %zu colour waves",
					physics->RowCount(),
					groups,
					physics->ConstraintIslandCount(),
					physics->SolverColourCount()
				);
				ImGui::TextDisabled(
					"chunk %.2f m, %zu border rows, %llu continuous sweeps (cumulative)",
					static_cast<double>(physics->SolverChunkSize()),
					physics->BorderRowCount(),
					static_cast<unsigned long long>(physics->SweptBodies())
				);
			});

		if (ImGui::BeginTabBar("physics profile")) {
			if (ImGui::BeginTabItem("Stages")) {
				if (ImGui::BeginTable(
						"physics stages", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp
					)) {
					ImGui::TableSetupColumn("stage", ImGuiTableColumnFlags_WidthStretch);
					ImGui::TableSetupColumn("inclusive ms");
					ImGui::TableSetupColumn("self ms");
					ImGui::TableSetupColumn("idle ms");
					ImGui::TableHeadersRow();
					for (const DiagnosticSpan &span : PhysicsProfiler.Spans) {
						ImGui::TableNextRow();
						ImGui::TableNextColumn();
						ImGui::Indent(12.0f * static_cast<float>(span.Depth));
						ImGui::TextUnformatted(span.Name.c_str());
						ImGui::Unindent(12.0f * static_cast<float>(span.Depth));
						if (span.Reported) {
							ImGui::SameLine();
							ImGui::TextDisabled("reported worker work");
						}
						ImGui::TableNextColumn();
						ImGui::Text("%.3f", span.Milliseconds);
						ImGui::TableNextColumn();
						ImGui::Text("%.3f", span.SelfMilliseconds);
						ImGui::TableNextColumn();
						ImGui::Text("%.3f", span.IdleMilliseconds);
					}
					ImGui::EndTable();
				}
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("Flame graph")) {
				DrawPhysicsFlame(PhysicsProfiler.Spans, PhysicsProfiler.FrameMilliseconds);
				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		}
		ImGui::End();
	}
}
