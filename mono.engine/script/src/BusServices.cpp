// The four services that reach out of a world, in neither language.
//
// **`MessagingService` fans out, `TeleportService` addresses, and the two stores
// answer a key.** All four sit on `world::Postbox`, which is the only crossing -
// rule 3 expressed as an API rather than as a convention: nothing crossing a
// world boundary is a pointer, so what goes is bytes and the far side gets a
// copy.
//
// **Written twice until v0.16, and one of the two copies was missing a
// method.** A retired `Services.cpp` held the Luau half and `JsBindings.cpp` and
// `JsDatatypes.cpp` held the JavaScript one, so `TeleportService.GetTeleportData`
// existed in one language and not the other - the per-service gap
// `ServiceSurface::LuauMethods` was invented to make visible and which the
// catalogue's row could not express. Describing the four once closes it: the
// method is a `ServiceMethod` row, both VMs install every row, and a JavaScript
// authority can now read what arrived with a player.
//
// **What made the stores describable is `ScriptCall::Await`.** A `Get` returns a
// `Ticket`, the reply lands at a later barrier applied in sorted order, and the
// two languages suspend on it completely differently - a yielded coroutine and a
// `Promise`. That is one member on the interface rather than four methods
// written twice, and it is the last thing about these services that a VM
// decides.
//
// **Four descriptions and no system, since v0.18.** `AdmitTeleports` - the pass
// that takes in the people a teleport sent here - ran on every world whether or
// not it had a script, and compiling it beside these four meant it was built
// against `<lua.h>`. It is `Teleport.cpp` now, and what the two still share is
// one child name.
//
// @tier L9 · shared

#include "Teleport.hpp"

#include <engine/ecs/Store.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Services.hpp>
#include <engine/script/Codec.hpp>
#include <engine/script/LuauTags.hpp>
#include <engine/script/ScriptCall.hpp>
#include <engine/script/ServiceSurface.hpp>
#include <engine/script/TeleportRequest.hpp>
#include <engine/world/Postbox.hpp>

