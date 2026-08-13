#include "Bindings.hpp"

#include <engine/ecs/EnumTable.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Controls.hpp>
#include <engine/scene/Input.hpp>

#include <algorithm>
#include <lualib.h>
#include <new>
#include <string>
#include <string_view>
#include <vector>

namespace engine::script {

	namespace {
		using scene::InputSource;
		using scene::InputState;
		using scene::KeyCode;
		using scene::MouseBehavior;
		using scene::MouseButton;

		// Where `UserInputService`'s method table lives, since the service is a
		// userdata and a userdata has no fields.
		//
		// **One constant read by both ends.** `OpenInputServices` hands it to
		// `InstallService` and `InputServiceIndex` reads it back; two spellings
		// of one key is a service whose methods are all nil, with nothing in the
		// build to say so.
		constexpr const char *INPUT_METHODS_KEY = "engine.userinput.methods";

		// This VM's world.
		//
		// **`ContextOf` and not `UpvalueContext`, which is the difference between
		// working everywhere and working only inside a bound closure.**
		// `UpvalueContext` reads an upvalue of the *currently running* C function,
		// so it is correct for the methods below and wrong for `PumpInput` — which
		// the runtime calls directly at the barrier, with no C closure on the
		// stack at all. That read garbage and turned every heartbeat into a
		// failure, including on worlds with no input.
		//
		// `ContextOf` goes through the registry and is right in both places, which
		// is why `PumpChanges` has always used it.
		ecs::Store &WorldOf(lua_State *state) {
			return *ContextOf(state).World;
		}

