// A rig's storage and the one pass over it.
//
// The cases here are about the *ordering* contract more than about the
// arithmetic: `Bone::ParentJoint` being lower than `Bone::Joint` is what turns
// a chain into a forward pass, and a rig that breaks it has to degrade to
// something an author can see rather than to a body at the origin.

#include "fixtures/SkinningParity.hpp"

#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Skinning.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <vector>

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

TEST_CASE("skinning verifies every authored pose and only advances changed rows", "[scene][skinning]") {
	skinning_fixture::World first(3, 32, true);
	skinning_fixture::World second(1, 256, false);
	for (size_t tick = 0; tick < 8; ++tick) {
		++first.Clock;
		first.Prepare();
		const uint64_t authored = first.InputHash();
		CHECK(ResolveBones(first.Storage) == first.ExpectedWrites);
		CHECK_NOTHROW(first.Verify());
		CHECK(first.InputHash() == authored);
		const uint64_t version = first.Storage.ChangeVersion();
		CHECK(ResolveBones(first.Storage) == 0);
		CHECK(first.Storage.ChangeVersion() == version);
		const uint64_t secondVersion = second.Storage.ChangeVersion();
		CHECK(ResolveBones(second.Storage) == 0);
		CHECK(second.Storage.ChangeVersion() == secondVersion);
		CHECK_NOTHROW(second.Verify());
	}
}

TEST_CASE(
	"duplicate palette slots follow tree order through reparent and component moves", "[scene][skinning]"
) {
	RegisterSceneClasses();
	Store store("skinning_test.duplicates");
	const Entity rig = store.CreateInstance(Classes::Find(Name("Folder")), "Rig");
	store.Set(rig, Skeleton{Name("skinning_test.Duplicate"), 4, {}});
	const Entity first = store.CreateInstance(BoneClass(), "First");
	const Entity second = store.CreateInstance(BoneClass(), "Second");
	const Entity child = store.CreateInstance(BoneClass(), "Child");
	for (Entity entity : {first, second, child})
		store.SetParent(entity, rig);
	Bone a;
	a.Rest = CFrame(Vector3(1, 0, 0));
	a.Joint = 1;
	Bone b;
	b.Rest = CFrame(Vector3(2, 0, 0));
	b.Joint = 1;
	b.WorldFrame = CFrame(Vector3(99, 98, 97));
	Bone c;
	c.Rest = CFrame(Vector3(0, 3, 0));
	c.Joint = 3;
	c.ParentJoint = 1;
	store.Set(first, a);
	store.Set(second, b);
	store.Set(child, c);
	CHECK(ResolveBones(store) == 2);
	CHECK_NOTHROW(skinning_fixture::VerifyFrame(store.Get<Bone>(first)->WorldFrame, {{1, 0, 0}}));
	CHECK_NOTHROW(skinning_fixture::VerifyFrame(store.Get<Bone>(child)->WorldFrame, {{1, 3, 0}}));
	CHECK(
		skinning_fixture::Words(store.Get<Bone>(second)->WorldFrame) == skinning_fixture::Words(b.WorldFrame)
	);
	// Removing and appending the first sibling changes the duplicate winner.
	store.SetParent(first, engine::ecs::NULL_ENTITY);
	store.SetParent(first, rig);
	store.Set(second, Transform{});
	CHECK(ResolveBones(store) == 2);
	CHECK_NOTHROW(skinning_fixture::VerifyFrame(store.Get<Bone>(second)->WorldFrame, {{2, 0, 0}}));
	CHECK_NOTHROW(skinning_fixture::VerifyFrame(store.Get<Bone>(child)->WorldFrame, {{2, 3, 0}}));
	store.Remove<Bone>(second);
	CHECK(ResolveBones(store) == 1);
	CHECK_NOTHROW(skinning_fixture::VerifyFrame(store.Get<Bone>(child)->WorldFrame, {{1, 3, 0}}));
	c.ParentJoint = 2;
	store.Set(child, c);
	CHECK(ResolveBones(store) == 1);
	CHECK_NOTHROW(skinning_fixture::VerifyFrame(store.Get<Bone>(child)->WorldFrame, {{0, 3, 0}}));
}

TEST_CASE("one bone resolve isolates dense sparse and empty rigs of changing sizes", "[scene][skinning]") {
	RegisterSceneClasses();
	Store store("skinning_test.mixed");
	struct ExpectedBone {
		Entity Instance;
		skinning_fixture::Affine World;
	};
	std::vector<ExpectedBone> expected;
	std::vector<Entity> rigs;
	const std::array<size_t, 7> counts{256, 3, 0, 129, 2, 256, 1};
	for (size_t index = 0; index < counts.size(); ++index) {
		engine::scene::PartDesc description;
		description.Frame = CFrame(Vector3(static_cast<float>(index) * 1024 + 8, 2, -4));
		const Entity rig = engine::scene::MakePart(store, description);
		store.Set(rig, Skeleton{Name("skinning_test.Mixed"), static_cast<uint16_t>(counts[index]), {}});
		rigs.push_back(rig);
		const bool sparse = index == 3;
		for (size_t slot = 0; slot < counts[index]; ++slot) {
			if (sparse && slot != 0 && slot != 128) continue;
			const Entity instance = store.CreateInstance(BoneClass(), "Joint");
			store.SetParent(instance, rig);
			Bone bone;
			bone.Rest = CFrame(Vector3(.5f, .25f, -.125f));
			bone.Joint = static_cast<uint16_t>(slot);
			bone.ParentJoint = slot == 0 ? NO_JOINT : static_cast<uint16_t>(sparse ? 1 : slot - 1);
			store.Set(instance, bone);
			const float steps = sparse ? 1 : static_cast<float>(slot + 1);
			expected.push_back(
				{instance,
				 {{description.Frame.Position.X + .5f * steps, 2 + .25f * steps, -4 - .125f * steps}}}
			);
		}
	}
	// Ensure this call actually exercises a descending, empty and sparse run.
	std::vector<Entity> visited;
	store.Each<const Skeleton>([&](Entity rig, const Skeleton &) { visited.push_back(rig); });
	REQUIRE(visited == rigs);
	CHECK(ResolveBones(store) == expected.size());
	for (const auto &bone : expected)
		CHECK_NOTHROW(skinning_fixture::VerifyFrame(store.Get<Bone>(bone.Instance)->WorldFrame, bone.World));
	const uint64_t version = store.ChangeVersion();
	CHECK(ResolveBones(store) == 0);
	CHECK(store.ChangeVersion() == version);
	// Delete a parent in the final large rig, so a later call must not retain
	// that slot's filled state and accidentally use its previous world pose.
	Entity removed;
	Entity child;
	store.EachDescendant(rigs[5], [&](Entity entity) {
		const Bone *bone = store.Get<Bone>(entity);
		if (bone && bone->Joint == 1) removed = entity;
		if (bone && bone->Joint == 2) child = entity;
	});
	REQUIRE(removed != engine::ecs::NULL_ENTITY);
	REQUIRE(child != engine::ecs::NULL_ENTITY);
	store.Remove<Bone>(removed);
	CHECK(ResolveBones(store) == 254);
	CHECK_NOTHROW(
		skinning_fixture::VerifyFrame(store.Get<Bone>(child)->WorldFrame, {{5128.5f, 2.25f, -4.125f}})
	);
}
