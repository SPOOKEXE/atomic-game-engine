#pragma once

// The client's half: what arrives, and what it does to the local world.
//
// **A replica is always some amount behind and sometimes wrong.** Correcting it
// is the normal case rather than an error path, and nothing here treats a
// correction as a fault.
//
// **A snapshot is reassembled before it is applied.** Chunks arrive out of
// order and some do not arrive at all; a client that applied them as they came
// would spend the join holding a world that is part old and part new. The
// reassembly buffer is sized from the total the first chunk declares and
// refuses anything claiming to run past it.
//
// **A join may arrive as two blobs, and only the second one ends it.** A server
// that declared an `Authority::SetPreface` sends what it wants seen first as its
// own snapshot, finished before the world's begins; `Prefaced` says that has
// landed and `Joined` still says the world has.
//
// **Reconciliation needs no cross-machine determinism.** The client drifting is
// expected; correcting the drift is the mechanism. Nothing here may assume the
// two machines compute the same floats.
//
// @tier L12 · shared

#include <engine/ecs/Store.hpp>
#include <engine/replication/Audit.hpp>
#include <engine/replication/Protocol.hpp>

#include <cstdint>
#include <functional>
#include <span>
#include <unordered_set>
#include <vector>

namespace engine::replication {

	// Why a message was refused.
	//
	// A `bool` was enough while the only caller was a test. Once a client
	// disconnects on a protocol error, "malformed" and "for a tick I already
	// applied" are different operational facts and only one of them is a
	// reason to drop the connection.
	//
	// @since v0.3
	enum class ApplyStatus : uint8_t {
		// Applied, or accepted as a chunk that is not the last.
		Ok,

		// The bytes are not a message this build reads.
		Malformed,

		// Well formed, and about a tick this replica has already passed. Not an
		// error: an unreliable transport reorders, and the newer state is
		// already applied.
		Stale,

		// A chunk that does not fit the snapshot being reassembled.
		BadChunk,

		// The reassembled snapshot could not be restored.
		BadSnapshot,

		// Names a component this build has not registered. The world would come
		// back narrower than it was sent, so it does not come back.
		UnknownComponent,
	};

	// Returns a stable, human-readable name for an apply status.
	//
	// @param status The status to name.
	// @return A view valid for the lifetime of the process.
	const char *Describe(ApplyStatus status);

	// Applies what the server sends to a local world.
	//
	// @since v0.3
	class Replica {
	  public:
		// Takes one message and applies it.
		//
		// @param store   The local world.
		// @param message The bytes as they arrived.
		// @return Why it was refused, or `Ok`.
		ApplyStatus Receive(ecs::Store &store, std::span<const std::byte> message);

		// The last tick applied in full.
		//
		// **In full means two things and both are checked.** Every part of the
		// tick's delta arrived - see `Delta::Part` - and every value in them
		// reached a row this client holds. The server retires everything a tick
		// carried the moment this is acknowledged, so a tick that was short of
		// either is not one this may name.
		//
		// It skips rather than stalls. A tick with a part missing is passed
		// over as soon as a later complete one lands, because everything the
		// missing part carried is still unconfirmed and rides that later tick.
		//
		// Zero before the snapshot has finished arriving - a client that
		// acknowledged a tick it had not applied would stop the server sending
		// the thing it is still waiting for.
		//
		// @return The tick.
		uint64_t Applied() const {
			return Applied_;
		}

		// Whether the joining snapshot has finished arriving and been applied.
		//
		// @return `true` once the world is usable.
		bool Joined() const {
			return Joined_;
		}

		// Whether what the server put in front of the world has arrived.
		//
		// **The window between this and `Joined` is what a loading screen is
		// for**, and it is the whole point of there being two blobs: the
		// entities `Authority::SetPreface` claimed are in the store and drawable
		// while the world they cover is still crossing.
		//
		// Stays `true` across a re-snapshot, because it is a fact about what
		// this replica holds rather than about the join in progress.
		//
		// @return `true` once the preface has been applied. Always `false` for a
		//         server that declared no preface, which is one blob and no
		//         window.
		// @since v0.15
		bool Prefaced() const {
			return Prefaced_;
		}

		// Called at the instant the preface is applied, with the store it went
		// into.
		//
		// **Because the moment `Prefaced` names cannot be polled for.** One
		// `Connector::Poll` drains the socket and applies everything that was in
		// it, so the preface and the first of the world behind it routinely land
		// in the same call - and anything checking `Prefaced()` between polls
		// sees a replica that has already joined and a store that already holds
		// the world. The window is real on the wire and invisible from outside
		// it. This is the only place it is observable.
		//
		// Runs before anything of the world behind the preface has been applied,
		// and once per preface. Replaced rather than added to.
		//
		// @param callback What to run, or `{}` to stop listening.
		// @since v0.15
		void OnPreface(std::function<void(ecs::Store &)> callback) {
			Preface_ = std::move(callback);
		}

