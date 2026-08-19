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
#include <engine/scene/EditableMesh.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

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
using engine::scene::EditableMeshContentName;
using engine::scene::RemoveTriangle;
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
