// `BreakpointService`, which is the debugger a script can drive.
//
// **A service rather than a host call, and that is what makes it worth
// having.** The editor's own panel writes into `Runtime::Debug()` directly; this
// is the same object reached from Luau, so a *tool* can arm a breakpoint the way
// a person would — which is the whole shape of "put a breakpoint on every line
// that writes this property" and the reason a debugger is worth scripting at
// all.
//
// **Only in a studio.** `RuntimeLimits::Role` says where a script is standing,
// and a shipped server has no business letting a game script single-step itself:
// arming a breakpoint switches Luau's step mode on, which costs the whole
// runtime its speed. The service is simply absent when the role is not a studio,
// so `game:GetService("BreakpointService")` fails the way an unknown service
// does rather than answering an object that refuses everything.
//
// ## Two levels, and the split is real rather than decorative
//
// **The high level takes a script instance.** `SetBreakpoint(script, 12)` is
// what a tool has in its hand — it just walked the tree and found a
// `LuaSourceContainer` — and it resolves the instance's `Source` property to the
// chunk name the VM will report. A caller doing that itself would be
// reimplementing the one mapping that has to agree with the runtime.
//
// **The low level takes the chunk name.** `Arm("enemy.luau", 12)` is what the
// editor's panel and a test have: a path typed by a person, or one read out of a
// hit that has already happened. It is also the only form that can name a chunk
// no instance carries — a module required from a file, or a chunk run by
// `Runtime::Run` with a name of its own.
//
// Neither is a wrapper over the other's storage. Both write the same
// `Debugger`, which is the object the runtime consults.

#include "LuauBindings.hpp"

#include <engine/core/Log.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/script/Debugger.hpp>
#include <engine/script/SourceCache.hpp>

#include <lualib.h>
#include <string>
#include <string_view>

namespace engine::script {

	namespace {
		using core::Name;
		using ecs::Entity;

		// The debugger this VM writes into, or an error saying there is none.
		//
		// **Null is a real state rather than an impossible one.** A runtime
		// built without one — every game runtime — reaches this only because
		// the service was installed, and the service is only installed in a
		// studio; so this is the belt to the role's braces and it says which of
		// the two failed.
		Debugger &CheckDebugger(lua_State *state) {
			Debugger *breakpoints = UpvalueContext(state).Breakpoints;
			if (breakpoints == nullptr) {
				luaL_errorL(state, "this runtime has no debugger");
			}
			return *breakpoints;
		}

		// The chunk name an argument names, however it was given.
		//
		// **A string is taken as it is and an instance is resolved.** That is
		// the whole of the two levels: a tool holds an instance and the editor
		// holds a path, and making one of them convert first would put the
		// mapping in two places.
		std::string SourceOf(lua_State *state, int index) {
			// **`lua_type`, not `lua_isstring`.** The second answers true for a
			// *number* too, because Lua coerces one — so `SetBreakpoint(7, 1)`
			// armed a breakpoint on a chunk called "7" and reported success. A
			// caller who got the argument order wrong is the likeliest way to
			// write that, and silently accepting it is a breakpoint that never
			// fires with nothing saying why.
			if (lua_type(state, index) == LUA_TSTRING) {
				size_t length = 0;
				const char *text = lua_tolstring(state, index, &length);
				return std::string(text, length);
			}

			void *value = lua_touserdatatagged(state, index, TAG_INSTANCE);
			if (value == nullptr) {
				luaL_typeerrorL(state, index, "Instance or string");
			}

			const Entity instance = *static_cast<Entity *>(value);
			const ecs::Store &store = *UpvalueContext(state).World;

			// `Source` is the path a `LuaSourceContainer` was loaded from, which
			// is exactly what the VM reports as the chunk name — so resolving it
			// here is reading the one fact both ends already agree on.
			Name path;
			if (!store.GetProperty(instance, Name("Source"), &path, sizeof(path))) {
				luaL_errorL(state, "that instance is not a script");
			}

			const std::string_view text = path.Text();
			if (text.empty()) {
				luaL_errorL(state, "that script names no source");
			}
			return std::string(text);
		}

