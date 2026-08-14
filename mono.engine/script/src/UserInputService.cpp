// `UserInputService`'s surface, in neither language.
//
// **Split out of `InputServices.cpp` at v0.18, which held three jobs.** That
// file was this service, `ContextActionService` and the Luau input pump in one
// translation unit — so a surface that names no VM was compiled against
// `<lua.h>`, and the pump that fires its signals sat under a heading rather than
// behind a file name. `LuauInput.cpp` is the pump and `JsInput.cpp` is its twin;
// what is left here is the seven methods, the ten properties and the six signal
// rows, every one of which both languages install from this one description.
//
// **The tag is the single Luau fact still in the file, and it is a number.**
// `ServiceProperty` makes the service a userdata in Luau to defeat `safeenv`'s
// `GETIMPORT` caching — `LuauTags.hpp` carries that argument — so this names
// `TAG_INPUT_SERVICE` and includes nothing else of that VM's.
//
// @tier L9 · shared
// @since v0.16

#include "Actions.hpp"
#include "LuauTags.hpp"
#include "ScriptCall.hpp"
#include "ServiceSurface.hpp"

#include <engine/ecs/EnumTable.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Controls.hpp>
#include <engine/scene/Input.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <vector>

namespace engine::script {

	namespace {
		using scene::InputState;
		using scene::KeyCode;
		using scene::MouseBehavior;
		using scene::MouseButton;

		// Where `UserInputService`'s method table lives, since the service is a
		// userdata and a userdata has no fields.
		//
		// **One constant on the surface, read by the generic `__index`.**
		// `UserInputServiceSurface` sets it and `LuauServiceIndex` reads it back
		// off the same surface, so the install and the lookup cannot name
		// different keys — which they could while each service wrote its own
		// metamethod.
		constexpr const char *INPUT_METHODS_KEY = "engine.userinput.methods";

		// **Written once since v0.16, which is what a property list bought.**
		// Every method below is a `ScriptMethod` and every property a
		// `ServiceProperty`, so both VMs install this service from the one
		// description in `UserInputServiceSurface` — where Luau built it from six
		// `lua_CFunction`s and a chain of `if (field == ...)` and JavaScript could
		// not build it at all. See `ServiceProperty`.

		// This call's input state, or null on a world nobody writes input to.
		//
		// The neutral twin of `StateOf` above, which the pump still needs because
		// it runs with no call in flight.
		const InputState *InputOf(ScriptCall &call) {
			return call.World().Resource<InputState>();
		}

		// `UserInputService:IsKeyDown(Enum.KeyCode.Space)`
		//
		// **Takes an `EnumItem` or a string**, which is the same latitude a
		// property with `PropertyType::Enum` gives: `part.AlphaMode = "Clip"` is
		// what a migrating script already contains, and refusing it here would
		// make input the one surface that is stricter than the rest.
		void IsKeyDown(ScriptCall &call) {
			core::Name member;
			if (!call.ReadEnum(0, core::Name("KeyCode"), member)) {
				call.Raise("IsKeyDown expects an Enum.KeyCode");
			}

			const InputState *input = InputOf(call);
			call.ReturnBoolean(input != nullptr && input->IsKeyDown(scene::KeyFromName(member.Text())));
		}

		void IsMouseButtonPressed(ScriptCall &call) {
			core::Name member;
			if (!call.ReadEnum(0, core::Name("UserInputType"), member)) {
				call.Raise("IsMouseButtonPressed expects an Enum.UserInputType");
			}

			// **`Enum.UserInputType` names three sources that are not buttons**
			// since `InputObject` needed to say where an event came from, and
			// "is `MouseMovement` pressed" is a question with no answer. False
			// rather than a cast past the end of the button bits, which would
			// have read whichever bit the arithmetic landed on.
			size_t ordinal = 0;
			const bool known = ecs::EnumTable::OrdinalOf(core::Name("UserInputType"), member, ordinal) &&
							   ordinal < static_cast<size_t>(MouseButton::Count);

			const InputState *input = InputOf(call);
			call.ReturnBoolean(
				input != nullptr && known && input->IsButtonDown(static_cast<MouseButton>(ordinal))
			);
		}

		// `UserInputService:GetMouseLocation()` — a `Vector2` in pixels.
		void GetMouseLocation(ScriptCall &call) {
			const InputState *input = InputOf(call);
			call.ReturnVector2(input == nullptr ? core::Vector2{} : input->MousePosition);
		}

		// `UserInputService:GetMouseDelta()` — how far it moved this frame.
		//
		// **Roblox's spelling, and the value a locked pointer needs.** See
		// `InputState::MouseDelta`: under `LockCenter` the position does not
		// change and only this does.
		void GetMouseDelta(ScriptCall &call) {
			const InputState *input = InputOf(call);
			call.ReturnVector2(input == nullptr ? core::Vector2{} : input->MouseDelta);
		}

