#include "../src/ImageGraphChoices.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <nodegraph/Graph.hpp>
#include <nodegraph/Registry.hpp>
#include <studio/ImageGraph.hpp>

TEST_SUITE_ID("studio.imagegraph_choices")

using namespace engine::imagegraph;

TEST_CASE(
	"Inspector source choices retain raw separator indices and finite fractional values",
	"[studio][imagegraph][source_choice]"
) {
	const auto *input = studio::imagegraph_choices::Input("pc.blend", "blend_mode");
	REQUIRE(input);
	const studio::imagegraph_choices::View view{*input};
	REQUIRE(view.Count() == 31);
	CHECK(view.At(2)->Separator);
	CHECK_FALSE(view.Select(2));
	const auto multiply = view.Select(3);
	REQUIRE(multiply);
	CHECK(std::get<EnumValue>(*multiply).Value == 3);
	CHECK(view.Selected(*multiply)->Label == "Multiply");
	CHECK_FALSE(view.Selected(Value{.5}));
	REQUIRE(view.Fraction(.5));
	CHECK(std::get<double>(*view.Fraction(.5)) == .5);
	CHECK_FALSE(view.Fraction(std::numeric_limits<double>::infinity()));
	CHECK_FALSE(view.Fraction(std::numeric_limits<double>::quiet_NaN()));
	CHECK_FALSE(view.Select(view.Count()));
	CatalogueInput unknown = *input;
	unknown.SourceChoices->Status = SourceChoicesStatus::Unknown;
	CHECK(studio::imagegraph_choices::View{unknown}.Count() == 0);
	CatalogueInput legacy;
	legacy.Type = ValueType::Enum;
	legacy.Choices = "First;Second";
	const studio::imagegraph_choices::View old{legacy};
	CHECK(old.Count() == 2);
	CHECK(old.At(1)->SourceIndex == 1);
	CHECK(old.At(1)->Label == "Second");
	CHECK_FALSE(old.Fraction(.5));
}

TEST_CASE(
	"Inspector edits source fractional enums without widening legacy enum properties",
	"[studio][imagegraph][source_choice]"
) {
	Document document;
	document.FormatVersion = 7;
	document.Nodes = {{"palette", "pc.gradient_palette", "", {}, {}, {}}};
	Diagnostic diagnostic;
	REQUIRE(studio::SetImageGraphValue(document, "palette", "interpolation", .5, diagnostic));
	CHECK(std::get<double>(document.Nodes.front().Values.front().Data) == .5);
	REQUIRE(
		studio::SetImageGraphValue(
			document,
			"palette",
			"palette",
			ArrayValue{ValueType::Colour, {Colour{0, 0, 0, 255}, Colour{255, 255, 255, 255}}},
			diagnostic
		)
	);
	document.Outputs = {{"out", "palette", "gradient"}};
	Document restored;
	REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
	CHECK(restored == document);
	Plan plan;
	REQUIRE(Compile(restored, plan, diagnostic) == Status::Ok);
	EvaluatedValue result;
	REQUIRE(EvaluateValue(restored, plan, "out", {}, result, diagnostic) == Status::Ok);
	const auto &gradient = std::get<Gradient>(result.Data);
	CHECK(gradient.Mode == 0);
	REQUIRE(gradient.Keys.size() == 2);
	CHECK(gradient.Keys[0].Time == 0);
	CHECK(gradient.Keys[1].Time == .5);
	document.Nodes.push_back({"legacy", "image.transform_3d", "", {}, {}, {}});
	CHECK_FALSE(studio::SetImageGraphValue(document, "legacy", "projection", .5, diagnostic));
	CHECK(diagnostic.Code == Status::TypeMismatch);
	CHECK(document.Nodes.back().Values.empty());
	CHECK_FALSE(
		studio::SetImageGraphValue(
			document, "palette", "interpolation", std::numeric_limits<double>::infinity(), diagnostic
		)
	);
	CHECK(std::get<double>(document.Nodes.front().Values.front().Data) == .5);
}

