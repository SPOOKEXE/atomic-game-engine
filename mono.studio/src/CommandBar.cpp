// A prompt that runs a line of Luau against the scene being edited.
//
// **The thing an editor is missing until it has one.** "Anchor everything I
// have selected", "rename these forty parts", "how many MeshParts are in this
// model" - each is one line, and without a prompt each is a plugin somebody has
// to write, put in a folder and reload.
//
// ## Why it is one waypoint
//
// A run is bracketed by a `ChangeHistoryService` recording, so whatever it did
// is **one** press of Ctrl+Z. A line that moves forty parts records forty
// commands and the person who typed it thinks of it as one action; forty
// presses to take it back would make the prompt something nobody dares use.
//
// A run that changed nothing commits nothing. Somebody who typed a query - a
// count, a print - should not find an empty step in their Edit menu afterwards.
//
// ## Why the runtime is kept
//
// Globals persist between commands, because a prompt where the previous line's
// work is gone is a prompt that can only ever do one thing at a time. It is
// rebuilt when the active scene changes, since a runtime is bound to one store.
//
// **Its own runtime rather than a plugin's**, so a command cannot spend a
// plugin's step budget or see its globals - the same reason plugins get one
// each.
//
// ## What a command cannot do
//
// A command runs once and is over: `BeatPlugins` never sees this runtime, so a
// widget or a toolbar it creates is never drawn and a handler it binds never
// fires again. That is the boundary rather than an omission - something that
// wants to be there next frame is a plugin, and a prompt that could quietly
// become one would be a plugin nobody can find the file for.
//
// ## Why it is not the command palette
//
// `Operators` is the list of things the *editor* can do, reachable by name.
// This runs code the editor has never heard of. The two look similar from the
// outside and share nothing: one is a menu, the other is a language.

#include <engine/core/Log.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/scripthost/Runtime.hpp>
#include <engine/ui/Theme.hpp>

#include <imgui.h>
#include <string>
#include <studio/Editor.hpp>
#include <utility>

namespace studio {

	namespace {
		// The panel's title, which is also its id in the saved layout.
		constexpr const char *COMMAND_BAR = "Command Bar";

		// What the recording is called in the Edit menu.
		//
		// One name for every command rather than the text that was run: a
		// menu item reading `for _,p in Selection:Get() do p.Anchored = true`
		// is a menu item nobody can read at a glance, and the command is in the
		// output panel anyway.
		constexpr const char *COMMAND_WAYPOINT = "Command";

		// How many commands are remembered.
		//
		// Bounded because it is kept for the life of the session and a person
		// at a prompt types a great many lines. Deep enough that walking back
		// to something from an hour ago works.
		constexpr size_t COMMAND_HISTORY_LIMIT = 200;
	}

	bool Editor::RunCommand(const std::string &source) {
		if (source.empty() || Universe == nullptr) {
			return false;
		}

		const engine::world::WorldId world = Active;
		if (!world.IsValid()) {
			Say("command: no scene is active", engine::core::LogLevel::Error);
			return false;
		}

		// **The recording opens before anything runs**, because a command's
		// first write is already an edit and a recording started afterwards
		// would leave it outside the group.
		std::optional<std::string> recording;
		const size_t before = Commands != nullptr ? Commands->Depth() : 0;
		if (Commands != nullptr) {
			recording = Commands->TryBeginRecording(COMMAND_WAYPOINT, "Command");
			if (!recording) {
				// Somebody else is mid-recording - a plugin that began one and
				// never finished. Refused rather than run outside it, because a
				// command whose changes landed in a plugin's undo step is a
				// command Ctrl+Z takes back at a moment nobody can predict.
				Say("command: another recording is already in progress", engine::core::LogLevel::Error);
				return false;
			}
		}

		bool ok = false;
		Universe->Enter(world, [&](engine::ecs::Store &store) {
			// Rebuilt when the scene changed, because a runtime is bound to one
			// store - and holding one against a world that has gone is the
			// dangling reference this rebuild exists to avoid.
			if (CommandHost.Vm == nullptr || !(CommandWorld == world)) {
				engine::script::RuntimeLimits limits;
				limits.Role.Server = false;
				limits.Role.Client = false;
				limits.Role.Studio = true;
				limits.Origin = engine::script::ScriptOrigin::Plugin;

				// The surface goes with the runtime that points at it, and in
				// that order: a host outliving its VM is a pointer nothing
				// owns.
				CommandHost.Vm.reset();
				CommandHost.Surface.reset();

				CommandHost.Manifest.Name = "Command Bar";
				CommandHost.Running = true;
				CommandHost.Vm = engine::script::MakeRuntime(store, engine::script::Language::Luau, limits);
				if (CommandHost.Vm == nullptr) {
					Say("command: could not start a runtime", engine::core::LogLevel::Error);
					return;
				}

				// **The same surface a plugin gets**, so `Selection`,
				// `ChangeHistoryService` and the script readers are all there.
				// A second, smaller surface for the prompt would be a second
				// vocabulary to learn for the same editor.
				CommandHost.Surface = MakePluginSurface(*this, CommandHost);
				if (CommandHost.Surface != nullptr) {
					CommandHost.Vm->SetHost(CommandHost.Surface.get());
				}
				CommandWorld = world;
			}

			ok = CommandHost.Vm->Run(source, "command");
			if (!ok) {
				// The whole error, because the person reading it is the person
				// who typed the line - a truncated message costs them the
				// column, which is the only part that matters.
				Say("command: " + CommandHost.Vm->LastError(), engine::core::LogLevel::Error);
			}
		});

		if (Commands != nullptr && recording) {
			// **Committed only when something happened.** A query that printed
			// a count is not an edit, and an empty step in the Edit menu is a
			// Ctrl+Z that appears to do nothing.
			const bool changed = Commands->Depth() > before;
			Commands->FinishRecording(
				*recording, changed ? FinishOperation::Commit : FinishOperation::Cancel
			);
		}

		return ok;
	}