		// `UserInputService:GetLastInputType()` — which device spoke last.
		//
		// **The poll beside `LastInputTypeChanged`'s edge**, which is the pair
		// Roblox offers and the pair a place actually needs: the signal is when to
		// swap the prompts and this is what to swap them to on the frame a menu
		// opens, where no edge is coming.
		//
		// **`Keyboard` on a world with no input state**, which is the same answer
		// a world that has one but has never been touched gives — see
		// `InputState::LastSource` for why there is no `None` member to report
		// instead.
		void GetLastInputType(ScriptCall &call) {
			const InputState *input = InputOf(call);
			const scene::InputSource source =
				input == nullptr ? scene::InputSource::Keyboard : input->LastSource;
			call.ReturnEnum(core::Name("UserInputType"), core::Name(scene::Describe(source)));
		}

		// `UserInputService:GetKeysPressed()` — every key down now.
		//
		// **A list of `EnumItem`s rather than of strings**, so what comes out is
		// what `IsKeyDown` takes. A surface whose getter and setter disagree about
		// a type is the round trip a property owes and a service owes equally.
		void GetKeysPressed(ScriptCall &call) {
			const InputState *input = InputOf(call);
			if (input == nullptr) {
				call.ReturnEnums(core::Name("KeyCode"), {});
				return;
			}

			// In `KeyCode` order, which is the order both pumps walk — so what a
			// script polls and what it is delivered agree about sequence.
			std::vector<core::Name> pressed;
			for (size_t index = 0; index < static_cast<size_t>(KeyCode::Count); index++) {
				const auto key = static_cast<KeyCode>(index);
				if (input->IsKeyDown(key)) {
					pressed.push_back(core::Name(scene::Describe(key)));
				}
			}
			call.ReturnEnums(core::Name("KeyCode"), pressed);
		}

		// `UserInputService:GetMouseButtonsPressed()` — every button down now.
		//
		// **A list of `InputObject`s and not of `EnumItem`s**, which is Roblox's
		// shape and is the useful one: the object carries where the pointer was
		// as well as which button it is, so a handler that wants both does not
		// have to ask twice and risk the two disagreeing. It is also why this is
		// not simply `GetKeysPressed` with a different loop — that one answers
		// with what `IsKeyDown` takes, and this one answers with what
		// `InputBegan` delivers.
		void GetMouseButtonsPressed(ScriptCall &call) {
			const InputState *input = InputOf(call);
			if (input == nullptr) {
				call.ReturnInputObjects({});
				return;
			}

			std::vector<InputReport> down;
			for (size_t index = 0; index < static_cast<size_t>(MouseButton::Count); index++) {
				const auto button = static_cast<MouseButton>(index);
				if (!input->IsButtonDown(button)) {
					continue;
				}

				// `Begin` for a held button, which is what Roblox reports here:
				// the state of a button that is down is the one it went down in.
				down.push_back(ButtonReport(*input, button, true));
			}
			call.ReturnInputObjects(down);
		}

		// --- the properties ----------------------------------------------------

		// `UserInputService.MouseBehavior`, read and written.
		//
		// **The one member here that travels towards the client.** A script sets
		// it, the client applies it to the window on the next frame — which is why
		// `InputState` is the seam in both directions rather than a report.
		void GetMouseBehavior(ScriptCall &call) {
			const InputState *input = InputOf(call);
			const auto behaviour = input == nullptr ? MouseBehavior::Default : input->Behaviour;
			call.ReturnEnum(
				core::Name("MouseBehavior"),
				ecs::EnumTable::MemberAt(core::Name("MouseBehavior"), static_cast<size_t>(behaviour))
			);
		}

		void SetMouseBehavior(ScriptCall &call) {
			core::Name member;
			if (!call.ReadEnum(0, core::Name("MouseBehavior"), member)) {
				call.Raise("MouseBehavior expects an Enum.MouseBehavior");
			}

			size_t ordinal = 0;
			if (!ecs::EnumTable::OrdinalOf(core::Name("MouseBehavior"), member, ordinal)) {
				call.Raise("unknown MouseBehavior");
			}

			// **Dropped on a world with no input state rather than creating
			// one**, which is the opposite of `SoundService.Volume` and is right
			// for the opposite reason: an `InputState` is written every frame by
			// whoever owns the window, so a resource minted here would be one the
			// device layer immediately overwrites — where an `AudioState` is only
			// ever written by a script.
			if (auto *input = call.World().ResourceMutable<InputState>()) {
				input->Behaviour = static_cast<MouseBehavior>(ordinal);
			}
		}

