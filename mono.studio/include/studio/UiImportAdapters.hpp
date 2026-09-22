#pragma once

// Bounded text adapters that turn external authoring formats into the engine's
// canonical UI values before a Studio command mutates an ECS store.

#include <engine/gui/Document.hpp>
#include <engine/gui/Localization.hpp>

#include <cstddef>
#include <string_view>

namespace studio {
	struct UiDesignImportLimits {
		static constexpr size_t HARD_MAXIMUM_BYTES = 1024 * 1024;
		static constexpr size_t HARD_MAXIMUM_DEPTH = 32;

		size_t MaximumBytes = HARD_MAXIMUM_BYTES;
		size_t MaximumDepth = HARD_MAXIMUM_DEPTH;
	};

	// Imports schema version 1 design tokens:
	// {"version":1,"theme":{"id":"...","name":"...","tokens":{
	//   "token":{"type":"color","value":[r,g,b]} | {"type":"number","value":n}
	// }}}. The complete document is validated before `out` is replaced.
	bool ImportUiDesignTokensJson(
		std::string_view source,
		engine::gui::UiDocument &out,
		engine::gui::DocumentReport &report,
		const UiDesignImportLimits &limits = {}
	);

	// Imports RFC 4180-style `locale,key,text` records. The header is required,
	// quoted fields may contain commas and newlines, and duplicate locale/key
	// rows are refused. `out` changes only after every row has been accepted.
	bool ImportUiLocalizationCsv(
		std::string_view source,
		engine::gui::LocalizationCatalogue &out,
		engine::gui::DocumentReport &report,
		const UiDesignImportLimits &limits = {}
	);
}
