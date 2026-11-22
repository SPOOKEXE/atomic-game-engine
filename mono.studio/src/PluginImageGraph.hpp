#pragma once

#include "ImageComposer.hpp"

#include <string>
#include <string_view>

namespace studio {
	// An image graph node editor a plugin hosts in one of its dock widgets. The plugin
	// owns the document text; the canvas is a view over it and reports each valid edit.
	struct PluginImageGraphEditor {
		// Canvas, history and selection, shared with the Image Composer.
		ImageComposerState State;
		// Canonical JSON of the last valid document, as the plugin last saw it.
		std::string Document;
		// Why the current canvas is not a valid document, empty when it is.
		std::string Error;
	};

	// Loads a graph document into the editor. Refusal keeps the previous graph.
	bool OpenPluginImageGraph(PluginImageGraphEditor &, std::string_view json, std::string &failure);

	// Draws the canvas, palette and inspector into the current ImGui window. Returns
	// true with the new canonical JSON when an edit produced a different valid document.
	bool DrawPluginImageGraph(PluginImageGraphEditor &, std::string &changed);

	// Canonical JSON for the current canvas; false while the canvas is incomplete.
	bool SavePluginImageGraph(PluginImageGraphEditor &, std::string &json);
}
