#include <engine/core/Log.hpp>
#include <engine/input/Translate.hpp>

#include <SDL3/SDL_gamepad.h>

#include <algorithm>
#include <cmath>
#include <iterator>

namespace engine::input {

	using scene::ControllerAxis;
	using scene::ControllerButton;
	using scene::KeyCode;
	using scene::MouseButton;

	KeyCode KeyOf(SDL_Keycode code) {
		// **A switch and not a table indexed by keycode.** SDL's keycodes are not
		// dense - the letters are their ASCII values and the function keys are up
		// past 0x40000000 - so an array would be four gigabytes or a hash. A
		// switch compiles to a jump table over the dense run and compares over the
		// rest, which is what a compiler is for.
		switch (code) {
		case SDLK_A:
			return KeyCode::A;
		case SDLK_B:
			return KeyCode::B;
		case SDLK_C:
			return KeyCode::C;
		case SDLK_D:
			return KeyCode::D;
		case SDLK_E:
			return KeyCode::E;
		case SDLK_F:
			return KeyCode::F;
		case SDLK_G:
			return KeyCode::G;
		case SDLK_H:
			return KeyCode::H;
		case SDLK_I:
			return KeyCode::I;
		case SDLK_J:
			return KeyCode::J;
		case SDLK_K:
			return KeyCode::K;
		case SDLK_L:
			return KeyCode::L;
		case SDLK_M:
			return KeyCode::M;
		case SDLK_N:
			return KeyCode::N;
		case SDLK_O:
			return KeyCode::O;
		case SDLK_P:
			return KeyCode::P;
		case SDLK_Q:
			return KeyCode::Q;
		case SDLK_R:
			return KeyCode::R;
		case SDLK_S:
			return KeyCode::S;
		case SDLK_T:
			return KeyCode::T;
		case SDLK_U:
			return KeyCode::U;
		case SDLK_V:
			return KeyCode::V;
		case SDLK_W:
			return KeyCode::W;
		case SDLK_X:
			return KeyCode::X;
		case SDLK_Y:
			return KeyCode::Y;
		case SDLK_Z:
			return KeyCode::Z;

		case SDLK_0:
			return KeyCode::Zero;
		case SDLK_1:
			return KeyCode::One;
		case SDLK_2:
			return KeyCode::Two;
		case SDLK_3:
			return KeyCode::Three;
		case SDLK_4:
			return KeyCode::Four;
		case SDLK_5:
			return KeyCode::Five;
		case SDLK_6:
			return KeyCode::Six;
		case SDLK_7:
			return KeyCode::Seven;
		case SDLK_8:
			return KeyCode::Eight;
		case SDLK_9:
			return KeyCode::Nine;

		case SDLK_SPACE:
			return KeyCode::Space;
		case SDLK_RETURN:
			return KeyCode::Return;
		case SDLK_ESCAPE:
			return KeyCode::Escape;
		case SDLK_BACKSPACE:
			return KeyCode::Backspace;
		case SDLK_TAB:
			return KeyCode::Tab;
		case SDLK_LSHIFT:
			return KeyCode::LeftShift;
		case SDLK_RSHIFT:
			return KeyCode::RightShift;
		case SDLK_LCTRL:
			return KeyCode::LeftControl;
		case SDLK_RCTRL:
			return KeyCode::RightControl;
		case SDLK_LALT:
			return KeyCode::LeftAlt;
		case SDLK_RALT:
			return KeyCode::RightAlt;

		case SDLK_UP:
			return KeyCode::Up;
		case SDLK_DOWN:
			return KeyCode::Down;
		case SDLK_LEFT:
			return KeyCode::Left;
		case SDLK_RIGHT:
			return KeyCode::Right;

		case SDLK_F1:
			return KeyCode::F1;
		case SDLK_F2:
			return KeyCode::F2;
		case SDLK_F3:
			return KeyCode::F3;
		case SDLK_F4:
			return KeyCode::F4;
		case SDLK_F5:
			return KeyCode::F5;
		case SDLK_F6:
			return KeyCode::F6;
		case SDLK_F7:
			return KeyCode::F7;
		case SDLK_F8:
			return KeyCode::F8;
		case SDLK_F9:
			return KeyCode::F9;
		case SDLK_F10:
			return KeyCode::F10;
		case SDLK_F11:
			return KeyCode::F11;
		case SDLK_F12:
			return KeyCode::F12;

		default:
			break;
		}
		return KeyCode::Unknown;
	}

	namespace {
		constexpr float AXIS_DEADZONE = 0.12f;

