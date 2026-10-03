#include <engine/imagegraph/Catalogue.hpp>
#include <engine/imagegraphio/PxcxStructureEdit.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <tuple>
TEST_SUITE_ID("engine.imagegraphio.stripe_mapped_range")
using namespace engine::imagegraph;
using namespace engine::imagegraphio;
namespace {
	PxcxImport ImportStripeMapped(std::string_view type, size_t index, size_t mapIndex) {
		engine::bake::PxcxArchive source;
		source.MetadataNumber = 121092;
		source.MetadataText = "1.22.10.201";
		source.GraphJson = "{\"nodes\":[{\"id\":\"mapped\",\"x\":0,\"y\":0,\"type\":\"";
		source.GraphJson += type;
		source.GraphJson += "\",\"inputs\":[";
		for (size_t i = 0; i <= 25; ++i) {
			if (i) source.GraphJson += ',';
			source.GraphJson +=
				i == index
					? R"({"anim":false,"r":{"d":[0.5,2.5],"future_anim":23},"future_input":29,"attri":{"mapped":true,"future":7}})"
				: i == mapIndex ? R"({"future_map":{"keep":19}})"
				: i == 19		? R"({"anim":false,"r":{"d":0}})"
								: "{}";
		}
		source.GraphJson += "]}]}";
		if (index == 1) {
			const auto position = source.GraphJson.find(R"("future_input":29)");
			REQUIRE(position != std::string::npos);
			source.GraphJson.insert(position, R"("unit":0,)");
		}
		source.GraphJson.push_back('\0');
		std::vector<std::byte> bytes;
		std::string failure;
		REQUIRE(engine::bake::WritePxcx(source, bytes, failure));
		engine::bake::PxcxArchive checked;
		REQUIRE(engine::bake::ReadPxcx(bytes, checked, failure));
		PxcxImport imported;
		REQUIRE(ImportPxcxImageGraph(checked, imported, failure));
		return imported;
	}
}
TEST_CASE(
	"Static mapped Stripe controls preserve source slots and unknown archive fields", "[stripe_mapped_range]"
) {
	for (const auto &[port, index, mapIndex, kind] : std::array{
			 std::tuple{"size", 1, 11, "Slider"},
			 std::tuple{"angle", 2, 12, "Rotation"},
			 std::tuple{"random", 5, 13, "Slider"},
			 std::tuple{"strip_ratio", 10, 14, "Slider"}
		 }) {
		const auto *entry = FindCatalogueEntry("pc.stripe");
		REQUIRE(entry);
		const auto *numeric = FindCatalogueInput(*entry, port);
		const auto *map = FindCatalogueInput(*entry, std::string(port) + "_map");
		REQUIRE(numeric);
		REQUIRE(map);
		CHECK(numeric->SourceIndex == index);
		CHECK(numeric->SourceKind == kind);
		CHECK(map->SourceIndex == mapIndex);
		auto imported = ImportStripeMapped("Node_Stripe", size_t(index), size_t(mapIndex));
		REQUIRE(imported.Graph.Nodes.size() == 1);
		REQUIRE(imported.Graph.Nodes[0].Type == "pc.stripe");
		auto &controls = imported.Graph.Nodes[0].Values;
		if (index == 1)
			REQUIRE(
				std::find(controls.begin(), controls.end(), AuthoredValue{"size_unit", EnumValue{0}}) !=
				controls.end()
			);
		REQUIRE(
			std::find(controls.begin(), controls.end(), AuthoredValue{std::string(port) + "_mapped", true}) !=
			controls.end()
		);
		REQUIRE(
			std::find(
				controls.begin(),
				controls.end(),
				AuthoredValue{std::string(port) + "_map_range", Vector2{.5, 2.5}}
			) != controls.end()
		);
		Diagnostic diagnostic;
		Document desired;
		REQUIRE(Read(Write(imported.Graph), desired, diagnostic) == Status::Ok);
		CHECK(desired == imported.Graph);
		std::vector<std::byte> bytes;
		const bool unchanged = WritePxcxProjection(imported, desired, {}, bytes, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(unchanged);
		CHECK(bytes == imported.Source.OriginalBytes);
		auto range = std::find_if(
			desired.Nodes[0].Values.begin(),
			desired.Nodes[0].Values.end(),
			[port](const AuthoredValue &item) { return item.Port == std::string(port) + "_map_range"; }
		);
		REQUIRE(range != desired.Nodes[0].Values.end());
		range->Data = Vector2{1.5, 3.5};
		const bool written = WritePxcxProjection(imported, desired, {}, bytes, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(written);
		engine::bake::PxcxArchive checked;
		std::string failure;
		REQUIRE(engine::bake::ReadPxcx(bytes, checked, failure));
		CHECK(checked.GraphJson.find("\"future\":7") != std::string::npos);
		CHECK(checked.GraphJson.find("\"future_anim\":23") != std::string::npos);
		CHECK(checked.GraphJson.find("\"future_input\":29") != std::string::npos);
		if (index == 1) CHECK(checked.GraphJson.find(R"("unit":0)") != std::string::npos);
		CHECK(checked.GraphJson.find(R"("mapped":true)") != std::string::npos);
		CHECK(checked.GraphJson.find(R"("future_map":{"keep":19})") != std::string::npos);
		CHECK(checked.GraphJson.find(R"("d":[1.5,3.5])") != std::string::npos);
		PxcxImport replay;
		REQUIRE(ImportPxcxImageGraph(checked, replay, failure));
		CHECK(replay.Graph.Nodes[0].Values == desired.Nodes[0].Values);
		SECTION("Invalid toggle preserves the previous archive") {
			range->Data = Vector2{9, 8};
			for (auto &item : desired.Nodes[0].Values)
				if (item.Port == std::string(port) + "_mapped") item.Data = int64_t{1};
			bytes = {std::byte{42}};
			CHECK_FALSE(WritePxcxProjection(imported, desired, {}, bytes, diagnostic));
			CHECK(bytes == std::vector<std::byte>{std::byte{42}});
		}
		SECTION("Processor array is not a fixed two-endpoint range") {
			range->Data = ArrayValue{ValueType::Vector2, {Vector2{1, 2}, Vector2{3, 4}}};
			bytes = {std::byte{42}};
			CHECK_FALSE(WritePxcxProjection(imported, desired, {}, bytes, diagnostic));
			CHECK(bytes == std::vector<std::byte>{std::byte{42}});
		}
	}
}

TEST_CASE("Static Stripe gradient map toggle edits its source gradient attributes", "[stripe_mapped_range]") {
	auto imported = ImportStripeMapped("Node_Stripe", 1, 11);
	auto desired = imported.Graph;
	auto toggle = std::find_if(
		desired.Nodes[0].Values.begin(), desired.Nodes[0].Values.end(), [](const AuthoredValue &v) {
			return v.Port == "colors_mapped";
		}
	);
	if (toggle == desired.Nodes[0].Values.end()) {
		// The importer retains physical source order: Colors is slot 7, Seed is slot 19.
		const auto seed = std::find_if(
			desired.Nodes[0].Values.begin(), desired.Nodes[0].Values.end(), [](const AuthoredValue &v) {
				return v.Port == "seed";
			}
		);
		REQUIRE(seed != desired.Nodes[0].Values.end());
		desired.Nodes[0].Values.insert(seed, {"colors_mapped", true});
	} else
		toggle->Data = true;
	Diagnostic diagnostic;
	std::vector<std::byte> bytes;
	const bool written = WritePxcxProjection(imported, desired, {}, bytes, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(written);
	engine::bake::PxcxArchive checked;
	std::string failure;
	REQUIRE(engine::bake::ReadPxcx(bytes, checked, failure));
	PxcxImport replay;
	REQUIRE(ImportPxcxImageGraph(checked, replay, failure));
	REQUIRE(replay.Graph.Nodes.size() == 1);
	CHECK(replay.Graph.Nodes[0].Type == "pc.stripe");
	CHECK(replay.Graph.Nodes[0].Values == desired.Nodes[0].Values);
	CHECK(
		std::find(
			replay.Graph.Nodes[0].Values.begin(),
			replay.Graph.Nodes[0].Values.end(),
			AuthoredValue{"colors_mapped", true}
		) != replay.Graph.Nodes[0].Values.end()
	);
	CHECK(checked.GraphJson.find("future_map") != std::string::npos);
	for (auto &v : desired.Nodes[0].Values)
		if (v.Port == "colors_mapped") v.Data = int64_t{1};
	bytes = {std::byte{42}};
	CHECK_FALSE(WritePxcxProjection(imported, desired, {}, bytes, diagnostic));
	CHECK(bytes == std::vector<std::byte>{std::byte{42}});
}
