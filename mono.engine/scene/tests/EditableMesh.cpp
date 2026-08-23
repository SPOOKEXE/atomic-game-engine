// The mesh a script builds a triangle at a time, and the arithmetic that
// keeps its ids meaning what they said they would.
//
// **What is pinned here is the storage `client::UpdateEditableMeshes` reads
// to build an `assets::MeshData` from - that half is L12 and this is what it
// consumes.** A vertex id survives every edit but `Clear`; a triangle id
// survives every edit except removing an earlier one, which is what
// `RemoveTriangle`'s swap-and-pop test exists to make explicit rather than
// discovered.

#include <engine/ecs/Store.hpp>
#include <engine/scene/CollisionShapes.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/EditableMesh.hpp>
#include <engine/scene/Enums.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>

TEST_SUITE_ID("engine.scene.editablemesh")

using Catch::Approx;
using engine::core::Color3;
using engine::core::Name;
using engine::core::Vector2;
using engine::core::Vector3;
using engine::ecs::Entity;
using engine::ecs::NULL_ENTITY;
using engine::ecs::Store;
using engine::scene::AddTriangle;
using engine::scene::AddVertex;
using engine::scene::ClearEditableMesh;
using engine::scene::EditableMesh;
using engine::scene::EditableMeshClass;
using engine::scene::EditableMeshCommit;
using engine::scene::EditableMeshContentName;
using engine::scene::EditableMeshGeometry;
using engine::scene::PrepareEditableMesh;
using engine::scene::RemoveTriangle;
using engine::scene::ReplaceEditableMesh;
using engine::scene::SetVertexColor;
using engine::scene::SetVertexNormal;
using engine::scene::SetVertexPosition;
using engine::scene::SetVertexUV;

namespace {
	Entity MakeEditableMesh(Store &store) {
		return store.CreateInstance(EditableMeshClass(), "Mesh");
	}
}

TEST_CASE("AddVertex returns a stable id and defaults sensibly", "[scene][editablemesh]") {
	Store store("editablemesh.vertex");
	const Entity mesh = MakeEditableMesh(store);

	const auto first = AddVertex(store, mesh, Vector3{1.0f, 2.0f, 3.0f});
	REQUIRE(first.has_value());
	CHECK(*first == 0);

	const auto second =
		AddVertex(store, mesh, Vector3{4.0f, 5.0f, 6.0f}, Vector3{0.0f, 0.0f, 1.0f}, Vector2{0.5f, 0.5f});
	REQUIRE(second.has_value());
	CHECK(*second == 1);

	const EditableMesh *held = store.Get<EditableMesh>(mesh);
	REQUIRE(held != nullptr);
	REQUIRE(held->Positions.size() == 2);
	CHECK(held->Positions[0].X == Approx(1.0f));
	CHECK(held->Normals[0].Y == Approx(1.0f)); // The default: straight up.
	CHECK(held->UVs[1].X == Approx(0.5f));
	CHECK(held->Revision == 2);
}

TEST_CASE("a triangle needs three vertices that already exist", "[scene][editablemesh]") {
	Store store("editablemesh.triangle");
	const Entity mesh = MakeEditableMesh(store);

	AddVertex(store, mesh, Vector3{0.0f, 0.0f, 0.0f});
	AddVertex(store, mesh, Vector3{1.0f, 0.0f, 0.0f});

	// Only two vertices exist; the third id names nothing.
	CHECK_FALSE(AddTriangle(store, mesh, 0, 1, 2).has_value());

	AddVertex(store, mesh, Vector3{0.0f, 1.0f, 0.0f});
	const auto triangle = AddTriangle(store, mesh, 0, 1, 2);
	REQUIRE(triangle.has_value());
	CHECK(*triangle == 0);

	const EditableMesh *held = store.Get<EditableMesh>(mesh);
	REQUIRE(held->Indices.size() == 3);
	CHECK(held->Indices[0] == 0);
	CHECK(held->Indices[1] == 1);
	CHECK(held->Indices[2] == 2);
}