		// The 1-based line an argument names.
		int LineOf(lua_State *state, int index) {
			const int line = static_cast<int>(luaL_checkinteger(state, index));
			if (line < 1) {
				// **Refused rather than clamped**, because a zero is almost
				// always an off-by-one in the caller and a breakpoint silently
				// moved to line one would fire somewhere nobody asked for.
				luaL_errorL(state, "a line number starts at 1, not %d", line);
			}
			return line;
		}

		// `BreakpointService:SetBreakpoint(scriptOrPath, line, stop?)`
		int SetBreakpoint(lua_State *state) {
			Debugger &breakpoints = CheckDebugger(state);

			const std::string source = SourceOf(state, 2);
			const int line = LineOf(state, 3);

			// **Capture unless asked otherwise**, which is the safe default: a
			// breakpoint that stopped by surprise would end the script a tool
			// was in the middle of walking.
			const BreakAction action =
				lua_toboolean(state, 4) != 0 ? BreakAction::Stop : BreakAction::Capture;

			// **An error rather than a breakpoint that never fires.** A tool
			// that armed one on a TypeScript file and got silence would read it
			// as the debugger being broken; the refusal names the language and
			// the entry that says what closing it would take.
			if (const std::string_view refused = BreakpointsRefused(source); !refused.empty()) {
				luaL_errorL(state, "cannot break in '%s': %s", source.c_str(), std::string(refused).c_str());
			}

			breakpoints.Add(source, line, action);
			return 0;
		}

		// `BreakpointService:RemoveBreakpoint(scriptOrPath, line)`
		int RemoveBreakpoint(lua_State *state) {
			Debugger &breakpoints = CheckDebugger(state);

			const std::string source = SourceOf(state, 2);
			const int line = LineOf(state, 3);

			lua_pushboolean(state, breakpoints.Remove(source, line));
			return 1;
		}

		// `BreakpointService:SetEnabled(scriptOrPath, line, enabled)`
		int SetEnabled(lua_State *state) {
			Debugger &breakpoints = CheckDebugger(state);

			const std::string source = SourceOf(state, 2);
			const int line = LineOf(state, 3);

			lua_pushboolean(state, breakpoints.Enable(source, line, lua_toboolean(state, 4) != 0));
			return 1;
		}

		// `BreakpointService:ClearBreakpoints()`
		int ClearBreakpoints(lua_State *state) {
			CheckDebugger(state).Clear();
			return 0;
		}

		// `BreakpointService:GetBreakpoints()`
		//
		// One table per breakpoint, in the order they were added — which is the
		// order the editor's list draws them in, so a tool and a person are
		// looking at the same thing in the same sequence.
		int GetBreakpoints(lua_State *state) {
			const Debugger &breakpoints = CheckDebugger(state);

			lua_newtable(state);
			int index = 0;

			for (const Breakpoint &point : breakpoints.Breakpoints()) {
				lua_newtable(state);

				lua_pushlstring(state, point.Source.data(), point.Source.size());
				lua_setfield(state, -2, "Source");

				lua_pushinteger(state, point.Line);
				lua_setfield(state, -2, "Line");

				lua_pushboolean(state, point.Enabled);
				lua_setfield(state, -2, "Enabled");

				lua_pushboolean(state, point.Action == BreakAction::Stop);
				lua_setfield(state, -2, "Stops");

				lua_pushnumber(state, static_cast<double>(point.Hits));
				lua_setfield(state, -2, "Hits");

				lua_rawseti(state, -2, ++index);
			}
			return 1;
		}

