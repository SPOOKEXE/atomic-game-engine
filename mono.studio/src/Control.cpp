// The editor's own rows in the control table.
//
// **`engine::control` already answers the protocol and the worlds**, so what is
// here is only what an editor has and a server does not: a selection, an output
// panel, a notion of which scene is active, and Play. Everything else - the
// handshake, `world_list`, `instance_set` - is the shared surface, registered
// first and then added to.
//
// **`engine_info` is replaced rather than extended.** The shared one knows about
// worlds; this one also knows which game file is open and how many frames have
// been drawn, and a client should get one answer to one question rather than two
// tools that overlap. `Surface::Add` replaces by name, which is what makes that
// a one-line decision.
//
// Every tool here runs on the editor thread, called from `PumpControl` in the
// frame loop, because `Universe::Enter` aborts on a foreign one.

#include <engine/core/Log.hpp>
#include <engine/core/Profiling.hpp>

#include <imgui.h>
#include <nlohmann/json.hpp>
#include <string>
#include <studio/Editor.hpp>

namespace studio {

	using engine::control::Tool;
	using nlohmann::json;

	void Editor::StartControl() {
		// **The port field is seeded whether or not the server starts**, because
		// the panel's Start button has to offer something sensible in the case
		// this function returns early - an editor launched without `--control`
		// is exactly the one somebody turns the surface on from the panel.
		if (Settings.ControlPort >= 0) {
			ControlPortField = Settings.ControlPort;
		}

		if (Settings.ControlPort < 0) {
			return;
		}

		// The shared tools first, then the editor's - so a replacement of one of
		// them is written after the thing it replaces.
		//
		// Guarded, because `ToggleControl` registers them too: the surface is
		// filled once per process and the socket is opened and closed as often
		// as somebody likes.
		if (ControlSurface.Count() == 0) {
			ControlSurface.AddUniverseTools(*Universe);
			RegisterControlTools();
		}

		if (!ControlServer.Start(static_cast<uint16_t>(Settings.ControlPort))) {
			Say("control: could not listen - is another program already on that port?",
				engine::core::LogLevel::Error);
			return;
		}

		Say("control: listening on 127.0.0.1:" + std::to_string(ControlServer.Port()) + " - " +
			std::to_string(ControlSurface.Count()) + " tools");
	}

	void Editor::PumpControl() {
		if (!ControlServer.IsRunning()) {
			return;
		}

		ENGINE_PROFILE("control");
		ControlServer.Pump([this](const std::string &line) { return ControlSurface.Answer(line); });
	}

