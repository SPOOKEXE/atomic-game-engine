#pragma once

// Where an `Attachment` actually is, once a frame.
//
// **One flat pass, and that is the whole point of the design.** `scene`'s
// standing rule is that every transform is world space and nothing propagates
// it - `Components.hpp` opens with it - and an attachment is the one thing that
// has to be relative to something. The resolution is kept to a single loop over
// a single component type rather than becoming a transform hierarchy: an
// attachment's parent is a part, a part's transform is already world space, and
// therefore an attachment's world frame is one `CFrame` product with no chain to
// walk.
//
// **Attachments do not nest, and that is enforced rather than assumed.** An
// `Attachment` parented to another `Attachment` resolves against the *part* it
// eventually sits under, not against its immediate parent's resolved frame -
// because resolving against a resolved value is what makes a pass order-
// dependent, and an order-dependent pass over an archetype walk is a pass whose
// answer depends on which rows moved last. Roblox does not nest them either.
//
// @tier L7 · shared

#include <engine/core/types/CFrame.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Entity.hpp>

#include <cstddef>

namespace engine::ecs {
	class Store;
}

namespace engine::scene {

	// Fills every `Attachment::WorldFrame` from its parent part.
	//
	// A `void(Store &)` so it registers as an ordinary system, exactly as
	// `ResolveActiveCamera` does and for the same reason.
	//
	// **Runs in `PreRender` beside the other presentation-derived passes**, not
	// in the simulation: what reads a world frame is a beam, a trail and a
	// particle emitter, all of which are drawn rather than simulated. A caller
	// that needs an attachment's world frame *during* the tick - a weld, a
	// constraint - is asking for something this pass does not promise, and should
	// say so rather than moving this one earlier.
	//
	// An attachment whose parent is not a `BasePart`, or has no parent at all,
	// takes its local frame as its world frame. That is the useful state rather
	// than an error: it makes an attachment usable as a bare point in space,
	// which is what a beam between two world positions needs.
	//
	// @param store The world to resolve in.
	// @return How many attachments were resolved.
	size_t ResolveAttachments(ecs::Store &store);

	// The world-space frame of one attachment, resolved on the spot.
	//
	// **For the callers that cannot wait for the pass**, which is exactly two: a
	// script reading `attachment.WorldCFrame` on the frame it created one, and a
	// test. Everything on the draw path reads the resolved field, because doing
	// this per reader is the hierarchy walk the field exists to avoid.
	//
	// @param store      The world.
	// @param attachment The attachment instance.
	// @return Its world frame, or its local frame when it has no part parent.
	core::CFrame ResolveAttachment(const ecs::Store &store, ecs::Entity attachment);

	// The `Attachment` class id, registering the scene tree on first call.
	//
	// **Derives from `Instance` rather than from `PVInstance`**, which is the
	// same omission `Sound` makes and for a related reason: a `PVInstance` has a
	// `Transform`, and a `Transform` on an attachment would be a second opinion
	// about where it is beside the `CFrame` it already carries relative to its
	// parent. Roblox's `Attachment` derives from `Instance` too.
	//
	// @return The class id.
	ecs::ClassId AttachmentClass();
}
