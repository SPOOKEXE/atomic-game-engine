#include "../src/PixelOps.hpp"
#include "../src/PixelOpsGenerate.hpp"
#include "../src/Timeline.hpp"

#include <engine/imagegraph/Document.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.document")

using engine::imagegraph::ArrayValue;
using engine::imagegraph::Colour;
using engine::imagegraph::Compile;
using engine::imagegraph::Diagnostic;
using engine::imagegraph::Document;
using engine::imagegraph::DynamicInput;
using engine::imagegraph::Evaluate;
using engine::imagegraph::EvaluateArray;
using engine::imagegraph::EvaluationRequest;
using engine::imagegraph::FindSchema;
using engine::imagegraph::Group;
using engine::imagegraph::ImageArray;
using engine::imagegraph::Junction;
using engine::imagegraph::Keyframe;
using engine::imagegraph::Limits;
using engine::imagegraph::Link;
using engine::imagegraph::Migrate;
using engine::imagegraph::Node;
using engine::imagegraph::Output;
using engine::imagegraph::Plan;
using engine::imagegraph::PortDirection;
using engine::imagegraph::Read;
using engine::imagegraph::Status;
using engine::imagegraph::TickRange;
using engine::imagegraph::ValidateTickRange;
using engine::imagegraph::ValueType;
using engine::imagegraph::Vector2;
using engine::imagegraph::Write;

namespace {
	Node Solid(std::string id, int64_t width = 2, int64_t height = 1, Colour colour = {12, 34, 56, 255}) {
		Node node;
		node.Id = std::move(id);
		node.Type = "image.solid";
		node.Position = {4.25, -8.5};
		node.Values = {{"width", width}, {"height", height}, {"colour", colour}};
		return node;
	}

	Document SolidDocument() {
		Document document;
		document.Nodes.push_back(Solid("node-a"));
		document.Outputs.push_back({"texture", "node-a", "image"});
		return document;
	}
}

TEST_CASE("authored imagegraph fields round trip with stable text", "[imagegraph]") {
	Document document = SolidDocument();
	document.Groups.push_back({"group-a", "Main\nGroup"});
	document.Nodes[0].GroupId = "group-a";
	document.Links.push_back({"node-a", "image", "node-b", "image"});
	document.Nodes.push_back({"node-b", "image.passthrough", "group-a", {9.0, 11.0}, {}});
	document.Nodes.push_back({"node-c", "vendor.unknown", "", {}, {{"opaque", std::string("keep me")}}});
	document.Outputs[0] = {"texture", "node-b", "image"};
	document.Keyframes.push_back({"node-a", "colour", 42, Colour{1, 2, 3, 4}, "linear"});
	document.Keyframes.push_back({"node-a", "offset", 43, Vector2{0.5, -1.25}, "step"});
	document.Keyframes.push_back({"node-a", "label", 44, std::string("cyan"), "step"});
	document.Keyframes.push_back({"node-a", "enabled", 45, true, "step"});
	document.Keyframes.push_back({"node-a", "seed", 46, int64_t{91}, "step"});
	document.Keyframes.push_back({"node-a", "gain", 47, 0.75, "linear"});

	const std::string text = Write(document);
	Document parsed;
	Diagnostic diagnostic;
	REQUIRE(Read(text, parsed, diagnostic) == Status::Ok);
	CHECK(parsed == document);
	CHECK(Write(parsed) == text);
}

TEST_CASE("legacy imagegraph document migrates without losing authored fields", "[imagegraph]") {
	Document document = SolidDocument();
	const std::string legacyText = Write(document);
	Document parsed;
	Diagnostic diagnostic;
	REQUIRE(Read(legacyText, parsed, diagnostic) == Status::Ok);
	REQUIRE(Migrate(parsed, diagnostic) == Status::Ok);
	CHECK(parsed.FormatVersion == 6);
	Document upgraded;
	REQUIRE(Read(Write(parsed), upgraded, diagnostic) == Status::Ok);
	CHECK(upgraded == parsed);
	CHECK(Write(parsed) != legacyText);
}

TEST_CASE("typed junctions route image dependencies through nested groups", "[imagegraph]") {
	Document document = SolidDocument();
	document.FormatVersion = 2;
	document.Groups = {{"outer", "Outer", ""}, {"inner", "Inner", "outer"}};
	document.Groups[0].Ports.push_back({"image", "outside", PortDirection::Output});
	document.Groups[1].Ports.push_back({"image", "inside", PortDirection::Output});
	document.Nodes[0].GroupId = "inner";
	document.Nodes.push_back({"copy", "image.passthrough", "", {}, {}});
	document.Junctions = {
		{"inside", "inner", ValueType::Image, std::nullopt},
		{"outside", "outer", ValueType::Image, std::nullopt},
	};
	document.Links = {
		{"node-a", "image", "inside", "value"},
		{"inside", "value", "outside", "value"},
		{"outside", "value", "copy", "image"},
	};
	document.Outputs[0].NodeId = "copy";
	Document parsed;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(document), parsed, diagnostic) == Status::Ok);
	CHECK(parsed == document);
	Plan plan;
	REQUIRE(Compile(parsed, plan, diagnostic) == Status::Ok);
	CHECK(plan.EffectiveLinks == std::vector<Link>{{"node-a", "image", "copy", "image"}});
	engine::imagegraph::Image image;
	REQUIRE(Evaluate(parsed, plan, "texture", image, diagnostic) == Status::Ok);
	CHECK(image.Pixels == std::vector<uint8_t>{12, 34, 56, 255, 12, 34, 56, 255});
}

