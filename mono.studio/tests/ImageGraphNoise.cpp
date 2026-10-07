#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/NoiseField.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <nodegraph/Graph.hpp>
#include <nodegraph/Registry.hpp>
#include <string_view>
#include <studio/ImageGraph.hpp>
#include <utility>
#include <vector>

TEST_SUITE_ID("studio.imagegraph_noise")
TEST_DEPENDS("engine.imagegraph.noise_field")

TEST_CASE("Noise dimension edits preserve field IDs and canvas union connections", "[studio][noise_field]") {
	using namespace engine::imagegraph;
	Document document;
	document.Nodes = {
		{"field", "value.noise_field", "", {}, {{"dimension", EnumValue{2}}}},
		{"sample", "value.sample_noise", "", {}, {{"position", Vector2{}}}},
		{"number", "pc.number", "", {}, {}}
	};
	document.Links = {{"field", "field", "sample", "field"}};
	document.Outputs = {{"out", "sample", "value"}};
	nodegraph::Graph graph;
	studio::ImageGraphCanvasIds ids;
	std::string error;
	Diagnostic diagnostic;
	for (int64_t dimension : {1, 2, 3}) {
		REQUIRE(studio::SetImageGraphValue(document, "field", "dimension", EnumValue{dimension}, diagnostic));
		Value position = dimension == 1	  ? Value{.25}
						 : dimension == 2 ? Value{Vector2{.25, .5}}
										  : Value{Vector3{.25, .5, .75}};
		REQUIRE(studio::SetImageGraphValue(document, "sample", "position", position, diagnostic));
		REQUIRE(studio::LoadImageGraphCanvas(document, graph, ids, error));
		CHECK(graph.LinkInto(ids.ToCanvas.at("sample"), "field") != nullptr);
		CHECK(
			graph.CanConnect(ids.ToCanvas.at("number"), "number", ids.ToCanvas.at("sample"), "position") ==
			nodegraph::LinkResult::Made
		);
		CHECK(
			graph.CanConnect(ids.ToCanvas.at("number"), "number", ids.ToCanvas.at("sample"), "field") ==
			nodegraph::LinkResult::TypeMismatch
		);
		Document saved;
		REQUIRE(studio::SaveImageGraphCanvas(graph, document, ids, saved, error));
		CHECK(saved == document);
		Document loaded;
		REQUIRE(Read(Write(saved), loaded, diagnostic) == Status::Ok);
		Plan plan;
		REQUIRE(Compile(loaded, plan, diagnostic) == Status::Ok);
		EvaluatedValue result;
		REQUIRE(EvaluateValue(loaded, plan, "out", {}, result, diagnostic) == Status::Ok);
		CHECK(std::holds_alternative<double>(result.Data));
	}
	CHECK_FALSE(studio::SetImageGraphValue(document, "field", "dimension", EnumValue{0}, diagnostic));
	CHECK_FALSE(studio::SetImageGraphValue(document, "sample", "position", std::string("bad"), diagnostic));
}

TEST_CASE(
	"Noise canvas defaults keep generator selectors at their documented choices", "[studio][noise_field]"
) {
	for (const auto &[property, expected] : std::array<std::pair<std::string_view, int64_t>, 3>{
			 {std::pair<std::string_view, int64_t>{"mode", 0}, {"dimension", 2}, {"output_type", 1}}
		 }) {
		auto value = studio::ImageGraphPropertyDefault("value.noise_field", property);
		REQUIRE(value);
		REQUIRE(std::holds_alternative<engine::imagegraph::EnumValue>(*value));
		CHECK(std::get<engine::imagegraph::EnumValue>(*value).Value == expected);
	}
}

TEST_CASE("Noise node starter values evaluate as a scalar two dimensional field", "[studio][noise_field]") {
	using namespace engine::imagegraph;
	Document document;
	document.FormatVersion = 9;
	Node node{"noise", "value.noise_field"};
	const NodeSchema *schema = FindSchema(node.Type);
	REQUIRE(schema);
	for (const PropertySchema &property : schema->Properties) {
		if (auto value = studio::ImageGraphPropertyDefault(node.Type, property.Id))
			node.Values.push_back({std::string(property.Id), std::move(*value)});
	}
	document.Nodes.push_back(std::move(node));
	document.Outputs = {{"field", "noise", "field"}};

	Diagnostic diagnostic;
	Plan plan;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluatedValue result;
	REQUIRE(EvaluateValue(document, plan, "field", {}, result, diagnostic) == Status::Ok);
	REQUIRE(std::holds_alternative<NoiseFieldValue>(result.Data));
	const auto &field = std::get<NoiseFieldValue>(result.Data);
	REQUIRE(ValidNoiseField(field));
	CHECK(field.Data->Dimensions == 2);
	CHECK(field.Data->Components == 1);
}

