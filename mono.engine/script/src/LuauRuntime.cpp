#include "LuauRuntime.hpp"

#include "Bindings.hpp"

#include <engine/core/Log.hpp>
#include <engine/core/Paths.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/script/Instances.hpp>
#include <engine/script/Runtime.hpp>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <luacode.h>
#include <lualib.h>
#include <sstream>

namespace engine::script {

	namespace {
		// What the VM is allowed to hold, how far it may run, and everything it
		// can reach.
		//
		// Carried in the state's userdata rather than in a static, so two
		// runtimes in one process have two of everything — which is what makes
		// "one runtime, one world" a property of the arrangement rather than a
		// rule somebody has to remember.
		struct Bounds {
			size_t MemoryLimit = 0;
			size_t MemoryHeld = 0;
			uint64_t StepBudget = 0;
			uint64_t StepsTaken = 0;

			LuauContext Context;
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
		//   simulated, and `OpenClock` is what a script gets instead.
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

	LuauContext &ContextOf(lua_State *state) {
		return BoundsOf(state).Context;
	}

	LuauContext &UpvalueContext(lua_State *state) {
		return *static_cast<LuauContext *>(lua_tolightuserdata(state, lua_upvalueindex(1)));
	}

	LuauRuntime::LuauRuntime(ecs::Store &store, const RuntimeLimits &limits) : Runtime(store, limits.Role) {
		auto *bounds = new Bounds();
		bounds->MemoryLimit = limits.MemoryBytes;
		bounds->StepBudget = limits.StepBudget;
		bounds->Context.World = &Store;
		bounds->Context.Role = limits.Role;

		State = lua_newstate(Allocate, bounds);
		lua_setthreaddata(State, bounds);
		bounds->Context.State = State;

		OpenSafeLibraries(State);

		lua_pushcfunction(State, Print, "print");
		lua_setglobal(State, "print");

		// **`nil` as a global, because this is a Roblox-shaped API.** Luau
		// already has the keyword; what this adds is nothing, and that is the
		// point — the JavaScript side needs an alias for `null` and this side
		// needs no such thing, so the two surfaces differ by a line rather than
		// by a concept.

		OpenValues(State);
		OpenDatatypes(State);
		OpenEnums(State);
		OpenSignals(State);
		OpenInstances(State, Store);
		OpenRunService(State);
		OpenGame(State);
		OpenWorkspace(State, Store);
		OpenServices(State, Store);
		OpenQueries(State);
		OpenTask(State);
		OpenClock(State);
		OpenBaseExtras(State);

		lua_callbacks(State)->interrupt = Interrupt;

		// Freezes the global table and the library tables. After this a script
		// can read `math.floor` and cannot replace it, so one script cannot
		// change the language the next one runs in.
		luaL_sandbox(State);
	}

	LuauRuntime::~LuauRuntime() {
		if (State == nullptr) {
			return;
		}

		auto *bounds = static_cast<Bounds *>(lua_getthreaddata(State));

		// **The store's change listeners go before the VM does.** They capture
		// `this` through the queue, and a store that outlives the runtime would
		// otherwise call into freed memory at its next barrier — which is the
		// ordinary case, because a world is destroyed after the scripts that
		// built it.
		if (bounds != nullptr) {
			bounds->Context.Changes.Detach(Store);
		}

		lua_close(State);
		delete bounds;
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

		// **`script` goes on the thread, after the sandbox.** The state's global
		// table is frozen and shared; the thread's is this chunk's alone, which
		// is exactly the scoping `script` needs — two scripts in one world each
		// see themselves and never each other.
		if (LuauContext &context = ContextOf(State); context.PendingScript != ecs::NULL_ENTITY) {
			PushInstanceValue(thread, context.PendingScript);
			lua_setglobal(thread, "script");
		}

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

		// **A yield is legal now, and only from `task`.**
		//
		// `docs/SCRIPT_CONCURRENCY.md` §1 permits a resume from a tick boundary,
		// a barrier delivery or a `Ticket` reply, and forbids everything else. A
		// thread that suspended through `task.wait` is registered for one of
		// those and something will come back for it. A thread that suspended any
		// other way found a route the sandbox did not intend, and finishing the
		// tick with it would be work crossing a tick boundary — which is exactly
		// what rule 5 forbids and what v0.5 refused outright.
		//
		// So the check is not "did it yield" but "will anything resume it", and
		// the answer is a lookup rather than a judgement.
		if (status == LUA_YIELD) {
			if (ThreadIsScheduled(ContextOf(State), thread)) {
				lua_pop(State, 1);
				return true;
			}

			Error = "the script yielded with nothing scheduled to resume it";
			lua_pop(State, 1);
			return false;
		}

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

		lua_pop(State, 1);
		return true;
	}

