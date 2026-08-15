#include "LuauBindings.hpp"

#include <cmath>
#include <lualib.h>
#include <string_view>

namespace engine::script {

	namespace {
		// Takes a ref to a thread and remembers which ref it is.
		//
		// The map is what makes `task.cancel` possible: `TaskQueue` holds a
		// `CallbackRef`, a script holds a thread object, and something has to
		// join the two. Keyed on the `lua_State *`, which is a thread's identity
		// for as long as the ref keeps it alive.
		CallbackRef RetainThread(LuauContext &context, lua_State *thread) {
			lua_pushthread(thread);
			lua_xmove(thread, context.State, 1);

			const int reference = lua_ref(context.State, -1);
			lua_pop(context.State, 1);

			// `insert_or_assign` for the reason `LuauBus.cpp` gives: a thread
			// that suspends twice must not leave the first reference behind.
			context.Threads.insert_or_assign(thread, reference);
			return reference;
		}

		void ReleaseThread(LuauContext &context, lua_State *thread, CallbackRef reference) {
			context.Threads.erase(thread);
			lua_unref(context.State, reference);
		}

		// `task.wait(seconds)` - resumes at a tick boundary.
		//
		// **Seconds in, ticks underneath**, which `docs/retired/SCRIPT_CONCURRENCY.md`
		// §2 settled. Seconds because that is what an author means and what
		// Roblox takes; ticks because a wall-clock sleep resumes after a
		// different amount of *simulation* on a busy machine than on an idle
		// one - the desync rule 5 names, arriving through the call an author
		// writes first.
		int TaskWait(lua_State *state) {
			LuauContext &context = UpvalueContext(state);
			const double seconds = luaL_optnumber(state, 1, 0.0);

			const uint64_t ticks = TicksFor(*context.World, seconds);
			const CallbackRef reference = RetainThread(context, state);

			context.Tasks.Delay(reference, context.World->Time().Tick + ticks);

			// The number of ticks, so `PumpTasks` can hand back how long the
			// wait actually was. Roblox's `task.wait` returns that, and a script
			// integrating against it would otherwise have to guess.
			context.WaitTicks.insert_or_assign(state, ticks);
			return lua_yield(state, 0);
		}

		// Runs a function on a fresh thread, starting it now.
		int TaskSpawn(lua_State *state) {
			LuauContext &context = UpvalueContext(state);
			luaL_checktype(state, 1, LUA_TFUNCTION);

			const int arguments = lua_gettop(state) - 1;

			lua_State *thread = lua_newthread(state);
			luaL_sandboxthread(thread);

			// The function and its arguments move to the new thread. `lua_xmove`
			// rather than a copy, because a closure is a reference and copying
			// the stack slot is what moving one means here.
			lua_pushvalue(state, 1);
			for (int index = 1; index <= arguments; index++) {
				lua_pushvalue(state, 1 + index);
			}
			lua_xmove(state, thread, arguments + 1);

			// Retained before it runs: the thread may yield on its first
			// statement, and a thread nothing holds a ref to is collectable
			// between the yield and the resume.
			const CallbackRef reference = RetainThread(context, thread);

			const int status = lua_resume(thread, state, arguments);
			if (status == LUA_OK) {
				// Finished inside the call, so nothing is owed. Roblox's
				// `task.spawn` is synchronous up to the first yield and this is
				// the same shape.
				ReleaseThread(context, thread, reference);
			} else if (status != LUA_YIELD) {
				const char *message = lua_tostring(thread, -1);
				ReleaseThread(context, thread, reference);
				luaL_errorL(state, "task.spawn: %s", message != nullptr ? message : "the thread failed");
			}

			// The thread object, so a script can `task.cancel` it.
			return 1;
		}

		// Queues a function to run at the end of this beat.
		int TaskDefer(lua_State *state) {
			LuauContext &context = UpvalueContext(state);
			luaL_checktype(state, 1, LUA_TFUNCTION);

			const int arguments = lua_gettop(state) - 1;

			lua_State *thread = lua_newthread(state);
			luaL_sandboxthread(thread);

			lua_pushvalue(state, 1);
			for (int index = 1; index <= arguments; index++) {
				lua_pushvalue(state, 1 + index);
			}
			lua_xmove(state, thread, arguments + 1);

			const CallbackRef reference = RetainThread(context, thread);
			context.Tasks.Defer(reference);
			context.PendingArguments.insert_or_assign(thread, arguments);
			return 1;
		}

		// Queues a function to run after a delay.
		int TaskDelay(lua_State *state) {
			LuauContext &context = UpvalueContext(state);
			const double seconds = luaL_checknumber(state, 1);
			luaL_checktype(state, 2, LUA_TFUNCTION);

			const int arguments = lua_gettop(state) - 2;

			lua_State *thread = lua_newthread(state);
			luaL_sandboxthread(thread);

			lua_pushvalue(state, 2);
			for (int index = 1; index <= arguments; index++) {
				lua_pushvalue(state, 2 + index);
			}
			lua_xmove(state, thread, arguments + 1);

			const CallbackRef reference = RetainThread(context, thread);
			context.Tasks.Delay(reference, context.World->Time().Tick + TicksFor(*context.World, seconds));
			context.PendingArguments.insert_or_assign(thread, arguments);
			return 1;
		}

