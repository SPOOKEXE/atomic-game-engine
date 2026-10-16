#include <engine/procedural/Region.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <limits>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.procedural.authoredinputs")
TEST_DEPENDS("engine.procedural.region")

using namespace engine::procedural;

namespace {
	constexpr int64_t AUTHORED_CELL = SUBCELLS_PER_CELL;

	CompiledRecipe AuthoredCompile(bool forest = false) {
		Recipe definition = ForestVillageDefaults("authored", 91);
		for (auto &node : definition.Nodes)
			for (auto &parameter : node.Parameters)
				if (parameter.Name == "density_ppm")
					parameter.Value = uint64_t{forest && node.Kind == "forest.scatter" ? 1000000U : 0U};
		CompiledRecipe recipe;
		Diagnostic diagnostic;
		REQUIRE(CompileForestVillage(definition, recipe, diagnostic) == Status::Ok);
		return recipe;
	}

	RegionRequest AuthoredRequest(const CompiledRecipe &recipe, RegionScope scope = {}) {
		Bounds owner;
		Bounds query;
		Diagnostic diagnostic;
		REQUIRE(RequiredRegionBounds(recipe, scope, owner, query, diagnostic) == Status::Ok);
		RegionRequest request;
		request.Scope = scope;
		request.Queries = {{"villages", query, {}, true, {}}, {"entrances", query, {}, true, {}}};
		request.Authored.Covered = query;
		request.Authored.Complete = true;
		return request;
	}

	const Placement *AuthoredFind(const RegionResult &result, const std::string &id) {
		const auto found =
			std::find_if(result.Placements.begin(), result.Placements.end(), [&](const auto &p) {
				return p.Id == id;
			});
		return found == result.Placements.end() ? nullptr : &*found;
	}

	bool SameAuthoredPlacement(const Placement &left, const Placement &right) {
		return left.Id == right.Id && left.Kind == right.Kind && left.SourceName == right.SourceName &&
			   left.Position == right.Position && left.RotationQuarterTurns == right.RotationQuarterTurns &&
			   left.Variant == right.Variant && left.Material == right.Material;
	}
}

TEST_CASE(
	"authored codecs have bounded canonical bytes and transactional refusal", "[procedural][authored]"
) {
	Diagnostic diagnostic;
	PlacementOverride edit;
	edit.TargetId = "tree/-1/0";
	std::vector<uint8_t> bytes;
	REQUIRE(EncodePlacementOverride(edit, bytes, diagnostic) == Status::Ok);
	CHECK(
		(bytes ==
		 std::vector<uint8_t>{'O', 'V', 'R', '1', 9, 0, 't', 'r', 'e', 'e', '/', '-', '1', '/', '0', 0})
	);
	PlacementOverride decoded;
	REQUIRE(DecodePlacementOverride(bytes, decoded, diagnostic) == Status::Ok);
	CHECK(decoded.TargetId == edit.TargetId);
	CHECK(decoded.Action == OverrideAction::Delete);
	const auto saved = bytes;
	for (const std::string id :
		 {"tree/-0/0",
		  "tree/01/0",
		  "tree/-01/0",
		  "tree/+1/0",
		  "tree/1/0/0",
		  "house/137438953472/0",
		  "lot/0/0",
		  "tree/9223372036854775807/0"}) {
		edit.TargetId = id;
		CHECK(EncodePlacementOverride(edit, bytes, diagnostic) == Status::InvalidInput);
		CHECK(bytes == saved);
	}
	edit.TargetId = "tree/-1/0";
	edit.Position = {1, 0};
	CHECK(EncodePlacementOverride(edit, bytes, diagnostic) == Status::InvalidInput);
	CHECK(bytes == saved);
	bytes.push_back(0);
	CHECK(DecodePlacementOverride(bytes, decoded, diagnostic) == Status::InvalidInput);
	CHECK(decoded.TargetId == "tree/-1/0");
	bytes = saved;
	bytes.back() = 2;
	CHECK(DecodePlacementOverride(bytes, decoded, diagnostic) == Status::InvalidInput);
	bytes.pop_back();
	CHECK(DecodePlacementOverride(bytes, decoded, diagnostic) == Status::InvalidInput);

	AuthoredSpline spline{"s", "e", {1, -1}};
	REQUIRE(EncodeAuthoredSpline(spline, bytes, diagnostic) == Status::Ok);
	CHECK((bytes == std::vector<uint8_t>{'S', 'P', 'L', '1', 1, 0,	 's', 1,   0,	'e', 1,	  0,   0,
										 0,	  0,   0,	0,	 0, 255, 255, 255, 255, 255, 255, 255, 255}));
	AuthoredSpline splineOut;
	REQUIRE(DecodeAuthoredSpline(bytes, splineOut, diagnostic) == Status::Ok);
	CHECK(splineOut.Name == "s");
	CHECK(splineOut.EntranceName == "e");
	CHECK(splineOut.Control == spline.Control);
	bytes.push_back(0);
	CHECK(DecodeAuthoredSpline(bytes, splineOut, diagnostic) == Status::InvalidInput);
	CHECK(splineOut.Control == spline.Control);
	spline.Control.X = std::numeric_limits<int64_t>::max();
	CHECK(EncodeAuthoredSpline(spline, bytes, diagnostic) == Status::InvalidInput);
}