	void Editor::DrawCommandBar() {
		if (!ShowCommandBar) {
			return;
		}

		if (!ImGui::Begin(COMMAND_BAR, &ShowCommandBar)) {
			ImGui::End();
			return;
		}

		ImGui::TextDisabled(
			"Luau, against the active scene. Enter runs it; one press of Ctrl+Z takes it back."
		);
		ImGui::Separator();

		// **Focused when the panel opens**, because a prompt somebody has to
		// click into before typing is a prompt they will stop opening.
		if (ImGui::IsWindowAppearing()) {
			ImGui::SetKeyboardFocusHere();
		}

		ImGui::SetNextItemWidth(-1.0f);

		const ImGuiInputTextFlags flags =
			ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory;

		// The arrows walk back through what has been run, which is what every
		// prompt does and the reason the history is kept at all.
		const auto history = [](ImGuiInputTextCallbackData *data) -> int {
			auto *editor = static_cast<Editor *>(data->UserData);
			return editor->WalkCommandHistory(data);
		};

		const bool submitted =
			ImGui::InputText("##command", CommandField, sizeof(CommandField), flags, history, this);

		if (submitted) {
			std::string source(CommandField);
			if (!source.empty()) {
				// Echoed before it runs, so the log reads as a transcript
				// rather than as answers with no questions.
				Say("> " + source);
				RunCommand(source);

				CommandHistory.push_back(std::move(source));
				while (CommandHistory.size() > COMMAND_HISTORY_LIMIT) {
					CommandHistory.erase(CommandHistory.begin());
				}
				CommandCursor = -1;
				CommandField[0] = '\0';
			}

			// Enter submits and keeps the focus, so a run of commands is typed
			// rather than clicked between.
			ImGui::SetKeyboardFocusHere(-1);
		}

		if (!CommandHistory.empty()) {
			ImGui::Spacing();
			ImGui::TextDisabled("%zu command(s) this session · up and down walk them", CommandHistory.size());
		}

		ImGui::End();
	}

	int Editor::WalkCommandHistory(ImGuiInputTextCallbackData *data) {
		if (CommandHistory.empty()) {
			return 0;
		}

		const int last = static_cast<int>(CommandHistory.size()) - 1;
		if (data->EventKey == ImGuiKey_UpArrow) {
			CommandCursor = CommandCursor < 0 ? last : (CommandCursor > 0 ? CommandCursor - 1 : 0);
		} else if (data->EventKey == ImGuiKey_DownArrow) {
			if (CommandCursor < 0) {
				return 0;
			}
			// Past the newest is back to an empty prompt, which is where
			// somebody expects down to land.
			CommandCursor = CommandCursor >= last ? -1 : CommandCursor + 1;
		} else {
			return 0;
		}

		const std::string text =
			CommandCursor < 0 ? std::string() : CommandHistory[static_cast<size_t>(CommandCursor)];
		data->DeleteChars(0, data->BufTextLen);
		data->InsertChars(0, text.c_str());
		return 0;
	}
}