		float AxisValue(int16_t value, bool trigger = false) {
			const float normalized = trigger ? std::max(0.0f, static_cast<float>(value) / 32767.0f)
											 : std::max(-1.0f, static_cast<float>(value) / 32767.0f);
			if (trigger) {
				return normalized < AXIS_DEADZONE ? 0.0f : normalized;
			}
			const float magnitude = std::abs(normalized);
			return magnitude < AXIS_DEADZONE
					   ? 0.0f
					   : std::copysign((magnitude - AXIS_DEADZONE) / (1.0f - AXIS_DEADZONE), normalized);
		}

		ControllerButton GamepadButtonOf(uint8_t button) {
			switch (button) {
			case SDL_GAMEPAD_BUTTON_SOUTH:
				return ControllerButton::A;
			case SDL_GAMEPAD_BUTTON_EAST:
				return ControllerButton::B;
			case SDL_GAMEPAD_BUTTON_WEST:
				return ControllerButton::X;
			case SDL_GAMEPAD_BUTTON_NORTH:
				return ControllerButton::Y;
			case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
				return ControllerButton::LeftShoulder;
			case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
				return ControllerButton::RightShoulder;
			case SDL_GAMEPAD_BUTTON_LEFT_STICK:
				return ControllerButton::LeftStick;
			case SDL_GAMEPAD_BUTTON_RIGHT_STICK:
				return ControllerButton::RightStick;
			case SDL_GAMEPAD_BUTTON_START:
				return ControllerButton::Start;
			case SDL_GAMEPAD_BUTTON_BACK:
				return ControllerButton::Select;
			case SDL_GAMEPAD_BUTTON_DPAD_UP:
				return ControllerButton::DPadUp;
			case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
				return ControllerButton::DPadDown;
			case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
				return ControllerButton::DPadLeft;
			case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
				return ControllerButton::DPadRight;
			default:
				return ControllerButton::Count;
			}
		}

		ControllerAxis GamepadAxisOf(uint8_t axis) {
			switch (axis) {
			case SDL_GAMEPAD_AXIS_LEFTX:
				return ControllerAxis::LeftX;
			case SDL_GAMEPAD_AXIS_LEFTY:
				return ControllerAxis::LeftY;
			case SDL_GAMEPAD_AXIS_RIGHTX:
				return ControllerAxis::RightX;
			case SDL_GAMEPAD_AXIS_RIGHTY:
				return ControllerAxis::RightY;
			case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:
				return ControllerAxis::LeftTrigger;
			case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER:
				return ControllerAxis::RightTrigger;
			default:
				return ControllerAxis::Count;
			}
		}

		// The mouse button an SDL index names, or `Count` for one this engine has
		// no name for. Side buttons exist and are not exposed, because a name that
		// mapped to nothing would be offering completion for a button no test has
		// ever pressed.
		MouseButton ButtonOf(uint8_t index) {
			switch (index) {
			case SDL_BUTTON_LEFT:
				return MouseButton::Left;
			case SDL_BUTTON_RIGHT:
				return MouseButton::Right;
			case SDL_BUTTON_MIDDLE:
				return MouseButton::Middle;
			default:
				break;
			}
			return MouseButton::Count;
		}
	}

	void Translator::BeginFrame() {
		// **The bits are rolled and the deltas are cleared, and those are two
		// different operations.** A key that is down stays down across frames and
		// its *edge* is the difference from last frame; a mouse delta is only ever
		// this frame's, so carrying it would make a camera keep turning after the
		// mouse stopped.
		Current.Previous = Current.Down;
		Current.PreviousButtons = Current.Buttons;

		// **Focus is rolled with the other two rather than derived where it is
		// read**, because `UserInputService.WindowFocused` is an edge and an edge
		// is the difference between two frames. Nothing else in this module needs
		// it; `scene::InputState::WasFocusGained` is what does.
		Current.PreviousFocused = Current.Focused;

		// The same rule once more, for the edge a place watches to swap "press E"
		// for "click here". **Rolled here and not on the event that changes it**,
		// so several events in one frame produce at most one change: a player who
		// moved the mouse and then typed has changed device once as far as a
		// script is concerned.
		Current.PreviousLastSource = Current.LastSource;

		Current.MouseDelta = {};
		Current.WheelDelta = 0.0f;

		for (scene::ControllerSlot &slot : ControllerCurrent.Slots) {
			slot.PreviousButtons = slot.Buttons;
			slot.PreviousConnected = slot.Connected;
			slot.PressedButtons = 0;
			std::copy(std::begin(slot.Axes), std::end(slot.Axes), std::begin(slot.PreviousAxes));
		}

		// A delta like the two above it: a character was typed once, and there
		// is no previous value for it to be an edge against.
		Typed.clear();
	}