TEST_CASE(
	"Source strict suggestion metadata affects discovery and preserves manual enum links",
	"[studio][imagegraph][source_choice]"
) {
	studio::RegisterImageGraphNodeTypes();
	const auto *input = studio::imagegraph_choices::Input("pc.gradient_palette", "interpolation");
	REQUIRE(input);
	CHECK_FALSE(studio::imagegraph_choices::Suggested(input));
	CHECK(studio::imagegraph_choices::Suggested(nullptr));
	const auto *target = nodegraph::NodeTypes::Find("pc.gradient_palette");
	REQUIRE(target);
	bool enumPort = false;
	for (const auto &port : target->Inputs) {
		if (port.Name == "interpolation") {
			enumPort = true;
			CHECK_FALSE(port.Suggest);
		}
	}
	CHECK(enumPort);
	nodegraph::NodeType source;
	source.Id = "imagegraph.choice.fixture";
	source.Outputs = {{"choice", "imagegraph.enum"}};
	nodegraph::NodeTypes::Register(source);
	nodegraph::Graph graph;
	const auto from = graph.Add(source.Id, 0, 0);
	const auto to = graph.Add("pc.gradient_palette", 100, 0);
	REQUIRE(from != nodegraph::NO_NODE);
	REQUIRE(to != nodegraph::NO_NODE);
	CHECK(graph.Connect(from, "choice", to, "interpolation") == nodegraph::LinkResult::Made);
}

TEST_CASE(
	"Inspector source array edits use exact source leaf and shape validation",
	"[studio][imagegraph][source_choice]"
) {
	Document document;
	document.FormatVersion = 7;
	document.Nodes = {{"sample", "pc.gradient_sample", "", {}, {}, {}}};
	Diagnostic diagnostic;
	ArrayValue ratios{ValueType::Scalar, {.25, .75}};
	REQUIRE(studio::SetImageGraphValue(document, "sample", "ratio", ratios, diagnostic));
	CHECK(std::get<ArrayValue>(document.Nodes.front().Values.front().Data) == ratios);
	CHECK(studio::imagegraph_choices::Input("pc.gradient_sample", "ratio")->Type == ValueType::Scalar);
	REQUIRE(studio::SetImageGraphValue(document, "sample", "type", EnumValue{1}, diagnostic));
	REQUIRE(
		studio::SetImageGraphValue(
			document,
			"sample",
			"gradient",
			Gradient{0, {{0, Colour{0, 0, 0, 255}}, {1, Colour{255, 255, 255, 255}}}},
			diagnostic
		)
	);
	document.Outputs = {{"out", "sample", "colors"}};
	Document restored;
	REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
	CHECK(restored == document);
	Plan plan;
	REQUIRE(Compile(restored, plan, diagnostic) == Status::Ok);
	EvaluatedValue result;
	REQUIRE(EvaluateValue(restored, plan, "out", {}, result, diagnostic) == Status::Ok);
	const auto &colours = std::get<ArrayValue>(result.Data);
	REQUIRE(colours.Elements.size() == 2);
	CHECK(std::get<Colour>(colours.Elements[0]) == Colour{64, 64, 64, 255});
	CHECK(std::get<Colour>(colours.Elements[1]) == Colour{191, 191, 191, 255});
	const Document before = document;
	CHECK_FALSE(
		studio::SetImageGraphValue(
			document, "sample", "ratio", ArrayValue{ValueType::Text, {std::string("wrong")}}, diagnostic
		)
	);
	CHECK(document == before);
	ArrayValue nested{ValueType::Scalar, {}};
	nested.Nested = {{.25, .75}};
	CHECK_FALSE(studio::SetImageGraphValue(document, "sample", "ratio", nested, diagnostic));
	CHECK(document == before);
	ratios.Elements.resize(Limits::MaximumArrayElements + 1, .5);
	CHECK_FALSE(studio::SetImageGraphValue(document, "sample", "ratio", ratios, diagnostic));
	CHECK(document == before);
	document.Nodes.push_back({"legacy", "value.number", "", {}, {}, {}});
	CHECK_FALSE(
		studio::SetImageGraphValue(
			document, "legacy", "value", ArrayValue{ValueType::Scalar, {.25, .75}}, diagnostic
		)
	);
	CHECK(document.Nodes.back().Values.empty());
}
