#include "Bindings.hpp"

#include <lualib.h>
#include <vector>

namespace engine::script {

	namespace {
		// The registry key the connection list lives under.
		//
		// A table in the registry rather than a C++ vector of `lua_ref`s,
		// because the registry is what keeps a function from being collected
		// and a second list would be a second thing to keep in step with it.
		constexpr const char *CONNECTIONS = "engine.heartbeat.connections";

		int Connect(lua_State *state) {
			// `signal:Connect(fn)` — the colon call, so the signal itself
			// arrives as the first argument. Roblox's spelling, and a script
			// author who writes `.Connect(fn)` gets a type error naming the
			// function rather than silently connecting the signal.
			luaL_checktype(state, 2, LUA_TFUNCTION);

			lua_getfield(state, LUA_REGISTRYINDEX, CONNECTIONS);
			const int count = lua_objlen(state, -1);

			lua_pushvalue(state, 2);
			lua_rawseti(state, -2, count + 1);
			lua_pop(state, 1);

			// Roblox returns an `RBXScriptConnection` with `:Disconnect()`.
			// Returning nothing is the honest subset: a handle whose only
			// method did not exist would be worse than no handle, and a scene
			// script has nothing to disconnect from yet.
			return 0;
		}
	}

	namespace {
		// `game:GetService("RunService")`.
		//
		// The service locator every Roblox script opens with. One service, and
		// it returns the same table the `RunService` global holds rather than a
		// second object — two objects for one service is two things to keep in
		// step, and a script comparing them would find them different.
		int GetService(lua_State *state) {
			const char *name = luaL_checkstring(state, 2);

			// Looked up as a global rather than from a table of its own, so
			// `game:GetService("RunService")` and `RunService` are one object.
			// Two objects for one service is two things to keep in step, and a
			// script comparing them would find them different.
			lua_getglobal(state, name);
			if (lua_isnil(state, -1)) {
				luaL_errorL(state, "'%s' is not a service this engine provides", name);
			}
			return 1;
		}
	}

	void OpenGame(lua_State *state) {
		// **`game` is the universe.**
		//
		// Roblox's `game` is a `DataModel`: the root holding every service and,
		// through `Workspace`, everything with a position. This engine already
		// has that thing and calls it `world::Universe` — one process, many
		// worlds, buses between them. The mapping is direct rather than
		// approximate:
		//
		//     game      -> the universe
		//     workspace -> the world this script runs on
		//
		// What it carries today is `GetService`, and the omission is worth
		// stating: the universe's *own* surface — other worlds by name,
		// `Teleport`, the four buses — is v0.6's, and `docs/SCRIPT_CONCURRENCY.md`
		// settles the rules it has to arrive under first. A `game` that
		// pretended to reach another world would be the one promise this
		// engine most needs to keep.
		lua_newtable(state);
		lua_pushcfunction(state, GetService, "GetService");
		lua_setfield(state, -2, "GetService");
		lua_setglobal(state, "game");
	}

	void OpenRunService(lua_State *state) {
		lua_newtable(state);
		lua_setfield(state, LUA_REGISTRYINDEX, CONNECTIONS);

		// The signal object. `Heartbeat` is a table with a `Connect` method
		// rather than a function, because `signal:Connect(fn)` is what a Roblox
		// author's fingers already type.
		lua_newtable(state);
		lua_pushcfunction(state, Connect, "Connect");
		lua_setfield(state, -2, "Connect");

		lua_newtable(state);
		lua_pushvalue(state, -2);
		lua_setfield(state, -2, "Heartbeat");
		lua_setglobal(state, "RunService");

		lua_pop(state, 1);
	}

	std::string PumpHeartbeat(lua_State *state, float delta) {
		lua_getfield(state, LUA_REGISTRYINDEX, CONNECTIONS);
		const int count = lua_objlen(state, -1);

		std::string firstError;
		for (int index = 1; index <= count; index++) {
			lua_rawgeti(state, -1, index);
			lua_pushnumber(state, delta);

			// **Every connection runs even when one raises**, and the first
			// error is what the host hears about. A script that threw once
			// would otherwise silently stop everything registered after it, and
			// the symptom — half a scene animating — points nowhere near the
			// cause.
			if (lua_pcall(state, 1, 0, 0) != LUA_OK) {
				if (firstError.empty()) {
					const char *message = lua_tostring(state, -1);
					firstError = message != nullptr ? message : "a heartbeat connection failed";
				}
				lua_pop(state, 1);
			}
		}

		lua_pop(state, 1);
		return firstError;
	}
}
