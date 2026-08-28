#include <engine/collision/ConvexHull.hpp>
#include <engine/collision/TriangleMesh.hpp>
#include <engine/core/Name.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/EnumTable.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/physics/Broadphase.hpp>
#include <engine/physics/NarrowPhase.hpp>
#include <engine/physics/PhysicsWorld.hpp>
#include <engine/physics/Pipeline.hpp>
#include <engine/physics/Query.hpp>
#include <engine/scene/CollisionShapes.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Enums.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <vector>

TEST_SUITE_ID("engine.physics.bakedshapes")
// The hull and the soup themselves.
TEST_DEPENDS("engine.collision.convexhull")
TEST_DEPENDS("engine.collision.trianglemesh")
// The general convex pair every baked contact is routed through.
TEST_DEPENDS("engine.physics.convexquery")
// The table a `Collider::Geometry` resolves through.
TEST_DEPENDS("engine.scene.components")

using Catch::Approx;
using engine::core::CFrame;
using engine::core::Name;
using engine::core::Vector3;
using engine::ecs::Entity;
using engine::ecs::Store;
using engine::physics::BroadPhase;
using engine::physics::ColliderHit;
using engine::physics::NarrowPhase;
using engine::physics::PhysicsWorld;
using engine::physics::PreparePhysicsWorld;
using engine::physics::Raycast;
using engine::physics::SyncBroadphase;
using engine::scene::Collider;
using engine::scene::CollisionShapes;
using engine::scene::Motion;
using engine::scene::ShapeKind;
using engine::scene::Simulated;
using engine::scene::Transform;

namespace {
	// The eight corners of an axis-aligned box.
	std::vector<Vector3> BoxCorners(float half) {
		std::vector<Vector3> corners;
		for (int index = 0; index < 8; index++) {
			corners.push_back(
				Vector3{
					(index & 1) != 0 ? half : -half,
					(index & 2) != 0 ? half : -half,
					(index & 4) != 0 ? half : -half,
				}
			);
		}
		return corners;
	}

	// A flat two-triangle quad on the XZ plane, `half` metres out on each side.
	engine::collision::TriangleMesh Ground(float half) {
		const std::vector<Vector3> vertices{
			Vector3{-half, 0.0f, -half},
			Vector3{half, 0.0f, -half},
			Vector3{half, 0.0f, half},
			Vector3{-half, 0.0f, half},
		};
		const std::vector<uint32_t> indices{0, 1, 2, 0, 2, 3};
		return engine::collision::BuildTriangleMesh(vertices, indices);
	}

	std::unique_ptr<Store> World() {
		auto store = std::make_unique<Store>("physics.bakedshapes");
		PreparePhysicsWorld(*store, 4.0f);
		return store;
	}

	// A part that collides as a named baked shape.
	Entity Baked(Store &store, ShapeKind kind, Name geometry, const Vector3 &at, bool anchored) {
		const Entity part = store.Create();
		store.Set<Transform>(part, Transform{CFrame{at}});

		Collider collider;
		collider.Shape = kind;
		collider.Geometry = geometry;

		// **The extent is still set**, because it is what the broad phase bounds
		// the part with and what a name that did not resolve falls back to.
		collider.Extent = Vector3{1.0f, 1.0f, 1.0f};
		store.Set<Collider>(part, collider);

		// Static is the absence of both, so the anchored branch stores nothing.
		if (!anchored) {
			store.Set<Simulated>(part, Simulated{});
			store.Set<Motion>(part, Motion{});
		}
		return part;
	}

	Entity Box(Store &store, const Vector3 &at, const Vector3 &half, bool anchored) {
		const Entity part = store.Create();
		store.Set<Transform>(part, Transform{CFrame{at}});

		Collider collider;
		collider.Extent = half;
		store.Set<Collider>(part, collider);

		if (!anchored) {
			store.Set<Simulated>(part, Simulated{});
			store.Set<Motion>(part, Motion{});
		}
		return part;
	}

	size_t ContactsIn(Store &store) {
		SyncBroadphase(store);
		BroadPhase(store);
		NarrowPhase(store);
		return store.Resource<PhysicsWorld>()->Manifolds().size();
	}
}

