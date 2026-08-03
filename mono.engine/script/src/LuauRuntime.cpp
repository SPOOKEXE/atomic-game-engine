#include "LuauRuntime.hpp"

#include "Bindings.hpp"

#include <engine/core/Log.hpp>
#include <engine/script/Runtime.hpp>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <luacode.h>
#include <lualib.h>
#include <sstream>

namespace engine::script {

	namespace {
		// What the VM is allowed to hold, and how far it may run.
		//
		// Carried in the state's userdata rather than in a static, so two
		// runtimes in one process have two budgets.
		struct Bounds {
			size_t MemoryLimit = 0;
			size_t MemoryHeld = 0;
			uint64_t StepBudget = 0;
			uint64_t StepsTaken = 0;
		};

		Bounds &BoundsOf(lua_State *state) {
			return *static_cast<Bounds *>(lua_getthreaddata(lua_mainthread(state)));
		}

		// Luau's allocator hook, with a ceiling.
		//
		// Refusing an allocation is how a script is told it has run out; Luau
		// turns a null return into an ordinary script error, so the host sees a
		// failed `Run` rather than a `bad_alloc` from somewhere in the middle
		// of an interpreter.
		void *Allocate(void *context, void *pointer, size_t oldSize, size_t newSize) {
			auto *bounds = static_cast<Bounds *>(context);

			if (newSize == 0) {
				bounds->MemoryHeld -= oldSize;
				std::free(pointer);
				return nullptr;
			}

			const size_t held = bounds->MemoryHeld - oldSize + newSize;
			if (bounds->MemoryLimit != 0 && held > bounds->MemoryLimit) {
				return nullptr;
			}

			void *resized = std::realloc(pointer, newSize);
			if (resized == nullptr) {
				return nullptr;
			}

			bounds->MemoryHeld = held;
			return resized;
		}

		// Called at loop back-edges, calls and returns.
		//
		// This is what bounds `while true do end`. It counts rather than reading
		// a clock deliberately: a wall-clock deadline makes whether a script
		// finished depend on how busy the machine was, and a recorded run would
		// then replay differently on a slower one — the desync rule 5 names.
		void Interrupt(lua_State *state, int) {
			Bounds &bounds = BoundsOf(state);
			if (bounds.StepBudget == 0) {
				return;
			}

			if (++bounds.StepsTaken > bounds.StepBudget) {
				bounds.StepsTaken = 0;
				luaL_errorL(state, "script exceeded its step budget");
			}
		}

		int Print(lua_State *state) {
			std::string line;
			const int count = lua_gettop(state);

			for (int index = 1; index <= count; index++) {
				if (index > 1) {
					line += '\t';
				}

				// `luaL_tolstring` honours `__tostring`, so an instance prints
				// its name and a Vector3 its components rather than an address.
				size_t length = 0;
				const char *text = luaL_tolstring(state, index, &length);
				line.append(text, length);
				lua_pop(state, 1);
			}

			ENGINE_INFO("[script] {}", line);
			return 0;
		}

		// The libraries a script gets, and the two it does not.
		//
		// `luaL_openlibs` would also open **`os`** and **`debug`**, and both are
		// refused here rather than removed afterwards:
		//
		// - `os` is `time`, `clock` and `date`. Every one of them reads a wall
		//   clock, and a script branching on one produces a run that does not
		//   replay — `just replay-check` would fail somewhere far from the
		//   script that caused it. A world's clock is `store.Time()`, which is
		//   simulated, and that is what a script will get when it gets one.
		// - `debug` reaches into the interpreter: stack frames, locals,
		//   upvalues. A game loads scripts it did not write, and that is the
		//   library for climbing out of a sandbox rather than working inside it.
		void OpenSafeLibraries(lua_State *state) {
			static const luaL_Reg libraries[] = {
				{"", luaopen_base},
				{LUA_COLIBNAME, luaopen_coroutine},
				{LUA_TABLIBNAME, luaopen_table},
				{LUA_STRLIBNAME, luaopen_string},
				{LUA_MATHLIBNAME, luaopen_math},
				{LUA_BITLIBNAME, luaopen_bit32},
				{LUA_UTF8LIBNAME, luaopen_utf8},
				{LUA_BUFFERLIBNAME, luaopen_buffer},
				{LUA_VECLIBNAME, luaopen_vector},
			};

			for (const luaL_Reg &library : libraries) {
				lua_pushcfunction(state, library.func, nullptr);
				lua_pushstring(state, library.name);
				lua_call(state, 1, 0);
			}
		}
	}

