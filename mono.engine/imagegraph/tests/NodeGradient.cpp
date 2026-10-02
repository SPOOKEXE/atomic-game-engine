// Source-object fixtures use CPU matrices and packed alpha, separately from gradient shader fixtures.

#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>

TEST_SUITE_ID("engine.imagegraph.node_gradient")

using namespace engine::imagegraph;
using imagegraph_test::RunNode;

namespace {
	const Colour BLACK{0, 0, 0, 255}, WHITE{255, 255, 255, 255};
	Gradient Ramp(uint8_t mode = 0) {
		return {mode, {{0, BLACK}, {1, WHITE}}};
	}
	ArrayValue Palette(std::initializer_list<Colour> colours) {
		ArrayValue value{ValueType::Colour, {}};
		for (Colour colour : colours)
			value.Elements.emplace_back(colour);
		return value;
	}
	ArrayValue Positions(std::initializer_list<double> positions) {
		ArrayValue value{ValueType::Scalar, {}};
		for (double position : positions)
			value.Elements.emplace_back(position);
		return value;
	}
	template <class T> const T &Get(const imagegraph_test::NodeRun &run, std::string_view port) {
		REQUIRE(run.Ok);
		const Value *value = run.OutputValue(port);
		REQUIRE(value);
		REQUIRE(std::holds_alternative<T>(*value));
		return std::get<T>(*value);
	}
	Colour Sample(const Gradient &gradient, double time) {
		const auto run = RunNode("pc.gradient_out", {}, {{"gradient", gradient}, {"sample", time}});
		return Get<Colour>(run, "color");
	}
}

TEST_CASE(
	"CPU gradient endpoints equal keys and all 128 keys retain source object order",
	"[imagegraph][node_gradient]"
) {
	CHECK(Sample(Ramp(), -.2) == BLACK);
	CHECK(Sample(Ramp(), 1.2) == WHITE);
	CHECK(Sample(Ramp(), .5) == Colour{128, 128, 128, 255});
	CHECK(Sample(Ramp(1), .999) == BLACK);
	CHECK(Sample(Ramp(1), 1) == WHITE);
	CHECK(Sample(Gradient{}, .5) == Colour{0, 0, 0, 0});
	CHECK(Sample({0, {{.5, WHITE}}}, -100) == WHITE);
	Gradient repeated{0, {{0, BLACK}, {.5, {255, 0, 0, 1}}, {.5, {0, 255, 0, 2}}, {1, WHITE}}};
	CHECK(Sample(repeated, .5) == Colour{255, 0, 0, 1});
	CHECK(Sample(repeated, .75) == Colour{128, 255, 128, 128});
	Gradient many;
	for (size_t index = 0; index < 128; index++)
		many.Keys.push_back({double(index) / 127, {uint8_t(index), 0, 0, 255}});
	CHECK(Sample(many, 1) == Colour{127, 0, 0, 255});
	const auto run = RunNode("pc.gradient_out", {}, {{"gradient", many}});
	CHECK(Get<Gradient>(run, "gradient") == many);
}

TEST_CASE(
	"CPU gradient blend fixtures distinguish row major Oklab and source packed alpha",
	"[imagegraph][node_gradient]"
) {
	CHECK(Sample(Ramp(4), .5) == Colour{186, 186, 186, 255});
	CHECK(Sample({0, {{0, {0, 1, 2, 0}}, {1, {1, 2, 3, 1}}}}, .5) == Colour{0, 2, 2, 0});
	CHECK(Sample({3, {{0, {255, 0, 0, 0}}, {1, {0, 0, 255, 1}}}}, .5).Alpha == 0);
	CHECK(Sample({3, {{0, {255, 0, 0, 1}}, {1, {0, 0, 255, 2}}}}, .5).Alpha == 254);
	Gradient opaque{3, {{0, {255, 0, 0, 255}}, {1, {0, 0, 255, 255}}}};
	// __clamp255 is clamp(round(value), 0, 255); the alpha helper then multiplies by 255.
	// Independently calculated from __matrix3 row products, then cube/root and ties-even rounding.
	CHECK(Sample(opaque, .5) == Colour{159, 115, 150, 1});
	Gradient hsv{2, {{0, {255, 0, 0, 64}}, {1, {0, 255, 0, 192}}}};
	CHECK(Sample(hsv, .5) == Colour{255, 252, 0, 128});
	hsv.Mode = 5;
	CHECK(Sample(hsv, .5) == Colour{255, 0, 0, 128});
	Gradient cmyk{6, {{0, {255, 0, 0, 255}}, {1, {0, 255, 0, 255}}}};
	CHECK(Sample(cmyk, .5) == Colour{128, 128, 0, 1});
	CHECK_FALSE(RunNode("pc.gradient_out", {}, {{"gradient", Ramp(6)}, {"sample", .5}}).Ok);
}