TEST_CASE("a hull collider collides as its baked points", "[bakedshapes]") {
	// **The end-to-end case.** A hull is baked into the world's table, a part
	// names it, and the narrow phase resolves the name and finds the contact
	// through the general convex search rather than through any of the six exact
	// pairs.
	std::unique_ptr<Store> owned = World();
	Store &store = *owned;

	CollisionShapes shapes;
	shapes.SetHull(Name("crate"), engine::collision::BuildConvexHull(BoxCorners(0.5f)));
	store.SetResource(shapes);

	Baked(store, ShapeKind::Hull, Name("crate"), Vector3{0.0f, 0.0f, 0.0f}, false);
	Box(store, Vector3{0.9f, 0.0f, 0.0f}, Vector3{0.5f, 0.5f, 0.5f}, true);

	CHECK(ContactsIn(store) == 1);
}

TEST_CASE("a hull collider separates when it should", "[bakedshapes]") {
	// The direction the failure has to run in. A hull that reported a contact
	// wherever its *part* was would be a crate-sized box wearing a hull's name,
	// and nothing would say so.
	std::unique_ptr<Store> owned = World();
	Store &store = *owned;

	CollisionShapes shapes;

	// A hull much smaller than the part that carries it, so the two answers are
	// visibly different: the part's extent is a metre and the hull is a tenth
	// of one.
	shapes.SetHull(Name("pebble"), engine::collision::BuildConvexHull(BoxCorners(0.1f)));
	store.SetResource(shapes);

	Baked(store, ShapeKind::Hull, Name("pebble"), Vector3{0.0f, 0.0f, 0.0f}, false);

	// Well inside the part's own metre extent and well outside the hull's tenth.
	Box(store, Vector3{0.7f, 0.0f, 0.0f}, Vector3{0.5f, 0.5f, 0.5f}, true);

	CHECK(ContactsIn(store) == 0);
}

TEST_CASE("a hull that resolves to nothing collides as its bound", "[bakedshapes]") {
	// **Stated behaviour rather than a fallback.** The alternatives for a name
	// nothing has baked are a box the size of the part or no collision at all,
	// and a part that silently stops colliding is a floor that is not there for
	// as long as content takes to stream. See `scene::Collider::Geometry`.
	std::unique_ptr<Store> owned = World();
	Store &store = *owned;
	store.SetResource(CollisionShapes{});

	Baked(store, ShapeKind::Hull, Name("missing"), Vector3{0.0f, 0.0f, 0.0f}, false);
	Box(store, Vector3{1.4f, 0.0f, 0.0f}, Vector3{0.5f, 0.5f, 0.5f}, true);

	// The part's own extent is a metre, so a box half a metre wide centred at
	// 1.4 overlaps it by a tenth. A hull-shaped nothing would not touch at all.
	CHECK(ContactsIn(store) == 1);
}

TEST_CASE("a world with no shape table still collides", "[bakedshapes]") {
	// A world that never registered a shape has no resource at all, and the
	// lookup has to survive that rather than dereference nothing.
	std::unique_ptr<Store> owned = World();
	Store &store = *owned;

	Baked(store, ShapeKind::Hull, Name("crate"), Vector3{0.0f, 0.0f, 0.0f}, false);
	Box(store, Vector3{1.4f, 0.0f, 0.0f}, Vector3{0.5f, 0.5f, 0.5f}, true);

	CHECK(ContactsIn(store) == 1);
}

TEST_CASE("a box resting on a mesh is held up by the triangles under it", "[bakedshapes]") {
	// **The case a mesh collider exists for**, and the one that says the
	// per-triangle solve works: a soup has no support point of its own, so each
	// triangle is solved as a three-point hull and the deepest contacts across
	// them become the manifold.
	std::unique_ptr<Store> owned = World();
	Store &store = *owned;

	CollisionShapes shapes;
	shapes.SetMesh(Name("terrain"), Ground(8.0f));
	store.SetResource(shapes);

	Baked(store, ShapeKind::Mesh, Name("terrain"), Vector3{0.0f, 0.0f, 0.0f}, true);

	// Sunk a centimetre into the surface, which is what a settled body is.
	Box(store, Vector3{0.0f, 0.49f, 0.0f}, Vector3{0.5f, 0.5f, 0.5f}, false);

	REQUIRE(ContactsIn(store) == 1);

	const auto &manifold = store.Resource<PhysicsWorld>()->Manifolds()[0];
	CHECK(manifold.PointCount >= 1);

	// The normal is vertical, whichever way round the pair came out.
	CHECK(std::abs(manifold.Normal.Y) > 0.9f);
}

