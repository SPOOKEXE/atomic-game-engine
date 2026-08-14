// The Luau half of the bus: the value walk, the prep, and the delivery pump.
//
// **What is here is what a VM decides, and nothing else.** The four services
// themselves are `ServiceSurface`s in `BusServices.cpp` — one description, two
// installers — so what a topic is, what a teleport carries and how a store
// refuses is written once. This file is the three things that cannot be:
// turning a Lua table into a `ScriptValue`, registering the mailbox types, and
// resuming or calling whoever a delivery is for.
//
// **The value walk is the bigger half and has two callers.** It was private to
// this file while `MessagingService` was the only one; `HttpService:JSONEncode`
// is the second, and a JSON encoder that walked a table itself would be a second
// answer to which tables are arrays, what a cycle is and what a key becomes.
//
// **There is no bus prep here any more.** It was one call to
// `world::RegisterMailboxTypes` plus two registry tables; the subscription table
// is `TopicSubscriptions` on the context now and the cycle-detection set is made
// on first use, so what was left named no VM and moved to the catalogue walk that
// needed it.
//
// @tier L9 · shared

#include "Codec.hpp"
#include "LuauBindings.hpp"

#include <engine/world/Postbox.hpp>

#include <lualib.h>
#include <string>
#include <string_view>
#include <vector>

namespace engine::script {

	namespace {
		using world::BusKind;
		using world::Delivery;
		using world::Postbox;

		// Where the set of tables a codec walk is inside lives.
		constexpr const char *VISITING = "engine.codec.visiting";

		// --- the codec bridge -------------------------------------------------
		//
		// A Luau value in, a `ScriptValue` out. **The sort is not here**: this
		// walks the table in whatever order Luau offers and `Encode` sorts, which
		// is the arrangement `Codec.hpp` argues for — a binding that had to
		// remember to sort would be a second place the determinism guarantee
		// could be lost.
		//
		// **`ReadScriptValue` and `PushScriptValue` are declared in
		// `LuauBindings.hpp` and defined below, outside this anonymous namespace.**
		// They were private to this file while `MessagingService` was the only
		// caller; `HttpService:JSONEncode` is the second, and a JSON encoder that
		// walked a table itself would be a second answer to which tables are
		// arrays, what a cycle is and what a key becomes. What is still private
		// is everything below — the two helpers those entry points are made of.

		// Whether a table is a dense array. Lua has one table type and two
		// meanings for it, and the wire needs to know which.
		bool IsArray(lua_State *state, int index) {
			const int length = lua_objlen(state, index);
			if (length == 0) {
				return false;
			}

			// Every key from 1 to length present, and nothing else. A table with
			// a hole is a map with numeric keys as far as this is concerned,
			// which round-trips correctly rather than silently losing the tail.
			int counted = 0;
			lua_pushnil(state);
			while (lua_next(state, index) != 0) {
				lua_pop(state, 1);
				if (!lua_isnumber(state, -1)) {
					lua_pop(state, 1);
					return false;
				}
				counted++;
				if (counted > length) {
					lua_pop(state, 1);
					return false;
				}
			}
			return counted == length;
		}

		// Pushes the set of tables this walk is inside, creating it on first use.
		//
		// **Created here rather than only by `OpenServices`**, because the walk
		// has a second caller now. `HttpService:JSONEncode` needs the same cycle
		// check, and an install order that happened to put it before
		// `OpenServices` would have turned a missing registry key into "attempt
		// to index nil" from inside a script — a failure a long way from the line
		// that caused it, and one nothing in the build would catch.
		void PushVisiting(lua_State *state) {
			lua_getfield(state, LUA_REGISTRYINDEX, VISITING);
			if (lua_istable(state, -1)) {
				return;
			}

			lua_pop(state, 1);
			lua_newtable(state);
			lua_pushvalue(state, -1);
			lua_setfield(state, LUA_REGISTRYINDEX, VISITING);
		}

