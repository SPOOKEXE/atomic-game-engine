#include <engine/core/Bytes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/procedural/Region.hpp>
#include <engine/script/ProceduralInputs.hpp>
#include <engine/script/ProceduralJobs.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <thread>

TEST_SUITE_ID("engine.script.proceduralinputs")
TEST_DEPENDS("engine.procedural.region")

using namespace engine;

namespace {
	constexpr int64_t CELL = procedural::SUBCELLS_PER_CELL;
	constexpr std::array<procedural::RegionScope, 3> SCOPES{{{0, 0, 16}, {20, 0, 16}, {40, 0, 16}}};

	struct Fixture {
		ecs::Store Store{"authored-inputs"};
		procedural::Diagnostic Diagnostic{};
		Fixture() {
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
	};

	std::vector<uint8_t> Key(const ecs::Store &store, procedural::RegionScope scope) {
		procedural::CompiledRecipe recipe;
		procedural::RegionRequest request;
		procedural::Diagnostic diagnostic;
		REQUIRE(script::CaptureProceduralRegion(store, scope, recipe, request, diagnostic));
		std::vector<uint8_t> key;
		REQUIRE(procedural::BuildRegionKey(recipe, request, key, diagnostic) == procedural::Status::Ok);
		return key;
	}

	procedural::AuthoredSpline Road(procedural::Point control = {10 * CELL, 8 * CELL}) {
		return {"west-road", "west-gate", control};
	}

	procedural::PlacementOverride MovedTree() {
		return {"tree/1/1", procedural::OverrideAction::Replace, {328 * CELL, 2 * CELL}, 1, 0, 0, ""};
	}
}

TEST_CASE("authored capture includes complete empty witnesses and only local facts", "[procedural][host]") {
	Fixture fixture;
	procedural::CompiledRecipe recipe;
	procedural::RegionRequest empty;
	REQUIRE(script::CaptureProceduralRegion(fixture.Store, SCOPES[2], recipe, empty, fixture.Diagnostic));
	CHECK(empty.Authored.Complete);
	CHECK(empty.Authored.Splines.empty());
	CHECK(empty.Authored.Overrides.empty());
	CHECK(empty.Authored.CoverageDigest != procedural::Digest{});

	const auto nearKey = Key(fixture.Store, SCOPES[0]);
	const auto middleKey = Key(fixture.Store, SCOPES[1]);
	const auto farKey = Key(fixture.Store, SCOPES[2]);
	REQUIRE(script::SetProceduralSpline(fixture.Store, Road(), fixture.Diagnostic));
	CHECK(Key(fixture.Store, SCOPES[0]) != nearKey);
	CHECK(Key(fixture.Store, SCOPES[1]) == middleKey);
	CHECK(Key(fixture.Store, SCOPES[2]) == farKey);

	REQUIRE(script::SetProceduralOverride(fixture.Store, MovedTree(), fixture.Diagnostic));
	procedural::RegionRequest near, middle;
	REQUIRE(script::CaptureProceduralRegion(fixture.Store, SCOPES[0], recipe, near, fixture.Diagnostic));
	REQUIRE(script::CaptureProceduralRegion(fixture.Store, SCOPES[1], recipe, middle, fixture.Diagnostic));
	REQUIRE(near.Authored.Splines.size() == 1);
	REQUIRE(near.Authored.Overrides.size() == 1);
	REQUIRE(middle.Authored.Overrides.size() == 1);
	CHECK(middle.Authored.Splines.empty());
	CHECK(Key(fixture.Store, SCOPES[1]) != middleKey);
	CHECK(Key(fixture.Store, SCOPES[2]) == farKey);
	const auto *authority = fixture.Store.Resource<script::ProceduralAuthority>();
	REQUIRE(authority);
	CHECK(authority->Regions[0].SourceRevision == 3);
	CHECK(authority->Regions[1].SourceRevision == 2);
	CHECK(authority->Regions[2].SourceRevision == 1);
}

TEST_CASE(
	"invalid road and source edits leave authored state and regional identity intact", "[procedural][host]"
) {
	Fixture fixture;
	REQUIRE(script::SetProceduralSpline(fixture.Store, Road({11 * CELL, 8 * CELL}), fixture.Diagnostic));
	const auto key = Key(fixture.Store, SCOPES[0]);
	CHECK_FALSE(script::SetProceduralSpline(fixture.Store, Road({20 * CELL, 8 * CELL}), fixture.Diagnostic));
	CHECK(Key(fixture.Store, SCOPES[0]) == key);
	CHECK_FALSE(
		script::SetProceduralSource(
			fixture.Store,
			{procedural::SourceKind::Entrance, "west-gate", {35 * CELL, 8 * CELL}, "oakstead"},
			fixture.Diagnostic
		)
	);
	CHECK(Key(fixture.Store, SCOPES[0]) == key);
	CHECK_FALSE(
		script::RemoveProceduralSource(
			fixture.Store, procedural::SourceKind::Village, "oakstead", fixture.Diagnostic
		)
	);
	CHECK(Key(fixture.Store, SCOPES[0]) == key);
	CHECK_FALSE(
		script::SetProceduralOverride(
			fixture.Store,
			{"tree/1/1", procedural::OverrideAction::Replace, {2 * CELL, 2 * CELL}, 0, 63, 0, ""},
			fixture.Diagnostic
		)
	);
	CHECK(Key(fixture.Store, SCOPES[0]) == key);
}

TEST_CASE("authored facts survive save and import while pending output becomes stale", "[procedural][host]") {
	Fixture fixture;
	REQUIRE(script::SetProceduralSpline(fixture.Store, Road(), fixture.Diagnostic));
	REQUIRE(script::SetProceduralOverride(fixture.Store, MovedTree(), fixture.Diagnostic));
	const auto key = Key(fixture.Store, SCOPES[0]);
	std::vector<uint8_t> portable;
	REQUIRE(script::EncodeProceduralAuthority(fixture.Store, portable, fixture.Diagnostic));
	ecs::Store imported{"authored-import"};
	REQUIRE(script::DecodeProceduralAuthority(imported, portable, fixture.Diagnostic));
	CHECK(Key(imported, SCOPES[0]) == key);
	std::vector<uint8_t> reencoded;
	REQUIRE(script::EncodeProceduralAuthority(imported, reencoded, fixture.Diagnostic));
	CHECK(reencoded == portable);
	core::ByteWriter writer;
	REQUIRE(fixture.Store.Save(writer));
	core::ByteReader reader(writer.Bytes());
	REQUIRE(fixture.Store.Load(reader));
	CHECK(Key(fixture.Store, SCOPES[0]) == key);

	script::ProceduralJobs jobs(fixture.Store);
	const uint64_t ticket = jobs.Submit(fixture.Store, SCOPES[0], parallel::JobContext::Serial);
	REQUIRE(ticket != 0);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (jobs.Completions().empty() && std::chrono::steady_clock::now() < deadline) {
		jobs.Poll();
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	REQUIRE(jobs.Completions().size() == 1);
	REQUIRE(jobs.Completions()[0].Status == script::ProceduralJobStatus::Ready);
	REQUIRE(script::RemoveProceduralSpline(fixture.Store, "west-road", fixture.Diagnostic));
	CHECK_FALSE(jobs.PublishBatch(fixture.Store, std::array{ticket}, fixture.Store.Time().Tick));
	CHECK(jobs.LastDiagnostic().Code == procedural::Status::StaleArtifact);
	CHECK(Key(fixture.Store, SCOPES[0]) != key);
}