namespace {
	using namespace engine::imagegraph;

	Value NoisePosition(int64_t dimension) {
		if (dimension == 1) return .25;
		if (dimension == 2) return Vector2{.25, .5};
		return Vector3{.25, .5, .75};
	}

	Document NoiseCanvasDocument(int64_t dimension, int64_t components, bool computed = false) {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {
			{"field",
			 "value.noise_field",
			 "",
			 {0, 0},
			 {{"mode", EnumValue{computed ? 1 : 0}},
			  {"dimension", EnumValue{dimension}},
			  {"output_type", EnumValue{components}},
			  {"position", NoisePosition(dimension)}}},
			{"sample",
			 "value.sample_noise",
			 "",
			 {240, 0},
			 {{"output_type", EnumValue{components}}, {"position", NoisePosition(dimension)}}}
		};
		document.Links = {{"field", "field", "sample", "field"}};
		document.Outputs = {{"out", computed ? "field" : "sample", "value"}};
		return document;
	}

	bool LoadCanvas(const Document &document, nodegraph::Graph &graph, studio::ImageGraphCanvasIds &ids) {
		std::string error;
		return studio::LoadImageGraphCanvas(document, graph, ids, error);
	}

	bool SaveCanvas(
		const Document &basis,
		const nodegraph::Graph &graph,
		studio::ImageGraphCanvasIds &ids,
		Document &saved
	) {
		std::string error;
		return studio::SaveImageGraphCanvas(graph, basis, ids, saved, error);
	}

	Status EvaluateNoise(const Document &document, EvaluatedValue &value, Diagnostic &diagnostic) {
		Plan plan;
		const auto compiled = Compile(document, plan, diagnostic);
		if (compiled != Status::Ok) return compiled;
		return EvaluateValue(document, plan, "out", {}, value, diagnostic);
	}
}

TEST_CASE(
	"Noise canvas roundtrips all nine field shapes through compile and evaluation", "[studio][noise_field]"
) {
	using namespace engine::imagegraph;
	for (int64_t components = 1; components <= 3; ++components) {
		for (int64_t dimension = 1; dimension <= 3; ++dimension) {
			DYNAMIC_SECTION("components " << components << ", dimension " << dimension) {
				Document document = NoiseCanvasDocument(dimension, components);
				Diagnostic diagnostic;
				REQUIRE(studio::SetImageGraphValue(document, "field", "mode", EnumValue{0}, diagnostic));
				REQUIRE(
					studio::SetImageGraphValue(
						document, "field", "dimension", EnumValue{dimension}, diagnostic
					)
				);
				REQUIRE(
					studio::SetImageGraphValue(
						document, "field", "output_type", EnumValue{components}, diagnostic
					)
				);
				REQUIRE(
					studio::SetImageGraphValue(
						document, "sample", "output_type", EnumValue{components}, diagnostic
					)
				);

				NoiseNodeChoices choices;
				std::string_view failed;
				REQUIRE(ResolveNoiseNodeChoices(document.Nodes.front(), choices, failed));
				const auto fieldPort = NoiseNodePort(document.Nodes.front(), "field", PortDirection::Output);
				REQUIRE(fieldPort);
				CHECK(fieldPort->Type == NoiseFieldType(uint8_t(dimension), uint8_t(components)));

				nodegraph::Graph graph;
				studio::ImageGraphCanvasIds ids;
				REQUIRE(LoadCanvas(document, graph, ids));
				CHECK(
					graph.CanConnect(ids.ToCanvas.at("field"), "field", ids.ToCanvas.at("sample"), "field") ==
					nodegraph::LinkResult::Made
				);
				REQUIRE(graph.LinkInto(ids.ToCanvas.at("sample"), "field") != nullptr);

				Document saved;
				REQUIRE(SaveCanvas(document, graph, ids, saved));
				CHECK(saved.Links == document.Links);
				Document restored;
				REQUIRE(Read(Write(saved), restored, diagnostic) == Status::Ok);
				EvaluatedValue result;
				REQUIRE(EvaluateNoise(restored, result, diagnostic) == Status::Ok);
				if (components == 1) CHECK(std::holds_alternative<double>(result.Data));
				if (components == 2) CHECK(std::holds_alternative<Vector2>(result.Data));
				if (components == 3) CHECK(std::holds_alternative<Vector3>(result.Data));
			}
		}
	}
}

