#include <engine/imagegraph/Document.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <variant>

TEST_SUITE_ID("engine.imagegraph.value_graph")

namespace graph = engine::imagegraph;

namespace {
	graph::Status
	Run(graph::Document &document,
		std::string output,
		graph::EvaluatedValue &value,
		graph::Diagnostic &diagnostic,
		uint64_t tick = 0) {
		graph::Plan plan;
		const graph::Status status = graph::Compile(document, plan, diagnostic);
		if (status != graph::Status::Ok) return status;
		return graph::EvaluateValue(document, plan, output, {tick, 0}, value, diagnostic);
	}
}

TEST_CASE("Typed math links resolve the selected source port", "[imagegraph]") {
	graph::Document document;
	document.Nodes = {
		{"first", "value.number", "", {}, {{"value", 7.0}}},
		{"second", "value.number", "", {}, {{"value", 3.0}}},
		{"math", "value.math", "", {}, {{"mode", int64_t{1}}}},
	};
	document.Links = {{"first", "number", "math", "a"}, {"second", "number", "math", "b"}};
	document.Outputs = {{"difference", "math", "result"}};
	graph::EvaluatedValue value;
	graph::Diagnostic diagnostic;
	REQUIRE(Run(document, "difference", value, diagnostic) == graph::Status::Ok);
	CHECK(value.Port == "result");
	CHECK(std::get<double>(value.Data) == 4.0);
	graph::Plan plan;
	REQUIRE(graph::Compile(document, plan, diagnostic) == graph::Status::Ok);
	graph::Image image;
	CHECK(graph::Evaluate(document, plan, "difference", image, diagnostic) == graph::Status::InvalidOutput);
	CHECK(diagnostic.NodeId == "math");
	CHECK(diagnostic.Port == "result");
	const std::string saved = graph::Write(document);
	graph::Document restored;
	REQUIRE(graph::Read(saved, restored, diagnostic) == graph::Status::Ok);
	CHECK(restored == document);
}

TEST_CASE("Colour data exposes distinct named outputs", "[imagegraph]") {
	graph::Document document;
	document.Nodes = {
		{"rgb",
		 "value.color_rgb",
		 "",
		 {},
		 {{"red", 1.0}, {"green", 0.0}, {"blue", 0.0}, {"alpha", 0.5}, {"normalized", true}}},
		{"data", "value.color_data", "", {}, {}},
	};
	document.Links = {{"rgb", "colour", "data", "colour"}};
	document.Outputs = {{"red", "data", "red"}, {"alpha", "data", "alpha"}};
	graph::EvaluatedValue value;
	graph::Diagnostic diagnostic;
	REQUIRE(Run(document, "red", value, diagnostic) == graph::Status::Ok);
	CHECK(std::get<double>(value.Data) == 1.0);
	REQUIRE(Run(document, "alpha", value, diagnostic) == graph::Status::Ok);
	CHECK(std::get<double>(value.Data) == Catch::Approx(128.0 / 255.0));
}

TEST_CASE("Typed text array stays separate from image array execution", "[imagegraph]") {
	graph::Document document;
	document.Nodes = {
		{"split",
		 "value.text_split",
		 "",
		 {},
		 {{"text", std::string{"a,b,"}}, {"delimiter", std::string{","}}}}
	};
	document.Outputs = {{"parts", "split", "array"}};
	graph::EvaluatedValue value;
	graph::Diagnostic diagnostic;
	REQUIRE(Run(document, "parts", value, diagnostic) == graph::Status::Ok);
	const graph::ArrayValue &parts = std::get<graph::ArrayValue>(value.Data);
	REQUIRE(parts.Elements.size() == 3);
	CHECK(std::get<std::string>(parts.Elements[0]) == "a");
	CHECK(std::get<std::string>(parts.Elements[1]) == "b");
	CHECK(std::get<std::string>(parts.Elements[2]) == "");
	graph::Plan plan;
	REQUIRE(graph::Compile(document, plan, diagnostic) == graph::Status::Ok);
	graph::ImageArray images;
	CHECK(
		graph::EvaluateArray(document, plan, "parts", {}, images, diagnostic) == graph::Status::InvalidOutput
	);
}

TEST_CASE("Typed output follows authored keyframes and rejects invalid math", "[imagegraph]") {
	graph::Document document;
	document.Nodes = {{"number", "value.number", "", {}, {{"value", 0.0}}}};
	document.Outputs = {{"out", "number", "number"}};
	document.Keyframes = {
		{"number", "value", 0, 0.0, "linear"},
		{"number", "value", 10, 10.0, "step"},
	};
	graph::EvaluatedValue value;
	graph::Diagnostic diagnostic;
	REQUIRE(Run(document, "out", value, diagnostic, 5) == graph::Status::Ok);
	CHECK(std::get<double>(value.Data) == 5.0);
	document.Keyframes.clear();
	document.Nodes = {
		{"math", "value.math", "", {}, {{"mode", int64_t{18}}, {"from", graph::Vector2{1.0, 1.0}}}}
	};
	document.Outputs = {{"out", "math", "result"}};
	CHECK(Run(document, "out", value, diagnostic) == graph::Status::UnsupportedExecution);
	CHECK(diagnostic.NodeId == "math");
	CHECK(diagnostic.Port == "mode");
}