#include <array>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace engine::script {

	namespace {
		namespace scene = engine::scene;
		using ecs::Store;
		using world::BusKind;
		using world::Delivery;
		using world::Postbox;
		using world::Ticket;

		constexpr const char *TELEPORT_METHODS_KEY = "engine.teleportservice.methods";

		// Whether this world's writes belong to somebody else.
		//
		// **Two flags, and checking one would have been checking half.**
		// `Postbox::IsReplica` reads the bus's own `Replica` resource, and
		// `Store::AdoptOnly` is what `SetProperty` refuses on - the same fact
		// recorded in two places, which is a rule-2 smell in `world` rather than
		// here. A script guard that consulted only one would let a write through
		// on a world the other had already disowned, so this consults both and
		// the reason is written down rather than left to whoever finds it.
		bool WritesBelongElsewhere(const Postbox &box, const Store &store) {
			return box.IsReplica() || store.AdoptOnly();
		}

		// Reads an argument and encodes it, raising a named refusal.
		//
		// **One helper for all four services**, because "the value cannot cross a
		// world boundary" is one sentence and it was four copies of it.
		std::vector<std::byte> EncodeArgument(ScriptCall &call, size_t index) {
			ScriptValue value;
			CodecStatus why = CodecStatus::Ok;

			if (!call.ReadValue(index, value, why)) {
				call.Raise(
					(std::string("the value cannot cross a world boundary: ") + Describe(why)).c_str()
				);
			}

			std::vector<std::byte> bytes;
			if (const CodecStatus status = Encode(value, bytes); status != CodecStatus::Ok) {
				call.Raise(
					(std::string("the value cannot cross a world boundary: ") + Describe(status)).c_str()
				);
			}
			return bytes;
		}

		// Suspends on a ticket, refusing by name when the bus took nothing.
		//
		// **The refusal is here and the suspension is the adapter's**, which is
		// the split `ScriptCall::Await` describes: an unexpected ticket means the
		// world spent its allowance, and only the caller knows which method to
		// name in the message.
		void AwaitTicket(ScriptCall &call, Ticket ticket, const char *what) {
			if (!ticket.Expected()) {
				call.Raise((std::string(what) + ": over this world's budget").c_str());
			}
			call.Await(ticket.Value);
		}

		// --- MessagingService -------------------------------------------------

		// `MessagingService:PublishAsync(topic, message)`
		//
		// **The only way out of a world to *everybody*.** A script holds one
		// `Store` and there is no binding anywhere that hands it another, so this
		// bus is the crossing.
		//
		// **A table, not only a string.** v0.5 took strings because the codec did
		// not exist; it does, so Roblox's actual signature works and the bytes
		// are the same from either VM.
		void PublishAsync(ScriptCall &call) {
			const std::string topic = call.AsString(0);
			const std::vector<std::byte> payload = EncodeArgument(call, 1);

			if (!Postbox(call.World()).Publish(topic.c_str(), payload)) {
				// Over budget. Named rather than silent: each bus gives a world
				// an allowance per tick, and a publish that vanished would look
				// like a subscriber that never fired.
				call.Raise(("PublishAsync: over this world's budget for '" + topic + "'").c_str());
			}
		}

		// `MessagingService:SubscribeAsync(topic, callback)`
		//
		// The callback fires when the barrier delivers, which is a deterministic
		// point in a deterministic order - so this needs no suspension and is
		// legal under `docs/retired/SCRIPT_CONCURRENCY.md` §1.
		//
		// **The handler is read before the bus is told, and released if the bus
		// refuses.** `RetainCallback` is what checks the argument is a function
		// at all, so subscribing first would register this world's interest in a
		// topic on the way to raising about the second argument - a subscription
		// nothing can ever withdraw. Retaining first costs one release on the
		// budget path and nothing on the ordinary one.
		void SubscribeAsync(ScriptCall &call) {
			const std::string topic = call.AsString(0);
			const CallbackRef callback = call.RetainCallback(1);

			if (!Postbox(call.World()).Subscribe(topic.c_str())) {
				call.ReleaseCallback(callback);
				call.Raise(("SubscribeAsync: over this world's budget for '" + topic + "'").c_str());
			}

			call.Subscriptions().Add(topic, callback);
		}

		constexpr std::array<ServiceMethod, 2> MESSAGING{{
			{"PublishAsync", PublishAsync},
			{"SubscribeAsync", SubscribeAsync},
		}};

		// --- the stores, and the suspension -----------------------------------
		//
		// **These are the calls a script genuinely suspends on, and they are the
		// reason `task` had to come first.** A `Get` returns a `Ticket`; the reply
		// lands in the inbox at a later tick, applied sorted at the barrier. So
		// the resume source is §1's *first* legal case - a `Ticket` reply the
		// barrier applied.
		//
		// §5's three refusals are part of the contract rather than an
		// afterthought: `OverBudget` when the world has spent its allowance, a
		// replica refusing a write, and `NotFound` / `Conflict` from the bus
		// itself. Each arrives as a value the script can test - see
		// `DescribeStatus`.

		// Both stores' `GetAsync`, `SetAsync` and `RemoveAsync`.
		//
		// **A template over the kind rather than six functions**, because the only
		// difference between a memory store and a data store is how long an entry
		// lives; the JavaScript half already shared its three through a magic
		// number and the Luau half had six copies.
		template <BusKind KIND> void StoreGetAsync(ScriptCall &call) {
			AwaitTicket(call, Postbox(call.World()).Get(KIND, call.AsString(0).c_str()), "GetAsync");
		}

		template <BusKind KIND> void StoreSetAsync(ScriptCall &call) {
			const std::string key = call.AsString(0);
			const std::vector<std::byte> payload = EncodeArgument(call, 1);

			Postbox box(call.World());
			if (WritesBelongElsewhere(box, call.World())) {
				// **`RunService`'s `IsReplica` without a call syntax**, because
				// this sentence is read in two languages and `:` is one of them:
				// a JavaScript author told to write `RunService:IsReplica()` is
				// being told to write a syntax error.
				call.Raise(
					"SetAsync: this world is a replica, and a store write here would be applied and then "
					"overwritten by the next delta. Ask RunService's IsReplica first"
				);
			}
			AwaitTicket(call, box.Set(KIND, key.c_str(), payload), "SetAsync");
		}

		template <BusKind KIND> void StoreRemoveAsync(ScriptCall &call) {
			AwaitTicket(call, Postbox(call.World()).Remove(KIND, call.AsString(0).c_str()), "RemoveAsync");
		}

		// `MemoryStoreService:UpdateAsync(key, version, value)`
		//
		// **The compare-and-swap, which is the cross-world lock.**
		// `docs/retired/SCRIPT_CONCURRENCY.md` §4: a lock in the shape an author
		// expects cannot exist here, because rule 3 leaves no shared memory to
		// guard. What they actually want is this - the version the caller read
		// goes in, and `Conflict` comes back when it has moved on.
		void MemoryStoreUpdateAsync(ScriptCall &call) {
			const std::string key = call.AsString(0);
			const auto version = static_cast<uint64_t>(call.AsNumber(1));
			const std::vector<std::byte> payload = EncodeArgument(call, 2);

			AwaitTicket(call, Postbox(call.World()).Update(key.c_str(), version, payload), "UpdateAsync");
		}

		constexpr std::array<ServiceMethod, 4> MEMORY_STORE{{
			{"GetAsync", StoreGetAsync<BusKind::MemoryStore>},
			{"SetAsync", StoreSetAsync<BusKind::MemoryStore>},
			{"UpdateAsync", MemoryStoreUpdateAsync},
			{"RemoveAsync", StoreRemoveAsync<BusKind::MemoryStore>},
		}};

		// **No `UpdateAsync`, unlike the memory store.** A compare-and-set needs a
		// version and a durable store has none.
		constexpr std::array<ServiceMethod, 3> DATA_STORE{{
			{"GetAsync", StoreGetAsync<BusKind::DataStore>},
			{"SetAsync", StoreSetAsync<BusKind::DataStore>},
			{"RemoveAsync", StoreRemoveAsync<BusKind::DataStore>},
		}};

		// --- TeleportService --------------------------------------------------
		//
		// **The one bus operation that names a world, and no script could reach
		// it until v0.15.** `world::BusKind::Teleport` and `Postbox::Teleport`
		// have existed since v0.2 with a router that delivers and an inbox that
		// receives - and the delivery pump dropped every arrival on the floor,
		// because it only ever looked for `Messaging`. So the crossing worked and
		// nothing could ask for one or notice one.
		//
		// **What crosses is a name and a payload, never an entity.** That is rule
		// 3 and it is also what makes the feature work: the destination rebuilds
		// the player from its *own* class definitions, so two worlds never have to
		// agree about what a `Player` is made of. Roblox works the same way and
		// for the same reason - the far side is another server.

		// The data a player is carrying, as the language's own value, or nil.
		//
		// **Nil for a player who walked in the front door**, because that is the
		// ordinary case and not a mistake: a game asks every arrival and acts on
		// the ones that came through a portal.
		void ReturnCarriedData(ScriptCall &call, ecs::Entity player) {
			const Store &store = call.World();

			const ecs::Entity held = store.FindFirstChild(player, TELEPORT_DATA);
			const auto *text = held == ecs::NULL_ENTITY ? nullptr : store.Get<scene::TextContent>(held);
			if (text == nullptr || text->Value.empty()) {
				call.ReturnNil();
				return;
			}

			const auto *bytes = reinterpret_cast<const std::byte *>(text->Value.data());

			ScriptValue value;
			if (Decode({bytes, text->Value.size()}, value) != CodecStatus::Ok) {
				call.ReturnNil();
				return;
			}

			call.ReturnValue(value);
		}

		// `TeleportService:Teleport(placeName, player, data?)`
		void Teleport(ScriptCall &call) {
			Store &store = call.World();

			const std::string place = call.AsString(0);
			const ecs::Entity player = call.AsInstance(1);

			if (!store.Alive(player) || !store.IsA(player, scene::PlayerClass())) {
				call.Raise("Teleport: the second argument must be a Player");
			}

			ScriptValue data;
			const ScriptValue *carried = nullptr;
			if (!call.IsNil(2)) {
				CodecStatus why = CodecStatus::Ok;
				if (!call.ReadValue(2, data, why)) {
					call.Raise((std::string("Teleport: the data cannot cross a world boundary: ") +
								Describe(why))
								   .c_str());
				}
				carried = &data;
			}

			Postbox box(store);
			if (WritesBelongElsewhere(box, store)) {
				const auto *local = store.Resource<scene::LocalPlayer>();
				if (local == nullptr || local->Instance != player) {
					call.Raise("Teleport: a replica may request a teleport only for its local Player");
				}
				std::string failure;
				if (!QueueTeleportRequest(store, place, data, failure)) {
					call.Raise(failure.c_str());
				}
				return;
			}

			std::string failure;
			if (!TeleportPlayer(store, place, player, carried, failure)) {
				call.Raise(failure.c_str());
			}
		}

		// `TeleportService.TeleportRequested = function(request) -> result`
		//
		// The handler is write-only: only the authority invokes it, and handing a
		// script another script's closure would violate the VM boundary.
		void GetTeleportRequested(ScriptCall &call) {
			call.ReturnNil();
		}

		void SetTeleportRequested(ScriptCall &call) {
			Store &store = call.World();
			if (auto *previous = store.ResourceMutable<TeleportRequestHandler>();
				previous != nullptr && previous->Callback.Valid()) {
				call.ReleaseHostCallback(previous->Callback);
			}

			if (call.IsNil(0)) {
				store.RemoveResource<TeleportRequestHandler>();
				return;
			}
			store.SetResource(TeleportRequestHandler{call.RetainHostCallback(0)});
		}

		// `TeleportService:GetLocalPlayerTeleportData()`
		void GetLocalPlayerTeleportData(ScriptCall &call) {
			const Store &store = call.World();

			const auto *local = store.Resource<scene::LocalPlayer>();
			if (local == nullptr || !store.Alive(local->Instance)) {
				// **Nil on a server, which is the point of the name.** Roblox's
				// is a client call; a `Script` reaching for it gets nothing
				// rather than somebody else's data.
				call.ReturnNil();
				return;
			}

			ReturnCarriedData(call, local->Instance);
		}

		// `TeleportService:GetTeleportData(player)`
		//
		// **The server's half of the call above, and the reason it exists is that
		// the payload had no reader on the side that can act on it.**
		// `GetLocalPlayerTeleportData` is Roblox's and is a *client* call - it
		// answers for `LocalPlayer` and nobody else, deliberately, so a script
		// cannot read somebody else's data. But the machine that decides where an
		// arriving character stands is the authority, and it had no way to see
		// what the sender wrote. A payload that only the arriving client can read
		// cannot place the arriving body.
		//
		// **Reachable from JavaScript since v0.16**, which is what describing the
		// service once bought: this was the per-method gap the roadmap named, and
		// closing it took no new mechanism at all.
		void GetTeleportData(ScriptCall &call) {
			const ecs::Entity player = call.AsInstance(0);
			if (!call.World().Alive(player) || !call.World().IsA(player, scene::PlayerClass())) {
				call.Raise("GetTeleportData: the argument must be a Player");
			}

			ReturnCarriedData(call, player);
		}

		constexpr std::array<ServiceMethod, 3> TELEPORT{{
			{"Teleport", Teleport},
			{"GetLocalPlayerTeleportData", GetLocalPlayerTeleportData},
			{"GetTeleportData", GetTeleportData},
		}};

		constexpr std::array<ServiceProperty, 1> TELEPORT_PROPERTIES{{
			{"TeleportRequested", GetTeleportRequested, SetTeleportRequested},
		}};

		constexpr std::array<ServiceSignal, 1> TELEPORT_SIGNALS{{
			{"TeleportResult", SignalKind::TeleportResult},
		}};
	}

	const ServiceSurface &MessagingServiceSurface() {
		static const ServiceSurface SURFACE = [] {
			ServiceSurface surface;
			surface.Name = "MessagingService";
			surface.Methods = MESSAGING;
			return surface;
		}();
		return SURFACE;
	}

	const ServiceSurface &TeleportServiceSurface() {
		static const ServiceSurface SURFACE = [] {
			ServiceSurface surface;
			surface.Name = "TeleportService";
			surface.Methods = TELEPORT;
			surface.Properties = TELEPORT_PROPERTIES;
			surface.Signals = TELEPORT_SIGNALS;
			surface.Tag = TAG_TELEPORT_SERVICE;
			surface.MethodsKey = TELEPORT_METHODS_KEY;
			return surface;
		}();
		return SURFACE;
	}

	const ServiceSurface &MemoryStoreServiceSurface() {
		static const ServiceSurface SURFACE = [] {
			ServiceSurface surface;
			surface.Name = "MemoryStoreService";
			surface.Methods = MEMORY_STORE;
			return surface;
		}();
		return SURFACE;
	}

	const ServiceSurface &DataStoreServiceSurface() {
		static const ServiceSurface SURFACE = [] {
			ServiceSurface surface;
			surface.Name = "DataStoreService";
			surface.Methods = DATA_STORE;
			return surface;
		}();
		return SURFACE;
	}
}