		// One captured frame as a table.
		void PushFrame(lua_State *state, const DebugFrame &frame) {
			const auto values = [state](const std::vector<DebugLocal> &named, const char *field) {
				lua_newtable(state);
				int at = 0;

				for (const DebugLocal &value : named) {
					lua_newtable(state);

					lua_pushlstring(state, value.Name.data(), value.Name.size());
					lua_setfield(state, -2, "Name");

					lua_pushlstring(state, value.Value.data(), value.Value.size());
					lua_setfield(state, -2, "Value");

					lua_rawseti(state, -2, ++at);
				}
				lua_setfield(state, -2, field);
			};

			lua_newtable(state);

			lua_pushlstring(state, frame.Source.data(), frame.Source.size());
			lua_setfield(state, -2, "Source");

			lua_pushlstring(state, frame.Function.data(), frame.Function.size());
			lua_setfield(state, -2, "Function");

			lua_pushinteger(state, frame.Line);
			lua_setfield(state, -2, "Line");

			// **Locals and upvalues as two lists, not one.** A local is a value
			// this frame made and an upvalue is one it captured; merging them
			// would lose the distinction that makes the second worth capturing.
			values(frame.Locals, "Locals");
			values(frame.Upvalues, "Upvalues");
		}

		// `BreakpointService:GetHits()`
		int GetHits(lua_State *state) {
			const Debugger &breakpoints = CheckDebugger(state);

			lua_newtable(state);
			int index = 0;

			for (const DebugHit &hit : breakpoints.Hits()) {
				lua_newtable(state);

				lua_pushlstring(state, hit.Source.data(), hit.Source.size());
				lua_setfield(state, -2, "Source");

				lua_pushinteger(state, hit.Line);
				lua_setfield(state, -2, "Line");

				if (hit.Instance != ecs::NULL_ENTITY) {
					PushInstanceValue(state, hit.Instance);
					lua_setfield(state, -2, "Script");
				}

				lua_newtable(state);
				int frame = 0;
				for (const DebugFrame &record : hit.Frames) {
					PushFrame(state, record);
					lua_rawseti(state, -2, ++frame);
				}
				lua_setfield(state, -2, "Frames");

				lua_rawseti(state, -2, ++index);
			}
			return 1;
		}

		// `BreakpointService:ClearHits()`
		int ClearHits(lua_State *state) {
			CheckDebugger(state).ClearHits();
			return 0;
		}

		// `BreakpointService:IsArmed()`
		//
		// **What decides whether this runtime is paying for single-step mode**,
		// which is the one cost of the whole feature — so a tool that arms
		// breakpoints in a loop can check that it disarmed them again.
		int IsArmed(lua_State *state) {
			lua_pushboolean(state, CheckDebugger(state).Armed());
			return 1;
		}
	}

	void OpenBreakpointService(lua_State *state) {
		LuauContext &context = ContextOf(state);

		// **Absent rather than refusing**, so `game:GetService` fails the way it
		// does for any service this engine does not provide. A service that
		// existed and answered "not in a game" to everything would be a surface
		// somebody writes against and then finds does nothing where it matters.
		if (!context.Role.Studio || context.Breakpoints == nullptr) {
			return;
		}

		static constexpr LuauServiceMethod METHODS[] = {
			// The high level: a script instance and a line.
			{"SetBreakpoint", SetBreakpoint},
			{"RemoveBreakpoint", RemoveBreakpoint},
			{"SetEnabled", SetEnabled},
			{"ClearBreakpoints", ClearBreakpoints},
			{"GetBreakpoints", GetBreakpoints},

			// The low level: what was caught, and whether anything is armed.
			{"GetHits", GetHits},
			{"ClearHits", ClearHits},
			{"IsArmed", IsArmed},
		};

		// A global, which is what makes `game:GetService("BreakpointService")`
		// find it — that function resolves a service by looking one up, and
		// every other surface service is the same shape for the same reason.
		ServiceSurface surface;
		surface.Name = "BreakpointService";
		surface.LuauMethods = METHODS;

		InstallService(state, surface);
	}
}
