#pragma once

// One barrier's worth of bus traffic: applied to the four backends, then fanned
// out to the worlds it landed for.
//
// Private to this module. A bus is reached through `Postbox` from inside a
// world, or not at all — and an envelope is applied by the driver at the
// barrier, or not at all.
//
// **Everything happens on the driver thread, at the barrier.** That is what
// makes ordering a property of the data rather than of the scheduler: the
// router walks every world's outbox in `(From, Sequence)` order and applies one
// operation at a time. Two runs of the same universe therefore apply the same
// operations in the same order, which is what a replay needs.
//
// The worlds arrive as a WorldDirectory rather than as the Universe holding
// them. That is the whole of the coupling and it is three lookups wide: the
// router can find a world and read one, and can neither create one, destroy one
// nor tick one.
//
// @tier L4 · shared

#include "Buses.hpp"

#include <engine/core/Bytes.hpp>
#include <engine/core/Name.hpp>
#include <engine/world/Bus.hpp>
#include <engine/world/Universe.hpp>
#include <engine/world/World.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace engine::world {

	// The world registry, as much of it as routing needs.
	//
	// A non-owning view, built where it is passed, so the router never holds a
	// world past the barrier it was handed one for.
	struct WorldDirectory {
		// Every slot, holes included — a destroyed world leaves a null behind
		// and the slots after it do not shift up.
		std::span<const std::unique_ptr<World>> Registry;

		// The host holding each world, parallel to Registry. An invalid Name is
		// a world this process holds.
		std::span<const core::Name> Hosts;

		// The world behind a handle.
		//
		// @param id The handle to resolve.
		// @return The world, or `nullptr` for an unknown or destroyed one.
		World *Reach(WorldId id) const;

		// The world registered under a name.
		//
		// The lookup everything crossing a boundary uses, because a name is
		// what crosses and an index is not.
		//
		// @param name The world's stable name.
		// @return The handle, or an invalid handle.
		WorldId Find(core::Name name) const;

		// The host holding a world.
		//
		// @param id The world to ask about.
		// @return The host's name, or an invalid Name for a local or unknown
		//         world.
		core::Name HostOf(WorldId id) const;
	};

	// What one barrier did, for the universe's diagnostics.
	struct BarrierCounts {
		// Bus operations applied.
		uint64_t BusOperations = 0;

		// Deliveries handed to worlds, wherever those worlds live.
		uint64_t Deliveries = 0;
	};

	// The four bus backends, and the barrier that applies traffic to them.
	class BusRouter {
	  public:
		// Applies one barrier's traffic, then hands every world its arrivals.
		//
		// Three shapes, and they share the collection and the ordering because
		// two routing paths agree until the day one of them does not: a replay
		// applies what it was injected, a federated host collects and forwards
		// without applying, and everything else applies what it collected.
		//
		// @param directory The worlds this barrier covers.
		// @param settings  The universe's budget and whether it is federated.
		// @return What the barrier applied and delivered.
		BarrierCounts Route(const WorldDirectory &directory, const UniverseSettings &settings);

		// Queues one delivery for a world's next barrier.
		//
		// @param id       The world to deliver to.
		// @param delivery What arrived for it.
		// @return `false` for a world with no fanout slot, which is an unknown
		//         world or one created since the last barrier.
		bool Deliver(WorldId id, Delivery delivery);

		// Uses `traffic` for the next barrier instead of the worlds' outboxes.
		//
		// @param traffic The recorded envelopes, in the order they were applied.
		void Inject(std::vector<Envelope> traffic);

		// Accepts what a host's worlds posted, for the next barrier.
		//
		// **An envelope whose sender is not one of that host's worlds is
		// dropped.** This is `Envelope::From` being stamped rather than trusted,
		// carried across a process boundary: a host that could claim to be a
		// world it does not hold could read that world's replies and publish in
		// its name.
		//
		// @param host      The host that sent them.
		// @param traffic   The envelopes it collected.
		// @param directory The worlds, to check each sender against.
		// @return The number accepted. Anything less means some were refused.
		size_t Ingest(core::Name host, std::span<const Envelope> traffic, const WorldDirectory &directory);

		// The traffic applied at the most recent barrier.
		//
		// @return A view valid until the next barrier.
		std::span<const Envelope> LastTraffic() const {
			return {Applied.data(), Applied.size()};
		}

		// What the last barrier decided for worlds that live elsewhere.
		//
		// @return The pending deliveries, in barrier order.
		std::span<const RemoteDelivery> Outbound() const {
			return Outgoing;
		}

		// Takes the pending remote deliveries, leaving none.
		//
		// @return The deliveries, in barrier order.
		std::vector<RemoteDelivery> TakeOutbound();

		// The number of worlds subscribed to a topic.
		//
		// @param topic The topic to count.
		// @return The subscriber count.
		size_t SubscriberCount(core::Name topic) const;

		// Reads a bus value directly, without going through a barrier.
		//
		// @param bus   The bus to read.
		// @param key   The key to read.
		// @param value Filled with the value when one is present.
		// @return `Ok`, `NotFound`, or `Unsupported` for a bus with no keys.
		BusStatus Peek(BusKind bus, core::Name key, std::vector<std::byte> *value) const;

		// Empties the backends and everything queued against them.
		//
		// The fanout is not queued state — it is scratch, resized and cleared at
		// the top of every barrier — and neither is the replay flag, which says
		// where the next barrier's traffic comes from rather than holding any.
		void Reset();

		// Appends the four backends: topics, values, queues, records.
		//
		// Written by **name** throughout, and each block sorted by name text.
		// Not by name id: an id is assigned in interning order, and a universe
		// restored from a snapshot interns in a different order than the one
		// that wrote it. Sorting by id therefore made a re-save differ from the
		// original byte for byte, which is exactly the property a recording
		// needs to be comparable.
		//
		// @param writer    The writer to append to.
		// @param directory The worlds, to name the subscribers with.
		void WriteBuses(core::ByteWriter &writer, const WorldDirectory &directory) const;

		// Reads the four backends back over whatever is here.
		//
		// A subscriber naming a world the reader does not hold is dropped rather
		// than refused: the worlds block is read first, so by here the registry
		// is everything this snapshot is going to have.
		//
		// @param reader    The reader to consume.
		// @param directory The worlds, to resolve the subscriber names against.
		void ReadBuses(core::ByteReader &reader, const WorldDirectory &directory);

	  private:
		void ApplyEnvelope(World &sender, const Envelope &envelope, const WorldDirectory &directory);
		uint64_t DeliverInboxes(const WorldDirectory &directory, const UniverseSettings &settings);

		Buses Backends;

		// Deliveries built during one barrier, keyed by destination world.
		// Reused between barriers so routing allocates nothing in a steady
		// universe.
		std::vector<std::vector<Delivery>> Fanout;

		// The world tick each world's inbox was filled at, by world index.
		//
		// **Because a barrier is not a tick, in either direction.** A barrier
		// runs once per host frame and a world's systems run at the world's own
		// rate — the studio measured 200 barriers against 91 ticks in one world,
		// and a world owing catch-up ticks is the same mismatch the other way.
		// Replacing an inbox unconditionally therefore took mail away before any
		// system had seen it; never replacing it hands the same mail over on
		// every later tick. This is the stamp that tells the two apart: mail is
		// dropped once the world's clock has moved past the tick it arrived at,
		// and kept until then.
		//
		// Here rather than on `Inbox` because it is the router's bookkeeping and
		// not a fact about the world — a field on the resource would ride in
		// every snapshot and on the wire.
		std::vector<uint64_t> DeliveredAt;

		// What the last barrier applied, and what the next one should apply
		// instead of collecting. Kept across barriers so a steady universe
		// allocates nothing for either.
		std::vector<Envelope> Applied;
		std::vector<Envelope> Injected;
		bool Replaying = false;

		// What hosts handed over, awaiting the next barrier.
		std::vector<Envelope> Ingested;

		// Deliveries for worlds that live elsewhere, awaiting the driver.
		std::vector<RemoteDelivery> Outgoing;
	};
}
