// The operator table's contents, and the palette that walks it.
//
// **The registrations are here rather than beside the things they call**, which
// is the one arrangement that makes the table readable as a list of what the
// editor does. Scattered across `Interface.cpp`, `Explorer.cpp` and `Editor.cpp`
// it would be a registry nobody could read in one go - and the reason for having
// it at all is that there is one place to look.

#include <engine/core/Log.hpp>
#include <engine/ui/Theme.hpp>

#include <imgui.h>

#include <studio/Editor.hpp>
#include <studio/Keybinds.hpp>
#include <studio/Widgets.hpp>

#include <string>

namespace studio {

	namespace {
		// The reason a command that edits the selection gives when there is
		// none. Written once because it is the same sentence four times, and
		// four hand-written versions of it is how a program ends up telling
		// somebody "no selection" in one place and "nothing selected" in
		// another.
		Availability NeedsSelection(bool have) {
			return have ? Availability::Yes() : Availability::No("nothing is selected");
		}
	}

	void Editor::RegisterOperators() {
		const auto always = [] { return Availability::Yes(); };

		// --- the file -------------------------------------------------------

		Operators.Add({Action::NewGame, "New Game", "Start an empty universe", always, [this] {
						   NewGame();
					   }});

		Operators.Add({Action::OpenGame, "Open Game", "Open a .agame file", always, [this] {
						   AskingOpen = true;
						   PathBuffer = GamePath.string();
					   }});

		Operators.Add({Action::Save,
					   "Save",
					   "Write the game to its file",
					   [this] {
						   return Modified ? Availability::Yes()
										   : Availability::No("nothing has changed");
					   },
					   [this] {
						   if (GamePath.empty()) {
							   AskingSaveAs = true;
							   PathBuffer = GamePath.string();
						   } else {
							   SaveGame(GamePath);
						   }
					   }});

		Operators.Add({Action::SaveAs, "Save As", "Write the game somewhere new", always, [this] {
						   AskingSaveAs = true;
						   PathBuffer = GamePath.string();
					   }});

		// --- running --------------------------------------------------------

		// **Scoped to the focused viewport's world, not "the" world**, for the
		// reason the toolbar gives at length: with runs per world, a transport
		// that always described the active world was describing the wrong one
		// half the time.
		Operators.Add({Action::Play,
					   "Play",
					   "Run the server and a client in this process",
					   [this] {
						   return ViewportWorld(FocusedViewport).IsValid()
									? Availability::Yes()
									: Availability::No("this viewport has no scene");
					   },
					   [this] {
						   const engine::world::WorldId scope = ViewportWorld(FocusedViewport);
						   SetRunMode(
							   scope, ModeOf(scope) == RunMode::Play ? RunMode::Edit : RunMode::Play
						   );
					   }});

		Operators.Add({Action::RunServer,
					   "Run",
					   "Run the server's scripts only",
					   [this] {
						   return ViewportWorld(FocusedViewport).IsValid()
									? Availability::Yes()
									: Availability::No("this viewport has no scene");
					   },
					   [this] {
						   const engine::world::WorldId scope = ViewportWorld(FocusedViewport);
						   SetRunMode(
							   scope,
							   ModeOf(scope) == RunMode::Server ? RunMode::Edit : RunMode::Server
						   );
					   }});

		Operators.Add({Action::Stop,
					   "Stop",
					   "Stop and restore the scene as it was",
					   [this] {
						   return AnyRunning() ? Availability::Yes()
											   : Availability::No("nothing is running");
					   },
					   [this] { EndAllRuns(); }});

		// --- editing --------------------------------------------------------

		Operators.Add({Action::Undo,
					   "Undo",
					   "Reverse the last edit",
					   [this] {
						   return Commands != nullptr && Commands->CanUndo()
									? Availability::Yes()
									: Availability::No("there is nothing to undo");
					   },
					   [this] { UndoEdit(); }});

		Operators.Add({Action::Redo,
					   "Redo",
					   "Reapply the last undone edit",
					   [this] {
						   return Commands != nullptr && Commands->CanRedo()
									? Availability::Yes()
									: Availability::No("there is nothing to redo");
					   },
					   [this] { RedoEdit(); }});

		Operators.Add({Action::Duplicate,
					   "Duplicate",
					   "Copy the selection beside itself",
					   [this] { return NeedsSelection(!Selection.empty()); },
					   [this] { DuplicateSelection(); }});

		Operators.Add({Action::Delete,
					   "Delete",
					   "Delete the selection",
					   [this] { return NeedsSelection(!Selection.empty()); },
					   [this] { DeleteSelection(); }});

		Operators.Add({Action::SelectNone,
					   "Select None",
					   "Clear the selection",
					   [this] { return NeedsSelection(!Selection.empty()); },
					   [this] { ClearSelection(); }});

		Operators.Add({Action::Rename,
					   "Rename",
					   "Rename the selected instance",
					   [this] { return NeedsSelection(!Selection.empty()); },
					   [this] { BeginRename(Selection.empty() ? Entity{} : Selection.front()); }});

		// --- the manipulators -----------------------------------------------
		//
		// **Registered rather than switched on directly by the key**, like
		// everything else here: the ribbon's buttons, the palette and the
		// binding all end up in one place, so a fifth way to change tool cannot
		// disagree with the other four about what "Scale" means.
		//
		// Always available. A tool with nothing selected draws no handles and
		// costs nothing, and greying the buttons would make picking a tool
		// something you can only do *after* choosing what to point it at -
		// which is the wrong way round for a person who selects with the tool
		// already in hand.
		const auto tool = [this, always](Action id, const char *name, const char *what, ToolMode mode) {
			Operators.Add({id, name, what, always, [this, mode] {
							   CurrentTool = mode;
						   }});
		};

		tool(Action::ToolSelect, "Select Tool", "Click to select, with no handles", ToolMode::Select);
		tool(Action::ToolMove, "Move Tool", "Drag an axis to move the selection", ToolMode::Move);
		tool(Action::ToolRotate, "Rotate Tool", "Drag a ring to turn the selection", ToolMode::Rotate);
		tool(Action::ToolScale, "Scale Tool", "Drag an axis to resize the selection", ToolMode::Scale);

		// --- the panels -----------------------------------------------------

		Operators.Add({Action::ShowStatistics, "Statistics", "Show the frame rate panel", always, [this] {
						   ShowStatistics = true;
					   }});

		Operators.Add({Action::ShowFrameGraph, "Frame Graph", "Show where the frame went", always, [this] {
						   ShowFrameGraph = true;
					   }});
		Operators.Add({Action::ShowHeap, "Heap", "Show where the memory went", always, [this] {
						   ShowHeap = true;
					   }});

		Operators.Add({Action::CommandPalette, "Command Palette", "Find and run any command", always, [this] {
						   ShowPalette = true;
					   }});

		// **The join, checked where it is made.** `tests/Operators.cpp` proves
		// the property is satisfiable; this proves these registrations satisfy
		// it. Without it, an `Action` added to `Keybinds.cpp` and forgotten here
		// is a command with a binding and no behaviour - the key does nothing,
		// the palette does not list it, and neither says why.
		//
		// A warning rather than an abort: a missing operator makes one command
		// unreachable and an editor that refused to start over it would make
		// every command unreachable.
		for (const Keybind &binding : Keybinds::All()) {
			if (Operators.Find(binding.Bound) == nullptr) {
				ENGINE_WARN("command '{}' has a binding and no operator", binding.Id);
			}
		}
	}