TEST_CASE("replacement choices and half-open influence refuse malformed extremes", "[procedural][authored]") {
	const CompiledRecipe recipe = AuthoredCompile();
	PlacementOverride edit{"tree/-1/0", OverrideAction::Replace, {16 * AUTHORED_CELL, -1}, 3, 3, 1, ""};
	Diagnostic diagnostic;
	std::vector<uint8_t> bytes;
	REQUIRE(ValidatePlacementOverride(recipe, edit, diagnostic) == Status::Ok);
	REQUIRE(EncodePlacementOverride(edit, bytes, diagnostic) == Status::Ok);
	PlacementOverride decoded;
	REQUIRE(DecodePlacementOverride(bytes, decoded, diagnostic) == Status::Ok);
	CHECK(decoded.Position == edit.Position);
	CHECK(decoded.RotationQuarterTurns == 3);
	CHECK(decoded.Variant == 3);
	CHECK(decoded.Material == 1);
	Bounds home;
	Bounds destination;
	REQUIRE(OverrideInfluence(edit, home, destination, diagnostic) == Status::Ok);
	CHECK((home == Bounds{{-AUTHORED_CELL, 0}, {0, AUTHORED_CELL}}));
	CHECK((destination == Bounds{{16 * AUTHORED_CELL, -1}, {16 * AUTHORED_CELL + 1, 0}}));
	const Bounds savedHome = home;
	const Bounds savedDestination = destination;
	edit.Variant = 4;
	CHECK(ValidatePlacementOverride(recipe, edit, diagnostic) == Status::InvalidInput);
	edit.Variant = 0;
	edit.Position.X = std::numeric_limits<int64_t>::max();
	CHECK(OverrideInfluence(edit, home, destination, diagnostic) == Status::InvalidInput);
	CHECK(home == savedHome);
	CHECK(destination == savedDestination);
	edit.Position = {};
	edit.RotationQuarterTurns = 4;
	CHECK(EncodePlacementOverride(edit, bytes, diagnostic) == Status::InvalidInput);
	edit.RotationQuarterTurns = 0;
	edit.SourceName = "village";
	CHECK(EncodePlacementOverride(edit, bytes, diagnostic) == Status::InvalidInput);
	edit.TargetId = "house/0/0";
	REQUIRE(EncodePlacementOverride(edit, bytes, diagnostic) == Status::Ok);
	REQUIRE(DecodePlacementOverride(bytes, decoded, diagnostic) == Status::Ok);
	CHECK(decoded.SourceName == "village");

	AuthoredSpline spline{"bend", "gate", {10 * AUTHORED_CELL, 9 * AUTHORED_CELL}};
	Bounds influence;
	REQUIRE(
		SplineInfluence(
			recipe,
			spline,
			{12 * AUTHORED_CELL, 8 * AUTHORED_CELL},
			{8 * AUTHORED_CELL, 8 * AUTHORED_CELL},
			influence,
			diagnostic
		) == Status::Ok
	);
	CHECK(influence.Min.X < 8 * AUTHORED_CELL);
	CHECK(influence.Max.Y > 9 * AUTHORED_CELL);
	const Bounds savedInfluence = influence;
	spline.Control.Y = 11 * AUTHORED_CELL;
	CHECK(
		SplineInfluence(
			recipe,
			spline,
			{12 * AUTHORED_CELL, 8 * AUTHORED_CELL},
			{8 * AUTHORED_CELL, 8 * AUTHORED_CELL},
			influence,
			diagnostic
		) == Status::InvalidInput
	);
	CHECK(influence == savedInfluence);
	CHECK(
		SplineInfluence(
			recipe, spline, {std::numeric_limits<int64_t>::min(), 0}, {}, influence, diagnostic
		) == Status::InvalidInput
	);
}

