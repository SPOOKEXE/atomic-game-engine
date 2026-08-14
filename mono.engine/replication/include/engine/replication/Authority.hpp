#pragma once

// @tier L12 · shared

#include <engine/core/Name.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/replication/Audit.hpp>
#include <engine/replication/Protocol.hpp>
#include <engine/replication/Submission.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace engine::replication {

	// One connected client, from the server's point of view.
	//
	// @since v0.3
	struct ClientId {
		// The index no client ever has, so a default handle is not client zero.
		static constexpr uint32_t INVALID = 0xFFFFFFFFu;

		// Which slot this client occupies.
		uint32_t Index = INVALID;

		// How many clients have occupied that slot before it.
		//
		// **The half that makes a stale handle safe.** Slots are reused the
		// moment a client leaves, so an index alone would let a message meant
		// for somebody who disconnected be delivered to whoever arrived next —
		// `ecs::Entity` carries a generation for the same reason.
		uint32_t Generation = 0;

		// @return `true` when it came from `Admit`.
		bool IsValid() const {
			return Index != INVALID;
		}

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
		// How much of a join snapshot goes into one message.
		//
		// Under the datagram limit on purpose: a chunk that cannot fit is
		// refused by the link every tick, and a refusal is indistinguishable
		// from ordinary backpressure.
		size_t ChunkBytes = 1024;

		// How many of those go out per tick, which is what spreads a join across
		// ticks instead of stalling one.
		size_t ChunksPerTick = 8;

		// How far behind a client may fall before it is re-snapshotted instead
		// of repaired.
		//
		// **Measured against how far behind the client is, not against the tick
		// number** — the version that compared tick numbers re-snapshotted a
		// client in perfect agreement with a quiet world every 120 ticks for
		// ever.
		uint64_t ResnapshotAfterTicks = 120;

		// The per-client message budget for one tick.
		size_t MessagesPerTick = 32;

		// The per-client byte budget for one tick.
		//
		// **Per client rather than per server, and the difference is not
		// pedantry**: the budget belongs to a link and there is one link per
		// connection, so a server-wide cap would have to be divided before it
		// could be enforced — and that division *is* a per-client cap.
		size_t BytesPerTick = 32 * 1024;

		// How long a value may wait before it outranks every score.
		//
		// What stops the priority ordering starving anything: the bound on how
		// late a value can be is `StarvationTicks + ceil(n/k)`, which is
		// asserted by a test rather than argued for.
		uint64_t StarvationTicks = 30;

		// The anti-entropy audit over what a client already holds.
		//
		// @since v0.15
		AuditSettings Audit;
	};

	// How the authority notices that a component's value moved.
	//
	// @since v0.7
	enum class ChangeDetection : uint8_t {
		Observed,

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
		// @param detection How changes are detected.
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

		// Stops sending a replicated component's *deltas* for entities carrying
		// a tag, without taking the component off the wire for everyone else.
		//
		// **The filter was per component and needed to be per row.** A character
		// is six parts and one of them moves: `scene::PoseCharacters` derives the
		// other five from the root, on whichever machine draws, precisely so they
		// cannot come apart at speed. Those five transforms crossed every tick
		// anyway and were overwritten the moment they landed — five ten-byte
		// quantised `CFrame`s per character per tick, against roughly ten bytes
		// for the root alone.
		//
		// **Named rather than typed, because the tag belongs to a module this one
		// must not see.** `scene` is L7 and this is L12, so the component that
		// marks a derived row is declared where the thing producing it lives and
		// reaches here as a string — which is rule 4, and the same way
		// `Replicate` already names what it sends.
		//
		// **Deltas only, and the baseline still carries one copy.** That is a
		// property worth having rather than a gap: a client that has just been
		// admitted holds the limb where the server last put it, so the first
		// frame is right before any derivation has run. What stops is the
		// per-tick repeat.
		//
		// Not a wire-format change. A delta already carries only the rows that
		// changed, so a receiver cannot tell an omitted row from an unchanged
		// one — which is why this could be decided without a second consumer to
		// check it against, as `D00115` expected to need.
		//
		// **A tagged entity is also out of the audit entirely.** The two ends
		// are meant to disagree about a derived row, and a hash has no
		// tolerance — see `Audit.hpp`. What that costs is that a character root
		// is never audited, and it moves every tick, so the audit would have
		// passed over it anyway.
		//
		// @param component The replicated component whose rows to filter.
		// @param tag       The component whose presence suppresses them. An
		//                  invalid name clears the filter.
		// @since v0.15
		void SuppressWhenTagged(core::Name component, core::Name tag);

		// Decides what a client may see.
		//
		// **Handed the world, exactly as `SetOwnership` is, and for the same
		// reason.** This runs inside a walk of the store `Stream` was given, so a
		// predicate that had to look a world up would be re-entering the host's
		// world lock from inside a loop that already holds it. It also would not
		// know *which* world: a host with several streams each of them, and a
		// predicate reaching for "the world" would answer about whichever one it
		// happened to name.
		//
		// **No predicate means everything is visible**, which is the opposite of
		// `SetOwnership`'s default and is right for the opposite reason: that one
		// gates writes, where forgetting must fail closed, and this one gates
		// reads of a world a host chose to stream at all. A server that means to
		// hide its `ServerStorage` says so — see `scene::VisibleToClients`.
		//
		// @param predicate Called as `predicate(ClientId, ecs::Entity, const
		//                  ecs::Store &)`.
		void SetInterest(std::function<bool(ClientId, ecs::Entity, const ecs::Store &)> predicate);

		// Scores which entities a client is sent first when not all of them
		// fit.
		//
		// @param score Called as `score(ClientId, ecs::Entity)`. Empty scores
		//        everything the same, which leaves the rotation in sole charge
		//        and is a plain round robin.
		void SetPriority(std::function<float(ClientId, ecs::Entity)> score);

		// Decides which entities a joining client is sent *before* the world.
		//
		// **`SetPriority` orders a stream and this orders a join, and neither
		// can do the other's job.** A join is one `ecs::Store::Save` chunked
		// across ticks in the order the store wrote its archetypes, and a score
		// does not reach inside a byte cursor — so a client's very first view of
		// a world was whatever the storage happened to lay down first, whatever
		// the priority hook said. What this predicate picks out is captured and
		// finished as its own blob before the world's first chunk is built.
		//
		// **Applied as an overlay on the far side, and sent twice.** The blob is
		// a slice of a world rather than the whole of one, so a receiver may not
		// sweep what it fails to mention — and the world blob that follows still
		// carries these entities, because *that* one is the complete picture and
		// the sweep behind it is what retires a client's stale rows. The
		// duplication is the price, and it is the size of what a host puts in
		// front rather than the size of the world.
		//
		// **No `ClientId`, unlike every other hook here.** What goes first is a
		// property of the content — Roblox's `ReplicatedFirst` is a container,
		// not a per-player decision — and interest has already narrowed the set
		// this is asked about to what that client may see.
		//
		// @param predicate Called as `predicate(ecs::Entity, const ecs::Store
		//        &)`, once per visible entity per join. Empty means no preface,
		//        which is one blob and exactly the behaviour that preceded this.
		// @since v0.15
		void SetPreface(std::function<bool(ecs::Entity, const ecs::Store &)> predicate);

		// Decides what to do with a client's identity claim.
		//
		// @param check Called as `check(ClientId, const Identify &)`.
		// @since v0.9
		void SetIdentityCheck(std::function<bool(ClientId, const Identify &)> check);

		// Decides what a client may *write*, which is the only thing standing
		// between a delta and the world.
		//
		// ## The policy, and it is a decision rather than a default
		//
		// **Authorisation is ownership, checked per entity per message.** A
		// client's delta names entities; every one this predicate refuses is
		// dropped from it and counted in `Statistics::Unowned`, and a delta
		// left with nothing is refused whole. There is no partial trust and no
		// benefit of the doubt: `AGENTS.md` in this directory says every field
		// of an inbound message is hostile, and an entity id is a field.
		//
		// **Plausibility is deliberately not checked here, and that is the
		// decision.** This module could compare a submitted position against
		// the last one and refuse a jump — and it would be wrong to, because it
		// does not know whether this game has teleports, launch pads, vehicles
		// or a grappling hook. A speed limit invented at this layer is a limit
		// every game has to work around and none can tune. So the engine
		// enforces *who*, the host enforces *what*, and a game that needs
		// movement validation writes it where the movement rules already live.
		// `Unowned` is the number that says somebody is trying.
		//
		// **No predicate means nothing may be written.** An authority that has
		// not been told who owns what refuses every inbound delta, because the
		// alternative — accepting them until somebody remembers to restrict it
		// — makes the insecure state the one you get by forgetting.
		//
		// **It is handed the world rather than reaching for one**, unlike
		// `SetInterest`, and the difference is not stylistic: this runs inside
		// `ApplySubmitted`, which a host calls with a world it has already
		// entered. A predicate that went and found the world itself would be
		// entering it a second time from inside itself.
		//
		// @param predicate Called as `predicate(ClientId, ecs::Entity, store)`.
		// @since v0.13
		void SetOwnership(std::function<bool(ClientId, ecs::Entity, const ecs::Store &)> predicate);

		// State a client sent for entities it owns, awaiting application.
		//
		// **Handed back rather than applied**, exactly like `Inputs`: this
		// module does not know what a component means, and a `replication` that
		// wrote one into a world would be a `replication` that knows what a
		// `Transform` is. `WriteComponents` in `Submission.hpp` is the generic
		// write a host uses, and the host is what decides when in its tick the
		// write happens.
		//
		// @param client The client to ask about.
		// @return Its accepted deltas, valid until the next `Receive` or
		//         `ClearSubmitted` for this client.
		// @since v0.13
		std::span<const Delta> Submitted(ClientId client) const;

		// Applies what a client submitted, filtered by ownership, and clears it.
		//
		// **This is where the policy is enforced**, and it is here rather than
		// in `Receive` for a mechanical reason worth knowing: a component's
		// values are one packed stream in entity order, so refusing an entity
		// means reading its value off the stream and discarding it — and only
		// the type's descriptor knows how long that value is. `Submission.hpp`
		// carries the whole of that.
		//
		// A value is written when the component is one this authority
		// replicates *and* the ownership predicate accepts the entity. Both, and
		// owning an entity does not grant a name.
		//
		// @param client The client whose submissions to apply.
		// @param store  The world to write into.
		// @return `Ok` for a write that landed whole or in part; `Malformed` or
		//         `UnknownComponent` for a delta this build cannot read, which
		//         is a client on a different build or a client making things up.
		// @since v0.13
		ApplyStatus ApplySubmitted(ClientId client, ecs::Store &store);

		// Forgets what a client submitted, without applying it.
		//
		// @param client The client to clear.
		// @since v0.13
		void ClearSubmitted(ClientId client);

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
		// @param store The authoritative world.
		// @param tick  The tick just completed.
		void Publish(ecs::Store &store, uint64_t tick);

		// What to send one client, each entry a whole message.
		//
		// @param client The client to ask about.
		// @return The messages, in the order they should go.
		std::span<const std::vector<std::byte>> Outgoing(ClientId client) const;

		// Hands back a message from `Outgoing` that the transport would not
		// take.
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
			// Whether the join snapshot is still going out.
			bool Streaming = false;

			// How many bytes of it are left to send, both blobs together.
			//
			// One number rather than two because a caller wants to know whether
			// this client is still joining; which of the two it is up to is
			// `SetPreface`'s business and nobody else's.
			size_t SnapshotRemaining = 0;

			// The last tick this client acknowledged applying in full.
			uint64_t Applied = 0;

			// How many entities this client is believed to hold.
			//
			// The set every `Created`, `Destroyed` and `Forgotten` is a
			// difference against — which is why losing sight of an entity is a
			// forget rather than a destroy.
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
			// Messages built by the last `Publish`.
			size_t Messages = 0;

			// Their total size.
			size_t Bytes = 0;

			// How many entities passed interest across every client.
			size_t Visible = 0;

			// Clients re-snapshotted rather than repaired.
			size_t Resnapshots = 0;

			// Messages the transport would not take.
			//
			// **Distinct from `Deferred`, and conflating the two is a mistake
			// people make twice.** This one means the *link* refused: the send
			// window was full or the datagram budget was spent. `Deferred` means
			// this module chose not to send yet. One is the network saying no
			// and the other is the priority ordering saying later.
			size_t Refused = 0;

			// Values this module held back because the budget was already spent.
			size_t Deferred = 0;

			// Entities a client tried to write and did not own.
			//
			// **The anti-cheat signal, and the only one this module offers.**
			// A steady zero is a game where nobody is trying; anything else is
			// a client submitting state for something it was not handed —
			// which is either a bug in that client or a person editing one.
			// See `SetOwnership` for why the plausibility half deliberately
			// lives in the host instead.
			//
			// Cumulative across `Receive` calls rather than per-`Publish`, so
			// it is a total rather than a rate. Every other figure here is
			// what the last `Publish` did.
			//
			// @since v0.13
			size_t Unowned = 0;

			// How many ticks the longest-waiting deferred value has waited.
			//
			// The number to read when something is not replicating: a rising
			// `Stalest` is a budget too small for the world, where a flat one
			// with a high `Refused` is a link problem.
			uint64_t Stalest = 0;

			// Audits built by the last `Publish`, across every client.
			//
			// @since v0.15
			size_t Audits = 0;

			// Groups a client has said it disagrees with, since this authority
			// was made.
			//
			// **The number that says the delta path let something through.**
			// A steady zero is the state this engine believes it is in; a
			// figure that climbs and then stops is divergence found and
			// repaired, and one that climbs for ever on a quiet link is a
			// client holding something the server has no record of sending it
			// — which the repair below cannot reach, and which nothing else
			// would have reported at all.
			//
			// Cumulative rather than per-`Publish`, like `Unowned`.
			//
			// @since v0.15
			size_t Disputed = 0;

			// Answers to an audit this authority refused.
			//
			// Every one is a client naming an audit it was not asked for,
			// answering one twice, or naming a group outside the slice. See
			// `Receive` for why the limit is enforced here rather than
			// configured.
			//
			// @since v0.15
			size_t DisputesRefused = 0;

			// Entities re-offered because an audit disagreed, since this
			// authority was made.
			//
			// @since v0.15
			size_t Repaired = 0;
		};

		// What the last `Publish` did.
		//
		// @return The statistics.
		const Statistics &Stats() const {
			return Stats_;
		}

	  private:
		static constexpr size_t NOWHERE = static_cast<size_t>(-1);

		// One entity's value for one component, from the sender's side.
		//
		struct Outstanding {
			uint64_t SentAt = 0;

			uint64_t WaitingSince = 0;

			uint64_t ConsideredAt = 0;
		};

		// One entity leaving or entering a client's known set.
		struct Edit {
			uint64_t Entity = 0;

			bool Restore = false;
		};

		// What one outgoing message moved, so a refusal can move it back.
		//
		struct Carried {
			size_t SnapshotOffset = NOWHERE;

			// Which blob that offset is into. A refusal that rewound the wrong
			// cursor is the `applied=184 refused=17865` failure with an extra
			// way to reach it.
			SnapshotStage Stage = SnapshotStage::World;

			uint32_t First = 0;
			uint32_t Count = 0;

			bool Values = false;

			// Whether this message was the tick's audit. A refused one leaves
			// the client with nothing to answer, so the record of what it owes
			// an answer for has to go with it — otherwise a later `Disputed`
			// naming that tick would be accepted for an audit that never went.
			bool Audit = false;
		};

		// One blob of a join, and how far through it this client is.
		struct Staged {
			std::vector<std::byte> Bytes;
			size_t Sent = 0;
			uint64_t Tick = 0;
		};

		static constexpr size_t STAGES = static_cast<size_t>(SnapshotStage::World) + 1;

		// The one audit a client may answer, and what answering it buys.
		//
		// **This record *is* the rate limit, and it is held here rather than
		// asked of the client.** A client claiming everything mismatches is
		// asking the server to resend the world, so the only thing an answer
		// can do is name groups out of a slice the server chose, once, for a
		// tick the server issued. Everything a peer could inflate — how often,
		// how many, and which — is a number on this side of the wire.
		struct AuditRecord {
			// The tick of the audit awaiting an answer, or zero for none.
			uint64_t Tick = 0;

			// Whether it has been answered. A second answer is refused rather
			// than merged: two answers to one question is a client repeating
			// itself or a client pushing.
			bool Answered = false;

			// The groups that went out, and the entities each of them named.
			// The repair is scoped to exactly these, so a client cannot widen
			// it by naming a group with more in it than the server hashed.
			std::vector<AuditGroup> Groups;
		};

		struct Client {
			uint32_t Generation = 0;
			bool Live = false;

			// Indexed by `SnapshotStage`, and streamed in that order. Two
			// cursors rather than one because a snapshot's chunking has no other
			// place ordering could live — see `SetPreface`.
			std::array<Staged, STAGES> Snapshots;

			std::unordered_set<uint64_t> Known;

			uint64_t Applied = 0;

			uint64_t Streamed = 0;

			uint64_t StreamedBefore = 0;

			std::vector<Input> Pending;

			// Accepted inbound state, cleared by the host once applied. Same
			// shape as `Pending` and for the same reason: this module carries
			// it and does not read it.
			std::vector<Delta> Submitted;

			// The newest tick this client has submitted for, which is what
			// makes an out-of-order submission a refusal rather than a
			// rewind.
			uint64_t SubmittedTick = 0;

			std::vector<std::vector<std::byte>> Outgoing;

			std::vector<std::unordered_map<uint64_t, Outstanding>> Unconfirmed;

			std::vector<Carried> Carried_;

			std::vector<Edit> Edits;

			// The audit this client owes an answer to, and the tick the last
			// one went out on.
			AuditRecord Audit;
			uint64_t AuditedAt = 0;

			// Where the next audit picks up, as an entity handle rather than a
			// position.
			//
			// **A handle, because the set it walks is not a list.** Entities
			// come and go between audits, so a position would skip whatever
			// happened to be inserted in front of it and re-audit whatever was
			// removed behind it — the same reason `Delta::Part` is a position
			// and not an arrival order, one layer up.
			uint64_t AuditCursor = 0;

			// Entities an accepted answer asked to have re-offered, consumed by
			// the next `BuildComponents`. The repair is the recovery walk that
			// already exists — a second path that resent a value would be the
			// second way to do one job.
			std::vector<uint64_t> Repairing;
		};

		// One entity's value for one component, built and waiting for a place
		// in a message.
		//
		struct Candidate {
			uint32_t Entry = 0;

			uint32_t Row = 0;

			ecs::Entity Entity;

			uint64_t WaitingSince = 0;

			float Hint = 0.0f;
		};

		// How much of a delta a packing pass got onto the wire.
		struct Placement {
			size_t Values = 0;
		};

		struct Signature {
			std::unordered_map<uint64_t, uint64_t> Hashes;

			std::vector<uint64_t> Changed;
		};

		// Queues an inbound delta, having checked only that somebody has said
		// who owns what. The filtering is `ApplySubmitted`'s.
		//
		// **Takes the resolved `Client` and not its id**, because the one caller
		// has already looked it up and a second lookup from the same id would be
		// a second answer to a question already answered.
		bool Submit(Client &into, Delta &&delta);

		Client *Reach(ClientId client);
		const Client *Reach(ClientId client) const;
		void Survey(ecs::Store &store);
		void Resign(ecs::Store &store);

		// Bytes of a join still owed to a client, across both blobs.
		static size_t Owed(const Client &client);

		// Saves `entities` and the values a client would decode for them.
		//
		// Empty means the world could not be written at all: a `Save` of a store
		// with nothing in it still writes a header.
		std::vector<std::byte> Capture(ecs::Store &store, std::span<const ecs::Entity> entities) const;

		void BeginSnapshot(Client &client, ecs::Store &store, uint64_t tick);
		void StreamSnapshot(Client &client);
		void BuildComponents(ecs::Store &store, Client &client, Delta &delta, uint64_t tick);
		void Prioritise(ClientId client, uint64_t tick);
		Placement Pack(Client &client, const Delta &delta, size_t messageLimit);
		void Record(Client &client, const Placement &placed, uint64_t tick);
		void EmitStructure(Client &client, const Structure &structure);

		// Builds this tick's audit for one client, if one is due.
		//
		// Emitted after the delta and never before it, so the message the byte
		// budget turns away first is the one whose loss costs nothing.
		void EmitAudit(const ecs::Store &store, ClientId handle, Client &client, uint64_t tick);

		// Takes a client's answer to an audit, having decided the server agrees
		// it asked the question.
		bool Dispute(Client &into, const replication::Disputed &disputed);

		AuthoritySettings Settings_;
		std::function<bool(ClientId, ecs::Entity, const ecs::Store &)> Interest;
		std::function<float(ClientId, ecs::Entity)> Priority;
		std::function<bool(ecs::Entity, const ecs::Store &)> Preface;
		std::function<bool(ClientId, const Identify &)> IdentityCheck;

		// Empty refuses everything, which is the safe half of the default. See
		// `SetOwnership`.
		std::function<bool(ClientId, ecs::Entity, const ecs::Store &)> Ownership;
		std::vector<core::Name> Components;

		std::vector<ChangeDetection> Detection;

		// Per slot, the tag whose presence suppresses that component's deltas for
		// one entity, or an invalid name for none. See `SuppressWhenTagged`.
		//
		// Parallel to `Components` for the same reason `Detection` is: these are
		// three facts about one slot, and a map keyed by name would be a second
		// place the slot list could be wrong.
		std::vector<core::Name> Suppressors;

		// The same list resolved to ids, refilled by `Survey` beside `Resolved`.
		//
		// Held rather than looked up in the delta loop because that loop runs per
		// component per client per tick, and `Components::Find` is a hash of a
		// string each time.
		std::vector<ecs::ComponentId> ResolvedSuppressors;

		std::vector<Signature> Signatures;

		std::vector<Client> Clients;
		Statistics Stats_;

		std::vector<ecs::Entity> Visible;

		// The subset of `Visible` the preface predicate claimed, refilled per
		// join rather than per publish because that is the only moment it is
		// asked.
		std::vector<ecs::Entity> Preceding;

		std::vector<Candidate> Candidates;

		std::vector<uint64_t> Bearing;

		std::vector<ecs::ComponentId> Resolved;

		// The same list as names, which is what an audit puts on the wire. Kept
		// beside `Resolved` rather than derived from `Components`, because a
		// leaf's ordinal is a position in both and only a shared filling pass
		// keeps them the same positions.
		std::vector<core::Name> ResolvedNames;

		std::vector<uint32_t> Order;

		std::vector<size_t> Strides;
		std::vector<size_t> SourceSlot;

		std::vector<size_t> OpenEntry;

		// Sorted recovery entries preserve deterministic wire order.
		std::vector<uint64_t> Recovering;

		std::vector<uint64_t> Appearing;

		// One client's known set in handle order, for the audit.
		//
		// Sorted rather than walked in place, for `Recovering`'s reason: the
		// known set is an `unordered_set` and two servers that inserted into it
		// in different orders would hash the same world into different digests.
		std::vector<ecs::Entity> Auditing;

		// The subset of `Resolved` one audit's batch actually carries, which is
		// what its leaf ordinals count in.
		std::vector<ecs::ComponentId> Auditable;
	};
}
