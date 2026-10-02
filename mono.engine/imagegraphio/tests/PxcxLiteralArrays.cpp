#include <engine/imagegraph/Catalogue.hpp>
#include <engine/imagegraphio/PxcxEdit.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.imagegraphio.pxcxliteralarrays")
TEST_DEPENDS("engine.imagegraphio.pxcximport")
TEST_DEPENDS("engine.imagegraphio.pxcxedit")

using namespace engine::imagegraph;
using namespace engine::imagegraphio;

namespace {
	std::string Static(std::string_view data) {
		return R"({"r":{"d":)" + std::string(data) + R"(},"future":{"keep":17}})";
	}
	PxcxImport Imported(std::string_view type, const std::vector<std::string> &inputs) {
		engine::bake::PxcxArchive source;
		source.MetadataNumber = 121092;
		source.MetadataText = "1.22.10.201";
		source.GraphJson =
			R"({"nodes":[{"id":"value","type":")" + std::string(type) + R"(","x":2,"y":4,"inputs":[)";
		for (size_t index = 0; index < inputs.size(); ++index) {
			if (index) source.GraphJson += ',';
			source.GraphJson += inputs[index];
		}
		source.GraphJson += R"(],"future":{"keep":"node"}}],"future":{"keep":"project"}})";
		source.GraphJson.push_back('\0');
		std::vector<std::byte> bytes;
		std::string failure;
		REQUIRE(engine::bake::WritePxcx(source, bytes, failure));
		engine::bake::PxcxArchive checked;
		REQUIRE(engine::bake::ReadPxcx(bytes, checked, failure));
		PxcxImport result;
		REQUIRE(ImportPxcxImageGraph(checked, result, failure));
		return result;
	}
	std::vector<std::string> Math(std::string_view a, std::string_view b, std::string_view amount) {
		return {
			Static("13"),
			Static(a),
			Static(b),
			Static("1"),
			Static("false"),
			Static(amount),
			Static("[0,1]"),
			Static("[0,1]"),
			Static("false")
		};
	}
	Document Selected(const PxcxImport &imported) {
		Document graph = imported.Graph;
		REQUIRE(graph.Nodes.size() == 1);
		const auto *entry = FindCatalogueEntry(graph.Nodes[0].Type);
		REQUIRE(entry != nullptr);
		REQUIRE(entry->Outputs.size() == 1);
		graph.Outputs = {{"result", "value", std::string(entry->Outputs[0].Id)}};
		return graph;
	}
	Value Evaluated(const Document &graph) {
		Diagnostic diagnostic;
		Plan plan;
		const auto compiled = Compile(graph, plan, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(compiled == Status::Ok);
		EvaluatedValue value;
		const auto evaluated = EvaluateValue(graph, plan, "result", {}, value, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(evaluated == Status::Ok);
		return value.Data;
	}
	void Persisted(const PxcxImport &imported, const Value &expected) {
		const auto graph = Selected(imported);
		CHECK(Evaluated(graph) == expected);
		Document restored;
		Diagnostic diagnostic;
		REQUIRE(Read(Write(graph), restored, diagnostic) == Status::Ok);
		CHECK(restored == graph);
		CHECK(Evaluated(restored) == expected);
	}
	PxcxImport Edited(const PxcxImport &imported, std::span<const PxcxEdit> edits) {
		std::vector<std::byte> bytes;
		Diagnostic diagnostic;
		const bool written =
			WritePxcxEdits(imported, imported.Source.OriginalBytes, edits, bytes, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(written);
		engine::bake::PxcxArchive archive;
		std::string failure;
		REQUIRE(engine::bake::ReadPxcx(bytes, archive, failure));
		PxcxImport result;
		REQUIRE(ImportPxcxImageGraph(archive, result, failure, imported.Options));
		CHECK(result.Source.GraphJson.find(R"("future":{"keep":17})") != std::string::npos);
		CHECK(result.Source.GraphJson.find(R"("future":{"keep":"node"})") != std::string::npos);
		CHECK(result.Source.GraphJson.find(R"("future":{"keep":"project"})") != std::string::npos);
		return result;
	}
}

TEST_CASE(
	"PXC Number Simple literal arrays persist and edit through the source catalogue",
	"[imagegraphio][pxcx_literal_array]"
) {
	// Pinned node_number_simple.gml update passes the authored Value through unchanged.
	const auto imported = Imported("Node_Number_Simple", {Static("[1,2]")});
	REQUIRE(imported.Graph.Nodes[0].Type == "pc.number_simple");
	REQUIRE(imported.CatalogueNodes == 1);
	Persisted(imported, ArrayValue{ValueType::Scalar, {1.0, 2.0}});
	const ArrayValue values{ValueType::Scalar, {3.0, 5.0, 8.0}};
	const std::array<PxcxEdit, 1> edits{PxcxInputValueEdit{"value", "value", values}};
	const auto replay = Edited(imported, edits);
	REQUIRE(replay.Graph.Nodes[0].Type == "pc.number_simple");
	Persisted(replay, values);
}

TEST_CASE(
	"PXC Math literal arrays retain manual loop broadcasting for all three numeric controls",
	"[imagegraphio][pxcx_literal_array]"
) {
	// Pinned node_math.gml evalArray loops a/b/Amount independently to their largest count.
	const auto imported = Imported("Node_Math", Math("[2,4,8]", "[10,20]", "[0.25,0.5]"));
	REQUIRE(imported.Graph.Nodes[0].Type == "pc.math");
	REQUIRE(imported.CatalogueNodes == 1);
	Persisted(imported, ArrayValue{ValueType::Scalar, {4.0, 12.0, 8.5}});
	const std::array<PxcxEdit, 3> edits{
		PxcxInputValueEdit{"value", "a", ArrayValue{ValueType::Scalar, {6.0, 2.0, 10.0}}},
		PxcxInputValueEdit{"value", "b", ArrayValue{ValueType::Scalar, {14.0}}},
		PxcxInputValueEdit{"value", "amount", ArrayValue{ValueType::Scalar, {.5, .25, .75}}}
	};
	const auto replay = Edited(imported, edits);
	REQUIRE(replay.Graph.Nodes[0].Type == "pc.math");
	Persisted(replay, ArrayValue{ValueType::Scalar, {10.0, 5.0, 13.0}});
}

TEST_CASE(
	"PXC Compare literal arrays retain manual zero padding and Boolean output leaves",
	"[imagegraphio][pxcx_literal_array]"
) {
	// Pinned node_compare.gml evalArray pads the shorter operand with zero instead of looping.
	const auto imported = Imported("Node_Compare", {Static("2"), Static("[5,1,0]"), Static("[2]")});
	REQUIRE(imported.Graph.Nodes[0].Type == "pc.compare");
	Persisted(imported, ArrayValue{ValueType::Boolean, {true, true, false}});
	const std::array<PxcxEdit, 2> edits{
		PxcxInputValueEdit{"value", "a", ArrayValue{ValueType::Scalar, {2.0, 5.0}}},
		PxcxInputValueEdit{"value", "b", ArrayValue{ValueType::Scalar, {3.0, 4.0, -1.0}}}
	};
	const auto replay = Edited(imported, edits);
	Persisted(replay, ArrayValue{ValueType::Boolean, {false, true, true}});
}

TEST_CASE(
	"PXC manual numeric arrays reject unsupported shapes and controls without partial projection",
	"[imagegraphio][pxcx_literal_array]"
) {
	std::string oversized = "[";
	for (size_t index = 0; index <= Limits::MaximumArrayElements; ++index) {
		if (index) oversized += ',';
		oversized += '1';
	}
	oversized += ']';
	for (const auto &data : {std::string("[1,\"text\"]"), std::string("[[1],[2]]"), oversized}) {
		for (const auto &[type, inputs] : std::array{
				 std::pair{"Node_Number_Simple", std::vector{Static(data)}},
				 std::pair{"Node_Math", Math(data, "[1]", "[0.5]")},
				 std::pair{"Node_Compare", std::vector{Static("0"), Static(data), Static("[1]")}}
			 }) {
			const auto imported = Imported(type, inputs);
			REQUIRE(imported.Graph.Nodes.size() == 1);
			CHECK(FindCatalogueEntry(imported.Graph.Nodes[0].Type) == nullptr);
			CHECK(imported.Graph.Nodes[0].Values.empty());
			CHECK(imported.Graph.Keyframes.empty());
			CHECK(imported.CatalogueNodes == 0);
			CHECK_FALSE(imported.Diagnostics.empty());
			std::vector<std::byte> bytes;
			std::string failure;
			REQUIRE(engine::bake::WritePxcx(imported.Source, bytes, failure));
			CHECK(bytes == imported.Source.OriginalBytes);
		}
	}
	const auto unknownControl = Imported("Node_Compare", {Static("[0,1]"), Static("[1,2]"), Static("[3,4]")});
	CHECK(FindCatalogueEntry(unknownControl.Graph.Nodes[0].Type) == nullptr);
	CHECK(unknownControl.Graph.Nodes[0].Values.empty());
}

TEST_CASE(
	"PXC boolean numeric literals retain source numeric broadcasting and persistence",
	"[imagegraphio][pxcx_literal_array]"
) {
	// GML boolean values are numeric zero/one. The pinned Float input retains arrays;
	// NumberSimple forwards them, Math loops operands, Compare zero-pads missing leaves.
	const auto number = Imported("Node_Number_Simple", {Static("[true,false]")});
	Persisted(number, ArrayValue{ValueType::Scalar, {1., 0.}});
	const auto math = Imported("Node_Math", Math("[true,false]", "[1]", "[0.5]"));
	Persisted(math, ArrayValue{ValueType::Scalar, {1., .5}});
	const auto compare = Imported("Node_Compare", {Static("0"), Static("[true,false]"), Static("[1]")});
	Persisted(compare, ArrayValue{ValueType::Boolean, {true, true}});
	for (const auto *imported : {&number, &math, &compare}) {
		CHECK(imported->CatalogueNodes == 1);
		CHECK_FALSE(imported->Graph.Keyframes.empty());
		for (const auto &key : imported->Graph.Keyframes) {
			CHECK(key.Tick == 0);
			CHECK(key.Kind == KeyframeKind::Normal);
			CHECK(key.Interpolation == "source");
			CHECK_FALSE(key.SourceDriver);
		}
		CHECK(imported->Diagnostics.empty());
		std::vector<std::byte> bytes;
		std::string failure;
		REQUIRE(engine::bake::WritePxcx(imported->Source, bytes, failure));
		CHECK(bytes == imported->Source.OriginalBytes);
	}
}
