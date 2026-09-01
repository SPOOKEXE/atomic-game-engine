// One component covering the constraint family, and the classes that name its
// members.
//
// The prototype cases are the load-bearing ones. The whole argument for a
// generic six-degree-of-freedom joint is that `Instance.new("HingeConstraint")`
// produces a hinge without a `Kind` field saying so, and that only works if the
// prototype rows are actually set.

#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Attachments.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Constraints.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.scene.constraints")

using engine::core::CFrame;
using engine::core::Name;
using engine::core::Vector3;
using engine::ecs::Classes;
using engine::ecs::Entity;
using engine::ecs::NULL_ENTITY;
using engine::ecs::Store;
using engine::scene::Attachment;
using engine::scene::AttachmentClass;
using engine::scene::Constraint;
using engine::scene::CONSTRAINT_AXES;
using engine::scene::ConstraintClass;
using engine::scene::ConstraintMotion;
using engine::scene::IsDrivenJoint;
using engine::scene::IsRigidJoint;
using engine::scene::JointBodies;
using engine::scene::JointInstance;
using engine::scene::RegisterSceneClasses;
using engine::scene::Transform;

namespace {
	// An attachment on a placed part, which is the only shape a joint anchors to.
	Entity MakeAnchor(Store &store, const char *name) {
		const Entity part = store.CreateInstance(Classes::Find(Name("Part")), name);
		store.Set(part, Transform{CFrame(Vector3(1.0f, 0.0f, 0.0f))});

		const Entity anchor = store.CreateInstance(AttachmentClass(), "Anchor");
		REQUIRE(store.SetParent(anchor, part));
		store.Set(anchor, Attachment{});
		return anchor;
	}

	const Constraint &Prototype(Store &store, const char *klass) {
		const Entity joint = store.CreateInstance(Classes::Find(Name(klass)), klass);
		const Constraint *row = store.Get<Constraint>(joint);
		REQUIRE(row != nullptr);
		return *row;
	}
}

TEST_CASE("the generic constraint default is every axis locked", "[scene][constraints]") {
	RegisterSceneClasses();
	Store store("constraints_test.weld");

	const Constraint &joint = Prototype(store, "Constraint");
	CHECK(IsRigidJoint(joint));
	CHECK_FALSE(IsDrivenJoint(joint));
}

TEST_CASE("each class's prototype is the joint that class names", "[scene][constraints]") {
	// The alternative to a `Kind` field, proved: seven classes, one component,
	// and the modes are what distinguishes them.
	RegisterSceneClasses();
	Store store("constraints_test.family");

	const Constraint &ball = Prototype(store, "BallSocketConstraint");
	CHECK_FALSE(IsRigidJoint(ball));
	for (size_t axis = 0; axis < CONSTRAINT_AXES; axis++) {
		CHECK(ball.Angular[axis] == ConstraintMotion::Free);
		CHECK(ball.Linear[axis] == ConstraintMotion::Locked);
	}

	const Constraint &hinge = Prototype(store, "HingeConstraint");
	CHECK(hinge.Angular[0] == ConstraintMotion::Free);
	CHECK(hinge.Angular[1] == ConstraintMotion::Locked);
	CHECK(hinge.Angular[2] == ConstraintMotion::Locked);

	const Constraint &prismatic = Prototype(store, "PrismaticConstraint");
	CHECK(prismatic.Linear[0] == ConstraintMotion::Free);
	CHECK(prismatic.Angular[0] == ConstraintMotion::Locked);

	const Constraint &cylindrical = Prototype(store, "CylindricalConstraint");
	CHECK(cylindrical.Linear[0] == ConstraintMotion::Free);
	CHECK(cylindrical.Angular[0] == ConstraintMotion::Free);

	const Constraint &rope = Prototype(store, "RopeConstraint");
	for (size_t axis = 0; axis < CONSTRAINT_AXES; axis++) {
		CHECK(rope.Linear[axis] == ConstraintMotion::Limited);
		CHECK(rope.Angular[axis] == ConstraintMotion::Free);
	}
	CHECK_FALSE(IsDrivenJoint(rope));

	// A spring is a rope with a stiffness, which is what the one-component
	// design buys: no new field, and the difference is a number an author can
	// also type.
	const Constraint &spring = Prototype(store, "SpringConstraint");
	CHECK(spring.Linear[0] == ConstraintMotion::Limited);
	CHECK(IsDrivenJoint(spring));
}