TEST_CASE(
	"authored spline changes actual road control while retaining named endpoints", "[procedural][authored]"
) {
	const CompiledRecipe recipe = AuthoredCompile();
	RegionRequest request = AuthoredRequest(recipe);
	request.Queries[0].Matches = {
		{SourceKind::Village, "town", {8 * AUTHORED_CELL, 8 * AUTHORED_CELL}, "", {}}
	};
	request.Queries[1].Matches = {
		{SourceKind::Entrance, "gate", {12 * AUTHORED_CELL, 8 * AUTHORED_CELL}, "town", {}}
	};
	request.Authored.Splines = {{{"bend", "gate", {10 * AUTHORED_CELL, 9 * AUTHORED_CELL}}, {}}};
	RegionResult result;
	Diagnostic diagnostic;
	REQUIRE(GenerateRegion(recipe, request, result, diagnostic) == Status::Ok);
	REQUIRE(result.Roads.size() == 1);
	CHECK(result.Roads[0].Id == "road/gate");
	CHECK(
		(result.Roads[0].Controls == std::array<Point, 3>{
										 Point{12 * AUTHORED_CELL, 8 * AUTHORED_CELL},
										 Point{10 * AUTHORED_CELL, 9 * AUTHORED_CELL},
										 Point{8 * AUTHORED_CELL, 8 * AUTHORED_CELL}
									 })
	);
	const auto savedKey = result.KeyBytes;
	request.Authored.Splines.push_back({{"other", "gate", {10 * AUTHORED_CELL, 8 * AUTHORED_CELL}}, {}});
	CHECK(GenerateRegion(recipe, request, result, diagnostic) == Status::InvalidInput);
	CHECK(result.KeyBytes == savedKey);
	request.Authored.Splines.pop_back();
	request.Authored.Splines[0].Value.EntranceName = "missing";
	CHECK(GenerateRegion(recipe, request, result, diagnostic) == Status::MissingSource);
	CHECK(result.KeyBytes == savedKey);
}

TEST_CASE("sparse deletion leaves all other baseline choices unchanged", "[procedural][authored]") {
	const CompiledRecipe recipe = AuthoredCompile(true);
	RegionRequest request = AuthoredRequest(recipe);
	RegionResult baseline;
	RegionResult edited;
	Diagnostic diagnostic;
	REQUIRE(GenerateRegion(recipe, request, baseline, diagnostic) == Status::Ok);
	REQUIRE(!baseline.Placements.empty());
	const std::string target = baseline.Placements.front().Id;
	request.Authored.Overrides = {{{target}, {}}};
	REQUIRE(GenerateRegion(recipe, request, edited, diagnostic) == Status::Ok);
	CHECK(AuthoredFind(edited, target) == nullptr);
	CHECK(edited.Placements.size() + 1 == baseline.Placements.size());
	for (const auto &placement : edited.Placements) {
		const Placement *original = AuthoredFind(baseline, placement.Id);
		REQUIRE(original != nullptr);
		CHECK(SameAuthoredPlacement(placement, *original));
	}
}