TEST_CASE(
	"Palette to Gradient remaps modes preserves custom order and bounds source singularity",
	"[imagegraph][node_gradient]"
) {
	for (int64_t choice = 0; choice < 5; choice++) {
		const auto run = RunNode(
			"pc.gradient_palette",
			{},
			{{"palette", Palette({BLACK, WHITE})}, {"interpolation", EnumValue{choice}}}
		);
		const Gradient &gradient = Get<Gradient>(run, "gradient");
		CHECK(gradient.Mode == (choice == 0 ? 1 : choice == 1 ? 0 : choice));
		REQUIRE(gradient.Keys.size() == 2);
		CHECK(gradient.Keys[0].Time == 0);
		CHECK(gradient.Keys[1].Time == (choice == 1 ? 1 : .5));
	}
	const auto custom = RunNode(
		"pc.gradient_palette",
		{},
		{{"palette", Palette({BLACK, WHITE, BLACK})},
		 {"custom_positions", true},
		 {"positions", Positions({.8, .2})}}
	);
	CHECK(
		Get<Gradient>(custom, "gradient").Keys ==
		std::vector<GradientKey>{{.8, BLACK}, {.2, WHITE}, {0, BLACK}}
	);
	const auto empty = RunNode("pc.gradient_palette", {}, {{"palette", Palette({})}});
	CHECK(Get<Gradient>(empty, "gradient").Keys.empty());
	CHECK_FALSE(RunNode("pc.gradient_palette", {}, {{"palette", Palette({WHITE})}}).Ok);
	CHECK(
		Get<Gradient>(
			RunNode(
				"pc.gradient_palette", {}, {{"palette", Palette({WHITE})}, {"interpolation", EnumValue{0}}}
			),
			"gradient"
		)
			.Keys.size() == 1
	);
	ArrayValue large{ValueType::Colour, {}};
	large.Elements.resize(129, WHITE);
	CHECK(
		Get<Gradient>(RunNode("pc.gradient_palette", {}, {{"palette", large}}), "gradient").Keys.size() == 128
	);
}

TEST_CASE(
	"Sample Gradient uses signed fractional steps and scalar or array ratios", "[imagegraph][node_gradient]"
) {
	const auto steps = RunNode("pc.gradient_sample", {}, {{"gradient", Ramp()}, {"step", int64_t(4)}});
	CHECK(
		Get<ArrayValue>(steps, "colors") ==
		Palette({BLACK, {64, 64, 64, 255}, {128, 128, 128, 255}, {191, 191, 191, 255}})
	);
	const auto ratios = RunNode(
		"pc.gradient_sample",
		{},
		{{"gradient", Ramp()}, {"type", EnumValue{1}}, {"ratio", Positions({-.5, 0, .5, 1, 1.5})}}
	);
	CHECK(
		Get<ArrayValue>(ratios, "colors") ==
		Palette({BLACK, BLACK, {128, 128, 128, 255}, BLACK, {128, 128, 128, 255}})
	);
	const auto scalar = RunNode(
		"pc.gradient_sample",
		{},
		{{"gradient", Ramp()}, {"type", EnumValue{1}}, {"ratio", .25}, {"shift", -.5}}
	);
	CHECK(Get<ArrayValue>(scalar, "colors") == Palette({BLACK}));
	CHECK(
		Get<ArrayValue>(RunNode("pc.gradient_sample", {}, {{"step", int64_t(0)}}), "colors").Elements.empty()
	);
	CHECK_FALSE(RunNode("pc.gradient_sample", {}, {{"step", int64_t(Limits::MaximumArrayElements + 1)}}).Ok);
}

