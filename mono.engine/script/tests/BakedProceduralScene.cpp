#include <engine/assets/ContentHash.hpp>
#include <engine/assets/Mesh.hpp>
#include <engine/assets/ProceduralDescription.hpp>
#include <engine/core/Bytes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/procedural/Artifact.hpp>
#include <engine/procedural/Geometry.hpp>
#include <engine/scene/EditableMesh.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/script/BakedProceduralScene.hpp>
#include <engine/script/ProceduralJobs.hpp>
#include <engine/script/ProceduralScene.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.script.bakedproceduralscene")
TEST_DEPENDS("engine.script.proceduralscene")
TEST_DEPENDS("engine.assets.procedural_description")

using namespace engine;

namespace {
	constexpr int64_t CELL = procedural::SUBCELLS_PER_CELL;
	constexpr procedural::RegionScope SCOPE{0, 0, 16};
	constexpr procedural::RegionScope FAR_SCOPE{20, 0, 16};
	constexpr std::array<procedural::RegionScope, 2> SCOPES{SCOPE, FAR_SCOPE};

	struct SceneFixture {
		ecs::Store Store{"baked-scene"};
		procedural::Diagnostic Diagnostic{};
		SceneFixture() {
			scene::RegisterSceneComponents();
			script::RegisterProceduralComponents();
			const std::array<script::ProceduralSource, 2> sources{
				{{procedural::SourceKind::Village, "oakstead", {8 * CELL, 8 * CELL}, ""},
				 {procedural::SourceKind::Entrance, "west-gate", {12 * CELL, 8 * CELL}, "oakstead"}}
			};
			REQUIRE(
				script::ConfigureProceduralWorld(
					Store,
					"demo",
					procedural::ForestVillageDefaults("forest-village", 42),
					SCOPES,
					sources,
					Diagnostic
				)
			);
		}
		script::ProceduralSceneTarget Target(procedural::RegionScope scope = SCOPE, bool bind = true) {
			const auto mesh = Store.CreateInstance(scene::EditableMeshClass(), "BakedRegion");
			scene::PartDesc description;
			description.Class = ecs::Classes::Find(core::Name("MeshPart"));
			description.Mesh = scene::EditableMeshContentName(Store, mesh);
			const auto part = scene::MakePart(Store, description);
			REQUIRE(part != ecs::NULL_ENTITY);
			auto *collider = Store.GetMutable<scene::Collider>(part);
			collider->Mask = bind ? spatial::LayerMask::None() : spatial::LayerMask::All();
			collider->Trigger = true;
			collider->CanQuery = false;
			script::ProceduralSceneTarget target;
			if (bind)
				REQUIRE(script::BindProceduralSceneTarget(Store, scope, mesh, part, target, Diagnostic));
			else
				REQUIRE(script::CaptureProceduralSceneTarget(Store, scope, mesh, part, target, Diagnostic));
			return target;
		}
	};

	struct OwnedMesh {
		std::string Name;
		std::vector<std::byte> Bytes;
	};
	struct BakedFixture {
		std::vector<std::byte> Description;
		assets::ContentHash DescriptionHash;
		std::vector<OwnedMesh> Owned;
		std::vector<script::BakedProceduralMesh> Views() const {
			std::vector<script::BakedProceduralMesh> views;
			views.reserve(Owned.size());
			for (const auto &mesh : Owned)
				views.push_back({mesh.Name, mesh.Bytes});
			return views;
		}
	};