TEST_CASE("Noise output selector changes retain incompatible links until restored", "[studio][noise_field]") {
	using namespace engine::imagegraph;
	Document document = NoiseCanvasDocument(2, 1);
	Diagnostic diagnostic;
	REQUIRE(studio::SetImageGraphValue(document, "field", "output_type", EnumValue{2}, diagnostic));

	nodegraph::Graph graph;
	studio::ImageGraphCanvasIds ids;
	REQUIRE(LoadCanvas(document, graph, ids));
	CHECK(ids.UnmappedLinks == document.Links);
	Document saved;
	REQUIRE(SaveCanvas(document, graph, ids, saved));
	CHECK(saved.Links == document.Links);
	Plan plan;
	CHECK(Compile(saved, plan, diagnostic) == Status::TypeMismatch);
	CHECK(diagnostic.NodeId == "sample");
	CHECK(diagnostic.Port == "field");

	REQUIRE(studio::SetImageGraphValue(document, "field", "output_type", EnumValue{1}, diagnostic));
	nodegraph::Graph restoredGraph;
	studio::ImageGraphCanvasIds restoredIds;
	REQUIRE(LoadCanvas(document, restoredGraph, restoredIds));
	CHECK(restoredIds.UnmappedLinks.empty());
	CHECK(restoredGraph.LinkInto(restoredIds.ToCanvas.at("sample"), "field") != nullptr);
	REQUIRE(SaveCanvas(document, restoredGraph, restoredIds, saved));
	CHECK(saved.Links == document.Links);
	EvaluatedValue result;
	REQUIRE(EvaluateNoise(saved, result, diagnostic) == Status::Ok);
	CHECK(std::holds_alternative<double>(result.Data));
}

TEST_CASE("Computed noise canvas uses selected coordinate and result dimensions", "[studio][noise_field]") {
	using namespace engine::imagegraph;
	for (int64_t dimension = 1; dimension <= 3; ++dimension) {
		for (int64_t components = 1; components <= 3; ++components) {
			DYNAMIC_SECTION("coordinate " << dimension << ", components " << components) {
				Document document = NoiseCanvasDocument(dimension, components, true);
				document.Links.clear();
				document.Nodes.resize(1);
				Diagnostic diagnostic;
				REQUIRE(studio::SetImageGraphValue(document, "field", "mode", EnumValue{1}, diagnostic));
				REQUIRE(
					studio::SetImageGraphValue(
						document, "field", "dimension", EnumValue{dimension}, diagnostic
					)
				);
				REQUIRE(
					studio::SetImageGraphValue(
						document, "field", "output_type", EnumValue{components}, diagnostic
					)
				);
				const auto expectedPosition = dimension == 1   ? ValueType::Scalar
											  : dimension == 2 ? ValueType::Vector2
															   : ValueType::Vector3;
				const auto positionPort =
					NoiseNodePort(document.Nodes.front(), "position", PortDirection::Input);
				REQUIRE(positionPort);
				CHECK(positionPort->Type == expectedPosition);
				const std::string coordinateType = dimension == 1	? "pc.number"
												   : dimension == 2 ? "pc.vector2"
																	: "pc.vector3";
				const std::string coordinatePort = dimension == 1 ? "number" : "vector";
				std::vector<AuthoredValue> coordinateValues;
				if (dimension == 1)
					coordinateValues = {{"value", .25}};
				else if (dimension == 2)
					coordinateValues = {{"x", .25}, {"y", .5}};
				else
					coordinateValues = {{"x", .25}, {"y", .5}, {"z", .75}};
				document.Nodes.push_back({"coordinate", coordinateType, "", {}, std::move(coordinateValues)});
				document.Links = {{"coordinate", coordinatePort, "field", "position"}};
				document.Outputs = {{"out", "field", "value"}};
				nodegraph::Graph graph;
				studio::ImageGraphCanvasIds ids;
				REQUIRE(LoadCanvas(document, graph, ids));
				CHECK(
					graph.CanConnect(
						ids.ToCanvas.at("coordinate"), coordinatePort, ids.ToCanvas.at("field"), "position"
					) == nodegraph::LinkResult::Made
				);
				Document saved;
				REQUIRE(SaveCanvas(document, graph, ids, saved));
				Document restored;
				REQUIRE(Read(Write(saved), restored, diagnostic) == Status::Ok);
				EvaluatedValue result;
				REQUIRE(EvaluateNoise(restored, result, diagnostic) == Status::Ok);
				if (components == 1) CHECK(std::holds_alternative<double>(result.Data));
				if (components == 2) CHECK(std::holds_alternative<Vector2>(result.Data));
				if (components == 3) CHECK(std::holds_alternative<Vector3>(result.Data));
			}
		}
	}
}