TEST_CASE(
	"Gradient Shift sorts reversals wraps negative times and replaces duplicate keys last",
	"[imagegraph][node_gradient]"
) {
	const auto shifted =
		RunNode("pc.gradient_shift", {}, {{"gradient", Ramp(4)}, {"shift", .25}, {"scale", -2.0}});
	CHECK(Get<Gradient>(shifted, "gradient") == Gradient{4, {{-.25, WHITE}, {1.75, BLACK}}});
	const auto wrapped =
		RunNode("pc.gradient_shift", {}, {{"gradient", Ramp()}, {"shift", -.25}, {"wrap", true}});
	CHECK(Get<Gradient>(wrapped, "gradient") == Gradient{0, {{.75, WHITE}}});
	const auto collapsed = RunNode("pc.gradient_shift", {}, {{"gradient", Ramp()}, {"scale", 0.0}});
	CHECK(Get<Gradient>(collapsed, "gradient") == Gradient{0, {{.5, WHITE}}});
	CHECK(
		Get<Gradient>(RunNode("pc.gradient_shift", {}, {{"gradient", Gradient{}}}), "gradient").Keys.empty()
	);
}

TEST_CASE(
	"Gradient Replace uses normalized RGB distance nearest first tie and looping target",
	"[imagegraph][node_gradient]"
) {
	Gradient gradient{5, {{0, {10, 0, 0, 1}}, {.5, {20, 0, 0, 2}}, {1, {30, 0, 0, 3}}}};
	const auto replaced = RunNode(
		"pc.gradient_replace_color",
		{},
		{{"gradient", gradient},
		 {"color_from", Palette({{0, 0, 0, 255}, {20, 0, 0, 255}, {30, 0, 0, 255}})},
		 {"color_to", Palette({BLACK, WHITE})},
		 {"threshold", 10.0 / 255}}
	);
	CHECK(Get<Gradient>(replaced, "gradient") == Gradient{5, {{0, BLACK}, {.5, WHITE}, {1, BLACK}}});
	const auto unchanged =
		RunNode("pc.gradient_replace_color", {}, {{"gradient", gradient}, {"color_to", Palette({})}});
	CHECK(Get<Gradient>(unchanged, "gradient") == gradient);
	CHECK(
		Get<Gradient>(RunNode("pc.gradient_replace_color", {}, {{"gradient", Gradient{}}}), "gradient") ==
		Gradient{0, {{0, BLACK}}}
	);
	const auto alphaIgnored = RunNode(
		"pc.gradient_replace_color",
		{},
		{{"gradient", gradient},
		 {"color_from", Palette({{10, 0, 0, 255}})},
		 {"color_to", Palette({WHITE})},
		 {"threshold", 0.0}}
	);
	CHECK(Get<Gradient>(alphaIgnored, "gradient").Keys[0].Color == WHITE);
}

TEST_CASE(
	"Gradient arrays route through compile evaluate serialize and linked ratio controls",
	"[imagegraph][node_gradient]"
) {
	Document document;
	document.FormatVersion = 7;
	document.Nodes = {
		{"palette", "pc.gradient_palette", "", {}, {{"palette", Palette({BLACK, WHITE})}}, {}},
		{"shift", "pc.gradient_shift", "", {}, {{"shift", .25}}, {}},
		{"extract", "pc.gradient_extract", "", {}, {}, {}},
		{"sample", "pc.gradient_sample", "", {}, {{"type", EnumValue{1}}}, {}},
		{"out", "pc.gradient_out", "", {}, {{"sample", .75}}, {}}
	};
	document.Links = {
		{"palette", "gradient", "shift", "gradient"},
		{"shift", "gradient", "extract", "gradient"},
		{"shift", "gradient", "sample", "gradient"},
		{"extract", "positions", "sample", "ratio"},
		{"shift", "gradient", "out", "gradient"}
	};
	document.Outputs = {
		{"colors", "extract", "colors"},
		{"positions", "extract", "positions"},
		{"mode", "extract", "type"},
		{"sampled", "sample", "colors"},
		{"color", "out", "color"}
	};
	Plan plan;
	Diagnostic diagnostic;
	Document roundtrip;
	REQUIRE(Read(Write(document), roundtrip, diagnostic) == Status::Ok);
	CHECK(roundtrip == document);
	INFO(diagnostic.Message);
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluationRequest request;
	EvaluatedValue value;
	REQUIRE(EvaluateValue(document, plan, "colors", request, value, diagnostic) == Status::Ok);
	CHECK(std::get<ArrayValue>(value.Data) == Palette({BLACK, WHITE}));
	REQUIRE(EvaluateValue(document, plan, "positions", request, value, diagnostic) == Status::Ok);
	CHECK(std::get<ArrayValue>(value.Data) == Positions({.25, 1.25}));
	REQUIRE(EvaluateValue(document, plan, "mode", request, value, diagnostic) == Status::Ok);
	CHECK(std::get<int64_t>(value.Data) == 0);
	REQUIRE(EvaluateValue(document, plan, "sampled", request, value, diagnostic) == Status::Ok);
	CHECK(std::get<ArrayValue>(value.Data) == Palette({BLACK, BLACK}));
	REQUIRE(EvaluateValue(document, plan, "color", request, value, diagnostic) == Status::Ok);
	CHECK(std::get<Colour>(value.Data) == Colour{128, 128, 128, 255});
}

