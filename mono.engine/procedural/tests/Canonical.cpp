#include <engine/procedural/Canonical.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

TEST_SUITE_ID("engine.procedural.canonical")

using namespace engine::procedural;

namespace {
	Recipe Example() {
		Recipe recipe;
		recipe.Name = "grove";
		recipe.Generator = "test.v1";
		recipe.SchemaVersion = 1;
		recipe.RootSeed = 17;
		recipe.Parameters = {{"z", uint64_t{7}}, {"a", -0.0}};
		recipe.Nodes = {
			{"b", "sink", {{"beta", Vector2{{1.0, -0.0}}}, {"alpha", StableText{"pine"}}}},
			{"a", "source", {}}
		};
		recipe.Links = {{"a", "out", "b", "in"}};
		return recipe;
	}
}

TEST_CASE("recipe bytes sort named content and normalize signed zero", "[procedural][canonical]") {
	Recipe first = Example();
	Recipe second = first;
	std::reverse(second.Parameters.begin(), second.Parameters.end());
	std::reverse(second.Nodes.begin(), second.Nodes.end());
	std::reverse(second.Nodes.back().Parameters.begin(), second.Nodes.back().Parameters.end());
	second.Parameters.front().Value = 0.0;
	for (auto &node : second.Nodes)
		for (auto &parameter : node.Parameters)
			if (parameter.Name == "beta") parameter.Value = Vector2{{1.0, 0.0}};
	std::vector<uint8_t> one;
	std::vector<uint8_t> two;
	Diagnostic diagnostic;
	REQUIRE(EncodeRecipe(first, one, diagnostic) == Status::Ok);
	REQUIRE(EncodeRecipe(second, two, diagnostic) == Status::Ok);
	CHECK(one == two);
	CHECK(one.size() > 4);
	CHECK(one[0] == 'R');
	CHECK(one[1] == 'E');
	CHECK(one[2] == 'C');
	CHECK(one[3] == '1');
}

TEST_CASE("recipe number fields have fixed little-endian bytes", "[procedural][canonical]") {
	Recipe recipe;
	recipe.Name = "x";
	recipe.Generator = "g";
	recipe.SchemaVersion = 1;
	recipe.RootSeed = 0x0102030405060708ull;
	recipe.Parameters = {{"n", uint64_t{0x1122334455667788ull}}};
	std::vector<uint8_t> bytes;
	Diagnostic diagnostic;
	REQUIRE(EncodeRecipe(recipe, bytes, diagnostic) == Status::Ok);
	CHECK((bytes == std::vector<uint8_t>{'R',  'E',	 'C',  '1',	 1,	   0,	 'x',  1,	0,	 'g', 1,
										 0,	   0,	 0,	   8,	 7,	   6,	 5,	   4,	3,	 2,	  1,
										 1,	   0,	 1,	   0,	 'n',  3,	 0,	   'u', '6', '4', 0x88,
										 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0,	0,	 0,	  0}));
}

TEST_CASE("canonical recipe round-trips every stable value tag", "[procedural][canonical]") {
	Recipe recipe = Example();
	recipe.Parameters = {
		{"bool", true},
		{"i64", int64_t{-7}},
		{"u64", uint64_t{7}},
		{"f64", -0.0},
		{"vector2", Vector2{{1.0, 2.0}}},
		{"vector3", Vector3{{1.0, 2.0, 3.0}}},
		{"vector4", Vector4{{1.0, 2.0, 3.0, 4.0}}},
		{"text", StableText{"oak"}},
		{"bytes", ByteBuffer{{1, 2, 3}}}
	};
	std::vector<uint8_t> encoded;
	std::vector<uint8_t> roundTrip;
	Recipe decoded;
	Diagnostic diagnostic;
	REQUIRE(EncodeRecipe(recipe, encoded, diagnostic) == Status::Ok);
	REQUIRE(DecodeRecipe(encoded, decoded, diagnostic) == Status::Ok);
	REQUIRE(EncodeRecipe(decoded, roundTrip, diagnostic) == Status::Ok);
	CHECK(encoded == roundTrip);
	REQUIRE(decoded.Parameters.size() == 9);
	for (const Parameter &parameter : decoded.Parameters)
		if (parameter.Name == "f64") CHECK(!std::signbit(std::get<double>(parameter.Value)));
}