TEST_CASE("v2 graph metadata checks typed routes and array budgets", "[imagegraph]") {
	Document document = SolidDocument();
	document.FormatVersion = 2;
	document.Nodes.push_back({"copy", "image.passthrough", "", {}, {}});
	document.Junctions.push_back({"route", "", ValueType::Scalar, 0.5});
	document.Links = {{"node-a", "image", "route", "value"}, {"route", "value", "copy", "image"}};
	document.Outputs[0].NodeId = "copy";
	Plan plan;
	Diagnostic diagnostic;
	CHECK(Compile(document, plan, diagnostic) == Status::TypeMismatch);
	CHECK(diagnostic.NodeId == "route");
	CHECK(diagnostic.Port == "value");
	document.Junctions[0].Type = ValueType::Image;
	document.Junctions[0].Default.reset();
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);

	document.Nodes[1].DynamicInputs.push_back({"extra", ValueType::Image, std::nullopt});
	CHECK(Compile(document, plan, diagnostic) == Status::InvalidValue);
	CHECK(diagnostic.NodeId == "copy");
	CHECK(diagnostic.Port == "extra");

	document.Nodes.pop_back();
	document.Links.clear();
	document.Junctions.clear();
	document.Outputs[0].NodeId = "node-a";
	Node arrayNode{"array", "value.array", "", {}, {}};
	arrayNode.DynamicInputs = {
		{"first", ValueType::Scalar, 0.25},
		{"second", ValueType::Scalar, 0.75},
	};
	document.Nodes.push_back(arrayNode);
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	document.Junctions.push_back({"constant", "", ValueType::Scalar, 0.5});
	document.Links.push_back({"constant", "value", "array", "first"});
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	CHECK(plan.ResolvedInputs == std::vector<engine::imagegraph::ResolvedInput>{{"array", "first", 0.5}});
	Document parsed;
	REQUIRE(Read(Write(document), parsed, diagnostic) == Status::Ok);
	CHECK(parsed == document);

	document.Nodes.back().DynamicInputs[1].Id = "first";
	CHECK(Compile(document, plan, diagnostic) == Status::DuplicateId);
	CHECK(diagnostic.NodeId == "array");
	CHECK(diagnostic.Port == "first");
	document.Nodes.back().DynamicInputs[1].Id = "second";
	document.Nodes.back().DynamicInputs.resize(Limits::MaximumDynamicInputsPerNode + 1);
	CHECK(Compile(document, plan, diagnostic) == Status::LimitExceeded);

	Document authored = SolidDocument();
	authored.FormatVersion = 2;
	authored.Nodes.push_back(
		{"future",
		 "vendor.future",
		 "",
		 {},
		 {{"values", ArrayValue{ValueType::Integer, {int64_t{1}, int64_t{2}}}}}}
	);
	REQUIRE(Read(Write(authored), parsed, diagnostic) == Status::Ok);
	CHECK(parsed == authored);

	Document oversized = SolidDocument();
	oversized.FormatVersion = 2;
	Node arrayNodeOverBudget{"array", "value.array", "", {}, {}};
	arrayNodeOverBudget.DynamicInputs.push_back(
		{"values",
		 ValueType::Array,
		 ArrayValue{
			 ValueType::Integer,
			 std::vector<engine::imagegraph::ElementValue>(Limits::MaximumArrayElements + 1, int64_t{1})
		 }}
	);
	oversized.Nodes.push_back(std::move(arrayNodeOverBudget));
	CHECK(Compile(oversized, plan, diagnostic) == Status::LimitExceeded);
	CHECK(diagnostic.NodeId == "array");
	CHECK(diagnostic.Port == "values");
}

TEST_CASE("subgraph interfaces reject invalid boundaries and hierarchy cycles", "[imagegraph]") {
	Document document = SolidDocument();
	document.FormatVersion = 2;
	document.Groups = {{"outer", "Outer", ""}, {"inner", "Inner", "outer"}};
	document.Nodes[0].GroupId = "inner";
	document.Nodes.push_back({"copy", "image.passthrough", "", {}, {}});
	document.Junctions.push_back({"inside", "inner", ValueType::Image, std::nullopt});
	document.Groups[1].Ports.push_back({"image", "inside", PortDirection::Output});
	document.Links = {{"node-a", "image", "inside", "value"}, {"inside", "value", "copy", "image"}};
	document.Outputs[0].NodeId = "copy";
	Plan plan;
	Diagnostic diagnostic;
	CHECK(Compile(document, plan, diagnostic) == Status::InvalidGroup);
	CHECK(diagnostic.NodeId == "copy");

	document.Groups[1].ParentId = "outer";
	document.Groups[0].ParentId = "inner";
	CHECK(Compile(document, plan, diagnostic) == Status::Cycle);
	CHECK(diagnostic.NodeId == "outer");

	document.Groups[0].ParentId.clear();
	document.Junctions.push_back(document.Junctions[0]);
	CHECK(Compile(document, plan, diagnostic) == Status::DuplicateId);
	CHECK(diagnostic.NodeId == "inside");
}