	bool Translator::HandleEvent(const SDL_Event &event) {
		auto findSlot = [this](uint32_t id) -> size_t {
			for (size_t index = 0; index < scene::MAX_CONTROLLERS; index++) {
				if (ControllerIds[index] == id) {
					return index;
				}
			}
			return scene::MAX_CONTROLLERS;
		};
		auto connect = [this, &findSlot](uint32_t id, bool mapped) -> size_t {
			size_t slot = findSlot(id);
			if (slot == scene::MAX_CONTROLLERS) {
				for (size_t index = 0; index < scene::MAX_CONTROLLERS; index++) {
					if (ControllerIds[index] == 0) {
						slot = index;
						ControllerIds[index] = id;
						break;
					}
				}
			}
			if (slot != scene::MAX_CONTROLLERS) {
				ControllerCurrent.Slots[slot].Connected = true;
				ControllerCurrent.Slots[slot].Mapped = mapped || ControllerCurrent.Slots[slot].Mapped;
			}
			return slot;
		};

		switch (event.type) {
		case SDL_EVENT_GAMEPAD_ADDED:
			return connect(event.gdevice.which, true) != scene::MAX_CONTROLLERS;
		case SDL_EVENT_JOYSTICK_ADDED:
			return connect(event.jdevice.which, false) != scene::MAX_CONTROLLERS;
		case SDL_EVENT_GAMEPAD_REMOVED:
		case SDL_EVENT_JOYSTICK_REMOVED: {
			const uint32_t id =
				event.type == SDL_EVENT_GAMEPAD_REMOVED ? event.gdevice.which : event.jdevice.which;
			const size_t slot = findSlot(id);
			if (slot == scene::MAX_CONTROLLERS) return false;
			ControllerCurrent.Slots[slot].Buttons = 0;
			std::fill(
				std::begin(ControllerCurrent.Slots[slot].Axes),
				std::end(ControllerCurrent.Slots[slot].Axes),
				0.0f
			);
			ControllerCurrent.Slots[slot].Connected = false;
			ControllerCurrent.Slots[slot].Mapped = false;
			ControllerIds[slot] = 0;
			return true;
		}
		case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
		case SDL_EVENT_GAMEPAD_BUTTON_UP: {
			const size_t slot = findSlot(event.gbutton.which);
			const ControllerButton button = GamepadButtonOf(event.gbutton.button);
			if (slot == scene::MAX_CONTROLLERS || button == ControllerButton::Count) return false;
			const uint32_t bit = 1u << static_cast<uint8_t>(button);
			if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
				if ((ControllerCurrent.Slots[slot].Buttons & bit) == 0) {
					ControllerCurrent.Slots[slot].PressedButtons |= bit;
				}
				ControllerCurrent.Slots[slot].Buttons |= bit;
			} else {
				ControllerCurrent.Slots[slot].Buttons &= ~bit;
			}
			Current.LastSource =
				static_cast<scene::InputSource>(static_cast<uint8_t>(scene::InputSource::Gamepad1) + slot);
			return true;
		}
		case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
			const size_t slot = findSlot(event.gaxis.which);
			const ControllerAxis axis = GamepadAxisOf(event.gaxis.axis);
			if (slot == scene::MAX_CONTROLLERS || axis == ControllerAxis::Count) return false;
			const bool trigger = axis == ControllerAxis::LeftTrigger || axis == ControllerAxis::RightTrigger;
			const size_t axisIndex = static_cast<size_t>(axis);
			const float normalized = AxisValue(event.gaxis.value, trigger);
			if (ControllerCurrent.Slots[slot].Axes[axisIndex] == normalized) return false;
			ControllerCurrent.Slots[slot].Axes[axisIndex] = normalized;
			if (trigger) {
				const ControllerButton button = axis == ControllerAxis::LeftTrigger
													? ControllerButton::LeftTrigger
													: ControllerButton::RightTrigger;
				const uint32_t bit = 1u << static_cast<uint8_t>(button);
				if (ControllerCurrent.Slots[slot].Axes[axisIndex] >= 0.5f) {
					if ((ControllerCurrent.Slots[slot].Buttons & bit) == 0) {
						ControllerCurrent.Slots[slot].PressedButtons |= bit;
					}
					ControllerCurrent.Slots[slot].Buttons |= bit;
				} else {
					ControllerCurrent.Slots[slot].Buttons &= ~bit;
				}
			}
			Current.LastSource =
				static_cast<scene::InputSource>(static_cast<uint8_t>(scene::InputSource::Gamepad1) + slot);
			return true;
		}
		case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
		case SDL_EVENT_JOYSTICK_BUTTON_UP: {
			const size_t slot = findSlot(event.jbutton.which);
			if (slot == scene::MAX_CONTROLLERS || ControllerCurrent.Slots[slot].Mapped ||
				event.jbutton.button >= static_cast<uint8_t>(ControllerButton::Count))
				return false;
			const uint32_t bit = 1u << event.jbutton.button;
			if (event.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN) {
				if ((ControllerCurrent.Slots[slot].Buttons & bit) == 0) {
					ControllerCurrent.Slots[slot].PressedButtons |= bit;
				}
				ControllerCurrent.Slots[slot].Buttons |= bit;
			} else {
				ControllerCurrent.Slots[slot].Buttons &= ~bit;
			}
			Current.LastSource =
				static_cast<scene::InputSource>(static_cast<uint8_t>(scene::InputSource::Gamepad1) + slot);
			return true;
		}
		case SDL_EVENT_JOYSTICK_AXIS_MOTION: {
			const size_t slot = findSlot(event.jaxis.which);
			if (slot == scene::MAX_CONTROLLERS || ControllerCurrent.Slots[slot].Mapped ||
				event.jaxis.axis >= static_cast<uint8_t>(ControllerAxis::Count))
				return false;
			const float normalized = AxisValue(event.jaxis.value);
			if (ControllerCurrent.Slots[slot].Axes[event.jaxis.axis] == normalized) return false;
			ControllerCurrent.Slots[slot].Axes[event.jaxis.axis] = normalized;
			Current.LastSource =
				static_cast<scene::InputSource>(static_cast<uint8_t>(scene::InputSource::Gamepad1) + slot);
			return true;
		}
		case SDL_EVENT_JOYSTICK_HAT_MOTION: {
			const size_t slot = findSlot(event.jhat.which);
			if (slot == scene::MAX_CONTROLLERS || ControllerCurrent.Slots[slot].Mapped || event.jhat.hat != 0)
				return false;
			constexpr uint32_t HAT_BITS = (1u << static_cast<uint8_t>(ControllerButton::DPadUp)) |
										  (1u << static_cast<uint8_t>(ControllerButton::DPadDown)) |
										  (1u << static_cast<uint8_t>(ControllerButton::DPadLeft)) |
										  (1u << static_cast<uint8_t>(ControllerButton::DPadRight));
			auto &controller = ControllerCurrent.Slots[slot];
			const uint32_t before = controller.Buttons;
			auto &buttons = controller.Buttons;
			buttons &= ~HAT_BITS;
			if ((event.jhat.value & SDL_HAT_UP) != 0)
				buttons |= 1u << static_cast<uint8_t>(ControllerButton::DPadUp);
			if ((event.jhat.value & SDL_HAT_DOWN) != 0)
				buttons |= 1u << static_cast<uint8_t>(ControllerButton::DPadDown);
			if ((event.jhat.value & SDL_HAT_LEFT) != 0)
				buttons |= 1u << static_cast<uint8_t>(ControllerButton::DPadLeft);
			if ((event.jhat.value & SDL_HAT_RIGHT) != 0)
				buttons |= 1u << static_cast<uint8_t>(ControllerButton::DPadRight);
			controller.PressedButtons |= buttons & ~before;
			Current.LastSource =
				static_cast<scene::InputSource>(static_cast<uint8_t>(scene::InputSource::Gamepad1) + slot);
			return true;
		}
		case SDL_EVENT_KEY_DOWN:
		case SDL_EVENT_KEY_UP: {
			// **A repeat is not an edge.** SDL sends key-down again while a key is
			// held, and setting the bit again is harmless - but it is worth
			// skipping rather than relying on that, because a future edge derived
			// from anything other than the two bitsets would fire once per repeat.
			if (event.key.repeat != 0) {
				return false;
			}

			const KeyCode key = KeyOf(event.key.key);
			if (key == KeyCode::Unknown) {
				// **`KeyCode` only names keys this engine can produce**, which is
				// `scene/Input.hpp`'s rule, so dropping the event is right. What
				// was missing is any way to tell: a script that cannot see a key
				// the player can see on their keyboard is a bug report with no
				// evidence in it, and the SDL keycode here is the thing somebody
				// would add to the switch above.
				ENGINE_DEBUG("SDL keycode {} has no scene::KeyCode; the event is dropped", event.key.key);
				return false;
			}
			Current.Down.Set(key, event.type == SDL_EVENT_KEY_DOWN);

			// **Set on the release as well as the press**, because letting go of
			// a key is the keyboard speaking too - a place that swapped its
			// prompts on the press and swapped them back on the release would
			// flicker every time somebody walked.
			Current.LastSource = scene::InputSource::Keyboard;
			return true;
		}

