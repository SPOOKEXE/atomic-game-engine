// Breakpoints, and what they caught.
//
// **This is a capture debugger and the panel says so.** `script/Debugger.hpp`
// carries the argument: a paused world is not a replayable one, and the editor's
// frame loop is the thread the VM runs on - so holding a script inside a line
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
	using studio::Label;

	void studio::Editor::DrawDebugger() {
		if (!ShowDebugger) {
			return;
		}

		if (!ImGui::Begin("Debugger", &ShowDebugger)) {
			ImGui::End();
			return;
		}

		// --- adding one --------------------------------------------------------

		ImGui::SetNextItemWidth(180.0f * Settings.Scale);
		TextField("##break-source", BreakSource, "script path - enemy.luau");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(70.0f * Settings.Scale);
		ImGui::InputInt("##break-line", &BreakLine, 0, 0);
		ImGui::SameLine();

		const bool valid = !BreakSource.empty() && BreakLine > 0;
		ImGui::BeginDisabled(!valid);
		if (ImGui::Button("Add")) {
			// **Into the editor's list first, then mirrored into anything
			// already running.** The editor's is the one that survives a Stop;
			// a runtime's is a copy it was handed at `BeginRun`. Adding only to
			// the live runtimes would make a breakpoint disappear the next time
			// somebody pressed Stop, which is the behaviour this arrangement
			// exists to prevent.
			// **The same refusal the gutter gives, from the same function.** A
			// panel that accepted what a gutter click refused would be two
			// answers to one question.
			const std::string_view refused = engine::script::BreakpointsRefused(BreakSource);

			if (!refused.empty()) {
				Say("cannot break in " + BreakSource + ": " + std::string(refused),
					engine::core::LogLevel::Warning);
			} else {
				const BreakAction action = BreakStops ? BreakAction::Stop : BreakAction::Capture;
				Breakpoints.Add(BreakSource, BreakLine, action);

				// Every running world, because a breakpoint is a thing about a
				// *script* and the same script may be running in several -
				// asking which one somebody meant would be a question with no
				// answer they could give.
				for (WorldRun &run : Runs) {
					if (run.Runtime != nullptr) {
						run.Runtime->Debug().Add(BreakSource, BreakLine, action);
					}
				}
			}
		}
		ImGui::EndDisabled();

		// **Said before the button is pressed as well as after.** Somebody who
		// has typed a `.ts` path should be told it cannot carry a breakpoint
		// while they are looking at the field, not once they have given up.
		if (const std::string_view refused = engine::script::BreakpointsRefused(BreakSource);
			!refused.empty()) {
			ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
			ImGui::TextWrapped("%s", std::string(refused).c_str());
			ImGui::PopStyleColor();
		}

		ImGui::SameLine();
		ImGui::Checkbox("stop", &BreakStops);
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip(
				"Stop ends that script's run with an ordinary error.\n"
				"Capture records the stack and lets it carry on."
			);
		}

		// The thing somebody has to know before they trust this panel.
		ImGui::TextDisabled("captures the stack - it does not pause the world");
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip(
				"A held tick is work crossing a tick boundary, which is what makes\n"
				"a recording stop replaying - and the editor's frame loop is the\n"
				"thread the VM runs on, so a pause would freeze this window too."
			);
		}

		ImGui::Separator();

		if (Runs.empty()) {
			ImGui::TextDisabled("nothing is running - press Play or Run");
			ImGui::End();
			return;
		}

		// --- the breakpoints, which belong to the editor ----------------------
		{
			std::string removeSource;
			int removeLine = 0;

			for (const Breakpoint &point : Breakpoints.Breakpoints()) {
				ImGui::PushID(point.Line);
				ImGui::PushID(point.Source.c_str());

				bool enabled = point.Enabled;
				if (ImGui::Checkbox("##on", &enabled)) {
					Breakpoints.Enable(point.Source, point.Line, enabled);
					for (WorldRun &run : Runs) {
						if (run.Runtime != nullptr) {
							run.Runtime->Debug().Enable(point.Source, point.Line, enabled);
						}
					}
				}

				ImGui::SameLine();
				ImGui::Text("%s:%d", point.Source.c_str(), point.Line);

				// **Summed across the runs, because the master list never fires
				// one.** The editor's copy is the record of what somebody asked
				// for; the counting happens in the VMs.
				uint64_t hits = 0;
				for (const WorldRun &run : Runs) {
					if (run.Runtime == nullptr) {
						continue;
					}
					for (const Breakpoint &live : run.Runtime->Debug().Breakpoints()) {
						if (live.Line == point.Line && live.Source == point.Source) {
							hits += live.Hits;
						}
					}
				}

				ImGui::SameLine();
				ImGui::TextDisabled(
					"%s · %llu hit%s",
					point.Action == BreakAction::Stop ? "stop" : "capture",
					static_cast<unsigned long long>(hits),
					hits == 1 ? "" : "s"
				);

				ImGui::SameLine();
				if (ImGui::SmallButton("x")) {
					removeSource = point.Source;
					removeLine = point.Line;
				}

				ImGui::PopID();
				ImGui::PopID();
			}

			if (removeLine > 0) {
				Breakpoints.Remove(removeSource, removeLine);
				for (WorldRun &run : Runs) {
					if (run.Runtime != nullptr) {
						run.Runtime->Debug().Remove(removeSource, removeLine);
					}
				}
			}

			if (Breakpoints.Breakpoints().empty()) {
				ImGui::TextDisabled("no breakpoints");
			}
		}

		ImGui::Separator();

		for (WorldRun &run : Runs) {
			if (run.Runtime == nullptr) {
				continue;
			}

			engine::script::Debugger &debug = run.Runtime->Debug();
			const std::string world(studio::Label(this->Universe->NameOf(run.World)));

			char header[192];
			std::snprintf(
				header,
				sizeof(header),
				"%s - %zu breakpoint%s, %zu caught###%u",
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

			// --- what this world caught ------------------------------------------
			//
			// The breakpoints themselves are above and belong to the editor;
			// what is per-world is what actually fired here.

			if (debug.Hits().empty()) {
				ImGui::TextDisabled("nothing caught yet");
			} else {
				if (ImGui::SmallButton("Clear caught")) {
					debug.ClearHits();
					SelectedHit = 0;
					SelectedFrame = 0;
				}

				const std::span<const DebugHit> hits = debug.Hits();

				// **A list beside a detail view rather than a tree of trees.**
				// The old shape put every frame of every hit inside nested
				// nodes, so reading one local meant opening three things and
				// the panel was mostly disclosure triangles. A capture has two
				// axes - which hit, and which frame of it - and two lists is
				// what that is.
				const float side = 220.0f * Settings.Scale;
				ImGui::BeginChild("##hits", ImVec2(side, 180.0f * Settings.Scale), true);

				// **Newest first.** The log keeps the most recent hits and drops
				// the oldest, so somebody watching a loop is reading the end of
				// it - and making them scroll there every time would be an
				// interface arguing with its own data.
				for (size_t index = hits.size(); index-- > 0;) {
					const DebugHit &hit = hits[index];

					char label[192];
					std::snprintf(
						label, sizeof(label), "%s:%d###hit%zu", hit.Source.c_str(), hit.Line, index
					);

					const bool chosen = SelectedWorld == run.World.Index && SelectedHit == index;
					if (ImGui::Selectable(label, chosen)) {
						SelectedWorld = run.World.Index;
						SelectedHit = index;

						// **The innermost frame, which is where the breakpoint
						// was.** Keeping the previous frame index would point
						// somewhere arbitrary in a stack of a different depth.
						SelectedFrame = 0;
					}
				}
				ImGui::EndChild();

				ImGui::SameLine();
				ImGui::BeginChild("##frames", ImVec2(0.0f, 180.0f * Settings.Scale), true);

				const bool showing = SelectedWorld == run.World.Index && SelectedHit < hits.size();
				if (!showing) {
					ImGui::TextDisabled("pick a capture");
				} else {
					const DebugHit &hit = hits[SelectedHit];

					if (hit.Instance != engine::ecs::NULL_ENTITY) {
						// Which script was running, when the runtime knew - a
						// path alone does not say which of two instances
						// sharing a module was the one that got here.
						ImGui::TextDisabled("script #%llu", static_cast<unsigned long long>(hit.Instance.Id));
					}

					if (ImGui::BeginTabBar("##capture")) {
						if (ImGui::BeginTabItem("Call Stack")) {
							// Innermost first, which is the order a stack is
							// read in and the order the capture walked it.
							for (size_t at = 0; at < hit.Frames.size(); at++) {
								const DebugFrame &frame = hit.Frames[at];

								char row[224];
								std::snprintf(
									row,
									sizeof(row),
									"%zu  %s  %s:%d###frame%zu",
									at,
									frame.Function.empty() ? "(chunk)" : frame.Function.c_str(),
									frame.Source.c_str(),
									frame.Line,
									at
								);

								if (ImGui::Selectable(row, SelectedFrame == at)) {
									SelectedFrame = at;
								}
							}
							ImGui::EndTabItem();
						}

						const DebugFrame *frame =
							SelectedFrame < hit.Frames.size() ? &hit.Frames[SelectedFrame] : nullptr;

						// **Locals and upvalues are two tabs, not one list.** A
						// local is a value this frame made and an upvalue is one
						// it captured from an enclosing scope - so "why did this
						// change when nothing here touched it" is a question
						// only the second answers, and merging them loses it.
						if (ImGui::BeginTabItem("Locals")) {
							DrawDebugValues(frame == nullptr ? nullptr : &frame->Locals, "no locals");
							ImGui::EndTabItem();
						}
						if (ImGui::BeginTabItem("Upvalues")) {
							DrawDebugValues(
								frame == nullptr ? nullptr : &frame->Upvalues,
								"nothing captured - a chunk's own frame closes over nothing in Luau"
							);
							ImGui::EndTabItem();
						}

						ImGui::EndTabBar();
					}
				}
				ImGui::EndChild();
			}

			ImGui::PopID();
		}

		ImGui::End();
	}

	void studio::Editor::DrawDebugValues(const std::vector<DebugLocal> *values, const char *empty) {
		if (values == nullptr) {
			ImGui::TextDisabled("pick a frame");
			return;
		}
		if (values->empty()) {
			// **The reason, not just the absence.** An empty upvalue list is the
			// ordinary state of a chunk's own frame in Luau, and a panel that
			// only said "none" would read as the capture having failed.
			ImGui::TextDisabled("%s", empty);
			return;
		}

		if (!ImGui::BeginTable("##values", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
			return;
		}

		ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 140.0f * Settings.Scale);
		ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableHeadersRow();

		for (const DebugLocal &value : *values) {
			ImGui::TableNextRow();

			ImGui::TableNextColumn();
			ImGui::TextUnformatted(value.Name.c_str());

			ImGui::TableNextColumn();

			// Wrapped rather than truncated: a rendered table or a long string
			// is exactly the value somebody opened this panel to read.
			ImGui::TextWrapped("%s", value.Value.c_str());
		}

		ImGui::EndTable();
	}
}

