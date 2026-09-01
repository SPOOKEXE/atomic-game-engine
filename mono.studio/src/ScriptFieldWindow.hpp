#pragma once

// Resolves the child window owned by a script's multiline input.
//
// InputTextMultiline derives its child window name from its parent and id, so
// the literal field label is never a window name. The hierarchical id captured
// beside the field is the stable seam used by syntax drawing, the gutter, the
// completion popup, hover lookup and the minimap.

#include <imgui.h>

struct ImGuiWindow;

namespace studio {

	ImGuiWindow *FindCodeField(ImGuiID fieldId);

}