		case SDL_EVENT_TEXT_INPUT:
			// **Appended whole, because one byte is not one character.** SDL
			// hands over the composed UTF-8 a keystroke produced - one byte for
			// `a`, two for `é`, four for an emoji, and more than one character
			// at once when an input method commits a word - so anything here
			// that took a byte at a time, or assumed one event was one letter,
			// would cut a codepoint in half the first time somebody typed in
			// their own language.
			//
			// **Accumulated rather than assigned**, for `MouseDelta`'s reason:
			// several of these arrive in a frame and the text is all of them.
			//
			// **A key event with this in the same frame is not a duplicate.**
			// `SDL_EVENT_KEY_DOWN` says which key moved and this says what it
			// spelled; a game reads the first for movement and a text box reads
			// the second, and the layout is the whole of the difference between
			// them.
			//
			// **SDL sends none of these until a host calls `SDL_StartTextInput`
			// on the window**, which is the platform's rule rather than this
			// module's: text input is what raises an on-screen keyboard and
			// starts composition, so it is off until something says it is
			// wanted. `client::Client` asks while a `TextBox` has the keyboard,
			// so this case runs for exactly as long as somebody is typing.
			Typed += event.text.text;
			Current.LastSource = scene::InputSource::Keyboard;
			return true;

		case SDL_EVENT_MOUSE_BUTTON_DOWN:
		case SDL_EVENT_MOUSE_BUTTON_UP: {
			const MouseButton button = ButtonOf(event.button.button);
			if (button == MouseButton::Count) {
				// Three buttons are named and a mouse may have eight.
				ENGINE_DEBUG("SDL mouse button {} is not one of the three named", event.button.button);
				return false;
			}
			const uint8_t bit = 1u << static_cast<uint8_t>(button);
			if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
				Current.Buttons |= bit;
			} else {
				Current.Buttons &= static_cast<uint8_t>(~bit);
			}

