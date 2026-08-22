// A rig's storage and the one pass over it.
//
// The cases here are about the *ordering* contract more than about the
// arithmetic: `Bone::ParentJoint` being lower than `Bone::Joint` is what turns
// a chain into a forward pass, and a rig that breaks it has to degrade to
// something an author can see rather than to a body at the origin.

#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Skinning.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.scene.skinning")

using Catch::Approx;
using engine::core::CFrame;
using engine::core::Name;
using engine::core::Vector3;
using engine::ecs::Classes;
using engine::ecs::Entity;
using engine::ecs::Store;
using engine::scene::Bone;
using engine::scene::BoneClass;
using engine::scene::NO_JOINT;
using engine::scene::RegisterSceneClasses;
using engine::scene::ResolveBone;
using engine::scene::ResolveBones;
using engine::scene::Skeleton;
using engine::scene::SkeletonOf;
using engine::scene::SkinningFrameOf;
using engine::scene::Transform;

namespace {
	// A rig on a placed drawable, with `count` joints in a straight chain along
	// X, each one metre past the last.
	Entity MakeRig(Store &store, size_t count, const CFrame &placement) {
		const Entity rig = store.CreateInstance(Classes::Find(Name("Part")), "Rig");
		store.Set(rig, Transform{placement});
		store.Set(rig, Skeleton{Name("skinning_test.Chain"), static_cast<uint16_t>(count), {}});

		Entity parent = rig;
		for (size_t index = 0; index < count; index++) {
			const Entity bone = store.CreateInstance(BoneClass(), "Joint");
			store.SetParent(bone, parent);

			Bone joint;
			joint.Rest = CFrame(Vector3(1.0f, 0.0f, 0.0f));
			joint.Joint = static_cast<uint16_t>(index);
			joint.ParentJoint = index == 0 ? NO_JOINT : static_cast<uint16_t>(index - 1);
			store.Set(bone, joint);

			parent = bone;
		}
		return rig;
	}

	Entity JointAt(const Store &store, Entity rig, uint16_t slot) {
		Entity found;
		store.EachDescendant(rig, [&](Entity descendant) {
			const Bone *bone = store.Get<Bone>(descendant);
			if (bone != nullptr && bone->Joint == slot) {
				found = descendant;
			}
		});
		return found;
	}
}

TEST_CASE("a chain composes from the rig's own placement", "[scene][skinning]") {
	RegisterSceneClasses();
	Store store("skinning_test.chain");

	const Entity rig = MakeRig(store, 3, CFrame(Vector3(10.0f, 0.0f, 0.0f)));
	CHECK(ResolveBones(store) == 3);

	// Three joints, each a metre further along X, starting from the drawable's
	// own transform. The point of the case is that the *third* joint is right:
	// it can only be if the second was written before it was read.
	const Bone *last = store.Get<Bone>(JointAt(store, rig, 2));
	REQUIRE(last != nullptr);
	CHECK(last->WorldFrame.Position.X == Approx(13.0f));
}

TEST_CASE("a rig with no transform composes from the identity", "[scene][skinning]") {
	// The fallback `ResolveAttachments` makes for an attachment under no part:
	// a rig built out of bare instances is a usable thing rather than an error.
	RegisterSceneClasses();
	Store store("skinning_test.bare");

	const Entity rig = store.CreateInstance(Classes::Find(Name("Part")), "Rig");
	store.Remove<Transform>(rig);
	store.Set(rig, Skeleton{Name("skinning_test.Bare"), 1, {}});

	const Entity bone = store.CreateInstance(BoneClass(), "Joint");
	store.SetParent(bone, rig);
	store.Set(bone, Bone{CFrame(Vector3(0.0f, 2.0f, 0.0f)), {}, {}, {}, 0, NO_JOINT});

	CHECK(ResolveBones(store) == 1);
	CHECK(store.Get<Bone>(bone)->WorldFrame.Position.Y == Approx(2.0f));
}

TEST_CASE("the animated transform stacks on the rest pose", "[scene][skinning]") {
	// The whole reason `Rest` and `Transform` are two fields: clearing what an
	// animation wrote puts the rig back where it started, with nothing to
	// restore from.
	RegisterSceneClasses();
	Store store("skinning_test.pose");

	const Entity rig = MakeRig(store, 2, CFrame{});
	const Entity second = JointAt(store, rig, 1);

	Bone *first = store.GetMutable<Bone>(JointAt(store, rig, 0));
	REQUIRE(first != nullptr);
	first->Transform = CFrame(Vector3(0.0f, 5.0f, 0.0f));

	ResolveBones(store);
	CHECK(store.Get<Bone>(second)->WorldFrame.Position.Y == Approx(5.0f));
	CHECK(store.Get<Bone>(second)->WorldFrame.Position.X == Approx(2.0f));

	first->Transform = CFrame{};
	ResolveBones(store);
	CHECK(store.Get<Bone>(second)->WorldFrame.Position.Y == Approx(0.0f));
}

