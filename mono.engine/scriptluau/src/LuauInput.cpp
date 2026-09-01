// Everything `UserInputService` and `ContextActionService` need a `lua_State`
// for, and nothing else.
//
// **This file is `JsInput.cpp`'s twin and was inside `InputServices.cpp` until
// v0.18.** That file described both services *and* met the VM on their behalf,
// so a description with no VM in it was compiled against `<lua.h>` and the pump
// was a heading rather than a file. What is here is the two things a language
// genuinely decides: the `InputObject` a handler is handed, and the walk that
// calls handlers at the barrier.
//
// **The report is shared and only the wrapper is here.** `Actions.hpp` holds
// `InputReport` and all four builders - `KeyReport`, `ButtonReport`,
// `MotionReport` and `WheelReport` - because two pumps building a report each is
// two answers to what a frame did. `ActionStack::ClaimingFrom` decides which
// claim a key reaches for the same reason, which is what makes `PumpJsInput` the
// same pump rather than a second one that agrees today.
//
// @tier L9 · shared
// @since v0.18

#include "LuauBindings.hpp"

#include <engine/scene/Input.hpp>

#include <cstddef>
#include <lualib.h>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::script {

	namespace {
		using scene::InputState;
		using scene::KeyCode;
		using scene::MouseButton;

		// The pump's input state, or null on a world nobody writes input to.
		//
		// **Null is the ordinary case on a server**, not an error: a headless
		// world ticks the same scripts and finds nothing pressed, so this answers
		// "nothing happened" rather than raising.
		//
		// **`ContextOf` and not `UpvalueContext`, which is the difference between
		// working everywhere and working only inside a bound closure.**
		// `UpvalueContext` reads an upvalue of the *currently running* C function,
		// and `PumpInput` is called by the runtime directly at the barrier with no
		// C closure on the stack at all. That read garbage and turned every
		// heartbeat into a failure, including on worlds with no input.
		//
		// The service's own members reach the world through `ScriptCall::World`
		// instead, since they became neutral - see `InputOf`.
		const InputState *StateOf(lua_State *state) {
			return ContextOf(state).World->Resource<InputState>();
		}

		const scene::ControllerState *ControllersOf(lua_State *state) {
			return ContextOf(state).World->Resource<scene::ControllerState>();
		}

		// --- InputObject ------------------------------------------------------
		//
		// **What Roblox hands an input handler, and what this engine used to hand
		// nobody.** `InputBegan` fired with a bare `Enum.KeyCode`, the bound-action
		// handler got one as its third argument, and the generated declarations
		// claimed the signals passed *nothing at all* - three different answers to
		// one question, none of them Roblox's, and a script copied from a Roblox
		// place indexed `input.KeyCode` on an `EnumItem` and got nil.
		//
		// So this is the datatype the whole surface was missing. Five fields,
		// which is what Roblox's `InputObject` has that a script reads; there is
		// no constructor, because an input report is a fact about a frame rather
		// than a value anybody authors, and Roblox offers none either.
		//
		// **The report itself is shared and only the wrapper is per language**,
		// since v0.16 - `Actions.hpp` holds `InputReport`, this file builds the
		// Luau userdata and `JsInput.cpp` the JavaScript object. That is what a
		// `ContextActionService` handler is handed as its third argument in
		// either language, and what `UserInputService`'s three input signals now carry
		// in either as well.

		// The metatable's name, and therefore what `typeof` answers.
		constexpr const char *INPUT_OBJECT_TYPE = "InputObject";

		int InputObjectIndex(lua_State *state) {
			const auto *report =
				static_cast<const InputReport *>(lua_touserdatatagged(state, 1, TAG_INPUT_OBJECT));
			if (report == nullptr) {
				luaL_typeerrorL(state, 1, INPUT_OBJECT_TYPE);
			}

			const std::string_view field = luaL_checkstring(state, 2);

			if (field == "KeyCode") {
				PushEnumItem(state, core::Name("KeyCode"), core::Name(scene::Describe(report->Key)));
				return 1;
			}
			if (field == "UserInputType") {
				PushEnumItem(state, core::Name("UserInputType"), core::Name(scene::Describe(report->Source)));
				return 1;
			}
			if (field == "UserInputState") {
				PushEnumItem(state, core::Name("UserInputState"), report->State);
				return 1;
			}
			if (field == "Position") {
				*PushVector3(state) = report->Position;
				return 1;
			}
			if (field == "Delta") {
				*PushVector3(state) = report->Delta;
				return 1;
			}

			luaL_errorL(state, "InputObject has no member '%s'", std::string(field).c_str());
		}
	}

	void PushInputObject(lua_State *state, const InputReport &report) {
		// **Not file-local since v0.16**, because `LuauCall.cpp` builds the list
		// `GetMouseButtonsPressed` answers with and that method is neutral now.
		// The metatable stays this file's; what crosses is one pusher, exactly as
		// `PushInstanceValue` does one door along.
		void *memory = lua_newuserdatatagged(state, sizeof(InputReport), TAG_INPUT_OBJECT);
		new (memory) InputReport(report);
		luaL_getmetatable(state, INPUT_OBJECT_TYPE);
		lua_setmetatable(state, -2);
	}

	void OpenInputObject(lua_State *state) {
		// **Read-only and unreachable**: there is no `__newindex`, so a handler
		// cannot edit the report it was given and hand it on, and `__metatable`
		// is set for `LuauValues.cpp`'s reason - a metatable a script can reach is one
		// it can rewrite, and then every `InputObject` in the world changes
		// underneath everything holding one.
		luaL_newmetatable(state, INPUT_OBJECT_TYPE);

		lua_pushcfunction(state, InputObjectIndex, "__index");
		lua_setfield(state, -2, "__index");

		// What `typeof` actually reads. Luau's `typeof` is a fastcall builtin
		// that returns this field rather than consulting a global - see
		// `LuauValues.cpp`'s `Install`.
		lua_pushstring(state, INPUT_OBJECT_TYPE);
		lua_setfield(state, -2, "__type");

		lua_pushstring(state, INPUT_OBJECT_TYPE);
		lua_setfield(state, -2, "__metatable");

		lua_pop(state, 1);
	}

	namespace {
		// Calls everything connected to one of the service's signals.
		//
		// **A helper because there are now five callers** - two key edges, two
		// button edges, motion and the wheel - where there was one. The name
		// filter is the whole of what distinguishes the signals: they share one
		// `SignalKind` and are told apart by what the connection carries, exactly
		// as `GetAttributeChangedSignal` reuses `PropertyChanged`.
		//
		// **`push` and not a report, since the signals stopped agreeing about
		// their arguments.** The three input signals take `(InputObject,
		// gameProcessedEvent)`, `LastInputTypeChanged` takes an
		// `Enum.UserInputType`, and the focus pair takes nothing - three shapes
		// over one retirement rule, and a second copy of that rule is exactly the
		// bug this function's own comment below records.
		//
		// @param state      The VM.
		// @param signal     Which signal, by name.
		// @param push       Pushes one call's arguments and answers how many.
		//        Called once per connected handler.
		// @param firstError Set to the first handler failure, if it is empty.
		template <typename Push>
		void FireInputSignal(lua_State *state, core::Name signal, Push push, std::string &firstError) {
			LuauContext &live = ContextOf(state);

			// **What a `:Once` connection spends, collected here and retired
			// below.** `SignalTable::Fire` deliberately does not retire anything
			// itself - only the VM knows how to release a callable, and a
			// disconnect arriving from inside a fire may be about a value still
			// on the stack - so every direct caller of `Fire` owes this, and this
			// one did not pay it. A `UserInputService` signal connected with
			// `:Once` fired on every edge for ever, which is a handler a script
			// cannot get rid of and did not ask to keep.
			//
			// `FireSignal` in `LuauSignals.cpp` and `FireJsSignal` in
			// `JsSurface.cpp` have always done this; those two are what this is
			// copied from rather than invented beside.
			std::vector<ConnectionId> spent;

			live.Signals.Fire(SignalKind::Input, ecs::NULL_ENTITY, [&](const Connection &connection) {
				if (connection.Property != signal) {
					return;
				}

				lua_getref(state, connection.Callback);
				const int arguments = push();

				if (lua_pcall(state, arguments, 0, 0) != LUA_OK) {
					if (firstError.empty()) {
						const char *message = lua_tostring(state, -1);
						firstError = message != nullptr ? message : "an input listener failed";
					}
					lua_pop(state, 1);
				}

				if (connection.Once) {
					spent.push_back(connection.Id);
				}
			});

			// **After the fire and not inside it**, which is the reason the list
			// exists at all: a `:Once` handler that connects another one would
			// otherwise have the connection list compacted underneath the walk
			// that is still reading it.
			//
			// **The name filter is not applied here**, and it does not need to
			// be: only connections the walk above actually called were pushed, so
			// a `:Once` on a *different* input signal is untouched.
			for (const ConnectionId id : spent) {
				CallbackRef released = 0;
				if (live.Signals.Disconnect(id, released)) {
					lua_unref(state, released);
				}
			}
		}

		// Hands one key edge down the claims until one of them sinks it.
		//
		// **The highest claim decides, and until v0.16 it was the only one that
		// heard.** `Enum.ContextActionResult` is how Roblox lets a handler decline
		// - `Pass` hands the press to the next claim down, and `Sink` or no answer
		// at all stops it here - and a pump that ignored the return value made the
		// enum unspellable and the door-and-vehicle case unsolvable in the one
		// direction that needs it: a vehicle that is not being driven wants to let
		// E through to the door it is parked beside.
		//
		// **A raise sinks.** A handler that failed did not say `Pass`, and handing
		// the key down on the strength of a crash would make a broken script change
		// which *other* script runs.
		//
		// **Keys only.** `BoundAction::Keys` holds `scene::KeyCode` ordinals, and
		// Roblox's `BindAction` also takes `Enum.UserInputType` members - binding a
		// mouse button would need the vector to say which of the two spaces each
		// entry is in, which is a change to `Actions.hpp`'s `BoundAction` rather
		// than to this loop.
		//
		// **`ActionStack::ClaimingFrom` decides the order, not this function**,
		// which is what makes `PumpJsInput` the same pump rather than a second one
		// that agrees today.
		void
		RunBoundActions(lua_State *state, KeyCode key, const InputReport &report, std::string &firstError) {
			size_t position = 0;
			while (true) {
				const BoundAction *action =
					ContextOf(state).Actions.ClaimingFrom(static_cast<uint16_t>(key), position);
				if (action == nullptr) {
					return;
				}

				lua_getref(state, action->Callback);
				lua_pushstring(state, action->Name.c_str());
				PushEnumItem(state, core::Name("UserInputState"), report.State);

				// **An `InputObject` where this used to push an `Enum.KeyCode`.**
				// Roblox's third argument has always been an `InputObject`, the
				// generated declarations said `Enum_KeyCode`, and a handler copied
				// from a Roblox place read `input.KeyCode` off an `EnumItem` and
				// got nil. A behaviour change, and a stated one.
				PushInputObject(state, report);

				if (lua_pcall(state, 3, 1, 0) != LUA_OK) {
					if (firstError.empty()) {
						const char *message = lua_tostring(state, -1);
						firstError = message != nullptr ? message : "a bound action failed";
					}
					lua_pop(state, 1);
					return;
				}

				// **Anything that is not `Pass` sinks**, which covers `Sink`, nil
				// and whatever else a handler happened to return - `ReadEnumValue`
				// answers false for all three. Roblox's default is to sink and a
				// stray return value must not silently change which script gets a
				// key.
				core::Name answer;
				const bool pass = ReadEnumValue(state, -1, core::Name("ContextActionResult"), answer) &&
								  answer == core::Name("Pass");
				lua_pop(state, 1);

				if (!pass) {
					return;
				}
			}
		}
	}

	std::string PumpInput(lua_State *state, std::span<const gui::GuiEvent> interface) {
		const InputState *input = StateOf(state);
		const scene::ControllerState *controllers = ControllersOf(state);
		if (input == nullptr && controllers == nullptr) {
			return {};
		}
		if ((input == nullptr || !input->HasFrameEvents()) &&
			(controllers == nullptr || !controllers->HasFrameEvents())) {
			return {};
		}

		std::string firstError;

		// **What the interface already took, decided once for the whole beat.**
		// `InterfaceHasPointer` and `InterfaceHasKeyboard` carry what each answer
		// means; asking the first per edge would walk the same queue for every
		// button on a frame that produced several.
		const bool pointerTaken = InterfaceHasPointer(interface);
		const bool keyboardTaken = InterfaceHasKeyboard(*ContextOf(state).World);

		// The two shapes every input signal below is fired with. **Named rather
		// than written out four times**, because the argument list is the half a
		// pump gets wrong - a signal that quietly passed one argument where its
		// neighbour passed two is `InputChanged` again.
		const auto pushNothing = [] { return 0; };
		const auto pushReport = [state, pointerTaken, keyboardTaken](const InputReport &report) {
			return [state, pointerTaken, keyboardTaken, &report] {
				PushInputObject(state, report);

				// **Roblox's second argument, and it is `false` rather than
				// absent when nothing took the input.** A handler written
				// `function(input, processed)` reads nil as false either way; one
				// written `if not processed then` on a signal that passed nothing
				// would too, which is exactly why the gap was invisible for six
				// versions.
				lua_pushboolean(
					state,
					IsPointerReport(report) ? pointerTaken
											: report.Source == scene::InputSource::Keyboard && keyboardTaken
				);
				return 2;
			};
		};

		// **Focus first, before the releases it caused.**
		// `input::Translator::ReleaseAll` clears every key on the frame focus is
		// lost, so this pump is also the one that reports them released - and a
		// listener that hears "you lost focus" after "W came up" has to guess
		// which of the two explains the other.
		if (input != nullptr && input->WasFocusGained()) {
			FireInputSignal(state, core::Name("WindowFocused"), pushNothing, firstError);
		}
		if (input != nullptr && input->WasFocusLost()) {
			FireInputSignal(state, core::Name("WindowFocusReleased"), pushNothing, firstError);
		}

		// **Before the edges, because it explains them.** A place that swaps its
		// prompts on this signal should have swapped them before the handler for
		// the press that changed the device runs, or the first press after a
		// switch is read against the old scheme.
		if (input != nullptr && input->WasLastSourceChanged()) {
			const core::Name member(scene::Describe(input->LastSource));
			FireInputSignal(
				state,
				core::Name("LastInputTypeChanged"),
				[state, member] {
					PushEnumItem(state, core::Name("UserInputType"), member);
					return 1;
				},
				firstError
			);
		}

		// **Edges only.** A held key fires once, which is what a bound action
		// means - `Enum.UserInputState.Begin` and `.End` are the two calls a
		// handler gets, and a third every frame would make every action a
		// repeat-rate question.
		for (size_t index = 0; input != nullptr && index < static_cast<size_t>(KeyCode::KeyboardCount);
			 index++) {
			const auto key = static_cast<KeyCode>(index);
			const bool began = input->WasKeyPressed(key);
			if (!began && !input->WasKeyReleased(key)) {
				continue;
			}

			const InputReport report = KeyReport(key, began);
			RunBoundActions(state, key, report, firstError);

			// The service's own signals, after the bound actions. **Not gated by
			// them**: `InputBegan` is a report of what happened and a claim on a
			// key does not stop it having happened, which is Roblox's split and
			// the one that lets a debug overlay watch every key regardless.
			FireInputSignal(
				state, core::Name(began ? "InputBegan" : "InputEnded"), pushReport(report), firstError
			);
		}

		if (controllers != nullptr) {
			for (size_t slotIndex = 0; slotIndex < scene::MAX_CONTROLLERS; slotIndex++) {
				const scene::ControllerSlot &slot = controllers->Slots[slotIndex];
				if (slot.Connected != slot.PreviousConnected) {
					const core::Name member(
						scene::Describe(
							static_cast<scene::InputSource>(
								static_cast<uint8_t>(scene::InputSource::Gamepad1) + slotIndex
							)
						)
					);
					FireInputSignal(
						state,
						core::Name(slot.Connected ? "GamepadConnected" : "GamepadDisconnected"),
						[state, member] {
							PushEnumItem(state, core::Name("UserInputType"), member);
							return 1;
						},
						firstError
					);
				}
				for (size_t buttonIndex = 0;
					 buttonIndex < static_cast<size_t>(scene::ControllerButton::Count);
					 buttonIndex++) {
					const auto button = static_cast<scene::ControllerButton>(buttonIndex);
					const bool began = slot.WasPressed(button);
					if (!began && !slot.WasReleased(button)) continue;
					const InputReport report = ControllerButtonReport(slotIndex, button, began);
					RunBoundActions(state, report.Key, report, firstError);
					FireInputSignal(
						state, core::Name(began ? "InputBegan" : "InputEnded"), pushReport(report), firstError
					);
				}

				for (const bool right : {false, true}) {
					const size_t x = static_cast<size_t>(
						right ? scene::ControllerAxis::RightX : scene::ControllerAxis::LeftX
					);
					const size_t y = static_cast<size_t>(
						right ? scene::ControllerAxis::RightY : scene::ControllerAxis::LeftY
					);
					if (slot.Axes[x] == slot.PreviousAxes[x] && slot.Axes[y] == slot.PreviousAxes[y])
						continue;
					const InputReport report = ControllerStickReport(slotIndex, slot, right);
					FireInputSignal(state, core::Name("InputChanged"), pushReport(report), firstError);
				}

				for (const scene::ControllerAxis axis :
					 {scene::ControllerAxis::LeftTrigger, scene::ControllerAxis::RightTrigger}) {
					const size_t index = static_cast<size_t>(axis);
					if (slot.Axes[index] == slot.PreviousAxes[index]) continue;
					const InputReport report = ControllerTriggerReport(slotIndex, slot, axis);
					FireInputSignal(state, core::Name("InputChanged"), pushReport(report), firstError);
				}
			}
		}

		// **The buttons, which fired nothing at all before v0.16.** `InputState`
		// has carried `WasButtonPressed` since v0.10 and nothing in the script
		// layer ever asked it, so a click was invisible to a script that was not
		// polling - the one input this engine had that a Roblox place could not
		// hear. No bound actions here, for the reason `RunBoundActions` gives.
		for (size_t index = 0; input != nullptr && index < static_cast<size_t>(MouseButton::Count); index++) {
			const auto button = static_cast<MouseButton>(index);
			const bool began = input->WasButtonPressed(button);
			if (!began && !input->WasButtonReleased(button)) {
				continue;
			}

			const InputReport report = ButtonReport(*input, button, began);
			FireInputSignal(
				state, core::Name(began ? "InputBegan" : "InputEnded"), pushReport(report), firstError
			);
		}

		// **`InputChanged`, which the service has offered since v0.10 and which
		// nothing ever fired.** It was reachable, connectable and silent - which
		// reads as a broken engine rather than as an unfinished one, and is the
		// trade `v0.5` records for `Heartbeat`. Motion and the wheel are the two
		// things this engine can report changing.
		//
		// **Exact compares against zero, not an epsilon.** Both fields are written
		// by the translator as a sum of integer SDL deltas and cleared to a literal
		// zero every frame, so "did anything happen" is exactly the question a
		// compare answers here - and an epsilon would swallow the one-pixel move
		// that a slow drag is made of.
		if (input != nullptr && (input->MouseDelta.X != 0.0f || input->MouseDelta.Y != 0.0f)) {
			const InputReport report = MotionReport(*input);
			FireInputSignal(state, core::Name("InputChanged"), pushReport(report), firstError);
		}

		if (input != nullptr && input->WheelDelta != 0.0f) {
			const InputReport report = WheelReport(*input);
			FireInputSignal(state, core::Name("InputChanged"), pushReport(report), firstError);
		}

		return firstError;
	}
}
