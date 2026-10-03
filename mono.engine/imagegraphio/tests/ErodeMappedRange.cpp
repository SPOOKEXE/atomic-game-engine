#include <engine/imagegraph/Catalogue.hpp>
#include <engine/imagegraphio/PxcxStructureEdit.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
TEST_SUITE_ID("engine.imagegraphio.erode_mapped_range")
using namespace engine::imagegraph;
using namespace engine::imagegraphio;
namespace {
	PxcxImport ImportMapped(std::string_view type, size_t index) {
		engine::bake::PxcxArchive source;
		source.MetadataNumber = 121092;
		source.MetadataText = "1.22.10.201";
		source.GraphJson = "{\"nodes\":[{\"id\":\"mapped\",\"x\":0,\"y\":0,\"type\":\"";
		source.GraphJson += type;
		source.GraphJson += "\",\"inputs\":[";
		for (size_t i = 0; i <= std::max<size_t>(index, 11); ++i) {
			if (i) source.GraphJson += ',';
			source.GraphJson +=
				i == index ? R"({"anim":false,"r":{"d":[0.5,2.5]},"attri":{"mapped":true,"future":7}})"
				: i == 11  ? R"({"anim":false,"r":{"d":0}})"
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
TEST_CASE("Static mapped Erode Int endpoint pair has reversible source storage", "[erode_mapped_range]") {
	auto imported = ImportMapped("Node_Erode", 1);
	REQUIRE(imported.Graph.Nodes.size() == 1);
	REQUIRE(imported.Graph.Nodes[0].Type == "pc.erode");
	auto &controls = imported.Graph.Nodes[0].Values;
	REQUIRE(
		std::find(controls.begin(), controls.end(), AuthoredValue{"width_mapped", true}) != controls.end()
	);
	REQUIRE(
		std::find(controls.begin(), controls.end(), AuthoredValue{"width_map_range", Vector2{.5, 2.5}}) !=
		controls.end()
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
		desired.Nodes[0].Values.begin(), desired.Nodes[0].Values.end(), [](const AuthoredValue &item) {
			return item.Port == "width_map_range";
		}
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
	PxcxImport replay;
	REQUIRE(ImportPxcxImageGraph(checked, replay, failure));
	CHECK(replay.Graph.Nodes[0].Values == desired.Nodes[0].Values);
	range->Data = Vector2{9, 8};
	for (auto &item : desired.Nodes[0].Values)
		if (item.Port == "width_mapped") item.Data = int64_t{1};
	bytes = {std::byte{42}};
	CHECK_FALSE(WritePxcxProjection(imported, desired, {}, bytes, diagnostic));
	CHECK(bytes == std::vector<std::byte>{std::byte{42}});
}
TEST_CASE(
	"Erode mapped source pattern edits refuse unsupported inverse storage atomically",
	"[erode_mapped_range]"
) {
	auto imported = ImportMapped("Node_Erode", 1);
	Document desired = imported.Graph;
	std::vector<std::byte> bytes;
	Diagnostic diagnostic;
	for (int pattern = 0; pattern < 4; ++pattern) {
		auto field =
			std::find_if(desired.Nodes[0].Values.begin(), desired.Nodes[0].Values.end(), [](const auto &v) {
				return v.Port == "pattern";
			});
		if (field == desired.Nodes[0].Values.end())
			desired.Nodes[0].Values.push_back({"pattern", EnumValue{pattern}});
		else
			field->Data = EnumValue{pattern};
		for (auto &key : desired.Keyframes)
			if (key.NodeId == "mapped" && key.Port == "pattern") key.Data = EnumValue{pattern};
		const bool written = WritePxcxProjection(imported, desired, {}, bytes, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(written);
		engine::bake::PxcxArchive checked;
		std::string failure;
		REQUIRE(engine::bake::ReadPxcx(bytes, checked, failure));
		PxcxImport replay;
		REQUIRE(ImportPxcxImageGraph(checked, replay, failure));
		CHECK(
			std::find(
				replay.Graph.Nodes[0].Values.begin(),
				replay.Graph.Nodes[0].Values.end(),
				AuthoredValue{"pattern", EnumValue{pattern}}
			) != replay.Graph.Nodes[0].Values.end()
		);
	}
	for (auto &v : desired.Nodes[0].Values)
		if (v.Port == "width_map_range")
			v.Data = ArrayValue{ValueType::Vector2, {Vector2{1, 2}, Vector2{3, 4}}};
	bytes = {std::byte{42}};
	CHECK_FALSE(WritePxcxProjection(imported, desired, {}, bytes, diagnostic));
	CHECK(bytes == std::vector<std::byte>{std::byte{42}});
}