	void Editor::RegisterControlTools() {
		Editor *editor = this;

		ControlSurface.Add(
			Tool{
				"engine_info",
				"The editor's own state: which game is open, whether it has unsaved changes, how many "
				"frames have been drawn, which scene is active and what is selected.",
				[] { return json{{"type", "object"}}; },
				[editor](const json &, std::string &) {
					json worlds = json::array();
					for (const WorldId id : editor->Universe->Worlds()) {
						worlds.push_back(std::string(editor->Universe->NameOf(id).Text()));
					}

					return json{
						{"game",
						 json{
							 {"name", std::string(editor->GameName.Text())},
							 {"path", editor->GamePath.string()},
							 {"modified", editor->Modified},
						 }},
						{"editor",
						 json{
							 {"headless", editor->Settings.Headless},
							 {"framesDrawn", editor->FramesDrawn},
							 {"activeWorld", std::string(editor->Universe->NameOf(editor->Active).Text())},
							 {"selection", editor->Selection.size()},
						 }},
						{"universe",
						 json{{"worlds", std::move(worlds)}, {"count", editor->Universe->Count()}}},
						{"control",
						 json{
							 {"port", editor->ControlServer.Port()},
							 {"served", editor->ControlServer.Served()},
						 }},
					};
				},
			}
		);

		ControlSurface.Add(
			Tool{
				"world_run",
				"Starts or stops one scene. `play` runs both halves in this process, `server` runs only "
				"Script instances, and `edit` stops it and restores the snapshot taken when it started - "
				"which is what makes running a scene non-destructive.",
				[] {
					return json{
						{"type", "object"},
						{"properties",
						 json{
							 {"world",
							  json{
								  {"type", "string"},
								  {"description", "Which scene. Defaults to the active one."}
							  }},
							 {"mode",
							  json{{"type", "string"}, {"enum", json::array({"edit", "server", "play"})}}},
						 }},
						{"required", json::array({"mode"})},
					};
				},
				[editor](const json &arguments, std::string &failure) -> json {
					const WorldId world = editor->ControlWorld(arguments, failure);
					if (!failure.empty()) {
						return nullptr;
					}

					const std::string mode = arguments.value("mode", std::string("edit"));
					RunMode wanted = RunMode::Edit;
					if (mode == "play") {
						wanted = RunMode::Play;
					} else if (mode == "server") {
						wanted = RunMode::Server;
					} else if (mode != "edit") {
						failure = "mode must be edit, server or play";
						return nullptr;
					}

					editor->SetRunMode(world, wanted);
					return json{
						{"world", std::string(editor->Universe->NameOf(world).Text())},
						{"mode", Describe(editor->ModeOf(world))},
					};
				},
			}
		);

		ControlSurface.Add(
			Tool{
				"selection_get",
				"What is selected in the editor right now.",
				[] { return json{{"type", "object"}}; },
				[editor](const json &, std::string &) {
					json ids = json::array();
					for (const Entity instance : editor->Selection) {
						ids.push_back(instance.Id);
					}
					return json{
						{"world", std::string(editor->Universe->NameOf(editor->SelectionWorld).Text())},
						{"instances", std::move(ids)},
					};
				},
			}
		);

		ControlSurface.Add(
			Tool{
				"select",
				"Selects instances in a scene, replacing whatever was selected.",
				[] {
					return json{
						{"type", "object"},
						{"properties",
						 json{
							 {"world", json{{"type", "string"}}},
							 {"ids", json{{"type", "array"}, {"items", json{{"type", "integer"}}}}},
						 }},
					};
				},
				[editor](const json &arguments, std::string &failure) -> json {
					const WorldId world = editor->ControlWorld(arguments, failure);
					if (!failure.empty()) {
						return nullptr;
					}

					editor->ClearSelection();
					if (arguments.contains("ids") && arguments["ids"].is_array()) {
						for (const json &id : arguments["ids"]) {
							editor->Select(world, Entity(id.get<uint64_t>()), true);
						}
					}

					json ids = json::array();
					for (const Entity instance : editor->Selection) {
						ids.push_back(instance.Id);
					}
					return json{{"instances", std::move(ids)}};
				},
			}
		);

		ControlSurface.Add(
			Tool{
				"log_tail",
				"The tail of the output panel: the engine log and everything scripts have printed.",
				[] {
					return json{
						{"type", "object"},
						{"properties",
						 json{
							 {"lines",
							  json{{"type", "integer"}, {"description", "From the end. Default 50."}}}
						 }},
					};
				},
				[editor](const json &arguments, std::string &) {
					const auto wanted = static_cast<size_t>(std::max(1, arguments.value("lines", 50)));
					const size_t from = editor->Output.size() > wanted ? editor->Output.size() - wanted : 0;

					json lines = json::array();
					for (size_t index = from; index < editor->Output.size(); index++) {
						lines.push_back(
							json{
								{"text", editor->Output[index].Text},
								{"level", static_cast<int>(editor->Output[index].Level)},
							}
						);
					}
					return json{{"lines", std::move(lines)}, {"total", editor->Output.size()}};
				},
			}
		);
	}

	WorldId Editor::ControlWorld(const json &arguments, std::string &failure) {
		// **No `world` means the active one**, which is the same default the
		// menus have: an author pressing Play with nothing chosen runs what they
		// are looking at. The shared surface defaults to the *first* world
		// instead, because a server has no notion of active.
		if (!arguments.contains("world") || !arguments["world"].is_string()) {
			return Active;
		}

		const std::string wanted = arguments["world"].get<std::string>();
		const WorldId found = Universe->Find(engine::core::Name(wanted.c_str()));
		if (!found.IsValid()) {
			failure = "no world called '" + wanted + "' - call world_list";
		}
		return found;
	}

