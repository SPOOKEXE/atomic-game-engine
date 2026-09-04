#pragma once

// What a script can hold and what it can touch, on the Luau side.
//
// Two halves, and they meet at `PropertyType`. The value types are what a
// property's bytes mean on the script side; the instance binding is what turns
// a name and one of those values into `Store::SetProperty`.
//
// **The marshalling is a switch over `PropertyType` and nothing else.** No
// per-property code, no table of special cases: a property added to `scene`
// tomorrow is reachable from Luau today, because the binding never learned any
// property's name. That is the payoff for making a property a conversion.

#include <engine/core/Random.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Color3.hpp>
#include <engine/core/types/NumberRange.hpp>
#include <engine/core/types/Rect.hpp>
#include <engine/core/types/Sequence.hpp>
#include <engine/core/types/TweenInfo.hpp>
#include <engine/core/types/UDim.hpp>
#include <engine/core/types/Vector2.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/script/Actions.hpp>
#include <engine/script/Bus.hpp>
#include <engine/script/Changes.hpp>
#include <engine/script/ChildWaiters.hpp>
#include <engine/script/Codec.hpp>
#include <engine/script/ComputeJobs.hpp>
#include <engine/script/Debris.hpp>
#include <engine/script/Debugger.hpp>
#include <engine/script/EditableMeshJobs.hpp>
#include <engine/script/Host.hpp>
#include <engine/script/LuauTags.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/script/Scope.hpp>
#include <engine/script/ScriptCall.hpp>
#include <engine/script/ServiceCatalogue.hpp>
#include <engine/script/ServiceSurface.hpp>
#include <engine/script/Signals.hpp>
#include <engine/script/Tasks.hpp>
#include <engine/script/Tweens.hpp>

