#include <engine/ecs/Store.hpp>
#include <engine/scene/Attachments.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>

namespace engine::scene {

	namespace {
		// The part an attachment hangs off, or a null entity.
		//
		// **One step and not a walk, which is the whole of why attachments do not
		// nest.** Following a chain would make this pass order-dependent — an
		// attachment resolved against another attachment's *resolved* frame reads
		// a value that may or may not have been written yet, depending on which
		// archetype the store visited first. `Attachments.hpp` carries the
		// argument; this function is where the rule is actually kept.
		//
		// A `Transform` is the test rather than a class check, because that is
		// what is actually needed — anything with a place in the world can carry
		// an attachment, and asking `IsA("BasePart")` would refuse a `Model` root
		// that legitimately has one.
		const Transform *ParentPlacement(const ecs::Store &store, ecs::Entity attachment) {
			const ecs::Entity parent = store.ParentOf(attachment);
			if (parent == ecs::NULL_ENTITY) {
				return nullptr;
			}
			return store.Get<Transform>(parent);
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
		// no handle to ask `ParentOf` about — and the parent's `Transform` lives
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
		store.Each<Attachment>([&store, &resolved](ecs::Entity entity, Attachment &point) {
			const Transform *placement = ParentPlacement(store, entity);
			point.WorldFrame = placement == nullptr ? point.Frame : placement->Frame * point.Frame;
			resolved++;
		});

		return resolved;
	}

	ecs::ClassId AttachmentClass() {
		// Through `PartClass` for `CameraClass`'s reason: one registration of the
		// whole tree, whichever class a caller asks for first.
		PartClass();
		return ecs::Classes::Find(core::Name("Attachment"));
	}
}