	void Editor::ToggleControl() {
		if (ControlServer.IsRunning()) {
			ControlServer.Stop();
			Say("control: stopped listening");
			return;
		}

		// **The tools are registered once, not once per start.** `Surface::Add`
		// replaces by name, so a second registration would be harmless and a
		// second `AddUniverseTools` would still be work nobody asked for - and
		// the count in the log line would go on saying the same number while
		// doing it twice.
		if (ControlSurface.Count() == 0) {
			ControlSurface.AddUniverseTools(*Universe);
			RegisterControlTools();
		}

		if (!ControlServer.Start(static_cast<uint16_t>(std::max(0, ControlPortField)))) {
			Say("control: could not listen on port " + std::to_string(ControlPortField) +
					" - is another program already there?",
				engine::core::LogLevel::Error);
			return;
		}

		// Read back rather than echoed: a zero asks the operating system to
		// pick, and the number somebody needs is the one it picked.
		ControlPortField = ControlServer.Port();
		Say("control: listening on 127.0.0.1:" + std::to_string(ControlServer.Port()) + " - " +
			std::to_string(ControlSurface.Count()) + " tools");
	}

	void Editor::DrawControl() {
		if (!ShowControl) {
			return;
		}

		if (!ImGui::Begin("Control (MCP)", &ShowControl)) {
			ImGui::End();
			return;
		}

		const bool running = ControlServer.IsRunning();

		// **The state before the button**, because the question that brings
		// somebody here is "is it on", and a button whose label is the *action*
		// answers the opposite question at a glance.
		ImGui::TextUnformatted("Status");
		ImGui::Separator();

		if (running) {
			ImGui::TextColored(ImVec4(0.4f, 0.8f, 0.4f, 1.0f), "listening");
			ImGui::SameLine();
			ImGui::Text("on 127.0.0.1:%u", ControlServer.Port());

			ImGui::Text("client: %s", ControlServer.IsConnected() ? "connected" : "none attached");
			ImGui::Text("requests answered: %zu", ControlServer.Served());
		} else {
			ImGui::TextDisabled("not listening");
		}

		ImGui::Spacing();

		// The port is only editable while the socket is closed. Changing it
		// under a live listener would show a number that is not the one bound,
		// which is the one fact this panel exists to be right about.
		ImGui::BeginDisabled(running);
		ImGui::SetNextItemWidth(120.0f);
		ImGui::InputInt("Port", &ControlPortField);
		if (ControlPortField < 0) {
			ControlPortField = 0;
		}
		if (ControlPortField > 65535) {
			ControlPortField = 65535;
		}
		ImGui::EndDisabled();

		ImGui::SameLine();
		ImGui::TextDisabled("(?)");
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip(
				"Zero asks the operating system to pick one.\n"
				"Loopback only - nothing off this machine can reach it."
			);
		}

		if (ImGui::Button(running ? "Stop" : "Start", ImVec2(120.0f, 0.0f))) {
			ToggleControl();
		}

		ImGui::Spacing();
		ImGui::Spacing();

		// **The table this program actually declares, read from the registry.**
		// A hand-kept list here would be the duplicate `control/Surface.hpp`
		// exists to prevent, and a panel claiming a tool that is not registered
		// is worse than no panel - it is the one place somebody would check
		// before concluding their client was at fault.
		ImGui::Text("Tools (%zu)", ControlSurface.Count());
		ImGui::Separator();

		if (ControlSurface.Count() == 0) {
			ImGui::TextDisabled("nothing registered yet - start the server");
			ImGui::End();
			return;
		}

		if (ImGui::BeginTable(
				"control-tools", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0.0f, 0.0f)
			)) {
			ImGui::TableSetupColumn("Tool", ImGuiTableColumnFlags_WidthFixed, 160.0f);
			ImGui::TableSetupColumn("What it does", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableHeadersRow();

			for (const engine::control::Tool &tool : ControlSurface.Registered()) {
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(tool.Name.c_str());

				ImGui::TableNextColumn();

				// Wrapped rather than truncated: these descriptions are the only
				// documentation a client gets, so the panel showing them is the
				// place to read one in full.
				ImGui::TextWrapped("%s", tool.Description.c_str());
			}
			ImGui::EndTable();
		}

		ImGui::End();
	}
}