TEST_CASE("removing a triangle moves the last one into its place", "[scene][editablemesh]") {
	// **The swap-and-pop, made visible rather than assumed.** Three
	// triangles over the same three vertices, distinguished only by winding,
	// so the test can tell them apart by which indices land where.
	Store store("editablemesh.remove");
	const Entity mesh = MakeEditableMesh(store);
	for (int i = 0; i < 4; i++) {
		AddVertex(store, mesh, Vector3{static_cast<float>(i), 0.0f, 0.0f});
	}

	REQUIRE(AddTriangle(store, mesh, 0, 1, 2) == 0u);
	REQUIRE(AddTriangle(store, mesh, 1, 2, 3) == 1u);
	REQUIRE(AddTriangle(store, mesh, 2, 3, 0) == 2u);

	// Removing the first moves the last (id 2) into its place.
	REQUIRE(RemoveTriangle(store, mesh, 0));

	const EditableMesh *held = store.Get<EditableMesh>(mesh);
	REQUIRE(held->Indices.size() == 6);
	CHECK(held->Indices[0] == 2);
	CHECK(held->Indices[1] == 3);
	CHECK(held->Indices[2] == 0);
	CHECK(held->Indices[3] == 1);
	CHECK(held->Indices[4] == 2);
	CHECK(held->Indices[5] == 3);

	// Out of range is refused rather than silently doing nothing wrong.
	CHECK_FALSE(RemoveTriangle(store, mesh, 5));
}

TEST_CASE("removing the last triangle needs no swap and still shrinks", "[scene][editablemesh]") {
	Store store("editablemesh.removelast");
	const Entity mesh = MakeEditableMesh(store);
	AddVertex(store, mesh, Vector3{});
	AddVertex(store, mesh, Vector3{});
	AddVertex(store, mesh, Vector3{});

	AddTriangle(store, mesh, 0, 1, 2);
	REQUIRE(RemoveTriangle(store, mesh, 0));

	CHECK(store.Get<EditableMesh>(mesh)->Indices.empty());
}

TEST_CASE(
	"per-vertex attributes are set independently and out of range is refused", "[scene][editablemesh]"
) {
	Store store("editablemesh.attributes");
	const Entity mesh = MakeEditableMesh(store);
	AddVertex(store, mesh, Vector3{});

	REQUIRE(SetVertexPosition(store, mesh, 0, Vector3{9.0f, 8.0f, 7.0f}));
	REQUIRE(SetVertexNormal(store, mesh, 0, Vector3{1.0f, 0.0f, 0.0f}));
	REQUIRE(SetVertexUV(store, mesh, 0, Vector2{0.25f, 0.75f}));
	REQUIRE(SetVertexColor(store, mesh, 0, Color3{0.1f, 0.2f, 0.3f}, 0.4f));

	CHECK_FALSE(SetVertexPosition(store, mesh, 1, Vector3{}));
	CHECK_FALSE(SetVertexNormal(store, mesh, 1, Vector3{}));
	CHECK_FALSE(SetVertexUV(store, mesh, 1, Vector2{}));
	CHECK_FALSE(SetVertexColor(store, mesh, 1, Color3{}, 0.0f));

	const EditableMesh *held = store.Get<EditableMesh>(mesh);
	CHECK(held->Positions[0].X == Approx(9.0f));
	CHECK(held->Normals[0].X == Approx(1.0f));
	CHECK(held->UVs[0].Y == Approx(0.75f));
	CHECK(held->Colours[0].B == Approx(0.3f));
	CHECK(held->Alphas[0] == Approx(0.4f));
}

TEST_CASE("Clear empties every array and bumps the revision once", "[scene][editablemesh]") {
	Store store("editablemesh.clear");
	const Entity mesh = MakeEditableMesh(store);
	AddVertex(store, mesh, Vector3{});
	AddVertex(store, mesh, Vector3{});
	AddTriangle(store, mesh, 0, 1, 0);

	const uint32_t before = store.Get<EditableMesh>(mesh)->Revision;
	REQUIRE(ClearEditableMesh(store, mesh));

	const EditableMesh *held = store.Get<EditableMesh>(mesh);
	CHECK(held->Positions.empty());
	CHECK(held->Indices.empty());
	CHECK(held->Revision == before + 1);
}

