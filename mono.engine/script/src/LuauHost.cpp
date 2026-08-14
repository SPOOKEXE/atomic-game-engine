#include "LuauBindings.hpp"

#include <engine/core/Log.hpp>

#include <lualib.h>
#include <string_view>

namespace engine::script {

	namespace {
		// --- the clock -------------------------------------------------------
		//
		// **Every one of these reads `store.Time()`, and not one reads a wall
		// clock.** A script branching on real time produces a run that does not
		// replay, and `just replay-check` would fail somewhere a long way from
		// the script that caused it. `os` is refused outright for the same
		// reason - see `LuauRuntime.cpp` - so these are what a script gets
		// instead, under names Roblox already uses for the same idea.

		// `time()` - simulated seconds since the world started.
		int Time(lua_State *state) {
			lua_pushnumber(state, UpvalueContext(state).World->Time().Elapsed);
			return 1;
		}

		// `elapsedTime()` - the same number.
		//
		// Roblox distinguishes `time()` (since the game started) from
		// `elapsedTime()` (since the client started), and here there is one
		// clock and one world. Two names for one number rather than a second
		// number nobody could define: a script that used either gets the answer
		// it expected, and neither name promises something the engine does not
		// have.
		int ElapsedTime(lua_State *state) {
			lua_pushnumber(state, UpvalueContext(state).World->Time().Elapsed);
			return 1;
		}

		// `tick()` - the tick count, not a Unix timestamp.
		//
		// **Roblox's `tick()` returns wall-clock seconds since the epoch and
		// this one does not**, which is a deliberate break rather than an
		// oversight. The name is exactly right for what this engine counts, and
		// a `tick()` that returned a real timestamp would be the single most
		// effective way to make a recording stop replaying - an author would
		// use it for seeding, for elapsed measurement and for save timestamps
		// without ever suspecting it.
		int Tick(lua_State *state) {
			lua_pushnumber(state, static_cast<double>(UpvalueContext(state).World->Time().Tick));
			return 1;
		}

		// `DateTime.now()`, which does not exist.
		//
		// A refusal naming its replacements, for the reason `wait` gets one:
		// an absence reads as "this engine forgot", and this reads as "this
		// engine will not read your machine's clock, and here is what to use".
		int DateTimeNow(lua_State *state) {
			luaL_errorL(
				state,
				"DateTime.now() does not exist here: a world's clock is simulated, and a script "
				"branching on wall time produces a run that does not replay. Use "
				"DateTime.fromSimulated() for this world's clock, or DateTime.fromUnixTimestamp(n) "
				"for a timestamp something else gave you"
			);
		}

		void PushDateTime(lua_State *state, double seconds) {
			lua_newtable(state);
			lua_pushnumber(state, seconds);
			lua_setfield(state, -2, "UnixTimestamp");
			lua_pushnumber(state, seconds * 1000.0);
			lua_setfield(state, -2, "UnixTimestampMillis");
		}

		// `DateTime.fromSimulated()` - this world's clock, named honestly.
		int DateTimeFromSimulated(lua_State *state) {
			PushDateTime(state, UpvalueContext(state).World->Time().Elapsed);
			return 1;
		}

		// `DateTime.fromUnixTimestamp(n)` - a timestamp something else supplied.
		//
		// A value type over a number and nothing more. A script that received a
		// real timestamp over a bus is holding data, not reading a clock, and
		// refusing to let it name that data would be refusing the wrong thing.
		int DateTimeFromUnixTimestamp(lua_State *state) {
			PushDateTime(state, luaL_checknumber(state, 1));
			return 1;
		}

		// --- warn ------------------------------------------------------------
		//
		// **`typeof` is not here, and that is a correction rather than an
		// omission.** It was bound as a global first, and the global was never
		// called: Luau's `typeof` is a *fastcall builtin*, so the compiler emits
		// an opcode that reaches `luaB_typeof` directly and never looks a global
		// up - and freezing the globals with `luaL_sandbox` is exactly what
		// licenses that optimisation.
		//
		// The mechanism Luau actually offers is the **`__type` metafield**,
		// which `luaB_typeof` reads, and it is the better answer anyway: one
		// string on each metatable beside the type's own name, rather than a
		// table of tags in a third file that has to be kept in step with them.
		// Every value type this module installs sets it.

