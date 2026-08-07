#include "JavaScriptRuntime.hpp"

#include "JsBindings.hpp"

#include <engine/core/Log.hpp>
#include <engine/core/Paths.hpp>
#include <engine/script/Instances.hpp>
#include <engine/script/SourceCache.hpp>

#include <cstdint>
#include <quickjs.h>
#include <string>

namespace engine::script {

	namespace {
		// The step counter, hung off the runtime.
		struct Budget {
			uint64_t Limit = 0;
			uint64_t Taken = 0;
		};

		// Called at safepoints. This is what bounds `while (true) {}`.
		//
		// Counted rather than timed, and the reason is the same one the Luau
		// side gives: a wall-clock deadline makes whether a script finished
		// depend on how busy the machine was, so a recording made on a fast
		// machine would replay differently on a slow one. That is the desync
		// rule 5 names, arriving through the one mechanism meant to prevent it.
		int Interrupt(JSRuntime *, void *opaque) {
			auto *budget = static_cast<Budget *>(opaque);
			if (budget->Limit == 0) {
				return 0;
			}
			if (++budget->Taken > budget->Limit) {
				budget->Taken = 0;
				return 1;
			}
			return 0;
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
			return message.empty() ? "the script failed" : message;
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
		JS_SetInterruptHandler(Vm, Interrupt, budget);
		JS_SetRuntimeOpaque(Vm, budget);

		// **Built up from nothing rather than trimmed down**, which is the same
		// stance the Luau side takes with `os` and `debug` — and it is not
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
		// **`JS_Eval` — the C entry point that runs a script at all — needs
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
		// asserts `list_empty(&rt->gc_obj_list)` on teardown — reproduced
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
		// be exploited through — it is the same VM, the same bindings and the
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
		// was destroyed — which is the ordinary case, because a world is
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
		// reactions whenever it liked — mid-tick, in an order nothing chose —
		// and that is the desync rule 5 names. `JS_ExecutePendingJob` hands
		// that decision to us, so reactions run at a point the engine picks and
		// in the order the queue holds them. An embeddable JS engine without
		// this API would have had to be rejected on rule 5 alone, whatever else
		// it offered.
		for (;;) {
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

	bool JavaScriptRuntime::Run(std::string_view source, std::string_view name) {
		Error.clear();

		if (auto *budget = static_cast<Budget *>(JS_GetRuntimeOpaque(Vm)); budget != nullptr) {
			budget->Taken = 0;
		}

		const std::string chunkName(name);
		// **Strict mode, and it is load-bearing rather than tidy.** An instance
		// is made non-extensible so a script cannot bolt a field onto it, and
		// in sloppy mode assigning to a non-extensible object *silently does
		// nothing* — so `part.Transparency = 0.5` would look like it worked and
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
		// script started and did not finish — and finishing the tick with one
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

		const Source *source = Store.Get<Source>(instance);
		if (source == nullptr || !source->Path.IsValid()) {
			return true;
		}

		// The world's `SourceCache` before the filesystem, through the same one
		// function the Luau side uses. Two resolvers is two places to forget
		// the cache, and both VMs load the same game file.
		std::string program;
		if (!ReadSource(Store, source->Path, program, Error)) {
			return false;
		}

		// `script` names the instance, for the reason the Luau side gives.
		//
		// **A global rather than a per-chunk scope, and that difference is
		// real**: `JS_Eval` with `JS_EVAL_TYPE_GLOBAL` shares one global object
		// across every chunk, where `luaL_sandboxthread` gives each Luau chunk
		// its own. So `script` is rebound before each and cleared after, and two
		// JavaScript scripts in one world can see each other's globals — which
		// is JavaScript's own model rather than something this engine chose.
		{
			JSValue global = JS_GetGlobalObject(Context);
			JS_SetPropertyStr(Context, global, "script", MakeJsInstance(Context, instance));
			JS_FreeValue(Context, global);
		}

		const bool ok = Run(program, std::string(source->Path.Text()));

		{
			JSValue global = JS_GetGlobalObject(Context);
			JS_SetPropertyStr(Context, global, "script", JS_NULL);
			JS_FreeValue(Context, global);
		}
		return ok;
	}

	bool JavaScriptRuntime::Heartbeat(float delta) {
		// **The same four steps in the same order as the Luau side**, because a
		// world scripted in either language must see one sequence: the
		// barrier's deliveries, then what changed, then the resumes due, then
		// the beat. Each of the first three is one of
		// `docs/retired/SCRIPT_CONCURRENCY.md` §1's legal resume sources.
		Error = PumpJsDeliveries(Context, Store);

		const auto note = [&](std::string message) {
			if (Error.empty()) {
				Error = std::move(message);
			}
		};

		note(PumpJsChanges(Context));

		// **After the property changes and before the tasks**, exactly as the
		// Luau side orders it: both are "what the previous barrier recorded",
		// and a handler watching a part's position and its ancestry should see
		// one world rather than two.
		note(PumpJsTree(Context));

		// Last within step 2 and before the tasks, exactly as the Luau side
		// orders it — see `LuauRuntime::Heartbeat`, which also gives the reason
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
}
