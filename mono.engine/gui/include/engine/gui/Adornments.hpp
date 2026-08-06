#pragma once

// Which instance an adornment is about, and whether it may be drawn.
//
// **An adornment is a description and this is the half of it `gui` owns.** A
// `SelectionBox` says what to outline, in what colour and how solid; turning
// that into geometry needs the adornee's `CFrame` and stud extent, which are
// `scene::Transform` and `scene::Bounds`. This module links neither and
// `gui/AGENTS.md` refuses the edge.
//
// That is not a gap, it is `D00022`'s split arrived at again: **whoever draws
// an adornment has both operands and this module has one.** So what is here is
// the tree logic — the two questions a drawer would otherwise each answer
// differently:
//
//   - *what is this about?* An unset `Adornee` means "my parent", which is what
//     makes a `SelectionBox` usable by parenting it to the thing it outlines
//     and setting nothing else. Resolved once here rather than at each drawer.
//   - *may it be drawn at all?* An adornment lives in `Workspace`, `StarterGui`
//     or a player's `PlayerGui` — the same containment `Layout` applies to a
//     `SurfaceGui`, and for the same reason: an adornment hangs off something
//     in the world, so the world is a legal home for it.
//
// A drawer that skipped these would be a second answer to both, and the two
// would disagree the first time one was fixed.
//
// @tier L7 · shared

#include <engine/ecs/Entity.hpp>

#include <functional>

namespace engine::ecs {
	class Store;
}

namespace engine::gui {

	// What an adornment is drawn around.
	//
	// **`Adornee` when set, the parent when not**, which is Roblox's rule and
	// the reason an unset `Adornee` is a meaningful value rather than an
	// incomplete one.
	//
	// @param store      The world.
	// @param adornment  The adornment instance.
	// @return The instance it adorns, or `ecs::NULL_ENTITY` when it has no
	//         `Adornment` at all and when its parent is nothing either.
	ecs::Entity AdorneeOf(const ecs::Store &store, ecs::Entity adornment);

	// Whether an adornment is somewhere it may be drawn from.
	//
	// The same containment `Layout` applies to the world-attached collectors:
	// `Workspace`, `StarterGui` or a player's `PlayerGui`. An adornment under a
	// `Part` is *not* contained — the part is what it adorns, not where it
	// lives — which is the distinction that would otherwise be discovered by
	// somebody wondering why their handle vanished when they tidied the tree.
	//
	// @param store     The world.
	// @param adornment The adornment instance.
	// @return Whether a drawer should draw it.
	bool AdornmentDrawn(const ecs::Store &store, ecs::Entity adornment);

	// Visits every adornment a drawer should draw, in `ZIndex` order.
	//
	// **Ordered here rather than by each drawer**, because two drawers sorting
	// independently is two answers to what covers what. Stable within a
	// `ZIndex`, so adornments that tie keep the order the store held them in
	// rather than swapping between frames as archetypes reshuffle.
	//
	// @param store The world.
	// @param body  Called as `body(adornment, adornee)`. The adornee is
	//        resolved, so a body never repeats `AdorneeOf`.
	// **A mutable store even though this only reads**, because `Store::Each` is
	// not const-callable: iteration defers structural changes and the deferral
	// is state. Taking a `const Store &` here would mean copying the entity list
	// out first, which is the same walk with an allocation in front of it.
	void EachAdornment(
		ecs::Store &store, const std::function<void(ecs::Entity adornment, ecs::Entity adornee)> &body
	);
}