TEST_CASE("manual moves keep stable IDs and own the half-open destination", "[procedural][authored]") {
	const CompiledRecipe recipe = AuthoredCompile();
	RegionRequest left = AuthoredRequest(recipe, {0, 0, 16});
	RegionRequest right = AuthoredRequest(recipe, {1, 0, 16});
	OverrideFact moved{
		{"tree/0/0", OverrideAction::Replace, {16 * AUTHORED_CELL, 8 * AUTHORED_CELL}, 2, 3, 1, ""}, {}
	};
	left.Authored.Overrides = {moved};
	right.Authored.Overrides = {moved};
	RegionResult leftResult;
	RegionResult rightResult;
	Diagnostic diagnostic;
	REQUIRE(GenerateRegion(recipe, left, leftResult, diagnostic) == Status::Ok);
	REQUIRE(GenerateRegion(recipe, right, rightResult, diagnostic) == Status::Ok);
	CHECK(leftResult.Placements.empty());
	REQUIRE(rightResult.Placements.size() == 1);
	const auto &placement = rightResult.Placements[0];
	CHECK(placement.Id == moved.Value.TargetId);
	CHECK(placement.Position == moved.Value.Position);
	CHECK(placement.RotationQuarterTurns == 2);
	CHECK(placement.Variant == 3);
	CHECK(placement.Material == 1);
	RegionRequest unrelated = AuthoredRequest(recipe, {2, 0, 16});
	unrelated.Authored.Overrides = {moved};
	CHECK(GenerateRegion(recipe, unrelated, rightResult, diagnostic) == Status::InvalidCoverage);
	CHECK(rightResult.Placements[0].Id == moved.Value.TargetId);
}

TEST_CASE(
	"manual house replacement carries its lot and obeys shared output budgets", "[procedural][authored]"
) {
	const CompiledRecipe recipe = AuthoredCompile();
	RegionRequest request = AuthoredRequest(recipe);
	request.Authored.Overrides = {{{"house/0/0", OverrideAction::Replace, {5, 7}, 3, 2, 1, "town"}, {}}};
	RegionResult result;
	Diagnostic diagnostic;
	REQUIRE(GenerateRegion(recipe, request, result, diagnostic) == Status::Ok);
	REQUIRE(result.Placements.size() == 2);
	const Placement *house = AuthoredFind(result, "house/0/0");
	const Placement *lot = AuthoredFind(result, "lot/0/0");
	REQUIRE(house != nullptr);
	REQUIRE(lot != nullptr);
	CHECK(house->Kind == PlacementKind::House);
	CHECK(house->Variant == 2);
	CHECK(lot->Kind == PlacementKind::VillageLot);
	CHECK(lot->Position == house->Position);
	CHECK(lot->SourceName == house->SourceName);
	CHECK(lot->Material == house->Material);
	CHECK(lot->Variant == 0);
	const auto savedKey = result.KeyBytes;
	request.Limits.MaximumPlacements = 1;
	CHECK(GenerateRegion(recipe, request, result, diagnostic) == Status::LimitExceeded);
	CHECK(result.KeyBytes == savedKey);
	CHECK(result.Placements.size() == 2);
	request.Limits = {};
	request.Limits.MaximumOutputBytes = result.Facts.OutputBytes - 1;
	CHECK(GenerateRegion(recipe, request, result, diagnostic) == Status::LimitExceeded);
	CHECK(result.KeyBytes == savedKey);
	request.Limits = {};
	request.Limits.MaximumWorkUnits = 0;
	CHECK(GenerateRegion(recipe, request, result, diagnostic) == Status::LimitExceeded);
	CHECK(result.KeyBytes == savedKey);
	request.Limits = {};
	request.Authored.Overrides[0].Value.Variant = 3;
	CHECK(GenerateRegion(recipe, request, result, diagnostic) == Status::InvalidInput);
	CHECK(result.KeyBytes == savedKey);
}

