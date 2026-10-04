#include <engine/imagegraphio/PxcxStructureEdit.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

TEST_SUITE_ID("engine.imagegraphio.pxcx_region_origins")
TEST_DEPENDS("engine.imagegraph.animation_region_origins")
namespace {
	using namespace engine::imagegraph;
	using namespace engine::imagegraphio;
	using Json = nlohmann::ordered_json;
	PxcxImport Import(const Json &root) {
		engine::bake::PxcxArchive raw;
		raw.MetadataNumber = 121092;
		raw.MetadataText = "1.22.10.201";
		raw.GraphJson = root.dump() + '\0';
		std::vector<std::byte> bytes;
		std::string failure;
		REQUIRE(engine::bake::WritePxcx(raw, bytes, failure));
		engine::bake::PxcxArchive checked;
		REQUIRE(engine::bake::ReadPxcx(bytes, checked, failure));
		PxcxImport imported;
		const bool ok = ImportPxcxImageGraph(checked, imported, failure);
		INFO(failure);
		REQUIRE(ok);
		return imported;
	}
	PxcxImport Saved(std::span<const std::byte> bytes) {
		engine::bake::PxcxArchive checked;
		std::string failure;
		REQUIRE(engine::bake::ReadPxcx(bytes, checked, failure));
		PxcxImport imported;
		REQUIRE(ImportPxcxImageGraph(checked, imported, failure));
		return imported;
	}
	Json Root() {
		Json root{{"nodes", Json::array()}, {"aRegion", Json::array()}, {"future_project", 73}};
		for (const auto name : {"first", "middle", "last"})
			root["aRegion"].push_back(
				{{"l", ""},
				 {"c", 124},
				 {"fs", -2.5},
				 {"fe", 3.25},
				 {"future", {{"opaque", name}, {"payload", Json::array({1, "two", nullptr})}}}}
			);
		return root;
	}
	Json Graph(const PxcxImport &imported) {
		return Json::parse(imported.Source.GraphJson.c_str());
	}
}
TEST_CASE(
	"Deleting and moving indistinguishable regions carries their complete retained source records",
	"[imagegraphio][region_origin]"
) {
	const auto original = Import(Root());
	REQUIRE(original.Graph.Project);
	const auto &regions = original.Graph.Project->AnimationRegions;
	REQUIRE(regions.size() == 3);
	CHECK(regions[0].SourceRegionId == "pxc:region:0");
	CHECK(regions[2].SourceRegionId == "pxc:region:2");
	Diagnostic diagnostic;
	std::vector<std::byte> bytes;
	REQUIRE(WritePxcxProjection(original, original.Graph, {}, bytes, diagnostic));
	CHECK(bytes == original.Source.OriginalBytes);
	Document candidate = original.Graph;
	candidate.Project->AnimationRegions = {regions[2], regions[0]};
	const auto unchanged = candidate;
	const bool saved = WritePxcxProjection(original, candidate, {}, bytes, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(saved);
	CHECK(candidate == unchanged);
	CHECK(bytes != original.Source.OriginalBytes);
	const auto reimported = Saved(bytes);
	const auto root = Graph(reimported);
	REQUIRE(root["aRegion"].size() == 2);
	CHECK(root["aRegion"][0] == Root()["aRegion"][2]);
	CHECK(root["aRegion"][1] == Root()["aRegion"][0]);
	CHECK(root["future_project"] == 73);
	CHECK(reimported.Graph.Project->AnimationRegions[0].SourceRegionId == "pxc:region:0");
	std::vector<std::byte> noOp;
	REQUIRE(WritePxcxProjection(reimported, reimported.Graph, {}, noOp, diagnostic));
	CHECK(noOp == bytes);
	auto second = reimported.Graph;
	std::swap(second.Project->AnimationRegions[0], second.Project->AnimationRegions[1]);
	second.Project->AnimationRegions[1].Label = "renamed";
	REQUIRE(WritePxcxProjection(reimported, second, {}, noOp, diagnostic));
	const auto again = Graph(Saved(noOp));
	CHECK(again["aRegion"][0]["future"]["opaque"] == "first");
	CHECK(again["aRegion"][1]["future"]["opaque"] == "last");
	CHECK(again["aRegion"][1]["l"] == "renamed");
}
TEST_CASE(
	"Region insertion uses a fresh record and stale duplicate identities preserve prior bytes",
	"[imagegraphio][region_origin]"
) {
	const auto original = Import(Root());
	auto candidate = original.Graph;
	candidate.Project->AnimationRegions.insert(
		candidate.Project->AnimationRegions.begin() + 1,
		AnimationRegion{"new", {1, 2, 3, 255}, {1, .5, false}, {2, .5, false}}
	);
	Diagnostic diagnostic;
	std::vector<std::byte> output;
	const bool ok = WritePxcxProjection(original, candidate, {}, output, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(ok);
	const auto written = Graph(Saved(output));
	CHECK(written["aRegion"][0]["future"]["opaque"] == "first");
	CHECK_FALSE(written["aRegion"][1].contains("future"));
	CHECK(written["aRegion"][2]["future"]["opaque"] == "middle");
	CHECK(written["aRegion"][3]["future"]["opaque"] == "last");
	const auto previous = output;
	for (const auto identity : {"pxc:region:1", "pxc:region:99", "pxc:region:01"}) {
		auto invalid = candidate;
		invalid.Project->AnimationRegions[1].SourceRegionId = identity;
		CHECK_FALSE(WritePxcxProjection(original, invalid, {}, output, diagnostic));
		CHECK(output == previous);
		CHECK(candidate.Project->AnimationRegions[1].SourceRegionId.empty());
	}
	auto cleared = original.Graph;
	cleared.Project->AnimationRegions.clear();
	REQUIRE(WritePxcxProjection(original, cleared, {}, output, diagnostic));
	CHECK(Graph(Saved(output))["aRegion"].empty());
}
TEST_CASE(
	"Copied opaque region payload is admitted before source record cloning", "[imagegraphio][region_origin]"
) {
	auto root = Root();
	root["aRegion"].clear();
	for (size_t index = 0; index < 80; ++index)
		root["aRegion"].push_back(
			{{"l", "same"},
			 {"c", 0},
			 {"fs", 0},
			 {"fe", 1},
			 {"opaque", std::string(64 * 1024, char('a' + index % 26))}}
		);
	const auto original = Import(root);
	auto candidate = original.Graph;
	std::reverse(candidate.Project->AnimationRegions.begin(), candidate.Project->AnimationRegions.end());
	const auto before = candidate;
	Diagnostic diagnostic;
	std::vector<std::byte> output{std::byte{7}, std::byte{19}};
	CHECK_FALSE(WritePxcxProjection(original, candidate, {}, output, diagnostic));
	CHECK(diagnostic.Message.find("region record exceeds operation bounds") != std::string::npos);
	CHECK(output == std::vector<std::byte>{std::byte{7}, std::byte{19}});
	CHECK(candidate == before);
}