TEST_CASE("bulk geometry is validated and committed as one revision", "[scene][editablemesh]") {
	Store store("editablemesh.bulk");
	const Entity mesh = MakeEditableMesh(store);

	EditableMeshGeometry geometry;
	geometry.Positions = {Vector3{}, Vector3{1.0f, 0.0f, 0.0f}, Vector3{0.0f, 0.0f, 1.0f}};
	geometry.Normals.assign(3, Vector3{0.0f, 1.0f, 0.0f});
	geometry.UVs.assign(3, Vector2{});
	geometry.Colours.assign(3, Color3{0.2f, 0.4f, 0.6f});
	geometry.Alphas.assign(3, 0.25f);
	geometry.Indices = {0, 2, 1};

	CHECK(ReplaceEditableMesh(store, mesh, geometry) == EditableMeshCommit::Applied);
	const EditableMesh *held = store.Get<EditableMesh>(mesh);
	REQUIRE(held != nullptr);
	CHECK(held->Revision == 1);
	CHECK(held->Signature != 0);
	CHECK(held->Positions == geometry.Positions);
	CHECK(held->Indices == geometry.Indices);

	// Exact content is not a change. This is what prevents a repeated graph
	// result from invalidating resident GPU and collision data.
	CHECK(ReplaceEditableMesh(store, mesh, geometry) == EditableMeshCommit::Unchanged);
	CHECK(store.Get<EditableMesh>(mesh)->Revision == 1);

	geometry.Positions[0].Y = 3.0f;
	CHECK(ReplaceEditableMesh(store, mesh, geometry) == EditableMeshCommit::Applied);
	CHECK(store.Get<EditableMesh>(mesh)->Revision == 2);
}

TEST_CASE("bulk geometry refuses malformed and stale transactions", "[scene][editablemesh]") {
	Store store("editablemesh.bulk.refuse");
	const Entity mesh = MakeEditableMesh(store);

	EditableMeshGeometry malformed;
	malformed.Positions.assign(3, Vector3{});
	malformed.Normals.assign(2, Vector3{});
	malformed.UVs.assign(3, Vector2{});
	malformed.Colours.assign(3, Color3{});
	malformed.Alphas.assign(3, 0.0f);
	malformed.Indices = {0, 1, 9};
	CHECK_FALSE(PrepareEditableMesh(std::move(malformed)).Valid);

	EditableMeshGeometry geometry;
	geometry.Positions.assign(3, Vector3{});
	geometry.Normals.assign(3, Vector3{0.0f, 1.0f, 0.0f});
	geometry.UVs.assign(3, Vector2{});
	geometry.Colours.assign(3, Color3{1.0f, 1.0f, 1.0f});
	geometry.Alphas.assign(3, 0.0f);
	geometry.Indices = {0, 1, 2};
	auto prepared = PrepareEditableMesh(std::move(geometry));
	REQUIRE(prepared.Valid);

	REQUIRE(AddVertex(store, mesh, Vector3{}).has_value());
	CHECK(
		engine::scene::CommitEditableMesh(store, mesh, std::move(prepared), 0) == EditableMeshCommit::Stale
	);
	CHECK(store.Get<EditableMesh>(mesh)->Positions.size() == 1);
}

TEST_CASE("writing the same vertex attributes does not invalidate geometry", "[scene][editablemesh]") {
	Store store("editablemesh.noop");
	const Entity mesh = MakeEditableMesh(store);
	REQUIRE(AddVertex(store, mesh, Vector3{1.0f, 2.0f, 3.0f}).has_value());
	const uint32_t revision = store.Get<EditableMesh>(mesh)->Revision;

	CHECK(SetVertexPosition(store, mesh, 0, Vector3{1.0f, 2.0f, 3.0f}));
	CHECK(SetVertexNormal(store, mesh, 0, Vector3{0.0f, 1.0f, 0.0f}));
	CHECK(SetVertexUV(store, mesh, 0, Vector2{}));
	CHECK(SetVertexColor(store, mesh, 0, Color3{1.0f, 1.0f, 1.0f}, 0.0f));
	CHECK(store.Get<EditableMesh>(mesh)->Revision == revision);
}

TEST_CASE("every door refuses an instance that is not an EditableMesh", "[scene][editablemesh]") {
	// **The registration first, because this is the one case here that never
	// makes a mesh.** Every other case opens with `MakeEditableMesh`, which
	// reaches `EnsureClassTree` and registers `scene.EditableMesh` under its
	// explicit name. This one only ever asks whether an entity *has* the
	// component - and `Store::Get<T>` mints the id under the compiler's
	// spelling as a side effect of looking, so running it first left
	// `engine::scene::EditableMesh` registered and aborted the next case to
	// reach the explicit name with "a type has one name".
	//
	// It fired on about one seed in forty, in whichever case the shuffle put
	// after this one, which is what made it read as a flake in an unrelated
	// suite. Reproduce it with:
	//
	//     test_scene "[editablemesh]" --order rand --rng-seed 3221043763
	engine::scene::RegisterSceneComponents();

	Store store("editablemesh.wrongtype");
	const Entity notAMesh = store.Create();

	CHECK_FALSE(AddVertex(store, notAMesh, Vector3{}).has_value());
	CHECK_FALSE(AddTriangle(store, notAMesh, 0, 0, 0).has_value());
	CHECK_FALSE(RemoveTriangle(store, notAMesh, 0));
	CHECK_FALSE(SetVertexPosition(store, notAMesh, 0, Vector3{}));
	CHECK_FALSE(ClearEditableMesh(store, notAMesh));
	CHECK_FALSE(EditableMeshContentName(store, notAMesh).IsValid());
	CHECK_FALSE(EditableMeshContentName(store, NULL_ENTITY).IsValid());
}