		// How much of the joining snapshot is still missing, in bytes.
		//
		// @return The outstanding byte count, zero once joined.
		size_t SnapshotOutstanding() const;

		// The message that tells the server what has been applied.
		//
		// @return The encoded `Applied` message, or empty before the join.
		std::vector<std::byte> Acknowledge() const;

		// The answer to the last audit, if it found anything.
		//
		// **Drained rather than repeated, and the server would refuse a repeat
		// anyway.** An audit may be answered once - see `Authority::Receive` -
		// so a replica that kept offering the same answer would be producing
		// upstream traffic that is by construction thrown away.
		//
		// Empty is the normal case and the one to expect: it means the last
		// audit agreed, or none has arrived.
		//
		// @return The encoded `Disputed` message, or empty when there is
		//         nothing to say.
		// @since v0.15
		std::vector<std::byte> Dispute();

		// Entities the server said to forget.
		//
		// **Not destroyed.** They are out of view, not gone - a client that
		// destroyed them would be wrong about the world the moment they came
		// back. The caller decides what stopping drawing them means; this only
		// says which. See `Structure::Forgotten`.
		//
		// @return The entities, valid until the next `Receive`.
		std::span<const ecs::Entity> Forgotten() const {
			return Forgotten_;
		}

		// Clears the forget list, once the caller has acted on it.
		void ClearForgotten();

		// What this replica has seen.
		//
		// @since v0.3
		struct Statistics {
			// Snapshots applied. More than one means the server decided this
			// client had fallen too far behind to catch up with deltas.
			//
			// The world blob only. A preface is half a join rather than a
			// snapshot of anything, and counting it here would make every
			// re-snapshot look like two.
			uint64_t Snapshots = 0;

			// Prefaces applied, which is one per join on a server that declares
			// one and zero on a server that does not.
			//
			// @since v0.15
			uint64_t Prefaces = 0;

			// Deltas applied.
			uint64_t Deltas = 0;

			// Deltas applied in part, because a value named a row this client
			// does not hold yet.
			//
			// Ordinary while a creation is in flight on the reliable channel and
			// the values that follow it are not. A figure that keeps climbing on
			// a link losing nothing is a server naming entities it never
			// announced.
			uint64_t Partial = 0;

			// Ticks a part of never arrived, so they were never acknowledged.
			//
			// **The number that says D00013's hole is being hit rather than
			// argued about.** A tick's delta goes out as however many
			// independently applicable messages it takes; losing one leaves the
			// tick short of a part, and a tick with a part missing is not
			// acknowledged - so every value it carried stays unconfirmed on the
			// server and comes back on the next tick. One of these costs one
			// tick of acknowledgement and nothing else.
			//
			// A figure that tracks the tick count is a link losing more than
			// the delta path can absorb: no tick ever completes, nothing is
			// acknowledged, and the server re-snapshots this client after
			// `AuthoritySettings::ResnapshotAfterTicks`. That is the bound on
			// the wait and it is deliberately the one that already existed.
			uint64_t Incomplete = 0;

			// Structural messages applied.
			//
			// Counts resends too, and deliberately: a figure well above the
			// number of entities that came and went is the reliable channel
			// covering real loss, which is the one place that shows.
			uint64_t Structures = 0;

			// Messages refused as malformed.
			uint64_t Malformed = 0;

			// Messages about a tick already passed. Not an error - an
			// unreliable transport reorders - but a figure that climbs is a
			// link delivering more late than useful.
			uint64_t Stale = 0;

			// Parents named by a delta that never turned up.
			//
			// **A number that should stay at zero.** `ecs.Hierarchy` crosses as
			// a parent handle and the tree is rebuilt from it, so a parent that
			// has not arrived within `HOLD_DELTAS` leaves its child unparented
			// - visible in the wrong place rather than invisible, which is the
			// better of the two, but wrong either way. A figure that climbs
			// means the sender is emitting children of rows this replica is not
			// being sent: an interest filter that admits one half of a pair, or
			// two builds that disagree about what is replicated.
			//
			// Also counted when the wait list is full, which is the same fault
			// arriving faster.
			//
			// @since v0.18
			uint64_t Orphaned = 0;

			// Audits checked against this replica's own copy.
			//
			// @since v0.15
			uint64_t Audits = 0;

			// Groups an audit found this replica disagreeing about.
			//
			// **A steady zero is the state the delta path claims to keep this
			// replica in**, and anything else is divergence that no ordinary
			// message would ever have reported - see `Audit.hpp`. It costs one
			// small message upstream and the server resends the group.
			//
			// @since v0.15
			uint64_t Mismatched = 0;
		};

		// What this replica has seen.
		//
		// @return The statistics.
		const Statistics &Stats() const {
			return Stats_;
		}

	  private:
		ApplyStatus Apply(ecs::Store &store, const SnapshotChunk &chunk);
		ApplyStatus Apply(ecs::Store &store, const replication::Delta &delta);
		ApplyStatus Apply(ecs::Store &store, const replication::Structure &structure);
		ApplyStatus Check(const ecs::Store &store, const replication::GroupSignatures &signatures);

