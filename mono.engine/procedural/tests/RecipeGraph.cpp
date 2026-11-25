#include <engine/procedural/Region.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string_view>
#include <vector>

TEST_SUITE_ID("engine.procedural.recipe_graph")
TEST_DEPENDS("engine.procedural.region")

using namespace engine::procedural;

namespace {
	constexpr int64_t CELL = SUBCELLS_PER_CELL;

	Digest Filled(uint8_t value) {
		Digest digest;
		digest.Bytes.fill(value);
		return digest;
	}

	// FNV-1a over every published field, so one number stands for the whole result.
	struct Fnv {
		uint64_t Value = 0xcbf29ce484222325ull;
		void Bytes(const void *data, size_t size) {
			const auto *bytes = static_cast<const uint8_t *>(data);
			for (size_t index = 0; index < size; ++index) {
				Value = (Value ^ bytes[index]) * 0x100000001b3ull;
			}
		}
		void Text(std::string_view text) {
			Number(text.size());
			Bytes(text.data(), text.size());
		}
		void Number(uint64_t value) {
			Bytes(&value, sizeof(value));
		}
		void At(Point point) {
			Number(static_cast<uint64_t>(point.X));
			Number(static_cast<uint64_t>(point.Y));
		}
	};

	uint64_t HashResult(const RegionResult &result) {
		Fnv hash;
		hash.Bytes(result.KeyBytes.data(), result.KeyBytes.size());
		for (const Placement &placed : result.Placements) {
			hash.Text(placed.Id);
			hash.Number(static_cast<uint64_t>(placed.Kind));
			hash.Text(placed.SourceName);
			hash.At(placed.Position);
			hash.Number(placed.RotationQuarterTurns);
			hash.Number(placed.Variant);
			hash.Number(placed.Material);
		}
		for (const RoadSpline &road : result.Roads) {
			hash.Text(road.Id);
			hash.Text(road.EntranceName);
			hash.Text(road.VillageName);
			for (const Point &control : road.Controls) {
				hash.At(control);
			}
		}
		hash.Number(result.Facts.CandidateCells);
		hash.Number(result.Facts.WorkUnits);
		hash.Number(result.Facts.SpatialTests);
		hash.Number(result.Facts.OutputBytes);
		return hash.Value;
	}
}

// The saved REC1 bytes of the default recipe, loaded and run through the ang
// recipe graph, give the result recorded from the stage loop it replaced.
TEST_CASE("saved forest village recipe generates the recorded region", "[procedural][region]") {
	std::vector<uint8_t> saved;
	Diagnostic diagnostic;
	REQUIRE(EncodeRecipe(ForestVillageDefaults("golden", 7), saved, diagnostic) == Status::Ok);
	Recipe loaded;
	REQUIRE(DecodeRecipe(saved, loaded, diagnostic) == Status::Ok);
	CompiledRecipe recipe;
	REQUIRE(CompileForestVillage(loaded, recipe, diagnostic) == Status::Ok);

	const RegionScope scope{0, 0, 32};
	Bounds owner;
	Bounds query;
	REQUIRE(RequiredRegionBounds(recipe, scope, owner, query, diagnostic) == Status::Ok);
	RegionRequest request;
	request.Scope = scope;
	request.RecipeDigest = Filled(1);
	request.Authored.Covered = query;
	request.Authored.Complete = true;
	request.Queries = {
		{"villages",
		 query,
		 Filled(2),
		 true,
		 {{SourceKind::Village, "oakstead", {8 * CELL, 8 * CELL}, "", Filled(3)},
		  {SourceKind::Village, "rivermouth", {24 * CELL, 22 * CELL}, "", Filled(4)}}},
		{"entrances",
		 query,
		 Filled(5),
		 true,
		 {{SourceKind::Entrance, "west-gate", {12 * CELL, 8 * CELL}, "oakstead", Filled(6)},
		  {SourceKind::Entrance, "ford", {20 * CELL, 26 * CELL}, "rivermouth", Filled(7)}}}
	};

	RegionResult result;
	REQUIRE(GenerateRegion(recipe, request, result, diagnostic) == Status::Ok);
	CHECK(result.Roads.size() == 2);
	CHECK(result.Placements.size() == 109);
	CHECK(HashResult(result) == 0x483ec7f11e20743full);
}