	BakedFixture FromGeometry(const procedural::RegionGeometry &geometry) {
		BakedFixture baked;
		std::vector<assets::ProceduralPlacement> placements;
		for (const auto &object : geometry.Objects) {
			assets::MeshData data;
			for (const auto &vertex : object.Vertices) {
				assets::MeshVertex converted{};
				for (size_t axis = 0; axis < 3; ++axis) {
					converted.Position[axis] = vertex.Position[axis];
					converted.Normal[axis] = vertex.Normal[axis];
				}
				for (size_t axis = 0; axis < 2; ++axis)
					converted.TexCoord[axis] = vertex.UV[axis];
				data.Vertices.push_back(converted);
			}
			data.Indices = object.Indices;
			assets::Submesh run;
			run.IndexCount = static_cast<uint32_t>(data.Indices.size());
			run.Material = "procedural/" + object.Kind + "/" + std::to_string(object.Material);
			data.Submeshes.push_back(std::move(run));
			REQUIRE(data.IsValid());
			core::ByteWriter writer;
			REQUIRE(assets::Mesh::Write(writer, data));
			auto bytes = writer.TakeBytes();
			const std::string name = "procedural/" + assets::Hasher::Of(bytes).ToHex() + ".amesh";
			const auto found = std::find_if(baked.Owned.begin(), baked.Owned.end(), [&](const auto &row) {
				return row.Name == name;
			});
			if (found == baked.Owned.end()) baked.Owned.push_back({name, std::move(bytes)});
			placements.push_back(
				{object.Id,
				 object.Kind,
				 name,
				 object.Origin,
				 object.Material,
				 object.Variant,
				 object.Centre,
				 object.Size,
				 object.Colour}
			);
		}
		std::sort(baked.Owned.begin(), baked.Owned.end(), [](const auto &a, const auto &b) {
			return procedural::StableNameLess(a.Name, b.Name);
		});
		std::string failure;
		REQUIRE(assets::EncodeProceduralDescription(placements, baked.Description, failure));
		baked.DescriptionHash = assets::Hasher::Of(baked.Description);
		return baked;
	}

	struct Baseline {
		BakedFixture Baked;
		scene::EditableMesh Mesh;
		script::ProceduralSceneRegion Region;
		core::Vector3 Position{};
		core::Vector3 HalfExtent{};
	};

	Baseline PublishAndBake(SceneFixture &fixture, procedural::RegionScope scope = SCOPE) {
		auto target = fixture.Target(scope);
		script::ProceduralJobs jobs(fixture.Store);
		const uint64_t ticket = jobs.Submit(fixture.Store, scope, parallel::JobContext::Serial);
		REQUIRE(ticket != 0);
		jobs.Poll();
		REQUIRE(jobs.PublishSceneBatch(
			fixture.Store, std::span(&ticket, 1), std::span(&target, 1), fixture.Store.Time().Tick
		));
		const auto &published = fixture.Store.Resource<script::ProceduralPublished>()->Regions;
		const auto found = std::find_if(published.begin(), published.end(), [&](const auto &row) {
			return row.Scope.X == scope.X && row.Scope.Y == scope.Y &&
				   row.Scope.CellsPerSide == scope.CellsPerSide;
		});
		REQUIRE(found != published.end());
		const auto &publication = *found;
		procedural::RegionResult result;
		procedural::RegionGeometry geometry;
		REQUIRE(
			procedural::DecodeRegionResult(publication.ArtifactBytes, result, fixture.Diagnostic) ==
			procedural::Status::Ok
		);
		REQUIRE(
			procedural::BuildRegionGeometry(result, scope, geometry, fixture.Diagnostic) ==
			procedural::Status::Ok
		);
		Baseline baseline;
		baseline.Baked = FromGeometry(geometry);
		baseline.Mesh = *fixture.Store.Get<scene::EditableMesh>(target.Mesh);
		baseline.Region = *fixture.Store.Get<script::ProceduralSceneRegion>(target.Mesh);
		baseline.Position = fixture.Store.Get<scene::Transform>(target.Part)->Frame.Position;
		baseline.HalfExtent = fixture.Store.Get<scene::Bounds>(target.Part)->HalfExtent;
		return baseline;
	}

	bool
	Install(SceneFixture &fixture, const script::ProceduralSceneTarget &target, const BakedFixture &baked) {
		const auto views = baked.Views();
		return script::InstallBakedProceduralScene(
			fixture.Store, target, baked.DescriptionHash, baked.Description, views, fixture.Diagnostic
		);
	}

