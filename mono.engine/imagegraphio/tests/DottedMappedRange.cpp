#include <engine/imagegraph/Catalogue.hpp>
#include <engine/imagegraphio/PxcxStructureEdit.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <tuple>
TEST_SUITE_ID("engine.imagegraphio.dotted_mapped_range")
using namespace engine::imagegraph;
using namespace engine::imagegraphio;
namespace {
	PxcxImport ImportDottedMapped(std::string_view type, size_t index) {
		engine::bake::PxcxArchive source;
		source.MetadataNumber = 121092;
		source.MetadataText = "1.22.10.201";
		source.GraphJson = "{\"nodes\":[{\"id\":\"mapped\",\"x\":0,\"y\":0,\"type\":\"";
		source.GraphJson += type;
		source.GraphJson += "\",\"inputs\":[";
		for (size_t i = 0; i <= 24; ++i) {
			if (i) source.GraphJson += ',';
			source.GraphJson +=
				i == index
					? R"({"anim":false,"r":{"d":[0.5,2.5],"future_anim":23},"future_input":29,"attri":{"mapped":true,"future":7}})"
				: i == index + 1 ? R"({"future_map":{"keep":19}})"
				: i == 20		 ? R"({"anim":false,"r":{"d":0}})"
								 : "{}";
		}
		source.GraphJson += "]}]}";
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
	"Static mapped Dotted controls preserve source slots and unknown archive fields", "[dotted_mapped_range]"
) {
	for (const auto &[port, index, kind] : std::array{
			 std::tuple{"size", 2, "Float"},
			 std::tuple{"angle", 4, "Rotation"},
			 std::tuple{"dot_size", 9, "Slider"}
		 }) {
		const auto *entry = FindCatalogueEntry("pc.dotted");
		REQUIRE(entry);
		const auto *numeric = FindCatalogueInput(*entry, port);
		const auto *map = FindCatalogueInput(*entry, std::string(port) + "_map");
		REQUIRE(numeric);
		REQUIRE(map);
		CHECK(numeric->SourceIndex == index);
		CHECK(numeric->SourceKind == kind);
		CHECK(map->SourceIndex == index + 1);
		auto imported = ImportDottedMapped("Node_Dotted", size_t(index));
		REQUIRE(imported.Graph.Nodes.size() == 1);
		REQUIRE(imported.Graph.Nodes[0].Type == "pc.dotted");
		auto &controls = imported.Graph.Nodes[0].Values;
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
