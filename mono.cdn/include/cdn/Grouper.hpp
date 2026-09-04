#pragma once

// arch-waiver public-header: forward origin API. Content publication keeps this
// deterministic group planner available as a complete configuration contract.

// How assets are gathered into the groups that get streamed.
//
// A group is the unit that is compressed, streamed, prioritised and cancelled,
// and it has one defining property: **a group that lands makes something
// appear.** That is the whole of "the game progressively builds", and it is
// decided here - when the group is *built* - rather than by anything about how
// the bytes travel. A group holding half a scene's meshes and none of its
// textures has arrived and shown nothing.
//
// Both naive answers are bad and this exists because of them: one request per
// asset is thousands of round trips, and one archive for the game means nothing
// is usable until all of it has arrived.
//
// Three rules, applied in this order and no other:
//
// 1. **Self-sufficiency.** Assets that are needed together go together, and an
//    affinity is never split across groups. This outranks size.
// 2. **A deliberate size mix.** Within the bound, a few large assets and the
//    many small ones that belong with them. A group of only tiny assets is
//    dominated by per-request cost; a group of only huge ones has a terrible
//    time-to-first-usable.
// 3. **Priority.** Groups come out in the order the player will need them.
//
// This is policy and it lives with the origin, not with the format. What a
// bundle *is* - a root over sorted asset roots - is `Engine::assets`.
//
// @tier shared

#include <engine/assets/ContentHash.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace cdn {

	// The size envelope groups are assembled within.
	//
	// Starting points rather than measurements - the group bound is an open
	// question, and there is no number beside it yet.
	struct GroupPolicy {
		// What a group aims to weigh, uncompressed.
		uint64_t TargetBytes = 16ull * 1024 * 1024;

		// The ceiling a group is not packed past.
		//
		// Not a hard limit: rule 1 outranks it, so a single affinity heavier
		// than this becomes one oversized group rather than being split. That
		// case is reported rather than hidden - see Assembly::Oversized.
		uint64_t MaximumBytes = 32ull * 1024 * 1024;

		// Whether these can be used. Requires 0 < Target <= Maximum.
		bool IsValid() const;
	};

	// One asset offered for grouping.
	struct GroupCandidate {
		// The asset's root, from the manifest.
		engine::assets::ContentHash Root;

		// Its uncompressed length, which is what the envelope is measured in.
		uint64_t Bytes = 0;

		// What it is needed *with*. Assets sharing an affinity land in one
		// group and are never split - a mesh, its textures, its material and
		// its collider share one.
		//
		// Zero means "belongs with nothing in particular" and is the only value
		// that does not bind assets together. Two unrelated assets both left at
		// zero must not be forced into one group by that alone.
		uint32_t Affinity = 0;

		// When the player needs it; lower is sooner. A group takes the lowest
		// priority among its members, because a group is wanted as soon as its
		// most urgent member is.
		uint32_t Priority = 0;
	};

	// One assembled group.
	struct Group {
		// Member asset roots, sorted - the arrangement `Manifest::AddBundle`
		// will put them in anyway, done here so the two cannot disagree.
		std::vector<engine::assets::ContentHash> Assets;

		// The group's uncompressed weight.
		uint64_t TotalBytes = 0;

		// The lowest priority among its members.
		uint32_t Priority = 0;
	};

	// What Assemble produced, including what it could not do cleanly.
	struct Assembly {
		// The groups, in priority order.
		std::vector<Group> Groups;

		// How many groups exceeded GroupPolicy::MaximumBytes because one
		// affinity did.
		//
		// Reported rather than silently tolerated. A bound that is quietly
		// broken reads as a bound that held, and the first anyone hears of it is
		// a client stalling on a group it cannot stream in time.
		size_t Oversized = 0;
	};

	// Assembles assets into groups.
	//
	// Deterministic: the same candidates and the same policy give the same
	// groups, on any machine and in any build. Two origins that group the same
	// content differently would prepare and cache different bundles for it, and
	// nothing anywhere would report that they had stopped sharing.
	class Grouper {
	  public:
		// @param policy The size envelope. An invalid one falls back to the
		//        defaults rather than aborting, for the reason Chunker does.
		explicit Grouper(GroupPolicy policy = {});

		// The policy actually in use, after the validity fallback.
		const GroupPolicy &Policy() const {
			return Envelope;
		}

		// Groups the candidates.
		//
		// Every candidate appears in exactly one group. An empty input gives no
		// groups rather than one empty group, for the reason an empty chunk is
		// not a chunk.
		//
		// @param candidates The assets to group.
		// @return The groups in priority order, and the oversized count.
		Assembly Assemble(std::span<const GroupCandidate> candidates) const;

	  private:
		GroupPolicy Envelope;
	};
}