		// `warn(...)` - `print`, at a level that stands out.
		int Warn(lua_State *state) {
			std::string line;
			const int count = lua_gettop(state);

			for (int index = 1; index <= count; index++) {
				if (index > 1) {
					line += '\t';
				}

				size_t length = 0;
				const char *text = luaL_tolstring(state, index, &length);
				line.append(text, length);
				lua_pop(state, 1);
			}

			ENGINE_WARN("[script] {}", line);
			return 0;
		}

		// The base-library gaps, present only to say why.
		//
		// **Each of these is a way to assemble or reach code no manifest
		// describes**, which is the test `script/AGENTS.md` states: the question
		// is what a library lets a script observe or do that a recording cannot
		// reproduce and a review cannot see.
		//
		// - `loadstring` compiles text at run time. A game that loaded a string
		//   off a bus would be running code the bindings manifest never saw.
		// - `getfenv`/`setfenv` rewrite the environment a function sees, which
		//   is how a sandboxed script reaches the one that sandboxed it.
		// - `require` reaches another module by path. Module scripts are a real
		//   feature and they arrive through the instance tree - see v0.7 - not
		//   through a file system a game can name.
		int RefuseLoadstring(lua_State *state) {
			luaL_errorL(
				state,
				"loadstring does not exist here: it would run code no bindings manifest describes, "
				"and nothing could review or replay it"
			);
		}

		int RefuseGetfenv(lua_State *state) {
			luaL_errorL(
				state,
				"getfenv and setfenv do not exist here: rewriting a function's environment is how a "
				"sandboxed script reaches the one that sandboxed it"
			);
		}

		int RefuseRequire(lua_State *state) {
			luaL_errorL(
				state,
				"require does not exist here yet: a module is reached through the instance tree "
				"rather than through a path, and ModuleScript arrives at v0.7"
			);
		}
	}

	void OpenClock(lua_State *state) {
		LuauContext &context = ContextOf(state);

		static const struct {
			const char *Name;
			lua_CFunction Function;
		} GLOBALS[] = {
			{"time", Time},
			{"elapsedTime", ElapsedTime},
			{"tick", Tick},
		};

		for (const auto &entry : GLOBALS) {
			lua_pushlightuserdata(state, &context);
			lua_pushcclosure(state, entry.Function, entry.Name, 1);
			lua_setglobal(state, entry.Name);
		}

		lua_newtable(state);
		lua_pushcfunction(state, DateTimeNow, "now");
		lua_setfield(state, -2, "now");
		lua_pushlightuserdata(state, &context);
		lua_pushcclosure(state, DateTimeFromSimulated, "fromSimulated", 1);
		lua_setfield(state, -2, "fromSimulated");
		lua_pushcfunction(state, DateTimeFromUnixTimestamp, "fromUnixTimestamp");
		lua_setfield(state, -2, "fromUnixTimestamp");
		lua_setglobal(state, "DateTime");
	}

	void OpenBaseExtras(lua_State *state) {
		lua_pushcfunction(state, Warn, "warn");
		lua_setglobal(state, "warn");

		// Overwritten rather than left absent, so the error says what to do.
		lua_pushcfunction(state, RefuseLoadstring, "loadstring");
		lua_setglobal(state, "loadstring");
		lua_pushcfunction(state, RefuseGetfenv, "getfenv");
		lua_setglobal(state, "getfenv");
		lua_pushcfunction(state, RefuseGetfenv, "setfenv");
		lua_setglobal(state, "setfenv");
		lua_pushcfunction(state, RefuseRequire, "require");
		lua_setglobal(state, "require");
	}
}
