#include <engine/scene/Input.hpp>

#include <array>
#include <string_view>

namespace engine::scene {

	namespace {
		// The names, in ordinal order, generated from one list.
		//
		// **One table for both directions**, which is the whole reason this is a
		// table rather than a switch: `Describe` and `KeyFromName` are inverses,
		// and two switches would be two places to add a key to and one place to
		// forget. `scene/Part.cpp` makes the same argument for generating the
		// `NormalId` member list from `Describe` rather than typing it out.
		//
		// The order **is** the enum's, and a `static_assert` below is what makes
		// that a checked claim rather than a hopeful one — inserting a key in the
		// middle of `KeyCode` without inserting a name here would silently shift
		// every name after it by one, so `Enum.KeyCode.W` would resolve to V.
		constexpr std::array<std::string_view, static_cast<size_t>(KeyCode::Count)> KEY_NAMES{{
			"Unknown",

			"A",		  "B",			 "C",
			"D",		  "E",			 "F",
			"G",		  "H",			 "I",
			"J",		  "K",			 "L",
			"M",		  "N",			 "O",
			"P",		  "Q",			 "R",
			"S",		  "T",			 "U",
			"V",		  "W",			 "X",
			"Y",		  "Z",

			"Zero",		  "One",		 "Two",
			"Three",	  "Four",		 "Five",
			"Six",		  "Seven",		 "Eight",
			"Nine",

			"Space",	  "Return",		 "Escape",
			"Backspace",  "Tab",		 "LeftShift",
			"RightShift", "LeftControl", "RightControl",
			"LeftAlt",	  "RightAlt",

			"Up",		  "Down",		 "Left",
			"Right",

			"F1",		  "F2",			 "F3",
			"F4",		  "F5",			 "F6",
			"F7",		  "F8",			 "F9",
			"F10",		  "F11",		 "F12",
		}};

		// The pin. A name list one short or one long is a compile error rather
		// than a scene where every key is off by one.
		static_assert(
			KEY_NAMES.size() == static_cast<size_t>(KeyCode::Count),
			"every KeyCode needs exactly one name, in ordinal order"
		);

		constexpr std::array<std::string_view, static_cast<size_t>(MouseButton::Count)> BUTTON_NAMES{{
			"MouseButton1",
			"MouseButton2",
			"MouseButton3",
		}};
	}

	const char *Describe(KeyCode key) {
		const auto index = static_cast<size_t>(key);
		return index < KEY_NAMES.size() ? KEY_NAMES[index].data() : "Unknown";
	}

	KeyCode KeyFromName(std::string_view name) {
		// **A linear scan and not a map.** Ninety-odd string compares sounds like
		// a lot and is not: this is called when a script *names* a key — binding
		// an action, testing one by name — which is an authoring-time frequency,
		// and the hot path is `IsKeyDown(KeyCode)` which takes the enum and never
		// comes here. A static map would be an allocation and a hash to save a
		// scan nothing runs in a loop.
		for (size_t index = 0; index < KEY_NAMES.size(); index++) {
			if (KEY_NAMES[index] == name) {
				return static_cast<KeyCode>(index);
			}
		}
		return KeyCode::Unknown;
	}

	const char *Describe(MouseButton button) {
		const auto index = static_cast<size_t>(button);
		return index < BUTTON_NAMES.size() ? BUTTON_NAMES[index].data() : "MouseButton1";
	}
}
