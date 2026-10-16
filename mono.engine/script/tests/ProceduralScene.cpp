#include <engine/core/Bytes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/procedural/Artifact.hpp>
#include <engine/procedural/Geometry.hpp>
#include <engine/scene/EditableMesh.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/script/ProceduralScene.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>

TEST_SUITE_ID("engine.script.proceduralscene")
TEST_DEPENDS("engine.script.proceduraljobs")
TEST_DEPENDS("engine.scene.editablemesh")
TEST_DEPENDS("engine.procedural.geometry")

using namespace engine;
namespace {
	constexpr int64_t CELL = procedural::SUBCELLS_PER_CELL;
	constexpr std::array<procedural::RegionScope, 2> SCOPES{{{0, 0, 16}, {20, 0, 16}}};
	struct SceneFixture {
		ecs::Store Store{"procedural-scene"};
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
		script::ProceduralSceneTarget Target(procedural::RegionScope scope) {
			const auto mesh = Store.CreateInstance(scene::EditableMeshClass(), "RegionalMesh");
			scene::PartDesc description;
			description.Class = ecs::Classes::Find(core::Name("MeshPart"));
			description.Mesh = scene::EditableMeshContentName(Store, mesh);
			const auto part = scene::MakePart(Store, description);
			REQUIRE(part != ecs::NULL_ENTITY);
			auto *collider = Store.GetMutable<scene::Collider>(part);
			collider->Mask = spatial::LayerMask::None();
			collider->CanQuery = false;
			script::ProceduralSceneTarget target;
			REQUIRE(script::BindProceduralSceneTarget(Store, scope, mesh, part, target, Diagnostic));
			return target;
		}
		void Rebind(script::ProceduralSceneTarget &target) {
			REQUIRE(
				script::BindProceduralSceneTarget(
					Store, target.Scope, target.Mesh, target.Part, target, Diagnostic
				)
			);
		}
	};
	uint64_t Ready(script::ProceduralJobs &jobs, ecs::Store &store, procedural::RegionScope scope) {
		const auto ticket = jobs.Submit(store, scope, parallel::JobContext::Serial);
		REQUIRE(ticket != 0);
		jobs.Poll();
		return ticket;
	}
}
TEST_CASE(
	"regional scene publication includes every visible kind with exact aggregate bounds",
	"[procedural][scene]"
) {
	SceneFixture fixture;
	auto target = fixture.Target(SCOPES[0]);
	script::ProceduralJobs jobs(fixture.Store);
	const std::array<uint64_t, 1> tickets{Ready(jobs, fixture.Store, target.Scope)};
	REQUIRE(jobs.PublishSceneBatch(fixture.Store, tickets, std::span(&target, 1), fixture.Store.Time().Tick));
	const auto *mesh = fixture.Store.Get<scene::EditableMesh>(target.Mesh);
	const auto *region = fixture.Store.Get<script::ProceduralSceneRegion>(target.Mesh);
	const auto *published = fixture.Store.Resource<script::ProceduralPublished>();
	REQUIRE(mesh);
	REQUIRE(region);
	REQUIRE(published);
	REQUIRE(published->Regions.size() == 1);
	REQUIRE(!mesh->Positions.empty());
	CHECK(region->ArtifactDigest == published->Regions.front().ArtifactDigest);
	for (const auto kind : {"tree", "house", "village-lot", "entrance", "road"})
		CHECK(std::any_of(region->Objects.begin(), region->Objects.end(), [&](const auto &object) {
			return object.Kind == kind;
		}));
	procedural::RegionResult result;
	procedural::RegionGeometry geometry;
	REQUIRE(
		procedural::DecodeRegionResult(
			published->Regions.front().ArtifactBytes, result, fixture.Diagnostic
		) == procedural::Status::Ok
	);
	REQUIRE(
		procedural::BuildRegionGeometry(result, target.Scope, geometry, fixture.Diagnostic) ==
		procedural::Status::Ok
	);
	const auto centre = procedural::GeometryWorldCentre(geometry);
	CHECK(
		fixture.Store.Get<scene::Transform>(target.Part)->Frame.Position ==
		core::Vector3{centre[0], centre[1], centre[2]}
	);
	CHECK(
		fixture.Store.Get<scene::PreviousTransform>(target.Part)->Frame.Position ==
		core::Vector3{centre[0], centre[1], centre[2]}
	);
	CHECK(
		fixture.Store.Get<scene::Bounds>(target.Part)->HalfExtent ==
		core::Vector3{geometry.Size[0] * 0.5f, geometry.Size[1] * 0.5f, geometry.Size[2] * 0.5f}
	);
	size_t vertex = 0, index = 0;
	REQUIRE(region->Objects.size() == geometry.Objects.size());
	for (size_t objectIndex = 0; objectIndex < geometry.Objects.size(); ++objectIndex) {
		const auto &object = geometry.Objects[objectIndex];
		const auto &range = region->Objects[objectIndex];
		CHECK(range.Id == object.Id);
		CHECK(range.FirstIndex == index);
		CHECK(range.IndexCount == object.Indices.size());
		for (const auto &source : object.Vertices) {
			const auto expected =
				procedural::GeometryPositionRelativeTo(object, geometry.Origin, source.Position);
			CHECK(mesh->Positions[vertex] == core::Vector3{expected[0], expected[1], expected[2]});
			CHECK(
				mesh->Colours[vertex] == core::Color3{object.Colour[0], object.Colour[1], object.Colour[2]}
			);
			++vertex;
		}
		index += object.Indices.size();
	}
	CHECK(vertex == mesh->Positions.size());
	CHECK(index == mesh->Indices.size());
	CHECK(jobs.Completions().front().Status == script::ProceduralJobStatus::Published);
}
TEST_CASE("a stale scene target refuses all meshes and canonical receipts together", "[procedural][scene]") {
	SceneFixture fixture;
	std::array<script::ProceduralSceneTarget, 2> targets{
		fixture.Target(SCOPES[0]), fixture.Target(SCOPES[1])
	};
	script::ProceduralJobs jobs(fixture.Store);
	const std::array<uint64_t, 2> tickets{
		Ready(jobs, fixture.Store, SCOPES[0]), Ready(jobs, fixture.Store, SCOPES[1])
	};
	fixture.Store.GetMutable<scene::Transform>(targets[1].Part)->Frame.Position.X = 42;
	CHECK_FALSE(jobs.PublishSceneBatch(fixture.Store, tickets, targets, fixture.Store.Time().Tick));
	CHECK(fixture.Store.Resource<script::ProceduralPublished>()->Regions.empty());
	CHECK(fixture.Store.Get<scene::EditableMesh>(targets[0].Mesh)->Revision == 0);
	CHECK(fixture.Store.Get<script::ProceduralSceneRegion>(targets[0].Mesh)->Objects.empty());
	fixture.Rebind(targets[1]);
	REQUIRE(jobs.PublishSceneBatch(fixture.Store, tickets, targets, fixture.Store.Time().Tick));
	CHECK(fixture.Store.Resource<script::ProceduralPublished>()->Regions.size() == 2);
	CHECK(fixture.Store.Get<scene::EditableMesh>(targets[0].Mesh)->Revision == 1);
}
TEST_CASE("canceled or source-stale scene work preserves the last accepted output", "[procedural][scene]") {
	SceneFixture fixture;
	auto target = fixture.Target(SCOPES[0]);
	script::ProceduralJobs jobs(fixture.Store);
	std::array<uint64_t, 1> tickets{Ready(jobs, fixture.Store, target.Scope)};
	REQUIRE(jobs.PublishSceneBatch(fixture.Store, tickets, std::span(&target, 1), fixture.Store.Time().Tick));
	const auto old = fixture.Store.Resource<script::ProceduralPublished>()->Regions.front().ArtifactDigest;
	const auto revision = fixture.Store.Get<scene::EditableMesh>(target.Mesh)->Revision;
	jobs.ClearCompletions();
	fixture.Rebind(target);
	tickets[0] = Ready(jobs, fixture.Store, target.Scope);
	REQUIRE(jobs.Cancel(tickets[0]));
	CHECK_FALSE(
		jobs.PublishSceneBatch(fixture.Store, tickets, std::span(&target, 1), fixture.Store.Time().Tick)
	);
	jobs.ClearCompletions();
	tickets[0] = Ready(jobs, fixture.Store, target.Scope);
	REQUIRE(
		script::SetProceduralSource(
			fixture.Store,
			{procedural::SourceKind::Village, "oakstead", {9 * CELL, 8 * CELL}, ""},
			fixture.Diagnostic
		)
	);
	CHECK_FALSE(
		jobs.PublishSceneBatch(fixture.Store, tickets, std::span(&target, 1), fixture.Store.Time().Tick)
	);
	CHECK(fixture.Store.Resource<script::ProceduralPublished>()->Regions.front().ArtifactDigest == old);
	CHECK(fixture.Store.Get<scene::EditableMesh>(target.Mesh)->Revision == revision);
	CHECK(fixture.Store.Get<script::ProceduralSceneRegion>(target.Mesh)->ArtifactDigest == old);
}
TEST_CASE(
	"published scene materialization needs no recipe authority and preserves ordinary mesh geometry",
	"[procedural][scene]"
) {
	SceneFixture fixture;
	auto target = fixture.Target(SCOPES[0]);
	script::ProceduralJobs jobs(fixture.Store);
	const std::array<uint64_t, 1> tickets{Ready(jobs, fixture.Store, target.Scope)};
	REQUIRE(jobs.PublishBatch(fixture.Store, tickets, fixture.Store.Time().Tick));
	fixture.Store.RemoveResource<script::ProceduralAuthority>();
	REQUIRE(
		script::MaterializePublishedProceduralRegions(
			fixture.Store, std::span(&target, 1), fixture.Diagnostic
		)
	);
	const auto positions = fixture.Store.Get<scene::EditableMesh>(target.Mesh)->Positions;
	const auto revision = fixture.Store.Get<scene::EditableMesh>(target.Mesh)->Revision;
	fixture.Rebind(target);
	REQUIRE(
		script::MaterializePublishedProceduralRegions(
			fixture.Store, std::span(&target, 1), fixture.Diagnostic
		)
	);
	CHECK(fixture.Store.Get<scene::EditableMesh>(target.Mesh)->Positions == positions);
	CHECK(fixture.Store.Get<scene::EditableMesh>(target.Mesh)->Revision == revision);
	auto *published = fixture.Store.ResourceMutable<script::ProceduralPublished>();
	published->Regions.front().ArtifactBytes[0] ^= 1;
	fixture.Rebind(target);
	CHECK_FALSE(
		script::MaterializePublishedProceduralRegions(
			fixture.Store, std::span(&target, 1), fixture.Diagnostic
		)
	);
	CHECK(fixture.Store.Get<scene::EditableMesh>(target.Mesh)->Positions == positions);
}

