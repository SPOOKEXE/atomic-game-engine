#include <engine/procedural/Region.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.procedural.region")
TEST_DEPENDS("engine.procedural.canonical")
TEST_DEPENDS("engine.procedural.random")

using namespace engine::procedural;

namespace {
	constexpr int64_t CELL = SUBCELLS_PER_CELL;

	Digest TestDigest(uint8_t value) {
		Digest digest;
		digest.Bytes.fill(value);
		return digest;
	}

	CompiledRecipe Compile(Recipe recipe) {
		CompiledRecipe compiled;
		Diagnostic diagnostic;
		REQUIRE(CompileForestVillage(recipe, compiled, diagnostic) == Status::Ok);
		return compiled;
	}

	RegionRequest Request(const CompiledRecipe &recipe, RegionScope scope = {}) {
		Bounds owner;
		Bounds query;
		Diagnostic diagnostic;
		REQUIRE(RequiredRegionBounds(recipe, scope, owner, query, diagnostic) == Status::Ok);
		RegionRequest request;
		request.Scope = scope;
		request.RecipeDigest = TestDigest(3);
		request.Authored.Covered = query;
		request.Authored.Complete = true;
		request.Queries = {
			{"villages",
			 query,
			 TestDigest(4),
			 true,
			 {{SourceKind::Village, "oakstead", {8 * CELL, 8 * CELL}, "", TestDigest(5)}}},
			{"entrances",
			 query,
			 TestDigest(6),
			 true,
			 {{SourceKind::Entrance, "west-gate", {12 * CELL, 8 * CELL}, "oakstead", TestDigest(7)}}}
		};
		return request;
	}

	bool SameOutput(const RegionResult &left, const RegionResult &right) {
		if (left.KeyBytes != right.KeyBytes || left.Roads.size() != right.Roads.size() ||
			left.Placements.size() != right.Placements.size())
			return false;
		for (size_t index = 0; index < left.Placements.size(); ++index) {
			const auto &a = left.Placements[index];
			const auto &b = right.Placements[index];
			if (a.Id != b.Id || a.Kind != b.Kind || a.SourceName != b.SourceName ||
				a.Position != b.Position || a.RotationQuarterTurns != b.RotationQuarterTurns ||
				a.Variant != b.Variant || a.Material != b.Material)
				return false;
		}
		for (size_t index = 0; index < left.Roads.size(); ++index) {
			const auto &a = left.Roads[index];
			const auto &b = right.Roads[index];
			if (a.Id != b.Id || a.EntranceName != b.EntranceName || a.VillageName != b.VillageName ||
				a.Controls != b.Controls)
				return false;
		}
		return true;
	}

	Recipe WideSpacingShortRoad() {
		Recipe recipe = ForestVillageDefaults("short-road", 91);
		for (OperationNode &node : recipe.Nodes) {
			for (Parameter &parameter : node.Parameters) {
				if (parameter.Name == "min_distance_cells") parameter.Value = uint64_t{8};
				if (parameter.Name == "radius_cells") parameter.Value = uint64_t{16};
				if (parameter.Name == "max_reach_cells") parameter.Value = uint64_t{1};
				if (parameter.Name == "bend_cells") parameter.Value = uint64_t{0};
				if (parameter.Name == "clearance_cells" && node.Kind == "road.spline")
					parameter.Value = uint64_t{1};
			}
		}
		return recipe;
	}
}

