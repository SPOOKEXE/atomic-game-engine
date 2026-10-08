#include <engine/imagegraph/Document.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <variant>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.any_junction")

using namespace engine::imagegraph;

namespace {
	Document ImageRoute() {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {
			{"solid",
			 "image.solid",
			 "",
			 {},
			 {{"width", int64_t{2}}, {"height", int64_t{1}}, {"colour", Colour{12, 34, 56, 255}}}},
			{"copy", "image.passthrough", "", {}, {}}
		};
		document.Junctions = {{"route", "", ValueType::Any, std::nullopt}};
		document.Links = {{"solid", "image", "route", "value"}, {"route", "value", "copy", "image"}};
		document.Outputs = {{"texture", "copy", "image"}};
		return document;
	}
}

TEST_CASE("Any junction routes a native image to its consumer", "[imagegraph]") {
	const Document document = ImageRoute();
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	CHECK(plan.EffectiveLinks == std::vector<Link>{{"solid", "image", "copy", "image"}});
	Image image;
	REQUIRE(Evaluate(document, plan, "texture", image, diagnostic) == Status::Ok);
	CHECK(image.Width == 2);
	CHECK(image.Height == 1);
	CHECK(image.Pixels == std::vector<uint8_t>{12, 34, 56, 255, 12, 34, 56, 255});
}

TEST_CASE("Any junction chains preserve native image fanout", "[imagegraph]") {
	Document document = ImageRoute();
	for (const bool reverseLinks : {false, true}) {
		document.Junctions.push_back({"second", "", ValueType::Any, std::nullopt});
		document.Nodes.push_back({"other", "image.passthrough", "", {}, {}});
		document.Links = {
			{"solid", "image", "route", "value"},
			{"route", "value", "second", "value"},
			{"second", "value", "copy", "image"},
			{"route", "value", "other", "image"}
		};
		if (reverseLinks) std::reverse(document.Links.begin(), document.Links.end());
		document.Outputs.push_back({"other", "other", "image"});
		Plan plan;
		Diagnostic diagnostic;
		REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
		REQUIRE(plan.EffectiveLinks.size() == 2);
		for (const Link &link : plan.EffectiveLinks) {
			CHECK(link.FromNode == "solid");
			CHECK(link.FromPort == "image");
			CHECK(link.ToPort == "image");
		}
		Image image, other;
		REQUIRE(Evaluate(document, plan, "texture", image, diagnostic) == Status::Ok);
		REQUIRE(Evaluate(document, plan, "other", other, diagnostic) == Status::Ok);
		CHECK(image.Pixels == std::vector<uint8_t>{12, 34, 56, 255, 12, 34, 56, 255});
		CHECK(other.Pixels == image.Pixels);
		document = ImageRoute();
	}
}

TEST_CASE("Any junction scalar default reaches a native number consumer", "[imagegraph]") {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {{"number", "value.number", "", {}, {}}};
	document.Junctions = {{"route", "", ValueType::Any, 2.5}};
	document.Links = {{"route", "value", "number", "value"}};
	document.Outputs = {{"number", "number", "number"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	CHECK(plan.ResolvedInputs == std::vector<ResolvedInput>{{"number", "value", 2.5}});
	EvaluatedValue number;
	REQUIRE(EvaluateValue(document, plan, "number", {}, number, diagnostic) == Status::Ok);
	REQUIRE(std::holds_alternative<double>(number.Data));
	CHECK(std::get<double>(number.Data) == 2.5);
}

TEST_CASE("Any junction does not satisfy a node reference route", "[imagegraph]") {
	Document document;
	document.FormatVersion = 9;
	document.Junctions = {
		{"route", "", ValueType::Any, 2.5}, {"reference", "", ValueType::NodeRef, std::nullopt}
	};
	document.Links = {{"route", "value", "reference", "value"}};
	Plan plan;
	Diagnostic diagnostic;
	CHECK(Compile(document, plan, diagnostic) == Status::TypeMismatch);
	CHECK(diagnostic.NodeId == "reference");
	CHECK(diagnostic.Port == "value");
}

TEST_CASE("Any junction does not bypass the noise sampler field union", "[imagegraph]") {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {{"sample", "value.sample_noise", "", {}, {}}};
	document.Junctions = {{"route", "", ValueType::Any, 2.5}};
	document.Links = {{"route", "value", "sample", "field"}};
	document.Outputs = {{"sample", "sample", "value"}};
	Plan plan;
	Diagnostic diagnostic;
	CHECK(Compile(document, plan, diagnostic) == Status::TypeMismatch);
	CHECK(diagnostic.NodeId == "sample");
	CHECK(diagnostic.Port == "field");
}

TEST_CASE("Any junction does not bypass exact computed noise coordinates", "[imagegraph]") {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"noise",
		 "value.noise_field",
		 "",
		 {},
		 {{"mode", EnumValue{1}}, {"dimension", EnumValue{3}}, {"output_type", EnumValue{1}}}}
	};
	document.Junctions = {{"route", "", ValueType::Any, Vector3{.25, .5, .75}}};
	document.Links = {{"route", "value", "noise", "position"}};
	document.Outputs = {{"noise", "noise", "value"}};
	Plan plan;
	Diagnostic diagnostic;
	CHECK(Compile(document, plan, diagnostic) == Status::TypeMismatch);
	CHECK(diagnostic.NodeId == "noise");
	CHECK(diagnostic.Port == "position");
}