TEST_CASE(
	"an empty region can atomically become visible on the next recipe publication", "[procedural][scene]"
) {
	SceneFixture fixture;
	auto target = fixture.Target(SCOPES[1]);
	auto emptyRecipe = procedural::ForestVillageDefaults("forest-village", 42);
	for (auto &node : emptyRecipe.Nodes)
		for (auto &parameter : node.Parameters)
			if (parameter.Name == "density_ppm") parameter.Value = uint64_t{0};
	REQUIRE(script::SetProceduralRecipe(fixture.Store, emptyRecipe, fixture.Diagnostic));
	script::ProceduralJobs jobs(fixture.Store);
	std::array<uint64_t, 1> tickets{Ready(jobs, fixture.Store, target.Scope)};
	REQUIRE(jobs.PublishSceneBatch(fixture.Store, tickets, std::span(&target, 1), fixture.Store.Time().Tick));
	CHECK(fixture.Store.Get<scene::EditableMesh>(target.Mesh)->Positions.empty());
	CHECK(fixture.Store.Get<script::ProceduralSceneRegion>(target.Mesh)->Objects.empty());
	jobs.ClearCompletions();
	fixture.Rebind(target);
	REQUIRE(
		script::SetProceduralRecipe(
			fixture.Store, procedural::ForestVillageDefaults("forest-village", 42), fixture.Diagnostic
		)
	);
	tickets[0] = Ready(jobs, fixture.Store, target.Scope);
	REQUIRE(jobs.PublishSceneBatch(fixture.Store, tickets, std::span(&target, 1), fixture.Store.Time().Tick));
	CHECK_FALSE(fixture.Store.Get<scene::EditableMesh>(target.Mesh)->Positions.empty());
	CHECK_FALSE(fixture.Store.Get<script::ProceduralSceneRegion>(target.Mesh)->Objects.empty());
}

