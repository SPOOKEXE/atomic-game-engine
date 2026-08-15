// The undo stack, as something a person can read.
//
// **Undo is only as trustworthy as it is visible.** The Edit menu names the next
// command and nothing names the rest, so the only way to find out what Ctrl+Z
// will do three presses from now is to press it three times and watch. That is
// the opposite of what an undo stack is for.
//
// `CommandLog` already holds everything this needs: two stacks of `Command`,
// each carrying the `Description` that was written for the Edit menu's label.
// This panel is a reader over them - it caches nothing, per `AGENTS.md`'s rule
// that a panel must never keep a copy of something the model owns, and walking
// to a point in the history is repeated `Undo`/`Redo` rather than a second way
// to move the stack.

#include <engine/ui/Theme.hpp>

#include <imgui.h>
#include <studio/Editor.hpp>

namespace studio {

	void Editor::DrawHistory() {
		if (!ShowHistory) {
			return;
		}

		if (!ImGui::Begin("History", &ShowHistory)) {
			ImGui::End();
			return;
		}

		if (Commands == nullptr) {
			ImGui::TextDisabled("no command log");
			ImGui::End();
			return;
		}

		const std::span<const Command> undoable = Commands->Undoable();
		const std::span<const Command> redoable = Commands->Redoable();

		if (undoable.empty() && redoable.empty()) {
			ImGui::TextDisabled("nothing has been edited yet");
			ImGui::End();
			return;
		}

		// **Counted rather than left for the reader to count.** "17 edits, 3
		// undone" is the sentence somebody wants before they start clicking.
		ImGui::Text("%zu edit%s", undoable.size(), undoable.size() == 1 ? "" : "s");
		if (!redoable.empty()) {
			ImGui::SameLine();
			ImGui::TextDisabled("· %zu undone", redoable.size());
		}
		ImGui::Separator();

		// How many steps the click asked for, applied after the walk rather than
		// inside it: `Undo` pops the vector these spans are over, so moving the
		// stack while iterating it is a walk over freed memory.
		int undoSteps = 0;
		int redoSteps = 0;

		if (ImGui::BeginChild("##history-rows")) {
			// **Oldest first, which is the reverse of the stack.** A history
			// list reads downwards in the order things happened; `Done.back()`
			// is the most recent. Presenting the vector as it is stored would
			// make the newest edit the top row, which nothing else in an editor
			// does.
			for (size_t index = 0; index < undoable.size(); index++) {
				const Command &command = undoable[index];

				// The row that Ctrl+Z would reverse, marked. Without it the list
				// is a history with no "you are here".
				const bool current = index + 1 == undoable.size();

				ImGui::PushID(static_cast<int>(index));
				if (ImGui::Selectable(command.Description.c_str(), current)) {
					// Clicking an entry means "put the world back to just after
					// this command", so everything above it is reversed.
					undoSteps = static_cast<int>(undoable.size() - index - 1);
				}
				ImGui::PopID();
			}

			// The undone ones, greyed, still in the order they happened - so an
			// edit does not jump across the list when it is undone.
			for (size_t index = redoable.size(); index > 0; index--) {
				const Command &command = redoable[index - 1];

				ImGui::PushID(static_cast<int>(undoable.size() + index));
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
				if (ImGui::Selectable(command.Description.c_str())) {
					// Redo far enough to include the row that was clicked.
					redoSteps = static_cast<int>(redoable.size() - index + 1);
				}
				ImGui::PopStyleColor();
				ImGui::PopID();
			}
		}
		ImGui::EndChild();

		ImGui::End();

		// **After `End`, and after the spans are out of scope.** `UndoEdit`
		// enters the world and clears the selection; doing that from inside the
		// panel's own draw is the re-entry `Universe::Enter` aborts on, and it
		// is the same rule every other action in this program follows.
		for (int step = 0; step < undoSteps; step++) {
			UndoEdit();
		}
		for (int step = 0; step < redoSteps; step++) {
			RedoEdit();
		}
	}
}
