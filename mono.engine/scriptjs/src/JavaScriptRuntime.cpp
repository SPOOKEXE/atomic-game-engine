#include "JavaScriptRuntime.hpp"

#include "JsBindings.hpp"
#include "SourceMap.hpp"

#include <engine/core/Log.hpp>
#include <engine/core/Paths.hpp>
#include <engine/script/Instances.hpp>
#include <engine/script/SourceCache.hpp>
#include <engine/scriptjs/Runtime.hpp>

#include <cstdint>
#include <quickjs.h>
#include <string>

namespace engine::script {

	namespace {
		// The step counter, hung off the runtime.
		//
		// `Taken` only ever goes up, and `Base` is where the call now running
		// started. The budget is the difference, so one call spends one budget
		// however many chunks, reactions or handlers it turns into, and
		// `StepsTaken` is still a figure two readings can be subtracted.
		struct Budget {
			uint64_t Limit = 0;
			uint64_t JobLimit = 0;
			uint64_t Taken = 0;
			uint64_t Base = 0;
		};

		// Called at safepoints. This is what bounds `while (true) {}`.
		//
		// Counted rather than timed, and the reason is the same one the Luau
		// side gives: a wall-clock deadline makes whether a script finished
		// depend on how busy the machine was, so a recording made on a fast
		// machine would replay differently on a slow one. That is the desync
		// rule 5 names, arriving through the one mechanism meant to prevent it.
		//
		// **Once tripped it stays tripped until the next call resets `Base`.**
		// Zeroing the counter here was two bugs at once: every queued job got a
		// fresh budget, so a script that could keep queuing work could not be
		// cut off at all; and `StepsTaken` lost exactly the script that had
		// spent the most.
		//
		// **One tick of `Taken` is ten thousand safepoints**, because QuickJS
		// polls this handler on a divider (`JS_INTERRUPT_COUNTER_INIT`) rather
		// than at every one. The divider is fixed, so the count is still the
		// same on every machine - which is the only property either the budget
		// or `StepsTaken` needs of it.
		int Interrupt(JSRuntime *, void *opaque) {
			auto *budget = static_cast<Budget *>(opaque);
			budget->Taken++;
			if (budget->Limit == 0) {
				return 0;
			}
			return budget->Taken - budget->Base > budget->Limit ? 1 : 0;
		}

		// The own, string-keyed property names of `value`, for
		// `Runtime::Surface`.
		//
		// **Own rather than inherited**, so walking `globalThis` does not drag
		// in `Object.prototype` and offer `hasOwnProperty` beside `workspace`.
		//
		// **Enumerable and not**, which is `JS_GPN_STRING_MASK` without
		// `JS_GPN_ENUM_ONLY` and is load-bearing rather than lax: most of this
		// engine's globals arrive through `JS_SetPropertyFunctionList`, which
		// marks what it writes non-enumerable, so asking for the enumerable
		// ones alone would find almost none of the surface this exists to
		// report.
		std::vector<std::string> OwnPropertyNames(JSContext *context, JSValueConst value) {
			std::vector<std::string> names;
			if (!JS_IsObject(value)) {
				return names;
			}

			JSPropertyEnum *properties = nullptr;
			uint32_t count = 0;
			if (JS_GetOwnPropertyNames(context, &properties, &count, value, JS_GPN_STRING_MASK) != 0) {
				return names;
			}

			names.reserve(count);
			for (uint32_t index = 0; index < count; index++) {
				if (const char *text = JS_AtomToCString(context, properties[index].atom); text != nullptr) {
					names.emplace_back(text);
					JS_FreeCString(context, text);
				}
			}

			JS_FreePropertyEnum(context, properties, count);
			return names;
		}

		// The exception, with its stack when it has one.
		std::string ExceptionText(JSContext *context) {
			JSValue thrown = JS_GetException(context);

			std::string message;
			if (const char *text = JS_ToCString(context, thrown); text != nullptr) {
				message = text;
				JS_FreeCString(context, text);
			}

			JSValue stack = JS_GetPropertyStr(context, thrown, "stack");
			if (!JS_IsUndefined(stack)) {
				if (const char *text = JS_ToCString(context, stack); text != nullptr) {
					message += "\n";
					message += text;
					JS_FreeCString(context, text);
				}
			}

			JS_FreeValue(context, stack);
			JS_FreeValue(context, thrown);
			if (message.empty()) {
				return "the script failed";
			}

			// **The frames name what the author wrote, when a map says what that
			// was.** A `.ts` scene is transpiled before it ever reaches this VM,
			// so every line number QuickJS knows is a line in generated
			// JavaScript. `MapStackFrames` rewrites the ones it can and leaves
			// the rest alone, which is why this is unconditional rather than
			// asking first whether the chunk was TypeScript.
			return MapStackFrames(message);
		}
	}