TEST_CASE("the content name is stable and distinct per instance", "[scene][editablemesh]") {
	Store store("editablemesh.contentname");
	const Entity a = MakeEditableMesh(store);
	const Entity b = MakeEditableMesh(store);

	const Name nameA = EditableMeshContentName(store, a);
	const Name nameB = EditableMeshContentName(store, b);
	REQUIRE(nameA.IsValid());
	REQUIRE(nameB.IsValid());
	CHECK(nameA != nameB);

	// Asking twice for the same instance answers the same name - a script
	// re-reading `ContentId` after a reload must get back what it wrote to
	// `MeshId` the first time.
	CHECK(EditableMeshContentName(store, a) == nameA);
}

// --- collision ---------------------------------------------------------------
//
// **The gap this closes was that a script built something that could be seen
// and not touched.** The uploader hands a run-time mesh to the renderer and
// registered nothing with `CollisionShapes`, so a `MeshPart` naming one fell
// back to colliding as its own bound - a box the size of the whole thing.

namespace {
	// A unit quad on the ground plane, centred on the mesh's own origin.
	//
	// Centred, because a baked shape is used in the part's *object* space and is
	// not scaled to it - `physics::ShapeInstance` states that - so a mesh built
	// around the origin is one whose collider lands where its geometry is.
	Entity MakeQuad(Store &store) {
		const Entity mesh = MakeEditableMesh(store);
		(void)AddVertex(store, mesh, Vector3{-1.0f, 0.0f, -1.0f});
		(void)AddVertex(store, mesh, Vector3{1.0f, 0.0f, -1.0f});
		(void)AddVertex(store, mesh, Vector3{-1.0f, 0.0f, 1.0f});
		(void)AddVertex(store, mesh, Vector3{1.0f, 0.0f, 1.0f});
		(void)AddTriangle(store, mesh, 0, 2, 1);
		(void)AddTriangle(store, mesh, 1, 2, 3);
		return mesh;
	}
}

TEST_CASE("a run-time mesh gets a hull and a soup", "[scene][editablemesh]") {
	engine::scene::RegisterSceneComponents();

	Store store("editablemesh.collision");
	const Entity mesh = MakeQuad(store);
	const Name name = EditableMeshContentName(store, mesh);

	// Nothing until something bakes, which is the state the report was about.
	CHECK(engine::scene::CollisionShapesOf(store) == nullptr);

	CHECK(engine::scene::RefreshEditableMeshCollision(store) == 1);

	const engine::scene::CollisionShapes *shapes = engine::scene::CollisionShapesOf(store);
	REQUIRE(shapes != nullptr);
	REQUIRE(shapes->FindMesh(name) != nullptr);
	CHECK(shapes->FindMesh(name)->TriangleCount() == 2);

	// **And no hull, because nothing asked for one.** Measured on a terrain
	// chunk of 4,225 points: the soup costs 1.3 ms and quickhull costs 7.3, and
	// a heightfield's convex hull is a dome over its summit. A delivered mesh
	// is baked once at load and can afford both; this runs on the tick a script
	// builds geometry, and a streamed world builds one a tick.
	CHECK(shapes->FindHull(name) == nullptr);
}

TEST_CASE("a hull is baked for the part that asks for one", "[scene][editablemesh]") {
	engine::scene::RegisterSceneComponents();

	Store store("editablemesh.collision.hull");
	const Entity mesh = MakeQuad(store);
	const Name name = EditableMeshContentName(store, mesh);

	REQUIRE(engine::scene::RefreshEditableMeshCollision(store) == 1);
	REQUIRE(engine::scene::CollisionShapesOf(store)->FindHull(name) == nullptr);

	// A part switched to `Hull` after its mesh was baked. The revision has not
	// moved, so the test that decides whether to bake cannot be "has this
	// changed" - it is "is a hull wanted and missing".
	const Entity part = store.Create();
	engine::scene::Collider collider;
	collider.Shape = engine::scene::ShapeKind::Hull;
	collider.Geometry = name;
	store.Set(part, collider);

	CHECK(engine::scene::RefreshEditableMeshCollision(store) == 1);

	const engine::scene::CollisionShapes *shapes = engine::scene::CollisionShapesOf(store);
	REQUIRE(shapes != nullptr);
	REQUIRE(shapes->FindHull(name) != nullptr);
	CHECK(shapes->FindHull(name)->Points.size() >= 3);

	// And it is not built again on the next tick, which is the whole point of
	// asking whether it is missing rather than whether it is wanted.
	CHECK(engine::scene::RefreshEditableMeshCollision(store) == 0);
}

