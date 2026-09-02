#pragma once

// Pure data shaping for Studio's visual MCP tools.
//
// The MCP handlers need SDL and a live Editor. Naming capture files and
// deciding which visible scene views belong in one request do not, so they stay
// here where the Studio suite can exercise collision and refusal cases.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace studio::automation {

	// Stable MCP spellings, kept separate from SDL's platform-facing modifier
	// flags so validation is testable without a window or input backend.
	enum KeyboardModifier : uint8_t {
		KeyboardModifierNone = 0,
		KeyboardModifierShift = 1 << 0,
		KeyboardModifierControl = 1 << 1,
		KeyboardModifierAlt = 1 << 2,
		KeyboardModifierGui = 1 << 3,
	};

	enum class ScreenshotTarget {
		Scene,
		Studio,
		All,
	};

	struct VisibleScene {
		std::string Name;
		size_t Slot = 0;
	};

	struct ScreenshotTask {
		std::filesystem::path Path;
		std::string Scene;
		size_t Slot = 0;
		bool Studio = false;
	};

	// Parses the public MCP spelling of a screenshot target.
	//
	// @return `false` for anything other than `scene`, `studio`, or `all`.
	bool ParseScreenshotTarget(std::string_view spelling, ScreenshotTarget &target);

	// Parses the public MCP modifier list into `KeyboardModifier` bits.
	// Repeated names are harmless. Unknown names refuse the whole request.
	bool ParseKeyboardModifiers(std::span<const std::string> names, uint8_t &modifiers, std::string &failure);

	// Bounds text kept alive while an SDL text event crosses the frame loop.
	bool ValidateTextInput(std::string_view text, std::string &failure);

	// Builds one bounded capture batch from the scene views currently visible.
	//
	// Repeated views of one scene produce one file. `scene` narrows that set
	// when supplied, and a narrowed scene that is not visible is refused rather
	// than silently photographing another panel.
	//
	// @param directory Where every BMP is written.
	// @param target Which class of image to include.
	// @param visible Scene views currently open and renderable.
	// @param scene Optional exact scene name.
	// @param failure Filled when the request cannot be honoured.
	// @return The ordered capture tasks, scene images first and Studio last.
	std::vector<ScreenshotTask> PlanScreenshots(
		const std::filesystem::path &directory,
		ScreenshotTarget target,
		std::span<const VisibleScene> visible,
		std::string_view scene,
		std::string &failure
	);
}