	JavaScriptRuntime::JavaScriptRuntime(ecs::Store &store, const RuntimeLimits &limits)
		: Runtime(store, limits.Role) {
		Vm = JS_NewRuntime();

		// A hard ceiling rather than a hope. Allocation past it fails inside
		// QuickJS, which surfaces as an ordinary script error rather than as a
		// bad_alloc somewhere in the middle of an interpreter.
		if (limits.MemoryBytes != 0) {
			JS_SetMemoryLimit(Vm, limits.MemoryBytes);
		}
		JS_SetMaxStackSize(Vm, 1u * 1024u * 1024u);

		auto *budget = new Budget();
		budget->Limit = limits.StepBudget;
		budget->JobLimit = limits.JobBudget;
		JS_SetInterruptHandler(Vm, Interrupt, budget);
		JS_SetRuntimeOpaque(Vm, budget);

		// **Built up from nothing rather than trimmed down**, which is the same
		// stance the Luau side takes with `os` and `debug` - and it is not
		// stylistic. `JS_NewContext` adds every intrinsic including **`Date`**,
		// and a script that branches on `Date.now()` produces a run that does
		// not replay; `just replay-check` would then fail a long way from the
		// script that caused it. A test asserted `Date` was absent and found it
		// present, which is exactly why the list is written out.
		//
		// What is here is the language and the things a scene needs. What is
		// not: **`Date`**, which is a wall clock; and **`Proxy`**, with which a
		// script could wrap an instance and intercept the property surface.
		// There is no timer, no `setTimeout`, no `fetch` and no module loader,
		// because none of those exist unless a host installs them and this one
		// does not.
		//
		// `JS_AddIntrinsicEval` is here and was not at first, which cost a run:
		// **`JS_Eval` - the C entry point that runs a script at all - needs
		// it.** Excluding it did not produce a sandbox, it produced
		// "TypeError: eval is not supported" for every script. The global
		// `eval` it also installs is deleted below.
		Context = JS_NewContextRaw(Vm);
		JS_AddIntrinsicBaseObjects(Context);
		JS_AddIntrinsicEval(Context);
		JS_AddIntrinsicRegExp(Context);
		JS_AddIntrinsicJSON(Context);
		JS_AddIntrinsicMapSet(Context);
		JS_AddIntrinsicTypedArrays(Context);
		JS_AddIntrinsicPromise(Context);

		// **`BigInt` is absent because it does not free cleanly on a raw
		// context**, not because it was unwanted. `JS_AddIntrinsicBigInt` over
		// `JS_NewContextRaw` leaves an object alive, and `JS_FreeRuntime`
		// asserts `list_empty(&rt->gc_obj_list)` on teardown - reproduced
		// against upstream in isolation, with every other intrinsic in this
		// list clean and `JS_NewContext` (which adds them all) clean too.
		//
		// Nothing here needs it: `PropertyType::Int64` marshals through a
		// double, because the property surface has no value wide enough to need
		// arbitrary precision. Revisit when one does, and check the teardown
		// again rather than assuming it was fixed.

		OpenJsBindings(Context, Store, limits.Role);
		OpenJsSurface(Context);

		// `eval` removed after the fact, because the intrinsic that provides
		// `JS_Eval` provides the global too and they cannot be separated at
		// registration.
		//
		// **Stated rather than overclaimed:** this closes the obvious door and
		// not every door. `new Function("...")` still compiles a string, and
		// removing `Function` would take every function expression with it. A
		// script assembling its own source is a script no manifest describes,
		// which is a reason to keep watching this rather than a hole a game can
		// be exploited through - it is the same VM, the same bindings and the
		// same refusals on the other side of it.
		{
			JSValue global = JS_GetGlobalObject(Context);
			const JSAtom name = JS_NewAtom(Context, "eval");
			JS_DeleteProperty(Context, global, name, 0);
			JS_FreeAtom(Context, name);
			JS_FreeValue(Context, global);
		}
	}

