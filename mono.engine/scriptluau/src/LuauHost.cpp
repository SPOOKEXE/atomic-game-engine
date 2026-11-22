#include "LuauBindings.hpp"

#include <engine/core/Clock.hpp>
#include <engine/core/Log.hpp>
#include <engine/script/DateTime.hpp>

#include <cmath>
#include <lualib.h>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace engine::script {

	namespace {
		// --- the clock -------------------------------------------------------
		//
		// **`time`, `elapsedTime` and `tick` read `store.Time()`**, the world's
		// simulated clock. The dates further down read `UnixNow`, which is the
		// world's real-time anchor plus that same clock, so none of them reads
		// the machine while a world runs and a run still replays.

		// `time()` - simulated seconds since the world started.
		int Time(lua_State *state) {
			lua_pushnumber(state, UpvalueContext(state).Clock.Elapsed(*UpvalueContext(state).World));
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
			lua_pushnumber(state, UpvalueContext(state).Clock.Elapsed(*UpvalueContext(state).World));
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

		// --- os and DateTime -------------------------------------------------
		//
		// **Real dates, read without reading the machine.** Every value below
		// comes from `UnixNow`, `ClockNow` and `UtcOffsetNow`: the world's
		// anchor plus simulated time, and one timer sample per tick. So a script
		// may branch on the date and the run still replays. `DateTime.hpp` has
		// the calendar.

		int64_t NowMillis(lua_State *state) {
			const LuauContext &context = ContextOf(state);
			return static_cast<int64_t>(std::llround(UnixNow(*context.World, context.Clock) * 1000.0));
		}

		int64_t LocalOffsetMillis(lua_State *state) {
			const LuauContext &context = ContextOf(state);
			return static_cast<int64_t>(UtcOffsetNow(*context.World, context.Clock)) * 1000;
		}

		void PushDateTime(lua_State *state, int64_t millis) {
			if (!datetime::InRange(millis)) {
				luaL_errorL(state, "DateTime is out of range: it holds the years 1400 through 9999");
			}
			void *memory = lua_newuserdatatagged(state, sizeof(int64_t), TAG_DATETIME);
			*static_cast<int64_t *>(memory) = millis;
			luaL_getmetatable(state, "DateTime");
			lua_setmetatable(state, -2);
		}

		int64_t CheckDateTime(lua_State *state, int index) {
			const void *value = lua_touserdatatagged(state, index, TAG_DATETIME);
			if (value == nullptr) luaL_typeerrorL(state, index, "DateTime");
			return *static_cast<const int64_t *>(value);
		}

		// The fields `fromUniversalTime` and `fromLocalTime` take, defaulting to
		// 1970-01-01T00:00:00.000.
		int64_t CivilArguments(lua_State *state) {
			return datetime::FromCivil(
				static_cast<int64_t>(luaL_optnumber(state, 1, 1970)),
				static_cast<int64_t>(luaL_optnumber(state, 2, 1)),
				static_cast<int64_t>(luaL_optnumber(state, 3, 1)),
				static_cast<int64_t>(luaL_optnumber(state, 4, 0)),
				static_cast<int64_t>(luaL_optnumber(state, 5, 0)),
				static_cast<int64_t>(luaL_optnumber(state, 6, 0)),
				static_cast<int64_t>(luaL_optnumber(state, 7, 0))
			);
		}

		// `DateTime.now()`, and `DateTime:GetUTCNow()` for the same value.
		int DateTimeNow(lua_State *state) {
			PushDateTime(state, NowMillis(state));
			return 1;
		}

		// A data package's `DateTime.now()`, which refuses. See `OpenClock`.
		int PackageDateTimeNow(lua_State *state) {
			luaL_errorL(
				state,
				"DateTime.now() does not exist in a data package: its output must depend on its seed "
				"and parameters, not on when it ran"
			);
		}

		// `DateTime.fromSimulated()` - this world's simulated seconds as a date
		// in 1970, which is what it always returned.
		int DateTimeFromSimulated(lua_State *state) {
			const LuauContext &context = ContextOf(state);
			PushDateTime(
				state, static_cast<int64_t>(std::llround(context.Clock.Elapsed(*context.World) * 1000.0))
			);
			return 1;
		}

		int DateTimeFromUnixTimestamp(lua_State *state) {
			PushDateTime(state, static_cast<int64_t>(std::llround(luaL_checknumber(state, 1) * 1000.0)));
			return 1;
		}

		int DateTimeFromUnixTimestampMillis(lua_State *state) {
			PushDateTime(state, static_cast<int64_t>(std::llround(luaL_checknumber(state, 1))));
			return 1;
		}

		int DateTimeFromUniversalTime(lua_State *state) {
			PushDateTime(state, CivilArguments(state));
			return 1;
		}

		int DateTimeFromLocalTime(lua_State *state) {
			PushDateTime(state, CivilArguments(state) - LocalOffsetMillis(state));
			return 1;
		}

		// `nil` on text that is not an ISO 8601 date, as Roblox returns.
		int DateTimeFromIsoDate(lua_State *state) {
			const std::optional<int64_t> millis = datetime::ParseIso(luaL_checkstring(state, 1));
			if (!millis) {
				lua_pushnil(state);
				return 1;
			}
			PushDateTime(state, *millis);
			return 1;
		}

		// `{Year, Month, Day, Hour, Minute, Second, Millisecond}`.
		void PushParts(lua_State *state, int64_t millis) {
			const datetime::Civil civil = datetime::ToCivil(millis);
			lua_createtable(state, 0, 7);
			const struct {
				const char *Name;
				double Value;
			} FIELDS[] = {
				{"Year", static_cast<double>(civil.Year)},
				{"Month", static_cast<double>(civil.Month)},
				{"Day", static_cast<double>(civil.Day)},
				{"Hour", static_cast<double>(civil.Hour)},
				{"Minute", static_cast<double>(civil.Minute)},
				{"Second", static_cast<double>(civil.Second)},
				{"Millisecond", static_cast<double>(civil.Millisecond)},
			};
			for (const auto &field : FIELDS) {
				lua_pushnumber(state, field.Value);
				lua_setfield(state, -2, field.Name);
			}
		}

		int DateTimeToUniversalTime(lua_State *state) {
			PushParts(state, CheckDateTime(state, 1));
			return 1;
		}

		int DateTimeToLocalTime(lua_State *state) {
			PushParts(state, CheckDateTime(state, 1) + LocalOffsetMillis(state));
			return 1;
		}

		int DateTimeToIsoDate(lua_State *state) {
			lua_pushstring(state, datetime::ToIso(CheckDateTime(state, 1)).c_str());
			return 1;
		}

		// The locale argument is read and not used: en-us is the only one here.
		int DateTimeFormatUniversalTime(lua_State *state) {
			const int64_t millis = CheckDateTime(state, 1);
			const std::string text = datetime::Format(millis, luaL_checkstring(state, 2));
			lua_pushstring(state, text.c_str());
			return 1;
		}

		int DateTimeFormatLocalTime(lua_State *state) {
			const int64_t millis = CheckDateTime(state, 1) + LocalOffsetMillis(state);
			const std::string text = datetime::Format(millis, luaL_checkstring(state, 2));
			lua_pushstring(state, text.c_str());
			return 1;
		}

		int DateTimeIndex(lua_State *state) {
			const int64_t millis = CheckDateTime(state, 1);
			const std::string_view field = luaL_checkstring(state, 2);
			if (field == "UnixTimestamp") {
				lua_pushnumber(state, static_cast<double>(datetime::FloorSeconds(millis)));
				return 1;
			}
			if (field == "UnixTimestampMillis") {
				lua_pushnumber(state, static_cast<double>(millis));
				return 1;
			}
			static const luaL_Reg METHODS[] = {
				{"ToUniversalTime", DateTimeToUniversalTime},
				{"ToLocalTime", DateTimeToLocalTime},
				{"ToIsoDate", DateTimeToIsoDate},
				{"FormatUniversalTime", DateTimeFormatUniversalTime},
				{"FormatLocalTime", DateTimeFormatLocalTime},
			};
			for (const luaL_Reg &method : METHODS) {
				if (field == method.name) {
					lua_pushcfunction(state, method.func, method.name);
					return 1;
				}
			}
			luaL_errorL(state, "%s is not a valid member of DateTime", std::string(field).c_str());
		}

		int DateTimeEqual(lua_State *state) {
			lua_pushboolean(state, CheckDateTime(state, 1) == CheckDateTime(state, 2));
			return 1;
		}

		int DateTimeLess(lua_State *state) {
			lua_pushboolean(state, CheckDateTime(state, 1) < CheckDateTime(state, 2));
			return 1;
		}

		int DateTimeLessEqual(lua_State *state) {
			lua_pushboolean(state, CheckDateTime(state, 1) <= CheckDateTime(state, 2));
			return 1;
		}

		int DateTimeToString(lua_State *state) {
			lua_pushstring(state, datetime::ToIso(CheckDateTime(state, 1)).c_str());
			return 1;
		}

		// `os.clock()` - the real timer, sampled once per tick. A plugin reads
		// the monotonic clock on every call so it can time its own work.
		int OsClock(lua_State *state) {
			const LuauContext &context = ContextOf(state);
			lua_pushnumber(
				state, context.LiveClock ? core::Clock::Seconds() : ClockNow(*context.World, context.Clock)
			);
			return 1;
		}

		// `debug.profilebegin(label)`, plugins only: opens a frame graph span
		// that `debug.profileend()` or the end of the current resume closes.
		int DebugProfileBegin(lua_State *state) {
			LuauContext &context = ContextOf(state);
			const char *label = luaL_checkstring(state, 1);
			context.ProfileSpans.push_back({
				.Scope = std::make_unique<core::FrameGraph::CopiedScope>(
					"plugin.profile", label, core::ProfileCategory::Script, core::ProfileOwner::Studio
				),
				.Depth = context.ResumeDepth,
			});
			return 0;
		}

		// `debug.profileend()`: closes the newest span this resume opened. An
		// unmatched call is ignored, as Roblox does.
		int DebugProfileEnd(lua_State *state) {
			LuauContext &context = ContextOf(state);
			if (!context.ProfileSpans.empty() && context.ProfileSpans.back().Depth == context.ResumeDepth) {
				context.ProfileSpans.pop_back();
			}
			return 0;
		}

		// A required integer field of an `os.time` table.
		int64_t DateField(lua_State *state, const char *name, std::optional<int64_t> fallback) {
			lua_getfield(state, 1, name);
			int64_t value = 0;
			if (lua_isnumber(state, -1)) {
				value = static_cast<int64_t>(lua_tonumber(state, -1));
			} else if (fallback) {
				value = *fallback;
			} else {
				luaL_errorL(state, "field '%s' missing in date table", name);
			}
			lua_pop(state, 1);
			return value;
		}

		// `os.time(table?)` - whole Unix seconds now, or of a date table read as
		// UTC, which is what Luau's own `os.time` does with one.
		int OsTime(lua_State *state) {
			if (lua_isnoneornil(state, 1)) {
				lua_pushnumber(state, static_cast<double>(datetime::FloorSeconds(NowMillis(state))));
				return 1;
			}
			luaL_checktype(state, 1, LUA_TTABLE);
			const int64_t millis = datetime::FromCivil(
				DateField(state, "year", std::nullopt),
				DateField(state, "month", std::nullopt),
				DateField(state, "day", std::nullopt),
				DateField(state, "hour", 12),
				DateField(state, "min", 0),
				DateField(state, "sec", 0),
				0
			);
			if (!datetime::InRange(millis)) {
				lua_pushnil(state);
				return 1;
			}
			lua_pushnumber(state, static_cast<double>(datetime::FloorSeconds(millis)));
			return 1;
		}

		// `os.date(format?, time?)` - a leading `!` is UTC and otherwise local
		// time; `*t` returns a table and anything else is `strftime`.
		int OsDate(lua_State *state) {
			std::string_view format = luaL_optstring(state, 1, "%c");
			const int64_t millis = lua_isnoneornil(state, 2)
									   ? NowMillis(state)
									   : static_cast<int64_t>(luaL_checknumber(state, 2)) * 1000;
			int32_t offset = static_cast<int32_t>(LocalOffsetMillis(state) / 1000);
			if (!format.empty() && format.front() == '!') {
				format.remove_prefix(1);
				offset = 0;
			}
			const int64_t shifted = millis + static_cast<int64_t>(offset) * 1000;
			if (format.starts_with("*t")) {
				const datetime::Civil civil = datetime::ToCivil(shifted);
				lua_createtable(state, 0, 9);
				const struct {
					const char *Name;
					int64_t Value;
				} FIELDS[] = {
					{"year", civil.Year},
					{"month", civil.Month},
					{"day", civil.Day},
					{"hour", civil.Hour},
					{"min", civil.Minute},
					{"sec", civil.Second},
					{"wday", civil.Weekday + 1},
					{"yday", civil.YearDay},
				};
				for (const auto &field : FIELDS) {
					lua_pushnumber(state, static_cast<double>(field.Value));
					lua_setfield(state, -2, field.Name);
				}
				lua_pushboolean(state, 0);
				lua_setfield(state, -2, "isdst");
				return 1;
			}
			std::string error;
			const std::string text = datetime::StrFTime(shifted, format, offset, error);
			if (!error.empty()) luaL_errorL(state, "bad argument #1 to 'date' (%s)", error.c_str());
			lua_pushlstring(state, text.data(), text.size());
			return 1;
		}

		int OsDifftime(lua_State *state) {
			lua_pushnumber(state, luaL_checknumber(state, 1) - luaL_optnumber(state, 2, 0.0));
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

	void OpenClock(lua_State *state, bool package) {
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

		luaL_newmetatable(state, "DateTime");
		const luaL_Reg METAMETHODS[] = {
			{"__index", DateTimeIndex},
			{"__eq", DateTimeEqual},
			{"__lt", DateTimeLess},
			{"__le", DateTimeLessEqual},
			{"__tostring", DateTimeToString},
		};
		for (const luaL_Reg &entry : METAMETHODS) {
			lua_pushcfunction(state, entry.func, entry.name);
			lua_setfield(state, -2, entry.name);
		}
		lua_pushstring(state, "DateTime");
		lua_setfield(state, -2, "__type");
		lua_pushstring(state, "DateTime");
		lua_setfield(state, -2, "__metatable");
		lua_pop(state, 1);

		// `GetUTCNow` is this engine's sugar for `now`. Called with `:` the
		// table is its first argument, which it ignores.
		const luaL_Reg CONSTRUCTORS[] = {
			{"now", DateTimeNow},
			{"GetUTCNow", DateTimeNow},
			{"fromSimulated", DateTimeFromSimulated},
			{"fromUnixTimestamp", DateTimeFromUnixTimestamp},
			{"fromUnixTimestampMillis", DateTimeFromUnixTimestampMillis},
			{"fromUniversalTime", DateTimeFromUniversalTime},
			{"fromLocalTime", DateTimeFromLocalTime},
			{"fromIsoDate", DateTimeFromIsoDate},
		};
		lua_newtable(state);
		for (const luaL_Reg &entry : CONSTRUCTORS) {
			const bool refused = package && (entry.func == DateTimeNow);
			lua_pushcfunction(state, refused ? PackageDateTimeNow : entry.func, entry.name);
			lua_setfield(state, -2, entry.name);
		}
		lua_setglobal(state, "DateTime");
		if (package) return;

		const luaL_Reg OS[] = {
			{"clock", OsClock},
			{"time", OsTime},
			{"date", OsDate},
			{"difftime", OsDifftime},
		};
		lua_newtable(state);
		for (const luaL_Reg &entry : OS) {
			lua_pushcfunction(state, entry.func, entry.name);
			lua_setfield(state, -2, entry.name);
		}
		lua_setglobal(state, "os");

		// Only the profiling half of `debug`: the rest reaches into frames and
		// locals, which `OpenSafeLibraries` refuses for every runtime.
		if (context.LiveClock) {
			lua_newtable(state);
			lua_pushcfunction(state, DebugProfileBegin, "profilebegin");
			lua_setfield(state, -2, "profilebegin");
			lua_pushcfunction(state, DebugProfileEnd, "profileend");
			lua_setfield(state, -2, "profileend");
			lua_setglobal(state, "debug");
		}
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
