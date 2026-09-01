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
		// that a checked claim rather than a hopeful one - inserting a key in the
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

			"ButtonA",	  "ButtonB",	 "ButtonX",
			"ButtonY",	  "ButtonL1",	 "ButtonR1",
			"ButtonL2",	  "ButtonR2",	 "ButtonL3",
			"ButtonR3",	  "ButtonStart", "ButtonSelect",
			"DPadUp",	  "DPadDown",	 "DPadLeft",
			"DPadRight",  "Thumbstick1", "Thumbstick2",
		}};

		// The pin. A name list one short or one long is a compile error rather
		// than a scene where every key is off by one.
		static_assert(
			KEY_NAMES.size() == static_cast<size_t>(KeyCode::Count),
			"every KeyCode needs exactly one name, in ordinal order"
		);

		// Every `Enum.UserInputType` member, in ordinal order.
		//
		// **One table for the buttons and the three that are not buttons**, which
		// is what keeps `Describe(MouseButton)` and `Describe(InputSource)` from
		// being two spellings of one list. The `static_assert`s below are what
		// pin the overlap that `IsMouseButtonPressed` relies on.
		constexpr std::array<std::string_view, static_cast<size_t>(InputSource::Count)> SOURCE_NAMES{{
			"MouseButton1",
			"MouseButton2",
			"MouseButton3",
			"Keyboard",
			"MouseMovement",
			"MouseWheel",
			"Gamepad1",
			"Gamepad2",
			"Gamepad3",
			"Gamepad4",
			"Gamepad5",
			"Gamepad6",
			"Gamepad7",
			"Gamepad8",
		}};

		static_assert(
			SOURCE_NAMES.size() == static_cast<size_t>(InputSource::Count),
			"every InputSource needs exactly one name, in ordinal order"
		);

		// **The buttons come first and keep their numbers.**
		// `UserInputService:IsMouseButtonPressed` resolves an
		// `Enum.UserInputType` member to an ordinal and casts it to a
		// `MouseButton`, so a member inserted ahead of these would silently make
		// "is the left button down" answer about the right one.
		static_assert(
			static_cast<size_t>(InputSource::MouseButton1) == static_cast<size_t>(MouseButton::Left)
		);
		static_assert(
			static_cast<size_t>(InputSource::MouseButton2) == static_cast<size_t>(MouseButton::Right)
		);
		static_assert(
			static_cast<size_t>(InputSource::MouseButton3) == static_cast<size_t>(MouseButton::Middle)
		);
		static_assert(static_cast<size_t>(MouseButton::Count) <= static_cast<size_t>(InputSource::Count));

		// **The layout is pinned, because `Column::Write` sends `sizeof(T)` bytes
		// and does not know which of them a member claimed.** Every field added
		// since v0.10 has come out of `Reserved` for that reason, and this is what
		// turns "came out of `Reserved`" from a habit into a checked claim: a
		// member appended to the end instead grows the object, moves the save
		// format, and does it silently.
		//
		// The number is what the members happen to add up to rather than a target
		// - the point is that changing it is a decision somebody has to make here.
		constexpr size_t SIZE_IS_PINNED = 56;
		static_assert(
			sizeof(InputState) == SIZE_IS_PINNED,
			"InputState changed size: take a new field out of Reserved, or move the save format on purpose"
		);
	}

	const char *Describe(KeyCode key) {
		const auto index = static_cast<size_t>(key);
		return index < KEY_NAMES.size() ? KEY_NAMES[index].data() : "Unknown";
	}

	KeyCode KeyFromName(std::string_view name) {
		// **A linear scan and not a map.** Ninety-odd string compares sounds like
		// a lot and is not: this is called when a script *names* a key - binding
		// an action, testing one by name - which is an authoring-time frequency,
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

	KeyCode KeyOf(ControllerButton button) {
		constexpr KeyCode KEYS[]{
			KeyCode::ButtonA,
			KeyCode::ButtonB,
			KeyCode::ButtonX,
			KeyCode::ButtonY,
			KeyCode::ButtonL1,
			KeyCode::ButtonR1,
			KeyCode::ButtonL2,
			KeyCode::ButtonR2,
			KeyCode::ButtonL3,
			KeyCode::ButtonR3,
			KeyCode::ButtonStart,
			KeyCode::ButtonSelect,
			KeyCode::DPadUp,
			KeyCode::DPadDown,
			KeyCode::DPadLeft,
			KeyCode::DPadRight,
		};
		const size_t index = static_cast<size_t>(button);
		return index < std::size(KEYS) ? KEYS[index] : KeyCode::Unknown;
	}

	const char *Describe(MouseButton button) {
		// The buttons are the first three sources, so this is that lookup with a
		// narrower argument rather than a second table.
		const auto index = static_cast<size_t>(button);
		return index < static_cast<size_t>(MouseButton::Count) ? Describe(static_cast<InputSource>(button))
															   : SOURCE_NAMES[0].data();
	}

	const char *Describe(InputSource source) {
		const auto index = static_cast<size_t>(source);
		return index < SOURCE_NAMES.size() ? SOURCE_NAMES[index].data() : SOURCE_NAMES[0].data();
	}
}
