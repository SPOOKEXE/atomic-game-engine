#include "Bindings.hpp"

#include <engine/ecs/EnumTable.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Controls.hpp>
#include <engine/scene/Input.hpp>

#include <algorithm>
#include <lualib.h>
#include <string>
#include <string_view>
#include <vector>

namespace engine::script {

	namespace {
		using scene::InputState;
		using scene::KeyCode;
		using scene::MouseBehavior;
		using scene::MouseButton;

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

		// --- UserInputService -------------------------------------------------

		// `UserInputService:IsKeyDown(Enum.KeyCode.Space)`
		//
		// **Takes an `EnumItem` or a string**, which is the same latitude a
		// property with `PropertyType::Enum` gives: `part.Material = "Plastic"` is
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
			const bool known = ecs::EnumTable::OrdinalOf(core::Name("UserInputType"), member, ordinal);

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

			if (field == "InputBegan" || field == "InputEnded" || field == "InputChanged") {
				// **One signal kind, filtered by name**, exactly as
				// `GetAttributeChangedSignal` reuses `PropertyChanged`. The three
				// are distinguished by which name the connection carries, and
				// `PumpInput` fires the one that matches.
				PushSignal(state, SignalKind::PropertyChanged, ecs::NULL_ENTITY, core::Name(field));
				return 1;
			}

			// The methods, from the shared table.
			lua_getfield(state, LUA_REGISTRYINDEX, "engine.userinput.methods");
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
	}

	// --- the pump -------------------------------------------------------------

	std::string PumpInput(lua_State *state) {
		const InputState *input = StateOf(state);
		if (input == nullptr) {
			return {};
		}

		std::string firstError;

		// **Edges only.** A held key fires once, which is what a bound action
		// means — `Enum.UserInputState.Begin` and `.End` are the two calls a
		// handler gets, and a third every frame would make every action a
		// repeat-rate question.
		for (size_t index = 0; index < static_cast<size_t>(KeyCode::Count); index++) {
			const auto key = static_cast<KeyCode>(index);
			const bool began = input->WasKeyPressed(key);
			const bool ended = input->WasKeyReleased(key);
			if (!began && !ended) {
				continue;
			}

			// **The first action claiming this key wins and the rest never see
			// it**, which is the whole reason `ContextActionService` exists beside
			// polling. `Enum.ContextActionResult.Pass` would let a handler decline
			// — Roblox has it and this does not, because a handler that returns
			// nothing should sink the input and returning `Pass` is the rarer case
			// worth adding when somebody needs it.
			for (const BoundAction &action : ActionsOf(state)) {
				const auto ordinal = static_cast<uint16_t>(key);
				if (std::find(action.Keys.begin(), action.Keys.end(), ordinal) == action.Keys.end()) {
					continue;
				}

				lua_getref(state, action.Callback);
				lua_pushstring(state, action.Name.c_str());
				PushEnumItem(state, core::Name("UserInputState"), core::Name(began ? "Begin" : "End"));
				PushEnumItem(state, core::Name("KeyCode"), core::Name(scene::Describe(key)));

				if (lua_pcall(state, 3, 0, 0) != LUA_OK) {
					if (firstError.empty()) {
						const char *message = lua_tostring(state, -1);
						firstError = message != nullptr ? message : "a bound action failed";
					}
					lua_pop(state, 1);
				}
				break;
			}

			// The service's own signals, after the bound actions. **Not gated by
			// them**: `InputBegan` is a report of what happened and a claim on a
			// key does not stop it having happened, which is Roblox's split and
			// the one that lets a debug overlay watch every key regardless.
			LuauContext &live = ContextOf(state);
			const core::Name signal(began ? "InputBegan" : "InputEnded");

			live.Signals.Fire(
				SignalKind::PropertyChanged, ecs::NULL_ENTITY, [&](const Connection &connection) {
					if (connection.Property != signal) {
						return;
					}

					lua_getref(state, connection.Callback);
					PushEnumItem(state, core::Name("KeyCode"), core::Name(scene::Describe(key)));
					if (lua_pcall(state, 1, 0, 0) != LUA_OK) {
						if (firstError.empty()) {
							const char *message = lua_tostring(state, -1);
							firstError = message != nullptr ? message : "an input listener failed";
						}
						lua_pop(state, 1);
					}
				}
			);
		}

		return firstError;
	}