TEST_CASE("image arrays preserve order and select without truncation", "[imagegraph]") {
	Document document;
	document.FormatVersion = 2;
	document.Nodes.push_back(Solid("red", 1, 1, {255, 0, 0, 255}));
	document.Nodes.push_back(Solid("blue", 1, 1, {0, 0, 255, 255}));
	Node arrayNode{"items", "value.array", "", {}, {}};
	arrayNode.DynamicInputs = {
		{"first", ValueType::Image, std::nullopt},
		{"second", ValueType::Image, std::nullopt},
	};
	document.Nodes.push_back(arrayNode);
	document.Nodes.push_back({"pick", "value.array_get", "", {}, {{"index", int64_t{1}}}});
	document.Nodes.push_back({"flip", "image.flip", "", {}, {{"axis", int64_t{1}}}});
	document.Links = {
		{"red", "image", "items", "first"},
		{"blue", "image", "items", "second"},
		{"items", "array", "pick", "array"},
		{"pick", "image", "flip", "image"},
	};
	document.Outputs = {{"all", "items", "array"}, {"selected", "flip", "image"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	ImageArray images;
	REQUIRE(EvaluateArray(document, plan, "all", EvaluationRequest{0, 42}, images, diagnostic) == Status::Ok);
	REQUIRE(images.Images.size() == 2);
	CHECK(images.Images[0].Pixels == std::vector<uint8_t>{255, 0, 0, 255});
	CHECK(images.Images[1].Pixels == std::vector<uint8_t>{0, 0, 255, 255});
	engine::imagegraph::Image selected;
	CHECK(
		Evaluate(document, plan, "all", EvaluationRequest{0, 42}, selected, diagnostic) ==
		Status::InvalidOutput
	);
	CHECK(diagnostic.NodeId == "items");
	CHECK(diagnostic.Port == "array");
	REQUIRE(
		Evaluate(document, plan, "selected", EvaluationRequest{0, 42}, selected, diagnostic) == Status::Ok
	);
	CHECK(selected.Pixels == images.Images[1].Pixels);
	document.Nodes[3].Values[0].Data = int64_t{-1};
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	REQUIRE(
		Evaluate(document, plan, "selected", EvaluationRequest{0, 42}, selected, diagnostic) == Status::Ok
	);
	CHECK(selected.Pixels == images.Images[1].Pixels);
	document.Nodes[3].Values[0].Data = int64_t{2};
	document.Nodes[3].Values.push_back({"overflow", int64_t{1}});
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	REQUIRE(
		Evaluate(document, plan, "selected", EvaluationRequest{0, 42}, selected, diagnostic) == Status::Ok
	);
	CHECK(selected.Pixels == images.Images[0].Pixels);

	document.Nodes[2].DynamicInputs.pop_back();
	document.Links.erase(document.Links.begin() + 1);
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	REQUIRE(Evaluate(document, plan, "all", EvaluationRequest{0, 42}, selected, diagnostic) == Status::Ok);
	CHECK(selected.Pixels == images.Images[0].Pixels);
}

TEST_CASE("array execution reports unsupported element type by durable input", "[imagegraph]") {
	Document document = SolidDocument();
	document.FormatVersion = 2;
	Node arrayNode{"items", "value.array", "", {}, {}};
	arrayNode.DynamicInputs.push_back({"number", ValueType::Scalar, 0.5});
	document.Nodes.push_back(std::move(arrayNode));
	document.Outputs.push_back({"all", "items", "array"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	ImageArray images;
	CHECK(
		EvaluateArray(document, plan, "all", EvaluationRequest{}, images, diagnostic) ==
		Status::UnsupportedExecution
	);
	CHECK(diagnostic.NodeId == "items");
	CHECK(diagnostic.Port == "number");
}

TEST_CASE("spread preserves nested shape and single-image consumers reject nested leaves", "[imagegraph]") {
	Document document;
	document.FormatVersion = 2;
	document.Nodes.push_back(Solid("red", 1, 1, {255, 0, 0, 255}));
	Node inner{"inner", "value.array", "", {}, {}};
	inner.DynamicInputs.push_back({"leaf", ValueType::Image, std::nullopt});
	Node outer{"outer", "value.array", "", {}, {}};
	outer.DynamicInputs.push_back({"nested", ValueType::Array, std::nullopt});
	document.Nodes.push_back(inner);
	document.Nodes.push_back(outer);
	document.Links = {{"red", "image", "inner", "leaf"}, {"inner", "array", "outer", "nested"}};
	document.Outputs = {{"all", "outer", "array"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	ImageArray images;
	REQUIRE(EvaluateArray(document, plan, "all", EvaluationRequest{}, images, diagnostic) == Status::Ok);
	REQUIRE(images.Images.size() == 1);
	REQUIRE(images.Items.size() == 1);
	CHECK(std::holds_alternative<std::vector<engine::imagegraph::ImageArrayItem>>(images.Items[0].Data));
	engine::imagegraph::Image image;
	CHECK(Evaluate(document, plan, "all", image, diagnostic) == Status::InvalidOutput);
	CHECK(diagnostic.NodeId == "outer");
	CHECK(diagnostic.Port == "array");
	document.Nodes[2].Values.push_back({"spread", true});
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	REQUIRE(EvaluateArray(document, plan, "all", EvaluationRequest{}, images, diagnostic) == Status::Ok);
	REQUIRE(images.Items.size() == 1);
	CHECK(std::get<size_t>(images.Items[0].Data) == 0);
	REQUIRE(Evaluate(document, plan, "all", image, diagnostic) == Status::Ok);
	CHECK(image.Pixels == std::vector<uint8_t>{255, 0, 0, 255});
}

TEST_CASE("height blend processes unequal image arrays in every documented mode", "[imagegraph]") {
	Document document;
	document.FormatVersion = 2;
	document.Nodes = {
		Solid("a0", 1, 1, {20, 20, 20, 255}),
		Solid("a1", 1, 1, {80, 80, 80, 255}),
		Solid("b0", 1, 1, {120, 120, 120, 255}),
		Solid("b1", 1, 1, {180, 180, 180, 255}),
		Solid("b2", 1, 1, {240, 240, 240, 255}),
	};
	Node left{"left", "value.array", "", {}, {}};
	left.DynamicInputs = {{"i0", ValueType::Image, std::nullopt}, {"i1", ValueType::Image, std::nullopt}};
	Node right{"right", "value.array", "", {}, {}};
	right.DynamicInputs = {
		{"i0", ValueType::Image, std::nullopt},
		{"i1", ValueType::Image, std::nullopt},
		{"i2", ValueType::Image, std::nullopt}
	};
	document.Nodes.push_back(left);
	document.Nodes.push_back(right);
	document.Nodes.push_back({"blend", "image.height_blend", "", {}, {{"array_process", int64_t{0}}}});
	document.Links = {
		{"a0", "image", "left", "i0"},
		{"a1", "image", "left", "i1"},
		{"b0", "image", "right", "i0"},
		{"b1", "image", "right", "i1"},
		{"b2", "image", "right", "i2"},
		{"left", "array", "blend", "background"},
		{"right", "array", "blend", "foreground"},
	};
	document.Outputs = {{"all", "blend", "image"}};
	const std::vector<std::vector<std::array<size_t, 2>>> schedules = {
		{{0, 0}, {1, 1}, {0, 2}},
		{{0, 0}, {1, 1}, {1, 2}},
		{{0, 0}, {0, 1}, {0, 2}, {1, 0}, {1, 1}, {1, 2}},
		{{0, 0}, {1, 0}, {0, 1}, {1, 1}, {0, 2}, {1, 2}},
	};
	const std::array<uint8_t, 2> leftGrey{20, 80};
	const std::array<uint8_t, 3> rightGrey{120, 180, 240};
	for (size_t mode = 0; mode < schedules.size(); mode++) {
		document.Nodes.back().Values[0].Data = static_cast<int64_t>(mode);
		Plan plan;
		Diagnostic diagnostic;
		REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
		ImageArray images;
		REQUIRE(EvaluateArray(document, plan, "all", EvaluationRequest{}, images, diagnostic) == Status::Ok);
		REQUIRE(images.Items.size() == schedules[mode].size());
		for (size_t row = 0; row < schedules[mode].size(); row++) {
			const auto pair = schedules[mode][row];
			const uint8_t a = leftGrey[pair[0]], b = rightGrey[pair[1]];
			const engine::imagegraph::Image background{1, 1, {a, a, a, 255}, 0};
			const engine::imagegraph::Image foreground{1, 1, {b, b, b, 255}, 0};
			engine::imagegraph::Image expected{1, 1, std::vector<uint8_t>(4), 0};
			REQUIRE(engine::imagegraph::detail::BlendHeight(background, foreground, expected, 0, 1, 0.5));
			CHECK(images.Images[row].Pixels == expected.Pixels);
		}
	}
}

TEST_CASE("compiler rejects duplicate durable node identifiers", "[imagegraph]") {
	Document document = SolidDocument();
	document.Nodes.push_back(Solid("node-a"));
	Plan plan;
	Diagnostic diagnostic;

	CHECK(Compile(document, plan, diagnostic) == Status::DuplicateId);
	CHECK(diagnostic.NodeId == "node-a");
}

TEST_CASE("unknown nodes survive parsing and are refused during compilation", "[imagegraph]") {
	Document document = SolidDocument();
	document.Nodes.push_back({"future-node", "vendor.future", "", {}, {}});
	const std::string text = Write(document);
	Document parsed;
	Diagnostic diagnostic;
	REQUIRE(Read(text, parsed, diagnostic) == Status::Ok);
	CHECK(parsed == document);

	Plan plan;
	CHECK(Compile(parsed, plan, diagnostic) == Status::UnknownNode);
	CHECK(diagnostic.NodeId == "future-node");
}

TEST_CASE("compiler validates property types and typed link ports", "[imagegraph]") {
	Document document = SolidDocument();
	document.Nodes[0].Values[0].Data = std::string("wide");
	Plan plan;
	Diagnostic diagnostic;
	CHECK(Compile(document, plan, diagnostic) == Status::TypeMismatch);

	document = SolidDocument();
	document.Nodes.push_back({"pass", "image.passthrough", "", {}, {}});
	document.Links.push_back({"node-a", "missing", "pass", "image"});
	CHECK(Compile(document, plan, diagnostic) == Status::UnknownPort);
}

TEST_CASE("registered node schemas expose the editor socket and property contract", "[imagegraph]") {
	const engine::imagegraph::NodeSchema *solid = FindSchema("image.solid");
	const engine::imagegraph::NodeSchema *passthrough = FindSchema("image.passthrough");
	REQUIRE(solid != nullptr);
	REQUIRE(passthrough != nullptr);
	CHECK(solid->Properties.size() == 6);
	CHECK(solid->Ports.size() == 3);
	CHECK(solid->Ports[0].Id == "foreground");
	CHECK(solid->Ports[1].Id == "mask");
	CHECK(solid->Ports[2].Id == "image");
	CHECK(solid->Ports[2].Direction == PortDirection::Output);
	const auto *heightBlend = FindSchema("image.height_blend");
	REQUIRE(heightBlend != nullptr);
	CHECK(heightBlend->Ports.size() == 3);
	CHECK(heightBlend->Properties.size() == 4);
	const auto *blend = FindSchema("image.blend");
	REQUIRE(blend != nullptr);
	CHECK(blend->Ports.size() == 4);
	CHECK(blend->Properties.size() == 13);
	CHECK(passthrough->Ports.size() == 2);
	CHECK(passthrough->Ports[0].Direction == PortDirection::Input);
	CHECK(passthrough->Ports[1].Direction == PortDirection::Output);
	for (const char *type : {"image.flip", "image.invert", "image.alpha_cutoff"}) {
		const auto *filter = FindSchema(type);
		REQUIRE(filter != nullptr);
		CHECK(filter->Ports.size() == 3);
		CHECK(filter->Ports[0].Id == "image");
		CHECK(filter->Ports[0].Direction == PortDirection::Input);
		CHECK(filter->Ports[1].Id == "mask");
		CHECK(filter->Ports[1].Direction == PortDirection::Input);
		CHECK(filter->Ports.back().Direction == PortDirection::Output);
	}
	CHECK(FindSchema("image.flip")->Properties.size() == 5);
	CHECK(FindSchema("image.invert")->Properties.size() == 5);
	CHECK(FindSchema("image.alpha_cutoff")->Properties.size() == 4);
	CHECK(FindSchema("vendor.unknown") == nullptr);
}

TEST_CASE("CPU passthrough preserves upstream dimensions, pixels and hash", "[imagegraph]") {
	Document document = SolidDocument();
	document.Nodes.push_back({"copy", "image.passthrough", "", {12.0, 0.0}, {}});
	document.Links.push_back({"node-a", "image", "copy", "image"});
	document.Outputs[0] = {"texture", "copy", "image"};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);

	engine::imagegraph::Image image;
	REQUIRE(Evaluate(document, plan, "texture", image, diagnostic) == Status::Ok);
	CHECK(image.Width == 2);
	CHECK(image.Height == 1);
	CHECK(image.Pixels == std::vector<uint8_t>{12, 34, 56, 255, 12, 34, 56, 255});
	CHECK(image.Hash == 0x4c71c14abed3596dull);
	engine::imagegraph::Image seeded;
	REQUIRE(
		Evaluate(document, plan, "texture", EvaluationRequest{0, 12345}, seeded, diagnostic) == Status::Ok
	);
	CHECK(seeded.Hash == image.Hash);
}

TEST_CASE("compiler rejects cycles and documents over fixed limits", "[imagegraph]") {
	Document document;
	document.Nodes.push_back({"pass", "image.passthrough", "", {}, {}});
	document.Links.push_back({"pass", "image", "pass", "image"});
	document.Outputs.push_back({"out", "pass", "image"});
	Plan plan;
	Diagnostic diagnostic;
	CHECK(Compile(document, plan, diagnostic) == Status::Cycle);

	document = {};
	document.Nodes.resize(Limits::MaximumNodes + 1);
	CHECK(Compile(document, plan, diagnostic) == Status::LimitExceeded);
}

TEST_CASE("solid output is bounded RGBA8 with deterministic pixels and hash", "[imagegraph]") {
	Document document = SolidDocument();
	document.Nodes.push_back(Solid("other", 1, 1, {200, 100, 50, 255}));
	document.Outputs.push_back({"alternate", "other", "image"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);

	engine::imagegraph::Image first;
	engine::imagegraph::Image second;
	REQUIRE(Evaluate(document, plan, "texture", first, diagnostic) == Status::Ok);
	REQUIRE(Evaluate(document, plan, "texture", second, diagnostic) == Status::Ok);
	CHECK(first.Width == 2);
	CHECK(first.Height == 1);
	CHECK(first.Pixels == std::vector<uint8_t>{12, 34, 56, 255, 12, 34, 56, 255});
	CHECK(first.Hash == second.Hash);
	CHECK(first.Hash == 0x4c71c14abed3596dull);
	engine::imagegraph::Image alternate;
	REQUIRE(Evaluate(document, plan, "alternate", alternate, diagnostic) == Status::Ok);
	CHECK(alternate.Width == 1);
	CHECK(alternate.Height == 1);
	CHECK(alternate.Pixels == std::vector<uint8_t>{200, 100, 50, 255});
	CHECK(alternate.Hash != first.Hash);

	Document oversized = SolidDocument();
	oversized.Nodes[0].Values[0].Data = int64_t{Limits::MaximumDimension + 1};
	CHECK(Compile(oversized, plan, diagnostic) == Status::LimitExceeded);
}

TEST_CASE("documented image filters evaluate bounded RGBA8 pixels", "[imagegraph]") {
	Document document;
	document.Nodes.push_back(Solid("source", 2, 1, {12, 34, 56, 100}));
	document.Nodes.push_back({"flip", "image.flip", "", {}, {{"axis", int64_t{1}}}});
	document.Nodes.push_back({"invert", "image.invert", "", {}, {{"include_alpha", false}}});
	document.Nodes.push_back({"cutoff", "image.alpha_cutoff", "", {}, {{"minimum", 0.5}}});
	document.Links = {
		{"source", "image", "flip", "image"},
		{"flip", "image", "invert", "image"},
		{"invert", "image", "cutoff", "image"},
	};
	document.Outputs = {
		{"flipped", "flip", "image"},
		{"inverted", "invert", "image"},
		{"cut", "cutoff", "image"},
	};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	engine::imagegraph::Image image;
	REQUIRE(Evaluate(document, plan, "flipped", image, diagnostic) == Status::Ok);
	CHECK(image.Pixels == std::vector<uint8_t>{12, 34, 56, 100, 12, 34, 56, 100});
	REQUIRE(Evaluate(document, plan, "inverted", image, diagnostic) == Status::Ok);
	CHECK(image.Pixels == std::vector<uint8_t>{243, 221, 199, 100, 243, 221, 199, 100});
	const uint64_t invertedHash = image.Hash;
	REQUIRE(Evaluate(document, plan, "inverted", image, diagnostic) == Status::Ok);
	CHECK(image.Hash == invertedHash);
	REQUIRE(Evaluate(document, plan, "cut", image, diagnostic) == Status::Ok);
	CHECK(image.Pixels == std::vector<uint8_t>{0, 0, 0, 0, 0, 0, 0, 0});

	document.Nodes[0].Values[2].Data = Colour{12, 34, 56, 128};
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	REQUIRE(Evaluate(document, plan, "cut", image, diagnostic) == Status::Ok);
	CHECK(image.Pixels == std::vector<uint8_t>{243, 221, 199, 128, 243, 221, 199, 128});
	document.Nodes[2].Values[0].Data = true;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	REQUIRE(Evaluate(document, plan, "inverted", image, diagnostic) == Status::Ok);
	CHECK(image.Pixels == std::vector<uint8_t>{243, 221, 199, 127, 243, 221, 199, 127});
}

TEST_CASE("image filters reject missing input and invalid typed controls", "[imagegraph]") {
	Document document = SolidDocument();
	document.Nodes.push_back({"flip", "image.flip", "", {}, {{"axis", int64_t{4}}}});
	document.Outputs[0] = {"texture", "flip", "image"};
	Plan plan;
	Diagnostic diagnostic;
	CHECK(Compile(document, plan, diagnostic) == Status::InvalidValue);
	CHECK(diagnostic.NodeId == "flip");
	CHECK(diagnostic.Port == "axis");
	document.Nodes[1].Values[0].Data = int64_t{2};
	CHECK(Compile(document, plan, diagnostic) == Status::InvalidValue);
	CHECK(diagnostic.Port == "image");
	document.Links.push_back({"node-a", "image", "flip", "image"});
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	document.Nodes[1].Values[0].Data = true;
	CHECK(Compile(document, plan, diagnostic) == Status::TypeMismatch);
	document.Nodes[1].Values[0].Data = int64_t{3};
	document.Nodes.push_back({"cutoff", "image.alpha_cutoff", "", {}, {{"minimum", 1.1}}});
	document.Links.push_back({"flip", "image", "cutoff", "image"});
	document.Outputs[0] = {"texture", "cutoff", "image"};
	CHECK(Compile(document, plan, diagnostic) == Status::InvalidValue);
	CHECK(diagnostic.NodeId == "cutoff");
	CHECK(diagnostic.Port == "minimum");
}

TEST_CASE("flip axes permute asymmetric pixels without changing channel bytes", "[imagegraph]") {
	engine::imagegraph::Image source;
	source.Width = 2;
	source.Height = 2;
	source.Pixels = {
		1,
		2,
		3,
		4,
		5,
		6,
		7,
		8,
		9,
		10,
		11,
		12,
		13,
		14,
		15,
		16,
	};
	engine::imagegraph::Image result = source;
	engine::imagegraph::detail::Flip(source, result, 1);
	CHECK(result.Pixels == std::vector<uint8_t>{5, 6, 7, 8, 1, 2, 3, 4, 13, 14, 15, 16, 9, 10, 11, 12});
	engine::imagegraph::detail::Flip(source, result, 2);
	CHECK(result.Pixels == std::vector<uint8_t>{9, 10, 11, 12, 13, 14, 15, 16, 1, 2, 3, 4, 5, 6, 7, 8});
	engine::imagegraph::detail::Flip(source, result, 3);
	CHECK(result.Pixels == std::vector<uint8_t>{13, 14, 15, 16, 9, 10, 11, 12, 5, 6, 7, 8, 1, 2, 3, 4});
}

TEST_CASE("evaluation releases finished intermediates within the live byte budget", "[imagegraph]") {
	Document document;
	document.Nodes.push_back(Solid("source", 3072, 3072, {7, 8, 9, 10}));
	for (int index = 1; index <= 3; index++) {
		const std::string nodeId = "copy-" + std::to_string(index);
		const std::string sourceId = index == 1 ? "source" : "copy-" + std::to_string(index - 1);
		document.Nodes.push_back({nodeId, "image.passthrough", "", {}, {}});
		document.Links.push_back({sourceId, "image", nodeId, "image"});
	}
	document.Outputs.push_back({"out", "copy-3", "image"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	engine::imagegraph::Image image;
	REQUIRE(Evaluate(document, plan, "out", image, diagnostic) == Status::Ok);
	CHECK(image.Pixels.size() == 3072u * 3072u * 4u);
	CHECK(image.Pixels.front() == 7);
	CHECK(image.Pixels.back() == 10);
}

TEST_CASE("fixed tick evaluation interpolates colour without mutating authored keys", "[imagegraph]") {
	Document document = SolidDocument();
	document.Keyframes = {
		{"node-a", "colour", 0, Colour{0, 10, 20, 30}, "linear"},
		{"node-a", "colour", 10, Colour{10, 20, 30, 40}, "step"},
	};
	const std::string authored = Write(document);
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	engine::imagegraph::Image image;
	REQUIRE(Evaluate(document, plan, "texture", 5, image, diagnostic) == Status::Ok);
	CHECK(image.Pixels == std::vector<uint8_t>{5, 15, 25, 35, 5, 15, 25, 35});
	const uint64_t halfwayHash = image.Hash;
	REQUIRE(Evaluate(document, plan, "texture", 5, image, diagnostic) == Status::Ok);
	CHECK(image.Hash == halfwayHash);
	REQUIRE(Evaluate(document, plan, "texture", 10, image, diagnostic) == Status::Ok);
	CHECK(image.Pixels == std::vector<uint8_t>{10, 20, 30, 40, 10, 20, 30, 40});
	REQUIRE(Evaluate(document, plan, "texture", 11, image, diagnostic) == Status::Ok);
	CHECK(image.Pixels == std::vector<uint8_t>{10, 20, 30, 40, 10, 20, 30, 40});
	REQUIRE(Evaluate(document, plan, "texture", image, diagnostic) == Status::Ok);
	CHECK(image.Pixels == std::vector<uint8_t>{0, 10, 20, 30, 0, 10, 20, 30});
	CHECK(Write(document) == authored);
	Document parsed;
	REQUIRE(Read(authored, parsed, diagnostic) == Status::Ok);
	REQUIRE(Compile(parsed, plan, diagnostic) == Status::Ok);
	REQUIRE(Evaluate(parsed, plan, "texture", 5, image, diagnostic) == Status::Ok);
	CHECK(image.Hash == halfwayHash);

	document.Keyframes[0].Tick = 3;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	REQUIRE(Evaluate(document, plan, "texture", 0, image, diagnostic) == Status::Ok);
	CHECK(image.Pixels == std::vector<uint8_t>{0, 10, 20, 30, 0, 10, 20, 30});
	document.Keyframes[0].Interpolation = "step";
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	REQUIRE(Evaluate(document, plan, "texture", 9, image, diagnostic) == Status::Ok);
	CHECK(image.Pixels == std::vector<uint8_t>{0, 10, 20, 30, 0, 10, 20, 30});
}

TEST_CASE("fixed tick scalar interpolation changes a selected filter", "[imagegraph]") {
	Document document = SolidDocument();
	document.Nodes[0].Values[2].Data = Colour{7, 8, 9, 128};
	document.Nodes.push_back({"cutoff", "image.alpha_cutoff", "", {}, {{"minimum", 0.0}}});
	document.Links.push_back({"node-a", "image", "cutoff", "image"});
	document.Outputs[0] = {"texture", "cutoff", "image"};
	document.Keyframes = {
		{"cutoff", "minimum", 0, 0.0, "linear"},
		{"cutoff", "minimum", 10, 1.0, "step"},
	};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	engine::imagegraph::Image image;
	REQUIRE(Evaluate(document, plan, "texture", 5, image, diagnostic) == Status::Ok);
	CHECK(image.Pixels == std::vector<uint8_t>{7, 8, 9, 128, 7, 8, 9, 128});
	REQUIRE(Evaluate(document, plan, "texture", 6, image, diagnostic) == Status::Ok);
	CHECK(image.Pixels == std::vector<uint8_t>{0, 0, 0, 0, 0, 0, 0, 0});
}

TEST_CASE("native interpolation has exact vector, colour and unsupported type rules", "[imagegraph]") {
	engine::imagegraph::Value value;
	CHECK(
		engine::imagegraph::detail::Interpolate(
			engine::imagegraph::Value{Vector2{-2.0, 4.0}},
			engine::imagegraph::Value{Vector2{2.0, 0.0}},
			5,
			10,
			value
		) == Status::Ok
	);
	CHECK(std::get<Vector2>(value) == Vector2{0.0, 2.0});
	CHECK(
		engine::imagegraph::detail::Interpolate(
			engine::imagegraph::Value{Colour{0, 255, 0, 255}},
			engine::imagegraph::Value{Colour{255, 0, 255, 0}},
			5,
			10,
			value
		) == Status::Ok
	);
	CHECK(std::get<Colour>(value) == Colour{128, 127, 128, 127});
	CHECK(
		engine::imagegraph::detail::Interpolate(
			engine::imagegraph::Value{int64_t{1}}, engine::imagegraph::Value{int64_t{2}}, 5, 10, value
		) == Status::UnsupportedExecution
	);
}

TEST_CASE("timeline rejects unsupported curves and bounds ranges", "[imagegraph]") {
	Document document = SolidDocument();
	document.Keyframes = {
		{"node-a", "colour", 0, Colour{0, 0, 0, 255}, "cubic"},
		{"node-a", "colour", 10, Colour{255, 255, 255, 255}, "step"},
	};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	engine::imagegraph::Image image;
	CHECK(Evaluate(document, plan, "texture", 5, image, diagnostic) == Status::UnsupportedExecution);
	CHECK(diagnostic.NodeId == "node-a");
	CHECK(diagnostic.Port == "colour");
	REQUIRE(Evaluate(document, plan, "texture", 10, image, diagnostic) == Status::Ok);
	CHECK(image.Pixels.front() == 255);
	CHECK(
		Evaluate(document, plan, "texture", Limits::MaximumTick + 1, image, diagnostic) ==
		Status::LimitExceeded
	);
	document.Keyframes[1].Tick = Limits::MaximumTick + 1;
	CHECK(Compile(document, plan, diagnostic) == Status::LimitExceeded);
	CHECK(diagnostic.Port == "colour");

	size_t count = 0;
	CHECK(ValidateTickRange(TickRange{0, 10, 3}, count, diagnostic) == Status::Ok);
	CHECK(count == 4);
	CHECK(ValidateTickRange(TickRange{0, 10, 0}, count, diagnostic) == Status::InvalidValue);
	CHECK(count == 0);
	CHECK(ValidateTickRange(TickRange{10, 0, 1}, count, diagnostic) == Status::InvalidValue);
	CHECK(
		ValidateTickRange(TickRange{0, Limits::MaximumTick + 1, 1}, count, diagnostic) ==
		Status::LimitExceeded
	);
	CHECK(
		ValidateTickRange(TickRange{0, Limits::MaximumRangeFrames, 1}, count, diagnostic) ==
		Status::LimitExceeded
	);
	CHECK(
		ValidateTickRange(TickRange{0, Limits::MaximumRangeFrames - 1, 1}, count, diagnostic) == Status::Ok
	);
	CHECK(count == Limits::MaximumRangeFrames);
}

TEST_CASE("timeline evaluates only keyframes upstream of the selected output", "[imagegraph]") {
	Document document = SolidDocument();
	document.Nodes.push_back(Solid("other", 1, 1, {5, 6, 7, 8}));
	document.Outputs.push_back({"other-out", "other", "image"});
	document.Keyframes = {
		{"other", "colour", 0, Colour{0, 0, 0, 0}, "cubic"},
		{"other", "colour", 10, Colour{255, 255, 255, 255}, "step"},
	};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	engine::imagegraph::Image image;
	REQUIRE(Evaluate(document, plan, "texture", 5, image, diagnostic) == Status::Ok);
	CHECK(image.Pixels == std::vector<uint8_t>{12, 34, 56, 255, 12, 34, 56, 255});
	CHECK(Evaluate(document, plan, "other-out", 5, image, diagnostic) == Status::UnsupportedExecution);
	CHECK(diagnostic.NodeId == "other");
}

TEST_CASE("masked invert applies blend before selected RGBA channels", "[imagegraph]") {
	Document document;
	document.Nodes.push_back(Solid("source", 1, 1, {10, 20, 30, 200}));
	document.Nodes.push_back(Solid("mask", 1, 1, {255, 255, 255, 128}));
	document.Nodes.push_back(
		{"invert", "image.invert", "", {}, {{"include_alpha", true}, {"mix", 1.0}, {"channel", int64_t{9}}}}
	);
	document.Links = {
		{"source", "image", "invert", "image"},
		{"mask", "image", "invert", "mask"},
	};
	document.Outputs.push_back({"out", "invert", "image"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	engine::imagegraph::Image image;
	REQUIRE(Evaluate(document, plan, "out", image, diagnostic) == Status::Ok);
	CHECK(image.Pixels == std::vector<uint8_t>{128, 20, 30, 127});
	const uint64_t hash = image.Hash;
	Document parsed;
	REQUIRE(Read(Write(document), parsed, diagnostic) == Status::Ok);
	REQUIRE(Compile(parsed, plan, diagnostic) == Status::Ok);
	REQUIRE(Evaluate(parsed, plan, "out", image, diagnostic) == Status::Ok);
	CHECK(image.Hash == hash);
	REQUIRE(Evaluate(document, plan, "out", image, diagnostic) == Status::Ok);
	CHECK(image.Hash == hash);
	document.Nodes[2].Values[1].Data = 0.0;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	REQUIRE(Evaluate(document, plan, "out", image, diagnostic) == Status::Ok);
	CHECK(image.Pixels == std::vector<uint8_t>{10, 20, 30, 200});
	document.Nodes[2].Values[1].Data = 1.0;
	document.Nodes[2].Values[2].Data = int64_t{0};
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	REQUIRE(Evaluate(document, plan, "out", image, diagnostic) == Status::Ok);
	CHECK(image.Pixels == std::vector<uint8_t>{10, 20, 30, 200});
}

TEST_CASE("alpha cutoff mask blends alpha and keeps RGB at transparent edge", "[imagegraph]") {
	Document document;
	document.Nodes.push_back(Solid("source", 1, 1, {10, 20, 30, 100}));
	document.Nodes.push_back(Solid("mask", 1, 1, {255, 255, 255, 128}));
	document.Nodes.push_back({"cut", "image.alpha_cutoff", "", {}, {{"minimum", 0.5}, {"mix", 1.0}}});
	document.Links = {
		{"source", "image", "cut", "image"},
		{"mask", "image", "cut", "mask"},
	};
	document.Outputs.push_back({"out", "cut", "image"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	engine::imagegraph::Image image;
	REQUIRE(Evaluate(document, plan, "out", image, diagnostic) == Status::Ok);
	CHECK(image.Pixels == std::vector<uint8_t>{10, 20, 30, 50});
	document.Nodes[1].Values[2].Data = Colour{0, 0, 0, 255};
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	REQUIRE(Evaluate(document, plan, "out", image, diagnostic) == Status::Ok);
	CHECK(image.Pixels == std::vector<uint8_t>{10, 20, 30, 100});
}

TEST_CASE("mask weights vary per pixel and transparent RGB follows source rules", "[imagegraph]") {
	engine::imagegraph::Image original{2, 1, {10, 20, 30, 200, 10, 20, 30, 0}, 0};
	engine::imagegraph::Image edited = original;
	engine::imagegraph::detail::Invert(edited, true);
	engine::imagegraph::Image mask{2, 1, {0, 0, 0, 255, 255, 255, 255, 255}, 0};
	engine::imagegraph::detail::ApplyMaskMix(original, edited, &mask, 0.5);
	CHECK(edited.Pixels == std::vector<uint8_t>{10, 20, 30, 200, 245, 235, 225, 128});
	engine::imagegraph::detail::ApplyChannels(original, edited, 0b0101);
	CHECK(edited.Pixels == std::vector<uint8_t>{10, 20, 30, 200, 245, 20, 225, 0});
}

TEST_CASE("masked filters reject invalid controls and sample different mask dimensions", "[imagegraph]") {
	Document document;
	document.Nodes.push_back(Solid("source", 1, 1));
	document.Nodes.push_back(Solid("mask", 2, 1));
	document.Nodes.push_back(
		{"invert", "image.invert", "", {}, {{"include_alpha", false}, {"channel", int64_t{16}}}}
	);
	document.Links = {
		{"source", "image", "invert", "image"},
		{"mask", "image", "invert", "mask"},
	};
	document.Outputs.push_back({"out", "invert", "image"});
	Plan plan;
	Diagnostic diagnostic;
	CHECK(Compile(document, plan, diagnostic) == Status::InvalidValue);
	CHECK(diagnostic.Port == "channel");
	document.Nodes[2].Values[1].Data = int64_t{15};
	document.Nodes[2].Values.push_back({"mix", 1.1});
	CHECK(Compile(document, plan, diagnostic) == Status::InvalidValue);
	CHECK(diagnostic.Port == "mix");
	document.Nodes[2].Values[2].Data = 1.0;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	engine::imagegraph::Image image;
	REQUIRE(Evaluate(document, plan, "out", image, diagnostic) == Status::Ok);
	CHECK(image.Pixels == std::vector<uint8_t>{43, 59, 75, 255});
}

TEST_CASE("mask sampling maps a smaller surface over the whole output", "[imagegraph]") {
	engine::imagegraph::Image original{
		4, 1, {10, 20, 30, 255, 10, 20, 30, 255, 10, 20, 30, 255, 10, 20, 30, 255}, 0
	};
	engine::imagegraph::Image edited = original;
	engine::imagegraph::detail::Invert(edited, false);
	engine::imagegraph::Image mask{2, 1, {0, 0, 0, 255, 255, 255, 255, 255}, 0};
	engine::imagegraph::detail::ApplyMaskMix(original, edited, &mask, 1.0);
	CHECK(
		edited.Pixels ==
		std::vector<uint8_t>{10, 20, 30, 255, 10, 20, 30, 255, 245, 235, 225, 255, 245, 235, 225, 255}
	);
}

TEST_CASE("Number Add value route drives Gradient Angle with exact pixels", "[imagegraph]") {
	Document document;
	document.FormatVersion = 6;
	document.Nodes.push_back({"number", "value.number", "", {}, {{"value", 30.0}}});
	document.Nodes.push_back(
		{"math", "value.math", "", {}, {{"b", 90.0}, {"mode", int64_t{0}}, {"degrees", true}}}
	);
	engine::imagegraph::Gradient gradient;
	gradient.Keys = {{0.0, {0, 0, 0, 255}}, {1.0, {255, 255, 255, 255}}};
	document.Nodes.push_back(
		{"gradient",
		 "image.gradient",
		 "",
		 {},
		 {{"width", int64_t{1}},
		  {"height", int64_t{1}},
		  {"gradient", gradient},
		  {"type", int64_t{0}},
		  {"center", Vector2{0, 0.5}}}}
	);
	document.Links = {{"number", "number", "math", "a"}, {"math", "result", "gradient", "angle_value"}};
	document.Outputs.push_back({"out", "gradient", "image"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	engine::imagegraph::Image image;
	REQUIRE(Evaluate(document, plan, "out", image, diagnostic) == Status::Ok);
	CHECK(image.Pixels == std::vector<uint8_t>{64, 64, 64, 255});

	gradient.Mode = 6;
	std::get<engine::imagegraph::Gradient>(document.Nodes[2].Values[2].Data) = gradient;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	CHECK(Evaluate(document, plan, "out", image, diagnostic) == Status::UnsupportedExecution);
	CHECK(diagnostic.Port == "gradient");
	CHECK(diagnostic.Message == "gradient CMYK pure black has undefined division");
}

TEST_CASE("flip applies source-era mask mix and channel after the spatial flip", "[imagegraph]") {
	engine::imagegraph::Image varied{2, 1, {10, 20, 30, 255, 100, 110, 120, 255}, 0};
	engine::imagegraph::Image mixed = varied;
	engine::imagegraph::detail::Flip(varied, mixed, 1);
	engine::imagegraph::Image fullMask{2, 1, {255, 255, 255, 255, 255, 255, 255, 255}, 0};
	engine::imagegraph::detail::ApplyMaskMix(varied, mixed, &fullMask, 0.5);
	engine::imagegraph::detail::ApplyChannels(varied, mixed, 1);
	CHECK(mixed.Pixels == std::vector<uint8_t>{55, 20, 30, 255, 55, 110, 120, 255});

	Document document;
	document.Nodes.push_back(Solid("source", 2, 1, {10, 20, 30, 255}));
	document.Nodes.push_back(Solid("mask", 2, 1, {255, 255, 255, 255}));
	document.Nodes.push_back(
		{"flip", "image.flip", "", {}, {{"axis", int64_t{1}}, {"mix", 0.5}, {"channel", int64_t{1}}}}
	);
	document.Links = {{"source", "image", "flip", "image"}, {"mask", "image", "flip", "mask"}};
	document.Outputs.push_back({"out", "flip", "image"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	engine::imagegraph::Image image;
	REQUIRE(Evaluate(document, plan, "out", image, diagnostic) == Status::Ok);
	CHECK(image.Pixels == std::vector<uint8_t>{10, 20, 30, 255, 10, 20, 30, 255});
	document.Nodes[1].Values[2].Data = Colour{0, 0, 0, 255};
	document.Nodes[2].Values.push_back({"invert_mask", true});
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	REQUIRE(Evaluate(document, plan, "out", image, diagnostic) == Status::Ok);
	CHECK(image.Pixels == std::vector<uint8_t>{10, 20, 30, 255, 10, 20, 30, 255});
}

TEST_CASE("inverted mask selects edited colour and feather spreads mask alpha", "[imagegraph]") {
	engine::imagegraph::Image original{1, 1, {10, 20, 30, 255}, 0};
	engine::imagegraph::Image edited = original;
	engine::imagegraph::detail::Invert(edited, false);
	engine::imagegraph::Image blackMask{1, 1, {0, 0, 0, 255}, 0};
	engine::imagegraph::detail::ApplyMaskMix(original, edited, &blackMask, 1.0, true);
	CHECK(edited.Pixels == std::vector<uint8_t>{245, 235, 225, 255});
	engine::imagegraph::Image halfBlack{1, 1, {0, 0, 0, 128}, 0};
	edited = original;
	engine::imagegraph::detail::Invert(edited, false);
	engine::imagegraph::detail::ApplyMaskMix(original, edited, &halfBlack, 1.0, true);
	CHECK(edited.Pixels == std::vector<uint8_t>{128, 128, 128, 255});

	engine::imagegraph::Image impulse{3, 1, {255, 255, 255, 0, 255, 255, 255, 255, 255, 255, 255, 0}, 0};
	const engine::imagegraph::Image feathered = engine::imagegraph::detail::FeatherMask(impulse, 3.0);
	CHECK(feathered.Pixels[3] > 0);
	CHECK(feathered.Pixels[3] < 255);
	CHECK(feathered.Pixels[7] < 255);
	CHECK(feathered.Pixels[3] == feathered.Pixels[11]);
	CHECK(engine::imagegraph::detail::FeatherMask(impulse, 0.0).Pixels == impulse.Pixels);
}

TEST_CASE("source-era mask feather is bounded", "[imagegraph]") {
	Document document = SolidDocument();
	document.Nodes.push_back(
		{"cut", "image.alpha_cutoff", "", {}, {{"minimum", 0.5}, {"mask_feather", 32.1}}}
	);
	document.Links.push_back({"node-a", "image", "cut", "image"});
	document.Outputs[0] = {"out", "cut", "image"};
	Plan plan;
	Diagnostic diagnostic;
	CHECK(Compile(document, plan, diagnostic) == Status::InvalidValue);
	CHECK(diagnostic.Port == "mask_feather");
}
