#include "Bindings.hpp"

#include <engine/world/Postbox.hpp>

#include <cstring>
#include <lualib.h>
#include <string>

namespace engine::script {

	namespace {
		using ecs::Store;

		// Where the topic-to-callback table lives.
		constexpr const char *SUBSCRIPTIONS = "engine.messaging.subscriptions";

		Store &StoreOfUpvalue(lua_State *state) {
			return *static_cast<Store *>(lua_tolightuserdata(state, lua_upvalueindex(1)));
		}

		// MessagingService:PublishAsync(topic, message)
		//
		// **The only way out of a world.** A script holds one `Store` and there
		// is no binding anywhere that hands it another, so this bus is the
		// crossing — which is rule 3 expressed as an API rather than as a
		// convention: nothing crossing a world boundary is a pointer, so the
		// message is bytes and the far side gets a copy.
		int PublishAsync(lua_State *state) {
			world::Postbox box(StoreOfUpvalue(state));

			const char *topic = luaL_checkstring(state, 2);
			size_t length = 0;
			const char *message = luaL_checklstring(state, 3, &length);

			// **Strings, not tables, and that is a stated limit rather than an
			// oversight.** Roblox's `MessagingService` takes a table, and a
			// table needs a value-to-bytes codec that sorts its keys and
			// produces identical bytes from both VMs — `docs/SCRIPT_CONCURRENCY.md`
			// §3 has the requirements. A codec that walked a hash map in memory
			// order would serialise differently on two runs of one script and
			// break `just determinism` a long way from here.
			const auto *bytes = reinterpret_cast<const std::byte *>(message);
			if (!box.Publish(topic, {bytes, length})) {
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
		// yielding and is legal under `docs/SCRIPT_CONCURRENCY.md` §1. That is
		// why subscribing works today and `GetAsync` does not.
		int SubscribeAsync(lua_State *state) {
			world::Postbox box(StoreOfUpvalue(state));

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

		lua_newtable(state);
		lua_setfield(state, LUA_REGISTRYINDEX, SUBSCRIPTIONS);

		lua_newtable(state);

		lua_pushlightuserdata(state, &store);
		lua_pushcclosure(state, PublishAsync, "PublishAsync", 1);
		lua_setfield(state, -2, "PublishAsync");

		lua_pushlightuserdata(state, &store);
		lua_pushcclosure(state, SubscribeAsync, "SubscribeAsync", 1);
		lua_setfield(state, -2, "SubscribeAsync");

		lua_setglobal(state, "MessagingService");
	}

	std::string PumpDeliveries(lua_State *state, ecs::Store &store) {
		const world::Postbox box(store);
		const auto deliveries = box.Deliveries();
		if (deliveries.empty()) {
			return {};
		}

		std::string firstError;
		lua_getfield(state, LUA_REGISTRYINDEX, SUBSCRIPTIONS);

		for (const world::Delivery &delivery : deliveries) {
			if (delivery.Bus != world::BusKind::Messaging) {
				continue;
			}

			lua_getfield(state, -1, delivery.Key.Text().data());
			if (lua_isnil(state, -1)) {
				lua_pop(state, 1);
				continue;
			}

			const int count = lua_objlen(state, -1);
			for (int index = 1; index <= count; index++) {
				lua_rawgeti(state, -1, index);

				// `(message, topic)`. Roblox hands a table with `Data` and
				// `Sent`; this hands what it actually has, and adding fields
				// that were not measured would be inventing a contract.
				lua_pushlstring(
					state, reinterpret_cast<const char *>(delivery.Payload.data()), delivery.Payload.size()
				);
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
			lua_pop(state, 1);
		}

		lua_pop(state, 1);
		return firstError;
	}
}
