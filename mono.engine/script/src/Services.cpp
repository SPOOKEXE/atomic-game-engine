#include "Bindings.hpp"
#include "Codec.hpp"

#include <engine/world/Postbox.hpp>

#include <cstring>
#include <lualib.h>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine::script {

	namespace {
		using ecs::Store;
		using world::BusKind;
		using world::BusStatus;
		using world::Delivery;
		using world::Postbox;
		using world::Ticket;

		// Where the topic-to-callback table lives.
		constexpr const char *SUBSCRIPTIONS = "engine.messaging.subscriptions";

		Store &StoreOfUpvalue(lua_State *state) {
			return *UpvalueContext(state).World;
		}

		// --- the codec bridge -------------------------------------------------
		//
		// A Luau value in, a `ScriptValue` out. **The sort is not here**: this
		// walks the table in whatever order Luau offers and `Encode` sorts, which
		// is the arrangement `Codec.hpp` argues for — a binding that had to
		// remember to sort would be a second place the determinism guarantee
		// could be lost.

		bool ToScriptValue(lua_State *state, int index, ScriptValue &out, uint32_t depth, CodecStatus &why);

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

		bool
		TableToScriptValue(lua_State *state, int index, ScriptValue &out, uint32_t depth, CodecStatus &why) {
			// **A cycle is an error, not a hang.** `Codec.hpp` §3's third
			// requirement, and the check has to be here rather than in the
			// encoder: by the time a tree exists the cycle has already become
			// infinite recursion. The registry table keyed on the table's own
			// pointer is what makes the check cost a hash lookup.
			lua_getfield(state, LUA_REGISTRYINDEX, "engine.codec.visiting");
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
					ok = ToScriptValue(state, lua_gettop(state), out.Items[item - 1], depth + 1, why);
					lua_pop(state, 1);
				}
			} else {
				lua_pushnil(state);
				while (ok && lua_next(state, index) != 0) {
					// **Keys cross as strings, always.** A numeric key becomes
					// its decimal text, because the far side may be JavaScript
					// where every object key already is one — and a format whose
					// key type depended on which VM wrote it is not one format.
					size_t length = 0;
					const char *text = luaL_tolstring(state, -2, &length);

					ScriptValue value;
					ok = ToScriptValue(state, lua_gettop(state) - 1, value, depth + 1, why);
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
			lua_getfield(state, LUA_REGISTRYINDEX, "engine.codec.visiting");
			lua_pushvalue(state, index);
			lua_pushnil(state);
			lua_rawset(state, -3);
			lua_pop(state, 1);
			return ok;
		}

		bool ToScriptValue(lua_State *state, int index, ScriptValue &out, uint32_t depth, CodecStatus &why) {
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

		// Encodes the value at `index`, raising a named error on refusal.
		std::vector<std::byte> EncodeArgument(lua_State *state, int index) {
			ScriptValue value;
			CodecStatus why = CodecStatus::Ok;

			if (!ToScriptValue(state, index, value, 0, why)) {
				luaL_errorL(state, "the value cannot cross a world boundary: %s", Describe(why));
			}

			std::vector<std::byte> bytes;
			if (const CodecStatus status = Encode(value, bytes); status != CodecStatus::Ok) {
				luaL_errorL(state, "the value cannot cross a world boundary: %s", Describe(status));
			}
			return bytes;
		}

		// --- MessagingService -------------------------------------------------

		// MessagingService:PublishAsync(topic, message)
		//
		// **The only way out of a world.** A script holds one `Store` and there
		// is no binding anywhere that hands it another, so this bus is the
		// crossing — which is rule 3 expressed as an API rather than as a
		// convention: nothing crossing a world boundary is a pointer, so the
		// message is bytes and the far side gets a copy.
		//
		// **A table now, not only a string.** v0.5 took strings because the
		// codec did not exist; it does, so Roblox's actual signature works and
		// the bytes are the same from either VM.
		int PublishAsync(lua_State *state) {
			Postbox box(StoreOfUpvalue(state));

			const char *topic = luaL_checkstring(state, 2);
			const std::vector<std::byte> payload = EncodeArgument(state, 3);

			if (!box.Publish(topic, payload)) {
				// Over budget. Named rather than silent: each bus gives a world
				// an allowance per tick, and a publish that vanished would look
				// like a subscriber that never fired.
				luaL_errorL(state, "PublishAsync: over this world's budget for '%s'", topic);
			}
			return 0;
		}

		// MessagingService:SubscribeAsync(topic, callback)
		//
		// The callback fires when the barrier delivers, which is a
		// deterministic point in a deterministic order — so this needs no
		// yielding and is legal under `docs/SCRIPT_CONCURRENCY.md` §1.
		int SubscribeAsync(lua_State *state) {
			Postbox box(StoreOfUpvalue(state));

			const char *topic = luaL_checkstring(state, 2);
			luaL_checktype(state, 3, LUA_TFUNCTION);

			if (!box.Subscribe(topic)) {
				luaL_errorL(state, "SubscribeAsync: over this world's budget for '%s'", topic);
			}

			// One list per topic, so a topic with three listeners costs one
			// table lookup at delivery rather than a scan of every subscription.
			lua_getfield(state, LUA_REGISTRYINDEX, SUBSCRIPTIONS);
			lua_getfield(state, -1, topic);
			if (lua_isnil(state, -1)) {
				lua_pop(state, 1);
				lua_newtable(state);
				lua_pushvalue(state, -1);
				lua_setfield(state, -3, topic);
			}

			const int count = lua_objlen(state, -1);
			lua_pushvalue(state, 3);
			lua_rawseti(state, -2, count + 1);
			lua_pop(state, 2);
			return 0;
		}

		// --- the stores, and the yield --------------------------------------
		//
		// **These are the calls a script genuinely suspends on, and they are the
		// reason `task` had to come first.** A `Get` returns a `Ticket`; the
		// reply lands in the inbox at a later tick, applied sorted at the
		// barrier. So the resume source is §1's *first* legal case — a `Ticket`
		// reply the barrier applied — and the whole suspend is nine lines
		// because v0.2 already built the shape.
		//
		// §5's three refusals are part of the contract rather than an
		// afterthought: `OverBudget` when the world has spent its allowance,
		// a replica refusing a write, and `NotFound` / `Conflict` from the bus
		// itself. Each arrives as a value the script can test.

		// Whether this world's writes belong to somebody else.
		//
		// **Two flags, and checking one would have been checking half.**
		// `Postbox::IsReplica` reads the bus's own `Replica` resource, and
		// `Store::AdoptOnly` is what `SetProperty` refuses on — the same fact
		// recorded in two places, which is a rule-2 smell in `world` rather than
		// here. A script guard that consulted only one would let a write through
		// on a world the other had already disowned, so this consults both and
		// the reason is written down rather than left to whoever finds it.
		bool WritesBelongElsewhere(const Postbox &box, const Store &store) {
			return box.IsReplica() || store.AdoptOnly();
		}

		// Suspends the running thread until `ticket` is answered.
		int AwaitTicket(lua_State *state, Ticket ticket, const char *what) {
			if (!ticket.Expected()) {
				luaL_errorL(state, "%s: over this world's budget", what);
			}

			LuauContext &context = UpvalueContext(state);

			lua_pushthread(state);
			lua_xmove(state, context.State, 1);
			const int reference = lua_ref(context.State, -1);
			lua_pop(context.State, 1);

			// **`insert_or_assign`, not `emplace`.** A resumed script that calls
			// another store method suspends again on the same thread, and
			// `emplace` would keep the *old* reference — so the fresh one would
			// leak and the next reply would resume a thread nothing was holding.
			context.Threads.insert_or_assign(state, reference);
			context.AwaitedTickets.insert_or_assign(ticket.Value, state);
			return lua_yield(state, 0);
		}

		int MemoryStoreGetAsync(lua_State *state) {
			Postbox box(StoreOfUpvalue(state));
			return AwaitTicket(state, box.Get(BusKind::MemoryStore, luaL_checkstring(state, 2)), "GetAsync");
		}

		int MemoryStoreSetAsync(lua_State *state) {
			Postbox box(StoreOfUpvalue(state));
			const char *key = luaL_checkstring(state, 2);
			const std::vector<std::byte> payload = EncodeArgument(state, 3);

			if (WritesBelongElsewhere(box, StoreOfUpvalue(state))) {
				luaL_errorL(
					state,
					"SetAsync: this world is a replica, and a store write here would be applied and "
					"then overwritten by the next delta. Test RunService:IsReplica() first"
				);
			}
			return AwaitTicket(state, box.Set(BusKind::MemoryStore, key, payload), "SetAsync");
		}

		int MemoryStoreUpdateAsync(lua_State *state) {
			Postbox box(StoreOfUpvalue(state));
			const char *key = luaL_checkstring(state, 2);
			const auto version = static_cast<uint64_t>(luaL_checknumber(state, 3));
			const std::vector<std::byte> payload = EncodeArgument(state, 4);

			// **The compare-and-swap, which is the cross-world lock.**
			// `docs/SCRIPT_CONCURRENCY.md` §4: a lock in the shape an author
			// expects cannot exist here, because rule 3 leaves no shared memory
			// to guard. What they actually want is this — the version the caller
			// read goes in, and `Conflict` comes back when it has moved on.
			return AwaitTicket(state, box.Update(key, version, payload), "UpdateAsync");
		}

		int MemoryStoreRemoveAsync(lua_State *state) {
			Postbox box(StoreOfUpvalue(state));
			return AwaitTicket(
				state, box.Remove(BusKind::MemoryStore, luaL_checkstring(state, 2)), "RemoveAsync"
			);
		}

		int DataStoreGetAsync(lua_State *state) {
			Postbox box(StoreOfUpvalue(state));
			return AwaitTicket(state, box.Get(BusKind::DataStore, luaL_checkstring(state, 2)), "GetAsync");
		}

		int DataStoreSetAsync(lua_State *state) {
			Postbox box(StoreOfUpvalue(state));
			const char *key = luaL_checkstring(state, 2);
			const std::vector<std::byte> payload = EncodeArgument(state, 3);

			if (WritesBelongElsewhere(box, StoreOfUpvalue(state))) {
				luaL_errorL(
					state,
					"SetAsync: this world is a replica and its store writes are refused. Test "
					"RunService:IsReplica() first"
				);
			}
			return AwaitTicket(state, box.Set(BusKind::DataStore, key, payload), "SetAsync");
		}

		int DataStoreRemoveAsync(lua_State *state) {
			Postbox box(StoreOfUpvalue(state));
			return AwaitTicket(
				state, box.Remove(BusKind::DataStore, luaL_checkstring(state, 2)), "RemoveAsync"
			);
		}

		// A stable, human-readable name for a bus refusal.
		//
		// §5: "Named, not swallowed." Each of these is something a script author
		// has to be able to see and handle, so each arrives as a string beside
		// the value rather than as a nil that could mean three things.
		const char *DescribeStatus(BusStatus status) {
			switch (status) {
			case BusStatus::Ok:
				return "Ok";
			case BusStatus::NotFound:
				return "NotFound";
			case BusStatus::Conflict:
				return "Conflict";
			case BusStatus::OverBudget:
				return "OverBudget";
			case BusStatus::NoSuchWorld:
				return "NoSuchWorld";
			case BusStatus::Unsupported:
				return "Unsupported";
			}
			return "Unknown";
		}

		void
		InstallService(lua_State *state, LuauContext &context, const char *name, const luaL_Reg *methods) {
			lua_newtable(state);
			for (const luaL_Reg *method = methods; method->name != nullptr; method++) {
				lua_pushlightuserdata(state, &context);
				lua_pushcclosure(state, method->func, method->name, 1);
				lua_setfield(state, -2, method->name);
			}
			lua_setglobal(state, name);
		}
	}

	void OpenServices(lua_State *state, ecs::Store &store) {
		// **Before anything constructs a `Postbox`, and this is not a
		// formality.** A `Postbox` is a view over two resources, and reading
		// one on a store that never registered them mints them under the
		// *compiler's* spelling — `engine::world::Inbox` rather than
		// `world.Inbox`. Nothing fails at that moment. What fails is the next
		// `Universe` to register them properly, which aborts with "a type has
		// one name", in whichever test order happened to reach it first.
		//
		// That is the cross-process hazard the explicit-naming rule exists to
		// prevent, reached from inside one process — and v0.4 hit the same
		// shape through `Store::Resource<T>()`. Idempotent, so this costs a
		// hash lookup.
		world::RegisterMailboxTypes();

		LuauContext &context = ContextOf(state);

		lua_newtable(state);
		lua_setfield(state, LUA_REGISTRYINDEX, SUBSCRIPTIONS);

		// The cycle-detection set, held in the registry rather than as a C++
		// container because what it keys on is a Lua table's identity.
		lua_newtable(state);
		lua_setfield(state, LUA_REGISTRYINDEX, "engine.codec.visiting");

		static const luaL_Reg messaging[] = {
			{"PublishAsync", PublishAsync}, {"SubscribeAsync", SubscribeAsync}, {nullptr, nullptr}
		};
		static const luaL_Reg memoryStore[] = {
			{"GetAsync", MemoryStoreGetAsync},
			{"SetAsync", MemoryStoreSetAsync},
			{"UpdateAsync", MemoryStoreUpdateAsync},
			{"RemoveAsync", MemoryStoreRemoveAsync},
			{nullptr, nullptr}
		};
		static const luaL_Reg dataStore[] = {
			{"GetAsync", DataStoreGetAsync},
			{"SetAsync", DataStoreSetAsync},
			{"RemoveAsync", DataStoreRemoveAsync},
			{nullptr, nullptr}
		};

		InstallService(state, context, "MessagingService", messaging);
		InstallService(state, context, "MemoryStoreService", memoryStore);
		InstallService(state, context, "DataStoreService", dataStore);

		(void)store;
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
				if (delivery.Status == BusStatus::Ok && !delivery.Payload.empty()) {
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

			if (delivery.Bus != BusKind::Messaging) {
				continue;
			}

			lua_getfield(state, LUA_REGISTRYINDEX, SUBSCRIPTIONS);
			lua_getfield(state, -1, delivery.Key.Text().data());
			if (lua_isnil(state, -1)) {
				lua_pop(state, 2);
				continue;
			}

			const int count = lua_objlen(state, -1);
			for (int index = 1; index <= count; index++) {
				lua_rawgeti(state, -1, index);

				// `(message, topic)`. Roblox hands a table with `Data` and
				// `Sent`; this hands what it actually has, and adding fields
				// that were not measured would be inventing a contract.
				ScriptValue value;
				if (Decode(delivery.Payload, value) == CodecStatus::Ok) {
					PushScriptValue(state, value);
				} else {
					lua_pushnil(state);
				}
				lua_pushstring(state, delivery.Key.Text().data());

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
			lua_pop(state, 2);
		}
		return firstError;
	}
}
