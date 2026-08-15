#pragma once

// What crosses a world boundary, and the only shape it may take.
//
// **A world may name another world and may not reach into one.** Everything that
// crosses goes through a bus, and four of the five are the Roblox ones -
// MessagingService, MemoryStore, DataStore, Teleport - because those are the
// shapes a game actually needs and because a Luau author already knows their
// semantics. `Channel` is the fifth and is this engine's own.
//
// **What is absent is the part that matters: there is no reference to an entity
// in another world.** A destination is a `core::Name` and a payload is bytes, so
// the type that would have broken "nothing crossing a world boundary is a
// pointer" does not exist and cannot be built out of what is here. Naming a world
// that has been destroyed is answered - `NoSuchWorld` - rather than being a
// dangling anything.
//
// **Everything is asynchronous**, because a world tick occupies a job worker
// and blocking one stalls every other world in the host. A call returns a
// `Ticket` and the reply lands in the world's inbox at a later tick - which is
// the same contract `:GetAsync()` already teaches, so the engine's requirement
// and the script author's expectation are the same thing.
//
// **Ordering is decided in one place.** Traffic is applied at the driver's
// barrier, on one thread, in `(From.Text(), Sequence)` order. Each world's
// outbox is already ordered by construction, so delivery is a k-way merge
// rather than a sort - and two runs of the same universe apply the same
// operations in the same order, which is what a replay needs.
//
// **The sort key is the sender's name *text*, not its `Name::Id()`**, and that
// is rule 4 rather than a preference. An id is handed out in interning order, so
// it depends on which world happened to be named first in this process - a
// universe restored from a snapshot interns in file order where the run that
// wrote it interned in creation order, and the same two envelopes then apply in
// the opposite order. `BusRouter::SortedKeys` already argues exactly this for the
// snapshot codec; the barrier's sort is the same rule and had the same bug.
//
// @tier L4 · shared

#include <engine/core/Bytes.hpp>
#include <engine/core/Name.hpp>

#include <cstdint>
#include <vector>

namespace engine::world {

	// Which bus an operation is for.
	//
	// @since v0.2
	enum class BusKind : uint8_t {
		// Publish and subscribe over named topics. Fire and forget: a publish
		// with no subscribers is not an error, it is a quiet afternoon.
		Messaging,

		// Ephemeral shared state - a matchmaking queue, a leaderboard, a
		// cross-world counter. Fast, and gone when the run ends.
		MemoryStore,

		// Durable key/value with versions. Player data, inventories, anything
		// that has to outlive the process.
		DataStore,

		// Moving a player and a payload to a named world. The only operation
		// that names a world, and it carries data rather than an entity.
		Teleport,

		// A payload sent to a named channel on a named world, with nobody
		// attached.
		//
		// **A channel is `(destination world, channel name)` and both halves are
		// required.** The receiving world *opens* a channel - `Subscribe` with
		// the channel in `Key` - and a sender addresses it with `Send`, `Key`
		// holding the channel and `Target` the destination world. A send naming a
		// channel the destination has not opened is `NoSuchChannel` rather than a
		// delivery, which is what keeps two subsystems talking between the same
		// pair of worlds out of each other's traffic: v0.15's cut had one unnamed
		// pipe per world pair, so every receiver in the destination saw every
		// message the pair exchanged and had to filter by inspecting the payload.
		//
		// **`Teleport` without the player, and it is a fifth kind rather than a
		// flag on the fourth.** The two look alike on the wire - both name a
		// destination and carry bytes - and they mean entirely different things
		// to whoever receives them: a teleport is a person arriving, and the
		// receiving world builds a `Player` and a character out of it. A channel
		// message is a message. Overloading `Teleport` would have meant every
		// receiver guessing which it had, and the one guess that matters is the
		// one `AdmitArrival` makes, which would try to construct a player from a
		// chat line.
		//
		// **The other route out of a world is `Messaging`, and it is a different
		// shape rather than a worse one.** A topic is a fan-out: a publisher does
		// not know or care who is listening, which is right for "the boss died"
		// and wrong for "world B, here is the score you asked me for". This
		// addresses one world by name and delivers to it alone.
		//
		// **Appended, because these are wire ordinals** - rule 4. A kind
		// inserted above this line renumbers `Teleport` for every process that
		// has not been rebuilt, and a teleport would arrive as something else.
		//
		// @since v0.15
		Channel,
	};

	// What an operation asks a bus to do.
	//
	// One enum across all four buses rather than one per bus, because the
	// envelope is one type and a bus-specific operation type would mean a
	// variant in the thing that has to be trivially serialisable.
	//
	// @since v0.2
	enum class BusOperation : uint8_t {
		// Messaging, and `Channel`'s open and close.
		//
		// **A channel is opened with `Subscribe` rather than with an operation of
		// its own**, because it is the same act one bus along: a world declaring
		// which named things it wants delivered. Two more ordinals would have
		// meant two more rows in every switch to say what `Subscribe` already
		// says, and the reader tells them apart by `BusKind` - which it has to
		// read anyway.
		Publish,
		Subscribe,
		Unsubscribe,