	void CheckSameMesh(const scene::EditableMesh &actual, const scene::EditableMesh &expected) {
		CHECK(actual.Positions == expected.Positions);
		CHECK(actual.Normals == expected.Normals);
		CHECK(actual.UVs == expected.UVs);
		CHECK(actual.Colours == expected.Colours);
		CHECK(actual.Alphas == expected.Alphas);
		CHECK(actual.Indices == expected.Indices);
	}
}

TEST_CASE(
	"ordinary baked assets recreate the accepted RGA scene with no recipe or generator", "[procedural][scene]"
) {
	SceneFixture fixture;
	const auto baseline = PublishAndBake(fixture);
	fixture.Store.RemoveResource<script::ProceduralAuthority>();
	fixture.Store.RemoveResource<script::ProceduralPublished>();
	auto target = fixture.Target(SCOPE, false);
	REQUIRE_FALSE(fixture.Store.Has<script::ProceduralSceneRegion>(target.Mesh));
	REQUIRE(Install(fixture, target, baseline.Baked));
	const auto *mesh = fixture.Store.Get<scene::EditableMesh>(target.Mesh);
	const auto *region = fixture.Store.Get<script::ProceduralSceneRegion>(target.Mesh);
	REQUIRE(mesh);
	REQUIRE(region);
	CheckSameMesh(*mesh, baseline.Mesh);
	REQUIRE(region->Objects.size() == baseline.Region.Objects.size());
	for (size_t index = 0; index < region->Objects.size(); ++index) {
		const auto &actual = region->Objects[index];
		const auto &expected = baseline.Region.Objects[index];
		CHECK(actual.Id == expected.Id);
		CHECK(actual.Kind == expected.Kind);
		CHECK(actual.FirstIndex == expected.FirstIndex);
		CHECK(actual.IndexCount == expected.IndexCount);
		CHECK(actual.Material == expected.Material);
		CHECK(actual.Variant == expected.Variant);
	}
	CHECK(fixture.Store.Get<scene::Transform>(target.Part)->Frame.Position == baseline.Position);
	CHECK(fixture.Store.Get<scene::PreviousTransform>(target.Part)->Frame.Position == baseline.Position);
	CHECK(fixture.Store.Get<scene::Bounds>(target.Part)->HalfExtent == baseline.HalfExtent);
	CHECK(region->ArtifactDigest.Bytes == baseline.Baked.DescriptionHash.Digest);
	CHECK(fixture.Store.Get<scene::Visual>(target.Part)->Fitted == target.Content);
	CHECK(fixture.Store.Get<scene::Collider>(target.Part)->Mask == spatial::LayerMask::None());
}

TEST_CASE(
	"baked closure, bounds and stale target refusals retain the last visible region", "[procedural][scene]"
) {
	SceneFixture fixture;
	const auto baseline = PublishAndBake(fixture);
	auto target = fixture.Target();
	REQUIRE(Install(fixture, target, baseline.Baked));
	const auto old = *fixture.Store.Get<scene::EditableMesh>(target.Mesh);
	const auto oldRegion = *fixture.Store.Get<script::ProceduralSceneRegion>(target.Mesh);
	const auto oldPosition = fixture.Store.Get<scene::Transform>(target.Part)->Frame.Position;
	const auto oldBounds = fixture.Store.Get<scene::Bounds>(target.Part)->HalfExtent;
	REQUIRE(
		script::BindProceduralSceneTarget(
			fixture.Store, SCOPE, target.Mesh, target.Part, target, fixture.Diagnostic
		)
	);

	BakedFixture damaged = baseline.Baked;
	REQUIRE(!damaged.Owned.empty());
	damaged.Owned.front().Bytes[0] ^= std::byte{1};
	CHECK_FALSE(Install(fixture, target, damaged));
	BakedFixture missing = baseline.Baked;
	missing.Owned.pop_back();
	CHECK_FALSE(Install(fixture, target, missing));
	BakedFixture shifted = baseline.Baked;
	std::vector<assets::ProceduralPlacement> placements;
	std::string failure;
	REQUIRE(assets::DecodeProceduralDescription(shifted.Description, placements, failure));
	REQUIRE(!placements.empty());
	placements.front().Origin.X = 16 * CELL;
	REQUIRE(assets::EncodeProceduralDescription(placements, shifted.Description, failure));
	shifted.DescriptionHash = assets::Hasher::Of(shifted.Description);
	CHECK_FALSE(Install(fixture, target, shifted));
	CHECK(fixture.Store.Get<scene::EditableMesh>(target.Mesh)->Revision == old.Revision);
	CheckSameMesh(*fixture.Store.Get<scene::EditableMesh>(target.Mesh), old);
	CHECK(
		fixture.Store.Get<script::ProceduralSceneRegion>(target.Mesh)->Objects.size() ==
		oldRegion.Objects.size()
	);
	CHECK(fixture.Store.Get<scene::Transform>(target.Part)->Frame.Position == oldPosition);
	CHECK(fixture.Store.Get<scene::Bounds>(target.Part)->HalfExtent == oldBounds);
	fixture.Store.GetMutable<scene::Transform>(target.Part)->Frame.Position.X += 1;
	CHECK_FALSE(Install(fixture, target, baseline.Baked));
	CHECK(fixture.Store.Get<scene::EditableMesh>(target.Mesh)->Revision == old.Revision);
}