	void OpenInputServices(lua_State *state) {
		LuauContext &context = ContextOf(state);

		static const struct {
			const char *Name;
			lua_CFunction Function;
		} INPUT_METHODS[] = {
			{"IsKeyDown", IsKeyDown},
			{"IsMouseButtonPressed", IsMouseButtonPressed},
			{"GetMouseLocation", GetMouseLocation},
			{"GetMouseDelta", GetMouseDelta},
			{"GetKeysPressed", GetKeysPressed},
		};

		lua_newtable(state);
		for (const auto &method : INPUT_METHODS) {
			lua_pushlightuserdata(state, &context);
			lua_pushcclosure(state, method.Function, method.Name, 1);
			lua_setfield(state, -2, method.Name);
		}
		lua_setfield(state, LUA_REGISTRYINDEX, "engine.userinput.methods");

		// **`UserInputService` is a userdata and not a table, and that is forced
		// rather than stylistic.**
		//
		// It has *properties* as well as methods, so it needs a metatable — and a
		// table with a metatable does not work here. `luaL_sandbox` freezes the
		// global table and enables Luau's `safeenv`, which lets the compiler turn
		// `UserInputService.MouseBehavior` into a `GETIMPORT`: a constant global
		// and a constant field, resolved **once** and cached in the closure. The
		// first read wins forever, so a property that changes reads as one that
		// does not.
		//
		// That was found by watching `__index` fire for the first read of
		// `MouseBehavior` and not for the second, with no raw key on the table to
		// explain it. A userdata's field access is never an import, so every read
		// goes through `__index`.
		//
		// Zero bytes of payload: what the object *is* is its metatable. `Instance`
		// carries an `ecs::Entity` for the same reason it needs one, and this
		// needs nothing — the world is on the context.
		//
		// **The userdata is necessary and not sufficient, which is worth stating
		// because it looks like a fix and is half of one.** `GETIMPORT` caches the
		// resolved *value* of a `Global.Field` chain, and it does so whether the
		// intermediate is a table or a userdata — so `UserInputService.MouseBehavior`
		// written as a bare global still reads once and never again. What the
		// userdata buys is that every read through a *local* goes to `__index`,
		// which is the form a Roblox script is written in anyway:
		//
		//     local UIS = game:GetService("UserInputService")
		//     UIS.MouseBehavior = Enum.MouseBehavior.LockCenter
		//
		// `game:GetService` is a method call and cannot be an import, so binding
		// the service to a local is what makes the property live. `DEFERRED.md`
		// D00030 records the sharp edge and what closing it would take.
		lua_newuserdatatagged(state, 1, TAG_INPUT_SERVICE);
		lua_newtable(state);

		lua_pushlightuserdata(state, &context);
		lua_pushcclosure(state, InputServiceIndex, "__index", 1);
		lua_setfield(state, -2, "__index");

		lua_pushlightuserdata(state, &context);
		lua_pushcclosure(state, InputServiceNewIndex, "__newindex", 1);
		lua_setfield(state, -2, "__newindex");

		lua_pushstring(state, "UserInputService");
		lua_setfield(state, -2, "__metatable");

		lua_setmetatable(state, -2);
		lua_setglobal(state, "UserInputService");

		// `ContextActionService` stays a plain table, because it is **methods
		// only** — and a method is exactly the case `GETIMPORT` is correct for:
		// the closure never changes, so caching it is what the optimisation is
		// for. Only a mutable *property* is broken by it.
		static const struct {
			const char *Name;
			lua_CFunction Function;
		} ACTION_METHODS[] = {
			{"BindAction", BindAction},
			{"BindActionAtPriority", BindActionAtPriority},
			{"UnbindAction", UnbindAction},
			{"UnbindAllActions", UnbindAllActions},
		};

		lua_newtable(state);
		for (const auto &method : ACTION_METHODS) {
			lua_pushlightuserdata(state, &context);
			lua_pushcclosure(state, method.Function, method.Name, 1);
			lua_setfield(state, -2, method.Name);
		}
		lua_setglobal(state, "ContextActionService");
	}
}
