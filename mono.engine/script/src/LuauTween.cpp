// The `Tween` handle, in Luau.
//
// **The handle and never the service.** `TweenService.cpp` describes `GetValue`
// and `Create` once and both VMs install them; what is here is the object
// `Create` answers with - a tagged userdata carrying the tween's entity, a
// metatable, and the three methods on it.
//
// **Those three are the one place this module deliberately did not use
// `ScriptCall`.** The neutral instance methods are installed flat on *every*
// instance, and `Play` is a name Roblox puts on three classes - claiming it
// there would take it from every part, sound and animation in the engine. Three
// small methods written twice is the cheaper of the two, and it is what
// `RBXScriptConnection` already pays. `Tweens.hpp` carries the whole argument.
//
// @tier L9 · shared

#include "LuauBindings.hpp"

#include <lua.h>
#include <lualib.h>
#include <string>
#include <string_view>
#include <vector>

namespace engine::script {

	namespace {
		using ecs::Entity;

		// A `Tween` argument, as the entity that names it.
		Entity CheckTween(lua_State *state, int index) {
			void *value = lua_touserdatatagged(state, index, TAG_TWEEN);
			if (value == nullptr) {
				luaL_typeerrorL(state, index, "Tween");
			}
			return *static_cast<const Entity *>(value);
		}

		// Drops a tween nothing holds any more: its connections, then its row.
		//
		// **The callables are released here rather than by `TweenTable`**,
		// because only a VM knows what a `CallbackRef` means - the same split
		// `SignalTable::DropSubject` is on, and the same one
		// `ScriptCall::ForgetSubject` puts on the neutral interface for
		// `TweenService:Create`'s benefit.
		void ReleaseTween(lua_State *state, LuauContext &context, Entity tween) {
			std::vector<CallbackRef> released;
			context.Signals.DropSubject(tween, released);
			for (const CallbackRef reference : released) {
				lua_unref(state, reference);
			}
			context.World->Destroy(tween);
		}

		// `tween:Play()`
		//
		// **Answers whether it started**, where Roblox answers nothing. A tween
		// whose target has been destroyed is the one case that can fail, it is an
		// ordinary thing for it to have happened, and a caller that ignores the
		// answer reads exactly as Roblox's does.
		int Play(lua_State *state) {
			LuauContext &context = UpvalueContext(state);
			lua_pushboolean(state, context.Tweens.Play(*context.World, CheckTween(state, 1)) ? 1 : 0);
			return 1;
		}

		// `tween:Pause()`
		int Pause(lua_State *state) {
			LuauContext &context = UpvalueContext(state);
			lua_pushboolean(state, context.Tweens.Pause(CheckTween(state, 1)) ? 1 : 0);
			return 1;
		}

		// `tween:Cancel()`
		int Cancel(lua_State *state) {
			LuauContext &context = UpvalueContext(state);
			lua_pushboolean(state, context.Tweens.Cancel(CheckTween(state, 1)) ? 1 : 0);
			return 1;
		}

		// `tween.Completed`, and the three methods.
		//
		// **A branch for the signal and a table for the rest**, which is the
		// shape `InstanceIndex` uses one door along and for the same reason: a
		// signal is not a member of any table, so it exists only as a string
		// comparison.
		int TweenIndex(lua_State *state) {
			const Entity tween = CheckTween(state, 1);
			const std::string_view field(luaL_checkstring(state, 2));

			if (field == "Completed") {
				PushSignal(state, SignalKind::TweenCompleted, tween);
				return 1;
			}

			lua_getfield(state, LUA_REGISTRYINDEX, "engine.tween.methods");
			lua_pushvalue(state, 2);
			lua_rawget(state, -2);
			return 1;
		}

		int TweenToString(lua_State *state) {
			(void)CheckTween(state, 1);
			lua_pushstring(state, "Tween");
			return 1;
		}

		// Two handles to one tween are equal, which they would not be by
		// identity: `PushTween` builds a fresh userdata per call, exactly as
		// `PushInstanceValue` does.
		int TweenEqual(lua_State *state) {
			void *left = lua_touserdatatagged(state, 1, TAG_TWEEN);
			void *right = lua_touserdatatagged(state, 2, TAG_TWEEN);
			lua_pushboolean(
				state,
				left != nullptr && right != nullptr &&
					*static_cast<Entity *>(left) == *static_cast<Entity *>(right)
			);
			return 1;
		}
	}

	void PushTween(lua_State *state, ecs::Entity tween) {
		void *memory = lua_newuserdatatagged(state, sizeof(ecs::Entity), TAG_TWEEN);
		*static_cast<ecs::Entity *>(memory) = tween;

		luaL_getmetatable(state, "Tween");
		lua_setmetatable(state, -2);
	}

	void OpenTweenHandle(lua_State *state) {
		LuauContext &context = ContextOf(state);

		// The method table, in the registry so `__index` hands a closure back
		// rather than building one per access - `LuauInstances.cpp` does the same
		// with `engine.instance.methods`, and for the same reason.
		static constexpr LuauServiceMethod TWEEN_METHODS[] = {
			{"Play", Play},
			{"Pause", Pause},
			{"Cancel", Cancel},
		};

		lua_newtable(state);
		for (const LuauServiceMethod &method : TWEEN_METHODS) {
			lua_pushlightuserdata(state, &context);
			lua_pushcclosure(state, method.Function, method.Name, 1);
			lua_setfield(state, -2, method.Name);
		}
		lua_setfield(state, LUA_REGISTRYINDEX, "engine.tween.methods");

		luaL_newmetatable(state, "Tween");
		lua_pushcfunction(state, TweenIndex, "__index");
		lua_setfield(state, -2, "__index");
		lua_pushcfunction(state, TweenToString, "__tostring");
		lua_setfield(state, -2, "__tostring");
		lua_pushcfunction(state, TweenEqual, "__eq");
		lua_setfield(state, -2, "__eq");

		// Locked, like every other value type's: a script that could replace
		// `__index` here would replace it for every tween in the world.
		lua_pushstring(state, "Tween");
		lua_setfield(state, -2, "__metatable");
		lua_pushstring(state, "Tween");
		lua_setfield(state, -2, "__type");
		lua_pop(state, 1);
	}

	std::string PumpTweens(lua_State *state, float delta) {
		LuauContext &context = ContextOf(state);

		// **Collected and then fired**, rather than fired from inside the walk:
		// a `Completed` handler may cancel the tween it was told about or start
		// another, and `TweenTable::Advance` is walking the list that names it.
		std::vector<ecs::Entity> completed;
		std::vector<ecs::Entity> dropped;
		context.Tweens.Advance(*context.World, delta, completed, dropped);

		std::string firstError;
		for (const ecs::Entity tween : completed) {
			// Every handler runs even when one raises, and the first error is
			// what the host hears about - `FireSignal`'s own rule, one level up.
			const std::string failed = FireSignal(state, SignalKind::TweenCompleted, tween, 0);
			if (firstError.empty()) {
				firstError = failed;
			}
		}

		// After the signals, so a tween dropped because its target died still
		// had whatever it was going to do this tick - and so a handler cannot be
		// called on a subject whose connections have already been released.
		for (const ecs::Entity tween : dropped) {
			ReleaseTween(state, context, tween);
		}
		return firstError;
	}
}
