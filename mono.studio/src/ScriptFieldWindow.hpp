#pragma once

// Resolves the child window owned by a script's multiline input.
//
// InputTextMultiline derives its child window name from its parent and id, so
// the literal field label is never a window name. The hierarchical id captured
// beside the field is the stable seam used by syntax drawing, the gutter, the
// completion popup, hover lookup and the minimap.

#include <imgui.h>
#include <string_view>

struct ImGuiWindow;

namespace studio {

	struct CodeEdit;

	ImGuiWindow *FindCodeField(ImGuiID fieldId);

	// Paints syntax and the caret into the multiline field child.
	void DrawScriptSource(
		std::string_view text, const CodeEdit &edit, ImVec2 fieldMin, ImVec2 fieldSize, ImGuiID fieldId
	);

}