	JavaScriptRuntime::~JavaScriptRuntime() {
		// **The store's listeners go before the VM does.** The removal hook
		// captures this `JSContext *`, and a store that outlived the runtime
		// would call into a freed context the next time anything in the world
		// was destroyed - which is the ordinary case, because a world is
		// destroyed after the scripts that built it. `CloseJsBindings` takes
		// the change subscriptions back for the same reason.
		Store.ClearDescendantRemoving();

		if (Context != nullptr) {
			CloseJsBindings(Context);
			JS_FreeContext(Context);
		}
		if (Vm != nullptr) {
			auto *budget = static_cast<Budget *>(JS_GetRuntimeOpaque(Vm));
			JS_FreeRuntime(Vm);
			delete budget;
		}
	}

	bool JavaScriptRuntime::DrainJobs() {
		// **The host drives the microtask queue, and this loop is the whole
		// reason a JavaScript VM can live under `world::Driver` at all.**
		//
		// A runtime that owned its own event loop would resolve promise
		// reactions whenever it liked - mid-tick, in an order nothing chose -
		// and that is the desync rule 5 names. `JS_ExecutePendingJob` hands
		// that decision to us, so reactions run at a point the engine picks and
		// in the order the queue holds them. An embeddable JS engine without
		// this API would have had to be rejected on rule 5 alone, whatever else
		// it offered.
		//
		// **Bounded, because a reaction may queue a reaction.** A `then` on a
		// resolved promise, called from the reaction that promise runs, never
		// empties the queue - and an unbounded loop here is a tick that never
		// ends, which is rule 5's boundary gone. It is the one place the step
		// budget cannot reach: QuickJS polls the interrupt handler once per ten
		// thousand safepoints, so ten thousand tiny jobs go by without the step
		// counter moving once. Measured against the vendored VM, 52 million
		// jobs and 31,201 polls in two minutes against a budget of 200 million.
		//
		// **A count and not a deadline**, so two runs of one recording drain
		// the same number of jobs whatever else the machine was doing. A
		// wall-clock cut-off would make `just determinism` and `just
		// replay-check` machine-dependent, which is the failure the bound
		// exists to prevent.
		//
		// **The refusal belongs to the script**, so it arrives as an ordinary
		// script error a caller reports and not as a host that stopped. The
		// jobs already queued stay queued; the next call drains at most another
		// `JobLimit` of them and refuses again, which is what keeps the cost
		// per tick bounded rather than pretending the queue can be emptied.
		auto *budget = static_cast<Budget *>(JS_GetRuntimeOpaque(Vm));
		const uint64_t jobLimit = budget != nullptr ? budget->JobLimit : 0;

		for (uint64_t drained = 0;; drained++) {
			if (jobLimit != 0 && drained >= jobLimit) {
				Error = "script exceeded its microtask budget: " + std::to_string(jobLimit) +
						" queued jobs ran and the queue was still not empty";
				return false;
			}

			JSContext *pending = nullptr;
			const int status = JS_ExecutePendingJob(JS_GetRuntime(Context), &pending);
			if (status == 0) {
				return true;
			}
			if (status < 0) {
				Error = ExceptionText(pending != nullptr ? pending : Context);
				return false;
			}
		}
	}

	uint64_t JavaScriptRuntime::StepsTaken() const {
		const auto *budget = Vm != nullptr ? static_cast<const Budget *>(JS_GetRuntimeOpaque(Vm)) : nullptr;
		return budget != nullptr ? budget->Taken : 0;
	}