TEST_CASE(
	"authored graph compiles and malformed typed input preserves compiled recipe", "[procedural][region]"
) {
	Recipe recipe = ForestVillageDefaults("town", 42);
	const auto schemas = ForestVillageNodeSchemas();
	REQUIRE(schemas.size() == recipe.Nodes.size());
	CHECK(schemas.data() == ForestVillageNodeSchemas().data());
	for (const OperationNode &node : recipe.Nodes) {
		const auto found = std::find_if(schemas.begin(), schemas.end(), [&](const auto &schema) {
			return schema.Id == node.Kind;
		});
		REQUIRE(found != schemas.end());
		CHECK(!found->Ports.empty());
	}
	CompiledRecipe compiled = Compile(recipe);
	const auto saved = compiled.CanonicalBytes();
	recipe.Links.back().ToPort = "unknown";
	Diagnostic diagnostic;
	CHECK(CompileForestVillage(recipe, compiled, diagnostic) == Status::InvalidRecipe);
	CHECK(compiled.CanonicalBytes() == saved);
	recipe = ForestVillageDefaults("town", 42);
	for (auto &node : recipe.Nodes)
		if (node.Kind == "forest.scatter")
			for (auto &parameter : node.Parameters)
				if (parameter.Name == "density_ppm") parameter.Value = uint64_t{0};
	CHECK(CompileForestVillage(recipe, compiled, diagnostic) == Status::Ok);
	CHECK(compiled.CanonicalBytes() != saved);
}

TEST_CASE("region identity is local, complete and independent of lifecycle tickets", "[procedural][region]") {
	const CompiledRecipe recipe = Compile(ForestVillageDefaults("town", 42));
	RegionRequest request = Request(recipe);
	std::vector<uint8_t> first;
	std::vector<uint8_t> second;
	Diagnostic diagnostic;
	REQUIRE(BuildRegionKey(recipe, request, first, diagnostic) == Status::Ok);
	request.RequestSequence = 99;
	request.ExpectedSourceRevision = 120;
	std::reverse(request.Queries.begin(), request.Queries.end());
	REQUIRE(BuildRegionKey(recipe, request, second, diagnostic) == Status::Ok);
	CHECK(first == second);
	request.Queries.front().CoverageDigest = TestDigest(8);
	REQUIRE(BuildRegionKey(recipe, request, second, diagnostic) == Status::Ok);
	CHECK(first != second);
	request.Queries.front().CoverageDigest = TestDigest(6);
	request.Queries.front().Matches.clear();
	REQUIRE(BuildRegionKey(recipe, request, second, diagnostic) == Status::Ok);
	CHECK(first != second);
	request.Queries.front().Complete = false;
	second = {1, 2, 3};
	CHECK(BuildRegionKey(recipe, request, second, diagnostic) == Status::InvalidCoverage);
	CHECK((second == std::vector<uint8_t>{1, 2, 3}));
}

TEST_CASE(
	"seeded village and forest generation owns connected outputs deterministically", "[procedural][region]"
) {
	const CompiledRecipe recipe = Compile(ForestVillageDefaults("town", 42));
	RegionRequest request = Request(recipe, {0, 0, 32});
	RegionResult first;
	RegionResult second;
	Diagnostic diagnostic;
	REQUIRE(GenerateRegion(recipe, request, first, diagnostic) == Status::Ok);
	CHECK(first.Facts.CandidateCells > 0);
	CHECK(first.Roads.size() == 1);
	REQUIRE(first.Roads.size() == 1);
	CHECK((first.Roads.front().Controls.front() == Point{12 * CELL, 8 * CELL}));
	CHECK((first.Roads.front().Controls.back() == Point{8 * CELL, 8 * CELL}));
	bool house = false;
	bool lot = false;
	bool tree = false;
	bool entrance = false;
	for (const Placement &placement : first.Placements) {
		house |= placement.Kind == PlacementKind::House;
		lot |= placement.Kind == PlacementKind::VillageLot;
		tree |= placement.Kind == PlacementKind::Tree;
		entrance |= placement.Kind == PlacementKind::Entrance;
	}
	CHECK(house);
	CHECK(lot);
	CHECK(tree);
	CHECK(entrance);
	request.RequestSequence = 15;
	request.ExpectedSourceRevision = 19;
	std::reverse(request.Queries.begin(), request.Queries.end());
	REQUIRE(GenerateRegion(recipe, request, second, diagnostic) == Status::Ok);
	CHECK(SameOutput(first, second));
	CHECK(second.Facts.WorkUnits == first.Facts.WorkUnits);
	CHECK(second.Facts.CandidateCells == first.Facts.CandidateCells);
	CHECK(second.Facts.SpatialTests == first.Facts.SpatialTests);
	CHECK(second.Facts.OutputBytes == first.Facts.OutputBytes);
	// Private generation scratch must not change logical road-test or output admission.
	REQUIRE(first.Facts.SpatialTests > 16);
	request.Limits.MaximumSpatialTests = first.Facts.SpatialTests;
	request.Limits.MaximumOutputBytes = first.Facts.OutputBytes;
	REQUIRE(GenerateRegion(recipe, request, second, diagnostic) == Status::Ok);
	CHECK(SameOutput(first, second));
	const RegionResult saved = second;
	--request.Limits.MaximumSpatialTests;
	CHECK(GenerateRegion(recipe, request, second, diagnostic) == Status::LimitExceeded);
	CHECK(SameOutput(saved, second));
	CHECK(second.Facts.SpatialTests == saved.Facts.SpatialTests);
	CHECK(second.Facts.OutputBytes == saved.Facts.OutputBytes);
	request.Limits = {};
	request.Limits.MaximumCandidateCells = 0;
	CHECK(GenerateRegion(recipe, request, second, diagnostic) == Status::LimitExceeded);
	CHECK(SameOutput(saved, second));
	request.Limits = {};
	for (SourceQuery &query : request.Queries)
		if (query.Source == "entrances") query.Matches.front().Anchor = {8 * CELL, 8 * CELL};
	CHECK(GenerateRegion(recipe, request, second, diagnostic) == Status::InvalidInput);
	CHECK(SameOutput(saved, second));
}

