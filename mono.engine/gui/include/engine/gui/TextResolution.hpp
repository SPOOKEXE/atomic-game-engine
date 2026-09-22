#pragma once

// Viewer-local text selection shared by layout and compile.
//
// Authored Label text never changes. A caller supplies viewer policy for one
// layout and compile operation, and both consumers receive the same resolved
// UTF-8 string.
//
// @tier L7 · shared

#include <string>
#include <string_view>

namespace engine::ecs {
	class Store;
	struct Entity;
}

namespace engine::gui {
	class LocalizationCatalogue;
	class FontPackage;
	struct Label;

	struct TextResolutionRequest {
		const LocalizationCatalogue *Catalogue = nullptr;
		std::string_view Locale;
		bool StudioMissingLocalizationMarker = false;

		// Multiplied into text metrics by layout. This belongs beside locale
		// because both are viewer-local inputs resolved before placement; authored
		// label sizes remain unchanged.
		float LayoutScale = 1.0f;
		const FontPackage *Fonts = nullptr;
	};

	std::string ResolveText(
		const ecs::Store &store,
		ecs::Entity instance,
		const Label &label,
		const TextResolutionRequest &request
	);
}