		// `UserInputService.MouseIconEnabled`, read and written.
		//
		// **The second member travelling towards the window, and separate from
		// `MouseBehavior` because Roblox's two are.** An inventory screen wants
		// the pointer drawn while the camera stays locked, and a cutscene wants it
		// gone while the pointer still moves freely; one enum could spell neither.
		//
		// **True on a world with no input state**, which is what "is the pointer
		// drawn" answers on a server: nothing is drawing one, and reporting the
		// default beats reporting that somebody hid it.
		void GetMouseIconEnabled(ScriptCall &call) {
			const InputState *input = InputOf(call);
			call.ReturnBoolean(input == nullptr || input->MouseIconEnabled);
		}

		void SetMouseIconEnabled(ScriptCall &call) {
			// Dropped on a world with no input state, for `SetMouseBehavior`'s
			// reason: the device layer would overwrite a resource minted here.
			const bool wanted = call.OptionalBoolean(0, true);
			if (auto *input = call.World().ResourceMutable<InputState>()) {
				input->MouseIconEnabled = wanted;
			}
		}

		// What a `MouseDeltaSensitivity` of one means, in radians per pixel.
		//
		// **The same literal `scene::CameraController::Sensitivity` defaults to**,
		// which is what makes a world nobody has configured answer exactly 1 —
		// and it is a named constant because the getter and the setter both need
		// it and a property whose two halves disagree by a digit is a round trip
		// that does not close.
		constexpr float RADIANS_PER_PIXEL = 0.0035f;

		// `UserInputService.MouseDeltaSensitivity`, read and written.
		//
		// Read off the camera controller rather than kept twice. Roblox puts it on
		// `UserInputService` and the value it scales is the camera's, so one
		// number in one place with two names beats two numbers that agree until
		// somebody sets one.
		void GetMouseDeltaSensitivity(ScriptCall &call) {
			const auto *controller = call.World().Resource<scene::CameraController>();

			// **Divided in `float` and widened after**, which is not a style
			// choice: 0.0035 has no exact binary form, so promoting the stored
			// `float` to `double` first and dividing by the `double` literal
			// answers 1.000000030866691 for a controller nobody has touched. In
			// `float` the two are the same bits and the quotient is exactly one.
			call.ReturnNumber(
				controller == nullptr ? 1.0 : static_cast<double>(controller->Sensitivity / RADIANS_PER_PIXEL)
			);
		}

		void SetMouseDeltaSensitivity(ScriptCall &call) {
			const auto scale = static_cast<float>(call.AsNumber(0));
			if (auto *controller = call.World().ResourceMutable<scene::CameraController>()) {
				controller->Sensitivity = RADIANS_PER_PIXEL * std::max(scale, 0.0f);
			}
		}

		// `UserInputService.KeyboardEnabled` and `.MouseEnabled`.
		//
		// True on anything with a window and false headless, which is what "is
		// there a keyboard" actually asks. A world with no input state is a world
		// nobody is typing at.
		void GetInputDevicePresent(ScriptCall &call) {
			call.ReturnBoolean(InputOf(call) != nullptr);
		}

		// `GamepadEnabled`, `TouchEnabled`, `VREnabled`, `AccelerometerEnabled`
		// and `GyroscopeEnabled`.
		//
		// **Present and false, which is better than absent.** Roblox scripts
		// branch on these — `if UserInputService.TouchEnabled then` is how a place
		// picks its control scheme — and a missing property raises where a false
		// one takes the other branch. There is no gamepad, touch, headset or
		// sensor anywhere in `input::Translator`, so the answer is a constant and
		// saying so is the honest version of not having one.
		void GetNoSuchDevice(ScriptCall &call) {
			call.ReturnBoolean(false);
		}

