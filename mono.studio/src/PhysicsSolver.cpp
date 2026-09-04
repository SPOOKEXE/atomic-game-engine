#include <engine/ecs/Store.hpp>
#include <engine/physics/PhysicsWorld.hpp>

#include <imgui.h>
#include <studio/Editor.hpp>

namespace studio {
	void Editor::DrawPhysicsSolver() {
		if (!ShowPhysicsSolver) {
			return;
		}
		if (!ImGui::Begin("Physics Solver", &ShowPhysicsSolver)) {
			ImGui::End();
			return;
		}
		if (Universe == nullptr || !Active.IsValid()) {
			ImGui::TextDisabled("no active world");
			ImGui::End();
			return;
		}

		Universe->Enter(Active, [&](engine::ecs::Store &store) {
			const engine::physics::PhysicsWorld *physics = store.Resource<engine::physics::PhysicsWorld>();
			if (physics == nullptr) {
				ImGui::TextDisabled("physics is not prepared for this world");
				return;
			}

			const size_t rows = physics->RowCount();
			const size_t groups = physics->SolverGroupCount();
			const char *route = "serial";
			if (physics->UsesIslandSchedule()) {
				route = "independent islands";
			} else if (physics->UsesColourSchedule()) {
				route = "contact colour waves";
			} else if (groups > 0) {
				route = "spatial chunks";
			}

			ImGui::Text("last solve: %s", route);
			ImGui::SeparatorText("solver capabilities");
			ImGui::BulletText("serial order%s", rows == 0 || groups == 0 ? "  active" : "");
			ImGui::BulletText("spatial chunk groups%s", groups > 0 ? "  active" : "");
			ImGui::BulletText("contact colour waves%s", physics->UsesColourSchedule() ? "  active" : "");
			ImGui::BulletText("independent islands%s", physics->UsesIslandSchedule() ? "  active" : "");

			ImGui::SeparatorText("last solve topology");
			ImGui::Text("contact rows  %zu", rows);
			ImGui::Text(
				"groups  %zu    islands  %zu    colour waves  %zu",
				groups,
				physics->ConstraintIslandCount(),
				physics->SolverColourCount()
			);
			ImGui::Text("chunk edge  %.2f m", static_cast<double>(physics->SolverChunkSize()));

			const size_t border = physics->BorderRowCount();
			const float borderFraction =
				rows == 0 ? 0.0f : static_cast<float>(border) / static_cast<float>(rows);
			ImGui::ProgressBar(borderFraction, ImVec2(-1.0f, 0.0f), "border rows");
			ImGui::TextDisabled("%zu border row(s), solved after parallel groups", border);

			ImGui::SeparatorText("collision work");
			ImGui::Text("continuous sweeps  %llu", static_cast<unsigned long long>(physics->SweptBodies()));
			ImGui::TextDisabled(
				"continuous sweeps are cumulative. all other values are from the last physics tick."
			);
		});
		ImGui::End();
	}
}
