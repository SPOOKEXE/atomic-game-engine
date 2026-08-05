// Breakpoints, and what they caught.
//
// **This is a capture debugger and the panel says so.** `script/Debugger.hpp`
// carries the argument: a paused world is not a replayable one, and the editor's
// frame loop is the thread the VM runs on — so holding a script inside a line
// would freeze the window that was going to show you the answer. What this does
// instead is record the stack and every local at the moment the line ran.
//
// Written on the panel rather than only in a header, because somebody who
// expected a step debugger and got this one needs to be told at the point of use
// rather than discovering it by pressing a button that is not there.

#include <engine/script/Debugger.hpp>
#include <engine/ui/Theme.hpp>

#include <cstdio>
#include <imgui.h>
#include <string>
#include <studio/Editor.hpp>
#include <studio/Widgets.hpp>

namespace studio {

	using engine::ecs::Store;
	using engine::script::BreakAction;
	using engine::script::Breakpoint;
	using engine::script::DebugFrame;
	using engine::script::DebugHit;
	using engine::script::DebugLocal;
	using engine::world::WorldId;

	void Editor::DrawDebugger() {
		if (!ShowDebugger) {
			return;
		}

		if (!ImGui::Begin("Debugger", &ShowDebugger)) {
			ImGui::End();
			return;
		}

		// --- adding one --------------------------------------------------------

		ImGui::SetNextItemWidth(180.0f * Settings.Scale);
		TextField("##break-source", BreakSource, "script path — enemy.luau");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(70.0f * Settings.Scale);
		ImGui::InputInt("##break-line", &BreakLine, 0, 0);
		ImGui::SameLine();

		const bool valid = !BreakSource.empty() && BreakLine > 0;
		ImGui::BeginDisabled(!valid);
		if (ImGui::Button("Add")) {
			// Added to every running runtime. A breakpoint is a thing about a
			// *script*, and the same script may be running in several worlds —
			// asking which one somebody meant would be a question with no
			// answer they could give.
			for (WorldRun &run : Runs) {
				if (run.Runtime != nullptr) {
					run.Runtime->Debug().Add(BreakSource, BreakLine, BreakStops ? BreakAction::Stop
																				: BreakAction::Capture);
				}
			}
			if (Runs.empty()) {
				Say("nothing is running — a breakpoint is added to a live runtime", engine::core::LogLevel::Warning);
			}
		}
		ImGui::EndDisabled();

		ImGui::SameLine();
		ImGui::Checkbox("stop", &BreakStops);
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip(
				"Stop ends that script's run with an ordinary error.\n"
				"Capture records the stack and lets it carry on."
			);
		}

		// The thing somebody has to know before they trust this panel.
		ImGui::TextDisabled("captures the stack — it does not pause the world");
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip(
				"A held tick is work crossing a tick boundary, which is what makes\n"
				"a recording stop replaying — and the editor's frame loop is the\n"
				"thread the VM runs on, so a pause would freeze this window too."
			);
		}

		ImGui::Separator();

		if (Runs.empty()) {
			ImGui::TextDisabled("nothing is running — press Play or Run");
			ImGui::End();
			return;
		}

		for (WorldRun &run : Runs) {
			if (run.Runtime == nullptr) {
				continue;
			}

			engine::script::Debugger &debug = run.Runtime->Debug();
			const std::string world(Label(Universe->NameOf(run.World)));

			char header[192];
			std::snprintf(
				header,
				sizeof(header),
				"%s — %zu breakpoint%s, %zu caught###%u",
				world.c_str(),
				debug.Breakpoints().size(),
				debug.Breakpoints().size() == 1 ? "" : "s",
				debug.Hits().size(),
				run.World.Index
			);

			if (!ImGui::CollapsingHeader(header, ImGuiTreeNodeFlags_DefaultOpen)) {
				continue;
			}

			ImGui::PushID(static_cast<int>(run.World.Index));

			// --- the breakpoints ------------------------------------------------
			std::string removeSource;
			int removeLine = 0;

			for (const Breakpoint &point : debug.Breakpoints()) {
				ImGui::PushID(point.Line);
				ImGui::PushID(point.Source.c_str());

				bool enabled = point.Enabled;
				if (ImGui::Checkbox("##on", &enabled)) {
					debug.Enable(point.Source, point.Line, enabled);
				}

				ImGui::SameLine();
				ImGui::Text("%s:%d", point.Source.c_str(), point.Line);

				ImGui::SameLine();
				ImGui::TextDisabled(
					"%s · %llu hit%s",
					point.Action == BreakAction::Stop ? "stop" : "capture",
					static_cast<unsigned long long>(point.Hits),
					point.Hits == 1 ? "" : "s"
				);

				ImGui::SameLine();
				if (ImGui::SmallButton("x")) {
					// Queued: removing inside the walk would erase the vector
					// being iterated.
					removeSource = point.Source;
					removeLine = point.Line;
				}

				ImGui::PopID();
				ImGui::PopID();
			}

			if (removeLine > 0) {
				debug.Remove(removeSource, removeLine);
			}

			if (debug.Breakpoints().empty()) {
				ImGui::TextDisabled("no breakpoints in this world");
			}

			// --- what they caught -----------------------------------------------
			ImGui::Separator();

			if (debug.Hits().empty()) {
				ImGui::TextDisabled("nothing caught yet");
			} else {
				if (ImGui::SmallButton("Clear caught")) {
					debug.ClearHits();
				}

				// **Newest first.** The log keeps the most recent hits and drops
				// the oldest, so somebody watching a loop is reading the end of
				// it — and making them scroll there every time would be an
				// interface arguing with its own data.
				const std::span<const DebugHit> hits = debug.Hits();
				for (size_t index = hits.size(); index-- > 0;) {
					const DebugHit &hit = hits[index];

					char label[192];
					std::snprintf(
						label, sizeof(label), "%s:%d###hit%zu", hit.Source.c_str(), hit.Line, index
					);

					if (!ImGui::TreeNode(label)) {
						continue;
					}

					for (const DebugFrame &frame : hit.Frames) {
						ImGui::TextDisabled(
							"%s:%d %s",
							frame.Source.c_str(),
							frame.Line,
							frame.Function.empty() ? "(chunk)" : frame.Function.c_str()
						);

						if (frame.Locals.empty()) {
							continue;
						}

						ImGui::Indent();
						for (const DebugLocal &local : frame.Locals) {
							ImGui::Text("%s", local.Name.c_str());
							ImGui::SameLine();
							ImGui::TextDisabled("= %s", local.Value.c_str());
						}
						ImGui::Unindent();
					}

					ImGui::TreePop();
				}
			}

			ImGui::PopID();
		}

		ImGui::End();
	}
}
