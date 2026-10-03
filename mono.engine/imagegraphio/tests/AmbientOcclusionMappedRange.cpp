#include <engine/imagegraph/Catalogue.hpp>
#include <engine/imagegraphio/PxcxStructureEdit.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
TEST_SUITE_ID("engine.imagegraphio.ambient_occlusion_mapped_range")
using namespace engine::imagegraph;
using namespace engine::imagegraphio;
namespace {
	PxcxImport ImportOcclusionMapped(std::string_view type, size_t index) {
		engine::bake::PxcxArchive source;
		source.MetadataNumber = 121092;
		source.MetadataText = "1.22.10.201";
		source.GraphJson = "{\"nodes\":[{\"id\":\"mapped\",\"x\":0,\"y\":0,\"type\":\"";
		source.GraphJson += type;
		source.GraphJson += "\",\"inputs\":[";
		for (size_t i = 0; i <= index; ++i) {
			if (i) source.GraphJson += ',';
			source.GraphJson +=
				i == index ? R"({"anim":false,"r":{"d":[0.5,2.5]},"attri":{"mapped":true,"future":7}})"
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
	"Source AO Float and Slider static mapped pairs retain source slots", "[ambient_occlusion_mapped_range]"
) {
	for (const auto &[port, index] :
		 std::array<std::pair<std::string, size_t>, 2>{{{"height", 3}, {"intensity", 1}}}) {
		auto imported = ImportOcclusionMapped("Node_Ambient_Occlusion", index);
		REQUIRE(imported.Graph.Nodes.size() == 1);
		REQUIRE(imported.Graph.Nodes[0].Type == "pc.ambient_occlusion");
		const auto &controls = imported.Graph.Nodes[0].Values;
		REQUIRE(
			std::find(controls.begin(), controls.end(), AuthoredValue{port + "_mapped", true}) !=
			controls.end()
		);
		REQUIRE(
			std::find(
				controls.begin(), controls.end(), AuthoredValue{port + "_map_range", Vector2{.5, 2.5}}
			) != controls.end()
		);
		Diagnostic diagnostic;
		Document desired;
		REQUIRE(Read(Write(imported.Graph), desired, diagnostic) == Status::Ok);
		std::vector<std::byte> bytes;
		REQUIRE(WritePxcxProjection(imported, desired, {}, bytes, diagnostic));
		REQUIRE(bytes == imported.Source.OriginalBytes);
		auto range = std::find_if(
			desired.Nodes[0].Values.begin(), desired.Nodes[0].Values.end(), [&](const AuthoredValue &v) {
				return v.Port == port + "_map_range";
			}
		);
		REQUIRE(range != desired.Nodes[0].Values.end());
		range->Data = Vector2{1.5, 3.5};
		const auto written = WritePxcxProjection(imported, desired, {}, bytes, diagnostic);
		INFO(diagnostic.Port << ":" << diagnostic.Message);
		REQUIRE(written);
		engine::bake::PxcxArchive checked;
		std::string failure;
		REQUIRE(engine::bake::ReadPxcx(bytes, checked, failure));
		REQUIRE(checked.GraphJson.find("\"future\":7") != std::string::npos);
		PxcxImport replay;
		REQUIRE(ImportPxcxImageGraph(checked, replay, failure));
		REQUIRE(replay.Graph.Nodes[0].Values == desired.Nodes[0].Values);
		for (auto &v : desired.Nodes[0].Values)
			if (v.Port == port + "_mapped") v.Data = int64_t{1};
		bytes = {std::byte{42}};
		REQUIRE_FALSE(WritePxcxProjection(imported, desired, {}, bytes, diagnostic));
		REQUIRE(bytes == std::vector<std::byte>{std::byte{42}});
	}
}
