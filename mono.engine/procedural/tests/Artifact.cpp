#include <engine/procedural/Artifact.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <vector>

TEST_SUITE_ID("engine.procedural.artifact")
TEST_DEPENDS("engine.procedural.region")

using namespace engine::procedural;

namespace {
	RegionResult Example() {
		RegionResult result;
		result.KeyBytes = {'R', 'G', 'K', '2', 7};
		result.Placements = {
			{"tree/b", PlacementKind::Tree, "", {-12, 25}, 3, 2, 1},
			{"tree/a", PlacementKind::Tree, "", {8, -9}, 1, 0, 1}
		};
		result.Roads = {
			{"road/west", "west", "town", std::array<Point, 3>{Point{1, 2}, Point{3, 4}, Point{5, 6}}}
		};
		result.Facts = {3, 4, 5, 6};
		return result;
	}
}

TEST_CASE("empty regional artifact has fixed version-one bytes", "[procedural][artifact]") {
	RegionResult result;
	result.KeyBytes = {'R', 'G', 'K', '2'};
	std::vector<uint8_t> bytes;
	Diagnostic diagnostic;
	REQUIRE(EncodeRegionResult(result, bytes, diagnostic) == Status::Ok);
	CHECK((bytes == std::vector<uint8_t>{'R', 'G', 'A', '1', 4, 0, 0, 0, 'R', 'G',
										 'K', '2', 0,	0,	 0, 0, 0, 0, 0,	  0}));
}

TEST_CASE("placement kind and signed coordinates have fixed artifact bytes", "[procedural][artifact]") {
	RegionResult result;
	result.KeyBytes = {'R', 'G', 'K', '2'};
	result.Placements = {{"t", PlacementKind::Tree, "", {1, -1}, 2, 3, 4}};
	std::vector<uint8_t> bytes;
	Diagnostic diagnostic;
	REQUIRE(EncodeRegionResult(result, bytes, diagnostic) == Status::Ok);
	CHECK((bytes == std::vector<uint8_t>{'R', 'G', 'A',	 '1',  4,	 0,	   0,	 0,	   'R',	 'G',  'K',
										 '2', 1,   0,	 0,	   0,	 4,	   0,	 't',  'r',	 'e',  'e',
										 1,	  0,   't',	 0,	   0,	 1,	   0,	 0,	   0,	 0,	   0,
										 0,	  0,   0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 2,
										 3,	  0,   4,	 0,	   0,	 0,	   0,	 0}));
}

TEST_CASE("artifact round-trip sorts outputs and omits runtime work facts", "[procedural][artifact]") {
	const RegionResult result = Example();
	std::vector<uint8_t> bytes;
	std::vector<uint8_t> repeated;
	RegionResult decoded;
	Diagnostic diagnostic;
	REQUIRE(EncodeRegionResult(result, bytes, diagnostic) == Status::Ok);
	REQUIRE(DecodeRegionResult(bytes, decoded, diagnostic, result.KeyBytes) == Status::Ok);
	REQUIRE(decoded.Placements.size() == 2);
	CHECK(decoded.Placements[0].Id == "tree/a");
	CHECK((decoded.Placements[0].Position == Point{8, -9}));
	CHECK(decoded.Placements[1].Id == "tree/b");
	REQUIRE(decoded.Roads.size() == 1);
	CHECK(decoded.Roads[0].Id == "road/west");
	CHECK((decoded.Roads[0].Controls[2] == Point{5, 6}));
	CHECK(decoded.Facts.CandidateCells == 0);
	CHECK(decoded.Facts.WorkUnits == 0);
	CHECK(decoded.Facts.SpatialTests == 0);
	CHECK(decoded.Facts.OutputBytes == 0);
	REQUIRE(EncodeRegionResult(decoded, repeated, diagnostic) == Status::Ok);
	CHECK(repeated == bytes);
}

TEST_CASE("stale and malformed artifacts preserve prior result", "[procedural][artifact]") {
	RegionResult result = Example();
	std::vector<uint8_t> bytes;
	Diagnostic diagnostic;
	REQUIRE(EncodeRegionResult(result, bytes, diagnostic) == Status::Ok);
	RegionResult previous;
	previous.KeyBytes = {'R', 'G', 'K', '2', 99};
	const auto saved = previous.KeyBytes;
	const std::array<uint8_t, 5> staleKey{'R', 'G', 'K', '2', 8};
	CHECK(DecodeRegionResult(bytes, previous, diagnostic, staleKey) == Status::StaleArtifact);
	CHECK(previous.KeyBytes == saved);
	bytes.push_back(0);
	CHECK(DecodeRegionResult(bytes, previous, diagnostic) == Status::InvalidInput);
	CHECK(previous.KeyBytes == saved);
	bytes.pop_back();
	bytes.pop_back();
	CHECK(DecodeRegionResult(bytes, previous, diagnostic) == Status::InvalidInput);
	CHECK(previous.KeyBytes == saved);
	result.Placements.push_back(result.Placements.front());
	std::vector<uint8_t> untouched{1, 2, 3};
	CHECK(EncodeRegionResult(result, untouched, diagnostic) == Status::InvalidInput);
	CHECK((untouched == std::vector<uint8_t>{1, 2, 3}));
	ArtifactLimits small;
	small.MaximumBytes = 20;
	result.Placements.pop_back();
	CHECK(EncodeRegionResult(result, untouched, diagnostic, small) == Status::LimitExceeded);
	CHECK((untouched == std::vector<uint8_t>{1, 2, 3}));
}

TEST_CASE("decoder rejects unknown kinds and noncanonical row order", "[procedural][artifact]") {
	RegionResult result;
	result.KeyBytes = {'R', 'G', 'K', '2'};
	result.Placements = {
		{"tree/a", PlacementKind::Tree, "", {1, 2}, 0, 0, 0},
		{"tree/b", PlacementKind::Tree, "", {3, 4}, 0, 0, 0}
	};
	std::vector<uint8_t> bytes;
	Diagnostic diagnostic;
	REQUIRE(EncodeRegionResult(result, bytes, diagnostic) == Status::Ok);
	RegionResult previous = Example();
	const auto saved = previous.KeyBytes;
	bytes[18] = 'x';
	CHECK(DecodeRegionResult(bytes, previous, diagnostic) == Status::InvalidInput);
	CHECK(previous.KeyBytes == saved);
	bytes[18] = 't';
	REQUIRE(bytes.size() == 94);
	std::swap_ranges(bytes.begin() + 16, bytes.begin() + 53, bytes.begin() + 53);
	CHECK(DecodeRegionResult(bytes, previous, diagnostic) == Status::InvalidInput);
	CHECK(previous.KeyBytes == saved);
}
