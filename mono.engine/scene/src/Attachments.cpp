#include <engine/ecs/Store.hpp>
#include <engine/scene/Attachments.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>

#include <utility>
#include <vector>

namespace engine::scene {

	namespace {
		// The part an attachment hangs off, or a null entity.
		//
		// **One step and not a walk, which is the whole of why attachments do not
		// nest.** Following a chain would make this pass order-dependent - an
		// attachment resolved against another attachment's *resolved* frame reads
		// a value that may or may not have been written yet, depending on which
		// archetype the store visited first. `Attachments.hpp` carries the
		// argument; this function is where the rule is actually kept.
		//
		// A `Transform` is the test rather than a class check, because that is
		// what is actually needed - anything with a place in the world can carry
		// an attachment, and asking `IsA("BasePart")` would refuse a `Model` root
		// that legitimately has one.
		const Transform *ParentPlacement(const ecs::Store &store, ecs::Entity attachment) {
			const ecs::Entity parent = store.ParentOf(attachment);
			if (parent == ecs::NULL_ENTITY) {
				return nullptr;
			}
			return store.Get<Transform>(parent);
		}

		// Whether two frames are the same value.
		//
		// **A free function because `core::CFrame` has no `operator==`**, and
		// giving it one is not this file's call: an equality on a rotation
		// invites `q` and `-q` - the same orientation, opposite quaternions - to
		// compare unequal, and every caller would inherit that surprise. Here
		// the two sides are the same product of the same inputs, so bitwise is
		// exactly the question being asked.
		bool Same(const core::CFrame &left, const core::CFrame &right) {
			return left.Position == right.Position && left.QuaternionX == right.QuaternionX &&
				   left.QuaternionY == right.QuaternionY && left.QuaternionZ == right.QuaternionZ &&
				   left.QuaternionW == right.QuaternionW;
		}
	}

	core::CFrame ResolveAttachment(const ecs::Store &store, ecs::Entity attachment) {
		const Attachment *point = store.Get<Attachment>(attachment);
		if (point == nullptr) {
			return core::CFrame{};
		}

		const Transform *placement = ParentPlacement(store, attachment);
		if (placement == nullptr) {
			// An attachment on nothing keeps its local frame, which is what makes
			// it usable as a bare point in space.
			return point->Frame;
		}
		return placement->Frame * point->Frame;
	}

	size_t ResolveAttachments(ecs::Store &store) {
		size_t resolved = 0;

		// **`Each` and not `EachBatchParallel`, and the difference is the parent
		// lookup.** A batched walk is handed columns and no entity, so there is
		// no handle to ask `ParentOf` about - and the parent's `Transform` lives
		// in a different archetype from the attachment's row, so the load is a
		// random access whichever way this is written. A world with tens of
		// thousands of attachments would want the parent's frame denormalised
		// onto the row by whatever writes the parent; a world with hundreds,
		// which is what a beam-and-emitter scene has, does not.
		//
		// **Stated rather than measured**, which is the honest label: the
		// crossover has not been read and the loop is not on the frame's critical
		// path today. `engine.effects.bench.particles` is where it would show up
		// if it were.
		//
		// **What moved, gathered before anything is written.**
		//
		// This used to write straight through the reference `Each` hands out,
		// which is a direct memory write and not a reported one - so
		// `Attachment.WorldCFrame` and `WorldPosition` could never fire
		// `.Changed`. A script waiting on one waited for ever while the value it
		// was watching moved every frame, which is the worst shape a signal bug
		// has: the property reads correctly, so nothing looks wrong until you
		// notice the callback never ran.
		//
		// **Only the rows that actually moved, which is why the compare is here
		// and not a `MarkAllChanged`.** Reporting every attachment every frame
		// would advance the world's change counter for ever, and two gates are
		// built on an unchanged counter meaning nothing authored has happened -
		// `physics`'s static broadphase and `gui`'s compile. `Store::GetUnobserved`
		// carries that argument at length; this is the other side of it.
		std::vector<std::pair<ecs::Entity, core::CFrame>> moved;

		store.Each<const Attachment>([&store,
									  &resolved,
									  &moved](ecs::Entity entity, const Attachment &point) {
			const Transform *placement = ParentPlacement(store, entity);
			const core::CFrame world = placement == nullptr ? point.Frame : placement->Frame * point.Frame;
			resolved++;

			// Exact rather than tolerant. This is a cache of a product the same
			// two inputs produce bit for bit, so anything but equality would be
			// a threshold below which a signal is silently dropped.
			if (!Same(world, point.WorldFrame)) {
				moved.emplace_back(entity, world);
			}
		});

		// **Through `GetMutable`, deliberately.** It is the call that reports a
		// write, which is the whole point of this shape - see above.
		for (const auto &[entity, world] : moved) {
			if (Attachment *point = store.GetMutable<Attachment>(entity)) {
				point->WorldFrame = world;
			}
		}

		return resolved;
	}

	ecs::ClassId AttachmentClass() {
		// Through `PartClass` for `CameraClass`'s reason: one registration of the
		// whole tree, whichever class a caller asks for first.
		EnsureClassTree();
		return ecs::Classes::Find(core::Name("Attachment"));
	}
}
