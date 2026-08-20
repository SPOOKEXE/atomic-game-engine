#pragma once

// How a system talks to a bus.
//
// A system takes the world and nothing else - that is `ecs/AGENTS.md`'s rule
// and it is not negotiable - so the mailbox has to be *in* the world. It is:
// `Outbox` and `Inbox` are resources on the world's store, which means they are
// covered by the affinity check, visible to the profiler, and carried by a
// snapshot. Pending traffic is world state, and a crash recovery that lost it
// would replay a world that had already sent things.
//
// `Postbox` is a thin view over those two resources. It owns nothing and costs
// nothing to construct, so a system makes one where it needs it rather than
// threading one through.
//
//     void Greet(ecs::Store &store) {
//         world::Postbox box(store);
//
//         for (const world::Delivery &arrived : box.Deliveries()) {
//             // Everything that reached this world since the last tick.
//         }
//
//         box.Publish("player.joined", payload);
//     }
//
// @tier L4 · shared

#include <engine/core/Bytes.hpp>
#include <engine/core/Name.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/world/Bus.hpp>

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

namespace engine::world {

	// Requests this world has made and not yet handed to the driver.
	//
	// A resource rather than a member of `World`, because a member would be
	// exactly the "private vector for data another module reads" that
	// `ecs/AGENTS.md` forbids - and because a snapshot has to carry it.
	//
	// @since v0.2
	struct Outbox {
		// In the order the world made them, which is what makes delivery a
		// merge rather than a sort.
		std::vector<Envelope> Pending;

		// The next ticket this world will issue.
		uint64_t NextTicket = 1;

		// The next sequence number. Per world, so `(From, Sequence)` totally
		// orders everything the universe applies in one barrier.
		uint64_t NextSequence = 1;
	};

	// What reached this world at the last barrier.
	//
	// Replaced wholesale each barrier rather than appended to, so a system that
	// forgets to drain it does not accumulate an unbounded backlog - it misses
	// messages, which is visible, instead of leaking, which is not.
	//
	// @since v0.2
	struct Inbox {
		// Sorted by `(From, Sequence)` before it is handed over.
		std::vector<Delivery> Arrived;
	};

	// How much bus traffic one world may generate per tick.
	//
	// Roblox has request budgets because they turned out to be necessary. A
	// world in a loop would otherwise fill the driver's barrier with its own
	// traffic and starve every other world in the universe.
	//
	// @since v0.2
	struct BusBudget {
		// Requests allowed per bus, per tick.
		uint32_t PerTick = 64;

		// Requests spent this tick. Reset at the barrier.
		uint32_t Spent = 0;
	};

	// Marks a world as a replica of one the server owns.
	//
	// A replica simulates its own copy and reconciles against authoritative
	// state. It must **not** write to a bus: DataStore, MemoryStore and
	// Teleport are authority operations, and a client issuing one would be
	// telling the universe something the server never said.
	//
	// Refused at the call rather than at review time, because "do not do this"
	// is a rule somebody eventually does. A replica may still *read* its inbox
	// - that is how it receives what the server published.
	//
	// @since v0.2
	struct Replica {
		// Whether this world is a replica. Present-and-false is the same as
		// absent, so a world can be demoted without removing the resource.
		bool Active = true;

		// The world this one mirrors, by the name that world is registered
		// under. Invalid where the authority is not in this process, which is
		// every replica a `--connect` client holds.
		//
		// **Because a replica's own name is not the name anything addresses it
		// by.** A host that mirrors a world gives the copy a name of its own -
		// the editor's is `"<world> (client 1)"` - so the registry can tell the
		// two apart, which rule 4 requires. Everything a scene *authored*
		// against that world still names the original: `Portal.DestinationWorld`
		// and `TeleportService` carry strings a person typed, and those strings
		// survive replication verbatim. So a world drawn from inside a replica
		// resolves a name against this rather than against `Universe::NameOf`,
		// or a pane onto another scene finds nothing and a pane back to this one
		// is not recognised as leading home.
		core::Name Of;

		// Whose copy this is, where a host mirrors the same worlds for several
		// viewers. The editor's is the client label - `"client 1"` - and it is
		// invalid for a process holding one view.
		//
		// **It is what keeps one viewer's rooms joined to each other.** Two
		// clients playing a universe of two worlds give four replicas, two
		// mirroring each world; a hole in client 1's copy of the first leads to
		// client 1's copy of the second, and pointing it at client 2's would
		// show one player another player's interpolated view.
		core::Name View;
	};

