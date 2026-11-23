// A saved pipeline set covering every operation, recorded from the reader
// before it moved onto the shared graphcore text reader.

#include <engine/bakegraph/Document.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

TEST_SUITE_ID("engine.bakegraph.documentgolden")
TEST_TIER("fast")

using namespace engine::bake;
using engine::core::Name;

namespace {
	constexpr std::string_view SAVED = "bakegraph 1\n"
									   "pipeline \"character\"\n"
									   "source \"hero.gltf\"\n"
									   "node import\n"
									   "fit 1.75\n"
									   "scale 1 0.5 0.333\n"
									   "node smooth\n"
									   "decimate 0.25\n"
									   "write \"hero\\tmesh\"\n"
									   "connect 1 2\n"
									   "connect 2 3\n"
									   "connect 3 4\n"
									   "connect 4 5\n"
									   "connect 5 6\n"
									   "connect 6 7\n"
									   "pipeline \"sprites\"\n"
									   "builtin \"quad\"\n"
									   "rasterize 512 256\n"
									   "resize 128 64\n"
									   "node opaque\n"
									   "node mipmap\n"
									   "retime 24\n"
									   "flipbook 4 13 12.5\n"
									   "write \"walk \\\"cycle\\\"\"\n"
									   "connect 1 2\n"
									   "connect 2 3\n";
}

TEST_CASE("a saved bake pipeline set reads and writes as recorded", "[bakegraph][golden]") {
	PipelineSet set;
	std::string offender;
	REQUIRE(Read(SAVED, set, offender) == DocumentStatus::Ok);
	CHECK(Write(set) == SAVED);
	REQUIRE(set.Count() == 2);
	const Document *character = set.Find(Name("character"));
	REQUIRE(character != nullptr);
	REQUIRE(character->Count() == 13);
	CHECK(character->NodeCount() == 7);
	CHECK(character->Operations()[1].Node == NodeKind::Import);
	CHECK(character->Operations()[3].Amount.Z == 0.333f);
	CHECK(character->Operations()[6].Text == "hero\tmesh");
	const Document *sprites = set.Find(Name("sprites"));
	REQUIRE(sprites != nullptr);
	CHECK(sprites->Operations()[4].Node == NodeKind::Mipmap);
	CHECK(sprites->Operations()[6].Frames == 13);
	CHECK(sprites->Operations()[7].Text == "walk \"cycle\"");

	Document single;
	const std::string text = Write(*sprites);
	REQUIRE(Read(text, single, offender) == DocumentStatus::Ok);
	CHECK(Write(single) == text);
	CHECK(text.starts_with("bakegraph 1\nbuiltin \"quad\"\n"));
}