TEST_CASE("cross-region replacement suppresses its real baseline home", "[procedural][authored]") {
	const CompiledRecipe recipe = AuthoredCompile(true);
	RegionRequest left = AuthoredRequest(recipe, {0, 0, 16});
	RegionRequest right = AuthoredRequest(recipe, {1, 0, 16});
	RegionResult baseline;
	RegionResult result;
	Diagnostic diagnostic;
	REQUIRE(GenerateRegion(recipe, left, baseline, diagnostic) == Status::Ok);
	REQUIRE(!baseline.Placements.empty());
	const Placement original = baseline.Placements.front();
	OverrideFact moved{
		{original.Id,
		 OverrideAction::Replace,
		 {16 * AUTHORED_CELL, 8 * AUTHORED_CELL},
		 original.RotationQuarterTurns,
		 original.Variant,
		 original.Material,
		 ""},
		{}
	};
	left.Authored.Overrides = {moved};
	right.Authored.Overrides = {moved};
	REQUIRE(GenerateRegion(recipe, left, result, diagnostic) == Status::Ok);
	CHECK(AuthoredFind(result, original.Id) == nullptr);
	CHECK(result.Placements.size() + 1 == baseline.Placements.size());
	REQUIRE(GenerateRegion(recipe, right, result, diagnostic) == Status::Ok);
	const Placement *replacement = AuthoredFind(result, original.Id);
	REQUIRE(replacement != nullptr);
	CHECK(replacement->Position == moved.Value.Position);
	CHECK(replacement->Variant == original.Variant);
	CHECK(std::count_if(result.Placements.begin(), result.Placements.end(), [&](const auto &p) {
			  return p.Id == original.Id;
		  }) == 1);
}

TEST_CASE(
	"regional keys sort authored facts and require complete bounded local witnesses", "[procedural][authored]"
) {
	const CompiledRecipe recipe = AuthoredCompile();
	RegionRequest request = AuthoredRequest(recipe);
	request.Authored.Overrides = {{{"tree/1/0"}, {}}, {{"tree/0/0"}, {}}};
	std::vector<uint8_t> key;
	std::vector<uint8_t> second;
	Diagnostic diagnostic;
	REQUIRE(BuildRegionKey(recipe, request, key, diagnostic) == Status::Ok);
	REQUIRE(key.size() >= 4);
	CHECK((std::vector<uint8_t>(key.begin(), key.begin() + 4) == std::vector<uint8_t>{'R', 'G', 'K', '2'}));
	std::reverse(request.Authored.Overrides.begin(), request.Authored.Overrides.end());
	request.RequestSequence = 42;
	request.ExpectedSourceRevision = 91;
	REQUIRE(BuildRegionKey(recipe, request, second, diagnostic) == Status::Ok);
	CHECK(key == second);
	request.Authored.Overrides[0].ContentDigest.Bytes[0] = 1;
	REQUIRE(BuildRegionKey(recipe, request, second, diagnostic) == Status::Ok);
	CHECK(key != second);
	request.Authored.Overrides[0].ContentDigest = {};
	request.Authored.CoverageDigest.Bytes[0] = 1;
	REQUIRE(BuildRegionKey(recipe, request, second, diagnostic) == Status::Ok);
	CHECK(key != second);
	request.Authored.CoverageDigest = {};
	request.Authored.Overrides[0].Value.TargetId = "tree/2/0";
	REQUIRE(BuildRegionKey(recipe, request, second, diagnostic) == Status::Ok);
	CHECK(key != second);
	const auto saved = second;
	request.Authored.Complete = false;
	CHECK(BuildRegionKey(recipe, request, second, diagnostic) == Status::InvalidCoverage);
	CHECK(second == saved);
	request.Authored.Complete = true;
	++request.Authored.Covered.Max.X;
	CHECK(BuildRegionKey(recipe, request, second, diagnostic) == Status::InvalidCoverage);
	CHECK(second == saved);
	--request.Authored.Covered.Max.X;
	request.Authored.Overrides[0] = request.Authored.Overrides[1];
	CHECK(BuildRegionKey(recipe, request, second, diagnostic) == Status::InvalidInput);
	CHECK(second == saved);
	request.Authored.Overrides.clear();
	request.Authored.Overrides.resize(513);
	CHECK(BuildRegionKey(recipe, request, second, diagnostic) == Status::LimitExceeded);
	CHECK(second == saved);
	request.Authored.Overrides = {{{"tree/0/0"}, {}}};
	request.Queries[0].Matches = {{SourceKind::Village, "town", {}, "", {}}};
	request.Limits.MaximumQueryLeaves = 1;
	CHECK(BuildRegionKey(recipe, request, second, diagnostic) == Status::LimitExceeded);
	CHECK(second == saved);
	request.Limits = {};
	request.Queries[0].Matches.clear();
	request.Authored.Overrides.clear();
	request.Authored.Complete = false;
	CHECK(BuildRegionKey(recipe, request, second, diagnostic) == Status::InvalidCoverage);
	CHECK(second == saved);
}

