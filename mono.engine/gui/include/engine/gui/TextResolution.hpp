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

	// Viewer-specific inputs used to resolve and measure authored label text.
	struct TextResolutionRequest {
		// Viewer-local message catalogue, if localization is enabled.
		const LocalizationCatalogue *Catalogue = nullptr;
		// Locale used to resolve localized labels.
		std::string_view Locale;
		// Whether Studio marks unresolved localized text.
		bool StudioMissingLocalizationMarker = false;

		// Multiplied into text metrics by layout. This belongs beside locale
		// because both are viewer-local inputs resolved before placement; authored
		// label sizes remain unchanged.
		float LayoutScale = 1.0f;
		// Font catalogue used to select viewer-local text faces.
		const FontPackage *Fonts = nullptr;
	};

	// Resolves a label using viewer-local localization and font settings.
	std::string ResolveText(
		const ecs::Store &store,
		ecs::Entity instance,
		const Label &label,
		const TextResolutionRequest &request
	);
}