	bool LuauRuntime::RunInstance(ecs::Entity instance) {
		Error.clear();

		const Source *source = Store.Get<Source>(instance);
		if (source == nullptr || !source->Path.IsValid()) {
			// A script with no path is a no-op rather than an error: an author
			// makes the instance before choosing the file, and that is a legal
			// state.
			return true;
		}

		// An absolute `Source` is used as it stands. `operator/` already drops
		// the left side for an absolute right side, so this is what the standard
		// does anyway — said out loud because a reader checking whether
		// `--script /tmp/x.luau` works should not have to know that.
		const std::filesystem::path named(source->Path.Text());
		const std::filesystem::path path = named.is_absolute() ? named : core::Paths::Assets() / named;

		std::ifstream file(path, std::ios::binary);
		if (!file) {
			Error = "could not open " + path.string();
			return false;
		}

		std::ostringstream contents;
		contents << file.rdbuf();

		// **`script` names the instance, and it is set before the chunk runs.**
		// That is the whole difference from `RunFile`: a chunk run this way can
		// reach its own parent and its own siblings, which is what makes a game
		// of many scripts possible rather than one file that builds a world.
		//
		// A plain global rather than an upvalue, because `luaL_sandboxthread`
		// gives each chunk its own global table — so one script's `script` is
		// invisible to the next, which is exactly the scoping an author expects.
		ContextOf(State).PendingScript = instance;

		const bool ok = Run(contents.str(), source->Path.Text());
		ContextOf(State).PendingScript = ecs::NULL_ENTITY;
		return ok;
	}

	bool LuauRuntime::Heartbeat(float delta) {
		// **The order is the tick's own, and every step of it is a rule.**
		//
		//   1. deliveries — a message the barrier applied belongs to the tick
		//      that is starting, so a subscriber sees it before anything that
		//      beat moves. The alternative is a world reacting one tick late to
		//      something it already has.
		//   2. changes — what the *previous* barrier recorded, fanned out from
		//      components to property names. Before the beat, so a `.Changed`
		//      handler and a `Heartbeat` handler see the same world.
		//   3. tasks — resumes due at this tick, then everything deferred from
		//      the last beat.
		//   4. the beat itself.
		//
		// Each of the first three is one of §1's three legal resume sources, in
		// the order the barrier produced them.
		//
		// **A span each, because one span over all four answers nothing.** The
		// four do entirely different work — a bus drain, a signal fan-out, a
		// coroutine resume and a script's own beat — and they fail differently:
		// deliveries scale with traffic, changes with how much of the world
		// moved, tasks with how many are due, and the beat with what the game
		// wrote. A single `script heartbeat` bar that spikes says only that
		// something did, which is where this was before a spike had to be found
		// in `Mirrors-1-world` and nothing under it could be read.
		std::string firstError;
		{
			ENGINE_PROFILE_CAT("script deliveries", core::ProfileCategory::Script);
			firstError = PumpDeliveries(State, Store);
		}

		const auto note = [&](std::string message) {
			if (firstError.empty()) {
				firstError = std::move(message);
			}
		};

		{
			ENGINE_PROFILE_CAT("script changes", core::ProfileCategory::Script);
			note(PumpChanges(State));
		}
		{
			ENGINE_PROFILE_CAT("script tasks", core::ProfileCategory::Script);
			note(PumpTasks(State));
		}
		{
			ENGINE_PROFILE_CAT("script beat", core::ProfileCategory::Script);
			note(PumpHeartbeat(State, delta));
		}

		// **The collector, on the host's clock rather than on an allocation's.**
		//
		// Luau collects incrementally when a script allocates, which puts the
		// pause inside whichever `Vector3.new` happened to cross the threshold —
		// so it lands *inside* the beat's span and is invisible as itself. A
		// scene animating parts allocates hard: `Mirrors-1-world` builds two
		// vectors and two CFrames per caster per frame, which at 24 casters and
		// 60 Hz is thousands of objects a second, and the collector's cost shows
		// up as a beat that is occasionally slow for no reason the beat can
		// explain.
		//
		// Stepping it here does not stop the automatic collector — it means part
		// of the debt is paid where it can be seen and named. What it costs is
		// one span a tick.
		{
			ENGINE_PROFILE_CAT("script gc", core::ProfileCategory::Script);
			lua_gc(State, LUA_GCSTEP, 1);
		}

		Error = firstError;
		return Error.empty();
	}
}