		// The input state, or null on a world nobody writes input to.
		//
		// **Null is the ordinary case on a server**, not an error: a headless
		// world ticks the same scripts and finds nothing pressed. Every function
		// here answers "no" rather than raising, so a script that polls input on
		// both halves of a game does not have to guard.
		const InputState *StateOf(lua_State *state) {
			return WorldOf(state).Resource<InputState>();
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
		// **Luau only, like the two services that produce it.** Nothing in
		// `JsBindings.hpp` can build a service with a live property — see the
		// `SoundService` row in `ServiceCatalogue.cpp` — so JavaScript has no
		// `UserInputService` to hand one to, and a datatype with no producer is a
		// declaration nobody can obtain a value of.

		// One input event, as a script sees it.
		//
		// Trivially copyable on purpose: it is placement-newed into userdata
		// memory, exactly as every value type in `Values.cpp` is.
		struct InputReport {
			// Where the pointer was, in pixels from the top-left of the window,
			// with the wheel's notches in Z.
			//
			// **Roblox's placement exactly**, including the odd-looking Z: a
			// wheel notch has nowhere else to go in a `Vector3`, and a script
			// migrated from a Roblox place reads `input.Position.Z` for it.
			core::Vector3 Position;

			// How far it moved since the previous frame, in the same space.
			core::Vector3 Delta;

			// `Begin`, `Change` or `End`, as an `Enum.UserInputState` member.
			//
			// **A name rather than an ordinal**, because the member list is
			// registered in `scene/Part.cpp` and an ordinal here would be a
			// second statement of its order — the kind that agrees until
			// somebody inserts a member.
			core::Name State;

			// The key, or `Unknown` for anything that is not one. Roblox reports
			// `Enum.KeyCode.Unknown` for a mouse event and so does this.
			KeyCode Key = KeyCode::Unknown;

			// Where it came from.
			InputSource Source = InputSource::Keyboard;
		};

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

		// Pushes one report as an `InputObject`.
		void PushInputObject(lua_State *state, const InputReport &report) {
			void *memory = lua_newuserdatatagged(state, sizeof(InputReport), TAG_INPUT_OBJECT);
			new (memory) InputReport(report);
			luaL_getmetatable(state, INPUT_OBJECT_TYPE);
			lua_setmetatable(state, -2);
		}

		// Installs the `InputObject` metatable.
		//
		// **Read-only and unreachable**: there is no `__newindex`, so a handler
		// cannot edit the report it was given and hand it on, and `__metatable`
		// is set for `Values.cpp`'s reason — a metatable a script can reach is one
		// it can rewrite, and then every `InputObject` in the world changes
		// underneath everything holding one.
		void OpenInputObject(lua_State *state) {
			luaL_newmetatable(state, INPUT_OBJECT_TYPE);

			lua_pushcfunction(state, InputObjectIndex, "__index");
			lua_setfield(state, -2, "__index");

			// What `typeof` actually reads. Luau's `typeof` is a fastcall
			// builtin that returns this field rather than consulting a global —
			// see `Values.cpp`'s `Install`.
			lua_pushstring(state, INPUT_OBJECT_TYPE);
			lua_setfield(state, -2, "__type");

			lua_pushstring(state, INPUT_OBJECT_TYPE);
			lua_setfield(state, -2, "__metatable");

			lua_pop(state, 1);
		}

		// A report for one key edge. Position and delta are zero, as Roblox's are:
		// a keyboard event has no place on the screen.
		InputReport KeyReport(KeyCode key, bool began) {
			InputReport report;
			report.Key = key;
			report.Source = InputSource::Keyboard;
			report.State = core::Name(began ? "Begin" : "End");
			return report;
		}

		// A report for one mouse button edge, or for one held button.
		//
		// **The delta is left at zero even though the pointer may have moved this
		// frame**, which is Roblox's shape: motion is reported by its own
		// `InputChanged`, and putting it on the click as well would have a handler
		// that sums deltas count the same movement twice.
		InputReport ButtonReport(const InputState &input, MouseButton button, bool began) {
			InputReport report;
			report.Source = static_cast<InputSource>(button);
			report.State = core::Name(began ? "Begin" : "End");
			report.Position = core::Vector3{input.MousePosition.X, input.MousePosition.Y, 0.0f};
			return report;
		}

		// A report for this frame's pointer motion.
		InputReport MotionReport(const InputState &input) {
			InputReport report;
			report.Source = InputSource::MouseMovement;
			report.State = core::Name("Change");
			report.Position = core::Vector3{input.MousePosition.X, input.MousePosition.Y, 0.0f};
			report.Delta = core::Vector3{input.MouseDelta.X, input.MouseDelta.Y, 0.0f};
			return report;
		}

		// A report for this frame's wheel movement.
		InputReport WheelReport(const InputState &input) {
			InputReport report;
			report.Source = InputSource::MouseWheel;
			report.State = core::Name("Change");
			report.Position = core::Vector3{input.MousePosition.X, input.MousePosition.Y, input.WheelDelta};
			report.Delta = core::Vector3{0.0f, 0.0f, input.WheelDelta};
			return report;
		}

		// --- UserInputService -------------------------------------------------

		// `UserInputService:IsKeyDown(Enum.KeyCode.Space)`
		//
		// **Takes an `EnumItem` or a string**, which is the same latitude a
		// property with `PropertyType::Enum` gives: `part.AlphaMode = "Clip"` is
		// what a migrating script already contains, and refusing it here would
		// make input the one surface that is stricter than the rest.
		int IsKeyDown(lua_State *state) {
			core::Name member;
			if (!ReadEnumValue(state, 2, core::Name("KeyCode"), member)) {
				luaL_errorL(state, "IsKeyDown expects an Enum.KeyCode");
			}

			const InputState *input = StateOf(state);
			lua_pushboolean(state, input != nullptr && input->IsKeyDown(scene::KeyFromName(member.Text())));
			return 1;
		}

		int IsMouseButtonPressed(lua_State *state) {
			core::Name member;
			if (!ReadEnumValue(state, 2, core::Name("UserInputType"), member)) {
				luaL_errorL(state, "IsMouseButtonPressed expects an Enum.UserInputType");
			}

			size_t ordinal = 0;
			const InputState *input = StateOf(state);

			// **`Enum.UserInputType` names three sources that are not buttons**
			// since `InputObject` needed to say where an event came from, and
			// "is `MouseMovement` pressed" is a question with no answer. False
			// rather than a cast past the end of the button bits, which would
			// have read whichever bit the arithmetic landed on.
			const bool known = ecs::EnumTable::OrdinalOf(core::Name("UserInputType"), member, ordinal) &&
							   ordinal < static_cast<size_t>(MouseButton::Count);

			lua_pushboolean(
				state, input != nullptr && known && input->IsButtonDown(static_cast<MouseButton>(ordinal))
			);
			return 1;
		}

		// `UserInputService:GetMouseLocation()` — a `Vector2` in pixels.
		int GetMouseLocation(lua_State *state) {
			const InputState *input = StateOf(state);
			*PushVector2(state) = input == nullptr ? core::Vector2{} : input->MousePosition;
			return 1;
		}

		// `UserInputService:GetMouseDelta()` — how far it moved this frame.
		//
		// **Roblox's spelling, and the value a locked pointer needs.** See
		// `InputState::MouseDelta`: under `LockCenter` the position does not
		// change and only this does.
		int GetMouseDelta(lua_State *state) {
			const InputState *input = StateOf(state);
			*PushVector2(state) = input == nullptr ? core::Vector2{} : input->MouseDelta;
			return 1;
		}

		// `UserInputService:GetKeysPressed()` — every key down now.
		//
		// **A list of `EnumItem`s rather than of strings**, so what comes out is
		// what `IsKeyDown` takes. A surface whose getter and setter disagree about
		// a type is the round trip a property owes and a service owes equally.
		int GetKeysPressed(lua_State *state) {
			const InputState *input = StateOf(state);

			lua_newtable(state);
			if (input == nullptr) {
				return 1;
			}

			int written = 0;
			for (size_t index = 0; index < static_cast<size_t>(KeyCode::Count); index++) {
				const auto key = static_cast<KeyCode>(index);
				if (!input->IsKeyDown(key)) {
					continue;
				}
				PushEnumItem(state, core::Name("KeyCode"), core::Name(scene::Describe(key)));
				lua_rawseti(state, -2, ++written);
			}
			return 1;
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
		int GetMouseButtonsPressed(lua_State *state) {
			const InputState *input = StateOf(state);

			lua_newtable(state);
			if (input == nullptr) {
				return 1;
			}

			int written = 0;
			for (size_t index = 0; index < static_cast<size_t>(MouseButton::Count); index++) {
				const auto button = static_cast<MouseButton>(index);
				if (!input->IsButtonDown(button)) {
					continue;
				}

				// `Begin` for a held button, which is what Roblox reports here:
				// the state of a button that is down is the one it went down in.
				PushInputObject(state, ButtonReport(*input, button, true));
				lua_rawseti(state, -2, ++written);
			}
			return 1;
		}

		// `UserInputService.MouseBehavior`, read and written.
		//
		// **The one field here that travels towards the client.** A script sets
		// it, the client applies it to the window on the next frame — which is why
		// `InputState` is the seam in both directions rather than a report.
		int InputServiceIndex(lua_State *state) {
			const std::string_view field = luaL_checkstring(state, 2);

			if (field == "MouseBehavior") {
				const InputState *input = StateOf(state);
				const auto behaviour = input == nullptr ? MouseBehavior::Default : input->Behaviour;
				PushEnumItem(
					state,
					core::Name("MouseBehavior"),
					ecs::EnumTable::MemberAt(core::Name("MouseBehavior"), static_cast<size_t>(behaviour))
				);
				return 1;
			}

			if (field == "MouseDeltaSensitivity") {
				// Read off the camera controller rather than kept twice. Roblox
				// puts it on `UserInputService` and the value it scales is the
				// camera's, so one number in one place with two names beats two
				// numbers that agree until somebody sets one.
				const auto *controller = WorldOf(state).Resource<scene::CameraController>();
				lua_pushnumber(state, controller == nullptr ? 1.0 : controller->Sensitivity / 0.0035);
				return 1;
			}

			if (field == "KeyboardEnabled" || field == "MouseEnabled") {
				// True on anything with a window and false headless, which is what
				// "is there a keyboard" actually asks. A world with no input state
				// is a world nobody is typing at.
				lua_pushboolean(state, StateOf(state) != nullptr);
				return 1;
			}

			if (field == "GamepadEnabled" || field == "TouchEnabled" || field == "VREnabled" ||
				field == "AccelerometerEnabled" || field == "GyroscopeEnabled") {
				// **Present and false, which is better than absent.** Roblox
				// scripts branch on these — `if UserInputService.TouchEnabled
				// then` is how a place picks its control scheme — and a missing
				// property raises where a false one takes the other branch. There
				// is no gamepad, touch, headset or sensor anywhere in
				// `input::Translator`, so the answer is a constant and saying so
				// is the honest version of not having one.
				lua_pushboolean(state, false);
				return 1;
			}

			if (field == "InputBegan" || field == "InputEnded" || field == "InputChanged" ||
				field == "WindowFocused" || field == "WindowFocusReleased") {
				// **One signal kind, filtered by name**, exactly as
				// `GetAttributeChangedSignal` reuses `PropertyChanged`. The five
				// are distinguished by which name the connection carries, and
				// `PumpInput` fires the one that matches.
				PushSignal(state, SignalKind::PropertyChanged, ecs::NULL_ENTITY, core::Name(field));
				return 1;
			}

			// The methods, from the shared table.
			//
			// **A userdata has no fields**, so `ServiceSurface` stashes the
			// method table in the registry under this key and this reads it
			// back. One constant, two readers, so the install and the lookup
			// cannot name different keys.
			lua_getfield(state, LUA_REGISTRYINDEX, INPUT_METHODS_KEY);
			lua_pushvalue(state, 2);
			lua_rawget(state, -2);
			if (!lua_isnil(state, -1)) {
				return 1;
			}

			luaL_errorL(state, "UserInputService has no member '%s'", std::string(field).c_str());
		}

		int InputServiceNewIndex(lua_State *state) {
			const std::string_view field = luaL_checkstring(state, 2);

			if (field == "MouseBehavior") {
				core::Name member;
				if (!ReadEnumValue(state, 3, core::Name("MouseBehavior"), member)) {
					luaL_errorL(state, "MouseBehavior expects an Enum.MouseBehavior");
				}

				size_t ordinal = 0;
				if (!ecs::EnumTable::OrdinalOf(core::Name("MouseBehavior"), member, ordinal)) {
					luaL_errorL(state, "unknown MouseBehavior");
				}

				ecs::Store &store = WorldOf(state);
				if (auto *input = store.ResourceMutable<InputState>()) {
					input->Behaviour = static_cast<MouseBehavior>(ordinal);
				}
				return 0;
			}

			if (field == "MouseDeltaSensitivity") {
				const auto scale = static_cast<float>(luaL_checknumber(state, 3));
				if (auto *controller = WorldOf(state).ResourceMutable<scene::CameraController>()) {
					controller->Sensitivity = 0.0035f * std::max(scale, 0.0f);
				}
				return 0;
			}

			luaL_errorL(state, "UserInputService.%s is read-only", std::string(field).c_str());
		}

		// --- ContextActionService ---------------------------------------------
		//
		// **What this adds over `UserInputService` is a priority stack**, and that
		// is the whole of it. Polling `IsKeyDown` is fine until two systems want E
		// — a door and a vehicle — and then the question is which one gets it. A
		// bound action is a claim on a key with a number attached, and the highest
		// claim wins.
		//
		// The stack lives on the VM rather than on the world, because a bound
		// action is a Luau function reference: it cannot cross a snapshot, cannot
		// be replicated, and dies with the runtime that registered it. A world
		// resource holding one would be a resource that cannot be serialised.

		// The stack, per VM.
		//
		// **On the context and never a `thread_local`**, which the first version
		// of this got wrong: two VMs on one thread would have shared the vector,
		// and a registry reference minted against one state would then be
		// dereferenced against the other. It crashed a MemoryStore test — a suite
		// with nothing to do with input — because that suite happened to build a
		// second runtime.
		std::vector<BoundAction> &ActionsOf(lua_State *state) {
			// `ContextOf` for `WorldOf`'s reason: `PumpInput` reaches this with no
			// C closure on the stack.
			return ContextOf(state).Actions;
		}

		// `ContextActionService:BindActionAtPriority(name, handler, touchButton, priority, ...keys)`
		//
		// The five-argument form is the one that says what it means; the
		// four-argument `BindAction` below is it with a priority of zero.
		int BindAtPriority(lua_State *state, int priorityIndex, int firstKey) {
			BoundAction action;
			action.Name = luaL_checkstring(state, 2);

			luaL_checktype(state, 3, LUA_TFUNCTION);
			lua_pushvalue(state, 3);
			action.Callback = lua_ref(state, -1);
			lua_pop(state, 1);

			action.Priority =
				priorityIndex > 0 ? static_cast<int>(luaL_checkinteger(state, priorityIndex)) : 0;

			for (int index = firstKey; index <= lua_gettop(state); index++) {
				core::Name member;
				if (!ReadEnumValue(state, index, core::Name("KeyCode"), member)) {
					continue;
				}
				const KeyCode key = scene::KeyFromName(member.Text());
				if (key != KeyCode::Unknown) {
					action.Keys.push_back(static_cast<uint16_t>(key));
				}
			}

			std::vector<BoundAction> &actions = ActionsOf(state);

			// **Rebinding a name replaces it rather than stacking a second.**
			// Roblox's behaviour, and the one that makes a script safe to run
			// twice: a reload that bound the same action again would otherwise
			// fire its handler twice per press, forever.
			const auto existing =
				std::find_if(actions.begin(), actions.end(), [&action](const BoundAction &bound) {
					return bound.Name == action.Name;
				});
			if (existing != actions.end()) {
				lua_unref(state, existing->Callback);
				*existing = std::move(action);
			} else {
				actions.push_back(std::move(action));
			}

			// **Sorted once here rather than searched per press.** A press walks
			// the list until something claims the key, so the order has to be the
			// priority order — and binding is rare where pressing is not.
			//
			// **Stable, so two actions at one priority fire in bind order.** That
			// is the only tie-break that is reproducible; an unstable sort would
			// make which one wins depend on the allocator.
			std::stable_sort(
				actions.begin(), actions.end(), [](const BoundAction &left, const BoundAction &right) {
					return left.Priority > right.Priority;
				}
			);
			return 0;
		}

		int BindAction(lua_State *state) {
			// `(self, name, handler, createTouchButton, ...keys)` — the touch
			// button is accepted and ignored, because there is no touch surface.
			// Accepted rather than refused so a Roblox script runs unchanged.
			return BindAtPriority(state, 0, 5);
		}

		int BindActionAtPriority(lua_State *state) {
			// `(self, name, handler, createTouchButton, priority, ...keys)`
			return BindAtPriority(state, 5, 6);
		}

		int UnbindAction(lua_State *state) {
			const std::string_view name = luaL_checkstring(state, 2);
			std::vector<BoundAction> &actions = ActionsOf(state);

			const auto found = std::find_if(actions.begin(), actions.end(), [name](const BoundAction &bound) {
				return bound.Name == name;
			});
			if (found != actions.end()) {
				lua_unref(state, found->Callback);
				actions.erase(found);
			}
			return 0;
		}

		int UnbindAllActions(lua_State *state) {
			std::vector<BoundAction> &actions = ActionsOf(state);
			for (const BoundAction &action : actions) {
				lua_unref(state, action.Callback);
			}
			actions.clear();
			return 0;
		}

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
			const std::vector<BoundAction> &actions = ActionsOf(state);

			const auto found = std::find_if(actions.begin(), actions.end(), [name](const BoundAction &bound) {
				return bound.Name == name;
			});
			if (found == actions.end()) {
				// **Nil for an unbound name, not an empty table.** Roblox's
				// answer, and the one `if info then` reads correctly — an empty
				// table is truthy and would report every name as bound.
				lua_pushnil(state);
				return 1;
			}

			PushActionInfo(state, *found, static_cast<size_t>(found - actions.begin()), actions.size());
			return 1;
		}

		// `ContextActionService:GetAllBoundActionInfo()` -> { [name]: table }
		//
		// Keyed by name rather than an array, which is Roblox's shape and the one
		// a caller wants: the question this answers is "what has claimed E", and
		// the name is how anything is unbound afterwards.
		int GetAllBoundActionInfo(lua_State *state) {
			const std::vector<BoundAction> &actions = ActionsOf(state);

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
				}
			);
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
		// entry is in, which is a change to `Bindings.hpp`'s `BoundAction` rather
		// than to this loop.
		void
		RunBoundActions(lua_State *state, KeyCode key, const InputReport &report, std::string &firstError) {
			for (const BoundAction &action : ActionsOf(state)) {
				const auto ordinal = static_cast<uint16_t>(key);
				if (std::find(action.Keys.begin(), action.Keys.end(), ordinal) == action.Keys.end()) {
					continue;
				}

				lua_getref(state, action.Callback);
				lua_pushstring(state, action.Name.c_str());
				PushEnumItem(state, core::Name("UserInputState"), report.State);

				// **An `InputObject` where this used to push an `Enum.KeyCode`.**
				// Roblox's third argument has always been an `InputObject`, the
				// generated declarations said `Enum_KeyCode`, and a handler copied
				// from a Roblox place read `input.KeyCode` off an `EnumItem` and
				// got nil. A behaviour change, and a stated one.
				PushInputObject(state, report);

				if (lua_pcall(state, 3, 0, 0) != LUA_OK) {
					if (firstError.empty()) {
						const char *message = lua_tostring(state, -1);
						firstError = message != nullptr ? message : "a bound action failed";
					}
					lua_pop(state, 1);
				}
				return;
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

	void OpenUserInputService(lua_State *state) {
		// **This one has properties, so `ServiceSurface` builds it as a userdata
		// rather than a table** — see `ServiceSurface::Index` for why that is
		// forced rather than stylistic, and `DEFERRED.md` D00030 for the sharp
		// edge that survives it: `GETIMPORT` caches a `Global.Field` chain
		// whether the intermediate is a table or a userdata, so a property is
		// live only when read through a local. Which is the form a Roblox script
		// uses anyway, since `game:GetService` is a method call and cannot be an
		// import:
		//
		//     local UIS = game:GetService("UserInputService")
		//     UIS.MouseBehavior = Enum.MouseBehavior.LockCenter
		static constexpr ServiceMethod METHODS[] = {
			{"IsKeyDown", IsKeyDown},
			{"IsMouseButtonPressed", IsMouseButtonPressed},
			{"GetMouseLocation", GetMouseLocation},
			{"GetMouseDelta", GetMouseDelta},
			{"GetKeysPressed", GetKeysPressed},
			{"GetMouseButtonsPressed", GetMouseButtonsPressed},
		};

		// **The datatype before the service that produces it**, so a metatable is
		// never looked up before it is registered. Here rather than beside the
		// other value types in `Values.cpp` because this is the only file that
		// makes one — an `InputObject` has no constructor and cannot arrive from
		// anywhere else.
		OpenInputObject(state);

		ServiceSurface surface;
		surface.Name = "UserInputService";
		surface.Methods = METHODS;
		surface.Index = InputServiceIndex;
		surface.NewIndex = InputServiceNewIndex;
		surface.Tag = TAG_INPUT_SERVICE;

		// The same key `InputServiceIndex` reads its method table back from.
		surface.MethodsKey = INPUT_METHODS_KEY;

		InstallService(state, surface);
	}

	void OpenContextActionService(lua_State *state) {
		// A plain table, because this one is **methods only** — and a method is
		// exactly the case `GETIMPORT` is correct for: the closure never
		// changes, so caching it is what the optimisation is for. Only a mutable
		// *property* is broken by it.
		static constexpr ServiceMethod METHODS[] = {
			{"BindAction", BindAction},
			{"BindActionAtPriority", BindActionAtPriority},
			{"UnbindAction", UnbindAction},
			{"UnbindAllActions", UnbindAllActions},
			{"GetBoundActionInfo", GetBoundActionInfo},
			{"GetAllBoundActionInfo", GetAllBoundActionInfo},
		};

		ServiceSurface surface;
		surface.Name = "ContextActionService";
		surface.Methods = METHODS;

		InstallService(state, surface);
	}
}
