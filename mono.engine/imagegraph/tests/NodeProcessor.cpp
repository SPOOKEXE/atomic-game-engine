// Graph fixtures exercise source processor depths and pinned input-slot expansion order.

#include "../src/ArrayOps.hpp"
#include "../src/ProcessorBatch.hpp"
#include "../src/ValuePayload.hpp"
#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>

TEST_SUITE_ID("engine.imagegraph.node_processor")

using namespace engine::imagegraph;

namespace {
	ArrayValue Numbers(std::initializer_list<double> entries) {
		ArrayValue result{ValueType::Scalar, {}};
		for (double entry : entries)
			result.Elements.emplace_back(entry);
		return result;
	}
	Node Collector(std::string id, std::initializer_list<Vector2> vectors) {
		Node node{std::move(id), "pc.array", "", {}, {}, {}};
		for (Vector2 vector : vectors)
			node.DynamicInputs.push_back(
				{"input_" + std::to_string(node.DynamicInputs.size()), ValueType::Vector2, vector}
			);
		return node;
	}
	EvaluatedValue Evaluate(Document &document, std::string_view output = "out") {
		Plan plan;
		Diagnostic diagnostic;
		INFO(diagnostic.Message);
		REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
		EvaluatedValue value;
		const Status status = EvaluateValue(document, plan, std::string(output), {}, value, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
		return value;
	}
}

TEST_CASE(
	"Unequal processor arrays implement Loop Hold and pinned source expansion orders",
	"[imagegraph][node_processor]"
) {
	Document document;
	document.FormatVersion = 7;
	document.Nodes = {
		Collector("a", {{1, 0}, {2, 0}}),
		Collector("b", {{0, 10}, {0, 20}, {0, 30}}),
		{"cross", "pc.vector_cross_2_d", "", {}, {}, {}}
	};
	document.Links = {{"a", "array", "cross", "point_1"}, {"b", "array", "cross", "point_2"}};
	document.Outputs = {{"out", "cross", "result"}};
	const std::array expected{
		Numbers({10, 40, 30}),
		Numbers({10, 40, 60}),
		Numbers({10, 20, 30, 20, 40, 60}),
		Numbers({10, 20, 10, 40, 20, 40})
	};
	for (int64_t mode = 0; mode < 4; mode++) {
		document.Nodes.back().Values = {{"attribute_array_process", EnumValue{mode}}};
		CHECK(std::get<ArrayValue>(Evaluate(document).Data) == expected[mode]);
	}
	// The source reverses suffix-table positions, retaining repeated unequal-length pairs.
	std::vector<std::vector<size_t>> unchanged{{99}};
	const std::array<size_t, 2> lengths{2, 3};
	CHECK(
		detail::BuildArraySchedule(lengths, detail::ArrayProcessMode::Expand, 4096, unchanged, 1) ==
		Status::LimitExceeded
	);
	CHECK(unchanged == std::vector<std::vector<size_t>>{{99}});
}

TEST_CASE(
	"Nested numeric collector rows route through magnitude and gradient sampling",
	"[imagegraph][node_processor]"
) {
	Document document;
	document.FormatVersion = 7;
	Node rows{"rows", "pc.array", "", {}, {}, {}};
	rows.DynamicInputs = {
		{"input_0", ValueType::Array, Numbers({3, 4})}, {"input_1", ValueType::Array, Numbers({0, 0})}
	};
	document.Nodes = {
		rows,
		{"magnitude", "pc.vector_magnitude", "", {}, {}, {}},
		{"sample",
		 "pc.gradient_sample",
		 "",
		 {},
		 {{"type", EnumValue{1}},
		  {"gradient", Gradient{0, {{0, {0, 0, 0, 255}}, {1, {255, 255, 255, 255}}}}}},
		 {}}
	};
	document.Links = {
		{"rows", "array", "magnitude", "vector"}, {"magnitude", "magnitude", "sample", "ratio"}
	};
	document.Outputs = {
		{"out", "magnitude", "magnitude"}, {"colors", "sample", "colors"}, {"rows", "rows", "array"}
	};
	CHECK(std::get<ArrayValue>(Evaluate(document).Data) == Numbers({5, 0}));
	const auto colorValue = Evaluate(document, "colors");
	const auto &colors = std::get<ArrayValue>(colorValue.Data);
	CHECK(colors.Elements == std::vector<ElementValue>{Colour{0, 0, 0, 255}, Colour{0, 0, 0, 255}});
	const auto rowsValue = Evaluate(document, "rows");
	CHECK(std::get<ArrayValue>(rowsValue.Data).Nested.size() == 2);
	document.Nodes.front().Values = {{"spread_array", true}};
	CHECK(std::get<ArrayValue>(Evaluate(document, "rows").Data) == Numbers({3, 4, 0, 0}));
}

TEST_CASE(
	"Gradient colors generate named image arrays and inactive filters copy every row",
	"[imagegraph][node_processor]"
) {
	Document document;
	document.FormatVersion = 7;
	document.Nodes = {
		{"sample", "pc.gradient_sample", "", {}, {{"step", int64_t(2)}}, {}},
		{"solid", "pc.solid", "", {}, {{"dimension", Vector2{1, 1}}, {"dimension_unit", EnumValue{0}}}, {}},
		{"invert", "pc.invert", "", {}, {}, {}}
	};
	document.Links = {
		{"sample", "colors", "solid", "color"}, {"solid", "surface_out", "invert", "surface_in"}
	};
	document.Outputs = {{"out", "invert", "surface_out"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	ImageArray array;
	Status status = EvaluateArray(document, plan, "out", {}, array, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	REQUIRE(array.Images.size() == 2);
	CHECK(array.Images[0].Pixels == std::vector<uint8_t>{255, 255, 255, 255});
	CHECK(array.Images[1].Pixels == std::vector<uint8_t>{127, 127, 127, 255});
	document.Nodes.back().Values = {{"active", false}};
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	REQUIRE(EvaluateArray(document, plan, "out", {}, array, diagnostic) == Status::Ok);
	CHECK(array.Images[0].Pixels == std::vector<uint8_t>{0, 0, 0, 255});
	CHECK(array.Images[1].Pixels == std::vector<uint8_t>{128, 128, 128, 255});
}

TEST_CASE(
	"Rich processor outputs remain separate named ports and empty rows fail atomically",
	"[imagegraph][node_processor]"
) {
	Document document;
	document.FormatVersion = 7;
	Node times{"times", "pc.array", "", {}, {}, {}};
	times.DynamicInputs = {{"input_0", ValueType::Scalar, 0.0}, {"input_1", ValueType::Scalar, 1.0}};
	document.Nodes = {
		times,
		{"gradient", "pc.gradient_out", "", {}, {}, {}},
		{"extract", "pc.gradient_extract", "", {}, {}, {}}
	};
	document.Links = {
		{"times", "array", "gradient", "sample"}, {"gradient", "gradient", "extract", "gradient"}
	};
	document.Outputs = {
		{"out", "gradient", "gradient"}, {"color", "gradient", "color"}, {"positions", "extract", "positions"}
	};
	const auto gradientValue = Evaluate(document);
	const auto &gradients = std::get<ArrayValue>(gradientValue.Data);
	CHECK(gradients.ElementType == ValueType::Gradient);
	REQUIRE(gradients.Elements.size() == 2);
	CHECK(std::get<Gradient>(gradients.Elements[0]) == std::get<Gradient>(gradients.Elements[1]));
	CHECK(std::get<ArrayValue>(Evaluate(document, "color").Data).ElementType == ValueType::Colour);
	CHECK(std::get<ArrayValue>(Evaluate(document, "positions").Data).Nested.size() == 2);
	document.Nodes.front().DynamicInputs.clear();
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluatedValue sentinel{"sentinel", 42.0};
	CHECK(EvaluateValue(document, plan, "color", {}, sentinel, diagnostic) == Status::InvalidValue);
	CHECK(sentinel == EvaluatedValue{"sentinel", 42.0});
}

TEST_CASE(
	"A single processor outer row collapses by the pinned source and context budgets restore",
	"[imagegraph][node_processor]"
) {
	Document document;
	document.FormatVersion = 7;
	document.Nodes = {Collector("a", {{3, 4}}), {"magnitude", "pc.vector_magnitude", "", {}, {}, {}}};
	document.Links = {{"a", "array", "magnitude", "vector"}};
	document.Outputs = {{"out", "magnitude", "magnitude"}};
	CHECK(std::get<double>(Evaluate(document).Data) == 5);
	const CatalogueEntry *entry = FindCatalogueEntry("pc.gradient_out");
	REQUIRE(entry);
	Node node{"gradient", "pc.gradient_out", "", {}, {}, {}};
	EvaluationRequest request;
	detail::NodeContext context(node, *entry, request);
	context.ByteBudget = 4096;
	context.Values = {
		{"sample", Numbers({0, 1})},
		{"gradient", Gradient{0, {{0, {0, 0, 0, 255}}, {1, {255, 255, 255, 255}}}}}
	};
	const bool processed = detail::RunProcessorBatch(context, detail::FindExecutor(node.Type));
	INFO(context.FailureMessage);
	INFO(context.FailurePort);
	INFO(sizeof(Value));
	INFO(sizeof(ElementValue));
	INFO(context.AllocationBudget().Peak());
	CHECK(processed);
	CHECK(context.ByteBudget == 4096);
	CHECK(context.ValueViews.empty());
	context.ByteBudget = 1;
	CHECK_FALSE(detail::RunProcessorBatch(context, detail::FindExecutor(node.Type)));
	CHECK(context.ByteBudget == 1);
	CHECK(context.OutputValues.empty());
	CHECK(context.OutputImages.empty());
	CHECK(context.OutputImageArrays.empty());
}

TEST_CASE(
	"Processor row failures and aggregate rich payload limits publish no partial ports",
	"[imagegraph][node_processor]"
) {
	const CatalogueEntry *entry = FindCatalogueEntry("pc.gradient_out");
	REQUIRE(entry);
	Node node{"gradient", "pc.gradient_out", "", {}, {}, {}};
	EvaluationRequest request;
	detail::NodeContext context(node, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	context.Values = {{"sample", Numbers({0, 1})}};
	const auto failSecond = +[](detail::NodeContext &row) {
		row.SetValue("color", Colour{255, 0, 0, 255});
		if (row.Scalar("sample") == 1) return row.Fail(Status::InvalidValue, "second row denied");
		return true;
	};
	CHECK_FALSE(detail::RunProcessorBatch(context, failSecond));
	CHECK(context.OutputValues.empty());
	CHECK(context.ByteBudget == Limits::MaximumEvaluationBytes);
	context.FailureCode = Status::Ok;
	ArrayValue times{ValueType::Scalar, {}};
	times.Elements.resize(4096, 0.0);
	Gradient gradient;
	for (size_t key = 0; key < Limits::MaximumGradientKeys; key++)
		gradient.Keys.push_back({double(key) / (Limits::MaximumGradientKeys - 1), {0, 0, 0, 255}});
	context.Values = {{"sample", std::move(times)}, {"gradient", std::move(gradient)}};
	CHECK_FALSE(detail::RunProcessorBatch(context, detail::FindExecutor(node.Type)));
	CHECK(context.FailureCode == Status::LimitExceeded);
	CHECK(context.OutputValues.empty());
	CHECK(context.OutputImageArrays.empty());
}

TEST_CASE("Image array metadata is charged before copying a tiny surface", "[imagegraph][node_processor]") {
	const CatalogueEntry *entry = FindCatalogueEntry("pc.array");
	REQUIRE(entry);
	Node node{"array", "pc.array", "", {}, {}, {}};
	node.DynamicInputs = {{"input_0", ValueType::Image, std::nullopt}};
	EvaluationRequest request;
	Image pixel{1, 1, {1, 2, 3, 255}, 0};
	uint64_t required = 0;
	{
		detail::EvaluationBudget budget(Limits::MaximumEvaluationBytes);
		auto previousCharge = budget.Reserve(32);
		REQUIRE(previousCharge);
		std::vector<double> previous{1, 2, 3, 4};
		{
			detail::NodeContext context(node, *entry, request, budget);
			context.ByteBudget = Limits::MaximumEvaluationBytes;
			context.Images = {{"input_0", &pixel}};
			const bool success = detail::RunProcessorBatch(context, detail::FindExecutor(node.Type));
			INFO(context.FailureMessage);
			INFO(context.FailurePort);
			REQUIRE(success);
			REQUIRE(context.OutputImageArrays.size() == 1);
			CHECK(context.OutputImageArrays[0].second.Images[0].Pixels == pixel.Pixels);
			required = budget.Peak();
		}
		CHECK(budget.Used() == 32);
		CHECK(previous == std::vector<double>{1, 2, 3, 4});
	}
	REQUIRE(required > 32 + sizeof(Image) + sizeof(ImageArrayItem) + pixel.Pixels.size());
	for (const uint64_t limit : {required - 1, required}) {
		detail::EvaluationBudget budget(limit);
		auto previousCharge = budget.Reserve(32);
		REQUIRE(previousCharge);
		std::vector<double> previous{1, 2, 3, 4};
		{
			detail::NodeContext context(node, *entry, request, budget);
			context.ByteBudget = limit;
			context.Images = {{"input_0", &pixel}};
			const bool success = detail::RunProcessorBatch(context, detail::FindExecutor(node.Type));
			if (limit < required) {
				CHECK_FALSE(success);
				CHECK(context.FailureCode == Status::LimitExceeded);
				CHECK(context.OutputImageArrays.empty());
			} else {
				REQUIRE(success);
				REQUIRE(context.OutputImageArrays.size() == 1);
				CHECK(context.OutputImageArrays[0].second.Images[0].Pixels == pixel.Pixels);
				CHECK(budget.Peak() == required);
			}
			CHECK(context.ByteBudget == limit);
		}
		CHECK(budget.Used() == 32);
		CHECK(previous == std::vector<double>{1, 2, 3, 4});
		CHECK(pixel.Pixels == std::vector<uint8_t>{1, 2, 3, 255});
	}
}

TEST_CASE(
	"Authored source ratio arrays round trip and replay without changing scalar schemas",
	"[imagegraph][node_processor]"
) {
	Document document;
	document.FormatVersion = 7;
	document.Nodes = {
		{"sample", "pc.gradient_sample", "", {}, {{"type", EnumValue{1}}, {"ratio", Numbers({0, .5, 1})}}, {}}
	};
	document.Outputs = {{"out", "sample", "colors"}};
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
	CHECK(restored == document);
	Plan plan;
	REQUIRE(Compile(restored, plan, diagnostic) == Status::Ok);
	const auto *property = FindCatalogueInput(*FindCatalogueEntry("pc.gradient_sample"), "ratio");
	REQUIRE(property);
	CHECK(property->Type == ValueType::Scalar);
	CHECK(property->ArrayDepth == 1);
	EvaluatedValue first, replay;
	EvaluationRequest request;
	REQUIRE(EvaluateValue(restored, plan, "out", request, first, diagnostic) == Status::Ok);
	request.Tick = 100;
	REQUIRE(EvaluateValue(restored, plan, "out", request, replay, diagnostic) == Status::Ok);
	CHECK(replay == first);
	request.Tick = 0;
	REQUIRE(EvaluateValue(restored, plan, "out", request, replay, diagnostic) == Status::Ok);
	CHECK(replay == first);
	CHECK(
		std::get<ArrayValue>(first.Data).Elements ==
		std::vector<ElementValue>{Colour{0, 0, 0, 255}, Colour{128, 128, 128, 255}, Colour{0, 0, 0, 255}}
	);
	document.Nodes.front().Type = "pc.gradient_out";
	document.Nodes.front().Values = {{"sample", Numbers({0, .5, 1})}};
	document.Outputs.front().Port = "color";
	CHECK(
		std::get<ArrayValue>(Evaluate(document).Data).Elements ==
		std::vector<ElementValue>{
			Colour{0, 0, 0, 255}, Colour{128, 128, 128, 255}, Colour{255, 255, 255, 255}
		}
	);
	CHECK(FindCatalogueInput(*FindCatalogueEntry("pc.gradient_out"), "sample")->ArrayDepth == 0);
	Document vectorDocument;
	vectorDocument.FormatVersion = 7;
	vectorDocument.Nodes = {
		{"cross",
		 "pc.vector_cross_2_d",
		 "",
		 {},
		 {{"point_1", Numbers({3, 4})}, {"point_2", Vector2{0, 1}}},
		 {}}
	};
	vectorDocument.Outputs = {{"out", "cross", "result"}};
	CHECK(std::get<double>(Evaluate(vectorDocument).Data) == 3);
}

TEST_CASE(
	"Authored array exceptions reject unknown depths attributes wrong leaves and legacy schemas",
	"[imagegraph][node_processor]"
) {
	Document document;
	document.FormatVersion = 7;
	document.Nodes = {
		{"sample", "pc.gradient_sample", "", {}, {{"ratio", ArrayValue{ValueType::Colour, {Colour{}}}}}, {}}
	};
	document.Outputs = {{"out", "sample", "colors"}};
	Plan plan;
	Diagnostic diagnostic;
	CHECK(Compile(document, plan, diagnostic) == Status::TypeMismatch);
	document.Nodes.front().Values = {{"attribute_process", ArrayValue{ValueType::Boolean, {true, false}}}};
	CHECK(Compile(document, plan, diagnostic) == Status::TypeMismatch);
	document.Nodes.front().Type = "pc.color_rgb";
	document.Nodes.front().Values = {{"red", Numbers({0, 1})}};
	document.Outputs.front().Port = "color";
	const auto *entry = FindCatalogueEntry("pc.color_rgb");
	REQUIRE(entry);
	const auto *red = FindCatalogueInput(*entry, "red");
	REQUIRE(red);
	CHECK(red->ArrayDepthKnown);
	CHECK(red->ArrayDepth == 0);
	CHECK(CatalogueAuthoredArray(*entry, *red, Numbers({0, 1})));
	CHECK(Compile(document, plan, diagnostic) == Status::Ok);
	document.Nodes.front().Type = "image.solid";
	document.Nodes.front().Values = {{"width", ArrayValue{ValueType::Integer, {int64_t(1), int64_t(2)}}}};
	document.Outputs.front().Port = "image";
	CHECK(Compile(document, plan, diagnostic) == Status::TypeMismatch);
}

TEST_CASE(
	"Legacy image arrays enter verified source processors and refuse wrong payload leaves",
	"[imagegraph][node_processor]"
) {
	Document document;
	document.FormatVersion = 7;
	Node collected{"rows", "value.array", "", {}, {}, {}};
	collected.DynamicInputs = {
		{"red", ValueType::Image, std::nullopt}, {"green", ValueType::Image, std::nullopt}
	};
	document.Nodes = {
		{"red",
		 "pc.solid",
		 "",
		 {},
		 {{"dimension", Vector2{1, 1}}, {"dimension_unit", EnumValue{0}}, {"color", Colour{255, 0, 0, 255}}},
		 {}},
		{"green",
		 "pc.solid",
		 "",
		 {},
		 {{"dimension", Vector2{1, 1}}, {"dimension_unit", EnumValue{0}}, {"color", Colour{0, 255, 0, 255}}},
		 {}},
		collected,
		{"invert", "pc.invert", "", {}, {}, {}}
	};
	document.Links = {
		{"red", "surface_out", "rows", "red"},
		{"green", "surface_out", "rows", "green"},
		{"rows", "array", "invert", "surface_in"}
	};
	document.Outputs = {{"out", "invert", "surface_out"}};
	Diagnostic diagnostic;
	Plan plan;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	ImageArray images;
	REQUIRE(EvaluateArray(document, plan, "out", {}, images, diagnostic) == Status::Ok);
	REQUIRE(images.Images.size() == 2);
	CHECK(images.Images[0].Pixels == std::vector<uint8_t>{0, 255, 255, 255});
	CHECK(images.Images[1].Pixels == std::vector<uint8_t>{255, 0, 255, 255});
	document.Nodes.back().Type = "pc.texture_remap";
	CHECK(Compile(document, plan, diagnostic) == Status::TypeMismatch);
	CHECK_FALSE(FindCatalogueInput(*FindCatalogueEntry("pc.texture_remap"), "surface_in")->ArrayDepthKnown);
	document.Nodes.back().Type = "image.invert";
	document.Nodes.back().Values = {{"include_alpha", false}};
	document.Links.back().ToPort = "image";
	document.Outputs.back().Port = "image";
	CHECK(Compile(document, plan, diagnostic) == Status::TypeMismatch);
	document.Nodes.back().Type = "pc.heightmap_project_3_d";
	document.Nodes.back().Values = {
		{"dimension", Vector2{4, 4}},
		{"dimension_unit", EnumValue{0}},
		{"view_angle", Vector3{}},
		{"scale", 2.},
		{"interpolate", EnumValue{1}}
	};
	document.Links.back().ToPort = "heightmap";
	document.Outputs.back().Port = "surface_out";
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	const auto projected = EvaluateArray(document, plan, "out", {}, images, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(projected == Status::Ok);
	REQUIRE(images.Images.size() == 2);
	CHECK(images.Images[0].Pixels != images.Images[1].Pixels);
	const std::array<std::string_view, 2> producers{"red", "green"};
	for (size_t row = 0; row < producers.size(); ++row) {
		// A separate scalar graph resolves one surface, without the legacy array producer edge.
		Document scalar = document;
		scalar.Links.back().FromNode = std::string(producers[row]);
		scalar.Links.back().FromPort = "surface_out";
		Plan scalarPlan;
		REQUIRE(Compile(scalar, scalarPlan, diagnostic) == Status::Ok);
		Image expected;
		const auto status = Evaluate(scalar, scalarPlan, "out", {}, expected, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
		CHECK(images.Images[row].Width == 4);
		CHECK(images.Images[row].Height == 4);
		CHECK(images.Images[row].Format == expected.Format);
		CHECK(images.Images[row].Pixels == expected.Pixels);
	}
	// The image-only resolver refuses a successfully computed multi-image result.
	Image sentinel{1, 1, {1, 2, 3, 4}, 0};
	CHECK(Evaluate(document, plan, "out", {}, sentinel, diagnostic) == Status::InvalidOutput);
	CHECK(diagnostic.Message == "single-image consumer requires exactly one top-level image");
	CHECK(sentinel.Pixels == std::vector<uint8_t>{1, 2, 3, 4});
	Document wrong;
	wrong.FormatVersion = 7;
	Node numbers{"rows", "value.array", "", {}, {}, {}};
	numbers.DynamicInputs = {{"first", ValueType::Scalar, .25}, {"second", ValueType::Scalar, .75}};
	wrong.Nodes = {numbers, {"invert", "pc.invert", "", {}, {}, {}}};
	wrong.Links = {{"rows", "array", "invert", "surface_in"}};
	wrong.Outputs = {{"out", "invert", "surface_out"}};
	REQUIRE(Compile(wrong, plan, diagnostic) == Status::Ok);
	CHECK(Evaluate(wrong, plan, "out", {}, sentinel, diagnostic) == Status::UnsupportedExecution);
	CHECK(sentinel.Pixels == std::vector<uint8_t>{1, 2, 3, 4});
}

TEST_CASE(
	"Typed processor arrays reject Text to Scalar before executor fallback",
	"[imagegraph][node_processor][source_choice]"
) {
	Document document;
	document.FormatVersion = 7;
	Node text{
		"rows",
		"value.text_split",
		"",
		{},
		{{"text", std::string("wrong leaf")}, {"delimiter", std::string(" ")}},
		{}
	};
	document.Nodes = {
		text,
		{"sample",
		 "pc.gradient_sample",
		 "",
		 {},
		 {{"gradient", Gradient{0, {{0, Colour{0, 0, 0, 255}}, {1, Colour{255, 255, 255, 255}}}}}},
		 {}}
	};
	document.Links = {{"rows", "array", "sample", "shift"}};
	document.Outputs = {{"rows", "rows", "array"}, {"out", "sample", "colors"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluatedValue produced;
	REQUIRE(EvaluateValue(document, plan, "rows", {}, produced, diagnostic) == Status::Ok);
	const auto &leaves = std::get<ArrayValue>(produced.Data);
	REQUIRE(leaves.ElementType == ValueType::Text);
	REQUIRE(leaves.Elements.size() == 2);
	CHECK(std::get<std::string>(leaves.Elements[0]) == "wrong");
	CHECK(std::get<std::string>(leaves.Elements[1]) == "leaf");
	EvaluatedValue sentinel{"sentinel", Vector2{7, 9}};
	CHECK(EvaluateValue(document, plan, "out", {}, sentinel, diagnostic) == Status::UnsupportedExecution);
	CHECK(std::get<Vector2>(sentinel.Data) == Vector2{7, 9});
	CHECK(diagnostic.Port == "shift");
	CHECK(diagnostic.NodeId == "sample");
	CHECK(diagnostic.Message == "processor array leaf type is unsupported for this input");
}

TEST_CASE(
	"Source enum outer rows bypass scalar getter clamping and retain fractional palette math",
	"[imagegraph][node_processor][source_choice]"
) {
	Document document;
	document.FormatVersion = 7;
	ArrayValue colours{ValueType::Colour, {Colour{0, 0, 0, 255}, Colour{255, 255, 255, 255}}};
	document.Nodes = {
		{"palette",
		 "pc.gradient_palette",
		 "",
		 {},
		 {{"palette", colours}, {"interpolation", Numbers({-1, .5, 8})}},
		 {}}
	};
	document.Outputs = {{"out", "palette", "gradient"}};
	const auto result = Evaluate(document);
	const auto &rows = std::get<ArrayValue>(result.Data);
	REQUIRE(rows.Elements.size() == 3);
	for (const auto &row : rows.Elements) {
		const auto &gradient = std::get<Gradient>(row);
		CHECK(gradient.Mode == 0);
		CHECK(gradient.Keys[1].Time == .5);
	}
}

TEST_CASE(
	"Integer-only source enum consumers diagnose fractional modes atomically",
	"[imagegraph][node_processor][source_choice]"
) {
	Document document;
	document.FormatVersion = 7;
	document.Nodes = {
		{"solid", "pc.solid", "", {}, {{"dimension", Vector2{1, 1}}, {"dimension_unit", EnumValue{0}}}, {}},
		{"posterize", "pc.posterize", "", {}, {{"space", .5}}, {}}
	};
	document.Links = {{"solid", "surface_out", "posterize", "surface_in"}};
	document.Outputs = {{"out", "posterize", "surface_out"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	Image sentinel{1, 1, {7, 8, 9, 10}, 0};
	CHECK(Evaluate(document, plan, "out", {}, sentinel, diagnostic) == Status::UnsupportedExecution);
	CHECK(sentinel.Pixels == std::vector<uint8_t>{7, 8, 9, 10});
	CHECK(diagnostic.Port == "space");
	CHECK(diagnostic.Message == "fractional source choice is not supported by this executor");
}

TEST_CASE(
	"Residual arrays in scalar readers diagnose failure and discard successful executor outputs",
	"[imagegraph][node_processor][typed_reader]"
) {
	const CatalogueEntry *entry = FindCatalogueEntry("pc.number_simple");
	REQUIRE(entry);
	Node node{"number", "pc.number_simple", "", {}, {}, {}};
	EvaluationRequest request;
	const std::array<detail::Executor, 3> readers{
		+[](detail::NodeContext &context) {
			CHECK(context.Scalar("value", 17) == 17);
			return true;
		},
		+[](detail::NodeContext &context) {
			CHECK(context.Integer("value", 23) == 23);
			return true;
		},
		+[](detail::NodeContext &context) {
			CHECK(context.Boolean("value", true));
			return true;
		}
	};
	const std::array messages{
		"scalar reader cannot consume an array input",
		"integer reader cannot consume an array input",
		"boolean reader cannot consume an array input"
	};
	for (size_t index = 0; index < readers.size(); index++) {
		detail::NodeContext direct(node, *entry, request);
		direct.Values = {{"value", Numbers({1, 2})}};
		CHECK(readers[index](direct));
		CHECK(direct.FailureCode == Status::UnsupportedExecution);
		CHECK(direct.FailurePort == "value");
		CHECK(direct.FailureMessage == messages[index]);

		detail::NodeContext context(node, *entry, request);
		context.ByteBudget = 4096;
		context.Values = {{"value", Numbers({1, 2})}, {"reader", int64_t(index)}};
		const auto publishFallback = +[](detail::NodeContext &row) {
			switch (row.Integer("reader")) {
			case 0:
				row.SetValue("number", row.Scalar("value", 17));
				break;
			case 1:
				row.SetValue("number", row.Integer("value", 23));
				break;
			default:
				row.SetValue("number", row.Boolean("value", true));
				break;
			}
			row.OutputImages.emplace_back("stale_image", Image{});
			row.OutputImageArrays.emplace_back("stale_array", ImageArray{});
			return true;
		};
		CHECK_FALSE(detail::RunProcessorBatch(context, publishFallback));
		CHECK(context.FailureCode == Status::UnsupportedExecution);
		CHECK(context.FailurePort == "value");
		CHECK(context.FailureMessage == messages[index]);
		CHECK(context.ByteBudget == 4096);
		CHECK(context.OutputValues.empty());
		CHECK(context.OutputImages.empty());
		CHECK(context.OutputImageArrays.empty());
	}
}

TEST_CASE(
	"Compiled gradient processor selects numeric leaves before scalar readers",
	"[imagegraph][node_processor][typed_reader]"
) {
	Document document;
	document.FormatVersion = 7;
	document.Nodes = {
		{"gradient",
		 "pc.gradient_out",
		 "",
		 {},
		 {{"sample", Numbers({0, 1})},
		  {"gradient", Gradient{0, {{0, {0, 0, 0, 255}}, {1, {255, 255, 255, 255}}}}}},
		 {}}
	};
	document.Outputs = {{"out", "gradient", "color"}};
	const auto result = Evaluate(document);
	const auto &colours = std::get<ArrayValue>(result.Data);
	REQUIRE(colours.Elements.size() == 2);
	CHECK(std::get<Colour>(colours.Elements[0]) == Colour{0, 0, 0, 255});
	CHECK(std::get<Colour>(colours.Elements[1]) == Colour{255, 255, 255, 255});
}

TEST_CASE(
	"Gradient sample manual ratio arrays and unused step-mode ratio bypass scalar readers",
	"[imagegraph][node_processor][typed_reader]"
) {
	Document document;
	document.FormatVersion = 7;
	document.Nodes = {
		{"sample",
		 "pc.gradient_sample",
		 "",
		 {},
		 {{"gradient", Gradient{0, {{0, {0, 0, 0, 255}}, {1, {255, 255, 255, 255}}}}},
		  {"ratio", Numbers({.25, .75})},
		  {"type", EnumValue{1}},
		  {"step", int64_t{2}}},
		 {}}
	};
	document.Outputs = {{"out", "sample", "colors"}};
	const auto ratios = Evaluate(document);
	const auto &colours = std::get<ArrayValue>(ratios.Data);
	REQUIRE(colours.Elements.size() == 2);
	CHECK(std::get<Colour>(colours.Elements[0]) == Colour{64, 64, 64, 255});
	CHECK(std::get<Colour>(colours.Elements[1]) == Colour{191, 191, 191, 255});
	document.Nodes[0].Values[2].Data = EnumValue{0};
	const auto steps = Evaluate(document);
	const auto &stepColours = std::get<ArrayValue>(steps.Data);
	REQUIRE(stepColours.Elements.size() == 2);
	CHECK(std::get<Colour>(stepColours.Elements[0]) == Colour{0, 0, 0, 255});
	CHECK(std::get<Colour>(stepColours.Elements[1]) == Colour{128, 128, 128, 255});
}

TEST_CASE(
	"Source inverse includes scalar singleton and retained inner input slots", "[imagegraph][node_processor]"
) {
	std::vector<std::vector<size_t>> schedule;
	const std::array<size_t, 3> lengths{1, 2, 3};
	REQUIRE(
		detail::BuildSourceArraySchedule(lengths, detail::ArrayProcessMode::ExpandInverse, 8, schedule) ==
		Status::Ok
	);
	CHECK(
		schedule ==
		std::vector<std::vector<size_t>>{{0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 1, 0}, {0, 1, 0}, {0, 1, 0}}
	);
	const std::array<size_t, 3> middle{2, 1, 3};
	REQUIRE(
		detail::BuildSourceArraySchedule(middle, detail::ArrayProcessMode::ExpandInverse, 8, schedule) ==
		Status::Ok
	);
	CHECK(
		schedule ==
		std::vector<std::vector<size_t>>{{0, 0, 0}, {1, 0, 0}, {0, 0, 0}, {1, 0, 1}, {0, 0, 1}, {1, 0, 1}}
	);
	const std::array<size_t, 3> trailing{2, 3, 1};
	REQUIRE(
		detail::BuildSourceArraySchedule(trailing, detail::ArrayProcessMode::ExpandInverse, 8, schedule) ==
		Status::Ok
	);
	CHECK(
		schedule ==
		std::vector<std::vector<size_t>>{{0, 0, 0}, {1, 1, 0}, {0, 2, 0}, {1, 0, 0}, {0, 1, 0}, {1, 2, 0}}
	);
	const std::array<size_t, 3> unequal{2, 3, 4};
	REQUIRE(
		detail::BuildSourceArraySchedule(unequal, detail::ArrayProcessMode::ExpandInverse, 24, schedule) ==
		Status::Ok
	);
	REQUIRE(schedule.size() == 24);
	CHECK(schedule[1] == std::vector<size_t>{1, 0, 0});
	CHECK(schedule[4] == std::vector<size_t>{0, 1, 0});
	CHECK(schedule[12] == std::vector<size_t>{0, 0, 1});
	CHECK(schedule[23] == std::vector<size_t>{1, 2, 1});
	const auto unchanged = schedule;
	CHECK(
		detail::BuildSourceArraySchedule(middle, detail::ArrayProcessMode::ExpandInverse, 8, schedule, 1) ==
		Status::LimitExceeded
	);
	CHECK(schedule == unchanged);

	Document vector;
	vector.FormatVersion = 7;
	vector.Nodes = {
		{"vector",
		 "pc.vector2",
		 "",
		 {},
		 {{"x", Numbers({1, 2})}, {"y", Numbers({10, 20, 30})}, {"attribute_array_process", EnumValue{3}}},
		 {}}
	};
	vector.Outputs = {{"out", "vector", "vector"}, {"x", "vector", "x"}, {"y", "vector", "y"}};
	const auto output = Evaluate(vector);
	const auto &packed = std::get<ArrayValue>(output.Data);
	CHECK(
		packed.Elements ==
		std::vector<ElementValue>{
			Vector2{1, 10}, Vector2{2, 20}, Vector2{1, 30}, Vector2{2, 10}, Vector2{1, 20}, Vector2{2, 30}
		}
	);
	CHECK(std::get<ArrayValue>(Evaluate(vector, "x").Data) == Numbers({1, 2, 1, 2, 1, 2}));
	CHECK(std::get<ArrayValue>(Evaluate(vector, "y").Data) == Numbers({10, 20, 30, 10, 20, 30}));
	Document parsed;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(vector), parsed, diagnostic) == Status::Ok);
	CHECK(Evaluate(parsed).Data == output.Data);

	Document retained;
	retained.FormatVersion = 7;
	retained.Nodes = {
		Collector("b", {{0, 10}, {0, 20}, {0, 30}}),
		{"cross",
		 "pc.vector_cross_2_d",
		 "",
		 {},
		 {{"point_1", Vector2{1, 0}}, {"attribute_array_process", EnumValue{3}}},
		 {}}
	};
	retained.Links = {{"b", "array", "cross", "point_2"}};
	retained.Outputs = {{"out", "cross", "result"}};
	CHECK(std::get<ArrayValue>(Evaluate(retained).Data) == Numbers({10, 10, 10}));
	Node inner{"inner", "pc.array", "", {}, {{"type", EnumValue{2}}}, {}};
	inner.DynamicInputs = {{"input_0", ValueType::Scalar, 1.0}, {"input_1", ValueType::Scalar, 0.0}};
	retained.Nodes.insert(retained.Nodes.begin(), inner);
	retained.Nodes.back().Values.erase(retained.Nodes.back().Values.begin());
	retained.Links.push_back({"inner", "array", "cross", "point_1"});
	CHECK(std::get<ArrayValue>(Evaluate(retained).Data) == Numbers({10, 10, 10}));
}

TEST_CASE(
	"Source inverse uses actual dynamic group slots and rejects unresolved geometry atomically",
	"[imagegraph][node_processor]"
) {
	Document document;
	document.FormatVersion = 7;
	Node left{"left", "pc.array", "", {}, {{"type", EnumValue{4}}}, {}};
	left.DynamicInputs = {
		{"input_0", ValueType::Text, std::string("A")}, {"input_1", ValueType::Text, std::string("B")}
	};
	Node right{"right", "pc.array", "", {}, {{"type", EnumValue{4}}}, {}};
	right.DynamicInputs = {
		{"input_0", ValueType::Text, std::string("1")},
		{"input_1", ValueType::Text, std::string("2")},
		{"input_2", ValueType::Text, std::string("3")}
	};
	Node merge{"merge", "pc.string_merge", "", {}, {{"attribute_array_process", EnumValue{3}}}, {}};
	merge.DynamicInputs = {
		{"text_0", ValueType::Text, std::nullopt},
		{"text_1", ValueType::Text, std::string("-")},
		{"text_2", ValueType::Text, std::nullopt}
	};
	document.Nodes = {left, right, merge};
	document.Links = {{"left", "array", "merge", "text_0"}, {"right", "array", "merge", "text_2"}};
	document.Outputs = {{"out", "merge", "text"}};
	const auto value = Evaluate(document);
	CHECK(
		std::get<ArrayValue>(value.Data).Elements == std::vector<ElementValue>{
														 std::string("A-1"),
														 std::string("B-1"),
														 std::string("A-1"),
														 std::string("B-2"),
														 std::string("A-2"),
														 std::string("B-2")
													 }
	);
	CHECK(document.Nodes[0].DynamicInputs == left.DynamicInputs);

	const auto *entry = FindCatalogueEntry("pc.vector_cross_2_d");
	REQUIRE(entry);
	CatalogueEntry unresolved = *entry;
	std::vector<CatalogueInput> inputs(entry->Inputs.begin(), entry->Inputs.end());
	for (auto &input : inputs)
		if (input.SourceIndex == 1) input.SourceIndex = 2;
	unresolved.Inputs = inputs;
	Node node{"cross", "pc.vector_cross_2_d", "", {}, {}, {}};
	EvaluationRequest request;
	detail::NodeContext context(node, unresolved, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	ArrayValue rows{ValueType::Vector2, {Vector2{1, 0}, Vector2{2, 0}}};
	context.Values = {
		{"point_1", rows}, {"point_2", Vector2{0, 10}}, {"attribute_array_process", EnumValue{3}}
	};
	context.OutputValues = {{"result", 99.0}};
	CHECK_FALSE(detail::RunProcessorBatch(context, detail::FindExecutor(node.Type)));
	CHECK(context.FailureCode == Status::UnsupportedExecution);
	CHECK(context.FailureMessage == "source processor input slot layout is unresolved");
	CHECK(context.OutputValues.empty());
}

TEST_CASE(
	"Source dynamic slot arithmetic preserves wide metadata until bounded refusal",
	"[imagegraph][node_processor]"
) {
	const auto *source = FindCatalogueEntry("pc.string_merge");
	REQUIRE(source);
	CatalogueEntry entry = *source;
	entry.DynamicFixedLength = std::numeric_limits<int32_t>::max();
	entry.DynamicGroupLength = std::numeric_limits<int32_t>::max();
	Node node{"merge", "pc.string_merge", "", {}, {}, {}};
	node.DynamicInputs = {{"text_2", ValueType::Text, std::nullopt}};
	EvaluationRequest request;
	detail::NodeContext context(node, entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	context.Values = {
		{"text_2", ArrayValue{ValueType::Text, {std::string("A"), std::string("B")}}},
		{"attribute_array_process", EnumValue{3}}
	};
	context.OutputValues = {{"text", std::string("unchanged")}};
	CHECK_FALSE(detail::RunProcessorBatch(context, detail::FindExecutor(node.Type)));
	CHECK(context.FailureCode == Status::UnsupportedExecution);
	CHECK(context.FailureMessage == "source processor dynamic slot layout is unresolved");
	CHECK(context.FailurePort == "text_2");
	CHECK(context.OutputValues.empty());
}