	// A system's view of its world's mailbox.
	//
	// Constructed per use and holding nothing: the state is the store's.
	//
	// @since v0.2
	class Postbox {
	  public:
		// Views the mailbox of one world.
		//
		// @param store The world's storage.
		explicit Postbox(ecs::Store &store) : Store_(&store) {}

		// Everything that arrived at the last barrier.
		//
		// Sorted by `(From, Sequence)`, so two runs of the same universe hand a
		// system the same messages in the same order.
		//
		// @return A view valid until the next barrier.
		std::span<const Delivery> Deliveries() const;

		// Publishes to a topic. Fire and forget.
		//
		// A publish with no subscribers is not an error. It is also not
		// delivered back to the publisher, because a world that had to filter
		// its own messages out of its own inbox would get it wrong once.
		//
		// @param topic   The topic to publish on.
		// @param payload The bytes to send.
		// @return `false` when the world is over budget.
		bool Publish(std::string_view topic, std::span<const std::byte> payload = {});

		// Subscribes this world to a topic.
		//
		// Idempotent. Takes effect at the next barrier, so a message published
		// in the same tick as the subscription is not received - which is the
		// honest answer, since the subscription did not exist when it was sent.
		//
		// @param topic The topic to subscribe to.
		// @return `false` when the world is over budget.
		bool Subscribe(std::string_view topic);

		// Unsubscribes this world from a topic.
		//
		// @param topic The topic to leave.
		// @return `false` when the world is over budget.
		bool Unsubscribe(std::string_view topic);

		// Reads a key. The reply arrives at a later tick.
		//
		// @param bus The bus to read from.
		// @param key The key to read.
		// @return The ticket the reply will carry, or NONE when over budget.
		Ticket Get(BusKind bus, std::string_view key);

		// Writes a key. The reply confirms it.
		//
		// @param bus     The bus to write to.
		// @param key     The key to write.
		// @param payload The value.
		// @return The ticket the reply will carry, or NONE when over budget.
		Ticket Set(BusKind bus, std::string_view key, std::span<const std::byte> payload);

		// Writes a key only if it is still at `version`.
		//
		// The read-modify-write a DataStore needs: two worlds updating one
		// player's inventory must not silently lose one of the writes. A
		// `Conflict` reply means re-read and retry.
		//
		// @param key     The key to write.
		// @param version The version the caller last saw.
		// @param payload The value.
		// @return The ticket the reply will carry, or NONE when over budget.
		Ticket Update(std::string_view key, uint64_t version, std::span<const std::byte> payload);

		// Removes a key.
		//
		// @param bus The bus to remove from.
		// @param key The key to remove.
		// @return The ticket the reply will carry, or NONE when over budget.
		Ticket Remove(BusKind bus, std::string_view key);

		// Appends to a MemoryStore queue.
		//
		// @param key     The queue.
		// @param payload The value to append.
		// @return The ticket the reply will carry, or NONE when over budget.
		Ticket Push(std::string_view key, std::span<const std::byte> payload);

		// Takes the front of a MemoryStore queue.
		//
		// The operation a matchmaker runs: several worlds pop the same queue and
		// each gets a different entry, because the barrier applies them one at a
		// time in a defined order.
		//
		// @param key The queue.
		// @return The ticket the reply will carry, or NONE when over budget.
		Ticket Pop(std::string_view key);

		// Sends a player identity and a payload to a named world.
		//
		// The destination rebuilds the character from its *own* class
		// definitions. No entity crosses: that is what keeps two worlds from
		// having to agree on class versions, and it is why there is no
		// cross-world reference anywhere in this module.
		//
		// @param world   The destination world's name.
		// @param payload What the destination needs to rebuild the player.
		// @return The ticket the reply will carry, or NONE when over budget.
		Ticket Teleport(std::string_view world, std::span<const std::byte> payload);

