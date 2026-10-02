#include <engine/imagegraph/Document.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("engine.imagegraph.dynamic_source_bindings")
using namespace engine::imagegraph;
TEST_CASE("Dynamic source layer names roundtrip as durable v9 input metadata", "[imagegraph][document]") {
	Document document;
	document.FormatVersion = 9;
	Node room;
	room.Id = "room";
	room.Type = "pc.gmroom";
	DynamicInput input;
	input.Id = "data_0";
	input.Type = ValueType::Any;
	input.SourceLayerName = "tiles foreground";
	room.DynamicInputs.push_back(input);
	document.Nodes.push_back(room);
	const auto text = Write(document);
	CHECK(text.find("dynamic_layer 0 \"room\" \"data_0\" \"tiles foreground\"") != std::string::npos);
	Document parsed;
	Diagnostic diagnostic;
	REQUIRE(Read(text, parsed, diagnostic) == Status::Ok);
	CHECK(parsed.Nodes[0].DynamicInputs[0].SourceLayerName == "tiles foreground");
	CHECK_FALSE(
		Read(text + "dynamic_layer 0 \"room\" \"data_0\" \"duplicate\"\n", parsed, diagnostic) == Status::Ok
	);
}
