#include "Bindings.hpp"
#include "Codec.hpp"

#include <engine/core/Log.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Services.hpp>
#include <engine/world/Postbox.hpp>

#include <algorithm>
#include <cstring>
#include <lualib.h>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine::script {

	namespace {
		namespace scene = engine::scene;
		using ecs::Store;
		using world::BusKind;
		using world::BusStatus;
		using world::Delivery;
		using world::Postbox;
		using world::Ticket;

		// Where the topic-to-callback table lives.
		constexpr const char *SUBSCRIPTIONS = "engine.messaging.subscriptions";

		// Where the set of tables a codec walk is inside lives.
		constexpr const char *VISITING = "engine.codec.visiting";

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
		//
		// **`ReadScriptValue` and `PushScriptValue` are declared in
		// `Bindings.hpp` and defined below, outside this anonymous namespace.**
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

	namespace {
		// Encodes the value at `index`, raising a named error on refusal.
		std::vector<std::byte> EncodeArgument(lua_State *state, int index) {
			ScriptValue value;
			CodecStatus why = CodecStatus::Ok;

			if (!ReadScriptValue(state, index, value, 0, why)) {
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
		// yielding and is legal under `docs/retired/SCRIPT_CONCURRENCY.md` §1.
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
			// `docs/retired/SCRIPT_CONCURRENCY.md` §4: a lock in the shape an author
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

		// --- TeleportService --------------------------------------------------
		//
		// **The one bus operation that names a world, and no script could reach
		// it.** `world::BusKind::Teleport` and `Postbox::Teleport` have existed
		// since v0.2 with a router that delivers and an inbox that receives —
		// and `PumpDeliveries` dropped every arrival on the floor, because it
		// only ever looked for `Messaging`. So the crossing worked and nothing
		// could ask for one or notice one.
		//
		// **What crosses is a name and a payload, never an entity.** That is
		// rule 3 and it is also what makes the feature work: the destination
		// rebuilds the player from its *own* class definitions, so two worlds
		// never have to agree about what a `Player` is made of. Roblox works the
		// same way and for the same reason — the far side is another server.

		// The child a teleported player carries their data in.
		//
		// **A `StringValue` under the player rather than a component of its
		// own.** The data is an arbitrary script value; a component holding one
		// would need a type, a serialiser and a wire form for something the
		// engine never reads. A `StringValue` is authored content that already
		// round-trips, already replicates, and is already something a script can
		// see in the explorer — which is worth more here than tidiness.
		constexpr const char *TELEPORT_DATA = "TeleportData";

		// Builds the payload one world hands another.
		//
		// The player's *name* and nothing else about them: the destination has
		// its own `Players` service, its own class table and its own spawn.
		std::vector<std::byte> PackTeleport(lua_State *state, std::string_view name, int dataIndex) {
			ScriptValue label{ValueTag::String};
			label.Text = std::string(name);

			ScriptValue envelope{ValueTag::Map};
			envelope.Entries.emplace_back("Player", std::move(label));

			if (!lua_isnoneornil(state, dataIndex)) {
				ScriptValue data;
				CodecStatus why = CodecStatus::Ok;
				if (!ReadScriptValue(state, dataIndex, data, 0, why)) {
					luaL_errorL(state, "Teleport: the data cannot cross a world boundary: %s", Describe(why));
				}
				envelope.Entries.emplace_back("Data", std::move(data));
			}

			std::vector<std::byte> bytes;
			if (const CodecStatus status = Encode(envelope, bytes); status != CodecStatus::Ok) {
				luaL_errorL(state, "Teleport: the data cannot cross a world boundary: %s", Describe(status));
			}
			return bytes;
		}

		// TeleportService:Teleport(placeName, player, data?)
		int Teleport(lua_State *state) {
			Store &store = StoreOfUpvalue(state);
			Postbox box(store);

			const char *place = luaL_checkstring(state, 2);
			const ecs::Entity player = CheckInstanceArgument(state, 3);

			if (!store.Alive(player) || !store.IsA(player, scene::PlayerClass())) {
				luaL_errorL(state, "Teleport: the second argument must be a Player");
			}

			if (WritesBelongElsewhere(box, store)) {
				// **A replica may not move anybody.** A client asking a server
				// to teleport somebody is a request, not an act — and there is
				// no request channel for it yet, so the honest answer is a
				// refusal a script can see rather than a silent no-op.
				luaL_errorL(state, "Teleport: this world is a replica and does not decide who is in it");
			}

			const core::Name name = store.InstanceNameOf(player);
			const std::vector<std::byte> payload =
				PackTeleport(state, name.IsValid() ? name.Text() : "Player", 4);

			// **A ticket rather than a bool, and the reply is deliberately not
			// awaited.** `NONE` means the world spent its allowance this tick;
			// anything else means the envelope is queued, and the only thing a
			// reply could say is that the destination does not exist — which is
			// a delivery this world will never see the far side of anyway.
			if (box.Teleport(place, payload).Value == world::Ticket::NONE) {
				luaL_errorL(state, "Teleport: over this world's budget for '%s'", place);
			}

			// **Removed here rather than when the arrival lands**, because the
			// two happen in different worlds and only this one can do it. A
			// player left behind would be in both places at once — and the
			// destination has no way to reach back and tidy up, which is
			// exactly the cross-world reference rule 3 forbids.
			//
			// **The router has already taken a copy.** `Postbox::Teleport`
			// queues an envelope holding the bytes, so destroying the instance
			// on the next line cannot lose the message.
			(void)scene::RemoveCharacter(store, player);
			store.DestroyInstance(player);
			return 0;
		}

		// TeleportService:GetLocalPlayerTeleportData()
		int GetLocalPlayerTeleportData(lua_State *state) {
			Store &store = StoreOfUpvalue(state);

			const auto *local = store.Resource<scene::LocalPlayer>();
			if (local == nullptr || !store.Alive(local->Instance)) {
				// **Nil on a server, which is the point of the name.** Roblox's
				// is a client call; a `Script` reaching for it gets nothing
				// rather than somebody else's data.
				lua_pushnil(state);
				return 1;
			}

			const ecs::Entity held = store.FindFirstChild(local->Instance, TELEPORT_DATA);
			const auto *text = held == ecs::NULL_ENTITY ? nullptr : store.Get<scene::TextContent>(held);
			if (text == nullptr || text->Value.empty()) {
				lua_pushnil(state);
				return 1;
			}

			const auto *bytes = reinterpret_cast<const std::byte *>(text->Value.data());

			ScriptValue value;
			if (Decode({bytes, text->Value.size()}, value) != CodecStatus::Ok) {
				lua_pushnil(state);
				return 1;
			}

			PushScriptValue(state, value);
			return 1;
		}

		// TeleportService:GetTeleportData(player)
		int GetTeleportData(lua_State *state) {
			Store &store = StoreOfUpvalue(state);

			const ecs::Entity player = CheckInstanceArgument(state, 2);
			if (!store.Alive(player) || !store.IsA(player, scene::PlayerClass())) {
				luaL_errorL(state, "GetTeleportData: the argument must be a Player");
			}

			// **The server's half of the call above, and the reason it exists is
			// that the payload had no reader on the side that can act on it.**
			// `GetLocalPlayerTeleportData` is Roblox's and is a *client* call —
			// it answers for `LocalPlayer` and nobody else, deliberately, so a
			// script cannot read somebody else's data. But the machine that
			// decides where an arriving character stands is the authority, and
			// it had no way to see what the sender wrote. A payload that only
			// the arriving client can read cannot place the arriving body.
			//
			// **Nil rather than an error for a player who walked in the front
			// door**, because that is the ordinary case and not a mistake: a
			// game asks every arrival and acts on the ones that came through a
			// portal.
			const ecs::Entity held = store.FindFirstChild(player, TELEPORT_DATA);
			const auto *text = held == ecs::NULL_ENTITY ? nullptr : store.Get<scene::TextContent>(held);
			if (text == nullptr || text->Value.empty()) {
				lua_pushnil(state);
				return 1;
			}

			const auto *bytes = reinterpret_cast<const std::byte *>(text->Value.data());

			ScriptValue value;
			if (Decode({bytes, text->Value.size()}, value) != CodecStatus::Ok) {
				lua_pushnil(state);
				return 1;
			}

			PushScriptValue(state, value);
			return 1;
		}

		// Rebuilds an arriving player in this world.
		//
		// **The engine admits them, not a script.** Who is in a game is the
		// host's business — `scene::AddPlayer` says so — and a teleport that
		// only worked in games whose author had written an arrival handler
		// would be a feature with a footnote. `Players.PlayerAdded` fires from
		// the parenting, so a game that *wants* to react already can.
		void AdmitArrival(ecs::Store &store, const Delivery &delivery) {
			ScriptValue envelope;
			if (Decode(delivery.Payload, envelope) != CodecStatus::Ok || envelope.Tag != ValueTag::Map) {
				return;
			}

			std::string name = "Player";
			const ScriptValue *data = nullptr;
			for (const auto &entry : envelope.Entries) {
				if (entry.first == "Player" && entry.second.Tag == ValueTag::String) {
					name = entry.second.Text;
				} else if (entry.first == "Data") {
					data = &entry.second;
				}
			}

			const ecs::Entity player = scene::AddPlayer(store, name);
			if (player == ecs::NULL_ENTITY) {
				// A world with no `Players` service takes nobody. Quiet rather
				// than an error, for `mono.server`'s reason: that is the
				// placeholder scene and it is furnished by nobody.
				return;
			}

			(void)scene::LoadCharacter(store, player);

			if (data == nullptr) {
				return;
			}

			// A copy, because `Encode` sorts a map's entries in place and the
			// delivery's tree is not this function's to reorder.
			ScriptValue carried = *data;

			std::vector<std::byte> bytes;
			if (Encode(carried, bytes) != CodecStatus::Ok) {
				return;
			}

			const ecs::Entity held =
				store.CreateInstance(ecs::Classes::Find(core::Name("StringValue")), TELEPORT_DATA);
			if (held == ecs::NULL_ENTITY) {
				return;
			}

			scene::TextContent text;
			text.Value.assign(reinterpret_cast<const char *>(bytes.data()), bytes.size());
			store.Set(held, text);
			store.SetParent(held, player);
		}

		// Four services, one shape.
		//
		// **`ServiceSurface` is the shape now**, and this is what is left of the
		// helper that used to live here: a name and a list, handed over.
		//
		// **`LuauMethods` and not `Methods`, which is the debt this file still
		// carries.** These four are the bus services and every one of them is
		// hand-written per language — `OpenJsMessagingService` and the rest are
		// in `JsBindings.cpp`. They work, and moving them across is the next
		// migration rather than this one; `ServiceSurface::LuauMethods` is where
		// that is visible instead of implied.
		void Install(lua_State *state, const char *name, std::span<const LuauServiceMethod> methods) {
			ServiceSurface surface;
			surface.Name = name;
			surface.LuauMethods = methods;
			InstallService(state, surface);
		}
	}

	void OpenBusSupport(lua_State *state) {
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

		lua_newtable(state);
		lua_setfield(state, LUA_REGISTRYINDEX, SUBSCRIPTIONS);

		// The cycle-detection set is not created here any more. It is held in
		// the registry rather than as a C++ container because what it keys on is
		// a Lua table's identity, and `PushVisiting` makes it on first use — so
		// the walk no longer depends on this function having run first.
	}

	void OpenMessagingService(lua_State *state) {
		static constexpr LuauServiceMethod METHODS[] = {
			{"PublishAsync", PublishAsync},
			{"SubscribeAsync", SubscribeAsync},
		};
		Install(state, "MessagingService", METHODS);
	}

	void OpenTeleportService(lua_State *state) {
		static constexpr LuauServiceMethod METHODS[] = {
			{"Teleport", Teleport},
			{"GetLocalPlayerTeleportData", GetLocalPlayerTeleportData},
			{"GetTeleportData", GetTeleportData},
		};
		Install(state, "TeleportService", METHODS);
	}

	void OpenMemoryStoreService(lua_State *state) {
		static constexpr LuauServiceMethod METHODS[] = {
			{"GetAsync", MemoryStoreGetAsync},
			{"SetAsync", MemoryStoreSetAsync},
			{"UpdateAsync", MemoryStoreUpdateAsync},
			{"RemoveAsync", MemoryStoreRemoveAsync},
		};
		Install(state, "MemoryStoreService", METHODS);
	}

	void OpenDataStoreService(lua_State *state) {
		// **No `UpdateAsync`, unlike the memory store above.** A compare-and-set
		// needs a version, and a durable store has none — the JavaScript side
		// declares the same three for the same reason.
		static constexpr LuauServiceMethod METHODS[] = {
			{"GetAsync", DataStoreGetAsync},
			{"SetAsync", DataStoreSetAsync},
			{"RemoveAsync", DataStoreRemoveAsync},
		};
		Install(state, "DataStoreService", METHODS);
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

			// **A channel message is delivered to the world rather than to a
			// topic**, so it goes to one signal with no subject rather than to a
			// list of subscribers keyed by name. See `SignalKind::
			// CrossWorldMessage` and `CrossWorldService`.
			if (delivery.Bus == BusKind::Channel) {
				ScriptValue value;
				if (Decode(delivery.Payload, value) == CodecStatus::Ok) {
					PushScriptValue(state, value);
				} else {
					lua_pushnil(state);
				}

				// **The sender's name, second, which is what makes a channel a
				// channel.** A topic subscriber is told which topic; a channel
				// receiver is told who to answer, because answering is the point
				// and the destination already knows it is itself.
				const std::string_view from = delivery.From.Text();
				lua_pushlstring(state, from.data(), from.size());

				std::string failure = FireSignal(state, SignalKind::CrossWorldMessage, ecs::NULL_ENTITY, 2);
				if (!failure.empty() && firstError.empty()) {
					firstError = std::move(failure);
				}
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

	size_t AdmitTeleports(ecs::Store &store) {
		size_t admitted = 0;

		// **Taken out of the inbox as it is admitted, and that is not tidiness.**
		// Reading without consuming means *whoever asks twice gets two people*,
		// and a host installing this system twice is not a hypothetical: the
		// studio's `Editor::BuildWorld` calls `client::InstallPresentation`,
		// which registers it, and then registered it again beside the physics
		// and gravity systems. Every arrival was therefore admitted twice — two
		// `Player` rows, two characters, one of them adopted by the play link
		// and the other an orphan nobody drives, standing in the world for ever
		// and one more of them per teleport.
		//
		// A second registration is now a wasted walk over an empty list rather
		// than a second person, which is the difference between a mistake that
		// costs nothing and one somebody has to photograph to find.
		//
		// **Only teleports are taken.** A subscriber's message has to still be
		// there when a runtime pumps — `PumpDeliveries` reads the same list —
		// so everything else is left exactly where the driver put it.
		// **The inbox is re-read every time round and nothing is held across an
		// admission.** Admitting creates instances, and a store that moves its
		// resource storage under a reference taken before the call is a
		// use-after-free waiting for a scene large enough to trigger it.
		for (;;) {
			auto *inbox = store.ResourceMutable<world::Inbox>();
			if (inbox == nullptr) {
				break;
			}

			const auto next =
				std::find_if(inbox->Arrived.begin(), inbox->Arrived.end(), [](const Delivery &delivery) {
					return delivery.Bus == BusKind::Teleport;
				});
			if (next == inbox->Arrived.end()) {
				break;
			}

			const Delivery taken = *next;
			inbox->Arrived.erase(next);

			AdmitArrival(store, taken);

			// `AdmitArrival` is quiet about a world with no `Players` service —
			// that is the placeholder scene and it takes nobody — so what is
			// counted is what it built rather than what arrived.
			admitted += scene::PlayersOf(store) == ecs::NULL_ENTITY ? 0u : 1u;
		}

		return admitted;
	}

	void RegisterTeleportAdmission(ecs::Scheduler &scheduler) {
		scheduler.Add("teleport.admit", ecs::Phase::PreSimulation, [](ecs::Store &store) {
			(void)AdmitTeleports(store);
		});
	}
}
