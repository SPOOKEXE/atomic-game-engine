#pragma once

// arch-waiver public-header: forward replication API. Replication hosts retain
// this complete audit contract for diagnostics and tooling.

// Anti-entropy over the replicated world: what the two ends hash, and how much
// of it they hash at a time.
//
// **Deltas are the fast path and this is the audit, and they are not two ways
// to do one job.** A delta says what moved. Nothing in a delta can say what
// quietly failed to move - a creation rolled back by a refused message, a value
// confirmed and never written, a tick that never completed - and every one of
// those was chased one cause at a time. The audit finds the whole class
// generically: the server hashes a group of the state a client has already
// acknowledged, sends the digest, and takes back the groups that disagreed.
//
// **The audit only ever catches *stale* divergence, and that shapes everything
// here.** Anything genuinely moving is already being corrected by the delta
// path, so a group holding an unconfirmed row is excluded rather than compared;
// what is left is state both ends should agree on exactly, and a disagreement
// in it is by definition not urgent. So the cadence is slow, the slice is
// small, and the repair rides the recovery walk that already exists.
//
// **The hash is `assets::HashTree`'s rather than a third one.** Its interiors
// are tagged and its leaf count is sealed into the root, which is what makes a
// *missing* entity a different digest rather than a matching prefix - and a
// missing entity is half of what this exists to find.
//
// @tier L12 · shared

#include <engine/assets/ContentHash.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/ecs/TypeDescriptor.hpp>

#include <cstdint>
#include <span>

namespace engine::replication {

	// How much of the world one audit covers, and how often one goes out.
	//
	// **A rotating slice, because that bounds three costs with one number.**
	// The hashing, the wire and the repair are all proportional to how much is
	// audited at once, so a slice small enough for a datagram is small enough
	// for the other two - and it is the same rotation the priority ordering
	// already uses. A world twice the size is audited half as often rather than
	// twice as expensively.
	//
	// **This is also the whole of the rate limit**, because a client cannot ask
	// for a repair larger than the slice it was asked about. See
	// `Authority::Receive`.
	//
	// @since v0.15
	struct AuditSettings {
		// Whether the audit runs at all.
		//
		// **Off by default, and the reason is a property rather than caution
		// about the code.** `replication/AGENTS.md` says a quiet world sends
		// nothing, and anti-entropy is exactly the thing that has to speak on a
		// world at rest - a value stranded on a still world is what no delta
		// would ever report. Those two cannot both hold, and which one a host
		// wants is the host's to say: `mono.server` turns this on, and a caller
		// measuring its own traffic against a world it knows is idle should
		// know it did.
		bool Enabled = false;

		// How many entities one group covers.
		//
		// The granularity of a *repair* rather than of the detection: a
		// disagreement anywhere in a group re-offers every value in it. Smaller
		// is a cheaper repair and a slower sweep.
		uint32_t EntitiesPerGroup = 16;

		// How many groups one audit carries.
		//
		// The message is trimmed to fit `AuthoritySettings::ChunkBytes`
		// whatever this says, so a world too large for the slice is audited
		// more slowly rather than fragmented.
		uint32_t GroupsPerAudit = 2;

		// How many ticks apart two audits are.
		//
		// **Eight rather than one, and the reasoning is the whole proposal.**
		// What this finds is stale by definition, so paying for it every tick
		// buys latency on a problem with no deadline.
		uint64_t EveryTicks = 8;
	};

	// Which end is hashing, and therefore whether the value has crossed yet.
	//
	// **The authority holds full precision and a replica holds what a decode
	// produced**, so hashing the two as they sit would disagree for every
	// component with a compact wire form. The authority puts its value through
	// the same encode-and-decode `Authority::Capture` puts a join snapshot
	// through, and both ends then hash the encoding of the value a replica
	// holds - one expression over one input, so agreement is by construction
	// rather than by the quantiser happening to be idempotent.
	//
	// @since v0.15
	enum class AuditSide : uint8_t {
		// The value has not crossed a wire and is hashed as the far side would
		// hold it.
		Authority,

		// The value arrived through a decode and is hashed as it stands.
		Replica,
	};

	// The digest of one group's replicated state.
	//
	// One leaf per `(entity, component)` pair present, in the order the
	// arguments give - so a component missing at one end, an entity missing at
	// one end and a value differing at one end are three different roots.
	//
	// @param store      The world to read.
	// @param components The components to hash, in the order both ends agreed.
	// @param entities   The entities to hash, in the order both ends agreed.
	// @param side       Whether these values have crossed a wire yet.
	// @return The root of a `assets::HashTree` over the leaves.
	// @since v0.15
	assets::ContentHash AuditDigest(
		const ecs::Store &store,
		std::span<const ecs::ComponentId> components,
		std::span<const ecs::Entity> entities,
		AuditSide side
	);
}