	bool JavaScriptRuntime::Run(std::string_view source, std::string_view name) {
		Error.clear();

		// A fresh budget for this chunk, and the counter keeps running. The
		// chunk and every reaction it queues share what is set here, which is
		// what makes `DrainJobs` spend a budget rather than hand one out.
		if (auto *budget = static_cast<Budget *>(JS_GetRuntimeOpaque(Vm)); budget != nullptr) {
			budget->Base = budget->Taken;
		}

		const std::string chunkName(name);
		// **Strict mode, and it is load-bearing rather than tidy.** An instance
		// is made non-extensible so a script cannot bolt a field onto it, and
		// in sloppy mode assigning to a non-extensible object *silently does
		// nothing* - so `part.Transparency = 0.5` would look like it worked and
		// read back as undefined. Strict mode turns that into the TypeError a
		// Luau script already gets from `__newindex`.
		JSValue result = JS_Eval(
			Context,
			source.data(),
			source.size(),
			chunkName.c_str(),
			JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_STRICT
		);

		if (JS_IsException(result)) {
			Error = ExceptionText(Context);
			JS_FreeValue(Context, result);
			return false;
		}
		JS_FreeValue(Context, result);

		// Drained here rather than left for later. Nothing in this version gives
		// a script anything to await, so a pending job at this point is work the
		// script started and did not finish - and finishing the tick with one
		// outstanding is exactly the tick-crossing v0.6 has to decide about
		// deliberately.
		if (!DrainJobs()) {
			return false;
		}

		// Collection placed rather than left to happen. `JS_RunGC` here means
		// the collector runs at a point the host chose, so its timing cannot
		// differ between two runs of one recording.
		JS_RunGC(Vm);
		return true;
	}

	bool JavaScriptRuntime::RunInstance(ecs::Entity instance) {
		Error.clear();

		// **The active container, not a component of its own.** An instance
		// may hold a program per language and `ActiveSourceOf` is the one place
		// that says which one runs - see `script::CodeSourceContainerSelector`.
		if (!ActiveSourceOf(Store, instance).IsValid()) {
			return true;
		}

		// The world's `SourceCache`, then the replicated row, then the
		// filesystem, through the same one function the Luau side uses. Two
		// resolvers is two places to forget one of the three, and both VMs load
		// the same game file and run in the same replica.
		core::Name path;
		std::string program;
		if (!ReadProgram(Store, instance, path, program, Error)) {
			return false;
		}

		// `script` names the instance, for the reason the Luau side gives.
		//
		// **A global rather than a per-chunk scope, and that difference is
		// real**: `JS_Eval` with `JS_EVAL_TYPE_GLOBAL` shares one global object
		// across every chunk, where `luaL_sandboxthread` gives each Luau chunk
		// its own. So `script` is rebound before each and cleared after, and two
		// JavaScript scripts in one world can see each other's globals - which
		// is JavaScript's own model rather than something this engine chose.
		{
			JSValue global = JS_GetGlobalObject(Context);
			JS_SetPropertyStr(Context, global, "script", MakeJsInstance(Context, instance));
			JS_FreeValue(Context, global);
		}

		const bool ok = Run(program, std::string(path.Text()));

		{
			JSValue global = JS_GetGlobalObject(Context);
			JS_SetPropertyStr(Context, global, "script", JS_NULL);
			JS_FreeValue(Context, global);
		}
		return ok;
	}

