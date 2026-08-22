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
	// A `void(Store &)` so it registers as an ordinary system, which is what lets
	// every host that owns a world register the same pass rather than each
	// growing its own.
	//
	// **Every host registers it, and the phase follows from what that host reads
	// the field for.** It is not presentation-only, and treating it as such is
	// what left the two sides of a session disagreeing until v0.19:
	//
	//   - `mono.client`'s scripted and presented worlds run it in `PreSimulation`
	//     - `effects::RefreshEmitters` wants a spawn point before the tick moves
	//     anything - and again in `PreRender`, where `client::CollectLights`
	//     places a lamp parented to an attachment.
	//   - `mono.client`'s *replica* runs it in `PreRender` alone. A replica ticks
	//     no simulation, and its lamps were lighting the world origin without it.
	//   - `mono.server` runs it in `PostSimulation`, where the tick's transforms
	//     are final. Nothing there draws; what the authority needs is the
	//     *reported* write, which is what makes `WorldCFrame` and `WorldPosition`
	//     fire their change signal for a server script.
	//
	// **Two production readers of the field, and beams are not among them.**
	// `effects::ParticleSystem` places a spawn from it and `client::CollectLights`
	// places a lamp; `effects::BuildRibbons` calls `ResolveAttachment` below per
	// beam instead, because a ribbon is built from two named attachments rather
	// than from a walk over all of them. A caller that needs a world frame
	// *during* the tick - a weld, a constraint - should call that one rather than
	// move this pass.
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