		// --- what this service deliberately does not have ---------------------
		//
		// **An absent member is better than one that does nothing**, which is the
		// rule `SoundService.cpp` states at length and `HttpService.cpp` one door
		// along: a member that exists looks decided, so a script author writes
		// against it and finds out later that it was never going to work. A
		// property that answers a *constant* is the exception and the five above
		// are it — a place branches on `TouchEnabled` to pick a control scheme, and
		// a false answer is true.
		//
		// What is missing, and what each would need first:
		//
		// - **`GetConnectedGamepads`, `GetGamepadState`, `GetSupportedGamepadKeyCodes`,
		//   `IsGamepadButtonDown`, `GamepadConnected`, `GamepadDisconnected`.**
		//   `input::Translator` handles five SDL event types and none of them is a
		//   gamepad, so there is no device to enumerate — `GetConnectedGamepads`
		//   would be an empty list forever and `GetGamepadState` a list of nothing.
		//   Closing it is `SDL_Gamepad` in the translator plus the `Gamepad1..8`
		//   and `Button*`/`Thumbstick*` members in `scene::InputSource` and
		//   `scene::KeyCode`, which is a change in those two files rather than
		//   this one.
		//
		// - **`TouchStarted` and its five neighbours, `GetFocusedTextBox`,
		//   `TextBoxFocused`, `TextBoxFocusReleased`.** There is no touch surface,
		//   and `gui` has no keyboard focus at all — `Router` holds a hover and a
		//   press and a `TextBox` is a class that draws. That absence is also why
		//   `gameProcessedEvent` is false for every key; `InterfaceHasPointer`
		//   carries it.
		//
		// - **`MouseIcon`.** A cursor *image* is an asset the renderer would have
		//   to hand SDL as a surface, and nothing in `render` produces one.
		//   `MouseIconEnabled` is here because hiding a cursor needs no image.
		//
		// - **`GetStringForKeyCode`, `GetKeyCodeFromString`.** What is printed on
		//   a key is a keyboard *layout* question — `SDL_GetKeyName` answers it and
		//   lives at L12 where this is L9, so the answer would have to come to rest
		//   in `scene::InputState` as a table that changes when the player switches
		//   layout. Worth doing when something asks; nothing has.
		//
		// - **`GetDeviceAcceleration`, `GetDeviceGravity`, `GetDeviceRotation`,
		//   `VREnabled`'s methods.** No sensors and no headset, which is what the
		//   three constant properties above already say.
	}

	const ServiceSurface &UserInputServiceSurface() {
		static constexpr std::array<ServiceMethod, 7> METHODS{{
			{"IsKeyDown", IsKeyDown},
			{"IsMouseButtonPressed", IsMouseButtonPressed},
			{"GetMouseLocation", GetMouseLocation},
			{"GetMouseDelta", GetMouseDelta},
			{"GetKeysPressed", GetKeysPressed},
			{"GetMouseButtonsPressed", GetMouseButtonsPressed},
			{"GetLastInputType", GetLastInputType},
		}};

		// **Ten properties, three of them writable.** The seven read-only rows
		// carry a null setter, which both languages turn into a refusal naming
		// the member rather than into a write that goes nowhere.
		static constexpr std::array<ServiceProperty, 10> PROPERTIES{{
			{"MouseBehavior", GetMouseBehavior, SetMouseBehavior},
			{"MouseIconEnabled", GetMouseIconEnabled, SetMouseIconEnabled},
			{"MouseDeltaSensitivity", GetMouseDeltaSensitivity, SetMouseDeltaSensitivity},
			{"KeyboardEnabled", GetInputDevicePresent, nullptr},
			{"MouseEnabled", GetInputDevicePresent, nullptr},
			{"GamepadEnabled", GetNoSuchDevice, nullptr},
			{"TouchEnabled", GetNoSuchDevice, nullptr},
			{"VREnabled", GetNoSuchDevice, nullptr},
			{"AccelerometerEnabled", GetNoSuchDevice, nullptr},
			{"GyroscopeEnabled", GetNoSuchDevice, nullptr},
		}};

		// **One `SignalKind` told apart by name**, exactly as
		// `GetAttributeChangedSignal` reuses `PropertyChanged` — see
		// `ServiceSignal::Property`. The subject is `NULL_ENTITY` because these
		// are the world's edges and not any instance's, and both pumps fire the
		// row whose name matches.
		static constexpr std::array<ServiceSignal, 6> SIGNALS{{
			{"InputBegan", SignalKind::PropertyChanged, "InputBegan"},
			{"InputEnded", SignalKind::PropertyChanged, "InputEnded"},
			{"InputChanged", SignalKind::PropertyChanged, "InputChanged"},
			{"WindowFocused", SignalKind::PropertyChanged, "WindowFocused"},
			{"WindowFocusReleased", SignalKind::PropertyChanged, "WindowFocusReleased"},
			{"LastInputTypeChanged", SignalKind::PropertyChanged, "LastInputTypeChanged"},
		}};

		static const ServiceSurface SURFACE = [] {
			ServiceSurface surface;
			surface.Name = "UserInputService";
			surface.Methods = METHODS;
			surface.Properties = PROPERTIES;
			surface.Signals = SIGNALS;

			// **The properties are what make this a userdata in Luau**, and
			// `DEFERRED.md` D00030 is the sharp edge that survives it: `GETIMPORT`
			// caches a `Global.Field` chain whether the intermediate is a table or
			// a userdata, so a property is live only when read through a local.
			// Which is the form a Roblox script uses anyway, since
			// `game:GetService` is a method call and cannot be an import:
			//
			//     local UIS = game:GetService("UserInputService")
			//     UIS.MouseBehavior = Enum.MouseBehavior.LockCenter
			//
			// JavaScript needs neither, because an accessor is not an import.
			surface.Tag = TAG_INPUT_SERVICE;
			surface.MethodsKey = INPUT_METHODS_KEY;
			return surface;
		}();
		return SURFACE;
	}
}