		// MemoryStore and DataStore.
		Get,
		Set,
		Remove,

		// MemoryStore queues.
		Push,
		Pop,

		// DataStore read-modify-write. Carries the version the caller last saw;
		// the bus refuses if it has moved since.
		Update,

		// Teleport.
		Send,
	};

	// How an operation turned out.
	//
	// **Every way a cross-world send can fail is one of these, and none of them
	// is a silent drop.** `Postbox::SendTo` always asks for a reply, so a status
	// always has a carrier back to the sender - which is what makes the semantics
	// below something a script can branch on rather than something it has to
	// infer from a message that never arrived:
	//
	// | Case | Answer |
	// |---|---|
	// | Delivered | `Ok` |
	// | The world spent its per-tick allowance | no ticket at all, at the call |
	// | No world of that name | `NoSuchWorld` |
	// | The world exists but has not opened that channel | `NoSuchChannel` |
	// | The world is `Faulted` and being restored | `WorldNotReady` |
	// | The destination already holds a barrier's worth | `Overflow` |
	//
	// **An open is answered the same way**, and it is the one operation on this
	// bus that a world asks for *itself*: a world already holding
	// `UniverseSettings::ChannelsPerWorld` channels is told `TooManyChannels`
	// rather than being given another one.
	//
	// **Delivery is at-most-once and the engine never retries.** A refusal is
	// reported once and the payload is dropped; a sender that wants the message
	// to land sends it again, because only the sender knows whether re-sending is
	// correct - a retry inside the bus would duplicate an operation whose second
	// application is not free.
	//
	// @since v0.2
	enum class BusStatus : uint8_t {
		// It worked. A payload is present when the operation returns one.
		Ok,

		// The key or topic holds nothing.
		NotFound,

		// An Update whose version had moved on. The caller re-reads and retries
		// - which is the whole reason a version is carried.
		Conflict,

		// The world has spent its allowance for this bus this tick. Roblox has
		// request budgets because they turned out to be necessary; there is no
		// reason to rediscover that.
		OverBudget,

		// A Teleport or a Channel send naming a world that does not exist.
		NoSuchWorld,

		// The operation is not one this bus performs.
		Unsupported,

		// A Channel send naming a channel the destination has not opened.
		//
		// **Told apart from `NoSuchWorld` because the two are fixed
		// differently.** A wrong world name is a sender holding a stale name; a
		// closed channel is a receiver that has not started listening yet, and a
		// sender retrying on the next tick is right in the second case and wrong
		// in the first.
		//
		// **Appended - these are wire ordinals**, rule 4. A status inserted above
		// this line renumbers `Unsupported` for every process that has not been
		// rebuilt.
		//
		// @since v0.17
		NoSuchChannel,

		// The destination already holds `UniverseSettings::ChannelQueueLimit`
		// channel deliveries for this barrier.
		//
		// **The bound made visible, which is the whole reason it is a status.**
		// A queue between two worlds that nothing bounds is a memory leak with
		// extra steps, and one that is bounded silently is a game that works
		// until the day it is busy. See `UniverseSettings::ChannelQueueLimit`.
		//
		// @since v0.17
		Overflow,

		// The destination world exists but cannot take delivery yet.
		//
		// `Faulted` today: the supervisor is restoring that world from its
		// snapshot, so anything queued for it now is about to be thrown away with
		// the rest of its inbox. A **suspended** world is deliberately not this -
		// its inbox is what wakes it, which is `LifecycleInputs::InboxWaiting`.
		//
		// @since v0.17
		WorldNotReady,

		// An open on a world that already holds
		// `UniverseSettings::ChannelsPerWorld` channels.
		//
		// **Not `OverBudget`, which is the nearest and means something else.**
		// A budget is a *rate* - spent this tick, back next tick - so a world
		// told it is over budget waits a tick and tries again, and a world that
		// did that with this refusal would loop for ever. This is a total, and
		// the only thing that clears it is the world closing a channel it holds.
		//
		// **Appended - these are wire ordinals**, rule 4.
		//
		// @since v0.17
		TooManyChannels,
	};

	// A handle for correlating a reply with the request that asked for it.
	//
	// Per world and monotonic, so a system can hold one across ticks and match
	// it against whatever arrives. Zero means fire-and-forget: a publish wants
	// no answer, and inventing one for it would put traffic on the bus that
	// nobody reads.
	//
	// @since v0.2
	struct Ticket {
		// The value meaning "no reply expected".
		static constexpr uint64_t NONE = 0;

