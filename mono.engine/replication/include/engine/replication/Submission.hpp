#pragma once

// Writing a delta's values into a world, in either direction.
//
// **One function, because a delta going up is the same bytes as a delta coming
// down.** `Replica` has applied server→client deltas since v0.3; v0.13 added
// the other direction, where a client sends state for an entity it owns. The
// two differ in who is allowed to say it - which is a predicate - and in
// nothing else, so they share the write rather than having one each.
//
// @tier L12 · shared

#include <engine/core/Name.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/replication/Protocol.hpp>
#include <engine/replication/Replica.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace engine::ecs {
	class Store;
}

namespace engine::replication {

	// What writing a delta did.
	//
	// @since v0.13
	// A parent a delta named that a store could not link yet.
	//
	// @since v0.18
	struct DeferredParent {
		// The instance whose parent was named.
		ecs::Entity Child;

		// What it should hang from once that entity exists.
		ecs::Entity Parent;
	};

	// What applying one delta did.
	//
	// **A struct rather than a status alone**, because "it worked" and "every
	// entity it named still existed" are different questions and a caller that
	// resubscribes on the second needs to tell them apart.
	struct WriteOutcome {
		// How it went. `Ok` for a write that landed, in whole or in part.
		ApplyStatus Status = ApplyStatus::Ok;

		// Whether every entity the delta named was alive to receive it.
		//
		// A delta naming a row that has since been destroyed is ordinary rather
		// than wrong - the destroy and the value crossed - so this is reported
		// and not refused.
		bool Whole = true;

		// Values a filter would not let through.
		//
		// Zero whenever no filter was given. See `Authority::SetOwnership` for
		// what a non-zero figure means on an inbound delta.
		size_t Refused = 0;

		// Parents this delta named that could not be linked yet, in read order.
		//
		// **Not a failure and not to be dropped.** `ecs.Hierarchy` crosses as a
		// parent handle and nothing else, and a child's may arrive in the same
		// delta that creates its parent or one before it - so "the parent is not
		// here" is an ordinary tick. `Replica` keeps these and retries them,
		// bounded by `HOLD_DELTAS`, which is the bound it already holds an
		// arriving entity for.
		//
		// Empty on almost every delta: it costs a vector nobody allocates into.
		//
		// @since v0.18
		std::vector<DeferredParent> Deferred;
	};

	// Writes a delta's component values into a world.
	//
	// **A refused value is read and discarded rather than skipped**, which is
	// the one thing to know before touching this. A component's values are one
	// packed stream in the same order as its entity list, and only the type's
	// descriptor knows how many bytes one value occupies - so a filter that
	// dropped an entity without consuming its bytes would shift every value
	// after it onto the wrong row, quietly, and the symptom would be one
	// client's position arriving on another client's part.
	//
	// It is also why the filter is a parameter here rather than something a
	// caller applies to the `Delta` beforehand: stripping entities out of a
	// `ComponentDelta` is exactly that mistake.
	//
	// @param store The world to write into.
	// @param delta The values to write.
	// @param allow Optional. Called as `allow(component, entity)` for every
	//              value; an empty function permits everything, which is the
	//              server→client case where the sender is the authority.
	// @return What was written and what was not.
	// @since v0.13
	WriteOutcome WriteComponents(
		ecs::Store &store, const Delta &delta, const std::function<bool(core::Name, ecs::Entity)> &allow = {}
	);

	// Builds a delta carrying named components of named entities.
	//
	// **The client's half, and it is deliberately not a publisher.** `Authority`
	// streams: it tracks what each client knows, signs values to notice changes,
	// prioritises, budgets and splits across ticks, because it is answering for
	// a whole world and many clients. A client submitting the handful of things
	// it owns needs none of that - it knows exactly what it owns and it is
	// talking to one machine - and building a second streamer to say so would be
	// most of `Authority` again for a list that fits in a cache line.
	//
	// Every value present is included. There is no change detection, because the
	// alternative is a client deciding its own value has not moved and a server
	// left holding the last one it heard.
	//
	// An entity with none of the named components contributes nothing rather
	// than an empty row, and a component no entity has is left out entirely - so
	// a submission with nothing to say is a delta with no components in it,
	// which `Authority` refuses as empty rather than applies as a no-op.
	//
	// @param store      The world to read.
	// @param tick       The tick this state is for.
	// @param entities   What to send. The caller's owned set.
	// @param components Which components of them to send.
	// @return The delta, ready for `Connector::SubmitState`.
	// @since v0.13
	Delta BuildSubmission(
		const ecs::Store &store,
		uint64_t tick,
		std::span<const ecs::Entity> entities,
		std::span<const core::Name> components
	);
}