TEST_CASE("a box above a mesh does not touch it", "[bakedshapes]") {
	std::unique_ptr<Store> owned = World();
	Store &store = *owned;

	CollisionShapes shapes;
	shapes.SetMesh(Name("terrain"), Ground(8.0f));
	store.SetResource(shapes);

	Baked(store, ShapeKind::Mesh, Name("terrain"), Vector3{0.0f, 0.0f, 0.0f}, true);
	Box(store, Vector3{0.0f, 3.0f, 0.0f}, Vector3{0.5f, 0.5f, 0.5f}, false);

	CHECK(ContactsIn(store) == 0);
}

TEST_CASE("two meshes never touch", "[bakedshapes]") {
	// Two surfaces with no inside have no overlap to resolve and no direction to
	// push either of them, and a mesh collider is level geometry - so a pair of
	// them is two pieces of level authored where they are. Reported as no
	// contact rather than refused.
	std::unique_ptr<Store> owned = World();
	Store &store = *owned;

	CollisionShapes shapes;
	shapes.SetMesh(Name("terrain"), Ground(8.0f));
	store.SetResource(shapes);

	Baked(store, ShapeKind::Mesh, Name("terrain"), Vector3{0.0f, 0.0f, 0.0f}, false);
	Baked(store, ShapeKind::Mesh, Name("terrain"), Vector3{0.0f, 0.0f, 0.0f}, true);

	CHECK(ContactsIn(store) == 0);
}

TEST_CASE("a raycast hits a hull where the hull is", "[bakedshapes]") {
	// The query path, which is a separate switch from the contact path and
	// therefore a separate way to have forgotten the new kinds.
	std::unique_ptr<Store> owned = World();
	Store &store = *owned;

	CollisionShapes shapes;
	shapes.SetHull(Name("crate"), engine::collision::BuildConvexHull(BoxCorners(0.5f)));
	store.SetResource(shapes);

	Baked(store, ShapeKind::Hull, Name("crate"), Vector3{4.0f, 0.0f, 0.0f}, true);
	SyncBroadphase(store);

	const std::optional<ColliderHit> hit =
		Raycast(store, engine::core::Ray{Vector3::Zero, Vector3{1.0f, 0.0f, 0.0f}}, 20.0f);

	REQUIRE(hit.has_value());

	// The hull's near face is at 3.5, not the part's own extent at 3.0.
	CHECK(hit->Distance == Approx(3.5f).margin(1e-2f));
	CHECK(hit->Normal.X == Approx(-1.0f).margin(1e-2f));
}

TEST_CASE("a raycast hits a mesh at its surface", "[bakedshapes]") {
	std::unique_ptr<Store> owned = World();
	Store &store = *owned;

	CollisionShapes shapes;
	shapes.SetMesh(Name("terrain"), Ground(8.0f));
	store.SetResource(shapes);

	Baked(store, ShapeKind::Mesh, Name("terrain"), Vector3{0.0f, 0.0f, 0.0f}, true);
	SyncBroadphase(store);

	const std::optional<ColliderHit> hit =
		Raycast(store, engine::core::Ray{Vector3{1.0f, 5.0f, 1.0f}, Vector3{0.0f, -1.0f, 0.0f}}, 20.0f);

	REQUIRE(hit.has_value());
	CHECK(hit->Distance == Approx(5.0f).margin(1e-2f));

	// **Turned to face the ray**, because a soup has no inside: the triangle's
	// winding says which way it was modelled and a caller raycasting terrain
	// wants the surface it hit.
	CHECK(hit->Normal.Y == Approx(1.0f).margin(1e-2f));
}

TEST_CASE("a shape table replaces a name rather than shadowing it", "[bakedshapes]") {
	// Two rows for one name is a lookup whose answer depends on which was found
	// first, and the case that produces it is a world reloading content it
	// already had.
	CollisionShapes shapes;
	shapes.SetHull(Name("crate"), engine::collision::BuildConvexHull(BoxCorners(0.5f)));
	shapes.SetHull(Name("crate"), engine::collision::BuildConvexHull(BoxCorners(2.0f)));

	CHECK(shapes.HullCount() == 1);
	REQUIRE(shapes.FindHull(Name("crate")) != nullptr);
	CHECK(shapes.FindHull(Name("crate"))->Bounds.Maximum.X == Approx(2.0f));

	// An invalid name matches nothing, which is `SurfaceTable::Find`'s first
	// line and the same trap: a collider that never named a shape would
	// otherwise resolve to whatever was registered first without one.
	CHECK(shapes.FindHull(Name()) == nullptr);
	CHECK(shapes.FindMesh(Name("crate")) == nullptr);
}

