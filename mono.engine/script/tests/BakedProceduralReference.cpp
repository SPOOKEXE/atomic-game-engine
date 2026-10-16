#include <engine/core/Bytes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/EditableMesh.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/script/BakedProceduralReference.hpp>
#include <engine/script/ProceduralJobs.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>

TEST_SUITE_ID("engine.script.bakedproceduralreference")
TEST_DEPENDS("engine.script.proceduralscene")

using namespace engine;

TEST_CASE("baked reference saves its content name, region and same-world part", "[procedural][content]") {
	scene::RegisterSceneComponents();
	script::RegisterProceduralComponents();
	ecs::Store source("baked-reference");
	const auto mesh = source.CreateInstance(scene::EditableMeshClass(), "Region");
	scene::PartDesc description;
	description.Class = ecs::Classes::Find(core::Name("MeshPart"));
	description.Mesh = scene::EditableMeshContentName(source, mesh);
	const auto part = scene::MakePart(source, description);
	REQUIRE(part != ecs::NULL_ENTITY);
	auto *collider = source.GetMutable<scene::Collider>(part);
	collider->Mask = spatial::LayerMask::None();
	collider->CanQuery = false;
	const procedural::RegionScope scope{3, -2, 16};
	procedural::Diagnostic diagnostic;
	REQUIRE(script::SetBakedProceduralReference(source, scope, mesh, part, "town/region.aproc", diagnostic));
	const auto *reference = source.Get<script::BakedProceduralReference>(mesh);
	REQUIRE(reference);
	CHECK(reference->Description.Text() == "town/region.aproc");
	CHECK(reference->Part == part);
	CHECK(reference->Scope.X == 3);
	CHECK(reference->Scope.Y == -2);
	CHECK(reference->Revision == 1);
	CHECK_FALSE(
		script::SetBakedProceduralReference(source, scope, mesh, part, "../region.aproc", diagnostic)
	);
	CHECK(source.Get<script::BakedProceduralReference>(mesh)->Description.Text() == "town/region.aproc");
	REQUIRE(script::SetBakedProceduralReference(source, scope, mesh, part, "town/region.aproc", diagnostic));
	CHECK(source.Get<script::BakedProceduralReference>(mesh)->Revision == 2);

	core::ByteWriter bytes;
	REQUIRE(source.Save(bytes));
	core::ByteReader reader(bytes.Bytes());
	ecs::Store restored("restored-reference");
	REQUIRE(restored.Load(reader));
	const auto *saved = restored.Get<script::BakedProceduralReference>(mesh);
	REQUIRE(saved);
	CHECK(saved->Description.Text() == "town/region.aproc");
	CHECK(saved->Part == part);
	CHECK(saved->Scope.X == scope.X);
	CHECK(saved->Scope.Y == scope.Y);
	CHECK(saved->Scope.CellsPerSide == scope.CellsPerSide);
	CHECK(saved->Revision == 2);
	auto terminal = *source.Get<script::BakedProceduralReference>(mesh);
	terminal.Revision = std::numeric_limits<uint64_t>::max();
	source.Set(mesh, terminal);
	CHECK_FALSE(
		script::SetBakedProceduralReference(source, scope, mesh, part, "town/region.aproc", diagnostic)
	);
	CHECK(diagnostic.Code == procedural::Status::LimitExceeded);
	CHECK(source.Get<script::BakedProceduralReference>(mesh)->Revision == terminal.Revision);
}

TEST_CASE("baked reference admission caps one world's region count", "[procedural][content]") {
	scene::RegisterSceneComponents();
	script::RegisterProceduralComponents();
	ecs::Store store("bounded-baked-references");
	const procedural::RegionScope scope{0, 0, 16};
	for (size_t index = 0; index < script::MAXIMUM_BAKED_PROCEDURAL_REFERENCES; ++index) {
		const auto mesh = store.CreateInstance(scene::EditableMeshClass(), "Region");
		store.Set(mesh, script::BakedProceduralReference{core::Name("town/region.aproc"), scope, mesh, 1});
	}
	const auto mesh = store.CreateInstance(scene::EditableMeshClass(), "Overflow");
	scene::PartDesc description;
	description.Class = ecs::Classes::Find(core::Name("MeshPart"));
	description.Mesh = scene::EditableMeshContentName(store, mesh);
	const auto part = scene::MakePart(store, description);
	REQUIRE(part != ecs::NULL_ENTITY);
	procedural::Diagnostic diagnostic;
	CHECK_FALSE(
		script::SetBakedProceduralReference(store, scope, mesh, part, "town/region.aproc", diagnostic)
	);
	CHECK(diagnostic.Code == procedural::Status::LimitExceeded);
	CHECK_FALSE(store.Has<script::BakedProceduralReference>(mesh));
}