TEST_CASE("half-open region edge gives entrance and road one owner", "[procedural][region]") {
	const CompiledRecipe recipe = Compile(ForestVillageDefaults("town", 42));
	RegionRequest left = Request(recipe, {0, 0, 16});
	RegionRequest right = Request(recipe, {1, 0, 16});
	left.Queries[1].Matches.front().Anchor = {16 * CELL, 8 * CELL};
	right.Queries[1].Matches.front().Anchor = {16 * CELL, 8 * CELL};
	RegionResult leftResult;
	RegionResult rightResult;
	Diagnostic diagnostic;
	REQUIRE(GenerateRegion(recipe, left, leftResult, diagnostic) == Status::Ok);
	REQUIRE(GenerateRegion(recipe, right, rightResult, diagnostic) == Status::Ok);
	CHECK(leftResult.Roads.empty());
	REQUIRE(rightResult.Roads.size() == 1);
	const auto entranceCount = [](const RegionResult &result) {
		return std::count_if(
			result.Placements.begin(), result.Placements.end(), [](const Placement &placement) {
				return placement.Kind == PlacementKind::Entrance;
			}
		);
	};
	CHECK(entranceCount(leftResult) == 0);
	CHECK(entranceCount(rightResult) == 1);
}

TEST_CASE("global cell priorities keep trees apart across a region seam", "[procedural][region]") {
	Recipe definition = ForestVillageDefaults("forest", 91);
	for (auto &node : definition.Nodes)
		if (node.Kind == "forest.scatter")
			for (auto &parameter : node.Parameters)
				if (parameter.Name == "density_ppm") parameter.Value = uint64_t{1000000};
	const CompiledRecipe recipe = Compile(definition);
	RegionRequest left = Request(recipe, {0, 0, 16});
	RegionRequest right = Request(recipe, {1, 0, 16});
	for (SourceQuery &query : left.Queries)
		query.Matches.clear();
	for (SourceQuery &query : right.Queries)
		query.Matches.clear();
	RegionResult leftResult;
	RegionResult rightResult;
	Diagnostic diagnostic;
	REQUIRE(GenerateRegion(recipe, left, leftResult, diagnostic) == Status::Ok);
	REQUIRE(GenerateRegion(recipe, right, rightResult, diagnostic) == Status::Ok);
	REQUIRE(!leftResult.Placements.empty());
	REQUIRE(!rightResult.Placements.empty());
	const int64_t minimumSquared = 2 * CELL * 2 * CELL;
	for (const Placement &a : leftResult.Placements) {
		CHECK(a.Kind == PlacementKind::Tree);
		CHECK(a.Position.X < 16 * CELL);
		for (const Placement &b : rightResult.Placements) {
			const int64_t dx = a.Position.X - b.Position.X;
			const int64_t dy = a.Position.Y - b.Position.Y;
			CHECK(dx * dx + dy * dy >= minimumSquared);
		}
	}
	for (const Placement &b : rightResult.Placements) {
		CHECK(b.Kind == PlacementKind::Tree);
		CHECK(b.Position.X >= 16 * CELL);
	}
}