TEST_CASE("every constraint class descends from the base", "[scene][constraints]") {
	RegisterSceneClasses();

	REQUIRE(ConstraintClass().IsValid());
	for (const char *klass :
		 {"BallSocketConstraint",
		  "HingeConstraint",
		  "PrismaticConstraint",
		  "CylindricalConstraint",
		  "RopeConstraint",
		  "SpringConstraint"}) {
		INFO(klass);
		CHECK(Classes::IsA(Classes::Find(Name(klass)), ConstraintClass()));
	}
}

TEST_CASE("Weld and WeldConstraint use their Roblox part-based hierarchies", "[scene][constraints]") {
	RegisterSceneClasses();
	Store store("constraints_test.rigid_classes");

	const auto jointBase = Classes::Find(Name("JointInstance"));
	const auto weldClass = Classes::Find(Name("Weld"));
	const auto directClass = Classes::Find(Name("WeldConstraint"));
	REQUIRE(jointBase.IsValid());
	CHECK(Classes::IsA(weldClass, jointBase));
	CHECK_FALSE(Classes::IsA(directClass, ConstraintClass()));

	const Entity weld = store.CreateInstance(weldClass, "Weld");
	const Entity direct = store.CreateInstance(directClass, "WeldConstraint");
	CHECK(store.Get<JointInstance>(weld) != nullptr);
	CHECK(store.Get<engine::scene::WeldConstraint>(direct) != nullptr);
}

TEST_CASE("a stiffness with nothing free to move is still a weld", "[scene][constraints]") {
	// `IsDrivenJoint` asks about the modes and the stiffness together, because a
	// solver that built a drive row for a locked axis would fight its own
	// hard constraint.
	Constraint joint;
	joint.Stiffness = 5000.0f;
	CHECK(IsRigidJoint(joint));
	CHECK_FALSE(IsDrivenJoint(joint));

	joint.Angular[1] = ConstraintMotion::Free;
	CHECK(IsDrivenJoint(joint));
}

TEST_CASE("the bodies come from the attachments and not from a cached pair", "[scene][constraints]") {
	RegisterSceneClasses();
	Store store("constraints_test.bodies");

	const Entity first = MakeAnchor(store, "Door");
	const Entity second = MakeAnchor(store, "Frame");

	Constraint joint;
	joint.Attachment0 = first;
	joint.Attachment1 = second;

	Entity a;
	Entity b;
	CHECK(JointBodies(store, joint, a, b));
	CHECK(a == store.ParentOf(first));
	CHECK(b == store.ParentOf(second));

	// Reparenting an attachment moves the joint with it, which is the whole
	// reason the pair is a walk rather than two more fields.
	const Entity moved = store.CreateInstance(Classes::Find(Name("Part")), "Wall");
	store.Set(moved, Transform{});
	REQUIRE(store.SetParent(second, moved));

	CHECK(JointBodies(store, joint, a, b));
	CHECK(b == moved);
}

TEST_CASE("one unattached end is pinned to the world rather than refused", "[scene][constraints]") {
	// How a swinging door hangs off a wall that is not a body.
	RegisterSceneClasses();
	Store store("constraints_test.pinned");

	Constraint joint;
	joint.Attachment0 = MakeAnchor(store, "Door");

	Entity a;
	Entity b;
	CHECK(JointBodies(store, joint, a, b));
	CHECK(b == NULL_ENTITY);

	// An unanchored first end is the case there is nothing to solve for.
	Constraint loose;
	CHECK_FALSE(JointBodies(store, loose, a, b));
}

TEST_CASE("a joint crosses a snapshot whole", "[scene][constraints]") {
	RegisterSceneClasses();
	Store source("constraints_test.save.source");

	const Entity instance = source.CreateInstance(Classes::Find(Name("HingeConstraint")), "Hinge");

	Constraint joint;
	joint.Angular[0] = ConstraintMotion::Limited;
	joint.AngularLower[0] = -1.0f;
	joint.AngularUpper[0] = 1.0f;
	joint.Stiffness = 250.0f;
	joint.Target = CFrame(Vector3(0.0f, 3.0f, 0.0f));
	source.Set(instance, joint);

	engine::core::ByteWriter writer;
	REQUIRE(source.Save(writer));

	Store restored("constraints_test.save.restored");
	engine::core::ByteReader reader(writer.Bytes());
	REQUIRE(restored.Load(reader));

	const Constraint *back = restored.Get<Constraint>(instance);
	REQUIRE(back != nullptr);
	CHECK(back->Angular[0] == ConstraintMotion::Limited);
	CHECK(back->AngularUpper[0] == 1.0f);
	CHECK(back->Stiffness == 250.0f);
	CHECK(back->Target.Position.Y == 3.0f);
}