TEST_CASE("a mesh whose revision has not moved is not rebaked", "[scene][editablemesh]") {
	engine::scene::RegisterSceneComponents();

	Store store("editablemesh.collision.steady");
	const Entity mesh = MakeQuad(store);

	CHECK(engine::scene::RefreshEditableMeshCollision(store) == 1);

	// The steady state, which is what makes this affordable in a world that
	// builds a mesh a frame: an integer compare per mesh and no quickhull.
	CHECK(engine::scene::RefreshEditableMeshCollision(store) == 0);

	// An edit moves the revision, so the shape is built again.
	CHECK(SetVertexPosition(store, mesh, 0, Vector3{-2.0f, 0.0f, -2.0f}));
	CHECK(engine::scene::RefreshEditableMeshCollision(store) == 1);
}

TEST_CASE("changed mesh collision is baked as one deterministic batch", "[scene][editablemesh]") {
	engine::scene::RegisterSceneComponents();

	Store store("editablemesh.collision.batch");
	const std::array meshes{MakeQuad(store), MakeQuad(store), MakeQuad(store)};

	CHECK(engine::scene::RefreshEditableMeshCollision(store) == meshes.size());
	const engine::scene::CollisionShapes *shapes = engine::scene::CollisionShapesOf(store);
	REQUIRE(shapes != nullptr);
	for (const Entity mesh : meshes) {
		const auto *shape = shapes->FindMesh(EditableMeshContentName(store, mesh));
		REQUIRE(shape != nullptr);
		CHECK(shape->TriangleCount() == 2);
	}

	// Worker results are published on the owner thread and the revision ledger
	// advances with them, so the next refresh has no work to repeat.
	CHECK(engine::scene::RefreshEditableMeshCollision(store) == 0);
}

TEST_CASE("a mesh that is gone takes its shapes with it", "[scene][editablemesh]") {
	engine::scene::RegisterSceneComponents();

	Store store("editablemesh.collision.forget");
	const Entity mesh = MakeQuad(store);
	const Name name = EditableMeshContentName(store, mesh);

	REQUIRE(engine::scene::RefreshEditableMeshCollision(store) == 1);
	REQUIRE(engine::scene::CollisionShapesOf(store)->FindMesh(name) != nullptr);

	// **A streamed world creates and destroys a mesh per chunk**, so a table
	// that only ever grew would hold a hull and a soup for every chunk anybody
	// walked past. Nothing can name them again - the content name carries the
	// entity's generation.
	store.Destroy(mesh);
	CHECK(engine::scene::RefreshEditableMeshCollision(store) == 1);

	const engine::scene::CollisionShapes *shapes = engine::scene::CollisionShapesOf(store);
	REQUIRE(shapes != nullptr);
	CHECK(shapes->FindMesh(name) == nullptr);
	CHECK(shapes->FindHull(name) == nullptr);
	CHECK(shapes->MeshCount() == 0);
	CHECK(shapes->HullCount() == 0);
}

TEST_CASE("a mesh with no triangles yet bakes nothing", "[scene][editablemesh]") {
	engine::scene::RegisterSceneComponents();

	Store store("editablemesh.collision.empty");
	const Entity mesh = MakeEditableMesh(store);
	(void)AddVertex(store, mesh, Vector3{0.0f, 0.0f, 0.0f});

	// Vertices added and no triangle yet is the ordinary state right after
	// `Instance.new("EditableMesh")`, and a hull of one point is a collider
	// that stops nothing and says nothing.
	CHECK(engine::scene::RefreshEditableMeshCollision(store) == 0);
	CHECK(engine::scene::CollisionShapesOf(store) == nullptr);

	// And it is tried again rather than remembered as done.
	(void)AddVertex(store, mesh, Vector3{1.0f, 0.0f, 0.0f});
	(void)AddVertex(store, mesh, Vector3{0.0f, 0.0f, 1.0f});
	(void)AddTriangle(store, mesh, 0, 1, 2);
	CHECK(engine::scene::RefreshEditableMeshCollision(store) == 1);
}