	LuauRuntime::LuauRuntime(ecs::Store &store, const RuntimeLimits &limits) : Runtime(store) {
		auto *bounds = new Bounds();
		bounds->MemoryLimit = limits.MemoryBytes;
		bounds->StepBudget = limits.StepBudget;

		State = lua_newstate(Allocate, bounds);
		lua_setthreaddata(State, bounds);

		OpenSafeLibraries(State);

		lua_pushcfunction(State, Print, "print");
		lua_setglobal(State, "print");

		OpenValues(State);
		OpenInstances(State, Store);
		OpenRunService(State);
		OpenGame(State);
		OpenWorkspace(State, Store);
		OpenServices(State, Store);

		lua_callbacks(State)->interrupt = Interrupt;

		// Freezes the global table and the library tables. After this a script
		// can read `math.floor` and cannot replace it, so one script cannot
		// change the language the next one runs in.
		luaL_sandbox(State);
	}

	LuauRuntime::~LuauRuntime() {
		if (State != nullptr) {
			auto *bounds = static_cast<Bounds *>(lua_getthreaddata(State));
			lua_close(State);
			delete bounds;
		}
	}

	bool LuauRuntime::Run(std::string_view source, std::string_view name) {
		Error.clear();

		// Compiled to bytecode first: Luau is a compiler and a VM, and they are
		// separate libraries on purpose. Optimisation at 1 and debug info at 1
		// — the default pairing, which keeps a usable traceback.
		lua_CompileOptions options = {};
		options.optimizationLevel = 1;
		options.debugLevel = 1;

		size_t bytecodeSize = 0;
		char *bytecode = luau_compile(source.data(), source.size(), &options, &bytecodeSize);
		if (bytecode == nullptr) {
			Error = "the compiler produced nothing";
			return false;
		}

		// A sandboxed thread rather than the main one, so a script's globals are
		// its own: assigning a global in one chunk does not leak into the next.
		lua_State *thread = lua_newthread(State);
		luaL_sandboxthread(thread);

		// `=` tells Luau to use the name as-is rather than decorating it, so a
		// traceback reads as the path somebody can open.
		const std::string chunkName = "=" + std::string(name);
		const int loaded = luau_load(thread, chunkName.c_str(), bytecode, bytecodeSize, 0);
		std::free(bytecode);

		if (loaded != 0) {
			// A compile error arrives as the loaded chunk's value.
			Error =
				lua_tostring(thread, -1) != nullptr ? lua_tostring(thread, -1) : "could not load the script";
			lua_pop(State, 1);
			return false;
		}

		BoundsOf(State).StepsTaken = 0;

		const int status = lua_resume(thread, nullptr, 0);
		if (status != LUA_OK && status != LUA_BREAK) {
			const char *message = lua_tostring(thread, -1);
			Error = message != nullptr ? message : "the script failed";

			// The traceback is what makes a script error actionable, and it has
			// to be read off the thread that failed before it is discarded.
			if (const char *trace = lua_debugtrace(thread); trace != nullptr) {
				Error += "\n";
				Error += trace;
			}

			lua_pop(State, 1);
			return false;
		}

		if (status == LUA_BREAK) {
			// A yield. Nothing in this version gives a script anything to yield
			// on, so reaching here means one found a way — and finishing the
			// tick with a suspended script would be work crossing a tick
			// boundary, which is exactly what rule 5 forbids. Refused loudly
			// rather than resumed at some later point nobody chose.
			Error = "the script yielded, and v0.5 has nothing to resume it from";
			lua_pop(State, 1);
			return false;
		}

		lua_pop(State, 1);
		return true;
	}

	bool LuauRuntime::Heartbeat(float delta) {
		// **Deliveries first, then the beat.** A message the barrier applied
		// belongs to the tick that is starting, so a subscriber sees it before
		// anything that beat moves — the alternative is a world reacting one
		// tick late to something it already has.
		Error = PumpDeliveries(State, Store);

		const std::string beat = PumpHeartbeat(State, delta);
		if (Error.empty()) {
			Error = beat;
		}
		return Error.empty();
	}
}