			// **The button and not `MouseButton1` for all three**, because
			// `InputSource` shares its first three ordinals with `MouseButton` by
			// construction - see the `static_assert`s in `scene/Input.cpp` - so a
			// right-click reports as `MouseButton2` and a script asking which
			// device is live gets the one it saw in `InputBegan`.
			Current.LastSource = static_cast<scene::InputSource>(button);
			return true;
		}

		case SDL_EVENT_MOUSE_MOTION:
			Current.MousePosition = core::Vector2{event.motion.x, event.motion.y};
			Current.LastSource = scene::InputSource::MouseMovement;

			// **Accumulated rather than assigned**, because several motion events
			// arrive per frame and a camera wants all of the movement. Assigning
			// would make the turn depend on how the compositor happened to batch.
			Current.MouseDelta = core::Vector2{
				Current.MouseDelta.X + event.motion.xrel, Current.MouseDelta.Y + event.motion.yrel
			};
			return true;

		case SDL_EVENT_MOUSE_WHEEL:
			Current.WheelDelta += event.wheel.y;
			Current.LastSource = scene::InputSource::MouseWheel;
			return true;

		case SDL_EVENT_WINDOW_FOCUS_GAINED:
			Current.Focused = true;
			return true;

		case SDL_EVENT_WINDOW_FOCUS_LOST:
			// **Everything released, because SDL sends no key-up for a key let go
			// in another window.** Without this a character walks forever after an
			// alt-tab, which is the bug every engine ships once.
			Current.Focused = false;

			// The frame this happens on reports every held key as released, and
			// a character that keeps walking after an alt-tab is what a missing
			// one looks like. Said at `debug` because it is the first thing to
			// check when that is the report.
			ENGINE_DEBUG("window focus lost; releasing everything held");
			ReleaseAll();
			return true;

		default:
			break;
		}
		return false;
	}

	void Translator::ReleaseAll() {
		// **`Previous` is left alone**, so the frame this happens on reports every
		// held key as *released* rather than as never having been down. A listener
		// waiting for a key-up gets one, which is what makes the state consistent
		// rather than merely empty.
		Current.Down = scene::KeyBits{};
		Current.Buttons = 0;
		Current.MouseDelta = {};
		Current.WheelDelta = 0.0f;
		for (scene::ControllerSlot &slot : ControllerCurrent.Slots) {
			slot.Buttons = 0;
			std::fill(std::begin(slot.Axes), std::end(slot.Axes), 0.0f);
		}
		Typed.clear();
	}
}