	void Editor::DrawPalette() {
		if (ShowPalette) {
			ImGui::OpenPopup("##palette");
		}

		// Positioned near the top rather than centred: the list grows downwards
		// as you type, and a centred palette moves its own first row every
		// keystroke.
		const ImGuiViewport *viewport = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(
			ImVec2(viewport->GetCenter().x, viewport->WorkPos.y + viewport->WorkSize.y * 0.18f),
			ImGuiCond_Always,
			ImVec2(0.5f, 0.0f)
		);
		ImGui::SetNextWindowSize(ImVec2(520.0f * Settings.Scale, 0.0f), ImGuiCond_Always);

		if (!ImGui::BeginPopup("##palette")) {
			ShowPalette = false;
			return;
		}

		// Focused on the frame it opens, so the first keystroke is part of the
		// query rather than being swallowed. The same trick `PathPrompt` uses.
		if (ImGui::IsWindowAppearing()) {
			ImGui::SetKeyboardFocusHere();
			PaletteQuery.clear();
			PaletteCursor = 0;
		}

		ImGui::SetNextItemWidth(-1.0f);
		if (TextField("##palette-query", PaletteQuery, "type a command")) {
			// The row that was second for one query is not the second for the
			// next, so the cursor cannot survive a change to the filter.
			PaletteCursor = 0;
		}

		const std::vector<const Operator *> found = Operators.Matching(PaletteQuery);

		if (found.empty()) {
			ImGui::Separator();
			ImGui::TextDisabled("no command matches");
			ImGui::EndPopup();
			return;
		}

		const int last = static_cast<int>(found.size()) - 1;
		if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
			PaletteCursor = PaletteCursor >= last ? 0 : PaletteCursor + 1;
		}
		if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
			PaletteCursor = PaletteCursor <= 0 ? last : PaletteCursor - 1;
		}
		PaletteCursor = std::clamp(PaletteCursor, 0, last);

		ImGui::Separator();

		// **Chosen outside the loop and run after `EndPopup`.** Running an
		// operator from inside the row that drew it would let an operator that
		// closes the palette - or opens a modal - do so while imgui is still
		// inside the popup it belongs to.
		const Operator *chosen = nullptr;

		if (ImGui::BeginChild("##palette-rows", ImVec2(0.0f, 320.0f * Settings.Scale))) {
			for (int index = 0; index <= last; index++) {
				const Operator *op = found[static_cast<size_t>(index)];
				const Availability state = op->Poll();

				// The id is pinned to the command rather than to the label, so
				// a row keeps its identity as the filter reorders the list.
				ImGui::PushID(static_cast<int>(op->Id));

				if (!state.Ready) {
					ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
				}

				if (ImGui::Selectable(
						std::string(op->Name).c_str(),
						index == PaletteCursor,
						ImGuiSelectableFlags_AllowDoubleClick
					)) {
					chosen = op;
				}

				if (!state.Ready) {
					ImGui::PopStyleColor();
				}

				// **The reason, where a shortcut would go when there is one.**
				// A greyed row that does not say why is a row somebody clicks
				// twice before giving up. See `Operators.hpp`.
				const std::string right =
					state.Ready ? Keybinds::Of(op->Id).Text() : state.Reason;

				if (!right.empty()) {
					ImGui::SameLine();
					const float width = ImGui::CalcTextSize(right.c_str()).x;
					ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - width);
					ImGui::TextDisabled("%s", right.c_str());
				}

				if (index == PaletteCursor && ImGui::IsWindowAppearing()) {
					ImGui::SetScrollHereY();
				}

				ImGui::PopID();
			}
		}
		ImGui::EndChild();

		if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
			chosen = found[static_cast<size_t>(PaletteCursor)];
		}

		if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
			ShowPalette = false;
			ImGui::CloseCurrentPopup();
		}

		if (chosen != nullptr) {
			ShowPalette = false;
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();

		// Outside the popup, per the note above. An unavailable command is a
		// no-op rather than a refusal with a message: the reason is already on
		// the row that was clicked.
		if (chosen != nullptr) {
			Operators.Run(chosen->Id);
		}
	}
}
