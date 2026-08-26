#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Constraints.hpp>
#include <engine/scene/Part.hpp>

namespace engine::scene {

	namespace {
		// The part an attachment is anchored to, or a null entity.
		//
		// A `Transform` is the test rather than a class check, which is the rule
		// `ParentPlacement` already keeps in `Attachments.cpp`: anything with a
		// place in the world can carry an attachment, and asking `IsA("BasePart")`
		// would refuse a `Model` root that legitimately has one.
		ecs::Entity BodyUnder(const ecs::Store &store, ecs::Entity attachment) {
			if (attachment == ecs::NULL_ENTITY) {
				return ecs::NULL_ENTITY;
			}
			const ecs::Entity parent = store.ParentOf(attachment);
			if (parent == ecs::NULL_ENTITY || store.Get<Transform>(parent) == nullptr) {
				return ecs::NULL_ENTITY;
			}
			return parent;
		}
	}

	bool IsRigidJoint(const Constraint &joint) {
		for (size_t axis = 0; axis < CONSTRAINT_AXES; axis++) {
			if (joint.Linear[axis] != ConstraintMotion::Locked ||
				joint.Angular[axis] != ConstraintMotion::Locked) {
				return false;
			}
		}
		return true;
	}

	bool IsDrivenJoint(const Constraint &joint) {
		// A stiffness with nothing free to move is still a weld, so the modes and
		// the stiffness are one question rather than two.
		if (joint.Stiffness == 0.0f) {
			return false;
		}
		return !IsRigidJoint(joint);
	}

	bool
	JointBodies(const ecs::Store &store, const Constraint &joint, ecs::Entity &first, ecs::Entity &second) {
		first = BodyUnder(store, joint.Attachment0);
		second = BodyUnder(store, joint.Attachment1);
		return first != ecs::NULL_ENTITY;
	}

	ecs::ClassId ConstraintClass() {
		// Through the one tree registration, for `AttachmentClass`'s reason.
		EnsureClassTree();
		return ecs::Classes::Find(core::Name("Constraint"));
	}
}