		bool
		TableToScriptValue(lua_State *state, int index, ScriptValue &out, uint32_t depth, CodecStatus &why) {
			// **Grow the stack before recursing.** A C function is guaranteed
			// `LUA_MINSTACK` slots and one level of this walk holds the visiting
			// table, the key, the value and the stringified key at the same
			// time — so a table nested five deep already wants more than the VM
			// promised, and `CODEC_MAX_DEPTH` allows sixteen. `ReadHostValue`
			// grows for exactly this reason and `script/AGENTS.md` records what
			// overrunning looked like: an illegal instruction from a script that
			// merely nested a table.
			if (lua_checkstack(state, 8) == 0) {
				why = CodecStatus::TooDeep;
				return false;
			}

			// **A cycle is an error, not a hang.** `Codec.hpp` §3's third
			// requirement, and the check has to be here rather than in the
			// encoder: by the time a tree exists the cycle has already become
			// infinite recursion. The registry table keyed on the table's own
			// pointer is what makes the check cost a hash lookup.
			PushVisiting(state);
			lua_pushvalue(state, index);
			lua_rawget(state, -2);

			const bool seen = !lua_isnil(state, -1);
			lua_pop(state, 1);

			if (seen) {
				lua_pop(state, 1);
				why = CodecStatus::Cyclic;
				return false;
			}

			lua_pushvalue(state, index);
			lua_pushboolean(state, 1);
			lua_rawset(state, -3);
			lua_pop(state, 1);

			const bool array = IsArray(state, index);
			out = ScriptValue{array ? ValueTag::Array : ValueTag::Map};

			bool ok = true;
			if (array) {
				const int length = lua_objlen(state, index);
				out.Items.resize(static_cast<size_t>(length));

				for (int item = 1; item <= length && ok; item++) {
					lua_rawgeti(state, index, item);
					ok = ReadScriptValue(state, lua_gettop(state), out.Items[item - 1], depth + 1, why);
					lua_pop(state, 1);
				}
			} else {
				lua_pushnil(state);
				while (ok && lua_next(state, index) != 0) {
					// **A key is a string, a number or a boolean, and anything
					// else is refused.** `luaL_tolstring` will stringify a table
					// or a function perfectly happily, and what it produces is
					// the *address* — which differs between two runs of one
					// script, so a table used as a key put a pointer into a
					// payload that a recording then has to reproduce. That is
					// the exact failure `Codec.hpp` §1's sort exists to prevent,
					// arriving one step earlier than the sort can see it.
					//
					// The three that are allowed all stringify from the value
					// and nothing else: `"a"`, `"1.5"`, `"true"`.
					const int keyType = lua_type(state, -2);
					if (keyType != LUA_TSTRING && keyType != LUA_TNUMBER && keyType != LUA_TBOOLEAN) {
						why = CodecStatus::Unsupported;
						ok = false;
						lua_pop(state, 1);
						continue;
					}

					// **Keys cross as strings, always.** A numeric key becomes
					// its decimal text, because the far side may be JavaScript
					// where every object key already is one — and a format whose
					// key type depended on which VM wrote it is not one format.
					size_t length = 0;
					const char *text = luaL_tolstring(state, -2, &length);

					ScriptValue value;
					ok = ReadScriptValue(state, lua_gettop(state) - 1, value, depth + 1, why);
					if (ok) {
						out.Entries.emplace_back(std::string(text, length), std::move(value));
					}

					lua_pop(state, 2);
				}
				if (!ok) {
					lua_pop(state, 1);
				}
			}

			// Out of the visiting set on the way back up, so a table appearing
			// twice as a *sibling* is fine — only a table reachable from itself
			// is a cycle.
			PushVisiting(state);
			lua_pushvalue(state, index);
			lua_pushnil(state);
			lua_rawset(state, -3);
			lua_pop(state, 1);
			return ok;
		}
	}

