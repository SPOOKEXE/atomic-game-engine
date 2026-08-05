#pragma once

// The server's half: what each client is told, and when.
//
// **A client is admitted, snapshotted, then streamed.** It joins knowing
// nothing, receives one full snapshot of what it may see, and from then on
// receives only what changed. There is no third mode — a client that falls too
// far behind is re-snapshotted rather than repaired, because a repair path is a
// second way to reach a correct world and only one of the two gets tested.
//
// **This produces messages; it does not send them.** Nothing here touches a
// socket, a `net::Link` or a clock. That is what lets the whole of it be driven
// by a test in microseconds, and it is the same argument `net/AGENTS.md` makes
// for `Link` doing no I/O.
//
// **Interest is a predicate, and absence is not destruction.** A client that
// stops being able to see an entity is told to *forget* it, never that it was
// destroyed — a client that conflated the two would delete something still
// there and then be wrong about it the moment it came back into view.
//
// **Interest says what a client may see; priority says what fits.** When a
// tick's delta is larger than one client's per-tick budget, the order the
// entities go in is decided here — by a score the game supplies and a rotation
// this class owns — rather than by where a component happens to sit in a
// vector. See `SetPriority` and `AuthoritySettings::StarvationTicks`.
//
// **The cap is per client, and that is a decision rather than an omission.**
// The budget being spent is `net::Link`'s and there is one link per connection,
// so a per-server cap would have to be divided among clients before anything
// could enforce it — and that division is a per-client cap with an extra step.
// Interest is per client too, so two clients are owed different worlds and a
// shared ordering would spend one client's bandwidth on another's entities. A
// machine-wide uplink is still a real limit; it is `MaximumClients` multiplied
// by this budget, which is a deployment decision and not an ordering one.
//
// @tier L12 · shared

#include <engine/core/Name.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/replication/Protocol.hpp>

