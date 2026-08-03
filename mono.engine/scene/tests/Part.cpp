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

#include <catch2/catch_approx.hpp>
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

// --- the property surface ---------------------------------------------------
//
// `v05.md` §5.5's tests. The interesting ones are not "does a setter set" —
// they are the four ways a property write can look like it worked and not have.

namespace {
	// Reads a property into a value of the type it says it is.
	template <class T> T Read(const Store &store, Entity instance, const char *property) {
		T value{};
		REQUIRE(store.GetProperty(instance, Name(property), &value, sizeof(T)));
		return value;
	}

	template <class T> bool Write(Store &store, Entity instance, const char *property, const T &value) {
		return store.SetProperty(instance, Name(property), &value, sizeof(T));
	}
}

TEST_CASE("Position writes the translation and keeps the rotation", "[scene][part]") {
	Store store("property_test");
	const Entity part = MakePart(store, PartDesc{});

	// A rotation that is not the identity, so a setter that replaced the whole
	// CFrame would be visible rather than a no-op.
	REQUIRE(Write(store, part, "Orientation", Vector3{0.0f, 90.0f, 0.0f}));
	const Vector3 before = Read<Vector3>(store, part, "Orientation");

	REQUIRE(Write(store, part, "Position", Vector3{3.0f, -4.0f, 5.0f}));

	const Vector3 position = Read<Vector3>(store, part, "Position");
	CHECK(position.X == 3.0f);
	CHECK(position.Y == -4.0f);
	CHECK(position.Z == 5.0f);

	// The whole point: an offset-shaped setter would have written twelve bytes
	// over the front of the CFrame and left the quaternion behind.
	const Vector3 after = Read<Vector3>(store, part, "Orientation");
	CHECK(after.Y == Catch::Approx(before.Y).margin(1.0e-3f));
}

TEST_CASE("Orientation writes the rotation and keeps the translation", "[scene][part]") {
	Store store("property_test");
	const Entity part = MakePart(store, PartDesc{});

	REQUIRE(Write(store, part, "Position", Vector3{1.0f, 2.0f, 3.0f}));
	REQUIRE(Write(store, part, "Orientation", Vector3{0.0f, 45.0f, 0.0f}));

	const Vector3 position = Read<Vector3>(store, part, "Position");
	CHECK(position.X == 1.0f);
	CHECK(position.Y == 2.0f);
	CHECK(position.Z == 3.0f);

	// Degrees, because Roblox's Orientation is degrees. Radians here would be
	// a factor of 57 that nothing else in the suite would notice.
	CHECK(Read<Vector3>(store, part, "Orientation").Y == Catch::Approx(45.0f).margin(1.0e-2f));
}

TEST_CASE("Size is a full extent over a stored half, and moves the collider too", "[scene][part]") {
	Store store("property_test");
	PartDesc desc;
	desc.Size = Vector3{2.0f, 2.0f, 2.0f};
	const Entity part = MakePart(store, desc);

	CHECK(Read<Vector3>(store, part, "Size").X == 2.0f);
	CHECK(store.Get<Bounds>(part)->HalfExtent.X == 1.0f);

	REQUIRE(Write(store, part, "Size", Vector3{4.0f, 6.0f, 8.0f}));

	CHECK(store.Get<Bounds>(part)->HalfExtent.X == 2.0f);
	CHECK(store.Get<Bounds>(part)->HalfExtent.Y == 3.0f);

	// Drawn at one size and collided at another is the bug this asserts
	// against, and nothing else in the tree would report it.
	CHECK(store.Get<Collider>(part)->Extent.X == 2.0f);
	CHECK(store.Get<Collider>(part)->Extent.Y == 3.0f);
}

TEST_CASE("a property write marks its column changed", "[scene][part]") {
	Store store("property_test");
	const Entity part = MakePart(store, PartDesc{});

	// The one that matters most and is invisible when it breaks: replication
	// builds deltas from the change channel, so a write the channel never sees
	// is a script edit the server never sends.
	store.Observe<Transform>();
	store.ClearChanges();
	CHECK_FALSE(store.Changed<Transform>(part));

	REQUIRE(Write(store, part, "Position", Vector3{1.0f, 0.0f, 0.0f}));
	CHECK(store.Changed<Transform>(part));
}

TEST_CASE("a size that disagrees with the property is refused", "[scene][part]") {
	Store store("property_test");
	const Entity part = MakePart(store, PartDesc{});

	// A Vector3 handed to a CFrame property. Truncating would leave the
	// rotation from whatever was there before, which reads as a physics bug a
	// long way from the binding that caused it.
	const Vector3 wrong{1.0f, 2.0f, 3.0f};
	CHECK_FALSE(store.SetProperty(part, Name("CFrame"), &wrong, sizeof(wrong)));

	CFrame value;
	CHECK_FALSE(store.GetProperty(part, Name("CFrame"), &value, sizeof(Vector3)));

	// And a name nothing declares.
	CHECK_FALSE(Write(store, part, "Transparency", 0.5f));
}

TEST_CASE("Anchored is presence rather than a flag", "[scene][part]") {
	Store store("property_test");

	const Entity dynamic = MakePart(store, PartDesc{});
	CHECK(store.Get<RigidBody>(dynamic) != nullptr);
	CHECK_FALSE(Read<bool>(store, dynamic, "Anchored"));

	// A structural write: the row moves to another archetype rather than a
	// boolean changing inside it.
	REQUIRE(Write(store, dynamic, "Anchored", true));
	CHECK(store.Get<RigidBody>(dynamic) == nullptr);
	CHECK(store.Get<Motion>(dynamic) == nullptr);
	CHECK(Read<bool>(store, dynamic, "Anchored"));

	REQUIRE(Write(store, dynamic, "Anchored", false));
	CHECK(store.Get<RigidBody>(dynamic) != nullptr);
	CHECK_FALSE(Read<bool>(store, dynamic, "Anchored"));
}

TEST_CASE("CanCollide is the inverse of the trigger flag", "[scene][part]") {
	Store store("property_test");
	const Entity part = MakePart(store, PartDesc{});

	CHECK(Read<bool>(store, part, "CanCollide"));

	REQUIRE(Write(store, part, "CanCollide", false));
	CHECK(store.Get<Collider>(part)->Trigger);

	// The layer mask is untouched, which is why this maps to Trigger and not
	// to the mask: clearing and restoring a mask loses whatever a game had
	// configured, and the loss only shows up much later.
	CHECK(store.Get<Collider>(part)->Mask == engine::spatial::LayerMask::All());
}

TEST_CASE("a replica refuses a property write", "[scene][part]") {
	Store store("property_test");
	const Entity part = MakePart(store, PartDesc{});

	store.SetAdoptOnly(true);

	// Refused rather than applied-then-overwritten. A script author cannot tell
	// those apart from inside the script.
	CHECK_FALSE(Write(store, part, "Position", Vector3{9.0f, 9.0f, 9.0f}));
	CHECK(store.Get<Transform>(part)->Frame.Position.X == 0.0f);
}