TEST_CASE("a part states which shape it collides as", "[bakedshapes]") {
	// **The authoring surface, and until v0.17 there was none.** A part is drawn
	// at its `Bounds` and collides at its `Collider`, and the second of those was
	// unreachable from a script or the properties panel at all - a box was the
	// only collider a part could ever have without a C++ call.
	Store store("physics.bakedshapes.properties");
	engine::scene::RegisterSceneComponents();
	engine::scene::RegisterSceneClasses();

	const Entity part = store.CreateInstance(engine::ecs::Classes::Find(Name("Part")), "Rock");
	REQUIRE(part != engine::ecs::NULL_ENTITY);

	// **The declared type is checked and not only the round-trip**, for
	// `TagFilter`'s reason one module over: writing raw bytes through
	// `SetProperty` succeeds whatever the descriptor claims, so a wrongly
	// declared property passes every test until the first script assigns to it.
	bool shapeDeclared = false;
	bool geometryDeclared = false;
	for (const engine::ecs::PropertyDescriptor &property : store.PropertiesOf(part)) {
		if (property.Name == Name("CollisionShape")) {
			shapeDeclared = true;
			CHECK(property.Type == engine::ecs::PropertyType::Enum);
			CHECK(property.EnumName == Name("ShapeKind"));
		}
		if (property.Name == Name("CollisionGeometry")) {
			geometryDeclared = true;
			CHECK(property.Type == engine::ecs::PropertyType::Name);
		}
	}
	CHECK(shapeDeclared);
	CHECK(geometryDeclared);

	// A part starts as a box, which is what every scene authored before this
	// was.
	Name read;
	REQUIRE(store.GetProperty(part, Name("CollisionShape"), &read, sizeof(read)));
	CHECK(read == Name("Box"));

	const Name hull("Hull");
	REQUIRE(store.SetProperty(part, Name("CollisionShape"), &hull, sizeof(hull)));
	CHECK(store.Get<Collider>(part)->Shape == ShapeKind::Hull);

	REQUIRE(store.GetProperty(part, Name("CollisionShape"), &read, sizeof(read)));
	CHECK(read == hull);

	// **A member the enum does not have is refused**, which is what an enum
	// property buys over a bare integer: a typo is an error at the assignment
	// rather than a part that silently became a cylinder.
	const Name nonsense("Hexahedron");
	CHECK_FALSE(store.SetProperty(part, Name("CollisionShape"), &nonsense, sizeof(nonsense)));
	CHECK(store.Get<Collider>(part)->Shape == ShapeKind::Hull);

	// Any geometry name is accepted, including one nothing has baked - a scene
	// is authored before its content finishes streaming, and a setter that
	// refused an unresolved name would make the order the two arrive in
	// load-bearing.
	const Name geometry("rock_01");
	REQUIRE(store.SetProperty(part, Name("CollisionGeometry"), &geometry, sizeof(geometry)));
	CHECK(store.Get<Collider>(part)->Geometry == geometry);

	REQUIRE(store.GetProperty(part, Name("CollisionGeometry"), &read, sizeof(read)));
	CHECK(read == geometry);
}

TEST_CASE("the shape enum is registered in its own declaration order", "[bakedshapes]") {
	// **The one way `CollisionShapeProperty` can be silently wrong.** It
	// converts between an ordinal and the enum by casting, so a member list
	// written in a different order than `ShapeKind` declares would make every
	// part a different shape than it says - and every round-trip would still
	// pass, because both halves would be wrong the same way.
	engine::scene::RegisterSceneComponents();
	engine::scene::RegisterSceneClasses();

	const Name shapeKind("ShapeKind");
	CHECK(engine::ecs::EnumTable::MemberAt(shapeKind, static_cast<size_t>(ShapeKind::Box)) == Name("Box"));
	CHECK(
		engine::ecs::EnumTable::MemberAt(shapeKind, static_cast<size_t>(ShapeKind::Sphere)) == Name("Sphere")
	);
	CHECK(
		engine::ecs::EnumTable::MemberAt(shapeKind, static_cast<size_t>(ShapeKind::Cylinder)) ==
		Name("Cylinder")
	);
	CHECK(engine::ecs::EnumTable::MemberAt(shapeKind, static_cast<size_t>(ShapeKind::Hull)) == Name("Hull"));
	CHECK(engine::ecs::EnumTable::MemberAt(shapeKind, static_cast<size_t>(ShapeKind::Mesh)) == Name("Mesh"));
	CHECK(
		engine::ecs::EnumTable::MemberAt(shapeKind, static_cast<size_t>(ShapeKind::Capsule)) ==
		Name("Capsule")
	);
}
