// Which script is spending the tick.
//
// **The number was already being counted.** `RuntimeLimits::StepBudget` bounds
// an interrupt counter, so Luau's `Interrupt` hook runs at every loop back-edge,
// call and return whether or not anybody is looking — this panel reads the
// figure that hook maintains and adds nothing to the hot path to do it.
//
// **Steps and not milliseconds**, which is `script/AGENTS.md`'s rule rather than
// a limitation: a wall-clock figure makes what a script cost depend on how busy
// the machine was, so two runs of one recording would disagree about it. A step
// is the same on every machine, which is what makes two numbers here comparable
// at all.
//
// ## What this measures, said plainly
//
// A script's **top level** — the run that `RunWorldScripts` performs when the
// world starts. Not its heartbeat work. `SignalTable` holds connections as
// opaque callables and nothing records which script made one, so attributing
// beat time to a script would be a guess with a number's confidence. The panel
// says so rather than letting a reader assume otherwise.

#include <engine/script/Runtime.hpp>
#include <engine/ui/Theme.hpp>

#include <algorithm>
#include <cstdio>
#include <imgui.h>
#include <studio/Editor.hpp>
#include <studio/Widgets.hpp>
#include <vector>

namespace studio {

	using engine::ecs::Store;
	using engine::script::ScriptCost;
	using engine::world::WorldId;

	void Editor::DrawScriptProfile() {
		if (!ShowScriptProfile) {
			return;
		}

		if (!ImGui::Begin("Script Profile", &ShowScriptProfile)) {
			ImGui::End();
			return;
		}

		if (Runs.empty()) {
			// **Not an empty table.** "No scripts are running" and "the scripts
			// that are running cost nothing" are different answers and a blank
			// grid gives the second when it means the first.
			ImGui::TextDisabled("nothing is running — press Play or Run");
			ImGui::End();
			return;
		}

		ImGui::TextDisabled("VM steps in each script's top level, not its heartbeat work");
		ImGui::Separator();

		for (const WorldRun &run : Runs) {
			if (run.Runtime == nullptr) {
				continue;
			}

			const std::string world(Label(Universe->NameOf(run.World)));

			// Copied out of the runtime before anything is drawn, and sorted
			// here rather than in the runtime: the order a panel wants is a
			// presentation decision and `Costs()` promises run order, which is
			// what a recording depends on.
			std::vector<ScriptCost> costs(run.Runtime->Costs().begin(), run.Runtime->Costs().end());
			std::sort(costs.begin(), costs.end(), [](const ScriptCost &left, const ScriptCost &right) {
				return left.Steps > right.Steps;
			});

			uint64_t total = 0;
			for (const ScriptCost &cost : costs) {
				total += cost.Steps;
			}

			char header[192];
			std::snprintf(
				header,
				sizeof(header),
				"%s — %zu script%s, %llu steps###%u",
				world.c_str(),
				costs.size(),
				costs.size() == 1 ? "" : "s",
				static_cast<unsigned long long>(total),
				run.World.Index
			);

			if (!ImGui::CollapsingHeader(header, ImGuiTreeNodeFlags_DefaultOpen)) {
				continue;
			}

			if (costs.empty()) {
				ImGui::TextDisabled("no scripts ran in this world");
				continue;
			}

			// Names are read out of the store, which means entering the world.
			// Gathered first and drawn after, like every other panel here.
			std::vector<std::string> names(costs.size());
			Universe->Enter(run.World, [&](Store &store) {
				for (size_t index = 0; index < costs.size(); index++) {
					names[index] = store.Alive(costs[index].Instance)
									   ? std::string(Label(store.InstanceNameOf(costs[index].Instance)))
									   : std::string("(deleted)");
				}
			});

			ImGui::PushID(static_cast<int>(run.World.Index));
			if (ImGui::BeginTable("##costs", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
				ImGui::TableSetupColumn("script", ImGuiTableColumnFlags_WidthStretch, 0.5f);
				ImGui::TableSetupColumn("steps", ImGuiTableColumnFlags_WidthStretch, 0.25f);
				ImGui::TableSetupColumn("share", ImGuiTableColumnFlags_WidthStretch, 0.25f);
				ImGui::TableHeadersRow();

				for (size_t index = 0; index < costs.size(); index++) {
					const ScriptCost &cost = costs[index];

					ImGui::TableNextRow();
					ImGui::TableNextColumn();

					if (cost.Completed) {
						ImGui::TextUnformatted(names[index].c_str());
					} else {
						// **A script that raised has a step count and it is not
						// comparable to one that finished** — it stopped where
						// it stopped. Marked rather than silently ranked beside
						// the others.
						ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.35f, 1.0f), "%s", names[index].c_str());
						if (ImGui::IsItemHovered()) {
							ImGui::SetTooltip("raised part way through — its count is where it stopped");
						}
					}

					ImGui::TableNextColumn();
					ImGui::Text("%llu", static_cast<unsigned long long>(cost.Steps));

					ImGui::TableNextColumn();
					if (total == 0) {
						// Every division in this panel is guarded, and this is
						// the one that fires: a world whose scripts are all
						// trivial legitimately totals zero steps.
						ImGui::TextDisabled("—");
					} else {
						const float share =
							static_cast<float>(cost.Steps) / static_cast<float>(total);

						char label[32];
						std::snprintf(label, sizeof(label), "%.0f%%", static_cast<double>(share) * 100.0);
						ImGui::ProgressBar(share, ImVec2(-1.0f, 0.0f), label);
					}
				}
				ImGui::EndTable();
			}
			ImGui::PopID();
		}

		ImGui::End();
	}
}
