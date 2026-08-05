#include "Bindings.hpp"

#include <lualib.h>
#include <string_view>

namespace engine::script {

	namespace {
		// `RunService:IsServer()` / `IsClient()` / `IsStudio()`
		//
		// **A script needs to be able to ask before it tries.**
		// `Store::SetProperty` already refuses a write on an adopt-only store
		// and says why, but a refusal is an error a script has to catch — and
		// the whole point of a client-side script is that it knows it is one.
		// These are the question that makes the refusal avoidable.
		int IsServer(lua_State *state) {
			lua_pushboolean(state, UpvalueContext(state).Role.Server);
			return 1;
		}

		int IsClient(lua_State *state) {
			lua_pushboolean(state, UpvalueContext(state).Role.Client);
			return 1;
		}

		int IsStudio(lua_State *state) {
			lua_pushboolean(state, UpvalueContext(state).Role.Studio);
			return 1;
		}

		// `RunService:IsReplica()` — whether this world's rows belong to somebody
		// else.
		//
		// **Not Roblox's, and it is the more precise question.** `IsServer()`
		// is about the *host*; this is about the *world*, and a single-player
		// process is a server whose client-side world is still a replica. A
		// script that guarded a write with `IsServer()` alone would be right on
		// a dedicated server and wrong in single player, which is the worst
		// place for a guard to be wrong.
		int IsReplica(lua_State *state) {
			lua_pushboolean(state, UpvalueContext(state).World->AdoptOnly());
			return 1;
		}

		// `game:GetService("RunService")`.
		//
		// The service locator every Roblox script opens with. Looked up as a
		// global rather than from a table of its own, so
		// `game:GetService("RunService")` and `RunService` are one object — two
		// objects for one service is two things to keep in step, and a script
		// comparing them would find them different.
		int GetService(lua_State *state) {
			const char *name = luaL_checkstring(state, 2);

			// **`Workspace` before the globals, because its global is spelled
			// differently.** Roblox's is `workspace`, lowercase, and a script
			// asking for it by its class name is asking for the same object —
			// which it did not get, because `lua_getglobal("Workspace")` finds
			// nothing and this refused a service the engine plainly provides.
			//
			// From the registry, so this and `game.Workspace` and the global
			// are one instance rather than three handles that compare equal.
			if (std::string_view(name) == "Workspace") {
				lua_getfield(state, LUA_REGISTRYINDEX, "engine.workspace");
				return 1;
			}

			lua_getglobal(state, name);
			if (!lua_isnil(state, -1)) {
				return 1;
			}
			lua_pop(state, 1);

			// **Then the tree, because that is where a scene service lives.**
			// `Players`, `ReplicatedStorage` and the rest are ordinary instances
			// `InstallServices` puts at the root — so looking them up by name is
			// looking them up the way everything else in the world is looked up.
			//
			// A global would have been a second handle onto one instance, and a
			// script comparing `game:GetService("Players")` with
			// `workspace.Parent.Players` would have found them different.
			const ecs::Entity service = UpvalueContext(state).World->FindFirstRoot(name);
			if (service != ecs::NULL_ENTITY) {
				PushInstanceValue(state, service);
				return 1;
			}

			luaL_errorL(state, "'%s' is not a service this engine provides", name);
		}

		// `game.Workspace` and `game:GetService("Workspace")`.
		int GameIndex(lua_State *state) {
			const std::string_view field = luaL_checkstring(state, 2);

			// `game.Workspace` is the world this script runs on, which is the
			// mapping `Bindings.hpp` states: game is the universe, workspace is
			// the world.
			if (field == "Workspace") {
				lua_getfield(state, LUA_REGISTRYINDEX, "engine.workspace");
				return 1;
			}

			if (field == "GetService") {
				// **The context is forwarded, not re-derived.** `GetService`
				// reaches the world to resolve a service instance from the
				// tree, and a plain `lua_pushcfunction` would give it no
				// upvalue to read one from — which is a garbage pointer rather
				// than a compile error.
				lua_pushvalue(state, lua_upvalueindex(1));
				lua_pushcclosure(state, GetService, "GetService", 1);
				return 1;
			}

			luaL_errorL(state, "game has no member '%s'", std::string(field).c_str());
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
		// What it carries is `GetService` and `Workspace`. Reaching *another*
		// world is deliberately not here and never will be as a property:
		// rule 3 says nothing crossing a world boundary is a pointer, so the
		// route out is `MessagingService` and the other bus services, which
		// carry copies.
		lua_newtable(state);

		lua_newtable(state);
		lua_pushlightuserdata(state, &ContextOf(state));
		lua_pushcclosure(state, GameIndex, "__index", 1);
		lua_setfield(state, -2, "__index");
		lua_pushstring(state, "DataModel");
		lua_setfield(state, -2, "__metatable");
		lua_setmetatable(state, -2);

		lua_setglobal(state, "game");
	}

	void OpenRunService(lua_State *state) {
		LuauContext &context = ContextOf(state);

		// `Heartbeat` is a real signal now rather than a list of its own, so
		// `:Connect` hands back an `RBXScriptConnection` a script can
		// `:Disconnect` — which is the thing v0.5 said was worse to fake than to
		// omit.
		lua_newtable(state);
		PushSignal(state, SignalKind::Heartbeat, ecs::NULL_ENTITY);
		lua_setfield(state, -2, "Heartbeat");

		static const struct {
			const char *Name;
			lua_CFunction Function;
		} PREDICATES[] = {
			{"IsServer", IsServer},
			{"IsClient", IsClient},
			{"IsStudio", IsStudio},
			{"IsReplica", IsReplica},
		};

		for (const auto &entry : PREDICATES) {
			lua_pushlightuserdata(state, &context);
			lua_pushcclosure(state, entry.Function, entry.Name, 1);
			lua_setfield(state, -2, entry.Name);
		}

		lua_setglobal(state, "RunService");
	}

	std::string PumpHeartbeat(lua_State *state, float delta) {
		lua_pushnumber(state, delta);
		return FireSignal(state, SignalKind::Heartbeat, ecs::NULL_ENTITY, 1);
	}
}