TEST_CASE("a bone whose parent slot is missing falls back to the rig", "[scene][skinning]") {
	// A slot nothing filled must not take the bone to the origin, which is what
	// a fallback to the identity would do on a rig placed anywhere else.
	RegisterSceneClasses();
	Store store("skinning_test.gap");

	const Entity rig = MakeRig(store, 3, CFrame(Vector3(100.0f, 0.0f, 0.0f)));

	Bone *third = store.GetMutable<Bone>(JointAt(store, rig, 2));
	REQUIRE(third != nullptr);
	third->ParentJoint = 7;

	CHECK(ResolveBones(store) == 3);

	const Bone *orphan = store.Get<Bone>(JointAt(store, rig, 2));
	REQUIRE(orphan != nullptr);
	CHECK(orphan->WorldFrame.Position.X == Approx(101.0f));
}

TEST_CASE("a parent slot at or above its own is refused rather than followed", "[scene][skinning]") {
	// `Bone::ParentJoint < Bone::Joint` is what makes the pass a forward one.
	// A rig that breaks it arrives from a file somebody else wrote, so the pass
	// has to answer rather than loop.
	RegisterSceneClasses();
	Store store("skinning_test.cycle");

	const Entity rig = MakeRig(store, 2, CFrame(Vector3(7.0f, 0.0f, 0.0f)));

	Bone *first = store.GetMutable<Bone>(JointAt(store, rig, 0));
	REQUIRE(first != nullptr);
	first->ParentJoint = 1;

	CHECK(ResolveBones(store) == 2);
	CHECK(store.Get<Bone>(JointAt(store, rig, 0))->WorldFrame.Position.X == Approx(8.0f));
}

TEST_CASE("resolving twice writes nothing the second time", "[scene][skinning]") {
	// `ResolveAttachments`' rule: writing every row every frame advances the
	// world's change counter for ever, and two gates are built on an unchanged
	// counter meaning nothing authored happened.
	RegisterSceneClasses();
	Store store("skinning_test.stable");

	MakeRig(store, 4, CFrame(Vector3(1.0f, 2.0f, 3.0f)));
	CHECK(ResolveBones(store) == 4);
	CHECK(ResolveBones(store) == 0);
}

TEST_CASE("resolving one bone on the spot matches the pass", "[scene][skinning]") {
	// The two answers have to agree, for `ResolveAttachment`'s reason: a
	// property that answered differently depending on when in the frame it was
	// asked is one nobody can reason about.
	RegisterSceneClasses();
	Store store("skinning_test.spot");

	const Entity rig = MakeRig(store, 3, CFrame(Vector3(0.0f, 0.0f, 4.0f)));
	const Entity third = JointAt(store, rig, 2);

	const CFrame direct = ResolveBone(store, third);
	ResolveBones(store);

	const Bone *row = store.Get<Bone>(third);
	REQUIRE(row != nullptr);
	CHECK(direct.Position.X == Approx(row->WorldFrame.Position.X));
	CHECK(direct.Position.Z == Approx(row->WorldFrame.Position.Z));
}

TEST_CASE("the skinning frame is the world frame times the inverse bind", "[scene][skinning]") {
	// One statement of the product, for `ReflectCamera`'s reason. A vertex bound
	// at the rest pose has to land back where it started when nothing is
	// playing, which is the property that catches the order being reversed.
	Bone bone;
	bone.Rest = CFrame(Vector3(3.0f, 0.0f, 0.0f));
	bone.WorldFrame = bone.Rest;
	bone.InverseBind = bone.Rest.Inverse();

	const CFrame skinning = SkinningFrameOf(bone);
	const Vector3 vertex = skinning.PointToWorldSpace(Vector3(3.0f, 1.0f, 0.0f));
	CHECK(vertex.X == Approx(3.0f));
	CHECK(vertex.Y == Approx(1.0f));
}

TEST_CASE("a bone finds the rig above it however deeply it nests", "[scene][skinning]") {
	RegisterSceneClasses();
	Store store("skinning_test.lookup");

	const Entity rig = MakeRig(store, 3, CFrame{});
	CHECK(SkeletonOf(store, JointAt(store, rig, 2)) == rig);

	const Entity loose = store.CreateInstance(BoneClass(), "Joint");
	CHECK(SkeletonOf(store, loose) == engine::ecs::NULL_ENTITY);
}

TEST_CASE("a bone is a class and it is not an attachment", "[scene][skinning]") {
	// The one departure from Roblox's tree, and it is deliberate: inheriting
	// `Attachment` would put two world frames on one row, resolved by two passes
	// against two different parents.
	RegisterSceneClasses();

	REQUIRE(BoneClass().IsValid());
	CHECK(Classes::IsA(BoneClass(), Classes::Find(Name("Instance"))));
	CHECK_FALSE(Classes::IsA(BoneClass(), Classes::Find(Name("Attachment"))));
}
