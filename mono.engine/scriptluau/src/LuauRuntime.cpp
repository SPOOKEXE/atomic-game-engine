#include "LuauRuntime.hpp"

#include "LuauBindings.hpp"

#include <engine/core/Log.hpp>
#include <engine/core/Paths.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/script/Instances.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/script/SourceCache.hpp>
#include <engine/scriptluau/Runtime.hpp>

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
		// runtimes in one process have two of everything - which is what makes
		// "one runtime, one world" a property of the arrangement rather than a
		// rule somebody has to remember.
		struct Bounds {
			size_t MemoryLimit = 0;
			size_t MemoryHeld = 0;
			uint64_t StepBudget = 0;

			// Only ever goes up, so a difference of two readings is what one
			// script cost. `StepsBase` is where the call now running started,
			// and the budget is the difference between the two.
			uint64_t StepsTaken = 0;
			uint64_t StepsBase = 0;

			// Where the last breakpoint fired, so a line that compiles to
			// several instructions reports once rather than once per opcode.
			int LastBreakLine = 0;
			std::string LastBreakSource;

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
		// then replay differently on a slower one - the desync rule 5 names.
		//
		// **Once tripped it stays tripped until the next call moves
		// `StepsBase`.** Zeroing the counter here was two bugs at once: a
		// `pcall` around the runaway caught the error and the loop that
		// followed got a whole fresh budget, so a script could run for as long
		// as it liked one trip at a time; and `StepsTaken` lost exactly the
		// script that had spent the most, which is the one a profile panel is
		// being read to find.
		void Interrupt(lua_State *state, int gc) {
			// Luau also calls this hook while assisting the collector. Counting that
			// work charges the host for VM maintenance, and raising from it can enter
			// the collector again while the error string is being built.
			if (gc >= 0) {
				return;
			}

			Bounds &bounds = BoundsOf(state);
			bounds.StepsTaken++;
			if (bounds.StepBudget == 0) {
				return;
			}

			if (bounds.StepsTaken - bounds.StepsBase > bounds.StepBudget) {
				luaL_errorL(state, "script exceeded its step budget");
			}
		}

		// Renders one stack value the way `print` would.
		//
		// Through `luaL_tolstring` so `__tostring` is honoured: an instance
		// reads as its name and a Vector3 as its components, which is the whole
		// difference between a useful capture and a column of addresses.
		std::string Rendered(lua_State *state, int index) {
			size_t length = 0;
			const char *text = luaL_tolstring(state, index, &length);
			std::string value(text, length);
			lua_pop(state, 1);
			return value;
		}

		// Walks the stack into a hit record.
		//
		// **Innermost frame first**, which is the order a stack is read in and
		// the order `lua_getinfo` numbers levels.
		void Capture(lua_State *state, DebugHit &hit) {
			const int depth = lua_stackdepth(state);

			for (int level = 0; level < depth; level++) {
				lua_Debug info;
				if (lua_getinfo(state, level, "sln", &info) == 0) {
					continue;
				}

				DebugFrame frame;
				frame.Source = info.short_src != nullptr ? info.short_src : "";
				frame.Function = info.name != nullptr ? info.name : "";
				frame.Line = info.currentline;

				// Locals and arguments both, because a script author does not
				// distinguish them and the one being looked for is as often a
				// parameter as a `local`.
				for (int slot = 1;; slot++) {
					const char *name = lua_getlocal(state, level, slot);
					if (name == nullptr) {
						break;
					}
					frame.Locals.push_back(DebugLocal{name, Rendered(state, -1)});
					lua_pop(state, 1);

					// Bounded: a frame with two hundred locals is a generated
					// one and the panel cannot show them anyway.
					if (frame.Locals.size() >= 32) {
						break;
					}
				}

				// **The upvalues, which need the function itself on the
				// stack.** `lua_getlocal` takes a level and `lua_getupvalue`
				// takes a closure, so the two walks cannot share a loop - which
				// is why this is a second pass rather than a second column in
				// the first.
				//
				// `lua_getinfo` with `f` pushes the running function; anything
				// that leaves it there leaks a stack slot per frame, so the pop
				// is unconditional.
				lua_Debug closure;
				if (lua_getinfo(state, level, "f", &closure) != 0) {
					for (int slot = 1;; slot++) {
						const char *name = lua_getupvalue(state, -1, slot);
						if (name == nullptr) {
							break;
						}

						// An unnamed upvalue is one the compiler made, and
						// `lua_getupvalue` reports those as an empty string
						// rather than as null. Numbered rather than blank, so a
						// reader can tell two of them apart.
						frame.Upvalues.push_back(
							DebugLocal{
								*name != '\0' ? name : ("(upvalue " + std::to_string(slot) + ")"),
								Rendered(state, -1)
							}
						);
						lua_pop(state, 1);

						if (frame.Upvalues.size() >= 32) {
							break;
						}
					}
					lua_pop(state, 1);
				}

				hit.Frames.push_back(std::move(frame));
			}
		}

		// Called after every instruction while single-step mode is on.
		//
		// **Only ever on while a breakpoint is armed.** `LuauRuntime::Run`
		// switches it on and off around each chunk, so a runtime nobody is
		// debugging never reaches this function.
		void DebugStep(lua_State *state, lua_Debug *) {
			Bounds &bounds = BoundsOf(state);
			Debugger *debug = bounds.Context.Breakpoints;
			if (debug == nullptr) {
				return;
			}

			lua_Debug here;
			if (lua_getinfo(state, 0, "sl", &here) == 0) {
				return;
			}

			const char *source = here.short_src != nullptr ? here.short_src : "";
			Breakpoint *point = debug->Match(source, here.currentline);
			if (point == nullptr) {
				return;
			}

			// **Once per arrival at the line, not once per instruction on it.**
			// A line compiles to several instructions and single-step fires for
			// each, so without this a breakpoint on one line reports four times
			// and the hit count means nothing.
			if (bounds.LastBreakLine == here.currentline && bounds.LastBreakSource == source) {
				return;
			}
			bounds.LastBreakLine = here.currentline;
			bounds.LastBreakSource = source;

			point->Hits++;

			DebugHit hit;
			hit.Source = source;
			hit.Line = here.currentline;
			hit.Instance = bounds.Context.RunningScript;
			Capture(state, hit);
			debug->Record(std::move(hit));

			if (point->Action == BreakAction::Stop) {
				// An ordinary script error, which the host already knows how to
				// report - rather than a new state the runtime would have to
				// learn to be in.
				luaL_errorL(state, "stopped at a breakpoint: %s:%d", source, here.currentline);
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
		//   replay - `just replay-check` would fail somewhere far from the
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

		// The string keys of the table at `index`, for `Runtime::Surface`.
		//
		// **A raw walk, so a table whose members come from an `__index`
		// function answers empty rather than answering wrongly.** That is the
		// honest result: `Enum` and every instance userdata resolve their
		// members in C++, and there is nothing there to enumerate. The editor
		// fills those from `ecs::EnumTable` and `ecs::Classes`, which actually
		// know.
		std::vector<std::string> TableMembers(lua_State *state, const int index) {
			std::vector<std::string> members;
			if (lua_type(state, index) != LUA_TTABLE) {
				return members;
			}

			// Absolute, because pushing the key below moves a negative one.
			const int table = lua_absindex(state, index);

			lua_pushnil(state);
			while (lua_next(state, table) != 0) {
				if (lua_type(state, -2) == LUA_TSTRING) {
					members.emplace_back(lua_tostring(state, -2));
				}
				lua_pop(state, 1);
			}
			return members;
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
		// point - the JavaScript side needs an alias for `null` and this side
		// needs no such thing, so the two surfaces differ by a line rather than
		// by a concept.

		OpenValues(State);
		OpenDatatypes(State);
		OpenEnums(State);
		OpenSignals(State);
		OpenInstances(State);
		OpenGame(State);
		OpenWorkspace(State, Store);

		// **Before the services, because two of them produce one.**
		// `UserInputService`'s signals and a bound action's handler are both
		// handed an `InputObject`, and a metatable registered after the service
		// that hands one over would be looked up before it exists. The JavaScript
		// runtime installs its class in the same position and for this reason.
		OpenInputObject(State);

		// **Before the services too, and for `OpenInputObject`'s reason.**
		// `TweenService:Create` hands back a `Tween`, whose metatable is
		// registered here - a service installed first would be one whose method
		// looks up a metatable that does not exist yet. The JavaScript runtime
		// installs its class in the same position.
		OpenTweenHandle(State);

		// **Every service this language binds, from the catalogue.** They used to
		// be a hand-written call list here, which is how "which services exist"
		// came to be two lists - this one and the JavaScript runtime's - that
		// drifted by four services with nothing in the build to say so. See
		// `ServiceCatalogue.hpp`.
		InstallLuauServices(State, ServiceAvailability::Always);

		OpenQueries(State);

		// After `OpenInstances`, whose method table this adds the component
		// half of the ECS surface to.
		OpenEcs(State);
		OpenTask(State);
		OpenClock(State);
		OpenBaseExtras(State);

		// **After every engine surface and before `OpenRequire`**, so a host
		// cannot replace one of the engine's globals by naming it - the table is
		// its own and `host.Thing` is the only way in.
		OpenHost(State);

		// **After `OpenBaseExtras`, which installs the refusal.** Overwriting it
		// here rather than deleting it there keeps a runtime that never calls this
		// refusing with a sentence rather than with a nil global.
		OpenRequire(State);

		lua_callbacks(State)->interrupt = Interrupt;

		// **The step callback is installed, single-step mode is not.** Luau only
		// calls this while stepping is enabled, and `Run` enables it for exactly
		// as long as a breakpoint is armed - so a runtime nobody is debugging
		// pays nothing for this line.
		lua_callbacks(State)->debugstep = DebugStep;

		BoundsOf(State).Context.Breakpoints = &Breakpoints;

		// **After that assignment and before the sandbox**, and the position is
		// forced from both sides. A studio service reads the pointer above to
		// decide whether to install itself at all, so it cannot run earlier; and
		// it writes a global, so it cannot run once the table is frozen. That is
		// the whole reason `ServiceAvailability` has a second value and the
		// catalogue is walked twice.
		InstallLuauServices(State, ServiceAvailability::Studio);

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
		// otherwise call into freed memory at its next barrier - which is the
		// ordinary case, because a world is destroyed after the scripts that
		// built it.
		if (bounds != nullptr) {
			bounds->Context.Changes.Detach(Store);
		}

		// **And the removal hook, for exactly the same reason.** It captures
		// this VM's `lua_State *`, and a store that outlived the runtime would
		// call into a closed state the next time anything was destroyed. Taken
		// back unconditionally: a runtime that never installed one is clearing
		// an empty function.
		Store.ClearDescendantRemoving();

		lua_close(State);
		delete bounds;
	}

	bool LuauRuntime::Run(std::string_view source, std::string_view name) {
		Error.clear();

		// Compiled to bytecode first: Luau is a compiler and a VM, and they are
		// separate libraries on purpose. Optimisation at 1 and debug info at 1
		// - the default pairing, which keeps a usable traceback.
		lua_CompileOptions options = {};
		options.optimizationLevel = 1;
		options.debugLevel = 1;

		// **A breakpoint compiles the chunk differently, and it has to.** At
		// optimisation level 1 the compiler folds constants and drops the
		// instructions their lines would have produced - so `local x = 1` is not
		// a line execution ever arrives at, and a breakpoint on it would sit
		// there never firing while the script plainly ran past it. That is the
		// worst failure a debugger has: silently pointing at the wrong place.
		//
		// So stepping compiles at level 0 with full debug info, which is the
		// same trade `dev` already makes for C++ - the code does what it says
		// rather than what it was rewritten into. It applies only while a
		// breakpoint is armed, so nothing else in the world pays for it.
		if (Breakpoints.Armed()) {
			options.optimizationLevel = 0;
			options.debugLevel = 2;
		}

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
		// is exactly the scoping `script` needs - two scripts in one world each
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

		// A fresh budget for this chunk, and the counter keeps running. Moving
		// the mark rather than zeroing the count is what lets `RunWorldScripts`
		// subtract two readings and get what this script cost.
		BoundsOf(State).StepsBase = BoundsOf(State).StepsTaken;

		// **Single-step is switched on only while something is armed, and off
		// again the moment the chunk ends.** Luau checks a flag per instruction
		// when it is on, so leaving it on would tax every script in the world
		// for a breakpoint nobody set - and a debugger that costs something when
		// unused is one that gets switched off and then rots.
		//
		// On the thread rather than the state, because that is what runs.
		const bool stepping = Breakpoints.Armed();
		if (stepping) {
			BoundsOf(State).LastBreakLine = 0;
			BoundsOf(State).LastBreakSource.clear();
			lua_singlestep(thread, 1);
		}

		const int status = lua_resume(thread, nullptr, 0);

		if (stepping) {
			lua_singlestep(thread, 0);
		}

		// **A yield is legal now, and only from `task`.**
		//
		// `docs/retired/SCRIPT_CONCURRENCY.md` §1 permits a resume from a tick boundary,
		// a barrier delivery or a `Ticket` reply, and forbids everything else. A
		// thread that suspended through `task.wait` is registered for one of
		// those and something will come back for it. A thread that suspended any
		// other way found a route the sandbox did not intend, and finishing the
		// tick with it would be work crossing a tick boundary - which is exactly
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

	namespace {
		// `require(moduleScript)`.
		//
		// **The argument is an instance, never a path.** A path would be a second
		// namespace beside the tree - two ways to name one module, disagreeing the
		// first time somebody moved a file - and it would reach code no manifest
		// describes, which is what the old refusal was written to prevent. An
		// instance is already in the world, already saved, already replicated.
		int Require(lua_State *state) {
			LuauContext &context = UpvalueContext(state);

			void *value = lua_touserdatatagged(state, 1, TAG_INSTANCE);
			if (value == nullptr) {
				luaL_typeerrorL(state, 1, "ModuleScript");
			}

			const ecs::Entity module = *static_cast<ecs::Entity *>(value);
			ecs::Store &store = *context.World;

			if (!store.Alive(module) || !ecs::Classes::IsA(store.ClassOf(module), ModuleScriptClass())) {
				// Named rather than nil, for `GetService`'s reason: a script that
				// gets nil back fails one line later, somewhere that says nothing
				// about the cause.
				luaL_errorL(state, "require expects a ModuleScript");
			}

			// **Already evaluated: hand back the same value.** Not a fresh copy -
			// two scripts requiring one module must see one table, or a module
			// used to share state silently shares nothing.
			if (const auto found = context.Modules.find(module.Id); found != context.Modules.end()) {
				lua_getref(state, found->second);
				return 1;
			}

			// A cycle, named at both ends. Recursing instead would exhaust the C
			// stack and surface as a crash with no line number.
			for (const ecs::Entity loading : context.Loading) {
				if (loading == module) {
					luaL_errorL(
						state,
						"require cycle: '%s' is already being required",
						store.InstanceNameOf(module).Text().data()
					);
				}
			}

			// **Through the instance rather than the path**, so a module
			// required in a replica reads the `script::Program` row that
			// arrived. `require` is how a client's `LocalScript` reaches a
			// library, and the path alone finds nothing there.
			core::Name modulePath;
			std::string program;
			std::string error;
			if (!ReadProgram(store, module, modulePath, program, error)) {
				luaL_errorL(state, "%s", error.c_str());
			}

			lua_CompileOptions options = {};
			options.optimizationLevel = 1;
			options.debugLevel = 1;

			size_t bytecodeSize = 0;
			char *bytecode = luau_compile(program.data(), program.size(), &options, &bytecodeSize);
			if (bytecode == nullptr) {
				luaL_errorL(state, "the compiler produced nothing for '%s'", modulePath.Text().data());
			}

			// The same shape a `Script` gets: its own sandboxed thread, so a
			// module assigning a global does not leak into whoever required it.
			lua_State *thread = lua_newthread(state);
			luaL_sandboxthread(thread);

			PushInstanceValue(thread, module);
			lua_setglobal(thread, "script");

			const std::string chunkName = "=" + std::string(modulePath.Text());
			const int loaded = luau_load(thread, chunkName.c_str(), bytecode, bytecodeSize, 0);
			std::free(bytecode);

			if (loaded != 0) {
				const char *message = lua_tostring(thread, -1);
				luaL_errorL(state, "%s", message != nullptr ? message : "could not load the module");
			}

			context.Loading.push_back(module);
			const int status = lua_resume(thread, nullptr, 0);
			context.Loading.pop_back();

			if (status == LUA_YIELD) {
				// **A module may not yield**, and the reason is the one every
				// yield here answers to: the value is cached the moment it is
				// produced, so a module suspended half way would leave every
				// later `require` waiting on a value that arrives at a tick
				// nobody chose.
				luaL_errorL(state, "a ModuleScript may not yield while it is being required");
			}
			if (status != LUA_OK) {
				const char *message = lua_tostring(thread, -1);
				luaL_errorL(state, "%s", message != nullptr ? message : "the module raised");
			}

			// Roblox requires exactly one value back. Nothing returned is the
			// mistake an author makes once, and it reads as `nil` three files
			// away unless it is refused here.
			if (lua_gettop(thread) < 1) {
				luaL_errorL(
					state,
					"'%s' returned nothing - a ModuleScript must return one value",
					store.InstanceNameOf(module).Text().data()
				);
			}

			lua_xmove(thread, state, 1);

			// Referenced before it is handed over, so the cache owns a value the
			// collector cannot take while a script is still holding it.
			lua_pushvalue(state, -1);
			context.Modules[module.Id] = lua_ref(state, -1);
			lua_pop(state, 1);

			return 1;
		}
	}

	void OpenRequire(lua_State *state) {
		LuauContext &context = ContextOf(state);

		lua_pushlightuserdata(state, &context);
		lua_pushcclosure(state, Require, "require", 1);
		lua_setglobal(state, "require");
	}

	uint64_t LuauRuntime::StepsTaken() const {
		if (State == nullptr) {
			return 0;
		}
		return BoundsOf(State).StepsTaken;
	}

	bool LuauRuntime::RunInstance(ecs::Entity instance) {
		Error.clear();

		// **The active container** - `script::CodeSourceContainerSelector` says
		// which of an instance's programs is the one to run.
		if (!ActiveSourceOf(Store, instance).IsValid()) {
			// A script with no path is a no-op rather than an error: an author
			// makes the instance before choosing the file, and that is a legal
			// state.
			return true;
		}

		// **The world's `SourceCache`, then the row the authority sent, then the
		// filesystem**, through the one function that knows all three. A
		// studio's unsaved edit lives in that table, a game file's scripts
		// arrive in it, and a replica has neither - so a second resolver here
		// would be a second place to forget one of them, and the symptom would
		// be code that runs from one entry point and not another.
		core::Name path;
		std::string program;
		if (!ReadProgram(Store, instance, path, program, Error)) {
			return false;
		}

		// Which script a captured hit came from. Cleared after the run, because
		// a heartbeat connection is not attributable to one - see
		// `LuauContext::RunningScript`.
		ContextOf(State).RunningScript = instance;

		// **`script` names the instance, and it is set before the chunk runs.**
		// That is the whole difference from `RunFile`: a chunk run this way can
		// reach its own parent and its own siblings, which is what makes a game
		// of many scripts possible rather than one file that builds a world.
		//
		// A plain global rather than an upvalue, because `luaL_sandboxthread`
		// gives each chunk its own global table - so one script's `script` is
		// invisible to the next, which is exactly the scoping an author expects.
		ContextOf(State).PendingScript = instance;

		const bool ok = Run(program, path.Text());
		ContextOf(State).PendingScript = ecs::NULL_ENTITY;
		return ok;
	}

	bool LuauRuntime::Heartbeat(float delta) {
		// **One budget for the whole beat.** Every connection, every resumed
		// task and every delivery handler spends the same one, because a budget
		// refreshed per handler bounds no tick: a script gets as many of them as
		// it can arrange to be called.
		BoundsOf(State).StepsBase = BoundsOf(State).StepsTaken;

		// **The order is the tick's own, and every step of it is a rule.**
		//
		//   1. deliveries - a message the barrier applied belongs to the tick
		//      that is starting, so a subscriber sees it before anything that
		//      beat moves. The alternative is a world reacting one tick late to
		//      something it already has.
		//   2. changes - what the *previous* barrier recorded, fanned out from
		//      components to property names. Before the beat, so a `.Changed`
		//      handler and a `Heartbeat` handler see the same world.
		//   3. tasks - resumes due at this tick, then everything deferred from
		//      the last beat.
		//   4. the beat itself.
		//
		// Each of the first three is one of §1's three legal resume sources, in
		// the order the barrier produced them.
		//
		// **A span each, because one span over all four answers nothing.** The
		// four do entirely different work - a bus drain, a signal fan-out, a
		// coroutine resume and a script's own beat - and they fail differently:
		// deliveries scale with traffic, changes with how much of the world
		// moved, tasks with how many are due, and the beat with what the game
		// wrote. A single `script heartbeat` bar that spikes says only that
		// something did, which is where this was before a spike had to be found
		// in `Mirrors-1-world` and nothing under it could be read.
		{
			// **Before everything, because what it produces is read after the
			// barrier rather than inside it.** The `script::Program` rows are
			// what a client is sent, and `replication::Authority::Publish` runs
			// once the tick is over - so a program edited between two ticks has
			// to be mirrored at the head of this one or a client waits a tick
			// for every save. It is also the cheapest thing in the beat: a
			// 64-bit compare unless somebody wrote to the cache.
			ENGINE_PROFILE_CAT("script source", core::ProfileCategory::Script);
			MirrorSourcePrograms(Store, Mirrored);
		}

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
			// **The world's own timed work, before anything that reacts to it.**
			// A tween and a debris deadline are not resumes: nothing asked to be
			// called, and what they produce is a property write and a removal -
			// the same things a `Heartbeat` handler produces, arriving from the
			// clock instead of from a script.
			//
			// So they run at the head of the barrier, and everything after them
			// - a bound action, a `.Changed`, a tree signal, a resumed task, the
			// beat - sees one world in which this tick's motion has already
			// happened. After the beat instead, every script would read a value
			// one tick stale and an expired instance would outlive its deadline
			// by a tick as far as anything could tell.
			//
			// It also settles what a tween made *during* this barrier does: it
			// first advances on the next one, rather than part way through the
			// tick it was created in, because every creator runs after this
			// point.
			//
			// **Tweens before debris**, so an instance's last tick of motion
			// happens before it is taken away. The reverse is defensible and the
			// point is that it is stated: two queues drained in whichever order
			// the code happened to be written in is exactly the sort of thing a
			// recording depends on and nobody wrote down.
			ENGINE_PROFILE_CAT("script tweens", core::ProfileCategory::Script);
			note(PumpTweens(State, delta));
			PumpDebris(Store, ContextOf(State).Debris);
		}

		{
			// **Input first, and that ordering is the useful one.** A bound
			// action's handler writes properties, and those writes should reach
			// their listeners on *this* barrier rather than the next - so the pump
			// that produces writes runs before the pump that delivers them.
			//
			// The reverse order works and puts every scripted response a frame
			// late, which is the kind of latency nobody can find by reading a
			// handler.
			//
			// **It is handed this beat's interface events even though it
			// dispatches none of them**, which is what `gameProcessedEvent` is: a
			// click the 2D tree consumed has to arrive at `InputBegan` marked
			// rather than swallowed or passed as though nobody handled it. They
			// are still queued at this point and `PumpGuiEvents` below drains
			// them - reading the queue twice is what lets the two pumps stay one
			// each.
			ENGINE_PROFILE_CAT("script input", core::ProfileCategory::Script);
			note(PumpInput(State, PendingGuiEvents));
		}
		{
			ENGINE_PROFILE_CAT("script changes", core::ProfileCategory::Script);
			note(PumpChanges(State));
		}
		{
			// **After the property changes and before the tasks**, which puts
			// it inside step 2 of the order above rather than beside it: both
			// are "what the previous barrier recorded", and a handler watching
			// a part's `Position` and its `AncestryChanged` should see one
			// world rather than two.
			ENGINE_PROFILE_CAT("script tree", core::ProfileCategory::Script);
			note(PumpTree(State));

			// **The tree's other listener, and the only one that is a resume.**
			// A `WaitForChild` wakes on the same arrival the signals above just
			// delivered, so it belongs inside this step rather than beside it -
			// and it goes second, because a signal every listener shares is a
			// worse thing to be one tick late with than one script's own wait.
			//
			// Its own span would say almost nothing: the pump does nothing at
			// all unless something is suspended, which is the ordinary state of
			// a world.
			note(PumpChildWaiters(State));
		}
		{
			// **After the tree, because a respawn is both.** A new character is
			// a `Model` parented into `Workspace` - which is a `ChildAdded` the
			// pump above delivers - and a link written onto the `Player`, which
			// is this one. A `CharacterAdded` handler indexing
			// `character.Humanoid` should find a world whose tree signals have
			// already agreed the model is there.
			ENGINE_PROFILE_CAT("script characters", core::ProfileCategory::Script);
			note(PumpCharacters(State));
		}
		{
			// **Still inside step 2 - "what the previous barrier recorded" -
			// and last within it.** A pointer event is about an element whose
			// rectangle came from the last frame's layout, so it belongs with
			// the other things the world already settled rather than with the
			// beat that is about to move it.
			//
			// After the tree rather than before, because a click that reparents
			// something should see the world its `.Changed` and
			// `AncestryChanged` handlers already agreed on. Before the tasks,
			// because a handler here may `task.defer`, and a resume scheduled by
			// a click belongs to this tick rather than the next.
			//
			// **Taken before a single handler runs, so the span cannot dangle.**
			// `PumpGuiEvents` walks what it is given; handing it the live vector
			// would mean a `DeliverGuiEvents` arriving mid-pass could reallocate
			// under the walk.
			//
			// No script can cause that today - only a host calls
			// `DeliverGuiEvents`, and a host is inside this call. So this is
			// structural rather than load-bearing, and a test cannot reach it:
			// removing the swap keeps the suite green. It stays because the
			// cost is one move of a vector per beat and the failure it prevents
			// is a use-after-free rather than a wrong answer.
			ENGINE_PROFILE_CAT("script gui", core::ProfileCategory::Script);
			std::vector<gui::GuiEvent> events;
			events.swap(PendingGuiEvents);
			note(PumpGuiEvents(State, events));
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
		// pause inside whichever `Vector3.new` happened to cross the threshold -
		// so it lands *inside* the beat's span and is invisible as itself. A
		// scene animating parts allocates hard: `Mirrors-1-world` builds two
		// vectors and two CFrames per caster per frame, which at 24 casters and
		// 60 Hz is thousands of objects a second, and the collector's cost shows
		// up as a beat that is occasionally slow for no reason the beat can
		// explain.
		//
		// Stepping it here does not stop the automatic collector - it means part
		// of the debt is paid where it can be seen and named. What it costs is
		// one span a tick.
		{
			ENGINE_PROFILE_CAT("script gc", core::ProfileCategory::Script);
			lua_gc(State, LUA_GCSTEP, 1);
		}

		Error = firstError;
		return Error.empty();
	}

	void LuauRuntime::SetHost(HostSurface *host) {
		// **Recorded on the context and the global rebuilt**, because
		// `OpenHost` reads `Names()` once: a host installed after a chunk had
		// already captured `host` would be invisible to it, which is `D00030`'s
		// mechanism biting a different surface.
		ContextOf(State).Host = host;
		OpenHost(State);
	}

	bool LuauRuntime::Invoke(HostCallback callback, HostArguments arguments) {
		// A host call is its own entry from its own frame rather than part of a
		// tick, so it gets its own budget - the same grant `Run` and `Heartbeat`
		// make, and for the same reason: the mark moves, the counter does not.
		BoundsOf(State).StepsBase = BoundsOf(State).StepsTaken;
		return CallHostCallback(State, callback, arguments);
	}

	void LuauRuntime::Release(HostCallback callback) {
		ReleaseHostCallback(State, callback);
	}

	ScriptSurface LuauRuntime::Surface() const {
		ScriptSurface surface;
		if (State == nullptr) {
			return surface;
		}

		// **A host entry, so it gets its own budget** - the same grant `Run`,
		// `Heartbeat` and `Invoke` make. The walk below passes safepoints, and a
		// runtime whose last script blew its budget stays tripped until
		// something moves the mark: without this, opening the completion list
		// after a runaway script would hand back an empty one.
		BoundsOf(State).StepsBase = BoundsOf(State).StepsTaken;

		// **The globals table pushed as a value rather than walked through
		// `LUA_GLOBALSINDEX` directly.** `lua_next` wants a real stack slot; a
		// pseudo-index happens to work in this VM and is the sort of thing that
		// stops working quietly, and a completion list that silently empties is
		// indistinguishable from an engine with no API.
		lua_pushvalue(State, LUA_GLOBALSINDEX);

		lua_pushnil(State);
		while (lua_next(State, -2) != 0) {
			// Key at -2, value at -1. Only string keys are names a script could
			// write; anything else is somebody using the globals table as a map.
			if (lua_type(State, -2) == LUA_TSTRING) {
				VocabularyEntry entry;
				entry.Name = lua_tostring(State, -2);

				switch (lua_type(State, -1)) {
				case LUA_TFUNCTION:
					entry.Kind = NameKind::Function;
					break;
				case LUA_TTABLE:
					entry.Kind = NameKind::Container;
					entry.Members = TableMembers(State, -1);
					break;
				default:
					entry.Kind = NameKind::Value;
					break;
				}

				surface.Globals.push_back(std::move(entry));
			}

			// The value goes, the key stays for the next step.
			lua_pop(State, 1);
		}

		lua_pop(State, 1);

		// **The same registry table `OpenInstances` filled**, so a method added
		// there is offered here with nothing else changing - and the five
		// `LuauEcs` appends to it arrive for free, which a list of method names
		// kept anywhere else would have missed.
		lua_getfield(State, LUA_REGISTRYINDEX, "engine.instance.methods");
		surface.InstanceMembers = TableMembers(State, -1);
		lua_pop(State, 1);

		// The signals, which are a branch chain rather than a table. This is the
		// one member list in the module that is written down; `LuauInstances.cpp`
		// keeps it beside the chain and `engine.script.vocabulary` checks it.
		for (const std::string_view signal : LuauInstanceSignalNames()) {
			surface.InstanceMembers.emplace_back(signal);
		}

		return surface;
	}

	std::unique_ptr<Runtime> MakeLuauRuntime(ecs::Store &store, const RuntimeLimits &limits) {
		return std::make_unique<LuauRuntime>(store, limits);
	}
}