		// `task.cancel(thread)` - forgets a scheduled resume.
		int TaskCancel(lua_State *state) {
			LuauContext &context = UpvalueContext(state);
			luaL_checktype(state, 1, LUA_TTHREAD);

			lua_State *thread = lua_tothread(state, 1);
			const auto found = context.Threads.find(thread);
			if (found == context.Threads.end()) {
				// Not scheduled. Not an error, for the reason
				// `:Disconnect` twice is not: a cleanup path runs whether or not
				// something else already ran.
				lua_pushboolean(state, false);
				return 1;
			}

			const CallbackRef reference = found->second;
			const bool cancelled = context.Tasks.Cancel(reference);
			if (cancelled) {
				context.WaitTicks.erase(thread);
				context.PendingArguments.erase(thread);
				ReleaseThread(context, thread, reference);
			}

			lua_pushboolean(state, cancelled);
			return 1;
		}

		// `wait(n)`, which does not exist.
		//
		// **A refusal that names its replacement**, which is
		// `docs/retired/SCRIPT_CONCURRENCY.md` §2's recommendation and the reason it is
		// worth a bound function rather than an absence. A missing global reads
		// as "this engine forgot `wait`"; this reads as "this engine renamed it,
		// and here is why". A familiar name with different semantics would cost
		// a debugging session instead, and cost it later.
		int RefuseWait(lua_State *state) {
			luaL_errorL(
				state,
				"wait() does not exist here. Use task.wait(seconds), which resumes at a tick boundary "
				"- a wait measured against a wall clock resumes after a different amount of simulation "
				"on a busy machine, and the recording stops replaying"
			);
		}

		int RefuseSpawn(lua_State *state) {
			luaL_errorL(state, "spawn() does not exist here. Use task.spawn(fn), which starts it now");
		}

		int RefuseDelay(lua_State *state) {
			luaL_errorL(
				state, "delay() does not exist here. Use task.delay(seconds, fn), which counts in ticks"
			);
		}
	}

	void OpenTask(lua_State *state) {
		LuauContext &context = ContextOf(state);

		static const struct {
			const char *Name;
			lua_CFunction Function;
		} FUNCTIONS[] = {
			{"wait", TaskWait},
			{"spawn", TaskSpawn},
			{"defer", TaskDefer},
			{"delay", TaskDelay},
			{"cancel", TaskCancel},
		};

		lua_newtable(state);
		for (const auto &entry : FUNCTIONS) {
			lua_pushlightuserdata(state, &context);
			lua_pushcclosure(state, entry.Function, entry.Name, 1);
			lua_setfield(state, -2, entry.Name);
		}
		lua_setglobal(state, "task");

		// The three globals Roblox deprecated, present only to say what to use.
		lua_pushcfunction(state, RefuseWait, "wait");
		lua_setglobal(state, "wait");
		lua_pushcfunction(state, RefuseSpawn, "spawn");
		lua_setglobal(state, "spawn");
		lua_pushcfunction(state, RefuseDelay, "delay");
		lua_setglobal(state, "delay");
	}

	std::string PumpTasks(lua_State *state) {
		LuauContext &context = ContextOf(state);
		std::string firstError;

		const auto resume = [&](CallbackRef reference) {
			lua_getref(state, reference);
			if (!lua_isthread(state, -1)) {
				lua_pop(state, 1);
				return;
			}

			lua_State *thread = lua_tothread(state, -1);
			lua_pop(state, 1);

			// A `task.wait` resumes with how long it waited; a `task.defer` or a
			// `task.delay` resumes with the arguments it was queued with, which
			// are already on its own stack.
			int arguments = 0;
			if (const auto waited = context.WaitTicks.find(thread); waited != context.WaitTicks.end()) {
				lua_pushnumber(
					thread,
					static_cast<double>(waited->second) * static_cast<double>(context.World->Time().Delta)
				);
				arguments = 1;
				context.WaitTicks.erase(waited);
			} else if (const auto queued = context.PendingArguments.find(thread);
					   queued != context.PendingArguments.end()) {
				arguments = queued->second;
				context.PendingArguments.erase(queued);
			}

			const int status = lua_resume(thread, nullptr, arguments);
			if (status == LUA_YIELD) {
				// Yielded again, and `TaskWait` has already re-registered it
				// under a fresh ref. The old one is released here rather than
				// there, because the thread is still running at that point.
				ReleaseThread(context, thread, reference);
				return;
			}

			if (status != LUA_OK && firstError.empty()) {
				const char *message = lua_tostring(thread, -1);
				firstError = message != nullptr ? message : "a resumed task failed";

				if (const char *trace = lua_debugtrace(thread); trace != nullptr) {
					firstError += "\n";
					firstError += trace;
				}
			}

			ReleaseThread(context, thread, reference);
		};

		// **Delayed work first, then deferred.** A `task.delay` due this tick
		// belongs to the tick, and a `task.defer` belongs to the end of the
		// beat - so the deferred pass sees everything the delayed one did. The
		// other order would make `task.defer` mean "before some of this tick"
		// depending on what else was scheduled.
		context.Tasks.Advance(context.World->Time().Tick, resume);
		context.Tasks.DrainDeferred(resume);
		return firstError;
	}

	bool ThreadIsScheduled(LuauContext &context, lua_State *thread) {
		return context.Threads.find(thread) != context.Threads.end();
	}
}
