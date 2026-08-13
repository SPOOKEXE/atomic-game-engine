#include "Bindings.hpp"

#include <engine/ecs/EnumTable.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Controls.hpp>
#include <engine/scene/Input.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
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
		// instead, since they became neutral — see `InputOf`.
		const InputState *StateOf(lua_State *state) {
			return ContextOf(state).World->Resource<InputState>();
		}

		// --- InputObject ------------------------------------------------------
		//
		// **What Roblox hands an input handler, and what this engine used to hand
		// nobody.** `InputBegan` fired with a bare `Enum.KeyCode`, the bound-action
		// handler got one as its third argument, and the generated declarations
		// claimed the signals passed *nothing at all* — three different answers to
		// one question, none of them Roblox's, and a script copied from a Roblox
		// place indexed `input.KeyCode` on an `EnumItem` and got nil.
		//
		// So this is the datatype the whole surface was missing. Five fields,
		// which is what Roblox's `InputObject` has that a script reads; there is
		// no constructor, because an input report is a fact about a frame rather
		// than a value anybody authors, and Roblox offers none either.
		//
		// **The report itself is shared and only the wrapper is per language**,
		// since v0.16 — `Actions.hpp` holds `InputReport`, this file builds the
		// Luau userdata and `JsInput.cpp` the JavaScript object. That is what a
		// `ContextActionService` handler is handed as its third argument in
		// either language, and what `UserInputService`'s five signals now carry
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
		// is set for `Values.cpp`'s reason — a metatable a script can reach is one
		// it can rewrite, and then every `InputObject` in the world changes
		// underneath everything holding one.
		luaL_newmetatable(state, INPUT_OBJECT_TYPE);

		lua_pushcfunction(state, InputObjectIndex, "__index");
		lua_setfield(state, -2, "__index");

		// What `typeof` actually reads. Luau's `typeof` is a fastcall builtin
		// that returns this field rather than consulting a global — see
		// `Values.cpp`'s `Install`.
		lua_pushstring(state, INPUT_OBJECT_TYPE);
		lua_setfield(state, -2, "__type");

		lua_pushstring(state, INPUT_OBJECT_TYPE);
		lua_setfield(state, -2, "__metatable");

		lua_pop(state, 1);
	}

	namespace {

		// --- UserInputService -------------------------------------------------
		//
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

		// --- ContextActionService ---------------------------------------------
		//
		// **What this adds over `UserInputService` is a priority stack**, and that
		// is the whole of it. Polling `IsKeyDown` is fine until two systems want E
		// — a door and a vehicle — and then the question is which one gets it. A
		// bound action is a claim on a key with a number attached, and the highest
		// claim wins.
		//
		// **The stack is shared and the callables are not, since v0.16.** What a
		// claim *is* and which handler a press reaches are ordering rules a
		// recording depends on, so they live in `ActionStack` and both languages
		// read them; a Luau handler is a registry ref and a JavaScript one an
		// index into `JsContext::Callables`, and both cross as a `CallbackRef`
		// nothing shared may interpret. That is what let this service become the
		// first input surface JavaScript can reach.
		//
		// It still lives on the VM rather than on the world, because a bound
		// action holds a callable: it cannot cross a snapshot, cannot be
		// replicated, and dies with the runtime that registered it. A world
		// resource holding one would be a resource that cannot be serialised.

		// `ContextActionService:BindActionAtPriority(name, handler, touchButton, priority, ...keys)`
		//
		// The five-argument form is the one that says what it means; the
		// four-argument `BindAction` below is it with a priority of zero.
		//
		// @param call     The call.
		// @param priority Where the priority sits, or a negative index for the
		//        form that has none.
		// @param firstKey Where the variadic key list starts.
		void BindAtPriority(ScriptCall &call, int priority, size_t firstKey) {
			BoundAction action;
			action.Name = call.AsString(0);
			action.Callback = call.RetainCallback(1);
			action.Priority = priority;

			// **The whole tail rather than up to the first nil**, which is what
			// `ScriptCall::Arguments` exists for: Roblox's `BindAction` also
			// takes `Enum.UserInputType` members and this engine binds keys only,
			// so a value this cannot read is walked past rather than refused.
			for (size_t index = firstKey; index < call.Arguments(); index++) {
				core::Name member;
				if (!call.ReadEnum(index, core::Name("KeyCode"), member)) {
					continue;
				}

				const KeyCode key = scene::KeyFromName(member.Text());
				if (key != KeyCode::Unknown) {
					action.Keys.push_back(static_cast<uint16_t>(key));
				}
			}

			// **The replaced handler is released and not leaked.** Rebinding a
			// name replaces it rather than stacking a second — Roblox's
			// behaviour, and the one that makes a script safe to run twice — so
			// the callable the old row held has to go back to the VM that minted
			// it.
			CallbackRef replaced = 0;
			if (call.Actions().Bind(std::move(action), replaced)) {
				call.ReleaseCallback(replaced);
			}
		}

		void BindAction(ScriptCall &call) {
			// `(name, handler, createTouchButton, ...keys)` — the touch button is
			// accepted and ignored, because there is no touch surface. Accepted
			// rather than refused so a Roblox script runs unchanged.
			BindAtPriority(call, 0, 3);
		}

		void BindActionAtPriority(ScriptCall &call) {
			// `(name, handler, createTouchButton, priority, ...keys)`
			BindAtPriority(call, static_cast<int>(call.AsNumber(3)), 4);
		}

		void UnbindAction(ScriptCall &call) {
			CallbackRef released = 0;
			if (call.Actions().Unbind(call.AsString(0), released)) {
				call.ReleaseCallback(released);
			}
		}

		void UnbindAllActions(ScriptCall &call) {
			std::vector<CallbackRef> released;
			call.Actions().UnbindAll(released);
			for (const CallbackRef callback : released) {
				call.ReleaseCallback(callback);
			}
		}

		// --- the two that are still Luau's ------------------------------------
		//
		// **`GetBoundActionInfo` and `GetAllBoundActionInfo` answer a record
		// holding a list of `Enum.KeyCode` members, and that is what keeps them
		// here.** `ScriptCall` can return a `ScriptValue`, which has no tag for an
		// `EnumItem` — and it must not gain one, because a `ScriptValue` crosses a
		// world and `Codec.hpp` is a wire format. The alternatives were to invent
		// a return type for one service's record shape, which `ScriptCall.hpp`
		// says the interface is not for, or to answer key *names* as strings,
		// which is a behaviour change to a Roblox-shaped surface made to buy a
		// diagnostic method nothing polls.
		//
		// So they sit in `ServiceSurface::LuauMethods`, where the shape itself
		// says a JavaScript author does not have them — the same per-method gap
		// `TeleportService::GetTeleportData` has had since v0.15, now with
		// somewhere to declare it. Closing it needs a neutral `EnumItem` return,
		// which is a change to make when a second caller wants one.

		// Pushes one action's info table.
		//
		// **Four of Roblox's six fields, and the two that are absent are absent
		// from the *binding*, not from this table.** `title` and `description`
		// come from `SetTitle` and `SetDescription`, which decorate a touch
		// button — there is no touch surface, so there is nothing to title and
		// the pair is not bound. Reporting them as empty strings would claim they
		// had been set to nothing.
		//
		// @param state    The VM.
		// @param action   The bound action.
		// @param position Its zero-based place in the priority-sorted list.
		// @param total    How many actions are bound.
		void PushActionInfo(lua_State *state, const BoundAction &action, size_t position, size_t total) {
			lua_createtable(state, 0, 4);

			lua_createtable(state, static_cast<int>(action.Keys.size()), 0);
			for (size_t index = 0; index < action.Keys.size(); index++) {
				const auto key = static_cast<KeyCode>(action.Keys[index]);
				PushEnumItem(state, core::Name("KeyCode"), core::Name(scene::Describe(key)));
				lua_rawseti(state, -2, static_cast<int>(index) + 1);
			}
			lua_setfield(state, -2, "inputTypes");

			lua_pushinteger(state, action.Priority);
			lua_setfield(state, -2, "priorityLevel");

			// **Inverted, so higher still wins.** `LuauContext::Actions` is sorted
			// highest priority *first*, and Roblox's `stackOrder` counts the other
			// way — the largest number is the claim that gets the key. Reporting
			// our index under Roblox's name would be the same word meaning the
			// opposite thing, which is worse than not reporting it.
			lua_pushinteger(state, static_cast<int>(total - position));
			lua_setfield(state, -2, "stackOrder");

			// Accepted and ignored at bind time, for the reason `BindAction`
			// gives, so it is always false here rather than whatever was passed.
			lua_pushboolean(state, false);
			lua_setfield(state, -2, "createTouchButton");
		}

		// `ContextActionService:GetBoundActionInfo(name)` -> table or nil
		int GetBoundActionInfo(lua_State *state) {
			const std::string_view name = luaL_checkstring(state, 2);
			const ActionStack &stack = ContextOf(state).Actions;
			const std::span<const BoundAction> actions = stack.Entries();

			const BoundAction *found = stack.Find(name);
			if (found == nullptr) {
				// **Nil for an unbound name, not an empty table.** Roblox's
				// answer, and the one `if info then` reads correctly — an empty
				// table is truthy and would report every name as bound.
				lua_pushnil(state);
				return 1;
			}

			// **`stackOrder` needs the position and `Find` answers the row**, so
			// the span is what turns one into the other — a pointer into the
			// stack's own vector, subtracted from its start.
			PushActionInfo(state, *found, static_cast<size_t>(found - actions.data()), actions.size());
			return 1;
		}

		// `ContextActionService:GetAllBoundActionInfo()` -> { [name]: table }
		//
		// Keyed by name rather than an array, which is Roblox's shape and the one
		// a caller wants: the question this answers is "what has claimed E", and
		// the name is how anything is unbound afterwards.
		int GetAllBoundActionInfo(lua_State *state) {
			const std::span<const BoundAction> actions = ContextOf(state).Actions.Entries();

			lua_createtable(state, 0, static_cast<int>(actions.size()));
			for (size_t index = 0; index < actions.size(); index++) {
				PushActionInfo(state, actions[index], index, actions.size());
				lua_setfield(state, -2, actions[index].Name.c_str());
			}
			return 1;
		}
	}

	// --- the pump -------------------------------------------------------------

	namespace {
		// Calls everything connected to one of the service's signals.
		//
		// **A helper because there are now five callers** — two key edges, two
		// button edges, motion and the wheel — where there was one. The name
		// filter is the whole of what distinguishes the signals: they share one
		// `SignalKind` and are told apart by what the connection carries, exactly
		// as `GetAttributeChangedSignal` reuses `PropertyChanged`.
		//
		// @param state      The VM.
		// @param signal     Which signal, by name.
		// @param report     The `InputObject` to hand over, or null for the two
		//        focus signals, which Roblox calls with nothing.
		// @param firstError Set to the first handler failure, if it is empty.
		void FireInputSignal(
			lua_State *state, core::Name signal, const InputReport *report, std::string &firstError
		) {
			LuauContext &live = ContextOf(state);

			// **What a `:Once` connection spends, collected here and retired
			// below.** `SignalTable::Fire` deliberately does not retire anything
			// itself — only the VM knows how to release a callable, and a
			// disconnect arriving from inside a fire may be about a value still
			// on the stack — so every direct caller of `Fire` owes this, and this
			// one did not pay it. A `UserInputService` signal connected with
			// `:Once` fired on every edge for ever, which is a handler a script
			// cannot get rid of and did not ask to keep.
			//
			// `FireSignal` in `LuauSignals.cpp` and `FireJsSignal` in
			// `JsSurface.cpp` have always done this; those two are what this is
			// copied from rather than invented beside.
			std::vector<ConnectionId> spent;

			live.Signals.Fire(
				SignalKind::PropertyChanged, ecs::NULL_ENTITY, [&](const Connection &connection) {
					if (connection.Property != signal) {
						return;
					}

					lua_getref(state, connection.Callback);
					if (report != nullptr) {
						PushInputObject(state, *report);
					}

					if (lua_pcall(state, report != nullptr ? 1 : 0, 0, 0) != LUA_OK) {
						if (firstError.empty()) {
							const char *message = lua_tostring(state, -1);
							firstError = message != nullptr ? message : "an input listener failed";
						}
						lua_pop(state, 1);
					}

					if (connection.Once) {
						spent.push_back(connection.Id);
					}
				}
			);

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

		// Hands one key edge to the first bound action that claims it.
		//
		// **The first claim wins and the rest never see it**, which is the whole
		// reason `ContextActionService` exists beside polling.
		// `Enum.ContextActionResult.Pass` would let a handler decline — Roblox has
		// it and this does not, because a handler that returns nothing should sink
		// the input and returning `Pass` is the rarer case worth adding when
		// somebody needs it.
		//
		// **Keys only.** `BoundAction::Keys` holds `scene::KeyCode` ordinals, and
		// Roblox's `BindAction` also takes `Enum.UserInputType` members — binding a
		// mouse button would need the vector to say which of the two spaces each
		// entry is in, which is a change to `Actions.hpp`'s `BoundAction` rather
		// than to this loop.
		//
		// **`ActionStack::Claiming` decides, not this function**, which is what
		// makes `PumpJsInput` the same pump rather than a second one that agrees
		// today: which action wins is a rule and lives with the stack.
		void
		RunBoundActions(lua_State *state, KeyCode key, const InputReport &report, std::string &firstError) {
			const BoundAction *action = ContextOf(state).Actions.Claiming(static_cast<uint16_t>(key));
			if (action == nullptr) {
				return;
			}

			lua_getref(state, action->Callback);
			lua_pushstring(state, action->Name.c_str());
			PushEnumItem(state, core::Name("UserInputState"), report.State);

			// **An `InputObject` where this used to push an `Enum.KeyCode`.**
			// Roblox's third argument has always been an `InputObject`, the
			// generated declarations said `Enum_KeyCode`, and a handler copied
			// from a Roblox place read `input.KeyCode` off an `EnumItem` and got
			// nil. A behaviour change, and a stated one.
			PushInputObject(state, report);

			if (lua_pcall(state, 3, 0, 0) != LUA_OK) {
				if (firstError.empty()) {
					const char *message = lua_tostring(state, -1);
					firstError = message != nullptr ? message : "a bound action failed";
				}
				lua_pop(state, 1);
			}
		}
	}

	std::string PumpInput(lua_State *state) {
		const InputState *input = StateOf(state);
		if (input == nullptr) {
			return {};
		}

		std::string firstError;

		// **Focus first, before the releases it caused.**
		// `input::Translator::ReleaseAll` clears every key on the frame focus is
		// lost, so this pump is also the one that reports them released — and a
		// listener that hears "you lost focus" after "W came up" has to guess
		// which of the two explains the other.
		if (input->WasFocusGained()) {
			FireInputSignal(state, core::Name("WindowFocused"), nullptr, firstError);
		}
		if (input->WasFocusLost()) {
			FireInputSignal(state, core::Name("WindowFocusReleased"), nullptr, firstError);
		}

		// **Edges only.** A held key fires once, which is what a bound action
		// means — `Enum.UserInputState.Begin` and `.End` are the two calls a
		// handler gets, and a third every frame would make every action a
		// repeat-rate question.
		for (size_t index = 0; index < static_cast<size_t>(KeyCode::Count); index++) {
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
			FireInputSignal(state, core::Name(began ? "InputBegan" : "InputEnded"), &report, firstError);
		}

		// **The buttons, which fired nothing at all before v0.16.** `InputState`
		// has carried `WasButtonPressed` since v0.10 and nothing in the script
		// layer ever asked it, so a click was invisible to a script that was not
		// polling — the one input this engine had that a Roblox place could not
		// hear. No bound actions here, for the reason `RunBoundActions` gives.
		for (size_t index = 0; index < static_cast<size_t>(MouseButton::Count); index++) {
			const auto button = static_cast<MouseButton>(index);
			const bool began = input->WasButtonPressed(button);
			if (!began && !input->WasButtonReleased(button)) {
				continue;
			}

			const InputReport report = ButtonReport(*input, button, began);
			FireInputSignal(state, core::Name(began ? "InputBegan" : "InputEnded"), &report, firstError);
		}

		// **`InputChanged`, which the service has offered since v0.10 and which
		// nothing ever fired.** It was reachable, connectable and silent — which
		// reads as a broken engine rather than as an unfinished one, and is the
		// trade `v0.5` records for `Heartbeat`. Motion and the wheel are the two
		// things this engine can report changing.
		//
		// **Exact compares against zero, not an epsilon.** Both fields are written
		// by the translator as a sum of integer SDL deltas and cleared to a literal
		// zero every frame, so "did anything happen" is exactly the question a
		// compare answers here — and an epsilon would swallow the one-pixel move
		// that a slow drag is made of.
		if (input->MouseDelta.X != 0.0f || input->MouseDelta.Y != 0.0f) {
			const InputReport report = MotionReport(*input);
			FireInputSignal(state, core::Name("InputChanged"), &report, firstError);
		}

		if (input->WheelDelta != 0.0f) {
			const InputReport report = WheelReport(*input);
			FireInputSignal(state, core::Name("InputChanged"), &report, firstError);
		}

		return firstError;
	}

	const ServiceSurface &UserInputServiceSurface() {
		static constexpr std::array<ServiceMethod, 6> METHODS{{
			{"IsKeyDown", IsKeyDown},
			{"IsMouseButtonPressed", IsMouseButtonPressed},
			{"GetMouseLocation", GetMouseLocation},
			{"GetMouseDelta", GetMouseDelta},
			{"GetKeysPressed", GetKeysPressed},
			{"GetMouseButtonsPressed", GetMouseButtonsPressed},
		}};

		// **Nine properties, two of them writable.** The seven read-only rows
		// carry a null setter, which both languages turn into a refusal naming
		// the member rather than into a write that goes nowhere.
		static constexpr std::array<ServiceProperty, 9> PROPERTIES{{
			{"MouseBehavior", GetMouseBehavior, SetMouseBehavior},
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
		static constexpr std::array<ServiceSignal, 5> SIGNALS{{
			{"InputBegan", SignalKind::PropertyChanged, "InputBegan"},
			{"InputEnded", SignalKind::PropertyChanged, "InputEnded"},
			{"InputChanged", SignalKind::PropertyChanged, "InputChanged"},
			{"WindowFocused", SignalKind::PropertyChanged, "WindowFocused"},
			{"WindowFocusReleased", SignalKind::PropertyChanged, "WindowFocusReleased"},
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

	const ServiceSurface &ContextActionServiceSurface() {
		// A plain table, because this one is **methods only** — and a method is
		// exactly the case `GETIMPORT` is correct for: the closure never
		// changes, so caching it is what the optimisation is for. Only a mutable
		// *property* is broken by it. That is also why this one could cross to
		// JavaScript and `UserInputService` above it could not.
		static constexpr std::array<ServiceMethod, 4> METHODS{{
			{"BindAction", BindAction},
			{"BindActionAtPriority", BindActionAtPriority},
			{"UnbindAction", UnbindAction},
			{"UnbindAllActions", UnbindAllActions},
		}};

		// The two that report, which the section above them says why.
		static constexpr std::array<LuauServiceMethod, 2> REPORTING{{
			{"GetBoundActionInfo", GetBoundActionInfo},
			{"GetAllBoundActionInfo", GetAllBoundActionInfo},
		}};

		static const ServiceSurface SURFACE = [] {
			ServiceSurface surface;
			surface.Name = "ContextActionService";
			surface.Methods = METHODS;
			surface.LuauMethods = REPORTING;
			return surface;
		}();
		return SURFACE;
	}
}