void studio::Editor::DrawCallStack() {
	if (!ShowCallStack) {
		return;
	}

	if (!ImGui::Begin("Call Stack", &ShowCallStack)) {
		ImGui::End();
		return;
	}

	if (Runs.empty()) {
		ImGui::TextDisabled("nothing is running - press Play or Run");
		ImGui::End();
		return;
	}

	// Find the selected hit across all runs
	const DebugHit *selectedHit = nullptr;
	WorldId selectedWorld = WorldId{};
	for (WorldRun &run : Runs) {
		if (run.Runtime == nullptr) continue;
		if (SelectedWorld == run.World.Index && SelectedHit < run.Runtime->Debug().Hits().size()) {
			selectedHit = &run.Runtime->Debug().Hits()[SelectedHit];
			selectedWorld = run.World;
			break;
		}
	}

	if (selectedHit == nullptr) {
		ImGui::TextDisabled("no capture selected - pick one in the Debugger panel");
		ImGui::End();
		return;
	}

	const std::string worldLabel(studio::Label(this->Universe->NameOf(selectedWorld)));
	ImGui::TextDisabled("world: %s", worldLabel.c_str());

	if (ImGui::BeginTable(
			"##callstack",
			3,
			ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable |
				ImGuiTableFlags_ScrollY
		)) {
		ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 40.0f * Settings.Scale);
		ImGui::TableSetupColumn("Function", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("Location", ImGuiTableColumnFlags_WidthFixed, 200.0f * Settings.Scale);
		ImGui::TableHeadersRow();

		for (size_t at = 0; at < selectedHit->Frames.size(); at++) {
			const DebugFrame &frame = selectedHit->Frames[at];

			ImGui::TableNextRow();

			ImGui::TableNextColumn();
			ImGui::Text("%zu", at);

			ImGui::TableNextColumn();
			const char *funcName = frame.Function.empty() ? "(chunk)" : frame.Function.c_str();
			bool isSelected = (SelectedFrame == at);
			if (ImGui::Selectable(funcName, isSelected, ImGuiSelectableFlags_SpanAllColumns)) {
				SelectedFrame = at;
			}

			ImGui::TableNextColumn();
			ImGui::Text("%s:%d", frame.Source.c_str(), frame.Line);
		}

		ImGui::EndTable();
	}

	ImGui::Separator();

	// Show locals/upvalues for selected frame
	const DebugFrame *frame =
		SelectedFrame < selectedHit->Frames.size() ? &selectedHit->Frames[SelectedFrame] : nullptr;

	if (frame != nullptr) {
		if (ImGui::BeginTabBar("##callstack_values")) {
			if (ImGui::BeginTabItem("Locals")) {
				DrawDebugValues(&frame->Locals, "no locals");
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("Upvalues")) {
				DrawDebugValues(
					&frame->Upvalues, "nothing captured - a chunk's own frame closes over nothing in Luau"
				);
				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		}
	}

	ImGui::End();
}

void studio::Editor::DrawBreakpointsWatch() {
	if (!ShowBreakpointsWatch) {
		return;
	}

	if (!ImGui::Begin("Breakpoints", &ShowBreakpointsWatch)) {
		ImGui::End();
		return;
	}

	if (Breakpoints.Breakpoints().empty() && Runs.empty()) {
		ImGui::TextDisabled("no breakpoints set and nothing running");
		ImGui::End();
		return;
	}

	// Master breakpoints (editor's list)
	if (!Breakpoints.Breakpoints().empty()) {
		if (ImGui::CollapsingHeader("Master Breakpoints", ImGuiTreeNodeFlags_DefaultOpen)) {
			std::string removeSource;
			int removeLine = 0;

			for (const auto &point : Breakpoints.Breakpoints()) {
				ImGui::PushID(point.Line);
				ImGui::PushID(point.Source.c_str());

				bool enabled = point.Enabled;
				if (ImGui::Checkbox("##master_on", &enabled)) {
					Breakpoints.Enable(point.Source, point.Line, enabled);
					for (WorldRun &run : Runs) {
						if (run.Runtime != nullptr) {
							run.Runtime->Debug().Enable(point.Source, point.Line, enabled);
						}
					}
				}

				ImGui::SameLine();
				ImGui::Text("%s:%d", point.Source.c_str(), point.Line);

				ImGui::SameLine();
				ImGui::TextDisabled(
					"%s", point.Action == engine::script::BreakAction::Stop ? "stop" : "capture"
				);

				ImGui::SameLine();
				if (ImGui::SmallButton("Remove##master")) {
					removeSource = point.Source;
					removeLine = point.Line;
				}

				ImGui::PopID();
				ImGui::PopID();
			}

			if (removeLine > 0) {
				Breakpoints.Remove(removeSource, removeLine);
				for (WorldRun &run : Runs) {
					if (run.Runtime != nullptr) {
						run.Runtime->Debug().Remove(removeSource, removeLine);
					}
				}
			}
		}
	}

	// Per-world breakpoints with hit counts
	if (!Runs.empty()) {
		ImGui::Separator();
		for (size_t runIdx = 0; runIdx < Runs.size(); ++runIdx) {
			WorldRun &run = Runs[runIdx];
			if (run.Runtime == nullptr) continue;

			engine::script::Debugger &debug = run.Runtime->Debug();
			const std::string worldLabel(studio::Label(this->Universe->NameOf(run.World)));

			if (debug.Breakpoints().empty() && debug.Hits().empty()) continue;

			std::string header = worldLabel + "###world_bp_" + std::to_string(run.World.Index);
			if (ImGui::CollapsingHeader(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
				if (debug.Breakpoints().empty()) {
					ImGui::TextDisabled("no breakpoints in this world");
				} else {
					std::string removeSource;
					int removeLine = 0;

					for (const auto &point : debug.Breakpoints()) {
						ImGui::PushID(point.Line);
						ImGui::PushID(point.Source.c_str());

						bool enabled = point.Enabled;
						if (ImGui::Checkbox("##wp_on", &enabled)) {
							debug.Enable(point.Source, point.Line, enabled);
						}

						ImGui::SameLine();
						ImGui::Text("%s:%d", point.Source.c_str(), point.Line);

						ImGui::SameLine();
						ImGui::TextDisabled(
							"%s · %llu hit%s",
							point.Action == engine::script::BreakAction::Stop ? "stop" : "capture",
							static_cast<unsigned long long>(point.Hits),
							point.Hits == 1 ? "" : "s"
						);

						ImGui::SameLine();
						if (ImGui::SmallButton("Remove##wp")) {
							removeSource = point.Source;
							removeLine = point.Line;
						}

						ImGui::PopID();
						ImGui::PopID();
					}

					if (removeLine > 0) {
						debug.Remove(removeSource, removeLine);
					}
				}

				if (!debug.Hits().empty()) {
					ImGui::Separator();
					ImGui::TextDisabled("%zu capture(s)", debug.Hits().size());
				}
			}
		}
	}

	ImGui::End();
}
