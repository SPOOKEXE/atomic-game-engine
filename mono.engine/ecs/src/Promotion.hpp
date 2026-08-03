#pragma once

// Turning a predicted entity into a server one.
//
// A replica mints predicted entities from the high half of the index space so
// that nothing it invents can collide with something the authority minted —
// see `EntityRange`. When the authority finally answers with the real entity,
// the local row has to stop being a guess and start being the real thing.
//
// **This is the primitive and not the policy.** *When* a predicted entity is
// promoted, which authoritative handle it is promoted to, and what happens to
// one the authority never confirms are all decisions belonging to the layer
// that predicts — and there is nothing predicting a spawn yet, because that
// wants a projectile, which wants physics and `Part`. `ROADMAP.md` says the
// design should not be guessed at before its consumer exists, so this file
// builds the operation and refuses to build the rule.
//
// Split out of `StoreState.cpp` because it is a self-contained job that touches
// four things at once — the directory, the row's own copy of its handle, the
// name maps and the instance tree — and a reader looking for any one of them
// should not have to read the rest of the storage to find it.
//
// @tier L3 · shared

#include "StoreState.hpp"

#include <engine/ecs/Entity.hpp>

#include <string_view>

namespace engine::ecs {

	// Rewrites a predicted entity's identity to an authoritative one.
	//
	// The row does not move: every component value, the archetype and the row
	// index are exactly what they were, and only the handle naming them changes.
	// That is the point — a promotion that rebuilt the entity would throw away
	// whatever the client had predicted, which is the state the prediction
	// existed to have.
	//
	// **What it fixes up, and what it cannot.** The directory, the row's own id
	// entry, the store's name maps and the instance hierarchy links around it
	// all follow the new handle. An `ecs::Entity` stored inside some *other*
	// component — a target, an owner, a field a game declared — does **not**,
	// because nothing in `TypeDescriptor` says which of a component's bytes are
	// entity handles and a byte-pattern search would rewrite an unrelated
	// integer that happened to match. Those handles keep the predicted value,
	// and the predicted index's generation is bumped as it is freed, so such a
	// handle reads as **dead** rather than as some other entity. A caller that
	// needs them to follow has to rewrite them itself, which it can do because
	// it is the layer that knows the component holds a handle.
	//
	// @param state         The world holding the predicted entity.
	// @param world         The store's name, for the refusal message.
	// @param predicted     The predicted handle, which must be live and in the
	//                      predicted range.
	// @param authoritative The handle it should answer to, which must be in the
	//                      authoritative range and not already live here.
	// @return `false` when either handle is in the wrong range, the predicted
	//         one is not live, the authoritative one already is, or the call
	//         came from inside an iteration.
	bool PromoteEntity(StoreState &state, std::string_view world, Entity predicted, Entity authoritative);
}