TEST_CASE("derived scene identity ranges clear during serialization", "[procedural][scene]") {
	script::RegisterProceduralComponents();
	script::ProceduralSceneRegion original;
	original.Scope = {1, 2, 16};
	original.Objects.push_back({"tree/1/2", "tree", 0, 72, 0, 1});
	const auto &descriptor = ecs::Components::Describe(ecs::Components::Of<script::ProceduralSceneRegion>());
	core::ByteWriter writer;
	descriptor.Write(writer, &original, 1);
	CHECK(writer.Bytes().empty());
	core::ByteReader reader(writer.Bytes());
	descriptor.Read(reader, &original, 1);
	CHECK(original.Objects.empty());
	CHECK(original.Scope.X == 0);
	CHECK(original.Scope.Y == 0);
	CHECK(original.Scope.CellsPerSide == procedural::RegionScope{}.CellsPerSide);
}

TEST_CASE(
	"selective scene regeneration leaves unrelated region geometry and receipts unchanged",
	"[procedural][scene]"
) {
	SceneFixture fixture;
	std::array<script::ProceduralSceneTarget, 2> targets{
		fixture.Target(SCOPES[0]), fixture.Target(SCOPES[1])
	};
	script::ProceduralJobs jobs(fixture.Store);
	const std::array<uint64_t, 2> initial{
		Ready(jobs, fixture.Store, SCOPES[0]), Ready(jobs, fixture.Store, SCOPES[1])
	};
	REQUIRE(jobs.PublishSceneBatch(fixture.Store, initial, targets, fixture.Store.Time().Tick));
	const auto distantDigest =
		fixture.Store.Get<script::ProceduralSceneRegion>(targets[1].Mesh)->ArtifactDigest;
	const auto distantRevision = fixture.Store.Get<scene::EditableMesh>(targets[1].Mesh)->Revision;
	const auto distantPositions = fixture.Store.Get<scene::EditableMesh>(targets[1].Mesh)->Positions;
	const auto distantFrame = fixture.Store.Get<scene::Transform>(targets[1].Part)->Frame.Position;
	jobs.ClearCompletions();
	fixture.Rebind(targets[0]);
	REQUIRE(
		script::SetProceduralSource(
			fixture.Store,
			{procedural::SourceKind::Village, "oakstead", {9 * CELL, 8 * CELL}, ""},
			fixture.Diagnostic
		)
	);
	const std::array<uint64_t, 1> replacement{Ready(jobs, fixture.Store, SCOPES[0])};
	REQUIRE(jobs.PublishSceneBatch(
		fixture.Store, replacement, std::span(targets.data(), 1), fixture.Store.Time().Tick
	));
	CHECK(fixture.Store.Get<script::ProceduralSceneRegion>(targets[1].Mesh)->ArtifactDigest == distantDigest);
	CHECK(fixture.Store.Get<scene::EditableMesh>(targets[1].Mesh)->Revision == distantRevision);
	CHECK(fixture.Store.Get<scene::EditableMesh>(targets[1].Mesh)->Positions == distantPositions);
	CHECK(fixture.Store.Get<scene::Transform>(targets[1].Part)->Frame.Position == distantFrame);
	const auto &canonical = fixture.Store.Resource<script::ProceduralPublished>()->Regions;
	REQUIRE(canonical.size() == 2);
	CHECK(canonical[1].ArtifactDigest == distantDigest);
}