TEST_CASE("authored leaf ceiling accepts exactly 512 local records", "[procedural][authored]") {
	const CompiledRecipe recipe = AuthoredCompile();
	RegionRequest request = AuthoredRequest(recipe);
	for (int y = 0; y < 16; ++y)
		for (int x = 0; x < 16; ++x)
			for (const std::string kind : {"tree/", "house/"}) {
				PlacementOverride edit;
				edit.TargetId = kind + std::to_string(x) + "/" + std::to_string(y);
				request.Authored.Overrides.push_back({edit, {}});
			}
	REQUIRE(request.Authored.Overrides.size() == 512);
	std::vector<uint8_t> bytes;
	Diagnostic diagnostic;
	REQUIRE(BuildRegionKey(recipe, request, bytes, diagnostic) == Status::Ok);
	const auto saved = bytes;
	request.Limits.MaximumQueryLeaves = 511;
	CHECK(BuildRegionKey(recipe, request, bytes, diagnostic) == Status::LimitExceeded);
	CHECK(bytes == saved);
}

TEST_CASE("authored spline keys sort values and bind named fact digests", "[procedural][authored]") {
	const CompiledRecipe recipe = AuthoredCompile();
	RegionRequest request = AuthoredRequest(recipe);
	request.Queries[0].Matches = {
		{SourceKind::Village, "town", {8 * AUTHORED_CELL, 8 * AUTHORED_CELL}, "", {}}
	};
	request.Queries[1].Matches = {
		{SourceKind::Entrance, "west", {4 * AUTHORED_CELL, 8 * AUTHORED_CELL}, "town", {}},
		{SourceKind::Entrance, "east", {12 * AUTHORED_CELL, 8 * AUTHORED_CELL}, "town", {}}
	};
	request.Authored.Splines = {
		{{"west-bend", "west", {6 * AUTHORED_CELL, 9 * AUTHORED_CELL}}, {}},
		{{"east-bend", "east", {10 * AUTHORED_CELL, 9 * AUTHORED_CELL}}, {}}
	};
	std::vector<uint8_t> bytes;
	std::vector<uint8_t> other;
	Diagnostic diagnostic;
	REQUIRE(BuildRegionKey(recipe, request, bytes, diagnostic) == Status::Ok);
	std::reverse(request.Authored.Splines.begin(), request.Authored.Splines.end());
	REQUIRE(BuildRegionKey(recipe, request, other, diagnostic) == Status::Ok);
	CHECK(other == bytes);
	request.Authored.Splines[0].ContentDigest.Bytes[0] = 1;
	REQUIRE(BuildRegionKey(recipe, request, other, diagnostic) == Status::Ok);
	CHECK(other != bytes);
	request.Authored.Splines[0].ContentDigest = {};
	++request.Authored.Splines[0].Value.Control.Y;
	REQUIRE(BuildRegionKey(recipe, request, other, diagnostic) == Status::Ok);
	CHECK(other != bytes);
}