	bool ReadScriptValue(lua_State *state, int index, ScriptValue &out, uint32_t depth, CodecStatus &why) {
		if (depth > CODEC_MAX_DEPTH) {
			why = CodecStatus::TooDeep;
			return false;
		}

		if (lua_isnil(state, index)) {
			out = ScriptValue{ValueTag::Nil};
			return true;
		}
		if (lua_isboolean(state, index)) {
			out = ScriptValue{lua_toboolean(state, index) != 0 ? ValueTag::True : ValueTag::False};
			out.Boolean = lua_toboolean(state, index) != 0;
			return true;
		}
		if (lua_isnumber(state, index)) {
			out = ScriptValue{ValueTag::Number};
			out.Number = lua_tonumber(state, index);
			return true;
		}
		if (lua_isstring(state, index)) {
			size_t length = 0;
			const char *text = lua_tolstring(state, index, &length);
			out = ScriptValue{ValueTag::String};
			out.Text.assign(text, length);
			return true;
		}

		if (lua_touserdatatagged(state, index, TAG_VECTOR3) != nullptr) {
			out = ScriptValue{ValueTag::Vector3};
			out.Vector = CheckVector3(state, index);
			return true;
		}
		if (lua_touserdatatagged(state, index, TAG_COLOR3) != nullptr) {
			out = ScriptValue{ValueTag::Color3};
			out.Colour = CheckColor3(state, index);
			return true;
		}
		if (lua_touserdatatagged(state, index, TAG_CFRAME) != nullptr) {
			out = ScriptValue{ValueTag::CFrame};
			out.Frame = CheckCFrame(state, index);
			return true;
		}

		if (lua_istable(state, index)) {
			return TableToScriptValue(state, index, out, depth, why);
		}

		// A function, a thread, or an instance. **An `Entity` is meaningless
		// outside this world**, so a reference must cross as whatever the
		// game uses to name things rather than as a handle — rule 3, stated
		// as a refusal an author can read.
		why = CodecStatus::Unsupported;
		return false;
	}

	void PushScriptValue(lua_State *state, const ScriptValue &value) {
		switch (value.Tag) {
		case ValueTag::Nil:
			lua_pushnil(state);
			return;
		case ValueTag::False:
		case ValueTag::True:
			lua_pushboolean(state, value.Boolean);
			return;
		case ValueTag::Number:
			lua_pushnumber(state, value.Number);
			return;
		case ValueTag::String:
			lua_pushlstring(state, value.Text.data(), value.Text.size());
			return;
		case ValueTag::Array:
			lua_newtable(state);
			for (size_t item = 0; item < value.Items.size(); item++) {
				PushScriptValue(state, value.Items[item]);
				lua_rawseti(state, -2, static_cast<int>(item) + 1);
			}
			return;
		case ValueTag::Map:
			lua_newtable(state);
			for (const auto &entry : value.Entries) {
				lua_pushlstring(state, entry.first.data(), entry.first.size());
				PushScriptValue(state, entry.second);
				lua_rawset(state, -3);
			}
			return;
		case ValueTag::Vector3:
			*PushVector3(state) = value.Vector;
			return;
		case ValueTag::Color3:
			*PushColor3(state) = value.Colour;
			return;
		case ValueTag::CFrame:
			*PushCFrame(state) = value.Frame;
			return;
		}
		lua_pushnil(state);
	}