TEST_CASE("Switching noise to computed keeps the field link in the canvas archive", "[studio][noise_field]") {
	using namespace engine::imagegraph;
	Document document = NoiseCanvasDocument(2, 1);
	Diagnostic diagnostic;
	REQUIRE(studio::SetImageGraphValue(document, "field", "mode", EnumValue{1}, diagnostic));
	nodegraph::Graph graph;
	studio::ImageGraphCanvasIds ids;
	REQUIRE(LoadCanvas(document, graph, ids));
	CHECK(ids.UnmappedLinks == document.Links);
	const auto fieldSchema = NoiseNodePort(document.Nodes.front(), "position", PortDirection::Input);
	REQUIRE(fieldSchema);
	CHECK(fieldSchema->Type == ValueType::Vector2);
	CHECK_FALSE(NoiseNodePort(document.Nodes.front(), "field", PortDirection::Output));
	const auto valueSchema = NoiseNodePort(document.Nodes.front(), "value", PortDirection::Output);
	REQUIRE(valueSchema);
	CHECK(valueSchema->Type == ValueType::Scalar);
	const auto canvasField = std::find_if(graph.Nodes().begin(), graph.Nodes().end(), [&](const auto &node) {
		return node.Id == ids.ToCanvas.at("field");
	});
	REQUIRE(canvasField != graph.Nodes().end());
	REQUIRE(canvasField->OutputPorts);
	CHECK(
		std::any_of(canvasField->OutputPorts->begin(), canvasField->OutputPorts->end(), [](const auto &port) {
			return port.Name == "value";
		})
	);
	CHECK_FALSE(
		std::any_of(canvasField->OutputPorts->begin(), canvasField->OutputPorts->end(), [](const auto &port) {
			return port.Name == "field";
		})
	);
	Document saved;
	REQUIRE(SaveCanvas(document, graph, ids, saved));
	CHECK(saved.Links == document.Links);
}

TEST_CASE(
	"Noise dimension edits preserve coordinate data and diagnose linked shape changes",
	"[studio][noise_field]"
) {
	using namespace engine::imagegraph;
	Document document = NoiseCanvasDocument(2, 1, true);
	document.Nodes.push_back({"coordinate", "pc.vector2", "", {}, {{"x", .25}, {"y", .75}}});
	document.Links = {{"coordinate", "vector", "field", "position"}};
	document.Outputs = {{"out", "field", "value"}};
	Diagnostic diagnostic;

	nodegraph::Graph graph;
	studio::ImageGraphCanvasIds ids;
	REQUIRE(LoadCanvas(document, graph, ids));
	REQUIRE(graph.LinkInto(ids.ToCanvas.at("field"), "position") != nullptr);
	REQUIRE(studio::SetImageGraphValue(document, "field", "dimension", EnumValue{3}, diagnostic));
	const auto field = std::find_if(document.Nodes.begin(), document.Nodes.end(), [](const Node &node) {
		return node.Id == "field";
	});
	REQUIRE(field != document.Nodes.end());
	const auto position =
		std::find_if(field->Values.begin(), field->Values.end(), [](const AuthoredValue &value) {
			return value.Port == "position";
		});
	REQUIRE(position != field->Values.end());
	CHECK((std::get<Vector3>(position->Data) == Vector3{.25, .5, 0}));

	nodegraph::Graph resizedGraph;
	studio::ImageGraphCanvasIds resizedIds;
	REQUIRE(LoadCanvas(document, resizedGraph, resizedIds));
	CHECK(resizedIds.UnmappedLinks == document.Links);
	Document saved;
	REQUIRE(SaveCanvas(document, resizedGraph, resizedIds, saved));
	CHECK(saved.Links == document.Links);
	Plan plan;
	CHECK(Compile(saved, plan, diagnostic) == Status::TypeMismatch);
	CHECK(diagnostic.NodeId == "field");
	CHECK(diagnostic.Port == "position");
}