TEST_CASE("query halo covers village influence on seam competitor cells", "[procedural][region]") {
	const CompiledRecipe recipe = Compile(WideSpacingShortRoad());
	Bounds owner;
	Bounds query;
	Diagnostic diagnostic;
	REQUIRE(RequiredRegionBounds(recipe, {0, 0, 16}, owner, query, diagnostic) == Status::Ok);
	// A competitor cell can start 10 cells outside the owner and still need a
	// village whose 24-cell forest exclusion reaches it.
	CHECK(query.Min.X <= -34 * CELL);
	CHECK(query.Min.Y <= -34 * CELL);
	CHECK(query.Max.X >= 50 * CELL);
	CHECK(query.Max.Y >= 50 * CELL);
	RegionRequest incomplete = Request(recipe, {0, 0, 16});
	for (SourceQuery &source : incomplete.Queries)
		source.Covered.Min.X = -26 * CELL;
	std::vector<uint8_t> key{1, 2, 3};
	CHECK(BuildRegionKey(recipe, incomplete, key, diagnostic) == Status::InvalidCoverage);
	CHECK((key == std::vector<uint8_t>{1, 2, 3}));
}

TEST_CASE("roads near seam competitors consume the declared road budget", "[procedural][region]") {
	const CompiledRecipe recipe = Compile(WideSpacingShortRoad());
	RegionRequest request = Request(recipe, {0, 0, 16});
	request.Queries[0].Matches = {
		{SourceKind::Village, "edge-village", {-10 * CELL, 8 * CELL}, "", TestDigest(5)}
	};
	request.Queries[1].Matches = {
		{SourceKind::Entrance, "edge-gate", {-11 * CELL, 8 * CELL}, "edge-village", TestDigest(7)}
	};
	request.Limits.MaximumRoads = 0;
	RegionResult previous;
	previous.KeyBytes = {1, 2, 3};
	Diagnostic diagnostic;
	CHECK(GenerateRegion(recipe, request, previous, diagnostic) == Status::LimitExceeded);
	CHECK((previous.KeyBytes == std::vector<uint8_t>{1, 2, 3}));
}

TEST_CASE(
	"generation stop is transactional and progress stays outside output identity", "[procedural][region]"
) {
	const CompiledRecipe recipe = Compile(ForestVillageDefaults("town", 42));
	const RegionRequest request = Request(recipe, {0, 0, 32});
	RegionResult result;
	result.KeyBytes = {1, 2, 3};
	Diagnostic diagnostic;
	GenerationControl canceled;
	canceled.StopRequested.store(true);
	CHECK(GenerateRegion(recipe, request, result, diagnostic, &canceled) == Status::Canceled);
	CHECK((result.KeyBytes == std::vector<uint8_t>{1, 2, 3}));
	CHECK(canceled.CandidateCells.load() == 0);
	GenerationControl running;
	REQUIRE(GenerateRegion(recipe, request, result, diagnostic, &running) == Status::Ok);
	CHECK(running.CandidateCells.load() == result.Facts.CandidateCells);
	CHECK(running.WorkUnits.load() == result.Facts.WorkUnits);
	CHECK(running.SpatialTests.load() == result.Facts.SpatialTests);
	CHECK(running.Placements.load() == result.Placements.size());
	CHECK(running.Roads.load() == result.Roads.size());
	RegionResult withoutControl;
	REQUIRE(GenerateRegion(recipe, request, withoutControl, diagnostic) == Status::Ok);
	CHECK(SameOutput(result, withoutControl));
}