	bool JavaScriptRuntime::Heartbeat(float delta) {
		// **One budget for the whole beat.** Every connection, every resumed
		// task and every reaction they queue spends the same one, because a
		// budget refreshed per handler bounds no tick: a script gets as many of
		// them as it can arrange to be called.
		if (auto *budget = static_cast<Budget *>(JS_GetRuntimeOpaque(Vm)); budget != nullptr) {
			budget->Base = budget->Taken;
		}

		// **The same four steps in the same order as the Luau side**, because a
		// world scripted in either language must see one sequence: the
		// barrier's deliveries, then what changed, then the resumes due, then
		// the beat. Each of the first three is one of
		// `docs/retired/SCRIPT_CONCURRENCY.md` §1's legal resume sources.
		//
		// **The source mirror comes before all four, exactly where the Luau side
		// puts it** - `LuauRuntime::Heartbeat` carries the argument: the rows it
		// writes are read by `replication::Authority::Publish` after the tick is
		// over, so mirroring at the end would put every client a tick behind
		// every save.
		MirrorSourcePrograms(Store, Mirrored);

		Error = PumpJsDeliveries(Context, Store);

		const auto note = [&](std::string message) {
			if (Error.empty()) {
				Error = std::move(message);
			}
		};

		// **The world's own timed work first, exactly where the Luau side puts
		// it** - see `LuauRuntime::Heartbeat`, which carries the whole argument:
		// a tween and a deadline are not resumes, and everything the rest of the
		// barrier delivers should see the world they already moved.
		note(PumpJsTweens(Context, delta));
		PumpDebris(Store, JsOf(Context).Debris);

		// **Input first, and that ordering is the useful one** - the same place
		// `LuauRuntime::Heartbeat` puts it, and for its reason: a bound action's
		// handler writes properties, and those writes should reach their
		// listeners on *this* barrier rather than the next.
		//
		// **It is handed this beat's interface events even though it dispatches
		// none of them**, which is what `gameProcessedEvent` is: a click the 2D
		// tree consumed has to arrive at `InputBegan` marked. They are still
		// queued at this point and are drained below.
		note(PumpJsInput(Context, PendingGuiEvents));

		note(PumpJsChanges(Context));

		// **After the property changes and before the tasks**, exactly as the
		// Luau side orders it: both are "what the previous barrier recorded",
		// and a handler watching a part's position and its ancestry should see
		// one world rather than two.
		note(PumpJsTree(Context));

		// **The tree's other listener, second within this step and never before
		// it** - the same place and the same reason the Luau side puts it: a
		// `ChildAdded` handler and a resumed `WaitForChild` are two scripts told
		// about one arrival, and the signal every listener shares goes first.
		note(PumpJsChildWaiters(Context));

		// After the tree, exactly as the Luau side orders it: a respawn is a
		// model parented into `Workspace` *and* a link written onto the
		// `Player`, and a `CharacterAdded` handler should find a world whose
		// tree signals have already agreed the model is there.
		note(PumpJsCharacters(Context));

		// Last within step 2 and before the tasks, exactly as the Luau side
		// orders it - see `LuauRuntime::Heartbeat`, which also gives the reason
		// the queue is moved out before the walk rather than drained in place.
		{
			std::vector<gui::GuiEvent> events;
			events.swap(PendingGuiEvents);
			note(PumpJsGuiEvents(Context, events));
		}

		note(PumpJsTasks(Context));
		note(PumpJsHeartbeat(Context, delta));

		// A connection may have created a promise. Drained here for the same
		// reason `Run` drains: a reaction left outstanding is work crossing a
		// tick boundary at a point nobody chose.
		if (!DrainJobs()) {
			return false;
		}
		return Error.empty();
	}

	ScriptSurface JavaScriptRuntime::Surface() const {
		ScriptSurface surface;
		if (Context == nullptr) {
			return surface;
		}

		// **A host entry, so it gets its own budget** - the same grant `Run` and
		// `Heartbeat` make, and for the reason `LuauRuntime::Surface` gives: the
		// walk below passes safepoints, and a runtime whose last script blew its
		// budget stays tripped until something moves the mark.
		if (auto *budget = static_cast<Budget *>(JS_GetRuntimeOpaque(Vm)); budget != nullptr) {
			budget->Base = budget->Taken;
		}

		JSValue global = JS_GetGlobalObject(Context);

		for (std::string &name : OwnPropertyNames(Context, global)) {
			JSValue value = JS_GetPropertyStr(Context, global, name.c_str());

			VocabularyEntry entry;
			entry.Name = std::move(name);

			if (JS_IsFunction(Context, value)) {
				// **A constructor is a function and is still worth its
				// members.** `Vector3` is callable *and* carries `zero` and
				// `one`, and a walk that stopped at the first fact would offer
				// neither.
				entry.Kind = NameKind::Function;
				entry.Members = OwnPropertyNames(Context, value);
			} else if (JS_IsObject(value)) {
				entry.Kind = NameKind::Container;
				entry.Members = OwnPropertyNames(Context, value);
			} else {
				entry.Kind = NameKind::Value;
			}

			surface.Globals.push_back(std::move(entry));
			JS_FreeValue(Context, value);
		}

		// **The object `InstallJsInstanceMethods` built**, which already holds
		// the signals as accessors - so unlike Luau there is no second list to
		// keep, and the five `JsEcs` appends arrive with the rest.
		JSValue methods = JS_GetPropertyStr(Context, global, "__instanceMethods");
		surface.InstanceMembers = OwnPropertyNames(Context, methods);
		JS_FreeValue(Context, methods);

		JS_FreeValue(Context, global);
		return surface;
	}

	std::unique_ptr<Runtime> MakeJavaScriptRuntime(ecs::Store &store, const RuntimeLimits &limits) {
		return std::make_unique<JavaScriptRuntime>(store, limits);
	}
}
