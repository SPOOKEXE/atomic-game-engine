#pragma once

// @tier L12 · shared

#include <engine/core/Name.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/replication/Audit.hpp>
#include <engine/replication/Protocol.hpp>
#include <engine/replication/Submission.hpp>

#include <algorithm>
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
		// for somebody who disconnected be delivered to whoever arrived next -
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
		// number** - the version that compared tick numbers re-snapshotted a
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
		// could be enforced - and that division *is* a per-client cap.
		size_t BytesPerTick = 32 * 1024;

		// How long a value may wait before it outranks every score.
		//
		// What stops the priority ordering starving anything: the bound on how
		// late a value can be is `StarvationTicks + ceil(n/k)`, which is
		// asserted by a test rather than argued for.
		uint64_t StarvationTicks = 30;

		// How much wider than a tick's reach the refinement window is.
		//
		// **What `SetPriorityRefinement` is asked about, as a multiple of the
		// rows this tick could carry.** One would refine exactly the rows that
		// fit, and a row demoted out of them would be replaced by one nothing
		// had looked at properly. Two leaves a refined row behind every refined
		// row, which is the cheapest number that does.
		//
		// Zero refines nothing and is the way to turn the second hook off
		// without unregistering it. Raising it buys fidelity at a linear cost in
		// whatever the refinement does, which is the expensive thing by
		// construction - it is the reason the hook is separate.
		//
		// @since v0.16
		size_t PriorityRefinementFactor = 2;

		// How many unacknowledged rows one component re-offers to one client in
		// a tick.
		//
		// **A bound on the *rebuild*, not on what is sent.** The recovery walk
		// re-offers every value a client has not acknowledged, which is right
		// while a client is keeping up and is the whole world once it is not:
		// at two hundred clients the measured link took about forty rows a tick
		// and this function was serialising two thousand of them per component
		// to choose from.
		//
		// Large enough that a healthy connection never reaches it, so this
		// changes nothing until a client is far behind - which is exactly when a
		// server can least afford the work. The walk resumes where it stopped,
		// so a bound costs coverage within a tick and none across ticks.
		//
		// Zero is no bound, and is the behaviour from before there was one.
		//
		// @since v0.16
		size_t RecoveryRowsPerTick = 512;

		// The anti-entropy audit over what a client already holds.
		//
		// @since v0.15
		AuditSettings Audit;

		// How many join snapshots one `Publish` may build.
		//
		// **A join builds a world, and a tick that built sixteen of them took
		// 1.8 seconds.** `Authority::Capture` makes a scratch `ecs::Store`, an
		// entity and a component row per entity in the client's interest set,
		// and a `Save` over the result - measured at about 113 ms for ten
		// thousand entities in the `release` preset. Nothing bounded how many of
		// those one tick did, so a listener that admitted sixteen clients
		// together spent that many in one tick, answered no handshakes while it
		// did, and the clients waiting behind it timed out. At thirty-two
		// clients against a ten-thousand-entity world, four of them ever joined.
		//
		// **Two, because the cost is a property of the world and not of this
		// number.** One join is already over a 30 Hz budget on a large world, so
		// the number cannot buy a tick that fits; what it buys is that the
		// overrun is one join long instead of however many arrived at once. Two
		// keeps the ordinary two-client case joining in a single tick. A client
		// turned away waits exactly one tick and is still joining on the next,
		// so a full server fills in `MaximumClients / JoinsPerTick` ticks.
		//
		// Zero is no bound, which is the behaviour from before there was one.
		//
		// @since v0.19
		size_t JoinsPerTick = 2;

		// How many steady-state clients a publish needs before it spreads them
		// across the job pool.
		//
		// **Below this the loop is the serial loop it always was, and the number
		// is measured rather than chosen.** Rule 5's second half: parallel is
		// not free, and the crossover is higher than it looks. A client's
		// publish is a walk of the world through the interest predicate, a
		// structural comparison against what it holds, and a delta built out of
		// runs `Survey` gathered - real work, but small enough at low client
		// counts that a batch dispatch and a cold lane cost more than they save.
		//
		// **Eight, and the number is two measurements rather than one.**
		// `engine.replication.bench.publish` runs the ladder at ten thousand
		// entities with four replicated components and five hundred of them
		// moving a tick, on a twenty-four-thread machine in the `bench` preset.
		// Nanoseconds per client per tick, serial against lanes:
		//
		//     1 client    421 833   546 196
		//     2 clients   498 376   279 655
		//     4 clients   383 135   238 385
		//     16 clients  367 494    92 175
		//     64 clients  392 635    53 743
		//     128 clients 384 908    84 350
		//     200 clients 485 804    79 305
		//
		// **The ladder cannot resolve the crossover below about four, and the
		// one-client row is what says so.** At one client both settings run the
		// same code - `LanesFor` answers one lane and the batch is never
		// dispatched - and the two rows are 30% apart anyway, from nothing but
		// which fixture was built onto a warmer heap. From sixteen up the gap is
		// four to seven times, which no fixture effect explains.
		//
		// A client count on its own is also the wrong unit, because what a
		// client's publish costs is a function of the world: a hundred-entity
		// world is a hundredth of that ladder's per-client work, about four
		// microseconds. `engine.parallel.bench.dispatch` measures a batch at
		// **20 us** for eight ranges, so on that world the crossover is around
		// six to eight clients. Eight is above the crossover on both readings,
		// and it costs a large world almost nothing: eight clients over ten
		// thousand entities is 3 ms of publish either way, on a tick with 33 ms
		// in it.
		//
		// A host whose interest or priority hooks are not safe to call from
		// several threads at once sets this to `SIZE_MAX`, which is the serial
		// loop for ever. See `Authority::SetInterest`.
		//
		// @since v0.19
		size_t ParallelClientThreshold = 8;
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
		// One authoritative world participating in a shared publish dispatch.
		// References are borrowed only for the duration of `PublishMany`.
		struct PublishRequest {
			// Replicator, live world, and authoritative tick to publish.
			//@{
			Authority &Source;
			ecs::Store &World;
			uint64_t Tick = 0;
			//@}
		};

		// Creates a server-side replicator.
		//
		// @param settings How to stream.
		explicit Authority(const AuthoritySettings &settings = {});

		// Declares a component as replicated, by name.
		//
		// **Opt in, not opt out.** A world holds components no client has any
		// business receiving - a server-side AI's scratch state, a pending bus
		// request - and a default of "everything" makes leaking one the
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
		// anyway and were overwritten the moment they landed - five ten-byte
		// quantised `CFrame`s per character per tick, against roughly ten bytes
		// for the root alone.
		//
		// **Named rather than typed, because the tag belongs to a module this one
		// must not see.** `scene` is L7 and this is L12, so the component that
		// marks a derived row is declared where the thing producing it lives and
		// reaches here as a string - which is rule 4, and the same way
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
		// one - which is why this could be decided without a second consumer to
		// check it against, as `D00115` expected to need.
		//
		// **A tagged entity is also out of the audit entirely.** The two ends
		// are meant to disagree about a derived row, and a hash has no
		// tolerance - see `Audit.hpp`. What that costs is that a character root
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
		// hide its `ServerStorage` says so - see `scene::VisibleToClients`.
		//
		// **Called from several threads at once, and so is every other hook on
		// this class except `SetPreface`.** `Publish` spreads its steady-state
		// clients across the job pool once there are more than
		// `AuthoritySettings::ParallelClientThreshold` of them, so a predicate
		// is asked about different clients on different threads inside one
		// call. What that requires of a host is exactly what it already
		// required of it: read the world and this module's arguments, and write
		// nothing. `mono.server`'s predicate is two binary searches and a map
		// lookup over tables its tick filled before the publish began, and its
		// scorer's occlusion test is `physics::Raycast` over a `const
		// ecs::Store &`.
		//
		// A host that cannot promise that sets `ParallelClientThreshold` to
		// `SIZE_MAX`, which keeps the serial loop this class has always had.
		//
		// @param predicate Called as `predicate(ClientId, ecs::Entity, const
		//                  ecs::Store &)`. Must be safe to call concurrently and
		//                  must not write to the store.
		void SetInterest(std::function<bool(ClientId, ecs::Entity, const ecs::Store &)> predicate);

		// Scores which entities a client is sent first when not all of them
		// fit.
		//
		// @param score Called as `score(ClientId, ecs::Entity)`, concurrently
		//        for different clients - see `SetInterest`. Empty scores
		//        everything the same, which leaves the rotation in sole charge
		//        and is a plain round robin.
		void SetPriority(std::function<float(ClientId, ecs::Entity)> score);

		// Adjusts the score of a row near enough to the front to be worth a
		// second, more expensive look.
		//
		// **The split exists because the two halves cost different amounts by
		// orders of magnitude.** `mono.server`'s score is a subtraction; its
		// occlusion test is a raycast against the broad phase, and asking it
		// about every entity for every client was measured at 51% of a
		// two-hundred-client tick. Every row it was asked about was one the tick
		// had no budget to send, so the answer was thrown away.
		//
		// **It is consulted for the rows that could reach the wire, and not for
		// the rest.** That window is derived from the same byte and message
		// budget `Prioritise` sorts against, widened by
		// `PriorityRefinementFactor` so a row demoted out of the sendable set
		// has candidates behind it that were looked at properly too.
		//
		// **This is an ordering within a tick and not a filter.** A row outside
		// the window keeps its unrefined score, which is why a refinement must
		// only ever *lower* one: an unrefined score is then an upper bound, and
		// a row that could not reach the front on its best case cannot reach it
		// on its real one. A refinement that raised a score would hide rows it
		// was never asked about, which is a different and much worse thing.
		//
		// **It is an approximation and the honest form of one.** A row just
		// outside the window that would have overtaken a heavily demoted row
		// inside it waits a tick. The starvation rotation is what bounds that:
		// a row that has waited its deadline outranks every score, refined or
		// not, so nothing here can starve anything.
		//
		// @param refine Called as `refine(ClientId, ecs::Entity, float)` with
		//        the score `SetPriority` gave, returning the score to sort by.
		//        Empty leaves every score as scored, which is what a host with
		//        one cheap hook wants and is the default.
		void SetPriorityRefinement(std::function<float(ClientId, ecs::Entity, float)> refine);

		// Decides which entities a joining client is sent *before* the world.
		//
		// **`SetPriority` orders a stream and this orders a join, and neither
		// can do the other's job.** A join is one `ecs::Store::Save` chunked
		// across ticks in the order the store wrote its archetypes, and a score
		// does not reach inside a byte cursor - so a client's very first view of
		// a world was whatever the storage happened to lay down first, whatever
		// the priority hook said. What this predicate picks out is captured and
		// finished as its own blob before the world's first chunk is built.
		//
		// **Applied as an overlay on the far side, and sent twice.** The blob is
		// a slice of a world rather than the whole of one, so a receiver may not
		// sweep what it fails to mention - and the world blob that follows still
		// carries these entities, because *that* one is the complete picture and
		// the sweep behind it is what retires a client's stale rows. The
		// duplication is the price, and it is the size of what a host puts in
		// front rather than the size of the world.
		//
		// **No `ClientId`, unlike every other hook here.** What goes first is a
		// property of the content - Roblox's `ReplicatedFirst` is a container,
		// not a per-player decision - and interest has already narrowed the set
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
		// the last one and refuse a jump - and it would be wrong to, because it
		// does not know whether this game has teleports, launch pads, vehicles
		// or a grappling hook. A speed limit invented at this layer is a limit
		// every game has to work around and none can tune. So the engine
		// enforces *who*, the host enforces *what*, and a game that needs
		// movement validation writes it where the movement rules already live.
		// `Unowned` is the number that says somebody is trying.
		//
		// **No predicate means nothing may be written.** An authority that has
		// not been told who owns what refuses every inbound delta, because the
		// alternative - accepting them until somebody remembers to restrict it
		// - makes the insecure state the one you get by forgetting.
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
		// means reading its value off the stream and discarding it - and only
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

		// Tells this authority what one client's link says the path will carry.
		//
		// **The link's allowance is the authority and `BytesPerTick` is a
		// ceiling on what this module is willing to *produce*.** Congestion
		// control measures the path; `BytesPerTick` is a number somebody typed.
		// Packing rows past the allowance does not send more - `Link::Reserve`
		// refuses them - it spends the encode and then hands the refusal to
		// `Unsent`, which rebuilds the same rows next tick. Under the allowance
		// the same shortfall reaches the *priority scheduler* instead, which is
		// the thing that exists to decide what a client sees when not all of it
		// fits.
		//
		// **Told rather than read, because this module holds no link.** That is
		// the same division `SetPriority` and `Rewind::Record` are built on: the
		// arithmetic is here and the lookup is the host's. `Listener::Advance`
		// is the caller.
		//
		// A caller that never calls this is unchanged: the default allowance is
		// unbounded and `BytesPerTick` alone decides, exactly as before v0.15.
		//
		// @param client The client whose link this describes.
		// @param bytes  `ConnectionStats::SendAllowanceBytes` for that link.
		// @since v0.15
		void SetAllowance(ClientId client, size_t bytes);

		// Builds this tick's messages for every client.
		//
		// @param store The authoritative world.
		// @param tick  The tick just completed.
		void Publish(ecs::Store &store, uint64_t tick);

		// Publishes independent worlds with one signing dispatch across all of
		// their component slots. The store-owning preparation and completion
		// passes remain ordered on the caller; only the gathered, read-only hash
		// work is combined.
		//
		// @return How many requests had an admitted client and were published.
		static size_t PublishMany(std::span<const PublishRequest> requests);

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
			// difference against - which is why losing sight of an entity is a
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

			// How many entities passed interest, summed over the clients this
			// publish actually asked about.
			//
			// **Not every live client**, and the difference is a walk of the
			// world per client per tick. A client part way through a join is
			// being sent a blob taken when it started, so asking the interest
			// predicate about it again produces an answer nothing reads.
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
			// a client submitting state for something it was not handed -
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
			// - which the repair below cannot reach, and which nothing else
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

			// Rows no delta message could ever carry, since this authority was
			// made.
			//
			// **The number that says a value is crossing by the other path.** A
			// delta row has to fit one message whole, and a `script.Program`
			// holding four kilobytes of Luau does not - so the entity is staged
			// as an overlay blob instead, through the chunking a preface already
			// uses, because a second bulk path would be a second way to do one
			// job. A figure that climbs on a running world is a game creating
			// large values mid-session rather than authoring them; one that
			// climbs every tick is a row being chased rather than staged, which
			// is the bug `Client::Oversize` exists to have fixed.
			//
			// @since v0.15
			size_t Oversized = 0;
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

		// What one client still owes an acknowledgement for, in one component.
		//
		// **A sorted vector rather than a hash map, and the ordering is the
		// whole reason.** The recovery walk has to visit these in a fixed order
		// or two runs of one server produce different bytes, and an
		// `unordered_map` gave no order at all - so every tick copied every
		// unconfirmed key of every component of every client into a scratch
		// vector and sorted it. On a saturated server the unconfirmed set *is*
		// the world, which made that copy and sort the largest single thing
		// `BuildComponents` did.
		//
		// Held in order instead, the walk is a pass over contiguous memory and
		// the sort does not exist. What it costs is insertion, which moves the
		// tail - and insertion is the rare operation here: an entity enters a
		// client's set once and is looked up every tick until it leaves.
		//
		// **Bulk insertion is a separate call for that reason.** Adding a
		// joining client's whole world one entity at a time is quadratic, so
		// `EmplaceAll` appends and re-sorts once. See its comment for why the
		// existing row survives a collision.
		class OutstandingSet {
		  public:
			// One entity's outstanding state, as the flat sorted array holds it.
			struct Row {
				// The entity this row is about, and the sort key.
				uint64_t Entity = 0;

				// What is outstanding for it.
				Outstanding Value;
			};

			// Finds or inserts, exactly as a map's `operator[]` does.
			Outstanding &operator[](uint64_t entity);

			// Inserts only when absent, leaving an existing row untouched.
			//
			// A repair must not restart the clock on something genuinely in
			// flight, which is what an assigning insert would do.
			void Emplace(uint64_t entity);

			// The same for a whole span, in one sort rather than one per entity.
			void EmplaceAll(std::span<const uint64_t> entities);

			void Erase(uint64_t entity);

			// Erases every entity in an **ascending** span, in one pass.
			//
			// The recovery walk drops rows as it goes and its input is already
			// in order, so erasing them one at a time would move the same tail
			// once per drop.
			void EraseSorted(std::span<const uint64_t> entities);

			bool Contains(uint64_t entity) const;

			void Clear() {
				Rows.clear();
			}

			size_t Size() const {
				return Rows.size();
			}

			// Ascending by entity, which is what the recovery walk relies on.
			std::vector<Row>::const_iterator begin() const {
				return Rows.begin();
			}

			std::vector<Row>::const_iterator end() const {
				return Rows.end();
			}

			// Fills `into` with up to `limit` entities not considered on `tick`,
			// resuming where the last call stopped and wrapping once.
			//
			// **A bound with a rotation, because the alternative is a bound
			// with a starvation.** A client far enough behind has every entity
			// it knows about unconfirmed, so an unbounded walk rebuilds and
			// re-serialises the whole world to send the forty rows its link will
			// take. Cutting the walk at the front would rebuild the same rows
			// every tick and never reach the rest; resuming past them visits
			// everything within `ceil(held / limit)` ticks, whatever the scores
			// say.
			//
			// **A limit of zero means no limit**, which is what a host that has
			// not thought about this gets and is the behaviour from before there
			// was a cursor.
			//
			// Ids rather than rows, because offering one inserts into this set.
			//
			// @param tick  The tick being built.
			// @param limit How many to take, or zero for all of them.
			// @param into  Cleared and filled. Not sorted: the wrap is what
			//              makes it a rotation.
			void SelectRecovering(uint64_t tick, size_t limit, std::vector<uint64_t> &into);

			// Drops every row a predicate answers true for, in one pass.
			template <class Match> void EraseIf(Match &&match) {
				Rows.erase(std::remove_if(Rows.begin(), Rows.end(), match), Rows.end());
			}

		  private:
			std::vector<Row> Rows;

			// Where `SelectRecovering` resumes, as an entity handle rather than
			// a position - `Client::AuditCursor`'s rule, and for its reason:
			// entities come and go between ticks, so a position would skip
			// whatever was inserted in front of it.
			uint64_t Cursor = 0;
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
			// an answer for has to go with it - otherwise a later `Disputed`
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
		// tick the server issued. Everything a peer could inflate - how often,
		// how many, and which - is a number on this side of the wire.
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
			// place ordering could live - see `SetPreface`.
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

			std::vector<OutstandingSet> Unconfirmed;

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
			// removed behind it - the same reason `Delta::Part` is a position
			// and not an arrival order, one layer up.
			uint64_t AuditCursor = 0;

			// Entities an accepted answer asked to have re-offered, consumed by
			// the next `BuildComponents`. The repair is the recovery walk that
			// already exists - a second path that resent a value would be the
			// second way to do one job.
			std::vector<uint64_t> Repairing;

			// What the link last said the path will carry in a tick, or
			// `SIZE_MAX` for a caller that has never said. See `SetAllowance`.
			size_t AllowanceBytes = SIZE_MAX;

			// Entities owed an overlay because one of their rows is larger than
			// any delta message.
			//
			// Filled by `BuildComponents` and drained by `StageOversize`, which
			// captures exactly these into the same staged blob a preface uses.
			// See `Statistics::Oversized`.
			std::vector<uint64_t> Oversize;
		};

		// One entity's value for one component, built and waiting for a place
		// in a message.
		//
		struct Candidate {
			uint32_t Entry = 0;

			// Where this row's encoded value sits inside its entry's `Values`,
			// and how long it is.
			//
			// **A span rather than a row number times a stride, because a row is
			// not a fixed width.** A component may serialise to a different
			// number of bytes per entity - `scene.Visual` writes two names and
			// `ecs.InstanceName` writes one, so two parts whose meshes have
			// differently-spelt names are two different lengths in the same
			// entry. `Pack` reorders rows by priority and slices them out again,
			// and slicing at `Row * stride` reads the wrong bytes for every row
			// after the first one whose length differs.
			uint32_t Offset = 0;
			uint32_t Bytes = 0;

			ecs::Entity Entity;

			uint64_t WaitingSince = 0;

			float Hint = 0.0f;
		};

		// How much of a delta a packing pass got onto the wire.
		struct Placement {
			size_t Values = 0;
		};

		// One contiguous slice of a store column, borrowed rather than copied.
		//
		// **Gathered on the thread that owns the store and read off it.** The
		// `Each*Runs` family walks the archetype tables and may only be called by
		// the owning thread; reading the block it handed back is a plain memory
		// read that any thread may do. Splitting the two is what lets signing fan
		// out across workers and what lets the per-client loop read this tick's
		// changed rows without walking the store once per client.
		//
		// The pointers are into the store's own columns and stay good until a
		// structural change. `Publish` reads and never writes - a join snapshot
		// is built into a scratch store, not this one - so they are good for the
		// whole of the publish that gathered them.
		struct ColumnRun {
			// The entities in this run, and the component bytes beside them.
			//@{
			const ecs::Entity *Entities = nullptr;
			const std::byte *Values = nullptr;
			//@}

			// How many rows both pointers cover.
			size_t Rows = 0;
		};

		// Everything about one declared slot that does not depend on which
		// client is being published to, worked out once per tick by `Survey`.
		//
		// **The point is that none of it depends on the client.** `Publish`'s
		// per-client loop used to ask `ecs::Components` for the id and the
		// descriptor of every declared slot and then walk the store for that
		// slot's changed rows - all three answers identical for every client, and
		// all three re-derived per client. Two hundred clients times two dozen
		// slots is nine thousand acquisitions of the component registry's
		// process-wide mutex and nine thousand archetype walks a tick, where two
		// dozen of each would do.
		struct Crossing {
			// The registered id, or invalid where this process registered no
			// such component.
			ecs::ComponentId Id;

			// The descriptor behind `Id`, or null with it.
			//
			// A pointer rather than a copy because `Components::Describe` hands
			// back a reference into a `std::deque` that only ever grows, so the
			// address is stable for the life of the process. Refreshed every
			// tick anyway, because a slot is allowed to resolve later than the
			// authority that declared it.
			const ecs::TypeDescriptor *Descriptor = nullptr;

			// Whether a row of this component may go into a delta at all: it is
			// registered, it has a serialisation, and its widest stored value
			// still leaves room in a message.
			//
			// A refused slot is warned about here, once a tick, where it used to
			// be warned about once per client per tick.
			bool Sendable = false;

			// This tick's changed rows, for a slot whose detection reads the
			// store's dirty bits. Empty for a signed slot, which carries its
			// changed ids in `Signature::Changed` instead.
			std::vector<ColumnRun> Changed;
		};

		struct Signature {
			// Entity, last hash - ascending by entity id, because `SignSlot`
			// rebuilds it by merge-walking `Hashed`, which is sorted for the
			// same reason.
			//
			// **A sorted vector rather than a hash map, as of v0.18.** A tick with
			// twenty thousand signed entities did twenty thousand
			// `unordered_map::try_emplace` calls here, each a scattered heap node;
			// walking two sorted sequences together touches memory in order instead.
			// It also folds in what used to be a second full pass: an entity this
			// tick's carrier set skipped past is simply not carried into the
			// rebuilt vector, so the map's separate dead-entry sweep no longer
			// exists.
			std::vector<std::pair<uint64_t, uint64_t>> Hashes;

			std::vector<uint64_t> Changed;

			// One contiguous block of this slot's column, as `Store::EachRuns`
			// hands it over.
			//
			// **Gathered on the owning thread and hashed off it**, which is the
			// whole reason `Resign` is in two passes. `EachRuns` walks the
			// archetype tables and may only be called by the thread that owns
			// the store; the hashing that follows touches nothing outside this
			// slot. Splitting them is what lets a dozen signed components be
			// signed at once rather than one after another - measured as a
			// dozen even costs of about 0.3 ms each, so the shape of the work
			// was already a fan-out waiting to happen.
			//
			// See `ColumnRun` for how long the borrowed pointers stay good.
			std::vector<ColumnRun> Runs;

			// Bytes one value occupies, read once on the owning thread because
			// `Components::Describe` takes the registry's process-wide lock.
			size_t Stride = 0;

			// This slot's scratch, per slot rather than shared.
			//
			// **One buffer between every slot was right while they were signed
			// one at a time and is a race now.** Held rather than local for the
			// reason it always was: a vector whose size barely moves, rebuilt
			// every tick, should not allocate every tick.
			//@{
			std::vector<std::pair<uint64_t, uint64_t>> Hashed;
			std::vector<std::pair<uint64_t, uint64_t>> Next;
			//@}

			// What this slot cost, so the panel keeps its row per component.
			//
			// **Timed by whichever worker signed it and reported by the owner.**
			// `FrameGraph` drops a span opened off the owning thread, so signing
			// in parallel would have quietly taken away the per-component
			// breakdown - which is the thing that showed signing to be a dozen
			// even costs rather than one expensive one.
			float Milliseconds = 0.0f;
		};

		// One publishing lane, and everything the per-client body writes that
		// does not belong to the client.
		//
		// **The authority used to keep one copy of each of these, which was
		// right while the loop was serial and is a race the moment two clients
		// are published at once.** Held rather than local for the reason they
		// always were: vectors whose size barely moves, rebuilt every tick,
		// should not allocate every tick.
		//
		// **A lane and not a client.** The storage is what keeps the allocation
		// out of the tick, and a server publishing two hundred clients through
		// eight workers needs eight of these, not two hundred. Which lane a
		// client lands in changes nothing it produces: every field here is
		// cleared before it is used and no client reads another's.
		struct Lane {
			// The entities this client may be sent, ascending by handle.
			std::vector<ecs::Entity> Visible;

			// The rows this tick built, and a permutation of them the priority
			// pass puts in order.
			//@{
			std::vector<Candidate> Candidates;
			std::vector<uint32_t> Order;
			//@}

			// One score per *entity* per client, indexed against `Bearing`.
			//
			// **Because a score is a function of the client and the entity, and
			// a candidate is a row.** An entity with a transform, a motion, a
			// name and a class is four candidates that a host's scorer is asked
			// about four times for one answer - and the answer costs whatever
			// that host's lookup costs, which for `mono.server` includes an
			// occlusion raycast. Refilled per client, so nothing survives a
			// publish to disagree with a world that has moved.
			std::vector<float> Scores;

			// The same, for the refined half, and a second array rather than a
			// flag beside the first.
			//
			// A refinement is asked about the entities inside the window and
			// about no others, so "not asked" and "asked and unchanged" have to
			// stay distinguishable: writing a refined value back over `Scores`
			// would make the second row of an entity already in the window look
			// unrefined the moment the window moved.
			std::vector<float> Refined;

			// Which declared slot each delta entry came from, and which message
			// each entry is currently open in.
			//@{
			std::vector<size_t> SourceSlot;
			std::vector<size_t> OpenEntry;
			//@}

			// Recovery entries in `OutstandingSet` order, which is ascending.
			// Held as ids rather than walked in place because offering one
			// inserts into the set it came from.
			std::vector<uint64_t> Recovering;

			// The rows the recovery walk found nothing behind, dropped in one
			// pass once the walk has finished. Ascending, because `Recovering`
			// is.
			std::vector<uint64_t> Dropping;

			// The entities this client has just been told about, seeded into
			// every component's unconfirmed set so the recovery walk carries
			// them.
			std::vector<uint64_t> Appearing;

			// One client's known set in handle order, for the audit.
			//
			// Sorted rather than walked in place, for `Recovering`'s reason: the
			// known set is an `unordered_set` and two servers that inserted into
			// it in different orders would hash the same world into different
			// digests.
			std::vector<ecs::Entity> Auditing;

			// The subset of `Resolved` one audit's batch actually carries, which
			// is what its leaf ordinals count in.
			std::vector<ecs::ComponentId> Auditable;

			// The non-overlapping pieces of a client's publish, in nanoseconds.
			//
			// **This is what an `ENGINE_PROFILE` in the per-client path became.**
			// `core::FrameGraph` drops a span it did not open on the recording
			// thread, so the moment the loop went parallel every one of those
			// bars would have turned into a number in the drop counter - which
			// is exactly the way profiling a parallel loop misleads. The lanes
			// count nanoseconds with no lock and `Publish` reports the totals
			// under the names the spans used, so the graph reads the same
			// whether the tick ran on one thread or twenty-three.
			//
			// **Leaves rather than a tree.** `FrameGraph::Report` adds a flat
			// bar, so a total reported beside its own parts would count the
			// parts twice. What is here is exactly the set of blocks that do not
			// contain one another.
			enum class Phase : uint8_t {
				Interest,			///< Filtering `Bearing` through the host's predicate.
				Structure,			///< What this client gained and lost since last tick.
				PrepareOutstanding, ///< Seeding the unconfirmed sets.
				DetectRows,			///< Turning what moved into candidate rows.
				RecoverRows,		///< Re-offering what the client has not confirmed.
				CommitRows,			///< Moving a component's rows into the delta.
				Score,				///< The host's cheap priority hook.
				Sort,				///< Ordering the candidates.
				Refine,				///< The host's expensive priority hook.
				Pack,				///< Cutting the delta into messages.
				Record,				///< The acknowledgement bookkeeping.
				Audit,				///< Hashing a slice of what the client holds.

				Count
			};

			std::array<double, static_cast<size_t>(Phase::Count)> Spent{};

			// Times a block into one of `Spent`.
			//
			// One monotonic clock read at each end and an add, which is what an
			// `ENGINE_PROFILE` costs when nothing is attached - so this is the
			// same price for a number that survives being taken on a worker.
			class Timed {
			  public:
				// Starts timing `phase` on `lane`, which must outlive this.
				Timed(Lane &lane, Phase phase);

				// Adds the elapsed nanoseconds to the phase.
				~Timed();

				// Each instance records exactly once, so there is nothing a copy
				// could mean.
				//@{
				Timed(const Timed &) = delete;
				Timed &operator=(const Timed &) = delete;
				//@}

			  private:
				double &Into;
				uint64_t Began;
			};

			// What this lane's clients contributed to `Statistics`, merged into
			// `Stats_` in lane order once the batch has finished.
			//
			// **Only the counters a per-client publish touches**, rather than a
			// whole `Statistics`: the rest are cumulative across `Receive` and
			// merging them by addition would count them twice. Six sums and one
			// maximum, so the merge order does not change the answer - it is
			// done in lane order anyway, because a number that depended on which
			// worker finished first would be a number nobody could reproduce.
			struct Tally {
				// The messages this lane built and their total size. See
				// `Statistics::Messages` and `Statistics::Bytes`.
				//@{
				size_t Messages = 0;
				size_t Bytes = 0;
				//@}

				// Entities that passed interest, over this lane's clients only.
				// See `Statistics::Visible` for what summing them means.
				size_t Visible = 0;

				// Values this lane held back because the budget was spent. See
				// `Statistics::Deferred`.
				size_t Deferred = 0;

				// Audits this lane built. See `Statistics::Audits`.
				size_t Audits = 0;

				// Rows this lane staged as a blob for being too big for any
				// message. See `Statistics::Oversized`.
				size_t Oversized = 0;

				// The longest deferred wait this lane saw, in ticks.
				//
				// **Merged by maximum where the six above merge by sum**, and
				// that asymmetry is what keeps the figure independent of how
				// many lanes ran: the stalest value in the world is the stalest
				// one some lane saw, and adding two lanes' worst cases would
				// report a wait nothing waited. See `Statistics::Stalest`.
				uint64_t Stalest = 0;
			};

			Tally Stats;

			// Whether this lane found that one audit group does not fit a
			// message.
			//
			// **A flag rather than the setting itself.** Switching the audit off
			// is a write to `Settings_`, and a worker writing a setting every
			// other worker is reading is the race the whole of this struct
			// exists to avoid. The owner applies it after the batch.
			bool AuditTooLarge = false;
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
		void PrepareSurvey(ecs::Store &store);
		void FinishSurvey(ecs::Store &store);
		void GatherSignatures(ecs::Store &store);
		void SignSignatures();
		void ReportSignatures();
		void ResetPublishStatistics();
		void PublishAfterSurvey(ecs::Store &store, uint64_t tick);

		// Signs one slot from the runs `Resign` gathered for it.
		//
		// **Called from a worker.** It reads the store's columns through those
		// runs and writes only into the signature handed to it, which is what
		// makes a dozen of them at once safe.
		static void SignSlot(Signature &signature);

		// Bytes of a join still owed to a client, across both blobs.
		static size_t Owed(const Client &client);

		// Saves `entities` and the values a client would decode for them.
		//
		// Empty means the world could not be written at all: a `Save` of a store
		// with nothing in it still writes a header.
		std::vector<std::byte> Capture(ecs::Store &store, std::span<const ecs::Entity> entities) const;

		void BeginSnapshot(Lane &lane, Client &client, ecs::Store &store, uint64_t tick);

		// Stages the entities `Client::Oversize` names as an overlay blob.
		//
		// **The same three pieces of machinery a preface uses, aimed at a
		// handful of entities instead of at a container.** A value larger than a
		// delta message needs a path that chunks, and this module has exactly
		// one; what it does not need is the whole world, which is what a
		// re-snapshot would have cost - measured at 81 ticks of streaming on a
		// scene of a hundred entities, twice, for three module scripts nothing
		// had edited.
		void StageOversize(Client &client, ecs::Store &store, uint64_t tick);

		void StreamSnapshot(Client &client);
		// Filters `Bearing` through the host's interest predicate into
		// `Lane::Visible`, ascending by handle.
		void SelectVisible(Lane &lane, ClientId handle, const ecs::Store &store);

		// Publishes one client that owes no snapshot bytes.
		//
		// **Called from a worker**, so what it may touch is the rule rather than
		// an implementation detail: this client, this lane, and everything
		// `Survey` left read-only. The store is read through `Alive`,
		// `HasComponent` and `GetComponent`, which are `const` and take no
		// affinity; the run walks that do are all in `Survey`, and building a
		// world - which is what a join does - is in `Publish`'s serial pass.
		void PublishOne(Lane &lane, ClientId handle, Client &client, ecs::Store &store, uint64_t tick);

		// How many lanes to publish `clients` steady-state clients through.
		//
		// One below `AuthoritySettings::ParallelClientThreshold`, and otherwise
		// the pool plus the calling thread, capped at the client count.
		size_t LanesFor(size_t clients) const;

		// Folds every lane's phase timings into the frame graph and the metrics
		// sink, on the thread that owns both.
		void ReportPhases(size_t lanes);

		void BuildComponents(Lane &lane, ecs::Store &store, Client &client, Delta &delta, uint64_t tick);
		void Prioritise(Lane &lane, ClientId client, uint64_t tick);
		Placement Pack(Lane &lane, Client &client, const Delta &delta, size_t messageLimit);
		void Record(Lane &lane, Client &client, const Placement &placed, uint64_t tick);
		void EmitStructure(Client &client, const Structure &structure);

		// Asks `Refinement` about the rows near enough to the front to contend,
		// and puts that window back in order.
		//
		// A template on the comparator rather than a `std::function` of it: this
		// runs per client per tick and the comparator is the hot part of the
		// sort beside it. It is instantiated only in `Authority.cpp`, where
		// `Prioritise` is its one caller.
		//
		// @param reachable How far into `Order` the sort put rows in order.
		// @param spend     The tick's byte budget. The window comes from adding
		//        up what the ordered rows really encode to, because the sort's
		//        own bound assumes rows that are all as short as the shortest.
		template <class Before>
		void Refine(Lane &lane, ClientId client, size_t reachable, size_t spend, const Before &before);

		// Builds this tick's audit for one client, if one is due.
		//
		// Emitted after the delta and never before it, so the message the byte
		// budget turns away first is the one whose loss costs nothing.
		void EmitAudit(Lane &lane, const ecs::Store &store, ClientId handle, Client &client, uint64_t tick);

		// Takes a client's answer to an audit, having decided the server agrees
		// it asked the question.
		bool Dispute(Client &into, const replication::Disputed &disputed);

		AuthoritySettings Settings_;
		std::function<bool(ClientId, ecs::Entity, const ecs::Store &)> Interest;
		std::function<float(ClientId, ecs::Entity)> Priority;

		// The expensive half of the score, asked only about the rows in
		// contention. See `SetPriorityRefinement`.
		std::function<float(ClientId, ecs::Entity, float)> Refinement;
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

		// The subset of `Lane::Visible` the preface predicate claimed, refilled
		// per join rather than per publish because that is the only moment it is
		// asked.
		//
		// **On the authority rather than on a lane, and that is the invariant
		// rather than an oversight.** A join is built on the thread that owns
		// the store - see `Publish` - so there is exactly one of these in flight
		// at a time and a copy per lane would be a copy nothing ever fills.
		std::vector<ecs::Entity> Preceding;

		std::vector<uint64_t> Bearing;

		// The signed slots this tick, as indices into `Signatures`.
		//
		// Built by `Resign`'s first pass so the second is a flat parallel range
		// rather than a walk that skips most of what it visits.
		std::vector<size_t> ResignWork;

		std::vector<ecs::ComponentId> Resolved;

		// What each declared slot resolves to this tick, indexed by slot.
		//
		// **`Resolved` is compacted and this is not, which is the whole point.**
		// `Detection`, `Signatures` and `Suppressors` are keyed by slot, so a pass
		// over any of them cannot use `Resolved`'s indices - `ResolvedSuppressors`
		// carries the same warning.
		//
		// This is the whole of what `Publish`'s per-client loop needs to know
		// about a component, which is what lets that loop take the registry's
		// process-wide mutex **zero** times. See `Crossing`.
		std::vector<Crossing> Crossings;

		// The same list as names, which is what an audit puts on the wire. Kept
		// beside `Resolved` rather than derived from `Components`, because a
		// leaf's ordinal is a position in both and only a shared filling pass
		// keeps them the same positions.
		std::vector<core::Name> ResolvedNames;

		// The lanes, one per thread a publish may use. Never shrunk, so a tick
		// with fewer clients keeps the buffers the busiest tick grew.
		std::vector<Lane> Lanes;

		// The live clients whose publish is neither a join nor a chunk of one,
		// as indices into `Clients`, in ascending index order.
		//
		// Built by the serial pass and spread across the lanes by the parallel
		// one. Ascending, because the bytes a client receives may not depend on
		// which lane it landed in and the order it is *listed* in is the one
		// thing about that split a reader can check.
		std::vector<uint32_t> Steady;
	};
}
