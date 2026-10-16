#include <engine/ecs/Store.hpp>
#include <engine/scene/EditableMesh.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/script/ProceduralService.hpp>
#include <engine/script/ServiceSurface.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>

TEST_SUITE_ID("engine.script.proceduralservice")
TEST_DEPENDS("engine.script.proceduralscene")
using namespace engine;
namespace {
	constexpr int64_t CELL = procedural::SUBCELLS_PER_CELL;
	constexpr std::array<procedural::RegionScope, 3> SERVICE_SCOPES{{{0, 0, 16}, {20, 0, 16}, {40, 0, 16}}};
	struct ServiceFixture {
		ecs::Store Store{"procedural-service"};
		procedural::Diagnostic Diagnostic{};
		ServiceFixture() {
			scene::RegisterSceneComponents();
			script::RegisterProceduralComponents();
		}
		std::shared_ptr<script::ProceduralEndpoint> Open(
			uint64_t runtime,
			script::ProceduralWorldMode mode = script::ProceduralWorldMode::AuthoritativeSerial
		) {
			auto endpoint = script::OpenProceduralEndpoint(Store, runtime, mode, Diagnostic);
			REQUIRE(endpoint);
			return endpoint;
		}
		void Configure(const std::shared_ptr<script::ProceduralEndpoint> &endpoint) {
			const std::array<script::ProceduralSource, 2> sources{
				{{procedural::SourceKind::Village, "oakstead", {8 * CELL, 8 * CELL}, ""},
				 {procedural::SourceKind::Entrance, "gate", {12 * CELL, 8 * CELL}, "oakstead"}}
			};
			REQUIRE(endpoint->Configure(
				Store, procedural::ForestVillageDefaults("demo", 42), SERVICE_SCOPES, sources, Diagnostic
			));
		}
		std::pair<ecs::Entity, ecs::Entity> Target() {
			const auto mesh = Store.CreateInstance(scene::EditableMeshClass(), "ProceduralMesh");
			scene::PartDesc description;
			description.Class = ecs::Classes::Find(core::Name("MeshPart"));
			description.Mesh = scene::EditableMeshContentName(Store, mesh);
			const auto part = scene::MakePart(Store, description);
			REQUIRE(part != ecs::NULL_ENTITY);
			auto *collider = Store.GetMutable<scene::Collider>(part);
			collider->Trigger = true;
			collider->CanQuery = false;
			return {mesh, part};
		}
		uint64_t Generate(const std::shared_ptr<script::ProceduralEndpoint> &endpoint, size_t scope) {
			const auto [mesh, part] = Target();
			uint64_t ticket = 0;
			REQUIRE(endpoint->Generate(Store, SERVICE_SCOPES[scope], mesh, part, ticket, Diagnostic));
			REQUIRE(ticket != 0);
			return ticket;
		}
	};
} // namespace
TEST_CASE(
	"all runtime endpoints share one world admission budget and "
	"individual release",
	"[procedural][service]"
) {
	ServiceFixture fixture;
	auto first = fixture.Open(1);
	auto second = fixture.Open(2);
	fixture.Configure(first);
	const auto a = fixture.Generate(first, 0);
	const auto b = fixture.Generate(second, 1);
	const auto [mesh, part] = fixture.Target();
	uint64_t refused = 77;
	CHECK_FALSE(first->Generate(fixture.Store, SERVICE_SCOPES[2], mesh, part, refused, fixture.Diagnostic));
	CHECK(refused == 77);
	CHECK(fixture.Diagnostic.Code == procedural::Status::LimitExceeded);
	REQUIRE(first->Release(fixture.Store, a, fixture.Diagnostic));
	script::ProceduralServiceProgress progress;
	REQUIRE(second->Progress(fixture.Store, b, progress, fixture.Diagnostic));
	CHECK(progress.Work.Status == script::ProceduralJobStatus::Ready);
	uint64_t accepted = 0;
	REQUIRE(first->Generate(fixture.Store, SERVICE_SCOPES[2], mesh, part, accepted, fixture.Diagnostic));
	CHECK(accepted != 0);
	CHECK(accepted != a);
}
TEST_CASE("runtime ticket ownership fences progress cancel publish and release", "[procedural][service]") {
	ServiceFixture fixture;
	auto first = fixture.Open(11);
	auto second = fixture.Open(12);
	fixture.Configure(first);
	const auto ticket = fixture.Generate(first, 0);
	CHECK(first->BelongsTo(fixture.Store, 11));
	CHECK_FALSE(first->BelongsTo(fixture.Store, 12));
	script::ProceduralServiceProgress progress;
	const std::array<uint64_t, 1> tickets{ticket};
	CHECK_FALSE(second->Progress(fixture.Store, ticket, progress, fixture.Diagnostic));
	CHECK_FALSE(second->Cancel(fixture.Store, ticket, fixture.Diagnostic));
	CHECK_FALSE(second->Publish(fixture.Store, tickets, fixture.Diagnostic));
	CHECK_FALSE(second->Release(fixture.Store, ticket, fixture.Diagnostic));
	REQUIRE(first->Progress(fixture.Store, ticket, progress, fixture.Diagnostic));
	CHECK(progress.Work.Status == script::ProceduralJobStatus::Ready);
	REQUIRE(first->Publish(fixture.Store, tickets, fixture.Diagnostic));
	CHECK(
		fixture.Store.Resource<script::ProceduralPublished>()->Regions.front().ActivationTick ==
		fixture.Store.Time().Tick
	);
	REQUIRE(first->Progress(fixture.Store, ticket, progress, fixture.Diagnostic));
	CHECK(progress.Work.Status == script::ProceduralJobStatus::Published);
}
TEST_CASE("detachment and world replacement revoke every old endpoint", "[procedural][service]") {
	ServiceFixture fixture;
	auto first = fixture.Open(21);
	fixture.Configure(first);
	fixture.Generate(first, 0);
	first->Detach();
	script::ProceduralServiceProgress progress;
	CHECK_FALSE(first->Progress(fixture.Store, 1, progress, fixture.Diagnostic));
	auto replacement = fixture.Open(21);
	const auto ticket = fixture.Generate(replacement, 1);
	REQUIRE(replacement->Progress(fixture.Store, ticket, progress, fixture.Diagnostic));
	fixture.Store.Clear();
	CHECK_FALSE(replacement->Progress(fixture.Store, ticket, progress, fixture.Diagnostic));
	auto newWorld = fixture.Open(21);
	fixture.Configure(newWorld);
	fixture.Generate(newWorld, 0);
}
TEST_CASE("a world fixes its execution mode and bounds attached runtimes", "[procedural][service]") {
	ServiceFixture fixture;
	std::vector<std::shared_ptr<script::ProceduralEndpoint>> endpoints;
	for (uint64_t runtime = 1; runtime <= 64; ++runtime)
		endpoints.push_back(fixture.Open(runtime));
	CHECK_FALSE(
		script::OpenProceduralEndpoint(
			fixture.Store, 65, script::ProceduralWorldMode::AuthoritativeSerial, fixture.Diagnostic
		)
	);
	CHECK_FALSE(
		script::OpenProceduralEndpoint(
			fixture.Store, 1, script::ProceduralWorldMode::AuthoritativeSerial, fixture.Diagnostic
		)
	);
	CHECK_FALSE(
		script::OpenProceduralEndpoint(
			fixture.Store, 66, script::ProceduralWorldMode::StudioPreview, fixture.Diagnostic
		)
	);
	endpoints.front()->Detach();
	CHECK(
		script::OpenProceduralEndpoint(
			fixture.Store, 65, script::ProceduralWorldMode::AuthoritativeSerial, fixture.Diagnostic
		)
	);
}
TEST_CASE(
	"service source spline and override edits preserve accepted output "
	"until publication",
	"[procedural][service]"
) {
	ServiceFixture fixture;
	auto endpoint = fixture.Open(31);
	fixture.Configure(endpoint);
	const std::array<uint64_t, 1> initial{fixture.Generate(endpoint, 0)};
	REQUIRE(endpoint->Publish(fixture.Store, initial, fixture.Diagnostic));
	const auto digest = fixture.Store.Resource<script::ProceduralPublished>()->Regions.front().ArtifactDigest;
	REQUIRE(endpoint->Release(fixture.Store, initial.front(), fixture.Diagnostic));
	const std::array<uint64_t, 1> stale{fixture.Generate(endpoint, 0)};
	REQUIRE(endpoint->SetSpline(
		fixture.Store, {"road-control", "gate", {10 * CELL, 9 * CELL}}, fixture.Diagnostic
	));
	procedural::PlacementOverride deleted;
	deleted.TargetId = "tree/1/1";
	REQUIRE(endpoint->SetOverride(fixture.Store, deleted, fixture.Diagnostic));
	CHECK_FALSE(endpoint->Publish(fixture.Store, stale, fixture.Diagnostic));
	CHECK(fixture.Store.Resource<script::ProceduralPublished>()->Regions.front().ArtifactDigest == digest);
	REQUIRE(endpoint->RemoveSpline(fixture.Store, "road-control", fixture.Diagnostic));
	REQUIRE(endpoint->RemoveOverride(fixture.Store, "tree/1/1", fixture.Diagnostic));
	REQUIRE(endpoint->Cancel(fixture.Store, stale.front(), fixture.Diagnostic));
	REQUIRE(endpoint->Release(fixture.Store, stale.front(), fixture.Diagnostic));
	const std::array<uint64_t, 1> current{fixture.Generate(endpoint, 0)};
	REQUIRE(endpoint->Publish(fixture.Store, current, fixture.Diagnostic));
	CHECK(fixture.Store.Resource<script::ProceduralPublished>()->Regions.front().ArtifactDigest == digest);
}
TEST_CASE(
	"one neutral procedural surface exposes all authoring and lifecycle "
	"methods",
	"[procedural][service]"
) {
	const auto &surface = script::ProceduralServiceSurface();
	CHECK(std::string_view(surface.Name) == "ProceduralService");
	CHECK(surface.LuauMethods.empty());
	REQUIRE(surface.Methods.size() == 13);
	for (const auto *name :
		 {"ConfigureForestVillage",
		  "SetSource",
		  "RemoveSource",
		  "SetSpline",
		  "RemoveSpline",
		  "SetOverride",
		  "RemoveOverride",
		  "Generate",
		  "Progress",
		  "Cancel",
		  "Publish",
		  "Release",
		  "UseBaked"})
		CHECK(std::any_of(surface.Methods.begin(), surface.Methods.end(), [&](const auto &row) {
			return std::string_view(row.Name) == name;
		}));
}