		// Opens a named channel on this world, so sends may address it.
		//
		// **A channel is opened by the world that receives on it**, which is what
		// makes `NoSuchChannel` answerable at all: the bus knows what a world is
		// listening for and can tell a sender that named something else. Nothing
		// is delivered on a channel this was never called for.
		//
		// Idempotent, and it takes effect at the next barrier - so a message sent
		// in the same tick as the open is refused, which is the honest answer
		// since the channel did not exist when it was addressed. That is
		// `Subscribe`'s rule, said again because it is the same one.
		//
		// **A ticket rather than a boolean, alone among the opens and closes on
		// this bus, because this is the one of them an authority may refuse.**
		// `UniverseSettings::ChannelsPerWorld` caps how many channels a world may
		// hold, and the count lives in the router's table - a world cannot answer
		// it from its own store without keeping a second copy of that table, and
		// the copy would drift the first time an open was refused. So the answer
		// is decided at the barrier and comes back the way every other barrier
		// verdict does: a `Delivery` on this ticket, `Bus` set to `Channel`, `Key`
		// to the channel, `Status` to `Ok` or `TooManyChannels`.
		//
		// A caller that ignores the reply is not left guessing silently: senders
		// addressing the channel are refused `NoSuchChannel`. It is the same
		// answer a channel nobody ever asked for gets, though, so the reply is
		// the only thing that tells a refused open from a forgotten one.
		//
		// @param channel The channel to listen on.
		// @return The ticket the verdict will carry, or NONE when the world is
		//         over budget or is a replica.
		// @since v0.17
		Ticket OpenChannel(std::string_view channel);

		// Closes a named channel on this world.
		//
		// Sends addressed to it afterwards are `NoSuchChannel`. Deliveries
		// already queued for this barrier are not withdrawn: they were accepted
		// while the channel was open, and unpicking that would make what a world
		// received depend on what it did later in the same tick.
		//
		// Still a boolean where `OpenChannel` is a ticket, because a close cannot
		// be refused by anybody: a world giving a channel up needs no permission,
		// and closing one it never opened is a no-op rather than a failure.
		//
		// @param channel The channel to stop listening on.
		// @return `false` when the world is over budget.
		// @since v0.17
		bool CloseChannel(std::string_view channel);

		// Sends a payload to a named channel on a named world, with nobody
		// attached.
		//
		// **The addressed route out of a world, which this module did not have.**
		// `Publish` is a topic fan-out - the sender does not know or care who is
		// listening, which is right for "the boss died" and wrong for "world B,
		// here is the score you asked for". `Teleport` is the only other
		// operation that names a world and it moves a *person*, so a game wanting
		// to say something to one particular world had to either broadcast it to
		// everybody or teleport a player carrying it.
		//
		// **The channel is half the address, and v0.15's cut had only the other
		// half.** One unnamed pipe per world pair meant every receiver in the
		// destination saw everything the pair exchanged, so two subsystems talking
		// between the same two worlds read each other's traffic and had to tell it
		// apart by looking inside the payload. Naming the channel puts that
		// distinction where the bus can enforce it.
		//
		// **No entity crosses, exactly as with a teleport**, and for the same
		// reason: rule 3 says nothing crossing a world boundary is a pointer, and
		// two worlds must not have to agree on class versions to talk.
		//
		// **Every refusal is a `BusStatus` on the reply rather than silence.** A
		// publish with no subscribers is a quiet afternoon; a message addressed to
		// a name nothing answers to is a mistake the sender wants told about, and
		// being able to tell the difference is half the reason this exists beside
		// `Publish`. `BusStatus` carries the table of what each case answers.
		//
		// The delivery arrives with `Bus == BusKind::Channel`, `Key` set to the
		// channel and `From` to the sender - a channel is the one route where
		// answering is the point, and the destination already knows it is itself.
		//
		// @param world   The destination world's name.
		// @param channel The channel on it, which that world must have opened.
		// @param payload The bytes to send.
		// @return The ticket the reply will carry, or NONE when over budget.
		// @since v0.15
		Ticket SendTo(std::string_view world, std::string_view channel, std::span<const std::byte> payload);

		// Reports whether this world is a replica, and so may not write.
		//
		// @return `true` when bus writes are refused.
		bool IsReplica() const;

		// How much of this world's allowance is left this tick.
		//
		// @return Requests still available.
		uint32_t Remaining() const;

	  private:
		// Queues an envelope, or reports that the budget is spent.
		//
		// `target` is the destination world and only a channel send has one, so
		// it is last and defaulted rather than sitting beside `key` - the two are
		// both `core::Name` and adjacent is where a positional call swaps them.
		Ticket Post(
			BusKind bus,
			BusOperation operation,
			core::Name key,
			std::span<const std::byte> payload,
			uint64_t version,
			bool wantsReply,
			core::Name target = {}
		);

		ecs::Store *Store_;
	};

	// Registers the mailbox resource types with serialisers of their own.
	//
	// An outbox holds a vector and a `core::Name`, neither of which survives
	// being written as its object representation - the vector is a pointer and
	// the name is a process-local id. Called once at startup by whatever builds
	// a universe.
	void RegisterMailboxTypes();
}