		// The per-world sequence number.
		uint64_t Value = NONE;

		// Reports whether a reply is expected for this ticket.
		//
		// @return `true` when the operation asked for an answer.
		constexpr bool Expected() const {
			return Value != NONE;
		}

		// Compares tickets.
		//
		// @param other The ticket to compare.
		// @return `true` when both name the same request.
		constexpr bool operator==(const Ticket &other) const {
			return Value == other.Value;
		}
	};

	// One request from a world to a bus.
	//
	// Bytes rather than a typed payload, because this is what crosses a process
	// boundary and a type that could hold a pointer is a type that would.
	//
	// @since v0.2
	struct Envelope {
		// Which bus.
		BusKind Bus = BusKind::Messaging;

		// What to do.
		BusOperation Operation = BusOperation::Publish;

		// The topic, key, channel, or destination world name.
		//
		// For `BusKind::Channel` this is always the *channel*, on all three of
		// its operations, and `Target` carries the world. Reusing one field for
		// both would have made "which name is this" depend on the operation, and
		// the operation is exactly what a reader gets wrong.
		core::Name Key;

		// The world that sent it. Half of the ordering key, and what a
		// subscriber sees as the sender.
		core::Name From;

		// The destination world, for `BusKind::Channel`'s `Send`.
		//
		// Ignored by every other bus and operation, the way `Version` is ignored
		// by everything but an `Update`.
		//
		// @since v0.17
		core::Name Target;

		// Per-sender and monotonic. The other half of the ordering key, and the
		// reason delivery is a merge rather than a sort.
		uint64_t Sequence = 0;

		// The reply correlation, or NONE.
		Ticket Reply;

		// The version an Update was based on. Ignored by every other operation.
		uint64_t Version = 0;

		// The value, for the operations that carry one.
		std::vector<std::byte> Payload;
	};

	// One thing arriving in a world's inbox.
	//
	// Both a reply to something this world asked for and a message somebody
	// else published, because a world reads one queue rather than two and the
	// ticket is what tells them apart.
	//
	// @since v0.2
	struct Delivery {
		// Which bus it came from.
		BusKind Bus = BusKind::Messaging;

		// The topic, key, or channel.
		//
		// **For a channel arrival this is the channel, not the sender** - which
		// is `Messaging`'s shape and was not v0.15's: the first cut had no channel
		// to name, so it put the sender here and the receiver read it twice.
		// `From` is who sent it, on every bus that has a sender.
		core::Name Key;

		// The world that sent it, or an invalid Name when the bus itself is
		// answering.
		core::Name From;

		// The request this answers, or NONE for a published message.
		Ticket Reply;

		// How the operation turned out. Always Ok for a published message.
		BusStatus Status = BusStatus::Ok;

		// The version the value carries, for DataStore reads.
		uint64_t Version = 0;

		// The value, when there is one.
		std::vector<std::byte> Payload;
	};

	// Writes an envelope.
	//
	// One encoder, because there are three readers of it - a recording, a
	// snapshot of pending traffic, and the link to a supervised host - and
	// three encoders would agree until the day one of them did not. Names are
	// written as text, so a frame crosses a process boundary where the
	// interned ids mean nothing.
	//
	// @param writer   The writer to append to.
	// @param envelope The envelope to write.
	// @since v0.2
	void WriteEnvelope(core::ByteWriter &writer, const Envelope &envelope);

	// Reads an envelope.
	//
	// @param reader The reader to consume.
	// @return The envelope. Check the reader for failure rather than the
	//         result: a truncated stream produces a default-shaped envelope
	//         that is indistinguishable from a real one.
	// @since v0.2
	Envelope ReadEnvelope(core::ByteReader &reader);

	// Writes a delivery.
	//
	// @param writer   The writer to append to.
	// @param delivery The delivery to write.
	// @since v0.2
	void WriteDelivery(core::ByteWriter &writer, const Delivery &delivery);

	// Reads a delivery.
	//
	// @param reader The reader to consume.
	// @return The delivery. Check the reader for failure rather than the
	//         result.
	// @since v0.2
	Delivery ReadDelivery(core::ByteReader &reader);

	// Returns a stable, human-readable name for a bus.
	//
	// @param bus The bus to name.
	// @return A view valid for the lifetime of the process.
	const char *Describe(BusKind bus);

	// Returns a stable, human-readable name for an operation.
	//
	// @param operation The operation to name.
	// @return A view valid for the lifetime of the process.
	const char *Describe(BusOperation operation);

	// Returns a stable, human-readable name for a status.
	//
	// @param status The status to name.
	// @return A view valid for the lifetime of the process.
	const char *Describe(BusStatus status);
}