TEST_CASE(
	"baked scene batch refuses a bad later region before editing an earlier target", "[procedural][scene]"
) {
	SceneFixture fixture;
	const auto near = PublishAndBake(fixture, SCOPE);
	const auto far = PublishAndBake(fixture, FAR_SCOPE);
	std::array<script::ProceduralSceneTarget, 2> targets{
		fixture.Target(SCOPE, false), fixture.Target(FAR_SCOPE, false)
	};
	const auto nearMeshes = near.Baked.Views();
	const auto farMeshes = far.Baked.Views();
	std::array<script::BakedProceduralRegion, 2> regions{
		{{targets[0], near.Baked.DescriptionHash, near.Baked.Description, nearMeshes},
		 {targets[1], far.Baked.DescriptionHash, far.Baked.Description, farMeshes}}
	};
	regions[1].DescriptionHash = {};
	CHECK_FALSE(script::InstallBakedProceduralSceneBatch(fixture.Store, regions, fixture.Diagnostic));
	CHECK(fixture.Store.Get<scene::EditableMesh>(targets[0].Mesh)->Revision == 0);
	CHECK(fixture.Store.Get<scene::EditableMesh>(targets[1].Mesh)->Revision == 0);
	for (const auto &target : targets) {
		CHECK_FALSE(fixture.Store.Has<script::ProceduralSceneRegion>(target.Mesh));
		CHECK(fixture.Store.Get<scene::Visual>(target.Part)->Fitted == target.BeforeFitted);
		CHECK(fixture.Store.Get<scene::Collider>(target.Part)->Mask == target.BeforeMask);
	}
	regions[1].DescriptionHash = far.Baked.DescriptionHash;
	fixture.Store.GetMutable<scene::Visual>(targets[1].Part)->Fitted = targets[1].Content;
	CHECK_FALSE(script::InstallBakedProceduralSceneBatch(fixture.Store, regions, fixture.Diagnostic));
	CHECK_FALSE(fixture.Store.Has<script::ProceduralSceneRegion>(targets[0].Mesh));
	CHECK(fixture.Store.Get<scene::Visual>(targets[0].Part)->Fitted == targets[0].BeforeFitted);
	CHECK(fixture.Store.Get<scene::Collider>(targets[0].Part)->Mask == targets[0].BeforeMask);
	CHECK(fixture.Store.Get<scene::EditableMesh>(targets[0].Mesh)->Revision == 0);
	fixture.Store.GetMutable<scene::Visual>(targets[1].Part)->Fitted = targets[1].BeforeFitted;
	REQUIRE(script::InstallBakedProceduralSceneBatch(fixture.Store, regions, fixture.Diagnostic));
	CheckSameMesh(*fixture.Store.Get<scene::EditableMesh>(targets[0].Mesh), near.Mesh);
	CheckSameMesh(*fixture.Store.Get<scene::EditableMesh>(targets[1].Mesh), far.Mesh);
}