TEST_CASE("refused generation preserves an unbound script-valid target", "[procedural][service][admission]") {
	ServiceFixture fixture;
	auto endpoint = fixture.Open(71);
	const auto [mesh, part] = fixture.Target();
	const auto beforeVisual = *fixture.Store.Get<scene::Visual>(part);
	const auto beforeCollider = *fixture.Store.Get<scene::Collider>(part);
	const auto beforeFrame = *fixture.Store.Get<scene::Transform>(part);
	const auto beforeBounds = *fixture.Store.Get<scene::Bounds>(part);
	REQUIRE_FALSE(fixture.Store.Has<script::ProceduralSceneRegion>(mesh));
	uint64_t ticket = 123;
	CHECK_FALSE(endpoint->Generate(fixture.Store, SERVICE_SCOPES[0], mesh, part, ticket, fixture.Diagnostic));
	CHECK(ticket == 123);
	CHECK_FALSE(fixture.Store.Has<script::ProceduralSceneRegion>(mesh));
	CHECK(fixture.Store.Get<scene::Visual>(part)->Fitted == beforeVisual.Fitted);
	CHECK(fixture.Store.Get<scene::Collider>(part)->Mask == beforeCollider.Mask);
	CHECK(fixture.Store.Get<scene::Collider>(part)->Trigger == beforeCollider.Trigger);
	CHECK(fixture.Store.Get<scene::Collider>(part)->CanQuery == beforeCollider.CanQuery);
	CHECK(fixture.Store.Get<scene::Transform>(part)->Frame.Position == beforeFrame.Frame.Position);
	CHECK(fixture.Store.Get<scene::Bounds>(part)->HalfExtent == beforeBounds.HalfExtent);
	fixture.Configure(endpoint);
	CHECK_FALSE(endpoint->Generate(fixture.Store, {999, 999, 16}, mesh, part, ticket, fixture.Diagnostic));
	CHECK_FALSE(fixture.Store.Has<script::ProceduralSceneRegion>(mesh));
	CHECK(fixture.Store.Get<scene::Visual>(part)->Fitted == beforeVisual.Fitted);
	CHECK(fixture.Store.Get<scene::Collider>(part)->Mask == beforeCollider.Mask);
}
