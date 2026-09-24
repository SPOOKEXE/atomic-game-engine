#include <engine/imagegraph/Document.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <variant>

TEST_SUITE_ID("engine.imagegraph.value_text_graph")

namespace graph = engine::imagegraph;

namespace {
	graph::Status
	Run(graph::Document &document,
		std::string output,
		graph::EvaluatedValue &value,
		graph::Diagnostic &diagnostic) {
		graph::Plan plan;
		const graph::Status compiled = graph::Compile(document, plan, diagnostic);
		if (compiled != graph::Status::Ok) return compiled;
		return graph::EvaluateValue(document, plan, output, {}, value, diagnostic);
	}
}

TEST_CASE("Text length counts UTF-8 characters and source word segments", "[imagegraph]") {
	graph::Document document;
	document.Nodes = {{"length", "value.text_length", "", {}, {{"text", std::string{"A猫🙂B"}}}}};
	document.Outputs = {{"out", "length", "length"}};
	graph::EvaluatedValue value;
	graph::Diagnostic diagnostic;
	REQUIRE(Run(document, "out", value, diagnostic) == graph::Status::Ok);
	CHECK(std::get<int64_t>(value.Data) == 4);
	document.Nodes[0].Values.push_back({"mode", int64_t{1}});
	document.Nodes[0].Values[0].Data = std::string{"a  b "};
	REQUIRE(Run(document, "out", value, diagnostic) == graph::Status::Ok);
	CHECK(std::get<int64_t>(value.Data) == 4);
	const std::string saved = graph::Write(document);
	graph::Document restored;
	REQUIRE(graph::Read(saved, restored, diagnostic) == graph::Status::Ok);
	CHECK(restored == document);
}

TEST_CASE("Linked text copy and delete preserve Unicode character positions", "[imagegraph]") {
	graph::Document document;
	document.Nodes = {
		{"source", "value.text", "", {}, {{"value", std::string{"A猫🙂B"}}}},
		{"copy", "value.text_get_char", "", {}, {{"index", int64_t{2}}, {"amount", int64_t{2}}}},
		{"delete", "value.text_delete", "", {}, {{"index", int64_t{0}}, {"amount", int64_t{1}}}},
	};
	document.Links = {{"source", "text", "copy", "text"}, {"copy", "text", "delete", "text"}};
	document.Outputs = {{"copied", "copy", "text"}, {"deleted", "delete", "text"}};
	graph::EvaluatedValue value;
	graph::Diagnostic diagnostic;
	REQUIRE(Run(document, "copied", value, diagnostic) == graph::Status::Ok);
	CHECK(std::get<std::string>(value.Data) == "猫🙂");
	REQUIRE(Run(document, "deleted", value, diagnostic) == graph::Status::Ok);
	CHECK(std::get<std::string>(value.Data) == "🙂");
}

TEST_CASE("Negative copy arguments report the source node and port", "[imagegraph]") {
	graph::Document document;
	document.Nodes = {
		{"copy", "value.text_get_char", "", {}, {{"text", std::string{"abc"}}, {"index", int64_t{-1}}}}
	};
	document.Outputs = {{"out", "copy", "text"}};
	graph::EvaluatedValue value;
	graph::Diagnostic diagnostic;
	CHECK(Run(document, "out", value, diagnostic) == graph::Status::UnsupportedExecution);
	CHECK(diagnostic.NodeId == "copy");
	CHECK(diagnostic.Port == "index");
}