TEST_CASE(
	"Gradient guards reject nonfinite controls wrong arrays and byte exhaustion before output",
	"[imagegraph][node_gradient]"
) {
	const double bad = std::numeric_limits<double>::infinity();
	for (const auto &[type, port] :
		 {std::pair{"pc.gradient_out", "sample"},
		  {"pc.gradient_sample", "shift"},
		  {"pc.gradient_shift", "scale"},
		  {"pc.gradient_replace_color", "threshold"}})
		CHECK_FALSE(RunNode(type, {}, {{port, bad}}).Ok);
	CHECK_FALSE(
		RunNode("pc.gradient_palette", {}, {{"custom_positions", true}, {"positions", Positions({bad})}}).Ok
	);
	CHECK_FALSE(RunNode("pc.gradient_sample", {}, {{"type", EnumValue{1}}, {"ratio", Palette({BLACK})}}).Ok);
	for (std::string_view type :
		 {"pc.gradient_out",
		  "pc.gradient_extract",
		  "pc.gradient_palette",
		  "pc.gradient_sample",
		  "pc.gradient_shift",
		  "pc.gradient_replace_color"}) {
		const auto *entry = FindCatalogueEntry(type);
		REQUIRE(entry);
		Node node{"n", std::string(type), "", {}, {}, {}};
		EvaluationRequest request;
		detail::NodeContext context(node, *entry, request);
		context.ByteBudget = 1;
		for (const auto &input : entry->Inputs)
			if (auto value = CatalogueDefault(input)) context.Values.emplace_back(input.Id, *value);
		REQUIRE(detail::FindExecutor(type));
		CHECK_FALSE(detail::FindExecutor(type)(context));
		CHECK(context.FailureCode == Status::LimitExceeded);
		CHECK(context.OutputValues.empty());
	}
}

TEST_CASE(
	"Source palette fractional choices retain constructor mode and scalar getter clamps",
	"[imagegraph][node_gradient][source_choice]"
) {
	// Pinned node_gradient_palette.gml switch assigns only exact cases; gradientObject starts smooth.
	const auto fractional =
		RunNode("pc.gradient_palette", {}, {{"palette", Palette({BLACK, WHITE})}, {"interpolation", .5}});
	const auto &gradient = Get<Gradient>(fractional, "gradient");
	CHECK(gradient.Mode == 0);
	CHECK(gradient.Keys[1].Time == .5);
	const auto below =
		RunNode("pc.gradient_palette", {}, {{"palette", Palette({BLACK, WHITE})}, {"interpolation", -4.0}});
	CHECK(Get<Gradient>(below, "gradient").Mode == 1);
	const auto above =
		RunNode("pc.gradient_palette", {}, {{"palette", Palette({BLACK, WHITE})}, {"interpolation", 8.0}});
	CHECK(Get<Gradient>(above, "gradient").Mode == 4);
	const auto retained = RunNode("pc.gradient_sample", {}, {{"gradient", Ramp()}, {"type", .5}});
	CHECK_FALSE(retained.Ok);
	CHECK(retained.Code == Status::UnsupportedExecution);
	CHECK(retained.Port == "type");
	CHECK(retained.Values.empty());
}