	std::string PumpDeliveries(lua_State *state, ecs::Store &store) {
		LuauContext &context = ContextOf(state);
		const Postbox box(store);
		const auto deliveries = box.Deliveries();
		if (deliveries.empty()) {
			return {};
		}

		std::string firstError;

		for (const Delivery &delivery : deliveries) {
			// **A reply first**, because a suspended script is waiting on it and
			// a subscriber is not. Both are barrier deliveries and both are
			// legal resume sources; the order is stated so that a world whose
			// script both published and awaited sees them the same way twice.
			if (delivery.Reply.Expected()) {
				const auto waiting = context.AwaitedTickets.find(delivery.Reply.Value);
				if (waiting == context.AwaitedTickets.end()) {
					continue;
				}

				lua_State *thread = waiting->second;
				context.AwaitedTickets.erase(waiting);

				const auto held = context.Threads.find(thread);
				if (held == context.Threads.end()) {
					continue;
				}
				const CallbackRef reference = held->second;
				context.Threads.erase(held);

				// `(value, status, version)`. Roblox's `GetAsync` returns the
				// value alone and swallows the rest; §5 says each refusal has to
				// be something a script can see, so the status rides beside it.
				if (delivery.Status == world::BusStatus::Ok && !delivery.Payload.empty()) {
					ScriptValue value;
					if (Decode(delivery.Payload, value) == CodecStatus::Ok) {
						PushScriptValue(thread, value);
					} else {
						lua_pushnil(thread);
					}
				} else {
					lua_pushnil(thread);
				}

				lua_pushstring(thread, DescribeStatus(delivery.Status));
				lua_pushnumber(thread, static_cast<double>(delivery.Version));

				// **A yield here is success, not failure**, and getting that
				// wrong is what a chained `SetAsync` then `GetAsync` found: the
				// second call suspends the same thread from inside this
				// `lua_resume`, so the return is `LUA_YIELD` — and reading an
				// error message off a *suspended* thread's stack is reading
				// whatever the yield left there.
				//
				// The old reference is released either way. When the thread
				// suspended again, `AwaitTicket` has already registered a new
				// one, so the thread stays alive across the gap.
				const int status = lua_resume(thread, nullptr, 3);
				if (status != LUA_OK && status != LUA_YIELD && firstError.empty()) {
					const char *message = lua_tostring(thread, -1);
					firstError = message != nullptr ? message : "a resumed store call failed";

					if (const char *trace = lua_debugtrace(thread); trace != nullptr) {
						firstError += "\n";
						firstError += trace;
					}
				}

				lua_unref(state, reference);
				continue;
			}

			// **An arrival is not this pump's any more, and that is the fix
			// rather than a tidy-up.** It used to be admitted here — which meant
			// a teleport was only ever taken in by a world with a *Luau* script
			// executing. A destination the studio was not playing, or one whose
			// scripts are JavaScript, or a scene furnished by C++, took the
			// payload into its inbox and left it there: destroyed in the world
			// you left, never built in the world you went to.
			//
			// `script::AdmitTeleports` is a system on every world now, running
			// whether or not anything is running scripts. See `Runtime.hpp`.
			if (delivery.Bus == BusKind::Teleport) {
				continue;
			}

			// **A channel message goes to the channel's own connections**, which
			// is one `SignalKind` with no subject filtered by name — the trick
			// `GetPropertyChangedSignal` and `GetAttributeChangedSignal` are
			// already on. Without the filter every listener in this world would
			// hear every channel it had opened, which is the traffic separation
			// `CrossWorldService` exists to provide.
			if (delivery.Bus == BusKind::Channel) {
				ScriptValue value;
				if (Decode(delivery.Payload, value) == CodecStatus::Ok) {
					PushScriptValue(state, value);
				} else {
					lua_pushnil(state);
				}

				// **The sender's name, second, which is what makes a channel a
				// channel.** A topic subscriber is told which topic; a channel
				// receiver already knows the channel — it named it to get this
				// handle — and what it cannot know is who to answer.
				const std::string_view from = delivery.From.Text();
				lua_pushlstring(state, from.data(), from.size());

				std::string failure =
					FireSignal(state, SignalKind::CrossWorldMessage, ecs::NULL_ENTITY, 2, delivery.Key);
				if (!failure.empty() && firstError.empty()) {
					firstError = std::move(failure);
				}
				continue;
			}

			if (delivery.Bus != BusKind::Messaging) {
				continue;
			}

			// **From `TopicSubscriptions` rather than a registry table**, which
			// is the same list `MessagingService:SubscribeAsync` writes in either
			// language. What stays this file's is the two lines that *call* one.
			const std::string_view topic = delivery.Key.Text();
			for (const CallbackRef callback : context.Subscriptions.Listeners(topic)) {
				lua_getref(state, callback);

				// `(message, topic)`. Roblox hands a table with `Data` and
				// `Sent`; this hands what it actually has, and adding fields
				// that were not measured would be inventing a contract.
				ScriptValue value;
				if (Decode(delivery.Payload, value) == CodecStatus::Ok) {
					PushScriptValue(state, value);
				} else {
					lua_pushnil(state);
				}
				lua_pushlstring(state, topic.data(), topic.size());

				// Every subscriber runs even when one raises, for the reason
				// the heartbeat gives: half a world reacting points nowhere
				// near the cause.
				if (lua_pcall(state, 2, 0, 0) != LUA_OK) {
					if (firstError.empty()) {
						const char *message = lua_tostring(state, -1);
						firstError = message != nullptr ? message : "a subscriber failed";
					}
					lua_pop(state, 1);
				}
			}
		}
		return firstError;
	}
}
