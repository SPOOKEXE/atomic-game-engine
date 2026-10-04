#include <engine/imagegraph/Catalogue.hpp>
#include <engine/imagegraphio/PxcxStructureEdit.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
TEST_SUITE_ID("engine.imagegraphio.text_array_select")
using namespace engine::imagegraph;
using namespace engine::imagegraphio;
namespace {
	PxcxImport TextSelection(std::string_view selection) {
		engine::bake::PxcxArchive source;
		source.MetadataNumber = 121092;
		source.MetadataText = "1.22.10.201";
		source.GraphJson = R"({"nodes":[{"id":"text","x":0,"y":0,"type":"Node_Text","inputs":[)";
		for (size_t i = 0; i < 39; ++i) {
			if (i) source.GraphJson += ',';
			if (i == 31) {
				source.GraphJson +=
					R"({"anim":false,"r":{"d":[4294967295],"future_anim":13},"attri":{"array_select":)";
				source.GraphJson += selection;
				source.GraphJson += R"(,"future":19},"future_input":23})";
			} else
				source.GraphJson += "{}";
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
	"Text letter selection maps exact physical palette metadata and preserves unknown bytes",
	"[text_array_select]"
) {
	auto imported = TextSelection("1");
	REQUIRE(imported.Graph.Nodes.size() == 1);
	REQUIRE(imported.Graph.Nodes[0].Type == "pc.text");
	const auto &values = imported.Graph.Nodes[0].Values;
	CHECK(
		std::find(values.begin(), values.end(), AuthoredValue{"color_by_letter_select", EnumValue{1}}) !=
		values.end()
	);
	Document desired;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(imported.Graph), desired, diagnostic) == Status::Ok);
	std::vector<std::byte> bytes;
	REQUIRE(WritePxcxProjection(imported, desired, {}, bytes, diagnostic));
	CHECK(bytes == imported.Source.OriginalBytes);
	auto &edited = desired.Nodes[0].Values;
	auto selector = std::find_if(edited.begin(), edited.end(), [](const auto &value) {
		return value.Port == "color_by_letter_select";
	});
	REQUIRE(selector != edited.end());
	selector->Data = EnumValue{2};
	const bool ok = WritePxcxProjection(imported, desired, {}, bytes, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(ok);
	engine::bake::PxcxArchive checked;
	std::string failure;
	REQUIRE(engine::bake::ReadPxcx(bytes, checked, failure));
	CHECK(checked.GraphJson.find(R"("array_select":2)") != std::string::npos);
	CHECK(checked.GraphJson.find(R"("future":19)") != std::string::npos);
	CHECK(checked.GraphJson.find(R"("future_anim":13)") != std::string::npos);
	CHECK(checked.GraphJson.find(R"("future_input":23)") != std::string::npos);
	PxcxImport restored;
	REQUIRE(ImportPxcxImageGraph(checked, restored, failure));
	CHECK(restored.Graph.Nodes[0].Values == desired.Nodes[0].Values);
	selector->Data = EnumValue{3};
	bytes = {std::byte{42}};
	CHECK_FALSE(WritePxcxProjection(imported, desired, {}, bytes, diagnostic));
	CHECK(bytes == std::vector<std::byte>{std::byte{42}});
}
TEST_CASE(
	"Malformed Text letter selector remains opaque with its source metadata intact", "[text_array_select]"
) {
	for (const std::string bad : {"-1", "3", "\"Random\"", "true"}) {
		const auto imported = TextSelection(bad);
		REQUIRE(imported.Graph.Nodes.size() == 1);
		CHECK(imported.Graph.Nodes[0].Type != "pc.text");
		CHECK_FALSE(imported.Diagnostics.empty());
		CHECK(imported.Source.GraphJson.find("\"array_select\":" + bad) != std::string::npos);
	}
}