#include <algorithm>
#include <lua.h>
#include <lualib.h>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace engine::script {

	// Everything one Luau runtime needs, hung off the state rather than a
	// static.
	//
	// **One runtime, one world.** Two runtimes over two worlds must not be able
	// to reach each other's storage, and a file-static would have made that
	// mistake available - the sort that works until the second world exists.
	// Every bound function reaches this through a light-userdata upvalue.
	//
	// @since v0.6
	struct LuauContext {
		// One C closure the runtime owns and names for binding profiling.
		//
		// A Luau function pointer has no portable conversion to data, so the
		// closure holds one pointer to this record instead of trying to smuggle a
		// function pointer through light userdata. `Bindings` owns the records,
		// which keeps the closure's upvalue valid for the VM's lifetime.
		struct Binding {
			lua_CFunction Function = nullptr;
			std::string Name;
		};

		// Every profiled C closure this VM installed.
		std::vector<std::unique_ptr<Binding>> Bindings;

		// The world this VM builds into.
		ecs::Store *World = nullptr;

		// What the host is, for `RunService:IsServer()` and friends.
		HostRole Role;

		// The runtime-owned, VM-neutral source profiler. The adapter samples this
		// only while Studio enables it, so a normal script tick never enters the
		// VM's instruction callback.
		ScriptProfiler *Profiler = nullptr;

		// The services and host seams this runtime may reach.
		ScriptCapabilities Access = ScriptCapabilities::None;

		// Connections, and the ordering rules both VMs share.
		SignalTable Signals;

		// Cleanup scopes own their shared lifecycle here; Luau owns the refs.
		ScopeTable Scopes;

		// Reused by the two bulk placement methods. The runtime is single-threaded,
		// and each call consumes these before another script method can replace them.
		std::vector<ecs::Entity> PlacementInstances;
		std::vector<core::CFrame> PlacementFrames;

		// What changed since the last barrier, as property names.
		ChangeQueue Changes;

		// Suspended threads and when they may run again.
		TaskQueue Tasks;

		// What `ContextActionService` has bound, highest priority first.
		//
		// **Shared machinery since v0.16, beside `Signals` and `Changes` and for
		// their reason.** The stack's rules - replace by name, stable sort by
		// priority, first claim wins - decide which handler a press reaches, and
		// two copies of that would agree until one was fixed. See `Actions.hpp`;
		// the callables stay this VM's, as registry refs inside a `CallbackRef`.
		ActionStack Actions;

		// Who is listening to which bus topic.
		//
		// **Shared machinery beside `Actions`, and it was a registry table
		// until v0.16.** The topic-to-callbacks map is the whole of what
		// `MessagingService:SubscribeAsync` records, it names no VM, and keeping
		// it in `engine.messaging.subscriptions` was what stopped that service
		// being described once. See `Bus.hpp`.
		TopicSubscriptions Subscriptions;

		// The VM this context belongs to, so a `Connection` userdata can reach
		// the table without a second upvalue on every method.
		lua_State *State = nullptr;

		// Whether `Store::OnDescendantRemoving` already holds this VM's hook.
		//
		// The store keeps one listener, so a second install would replace the
		// first with an identical one - harmless, and still worth not doing
		// once per connection.
		bool RemovingHooked = false;

		// Where execution should be reported from, owned by the `Runtime`.
		//
		// A pointer rather than a copy: breakpoints are edited from the editor
		// while the world runs, and a copy taken at construction would be a
		// second answer to what is armed.
		Debugger *Breakpoints = nullptr;

		// What each `ModuleScript` evaluated to, by entity id.
		//
		// **A registry ref per module, and the module runs once.** Roblox's rule:
		// every `require` of one module hands back the same value, so a module
		// with a side effect at its top level has that side effect once - on
		// whichever script required it first. A map that re-ran would make
		// module order something an author had to reason about.
		//
		// Keyed by `Entity::Id` rather than by path, because two instances may
		// name one file and they are two modules. That is what makes a module a
		// thing in the tree rather than a thing on disk.
		std::unordered_map<uint64_t, int> Modules;

		// Modules part way through evaluating, innermost last.
		//
		// **Cycle detection, and it has to be by instance.** `a` requiring `b`
		// requiring `a` would otherwise recurse until the C stack ran out, which
		// surfaces as a crash with no line number rather than as a script error
		// naming the two files.
		std::vector<ecs::Entity> Loading;

		// The script currently being run by `RunInstance`, so a captured hit can
		// say which one it came from.
		//
		// Set around one run and cleared after it. Null during a heartbeat,
		// which is honest - the connection that is running was made by a script
		// and nothing records which, so naming one would be a guess.
		ecs::Entity RunningScript;

		// The script instance the next chunk belongs to.
		//
		// **Set before `Run` and consumed by it**, rather than assigned as a
		// global here - `luaL_sandboxthread` gives each chunk its own global
		// table, so `script` has to be written onto the thread after it is
		// sandboxed and not onto the state before. That per-chunk scoping is the
		// point: one script's `script` is invisible to the next.
		ecs::Entity PendingScript;

		// Which registry ref holds each suspended thread.
		//
		// `TaskQueue` holds a `CallbackRef` and a script holds a thread object;
		// this is what joins the two, and it is what makes `task.cancel`
		// possible at all. Keyed on the thread's `lua_State *`, which is its
		// identity for as long as the ref keeps it alive.
		std::unordered_map<lua_State *, CallbackRef> Threads;

		// How many ticks a `task.wait` asked for, so the resume can report how
		// long it actually waited.
		std::unordered_map<lua_State *, uint64_t> WaitTicks;

		// How many arguments a `task.defer` or `task.delay` left on its
		// thread's stack, so the resume passes them on.
		std::unordered_map<lua_State *, int> PendingArguments;

		// What this program offers beyond the world, or null.
		//
		// **Null for a game script and set for a tool**, which is the whole of
		// the distinction: a game's vocabulary is the scene and an editor tool's
		// is the program running it. See `script/Host.hpp`.
		HostSurface *Host = nullptr;

		// Which registry ref holds each function a host was handed.
		//
		// **Keyed by a counter rather than by an address**, because an address
		// is not stable between runs and this module refuses one in an id
		// anywhere. Starting at one leaves zero meaning "no callback".
		//@{
		std::unordered_map<uint64_t, int> HostCallbacks;
		uint64_t NextHostCallback = 0;
		//@}

		// Which thread is suspended on which `world::Ticket`.
		//
		// **The first of `docs/retired/SCRIPT_CONCURRENCY.md` §1's three legal resume
		// sources**, made concrete. A `GetAsync` returns a ticket, the reply
		// lands at a later barrier applied in sorted order, and this is what
		// joins the reply back to the script waiting for it. Keyed on the
		// ticket's value, which is per world and monotonic.
		std::unordered_map<uint64_t, lua_State *> AwaitedTickets;

		// Which thread is suspended on which `ChildWaiters` entry.
		//
		// **The second resume source, and it is a second map rather than a
		// widened first one.** A bus reply is named by a `world::Ticket` that the
		// world hands out; a `WaitForChild` is named by an id this VM's own table
		// hands out, and the two counters know nothing about each other - so one
		// map would be two number spaces sharing a key. What resumes each is
		// different too: `PumpDeliveries` pushes `(value, status, version)` and
		// `PumpChildWaiters` pushes one instance or nil.
		std::unordered_map<uint64_t, lua_State *> AwaitedChildren;

		// Complete geometry transactions and the threads waiting on their ordered
		// owner-thread commits. This is a distinct key space from tickets and child
		// waiters, so it has a distinct table.
		EditableMeshJobs EditableMeshes;
		std::unordered_map<uint64_t, lua_State *> AwaitedEditableMeshes;

		// Typed work that may outlive the submitting heartbeat. The queue owns
		// every worker and the map owns only suspended VM threads.
		ComputeJobs Computations;
		std::unordered_map<uint64_t, lua_State *> AwaitedComputations;

		// How many GUIDs `HttpService:GenerateGUID` has handed out.
		//
		// **A counter rather than a clock or an entropy source, and that is what
		// makes a GUID replayable.** `HttpService.cpp` carries the whole
		// argument; what it needs from here is a draw number that advances the
		// same way on every run of the same script. On the context rather than a
		// file-static for this file's own reason - two runtimes over two worlds
		// must not share one.
		uint64_t NextGuid = 0;

		// Every tween this VM has made, and what is waiting to be destroyed.
		//
		// **Beside `Signals`, `Changes` and `Tasks`, and for their reason.**
		// Both step on the fixed tick delta at the barrier and both drain in a
		// stated order, so both are shared machinery the two languages must not
		// have two copies of - see `Tweens.hpp` and `Debris.hpp`.
		//@{
		TweenTable Tweens;
		DebrisQueue Debris;
		//@}

		// Every `WaitForChild` this VM has outstanding.
		//
		// **Beside the two above and for their reason**: a wait's deadline is a
		// tick number and the order two of them are answered in is a thing a
		// recording depends on, so there is one implementation and each binding
		// supplies only the resume. See `ChildWaiters.hpp`.
		ChildWaiters Waiters;
	};

	// Reports whether a thread has a scheduled resume.
	//
	// What tells a legal yield from an illegal one: a thread that suspended
	// through `task` is waiting for a tick boundary, which
	// `docs/retired/SCRIPT_CONCURRENCY.md` §1 permits. One that suspended any other way
	// found a route out of the sandbox and is refused.
	//
	// @param context The VM's context.
	// @param thread  The suspended thread.
	// @return `true` when something will resume it.
	bool ThreadIsScheduled(LuauContext &context, lua_State *thread);

	// The context bound to a state.
	//
	// @param state The VM, or one of its threads.
	// @return The context.
	LuauContext &ContextOf(lua_State *state);

	// The context on a bound function's first upvalue.
	//
	// @param state The VM inside a bound C function.
	// @return The context.
	LuauContext &UpvalueContext(lua_State *state);

	// Calls a binding after its profiler closure recovered the original function.
	//
	// The wrapper records only normal returns. Luau errors and coroutine yields
	// leave through non-local control flow, so a C++ scope would not be closed
	// safely on those paths.
	int InvokeProfiledBinding(lua_State *state);

	// Enables a VM step callback for one resume when either the debugger or the
	// source profiler needs it, then closes the profile interval afterwards.
	void PrepareProfiledResume(lua_State *state, lua_State *from);
	void FinishProfiledResume(lua_State *state, int status);

	// The raw Luau resume call is kept above the macro below. Every adapter
	// resume goes through the two hooks, including a coroutine resumed by a
	// task, signal, delivery or compute completion.
	inline int ResumeProfiledLua(lua_State *state, lua_State *from, int arguments) {
		PrepareProfiledResume(state, from);
		const int status = lua_resume(state, from, arguments);
		FinishProfiledResume(state, status);
		return status;
	}

	// Pushes a profiler wrapper around `function` while preserving its existing
	// upvalues after the wrapper's record at index one.
	inline void
	PushProfiledClosure(lua_State *state, lua_CFunction function, const char *name, int upvalues) {
		LuauContext &context = ContextOf(state);
		auto binding = std::make_unique<LuauContext::Binding>();
		binding->Function = function;
		// The shared prefix keeps the bindings flame graph independent of a VM.
		// JavaScript and C# adapters can use `binding.<language>.<member>` too.
		binding->Name = "binding.luau.";
		binding->Name += name != nullptr ? name : "binding";
		LuauContext::Binding *held = binding.get();
		context.Bindings.push_back(std::move(binding));

		lua_pushlightuserdata(state, held);
		if (upvalues > 0) {
			lua_insert(state, -upvalues - 1);
		}
		lua_pushcclosure(state, InvokeProfiledBinding, name, upvalues + 1);
	}

	// Pushes a profiler wrapper around a binding with no existing upvalues.
	inline void PushProfiledFunction(lua_State *state, lua_CFunction function, const char *name) {
		PushProfiledClosure(state, function, name, 0);
	}

	// Installs a `luaL_Reg` array through the profiler rather than
	// `luaL_register`, whose implementation creates raw closures inside the VM.
	// The caller has already pushed the target table.
	inline void RegisterProfiledFunctions(lua_State *state, const luaL_Reg *functions) {
		for (const luaL_Reg *entry = functions; entry->name != nullptr; entry++) {
			PushProfiledFunction(state, entry->func, entry->name);
			lua_setfield(state, -2, entry->name);
		}
	}

	// --- values ---------------------------------------------------------------

	// How wide one marshalled value can be, and therefore how big the stack
	// buffers that carry one are.
	//
	// **This was `sizeof(core::CFrame)` until v0.10 added the sequences.** A
	// `core::ColorSequence` is twenty keypoints and does not fit in twenty-eight
	// bytes, and every guard is `size > sizeof(bytes)` - so an emitter's `Color`
	// failed its read with "could not read 'Color'", naming a property that is
	// declared and readable and whose only problem was a buffer one file away.
	//
	// A named constant rather than a `sizeof` at each buffer, because there are
	// several and a getter's being narrower than the setter's is a bug with no
	// symptom on the setter.
	constexpr size_t WIDEST_PROPERTY = std::max(sizeof(core::ColorSequence), sizeof(core::NumberSequence));

	// Pushes a value of one `PropertyType`, read into a buffer of its size.
	//
	// **The type and the enum name, never a descriptor**, because the second
	// caller has no descriptor to give: an ECS component field carries exactly
	// these values and is not a property. One switch rather than two that agree
	// until somebody edits one.
	//
	// `PropertyType::String` is refused rather than handled, and the refusal is
	// the design: `bytes` is uninitialised storage and a `std::string` cannot be
	// assigned into that, so every caller takes strings down a path of its own.
	// Failing loudly is what catches a future caller that forgot the branch.
	//
	// @param state    The VM.
	// @param type     What the bytes mean.
	// @param enumName Which set an `Enum` value belongs to. Ignored otherwise.
	// @param bytes    The value.
	// @return `false` when the type has no script representation.
	// @since v0.12
	bool PushPropertyValue(lua_State *state, ecs::PropertyType type, core::Name enumName, const void *bytes);

	// Reads a Luau value into a buffer of that type's size.
	//
	// Raises a Luau type error for a value of the wrong shape, exactly as the
	// `Check*` helpers below do - so a caller checks the return for "this type
	// cannot cross" and never for "the script passed the wrong thing".
	//
	// @param state    The VM.
	// @param index    The stack index to read.
	// @param type     What to read it as.
	// @param enumName Which set an `Enum` value must belong to.
	// @param out      Where to write it. At least `Schemas::SizeOf(type)` bytes.
	// @return `false` when the type has no script representation, or when an
	//         `Enum` value belongs to a different set.
	// @since v0.12
	bool
	ReadPropertyValue(lua_State *state, int index, ecs::PropertyType type, core::Name enumName, void *out);

	// Installs `Vector3`, `Color3` and `CFrame` as globals.
	void OpenValues(lua_State *state);

	// Installs v0.6's datatypes: `Vector2`, `UDim`, `UDim2`, `Rect`, `Region3`,
	// `NumberRange`, `NumberSequence`, `ColorSequence`, `TweenInfo`, `Ray` and
	// `Random`.
	//
	// **Each is a shim over what the engine already holds**, exactly as
	// `Vector3` and `CFrame` are, and none of them is a component. `Region3` is
	// `core::AABB` and `Ray` is `core::Ray`: the engine had both, and adding a
	// second spelling of either would have been the duplicate the root
	// `AGENTS.md` calls the most expensive kind of debt.
	//
	// @param state The VM.
	void OpenDatatypes(lua_State *state);

	// Pushes a fresh value of each type and returns it for filling in. The
	// instance binding uses these to hand a property's bytes back to a script.
	core::Vector3 *PushVector3(lua_State *state);
	core::Color3 *PushColor3(lua_State *state);
	core::CFrame *PushCFrame(lua_State *state);

	// Reads a value of each type, raising a Luau type error when the argument
	// is something else. The tag is what makes this safe: `Vector3` and
	// `Color3` are the same three floats, so a check on shape would pass.
	core::Vector3 &CheckVector3(lua_State *state, int index);
	core::Color3 &CheckColor3(lua_State *state, int index);
	core::CFrame &CheckCFrame(lua_State *state, int index);

	// The four `gui` is authored in. Same split as the three above: the
	// metatable is `LuauDatatypes.cpp`'s and these only carry a property's
	// bytes across.
	//
	// `CheckVector2Value` rather than `CheckVector2` because that name is
	// already taken by a file-local in `LuauDatatypes.cpp` doing exactly this;
	// merging the two is a tidy-up worth doing and not one to do inside the
	// change that adds four property types.
	core::Vector2 *PushVector2(lua_State *state);
	core::UDim *PushUDim(lua_State *state);
	core::UDim2 *PushUDim2(lua_State *state);
	core::Rect *PushRect(lua_State *state);

	core::Vector2 &CheckVector2Value(lua_State *state, int index);
	core::UDim &CheckUDim(lua_State *state, int index);
	core::UDim2 &CheckUDim2(lua_State *state, int index);
	core::Rect &CheckRect(lua_State *state, int index);

	// The curve a tween is authored with. Raises for anything else, exactly as
	// the four above do.
	core::TweenInfo &CheckTweenInfoValue(lua_State *state, int index);

	// --- the three a curve is authored in -------------------------------------
	//
	// The same split again: the metatables are `LuauDatatypes.cpp`'s and these
	// hand a property's bytes across. See `LuauValues.cpp` for what makes these three
	// different from the seven above - they are hundreds of bytes rather than a
	// handful of floats.

	core::NumberRange *PushNumberRange(lua_State *state);
	core::NumberSequence *PushNumberSequence(lua_State *state);
	core::ColorSequence *PushColorSequence(lua_State *state);

	core::NumberRange &CheckNumberRange(lua_State *state, int index);
	core::NumberSequence &CheckNumberSequence(lua_State *state, int index);
	core::ColorSequence &CheckColorSequence(lua_State *state, int index);

	// --- the codec bridge -----------------------------------------------------
	//
	// **One answer to "what is a Lua table", reached from two places.** These are
	// defined in `LuauBus.cpp`, beside the pump `MessagingService` was the first
	// caller from,
	// and they are declared here because they must not gain a second
	// implementation: `Codec.hpp` decides which tables are arrays, what a cycle
	// is and what a key becomes, and `HttpService:JSONEncode` has to give the
	// same answers or one value crosses a bus in one shape and lands in a JSON
	// document in another.

	// Reads a Luau value at `index` into a `ScriptValue`.
	//
	// **The sort is not here**: this walks a table in whatever order Luau offers
	// and the writer sorts, which is the arrangement `Codec.hpp` argues for - a
	// caller that had to remember to sort would be a second place the
	// determinism guarantee could be lost.
	//
	// @param state The VM.
	// @param index The stack index to read. Absolute, not relative.
	// @param out   Filled in.
	// @param depth How deep this value sits. Zero at the top.
	// @param why   Set when the answer is `false`.
	// @return `false` when the value has no representation.
	bool ReadScriptValue(lua_State *state, int index, ScriptValue &out, uint32_t depth, CodecStatus &why);

	// Pushes a `ScriptValue` as the Luau value it names.
	//
	// Arrays land as **one-based** tables, because `#` and `ipairs` mean
	// one-based in Luau and a zero-based array handed to a script is a list whose
	// first element is invisible.
	//
	// @param state The VM.
	// @param value What to push.
	void PushScriptValue(lua_State *state, const ScriptValue &value);

	// --- signals --------------------------------------------------------------

	// Pushes a signal object onto the stack.
	//
	// **A handle, not a list.** The connections live in `SignalTable`, so two
	// scripts holding "the same signal" are holding the same thing rather than
	// two objects that behave alike.
	//
	// @param state    The VM.
	// @param kind     Which signal.
	// @param subject  The instance, or `NULL_ENTITY` for a world signal.
	// @param property The property to filter on, for `PropertyChanged`.
	void PushSignal(lua_State *state, SignalKind kind, ecs::Entity subject, core::Name property = {});

	// Installs the `RBXScriptSignal` and `RBXScriptConnection` metatables.
	//
	// @param state The VM.
	void OpenSignals(lua_State *state);
	void OpenScopes(lua_State *state);

	// Calls everything connected to one signal, with the arguments already on
	// the stack.
	//
	// **`property` is the same filter `Connection::Property` carries**, and it is
	// how one `SignalKind` serves a set of names told apart at fire time:
	// `PropertyChanged` does it for a property and `CrossWorldMessage` for a
	// channel. An invalid name fires every connection on the signal, which is what
	// a kind with one meaning wants and what every caller but the channel pump
	// passes.
	//
	// @param state     The VM.
	// @param kind      Which signal.
	// @param subject   The instance, or `NULL_ENTITY`.
	// @param arguments How many values on top of the stack to pass.
	// @param property  Fire only connections carrying this name, or all of them
	//        when it is invalid.
	// @return An error message when a handler raised, or empty.
	std::string FireSignal(
		lua_State *state, SignalKind kind, ecs::Entity subject, int arguments, core::Name property = {}
	);

	// --- services -------------------------------------------------------------
	//
	// **A surface service is a global table, and there is one way to build one.**
	// `scene/Services.hpp` states the other kind - a *container* service is an
	// instance in the tree with children, properties and a row in the save file,
	// and `scene::InstallServices` builds those ten from one table. This is the
	// half that had no such table: nine engine surfaces spread over five files,
	// each hand-rolling the same ten lines of `lua_newtable`, push-closure loop
	// and `lua_setglobal`, over two different method-array types.
	//
	// Five copies of ten lines is not a crisis on its own. What it costs is that
	// there was nowhere to state a rule about services and have it hold: every
	// method takes the context as upvalue 1 and every one of the five had to
	// remember, `game:GetService(name)` and the global must be **one** object and
	// nothing said so, and adding a tenth service meant copying a sixth.
	//
	// **What a service *is* moved out to `ServiceSurface.hpp` at v0.16**, and the
	// move is what let five services reach JavaScript: a description with no VM in
	// it is one `ServiceCatalogue.cpp` can read twice. `InstallService` and the
	// surface types are declared there.

	// Builds one surface service and sets it as a Luau global.
	//
	// **Every method gets the runtime's `LuauContext` as upvalue 1**, which is
	// how any of them reach the store, the world and the universe - see
	// `UpvalueContext`. That was the one invariant the five hand-rolled copies
	// each had to remember separately, and it is the one a further copy would
	// have been most likely to forget: a method installed with
	// `lua_pushcfunction` instead compiles, links, runs, and reads a garbage
	// pointer.
	//
	// **Must run before `luaL_sandbox`**, which freezes the global table. A
	// service installed after it is silently absent.
	//
	// **`surface` must outlive the VM**, because a property-bearing service puts
	// its address on the metamethods' upvalue rather than copying the lists. Every
	// caller hands over a `static const` built once - which is what the
	// `...Surface()` accessor shape in `LuauBindings.hpp` is for, and why a
	// `ServiceSurface` built as a local would compile and then read freed memory
	// on the first property access.
	//
	// @param state   The VM.
	// @param surface What to build. Must have static storage duration.
	void InstallService(lua_State *state, const ServiceSurface &surface);

	// The two metamethods a property-bearing service's userdata carries.
	//
	// **`LuauCall.cpp`'s, for `InstallLuauServiceMethods`' reason** - a getter and
	// a setter are `ScriptMethod`s, so reaching them means meeting the VM on the
	// neutral layer's behalf, and that file is where this module does that.
	//
	// Upvalue 1 is the `LuauContext` and upvalue 2 is a light userdata pointing at
	// the `ServiceSurface`, which is why the surface must outlive the VM.
	//
	// `__index` walks `Properties` and falls back to the method table stashed
	// under `MethodsKey`; `__newindex` walks the writable half and refuses
	// anything else by name.
	//@{
	int LuauServiceIndex(lua_State *state);
	int LuauServiceNewIndex(lua_State *state);
	//@}

	// Puts the neutral methods of one service on the Luau table at the top of
	// the stack.
	//
	// **`LuauCall.cpp`'s, because that is the file that has met the VM on the
	// neutral layer's behalf** - one trampoline, the row's address on an upvalue,
	// and the receiver deliberately unchecked because a service's is its own
	// table.
	//
	// @param state   The VM.
	// @param methods The service's rows.
	void InstallLuauServiceMethods(lua_State *state, std::span<const ServiceMethod> methods);

	// Every service this language binds, from the catalogue.
	//
	// **The list is `ServiceCatalogue.cpp`'s and not this file's**, which is what
	// makes "which services exist" one fact rather than one per VM. See
	// `ServiceCatalogue.hpp` for why it is a table naming installers rather than
	// a registrar per service file.
	//
	// **Two phases, because they install at different moments.**
	// `ServiceAvailability::Always` runs early, with the rest of the vocabulary.
	// `Studio` runs late - `BreakpointService` reads `LuauContext::Breakpoints`
	// to decide whether to install at all, so it cannot run before that pointer
	// is set, and it writes a global, so it cannot run once `luaL_sandbox` has
	// frozen the table.
	//
	// A service this language does not bind installs nothing. The refusal is in
	// `GetService`, which is the only place a script can tell the difference.
	//
	// @param state The VM.
	// @param phase Which set to install.
	// @since v0.15
	void InstallLuauServices(lua_State *state, ServiceAvailability phase, ScriptCapabilities access);

	// Installs the `InputObject` metatable, and pushes one.
	//
	// **The datatype before anything that produces one**, so a metatable is never
	// looked up before it is registered - which is why `LuauRuntime` calls the
	// first of these beside the other value types rather than leaving it to
	// whichever service happens to install earliest. Here rather than in
	// `LuauValues.cpp` because `LuauInput.cpp` is the only file that makes one: an
	// `InputObject` has no constructor and cannot arrive from anywhere else.
	//
	// `PushInputObject` is not file-local because `LuauCall.cpp` builds the list
	// `UserInputService:GetMouseButtonsPressed` answers with, and that method is
	// neutral since v0.16.
	//@{
	void OpenInputObject(lua_State *state);
	void PushInputObject(lua_State *state, const InputReport &report);
	//@}

	// Turns this frame's input edges into bound actions and input signals.
	//
	// Called at a barrier beside `PumpChanges`, for the same reason: a handler
	// that mutated the world would otherwise do it in the middle of a loop over
	// it.
	//
	// **Six things now and one until v0.16**: the focus edges, the device change,
	// key edges, mouse button edges, pointer motion and the wheel - in that
	// order, so a listener sees `WindowFocusReleased` before the releases losing
	// focus caused and hears which device is live before the press that proves
	// it. Every signal but the focus pair is handed an argument;
	// `LuauInput.cpp` carries what the three previous answers were.
	//
	// **The interface's events are taken as well as the input state, and that is
	// what `gameProcessedEvent` is.** A pointer press the 2D tree consumed has to
	// arrive at `InputBegan` marked rather than swallowed or passed as though
	// nobody handled it, and the only record of it having been consumed is what
	// `gui::Router` produced for this beat. See `InterfaceHasPointer`.
	//
	// @param state     The VM.
	// @param interface What the router produced for this beat, in its order. The
	//        caller still delivers these to `PumpGuiEvents` afterwards; this
	//        reads them and dispatches nothing.
	// @return An error message when a handler raised, or empty.
	std::string PumpInput(lua_State *state, std::span<const gui::GuiEvent> interface);

	// Installs `game`, whose `GetService` is how a Roblox script reaches one.
	void OpenGame(lua_State *state);

	// Installs the `Tween` metatable and the three methods on a tween handle.
	//
	// **The handle and not the service**, which is a `ServiceSurface` -
	// `TweenServiceSurface` says why `Play`, `Pause` and `Cancel` are the one
	// place this module deliberately did not use `ScriptCall`.
	//
	// @param state The VM.
	// @since v0.16
	void OpenTweenHandle(lua_State *state);

	// Pushes a `Tween` userdata for a tween's entity.
	//
	// @param state The VM.
	// @param tween The tween's entity, from `TweenTable::Create`.
	// @since v0.16
	void PushTween(lua_State *state, ecs::Entity tween);

	// Advances every tween by one tick and fires what completed.
	//
	// **Called at the head of the barrier, before the input pump** - see
	// `LuauRuntime::Heartbeat`, which states the whole order and why the world's
	// own timed work goes first.
	//
	// @param state The VM.
	// @param delta The **fixed tick delta**. Never wall time: a tween advanced
	//        by how long the last frame took puts the scene somewhere else on a
	//        busy machine, and the recording stops replaying.
	// @return An error message when a `Completed` handler raised, or empty.
	// @since v0.16
	std::string PumpTweens(lua_State *state, float delta);

	// Installs `require`, which is the only route to a `ModuleScript`.
	//
	// **Defined beside the compiler rather than with the other globals**, because
	// evaluating a module is loading a chunk - the same compile, the same
	// sandboxed thread and the same `script` global that a `Script` gets. A
	// second loader here would be a second answer to what running a program
	// means.
	void OpenRequire(lua_State *state);

	// Installs `task`, and the yield rule made real.
	void OpenTask(lua_State *state);

	// Installs `RaycastParams` and `workspace:Raycast`.
	//
	// **The one part of the datatype vocabulary that is a query rather than a
	// value.** Against `physics::Raycast` and the exact shapes, not
	// `spatial::Raycast` and the proxy boxes: a ray that reported the bounding
	// box of a rotated part would be plausible and wrong.
	//
	// Called after `OpenWorkspace`, because it adds to the world's method table.
	void OpenQueries(lua_State *state);

	// Installs `World`, and the component methods every instance gains.
	//
	// **The storage named directly, underneath the Roblox vocabulary the rest of
	// this file installs.** A game's own data - a health, a cooldown, an
	// inventory slot - has been an attribute or a C++ component until now, and
	// neither is a component a query can reach without a rebuild.
	//
	// Called after `OpenInstances`, because the instance half of the surface adds
	// to the method table that function creates.
	//
	// @param state The VM.
	// @since v0.12
	void OpenEcs(lua_State *state);

	// Installs `BreakpointService`, when this runtime is a studio's.
	//
	// **Absent outside a studio**, so `game:GetService("BreakpointService")`
	// fails the way it does for any service this engine does not provide. Arming
	// a breakpoint switches Luau's step mode on and costs the whole runtime its
	// speed, which a shipped server has no business letting a game script do.
	//
	// @param state The VM.
	// @since v0.12
	void OpenBreakpointService(lua_State *state);

	// Installs `host`, when the runtime has one.
	//
	// **One closure per `HostSurface::Names` entry**, so a name the host does
	// not list is not a member - which turns a typo into "attempt to call a nil
	// value" at the call site rather than a refusal from inside a program the
	// author cannot see.
	//
	// Does nothing when there is no host, which is every game script.
	//
	// @param state The VM.
	// @since v0.12
	void OpenHost(lua_State *state);

	// Calls a function a script handed the host.
	//
	// @param state     The VM.
	// @param callback  What to call.
	// @param arguments What to pass.
	// @return `false` when the callback is unknown or the handler raised.
	// @since v0.12
	bool CallHostCallback(
		lua_State *state, HostCallback callback, HostArguments arguments, HostValue *result = nullptr
	);

	// Lets go of one, so its closure can be collected.
	//
	// @param state    The VM.
	// @param callback What to release.
	// @since v0.12
	void ReleaseHostCallback(lua_State *state, HostCallback callback);

	// Installs `Enum`, over the sets `ecs::EnumTable` holds.
	void OpenEnums(lua_State *state);

	// Pushes one member of one enum as an `EnumItem`.
	//
	// @param state    The VM.
	// @param enumName Which set.
	// @param member   Which member.
	void PushEnumItem(lua_State *state, core::Name enumName, core::Name member);

	// Reads an `EnumItem` or a bare string as a member name.
	//
	// @param state    The VM.
	// @param index    The stack index to read.
	// @param enumName The set the value must belong to.
	// @param out      Filled in with the member's name.
	// @return `false` when the value is a member of a different enum, or is
	//         neither an `EnumItem` nor a string.
	bool ReadEnumValue(lua_State *state, int index, core::Name enumName, core::Name &out);

	// Reads an `EnumItem` of any set, for a reader that does not know which one
	// to expect.
	//
	// **What `HostSurface` needs and `ReadEnumValue` cannot give it.** A host
	// call is a generic bridge - it converts whatever a script passed without
	// knowing what the host will do with it - so it has no enum name to check
	// against. This answers "is this an EnumItem, and which member" and leaves
	// the checking to whoever asked.
	//
	// @param state    The VM.
	// @param index    The stack index to read.
	// @param enumName Filled in with the set it belongs to.
	// @param member   Filled in with the member's name.
	// @return `false` when the value is not an `EnumItem`.
	bool ReadAnyEnumValue(lua_State *state, int index, core::Name &enumName, core::Name &member);

	// --- the camera -----------------------------------------------------------

	// Pushes the world's live camera, or nil when nothing has made one.
	void PushCurrentCamera(lua_State *state);

	// Makes the instance at `index` the world's live camera.
	//
	// @param state The VM.
	// @param index The stack index holding a `Camera` instance, or nil.
	void SetCurrentCamera(lua_State *state, int index);

	// Dispatches this tick's deliveries to their subscribers.
	//
	// @return An error message when a subscriber raised, or empty.
	std::string PumpDeliveries(lua_State *state, ecs::Store &store);

	// Fires `.Changed` for everything the last barrier recorded.
	//
	// @return An error message when a handler raised, or empty.
	std::string PumpChanges(lua_State *state);

	// Delivers everything the tree recorded since the last barrier.
	//
	// **Beside `PumpChanges` and for the same reason.** A reparent cannot fire
	// a signal from inside `SetParent` - the handler would re-enter the VM with
	// the sibling list half-relinked - so the store writes it down and this
	// hands it over one tick later. See `ecs::TreeChange`.
	//
	// @param state The VM to deliver into.
	// @return The first error a handler raised, or empty.
	std::string PumpTree(lua_State *state);

	// Resumes every `WaitForChild` whose child has arrived or whose wait has run
	// out.
	//
	// **The second resume source at the barrier, and the first that is not a bus
	// reply.** `PumpDeliveries` resumes a thread with what a `Ticket` answered;
	// this one resumes it with an instance or with nil, which is a value a
	// delivery may never carry - rule 3 keeps a handle off a bus, and that is
	// exactly why `WaitForChild` could not be built on the mechanism that
	// already existed.
	//
	// **After `PumpTree` and never before it.** A `ChildAdded` handler and a
	// resumed `WaitForChild` are two scripts told about one arrival, and the
	// signal every listener shares goes first; the waiter is one script's own,
	// and it wakes into a world whose tree signals have already agreed the child
	// is there.
	//
	// @param state The VM to resume in.
	// @return The first error a resumed script raised, or empty.
	// @since v0.15
	std::string PumpChildWaiters(lua_State *state);

	// Prepares every queued editable-mesh transaction as one batch, commits in
	// ticket order on this world thread, and resumes its callers.
	std::string PumpEditableMeshJobs(lua_State *state);
	std::string PumpComputeJobs(lua_State *state);

	// Fires `Player.CharacterAdded` and `CharacterRemoving` for everything
	// `scene` recorded since the last barrier.
	//
	// **Beside `PumpTree` and for the same reason, one layer down.** A character
	// is bound to a player by `scene::SetPlayerCharacter`, which is L7 and
	// cannot call a signal at L9; so that module writes the transitions down in
	// order and this hands them over. See `scene::CharacterChange`, which also
	// carries the one consequence a caller has to know: the model named by a
	// removal may already have been destroyed.
	//
	// @param state The VM to deliver into.
	// @return The first error a handler raised, or empty.
	// @since v0.17
	std::string PumpCharacters(lua_State *state);

	// Delivers what a pointer did to the 2D tree since the last beat.
	//
	// **Beside `PumpTree` and for the same reason one door along**: the events
	// were produced by `gui::Router` while a host walked a compiled draw list,
	// and a handler that destroys the element it was called about would pull
	// that list out from under the walk. `Runtime::DeliverGuiEvents` writes them
	// down and this hands them over at the barrier.
	//
	// **The arguments each signal gets, and the one that is deliberately
	// empty.** `MouseEnter`, `MouseLeave` and `MouseMoved` are called with
	// `(x, y)` in canvas pixels, which is Roblox's signature exactly.
	// `Activated`, `InputBegan` and `InputEnded` are called with **nothing**:
	// Roblox passes an `InputObject`, and an argument nobody can rely on is worse
	// than an argument that is not there - the same trade `VideoFrame` and
	// `AutomaticSize` were decided on.
	//
	// **The datatype exists since v0.16 and this still passes nothing**, which is
	// a narrower gap than it was and worth stating rather than closing by reflex.
	// `gui::GuiEvent` carries a kind, an entity and two points - no key, no
	// button, no `Enum.UserInputType` - so an `InputObject` built here would have
	// to *invent* the field a handler reads it for. What closing this needs is
	// `gui::Router` recording which button produced an event, which is a change
	// in `gui` and not one here.
	//
	// @param state  The VM to deliver into.
	// @param events What the host collected since the last beat.
	// @return The first error a handler raised, or empty.
	std::string PumpGuiEvents(lua_State *state, std::span<const gui::GuiEvent> events);

	// Resumes every task due at the world's current tick.
	//
	// @return An error message when a resumed thread raised, or empty.
	std::string PumpTasks(lua_State *state);

	// Installs `workspace` - **this world's `Workspace` service**.
	//
	//     game      -> the universe
	//     workspace -> the `Workspace` instance in the world this script runs on
	//
	// **It is an instance, and until v0.7 it was not.** The old mapping made
	// `workspace` stand for the world itself, so `part.Parent = workspace` meant
	// `SetParent(part, NULL_ENTITY)` - "a root of this world" - and reading
	// `.Parent` back on a root handed `workspace` over. That was the honest
	// shape while a world had no `Workspace` in it. `scene::InstallServices`
	// changed that: a world now has a real `Workspace` instance, and keeping
	// both meant the engine held two answers to "what is in the scene" and the
	// renderer listened to neither.
	//
	// Collapsing them is what lets a null parent mean **nil**. An instance with
	// no parent is now an orphan rather than a root: nothing draws it, no walk
	// of the tree reaches it, and it becomes visible when a script says where it
	// goes. `scene/Visibility.hpp` is the other half - it is what makes
	// "under `Workspace`" the thing the renderer actually tests.
	//
	// Calls `InstallServices`, so a world that has none gets them here. That is
	// idempotent and finds before it creates.
	//
	// **Run before `OpenQueries`**, which fills in the Workspace's own method
	// table with `Raycast`.
	//
	// @param state The VM.
	// @param store The world this script runs on.
	void OpenWorkspace(lua_State *state, ecs::Store &store);

	// Calls every connected Heartbeat function with `delta`.
	//
	// @return An error message when one raised, or empty.
	std::string PumpHeartbeat(lua_State *state, float delta);

	// Pushes an instance userdata onto the stack.
	//
	// Used by `Runtime::RunInstance` to bind `script`, which is why it is not
	// file-local to `LuauInstances.cpp`.
	//
	// @param state    The VM or one of its threads.
	// @param instance The instance to push.
	void PushInstanceValue(lua_State *state, ecs::Entity instance);

	// Reads an `Instance` argument, raising a type error when it is not one.
	//
	// **Shared rather than copied**, because the second copy is the one that
	// forgets the tag check and reads eight bytes of somebody else's userdata
	// as an entity handle. `LuauInstances.cpp` owns the tag and this is how anything
	// else asks it.
	//
	// @param state The VM.
	// @param index The stack index.
	// @return The entity the instance names.
	ecs::Entity CheckInstanceArgument(lua_State *state, int index);

	// Installs `Instance`, and the metatable that turns `part.Size = v` into a
	// property write.
	//
	// @param state The VM.
	void OpenInstances(lua_State *state);

	// The signals `LuauInstances.cpp`'s `InstanceIndex` answers from its branch
	// chain.
	//
	// **Here rather than in `script/Signals.hpp`, which is where it was declared
	// until v0.19.** Nothing in the signature is Luau's, and that was the whole
	// argument for putting it one layer down: it let `Vocabulary.cpp` name the
	// list without being compiled against `<lua.h>`. What it also did was let an
	// L9 file call a function an L10 module defines, which the layer rule forbids
	// and which neither check could see - the tier check reads link edges and
	// this was a bare symbol. The caller is gone and the declaration follows it.
	//
	// **Written down because a branch cannot be walked.** Everything else the
	// editor offers is read back out of a live VM, and a signal is the one member
	// that exists only as a string comparison. `LuauRuntime::Surface` is the sole
	// caller, and `engine.scripthost.vocabulary` is what stops the list going
	// stale beside the chain.
	//
	// @return The signal names, in the order the chain tests them.
	std::vector<std::string_view> LuauInstanceSignalNames();

	// Adds every neutral instance method to the table `OpenInstances` built.
	//
	// **The Luau half of `ScriptCall.hpp`.** A method that is written once is
	// installed by both VMs from one table, so a row added in
	// `ScriptMethods.cpp` is reachable from Luau and from JavaScript in the same
	// commit - which is what the thirty-against-twenty-one drift cost when a
	// method was a `lua_CFunction` and a `JSCFunction` written separately.
	//
	// Called after `OpenInstances`, whose registry table this adds to.
	//
	// @param state The VM.
	// @since v0.16
	void InstallLuauNeutralMethods(lua_State *state);

	// Installs the world clock under names that do not lie about it.
	//
	// `time`, `elapsedTime`, `tick` and `DateTime`, all reading `store.Time()`.
	// **Never the wall clock**: a script that branched on real time would
	// produce a run that does not replay, and `just replay-check` would fail a
	// long way from the script that caused it.
	//
	// @param state The VM.
	void OpenClock(lua_State *state);

	// Installs `typeof` and `warn`, and replaces the base-library gaps with
	// refusals that say why.
	//
	// @param state The VM.
	void OpenBaseExtras(lua_State *state);
}

// Every C closure installed by this adapter passes through the binding profiler.
// The wrapper owns upvalue one, so existing adapter upvalues shift by one. The
// few direct reads of `lua_upvalueindex` are kept beside this definition and
// deliberately use the shifted indexes.
#undef lua_pushcclosure
#undef lua_pushcfunction
#undef lua_resume
#define lua_pushcclosure(state, function, name, upvalues)                                                    \
	::engine::script::PushProfiledClosure((state), (function), (name), (upvalues))
#define lua_pushcfunction(state, function, name)                                                             \
	::engine::script::PushProfiledFunction((state), (function), (name))
#define lua_resume(state, from, arguments) ::engine::script::ResumeProfiledLua((state), (from), (arguments))
