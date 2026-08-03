#include <engine/core/Name.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Enums.hpp>
#include <engine/scene/Part.hpp>
#include <engine/spatial/LayerMask.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.scene.part")
// MakePart's collider defaults are spatial::LayerMask values.
TEST_DEPENDS("engine.spatial.layermask")

using engine::core::CFrame;
using engine::core::Name;
using engine::core::Vector3;
using engine::ecs::Classes;
using engine::ecs::ClassId;
using engine::ecs::Entity;
using engine::ecs::NULL_ENTITY;
using engine::ecs::Store;
using engine::scene::Bounds;
using engine::scene::Collider;
using engine::scene::MakePart;
using engine::scene::Motion;
using engine::scene::PartClass;
using engine::scene::PartDesc;
using engine::scene::RigidBody;
using engine::scene::ShapeKind;
using engine::scene::Surface;
using engine::scene::Transform;
using engine::scene::Visual;

TEST_CASE("the part class inherits the whole set", "[scene][part]") {
	// Inheritance is set inclusion, so a query for `Transform` matches every
	// part without knowing parts exist. If `Part` stopped descending from
	// `PVInstance`, that query would silently stop matching rather than fail to
	// compile.
	const ClassId part = PartClass();
	REQUIRE(part.IsValid());

	const ClassId basePart = Classes::Find(Name("BasePart"));
	const ClassId pvInstance = Classes::Find(Name("PVInstance"));
	const ClassId instance = Classes::Find(Name("Instance"));

	CHECK(Classes::IsA(part, basePart));
	CHECK(Classes::IsA(part, pvInstance));
	CHECK(Classes::IsA(part, instance));
	CHECK_FALSE(Classes::IsA(instance, part));
}

TEST_CASE("a part carries the five components the plan names", "[scene][part]") {
	Store store("part_test.set");

	const Entity part = MakePart(store, PartDesc{});
	REQUIRE(part != NULL_ENTITY);

	CHECK(store.Has<Transform>(part));
	CHECK(store.Has<Bounds>(part));
	CHECK(store.Has<Visual>(part));
	CHECK(store.Has<Collider>(part));
	CHECK(store.Has<Surface>(part));

	CHECK(store.IsA(part, PartClass()));
}

TEST_CASE("size is halved once, into bounds and collider", "[scene][part]") {
	// `Size` is a full extent because that is what a person types; everything
	// downstream wants the half. Halving in two places is where the two
	// eventually disagree by a factor of two, and a collider twice the size of
	// its bounds is a body that collides with things the broad phase never
	// offered it.
	Store store("part_test.size");

	PartDesc desc;
	desc.Size = Vector3(4.0f, 2.0f, 6.0f);

	const Entity part = MakePart(store, desc);
	REQUIRE(part != NULL_ENTITY);

	const Bounds *bounds = store.Get<Bounds>(part);
	REQUIRE(bounds != nullptr);
	CHECK(bounds->HalfExtent == Vector3(2.0f, 1.0f, 3.0f));

	const Collider *collider = store.Get<Collider>(part);
	REQUIRE(collider != nullptr);
	CHECK(collider->Extent == bounds->HalfExtent);
}

TEST_CASE("MakePart with Anchored adds no Motion", "[scene][part]") {
	// Named in `v02v03v04.md` §3.7. `Anchored` decides presence and not a flag,
	// so an anchored part is in a different archetype and the dynamic queries
	// never visit it — which is the whole reason static geometry costs nothing
	// per tick.
	Store store("part_test.anchored");

	PartDesc anchored;
	anchored.Anchored = true;

	const Entity fixed = MakePart(store, anchored);
	REQUIRE(fixed != NULL_ENTITY);

	CHECK_FALSE(store.Has<Motion>(fixed));
	CHECK_FALSE(store.Has<RigidBody>(fixed));
}

TEST_CASE("a dynamic part is a body that moves", "[scene][part]") {
	Store store("part_test.dynamic");

	const Entity moving = MakePart(store, PartDesc{});
	REQUIRE(moving != NULL_ENTITY);

	CHECK(store.Has<Motion>(moving));
	CHECK(store.Has<RigidBody>(moving));
}

TEST_CASE("anchored and dynamic parts do not share a query", "[scene][part]") {
	// The archetype claim, checked rather than asserted: the integration query
	// is `<Transform, const Motion>`, and it must visit exactly the parts that
	// can move however many anchored ones sit beside them.
	Store store("part_test.archetypes");

	PartDesc anchored;
	anchored.Anchored = true;

	for (int index = 0; index < 5; index++) {
		MakePart(store, anchored);
	}
	MakePart(store, PartDesc{});
	MakePart(store, PartDesc{});

	CHECK(store.CountMatching<Transform>() == 7);
	CHECK(store.CountMatching<Transform, Motion>() == 2);
}

TEST_CASE("a description's names and shape reach the components", "[scene][part]") {
	Store store("part_test.described");

	PartDesc desc;
	desc.Frame = CFrame(Vector3(1.0f, 2.0f, 3.0f));
	desc.Shape = ShapeKind::Cylinder;
	desc.Material = Name("part_test.Oak");
	desc.Mesh = Name("part_test.Barrel");

	const Entity part = MakePart(store, desc);
	REQUIRE(part != NULL_ENTITY);

	CHECK(store.Get<Transform>(part)->Frame.Position == Vector3(1.0f, 2.0f, 3.0f));
	CHECK(store.Get<Collider>(part)->Shape == ShapeKind::Cylinder);
	CHECK(store.Get<Surface>(part)->Material == desc.Material);
	CHECK(store.Get<Visual>(part)->Mesh == desc.Mesh);
}

TEST_CASE("what the description does not name keeps its prototype default", "[scene][part]") {
	// `MakePart` reads before it writes for the two components `PartDesc` only
	// partly describes. Constructing a fresh `Collider` or `Visual` instead
	// would silently overwrite a layer mask or a tint the class had declared,
	// and nothing at the call site would look wrong.
	Store store("part_test.defaults");

	PartDesc desc;
	desc.Shape = ShapeKind::Sphere;

	const Entity part = MakePart(store, desc);
	REQUIRE(part != NULL_ENTITY);

	const Collider *collider = store.Get<Collider>(part);
	REQUIRE(collider != nullptr);
	CHECK(collider->Layer == engine::spatial::LayerMask::Only(0));
	CHECK(collider->Mask == engine::spatial::LayerMask::All());
	CHECK_FALSE(collider->Trigger);

	const Visual *visual = store.Get<Visual>(part);
	REQUIRE(visual != nullptr);
	CHECK(visual->Visible);
	CHECK(visual->Tint.R == 1.0f);

	// Named by nothing in the description, so it stays unset rather than
	// picking up the mesh name.
	CHECK_FALSE(visual->Material.IsValid());
}

TEST_CASE("a replica refuses to mint a part", "[scene][part]") {
	// `Store::CreateInstance` used to walk straight past `SetAdoptOnly` — only
	// `Store::Create` checked it — so `MakePart` carried a guard of its own. The
	// guard is gone and the storage covers it now, which is the right place for
	// it; this case stays because the behaviour a caller depends on is unchanged
	// and the *reason* it holds is exactly what moved.
	Store store("part_test.replica");
	store.SetAdoptOnly(true);

	CHECK(MakePart(store, PartDesc{}) == NULL_ENTITY);
}