		// Which parts of one tick's delta have arrived.
		//
		// **One tick rather than a ring, and that is a decision.** A tick that
		// never completes is superseded rather than waited for: the values its
		// missing part carried are still unconfirmed on the server, so they are
		// offered again on the next tick, and finishing the older tick
		// afterwards would confirm nothing newer than finishing the newer one.
		// A ring would be a second thing to keep in step for an answer that is
		// never fresher.
		struct Parts {
			// The tick being counted, and whether one is being counted at all.
			uint64_t Tick = 0;
			bool Counting = false;

			// Which parts have arrived, indexed by `Delta::Part` and bounded by
			// `MAXIMUM_PARTS`. A set of positions rather than a count of
			// arrivals, because the unreliable channel delivers a part twice as
			// readily as once and a count would read the second copy as
			// progress.
			std::vector<bool> Held;

			// The part that carried `Delta::Final`, and whether one has.
			uint16_t Last = 0;
			bool Ended = false;

			// Whether every part from zero to `Last` is in `Held`. Kept so that
			// abandoning this tick can tell a lost part from a tick that was
			// complete and merely named a row this client does not hold.
			bool Whole = false;
		};

		// Withholds an arriving entity's parent, and gives it back.
		//
		// **An entity is not in the world until the tick that made it is
		// whole.** A spawn crosses as a `Structure` naming the entity and a
		// delta carrying its components, and a delta is split by the bandwidth
		// budget - so the row that puts it in `Workspace` can arrive a tick
		// before the row that says where it is. Between the two, the walk in
		// `scene::SyncRendered` reaches it, and it draws at the identity: a part
		// at the origin for a tick, then a jump to its real place.
		//
		// So a created entity is unparented as soon as it is linked, its parent
		// is kept here, and it is put back when the tick completes - which is
		// this module's version of the rule every Roblox script already follows:
		// set the properties, then set `Parent`.
		//
		// **Unparenting through `Store::SetParent` rather than by writing the
		// component**, because the link is two rows: the child names its parent
		// and the parent names its first child. Writing one of them would leave
		// a parent claiming a child that does not claim it back, and the walk
		// descends from the parent - so the object would still draw and this
		// would fix nothing.
		//@{
		void HoldArrivals(ecs::Store &store);
		void ReleaseArrivals(ecs::Store &store);
		//@}

		// One entity waiting for the rest of its tick.
		struct Arrival {
			ecs::Entity Entity;

			// Where it goes once the tick is whole, or null while the delta
			// that says so has not arrived.
			ecs::Entity Parent;

			// Which delta this arrived on, so a stream that never completes
			// cannot hide an object for ever. See `HOLD_DELTAS`.
			uint64_t Since = 0;
		};

		// How many deltas an entity may be held for before it is shown anyway.
		//
		// **A bound rather than a promise.** The release is meant to happen when
		// a tick completes; if partial deltas keep arriving and none ever does,
		// holding for ever would turn a bandwidth problem into content that is
		// simply missing - which is the worse of the two failures, because
		// nothing says it happened.
		static constexpr uint64_t HOLD_DELTAS = 8;

		// The most entities that may be waiting at once.
		//
		// **A bound on the list as well as on each entry's age**, because the
		// two fail differently: an entry ages out because its tick never
		// completed, and the list grows because many of them did not. A sender
		// emitting parents this replica will never have - an interest filter
		// that hides them, a build that disagrees - would otherwise be a vector
		// that only grows.
		//
		// Generous against what a real tick carries: a delta names a few
		// hundred rows and almost none of them defer.
		static constexpr size_t MAXIMUM_DEFERRED = 4096;

		// Notes one part's arrival and says whether its tick is now held whole.
		//
		// @param delta The part that has just been applied.
		// @return `true` when every part the sender emitted for that tick is
		//         here, so the tick may be acknowledged.
		bool Count(const replication::Delta &delta);

		// The snapshot being reassembled. Sized from the total the first chunk
		// declares, and every later chunk is checked against it.
		std::vector<std::byte> Snapshot;
		std::vector<bool> Received;
		size_t Outstanding = 0;
		uint64_t SnapshotTick = 0;

		// Which of the join's two blobs is in the buffer above. Part of its
		// identity beside the tick, because both blobs carry the same one.
		SnapshotStage Stage = SnapshotStage::World;
		bool Assembling = false;

		std::vector<ecs::Entity> Forgotten_;

		// The answer to the last audit, waiting for `Dispute` to take it.
		replication::Disputed Disputing_;

		// Entities created and not yet shown. A vector rather than a map: a
		// tick's spawns are a handful, and this is walked whole or not at all.
		std::vector<Arrival> Arriving_;
		Parts Counting;
		uint64_t Applied_ = 0;
		bool Joined_ = false;
		bool Prefaced_ = false;

		// Fired the moment the preface lands. See `OnPreface`.
		std::function<void(ecs::Store &)> Preface_;

		Statistics Stats_;
	};
}