#include <cstdint>
#include <functional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace engine::replication {

	// One connected client, from the server's point of view.
	//
	// An index and a generation, for the reason every handle in this engine is:
	// a slot is reused and a stale handle must stop resolving rather than start
	// naming somebody else's client.
	//
	// @since v0.3
	struct ClientId {
		// The value no client is given.
		static constexpr uint32_t INVALID = 0xFFFFFFFFu;

		// The slot.
		uint32_t Index = INVALID;

		// The generation the handle was issued at.
		uint32_t Generation = 0;

		// Whether this handle names a client.
		//
		// @return `true` when it came from `Admit`.
		bool IsValid() const {
			return Index != INVALID;
		}

		// Compares handles.
		//
		// @param other The handle to compare.
		// @return `true` when both name the same client.
		bool operator==(const ClientId &other) const {
			return Index == other.Index && Generation == other.Generation;
		}
	};

	// How the server streams.
	//
	// @since v0.3
	struct AuthoritySettings {
		// The most snapshot bytes put in one chunk.
		//
		// Sized under `net::Packet::MAXIMUM_MESSAGE_BYTES` — the payload limit
		// less the sixteen-byte tag every message is sealed with — and with room
		// for this layer's own header on top. A chunk that did not fit would be
		// refused by the transport, and a snapshot that never finished arriving
		// is a client that never joins.
		//
		// **Capped at construction, loudly.** Anything larger cannot be sent at
		// all, and a message that can never be sent looks exactly like a link
		// that is momentarily busy.
		size_t ChunkBytes = 1024;

		// How many chunks of a joining client's snapshot go out per tick.
		//
		// **Not all of them.** A ten-megabyte world sent in one tick is ten
		// megabytes into a link sized for a few kilobytes, which drops most of
		// it and starves every other client while it does. Spreading the join
		// costs the joiner a moment and costs everybody else nothing.
		size_t ChunksPerTick = 8;

		// Ticks a client may go without acknowledging before it is re-snapshot.
		//
		// A client this far behind cannot be caught up by deltas it never got.
		uint64_t ResnapshotAfterTicks = 120;

		// The most delta messages one client is sent per tick.
		//
		// **Per client, not per server** — the note at the top of this file
		// says why. What does not fit is held over rather than dropped, and
		// `Statistics::Deferred` says how much.
		//
		// **Values only.** Structural messages are not counted against it: an
		// entity is created once and destroyed once, so holding one over would
		// be holding it over against a link that will be no less busy next
		// tick, and the link's own refusal already sends it back through
		// `Unsent`.
		//
		// Keep it under `net::LinkSettings::PacketsPerTick` with room left for
		// `ChunksPerTick` and for reliable retransmissions, because all three
		// spend the same budget. `Listener` warns at construction when the
		// numbers do not add up; past that point `Link::Reserve` refuses the
		// excess and `net::ConnectionStats::SendsOverBudget` is what says so.
		size_t MessagesPerTick = 32;

		// The most delta bytes one client is sent per tick.
		//
		// The second half of `net`'s pair of budgets, for the same reason it
		// keeps two: a message count bounds per-packet overhead and a byte
		// count bounds bandwidth, and neither implies the other.
		size_t BytesPerTick = 32 * 1024;

		// Ticks one entity's component value may wait before it outranks every
		// score there is.
		//
		// **This is what makes "nothing starves" a property rather than a
		// hope.** A score summed with a staleness term still lets a
		// permanently high score hold a low one off the wire forever, and the
		// symptom is one component looking broken rather than one budget
		// looking small. A value that has waited this long jumps the whole
		// queue instead, so the longest anything waits is this plus the ticks
		// it takes to drain the backlog that was already waiting.
		uint64_t StarvationTicks = 30;
	};

	// How the authority notices that a component's value moved.
	//
	// **Two detectors, because the writers are not alike.** A position is
	// written every tick by a system that goes through `Store::Set`, and a
	// colour is written once by a script and then never again. The cheap answer
	// for the first is the expensive answer for the second, and the reverse.
	//
	// @since v0.7
	enum class ChangeDetection : uint8_t {
		// The dirty bits, through `Store::EachChangedRuns`.
		//
		// **Costs nothing to ask and requires `Store::Observe`.** A component
		// nobody observes has no bits, so this detector finds nothing for it —
		// silently, and for ever. That is the right trade for something a
		// system writes every tick: the bit was set by the write that already
		// happened, and the walk hands over adjacent changed rows as a block so
		// a delta is a memcpy per run.
		//
		// **What it cannot see** is a write through a raw column pointer.
		// `EachBatch` and `EachBatchParallel` set no bit — deliberately, since
		// checking per row is the cost those paths exist to avoid — so a system
		// that writes in bulk is invisible to this. `ecs/ChangeChannel.hpp`
		// says so in its own words.
		Observed,

		// A hash of the value, compared against the last one seen.
		//
		// **Indifferent to how the write happened**, which is the whole point:
		// a script assignment, a bulk column write and a snapshot restore all
		// change the bytes, and none of them has to remember to announce it.
		// That closes the two holes `Observed` leaves — the unobserved
		// component and the batch path — at the cost of reading every value of
		// every entity once per `Publish`.
		//
		// **For what changes rarely and matters when it does.** `Bounds` and
		// `Visual` are the case this was added for: nothing writes them per
		// tick, so observing them buys a dirty column paid for every tick and
		// read never — and *not* observing them meant a part recoloured by a
		// script stayed its old colour on every client until it happened to be
		// re-snapshotted. They crossed once, in the join snapshot, and never
		// again.
		//
		// Not for a position. A transform moves every tick, so the hash differs
		// every tick, and hashing it is a pass over the world to learn what the
		// dirty bits already knew for free.
		Signature,
	};

	// What the server owes each client this tick.
	//
	// @since v0.3
	class Authority {
	  public:
		// Creates a server-side replicator.
		//
		// @param settings How to stream.
		explicit Authority(const AuthoritySettings &settings = {});

		// Declares a component as replicated, by name.
		//
		// **Opt in, not opt out.** A world holds components no client has any
		// business receiving — a server-side AI's scratch state, a pending bus
		// request — and a default of "everything" makes leaking one the
		// consequence of forgetting rather than of deciding.
		//
		// @param component The component's registered name.
		// @param detection How a change to it is noticed. See `ChangeDetection`
		//        — the default suits something a system writes every tick, and
		//        a component nobody observes needs `Signature` or it never
		//        deltas at all.
		void Replicate(core::Name component, ChangeDetection detection = ChangeDetection::Observed);

		// How a replicated component's changes are noticed.
		//
		// @param component The component's registered name.
		// @return Its detector, or `Observed` for one that is not replicated.
		ChangeDetection DetectionFor(core::Name component) const;

		// Whether a component is replicated.
		//
		// @param component The component's registered name.
		// @return `true` when it is sent.
		bool Replicated(core::Name component) const;

		// Decides what a client may see.
		//
		// Called once per client per tick for every entity that has something
		// to send, so it must be cheap. An empty predicate means every such
		// entity is visible to everybody, which is the right answer while
		// worlds are small and the wrong one to leave in place silently —
		// `Statistics::Visible` is what says which you have.
		//
		// **An entity carrying no replicated component is never offered here.**
		// There is nothing to decide about it: it cannot be sent, and it must
		// not appear in a snapshot as an empty row, because a row with no data
		// still leaks how many entities the world holds.
		//
		// @param predicate Called as `predicate(ClientId, ecs::Entity)`.
		void SetInterest(std::function<bool(ClientId, ecs::Entity)> predicate);

		// Scores which entities a client is sent first when not all of them
		// fit.
		//
		// Higher goes earlier. Only the order changes: `Replicate` and
		// `SetInterest` still decide what may be sent at all, and a score
		// cannot put an entity on the wire that interest excluded.
		//
		// **The game supplies this because the engine cannot.** Distance to
		// the client's viewpoint and whether the client is looking at
		// something are the two scores worth having and both are read off a
		// transform, which this module deliberately does not know about — it
		// carries named components and has no idea which of them is a
		// position. The same argument `SetInterest` is built on.
		//
		// Called once per entity per client on a tick where the budget is
		// short, so it must be cheap. Non-finite results are read as zero: a
		// NaN in a comparator is not a weak ordering and `std::sort` on one is
		// undefined rather than merely wrong.
		//
		// **A score alone cannot starve anything.** A value that has waited
		// `AuthoritySettings::StarvationTicks` outranks every score, which is
		// what bounds the wait.
		//
		// @param score Called as `score(ClientId, ecs::Entity)`. Empty scores
		//        everything the same, which leaves the rotation in sole charge
		//        and is a plain round robin.
		void SetPriority(std::function<float(ClientId, ecs::Entity)> score);

		// Admits a client. It will be sent a full snapshot before any delta.
		//
		// @return Its handle.
		ClientId Admit();

		// Drops a client and forgets what it knew.
		//
		// @param client The client to drop.
		// @return `false` for a handle that never named one, or names one that
		//         has already gone.
		bool Remove(ClientId client);

		// The number of admitted clients.
		//
		// @return The count.
		size_t Count() const;

		// Whether a client is still admitted.
		//
		// @param client The handle to test.
		// @return `true` when it resolves.
		bool Holds(ClientId client) const;

		// Builds this tick's messages for every client.
		//
		// Call once per tick, after the world has ticked and before its change
		// bits are cleared — the bits are the delta source, and clearing them
		// first is how a tick's worth of movement goes missing.
		//
		// @param store The authoritative world.
		// @param tick  The tick just completed.
		void Publish(ecs::Store &store, uint64_t tick);

		// What to send one client, each entry a whole message.
		//
		// Valid until the next `Publish`. **Send them in order and hand back
		// what the transport refused** — see `Unsent`.
		//
		// @param client The client to ask about.
		// @return The messages, in the order they should go.
		std::span<const std::vector<std::byte>> Outgoing(ClientId client) const;

		// Hands back a message from `Outgoing` that the transport would not
		// take.
		//
		// **A refusal is ordinary backpressure for some of these messages and
		// a permanent hole for the rest, and the two are indistinguishable at
		// the call site — which is the whole reason this exists.** A component
		// value carried by a refused delta is offered again next tick by the
		// unconfirmed set, so the value itself needs nothing undone — but the
		// *tick* does: a refused part is a part that never arrives, so the
		// client will never acknowledge that tick, so it must stop counting as
		// a tick that streamed. Without that, a link whose packet budget is
		// below `MessagesPerTick` re-snapshots its client every
		// `ResnapshotAfterTicks` for ever. A snapshot chunk is
		// not: the cursor moved when the chunk was *built*, so a refused chunk
		// is a gap in a stream the receiver waits on forever, and the symptom
		// is a client that streams 184 chunks of 192 and then refuses every
		// delta that follows as stale. A `Structure` message is the same shape:
		// a creation, a destruction and a forget are each said exactly once and
		// moved the known set when they were built.
		//
		// **This covers a refusal and not a loss.** A structural message the
		// transport accepted and the network then dropped is covered by the
		// reliable channel `Session` puts it on, which is where D00011 was
		// closed — see `ChannelFor`.
		//
		// Safe to call for any index, including one that needs nothing undone.
		//
		// @param client The client the message was for.
		// @param index  Its position in `Outgoing`.
		void Unsent(ClientId client, size_t index);

		// Takes a message from a client.
		//
		// **Every field of it is hostile.** A malformed message is refused and
		// counted; it is never partly applied.
		//
		// @param client  Who sent it.
		// @param message The bytes.
		// @return `false` when it was refused.
		bool Receive(ClientId client, std::span<const std::byte> message);

		// The inputs a client has sent and the game has not yet consumed.
		//
		// @param client The client to ask about.
		// @return The inputs, oldest first.
		std::span<const Input> Inputs(ClientId client) const;

		// Drops the inputs a client sent, once the game has applied them.
		//
		// @param client The client to clear.
		void ClearInputs(ClientId client);

		// What one client's stream is doing.
		//
		// @since v0.3
		struct ClientStatus {
			// Whether the snapshot has finished going out.
			bool Streaming = false;

			// Snapshot bytes still to send.
			size_t SnapshotRemaining = 0;

			// The last tick this client said it applied.
			uint64_t Applied = 0;

			// Entities it currently knows about.
			size_t Known = 0;
		};

		// One client's stream.
		//
		// @param client The client to ask about.
		// @return Its status, or an empty record for an unknown handle.
		ClientStatus StatusOf(ClientId client) const;

		// What the last `Publish` did.
		//
		// @since v0.3
		struct Statistics {
			// Messages built, across every client.
			size_t Messages = 0;

			// Bytes built, across every client.
			size_t Bytes = 0;

			// Entities visible, summed across clients. Compared against the
			// world's own count, this is what says whether interest is
			// filtering anything at all.
			//
			// Counts what a client is actually told about, so an entity with no
			// replicated component is not in it — see `Bearing`.
			size_t Visible = 0;

			// Clients restarted from a snapshot because they fell too far
			// behind. Not zero is a bandwidth problem, not a bug.
			size_t Resnapshots = 0;

			// Messages refused as malformed since the last reset.
			size_t Refused = 0;

			// Entity values the per-client cap held over to a later tick,
			// summed across clients.
			//
			// **This is "the budget was exceeded", and it is deliberately not
			// the same number as `net::ConnectionStats::SendsOverBudget`.**
			// That one counts what the *link* refused, which after this cap
			// exists means something went wrong — a retransmission storm, or a
			// cap set above the link's own budget. This one counts what the
			// authority chose to hold back, which is the ordinary answer to a
			// world larger than a link and is the number to read before
			// concluding that a component has stopped replicating.
			size_t Deferred = 0;

			// Ticks the longest-waiting held-over value has waited.
			//
			// Bounded by `AuthoritySettings::StarvationTicks` plus the ticks
			// it takes to drain the backlog ahead of it. Growing without limit
			// is the rotation not working, and there is no other symptom.
			uint64_t Stalest = 0;
		};

		// What the last `Publish` did.
		//
		// @return The statistics.
		const Statistics &Stats() const {
			return Stats_;
		}

	  private:
		// The index that names no position, for a record that has none.
		static constexpr size_t NOWHERE = static_cast<size_t>(-1);

		// One entity's value for one component, from the sender's side.
		//
		// Three ticks rather than one, because the three questions they answer
		// pulled apart the moment a value could be built and then held over.
		struct Outstanding {
			// The tick this value last actually went out on, or zero while it
			// has changed and not been sent.
			//
			// **An acknowledgement may only retire a non-zero one.** Retiring
			// on a tick the value never left confirms something the client was
			// never told, and the entity is then wrong until a re-snapshot
			// that nothing will ask for.
			uint64_t SentAt = 0;

			// The tick it started waiting, or zero while nothing is waiting.
			// What the rotation measures — a value the budget held over keeps
			// this, so its wait grows until it outranks everything.
			uint64_t WaitingSince = 0;

			// The tick it was last put into a delta being built, so the
			// recovery walk does not add the same entity twice.
			uint64_t ConsideredAt = 0;
		};

		// One entity leaving or entering a client's known set.
		struct Edit {
			uint64_t Entity = 0;

			// Whether undoing this means putting the entity back in the known
			// set. A destroy and a forget both took one out; a creation put
			// one in.
			bool Restore = false;
		};

		// What one outgoing message moved, so a refusal can move it back.
		//
		// **Only what cannot rebuild itself is here.** A component value in a
		// refused delta is offered again next tick by the unconfirmed set.
		struct Carried {
			// Where in the snapshot a chunk started, or `NOWHERE`.
			size_t SnapshotOffset = NOWHERE;

			// The half-open range of `Client::Edits` this message announced.
			uint32_t First = 0;
			uint32_t Count = 0;

			// Whether this message is one of a tick's delta parts.
			//
			// Stated rather than inferred from the two fields above being
			// empty. They are empty for a delta today and the day a fourth kind
			// of message is added with nothing to undo is the day that
			// inference starts naming it too.
			bool Values = false;
		};

		struct Client {
			uint32_t Generation = 0;
			bool Live = false;

			// The snapshot being streamed, and how much of it has gone.
			std::vector<std::byte> Snapshot;
			size_t Sent = 0;
			uint64_t SnapshotTick = 0;

			// Entities this client has been told about. What `Structure`'s three
			// lists are all differences against.
			std::unordered_set<uint64_t> Known;

			uint64_t Applied = 0;

			// The tick a delta's *values* last actually went out on.
			//
			// Paired with `Applied` to say whether a client is behind or merely
			// watching a world where nothing is happening. Without it the two
			// are indistinguishable and the second is re-snapshotted for ever.
			// A structural message does not move it, because `Replica` does not
			// move `Applied` for one either.
			uint64_t Streamed = 0;

			// What `Streamed` was before this tick moved it.
			//
			// **A tick the transport cut short is not a tick that streamed**,
			// and this is what lets `Unsent` say so. A client cannot
			// acknowledge a tick it holds only some of the parts of, so
			// counting one against its silence is measuring it against
			// something it was never given the chance to answer — and on a link
			// whose budget is below `MessagesPerTick`, that is every tick, for
			// ever. The same argument as the quiet world and the held-back
			// budget, one layer down: this refusal is the *link's*.
			uint64_t StreamedBefore = 0;

			std::vector<Input> Pending;
			std::vector<std::vector<std::byte>> Outgoing;

			// What has been sent and not confirmed, per replicated component:
			// entity to the tick its value last went out on, or zero for one
			// that has changed and not yet been sent.
			//
			// **This is the baseline.** A delta built from the dirty bits alone
			// describes one tick, so an entity that moved in a tick whose
			// datagram was lost and then went still is wrong on that client
			// until something re-snapshots it — and nothing will, because the
			// client is acknowledging happily and is not behind. Carrying the
			// unconfirmed entries forward makes the stream self-healing: a value
			// keeps being resent until the client says it applied a tick at or
			// after the one it went out on.
			//
			// Bounded by the visible world rather than by time, because an entry
			// is replaced rather than appended when the same entity changes
			// again, and the whole map is dropped when a client is
			// re-snapshotted.
			std::vector<std::unordered_map<uint64_t, Outstanding>> Unconfirmed;

			// What each entry of `Outgoing` moved when it was built, so that
			// `Unsent` can move it back. Parallel to `Outgoing`.
			std::vector<Carried> Carried_;

			// The known-set edits the messages above are carrying, referenced
			// by `Carried::First` and `Carried::Count`. One flat list rather
			// than a vector per message, because a per-message vector is an
			// allocation per message per tick for something almost always
			// empty.
			std::vector<Edit> Edits;
		};

		// One entity's value for one component, built and waiting for a place
		// in a message.
		//
		// **Materialised before the order is decided**, so the priority pass
		// permutes rows that are already in `Delta::Components` rather than
		// reading the world a second time. That is also what keeps the
		// unpressured path free: the values were copied run by run exactly as
		// `EachChangedRuns` handed them over, and if everything fits, nothing
		// looks at these again.
		struct Candidate {
			// Which entry of `Delta::Components` holds this value.
			uint32_t Entry = 0;

			// Which row of that entry.
			uint32_t Row = 0;

			ecs::Entity Entity;

			// The tick this value started waiting.
			uint64_t WaitingSince = 0;

			// What `SetPriority` said, or zero.
			float Hint = 0.0f;
		};

		// How much of a delta a packing pass got onto the wire.
		struct Placement {
			// Leading entries of `Order` that went. Structure is not here: it
			// is not ranked and not budgeted, and goes whole in its own
			// messages.
			size_t Values = 0;
		};

		// One `Signature`-detected component's memory of what it last saw.
		//
		// **Server-wide and not per client**, because the question it answers —
		// "did this value move" — has one answer for everybody. What each
		// client has actually *received* is a different question and is
		// `Client::Unconfirmed`'s, which is why a value that changed once is
		// resent until it is acknowledged rather than only on the tick it moved.
		struct Signature {
			// Entity id to the hash of its value as of the last `Publish`.
			//
			// Bounded by the world rather than by time: an entry is replaced
			// when the value changes and swept when the component or the entity
			// goes away.
			std::unordered_map<uint64_t, uint64_t> Hashes;

			// Entities whose hash differed this `Publish`, sorted.
			//
			// Sorted because `Bearing` is walked in order to build it, and two
			// runs of one server have to produce the same bytes — the same
			// reason `Recovering` is sorted before it is walked.
			std::vector<uint64_t> Changed;
		};

		Client *Reach(ClientId client);
		const Client *Reach(ClientId client) const;
		void Survey(ecs::Store &store);
		void Resign(ecs::Store &store);
		void BeginSnapshot(Client &client, ecs::Store &store, uint64_t tick);
		void BuildComponents(ecs::Store &store, Client &client, Delta &delta, uint64_t tick);
		void Prioritise(ClientId client, uint64_t tick);
		Placement Pack(Client &client, const Delta &delta, size_t messageLimit);
		void Record(Client &client, const Placement &placed, uint64_t tick);
		void EmitStructure(Client &client, const Structure &structure);

		AuthoritySettings Settings_;
		std::function<bool(ClientId, ecs::Entity)> Interest;
		std::function<float(ClientId, ecs::Entity)> Priority;
		std::vector<core::Name> Components;

		// How each entry of `Components` notices a change. Parallel to it.
		std::vector<ChangeDetection> Detection;

		// The memory a `Signature` component keeps. Parallel to `Components`,
		// and left empty for a slot that uses the dirty bits — a vector of
		// unused maps costs nothing and keeps one index meaningful everywhere.
		std::vector<Signature> Signatures;

		std::vector<Client> Clients;
		Statistics Stats_;

		// Reused between ticks so a server streaming every frame stops
		// allocating.
		std::vector<ecs::Entity> Visible;
		std::vector<Candidate> Candidates;

		// Entities carrying at least one replicated component, sorted, rebuilt
		// once per `Publish`.
		//
		// **An entity with none of them is not sent at all, not even as an
		// empty row.** A bare row in a join snapshot carries no data and still
		// tells a client how many entities the server is holding, which is a
		// count of a world it was told it may not see. It is one list rather
		// than one per client because the answer does not depend on the client
		// — interest does, this does not.
		std::vector<uint64_t> Bearing;

		// The replicated components, resolved to ids once per `Publish` instead
		// of once per entity.
		std::vector<ecs::ComponentId> Resolved;

		// Indices into `Candidates`, in the order they should go out. The one
		// place the priority decision lives.
		std::vector<uint32_t> Order;

		// Per entry of `Delta::Components`: the bytes one entity's value
		// occupies, and which `Components` slot it came from.
		std::vector<size_t> Strides;
		std::vector<size_t> SourceSlot;

		// Which entry of the message being packed is open for each entry of
		// the delta, so a component's name is written once per message rather
		// than once per entity.
		std::vector<size_t> OpenEntry;

		// Entities the recovery walk is about to consider, sorted — an
		// unordered map is walked in whatever order it likes, and two runs of
		// one server have to produce the same bytes.
		std::vector<uint64_t> Recovering;

		// Entities this client has just been told about, which are owed their
		// current value for every replicated component rather than only what the
		// dirty bits say moved. Rebuilt per client per `Publish`.
		std::vector<uint64_t> Appearing;
	};
}