TEST_CASE("decoder refuses malformed and noncanonical bytes transactionally", "[procedural][canonical]") {
	Recipe recipe;
	recipe.Name = "x";
	recipe.Generator = "g";
	recipe.SchemaVersion = 1;
	recipe.Parameters = {{"n", uint64_t{7}}};
	std::vector<uint8_t> bytes;
	Diagnostic diagnostic;
	REQUIRE(EncodeRecipe(recipe, bytes, diagnostic) == Status::Ok);
	Recipe decoded = Example();
	const std::string savedName = decoded.Name;
	auto unknownTag = bytes;
	constexpr std::array<uint8_t, 3> TAG{'u', '6', '4'};
	const auto tag = std::search(unknownTag.begin(), unknownTag.end(), TAG.begin(), TAG.end());
	REQUIRE(tag != unknownTag.end());
	*tag = 'x';
	CHECK(DecodeRecipe(unknownTag, decoded, diagnostic) == Status::InvalidRecipe);
	CHECK(decoded.Name == savedName);
	bytes.push_back(0);
	CHECK(DecodeRecipe(bytes, decoded, diagnostic) == Status::InvalidRecipe);
	CHECK(decoded.Name == savedName);
	bytes.pop_back();
	bytes.pop_back();
	CHECK(DecodeRecipe(bytes, decoded, diagnostic) == Status::InvalidRecipe);
	CHECK(decoded.Name == savedName);
	CanonicalLimits limit;
	limit.MaximumBytes = bytes.size() - 1;
	CHECK(DecodeRecipe(bytes, decoded, diagnostic, limit) == Status::LimitExceeded);
	CHECK(decoded.Name == savedName);
	recipe.Parameters.front().Value = 0.0;
	REQUIRE(EncodeRecipe(recipe, bytes, diagnostic) == Status::Ok);
	bytes[bytes.size() - 5] = 0x80;
	CHECK(DecodeRecipe(bytes, decoded, diagnostic) == Status::InvalidRecipe);
	CHECK(decoded.Name == savedName);
	recipe.Parameters = {{"b", uint64_t{2}}, {"a", uint64_t{1}}};
	REQUIRE(EncodeRecipe(recipe, bytes, diagnostic) == Status::Ok);
	REQUIRE(bytes.size() == 60);
	std::swap_ranges(bytes.begin() + 24, bytes.begin() + 40, bytes.begin() + 40);
	CHECK(DecodeRecipe(bytes, decoded, diagnostic) == Status::InvalidRecipe);
	CHECK(decoded.Name == savedName);
}

TEST_CASE(
	"invalid values and duplicate identities preserve previous canonical bytes", "[procedural][canonical]"
) {
	Recipe recipe = Example();
	std::vector<uint8_t> bytes{1, 2, 3};
	Diagnostic diagnostic;
	recipe.Parameters.front().Value = std::numeric_limits<double>::infinity();
	CHECK(EncodeRecipe(recipe, bytes, diagnostic) == Status::InvalidRecipe);
	CHECK((bytes == std::vector<uint8_t>{1, 2, 3}));
	recipe = Example();
	recipe.Parameters.push_back(recipe.Parameters.front());
	CHECK(EncodeRecipe(recipe, bytes, diagnostic) == Status::InvalidRecipe);
	CHECK((bytes == std::vector<uint8_t>{1, 2, 3}));
	recipe = Example();
	recipe.Name = std::string("bad\nname");
	CHECK(EncodeRecipe(recipe, bytes, diagnostic) == Status::InvalidRecipe);
	CHECK((bytes == std::vector<uint8_t>{1, 2, 3}));
	recipe.Name = std::string("bad\xc2\x85name");
	CHECK(EncodeRecipe(recipe, bytes, diagnostic) == Status::InvalidRecipe);
	CHECK((bytes == std::vector<uint8_t>{1, 2, 3}));
}
