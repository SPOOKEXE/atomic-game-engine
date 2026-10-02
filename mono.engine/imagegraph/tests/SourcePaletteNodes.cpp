#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/HostCapture.hpp>
#include <engine/imagegraph/SourceBuiltinRandom.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <limits>

TEST_SUITE_ID("engine.imagegraph.source_palette_nodes")
using namespace engine::imagegraph;
namespace {
	ArrayValue SourcePalette(std::initializer_list<Colour> colours) {
		ArrayValue result{ValueType::Colour, {}};
		for (auto colour : colours)
			result.Elements.emplace_back(colour);
		return result;
	}
	Document PaletteDocument(
		std::string_view type, std::vector<AuthoredValue> values, std::string_view port = "palette"
	) {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {{"palette", std::string(type), "", {}, std::move(values)}};
		document.Outputs = {{"colours", "palette", std::string(port)}};
		return document;
	}
	ArrayValue EvaluatePalette(const Document &document, const EvaluationRequest &request = {}) {
		Plan plan;
		Diagnostic diagnostic;
		const auto compile = Compile(document, plan, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(compile == Status::Ok);
		EvaluatedValue result;
		const auto status = EvaluateValue(document, plan, "colours", request, result, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
		REQUIRE(std::holds_alternative<ArrayValue>(result.Data));
		return std::get<ArrayValue>(result.Data);
	}
	std::vector<Colour> Colours(const ArrayValue &palette) {
		std::vector<Colour> result;
		for (const auto &value : palette.Elements) {
			REQUIRE(std::holds_alternative<Colour>(value));
			result.push_back(std::get<Colour>(value));
		}
		return result;
	}
}
TEST_CASE(
	"Palette trim sorts and clamps its endpoints before flooring exclusive indices",
	"[imagegraph][source_palette]"
) {
	const auto palette =
		SourcePalette({{0, 0, 0, 32}, {64, 64, 64, 64}, {128, 128, 128, 128}, {255, 255, 255, 255}});
	auto document = PaletteDocument("pc.palette", {{"palette", palette}, {"trim_range", Vector2{.75, .25}}});
	CHECK(Colours(EvaluatePalette(document)) == std::vector<Colour>{{64, 64, 64, 64}, {128, 128, 128, 128}});
	document.Nodes[0].Values[1].Data = Vector2{-1, 2};
	CHECK(EvaluatePalette(document) == palette);
	document.Nodes[0].Values[1].Data = Vector2{.2, .4};
	CHECK(Colours(EvaluatePalette(document)) == std::vector<Colour>{{0, 0, 0, 32}});
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
	CHECK(EvaluatePalette(restored) == EvaluatePalette(document));
}
TEST_CASE(
	"Palette sorting keeps sparse source enum indices and distinct custom brightness weights",
	"[imagegraph][source_palette]"
) {
	const Colour red{255, 0, 0, 32}, green{0, 255, 0, 64}, blue{0, 0, 255, 128};
	const auto palette = SourcePalette({red, green, blue});
	const std::array<std::vector<Colour>, 11> expected{
		std::vector<Colour>{green, red, blue},
		{red, green, blue},
		{blue, green, red},
		{blue, green, red},
		{blue, green, red},
		{red, green, blue},
		{red, green, blue},
		{green, blue, red},
		{blue, red, green},
		{red, green, blue},
		{red, green, blue}
	};
	for (int64_t mode = 0; mode <= 10; ++mode) {
		auto document = PaletteDocument(
			"pc.palette_sort",
			{{"palette_in", palette}, {"order", EnumValue{mode}}, {"sort_order", std::string{"RGB"}}},
			"sorted_palette"
		);
		CHECK(Colours(EvaluatePalette(document)) == expected[size_t(mode)]);
	}
	auto custom = PaletteDocument(
		"pc.palette_sort",
		{{"palette_in", palette}, {"order", EnumValue{10}}, {"sort_order", std::string{"rgb"}}},
		"sorted_palette"
	);
	CHECK(Colours(EvaluatePalette(custom)) == std::vector<Colour>{blue, green, red});
	custom.Nodes[0].Values.push_back({"reverse", true});
	CHECK(Colours(EvaluatePalette(custom)) == std::vector<Colour>{red, green, blue});
	const Colour dimRed{100, 0, 0, 255}, dimBlue{0, 0, 160, 255};
	custom.Nodes[0].Values[0].Data = SourcePalette({dimRed, dimBlue});
	custom.Nodes[0].Values[2].Data = std::string{"L"};
	custom.Nodes[0].Values.pop_back();
	CHECK(Colours(EvaluatePalette(custom)) == std::vector<Colour>{dimBlue, dimRed});
	custom.Nodes[0].Values[1].Data = EnumValue{0};
	CHECK(Colours(EvaluatePalette(custom)) == std::vector<Colour>{dimRed, dimBlue});
}
TEST_CASE(
	"Palette replacement accepts threshold equality preserves first ties and defaults short targets",
	"[imagegraph][source_palette]"
) {
	const Colour red{255, 0, 0, 128}, blue{0, 0, 255, 255}, black{0, 0, 0, 255}, target{0, 255, 0, 16};
	auto document = PaletteDocument(
		"pc.palette_replace",
		{{"palette_in", SourcePalette({red, blue})},
		 {"palette_from", SourcePalette({black, blue})},
		 {"palette_to", SourcePalette({target})},
		 {"threshold", 1.}},
		"surface_out"
	);
	CHECK(Colours(EvaluatePalette(document)) == std::vector<Colour>{target, blue});
	document.Nodes[0].Values[1].Data = SourcePalette({red, red});
	document.Nodes[0].Values[2].Data = SourcePalette({target, black});
	document.Nodes[0].Values[3].Data = 0.;
	CHECK(Colours(EvaluatePalette(document)) == std::vector<Colour>{target, blue});
	document.Nodes[0].Values[2].Data = SourcePalette({});
	CHECK(Colours(EvaluatePalette(document)) == std::vector<Colour>{red, blue});
}
TEST_CASE(
	"Palette shrink preserves input when already short and uses RGB histogram endpoints",
	"[imagegraph][source_palette]"
) {
	const auto palette =
		SourcePalette({{0, 0, 0, 16}, {85, 85, 85, 32}, {170, 170, 170, 64}, {255, 255, 255, 128}});
	auto document = PaletteDocument(
		"pc.palette_shrink", {{"palette_in", palette}, {"amount", int64_t{3}}, {"algorithm", EnumValue{0}}}
	);
	CHECK(
		Colours(EvaluatePalette(document)) ==
		std::vector<Colour>{{0, 0, 0, 255}, {128, 128, 128, 255}, {255, 255, 255, 255}}
	);
	document.Nodes[0].Values[1].Data = int64_t{4};
	CHECK(EvaluatePalette(document) == palette);
	document.Nodes[0].Values[1].Data = std::numeric_limits<int64_t>::max();
	CHECK(EvaluatePalette(document) == palette);
}
TEST_CASE(
	"Palette K-mean runs ten passes then sorts unique representative source indices",
	"[imagegraph][source_palette]"
) {
	auto document = PaletteDocument(
		"pc.palette_shrink",
		{{"palette_in",
		  SourcePalette(
			  {{0, 0, 0, 16},
			   {64, 64, 64, 32},
			   {128, 128, 128, 64},
			   {192, 192, 192, 128},
			   {255, 255, 255, 255}}
		  )},
		 {"amount", int64_t{2}}}
	);
	CHECK(Colours(EvaluatePalette(document)) == std::vector<Colour>{{0, 0, 0, 16}, {192, 192, 192, 128}});
	document.Nodes[0].Values[1].Data = int64_t{1};
	document.Nodes[0].Values.push_back({"shift", int64_t{1}});
	CHECK(Colours(EvaluatePalette(document)) == std::vector<Colour>{{128, 128, 128, 64}});
}
TEST_CASE(
	"Palette random cluster initialization consumes exact observed random calls",
	"[imagegraph][source_palette]"
) {
	auto document = PaletteDocument(
		"pc.palette_shrink",
		{{"palette_in",
		  SourcePalette(
			  {{0, 0, 0, 255},
			   {64, 64, 64, 255},
			   {128, 128, 128, 255},
			   {192, 192, 192, 255},
			   {255, 255, 255, 255}}
		  )},
		 {"amount", int64_t{2}},
		 {"sample_type", EnumValue{1}},
		 {"seed", 42.}}
	);
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	SourceBuiltinRandomCapture capture;
	REQUIRE(
		PrepareSourceBuiltinRandomCapture(document, plan, "palette", {}, capture, diagnostic) == Status::Ok
	);
	capture.Draws = {
		{SourceBuiltinRandomOperation::Random, 0, 1, .1}, {SourceBuiltinRandomOperation::Random, 0, 1, .9}
	};
	EvaluationRequest request;
	request.BuiltinRandomCaptures = std::span<const SourceBuiltinRandomCapture>(&capture, 1);
	CHECK(
		Colours(EvaluatePalette(document, request)) ==
		std::vector<Colour>{{0, 0, 0, 255}, {192, 192, 192, 255}}
	);
	EvaluatedValue output;
	output.Data = std::string{"unchanged"};
	capture.Draws[0].Upper = 2;
	CHECK(EvaluateValue(document, plan, "colours", request, output, diagnostic) == Status::InvalidValue);
	CHECK(std::get<std::string>(output.Data) == "unchanged");
}
TEST_CASE(
	"Palette singular source arithmetic and custom key overflow refuse without changing caller output",
	"[imagegraph][source_palette]"
) {
	auto document = PaletteDocument(
		"pc.palette_shrink",
		{{"palette_in", SourcePalette({{0, 0, 0, 255}, {255, 255, 255, 255}})},
		 {"amount", int64_t{1}},
		 {"algorithm", EnumValue{0}}}
	);
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluatedValue output;
	output.Data = std::string{"retained"};
	CHECK(EvaluateValue(document, plan, "colours", {}, output, diagnostic) == Status::UnsupportedExecution);
	CHECK(std::get<std::string>(output.Data) == "retained");
	HostNodeCapture observation;
	REQUIRE(PrepareHostCapture(document, plan, "palette", {}, observation, diagnostic) == Status::Ok);
	observation.Outputs = {{"palette", SourcePalette({{42, 42, 42, 255}})}};
	EvaluationRequest request;
	request.HostCaptures = std::span<const HostNodeCapture>(&observation, 1);
	CHECK(Colours(EvaluatePalette(document, request)) == std::vector<Colour>{{42, 42, 42, 255}});
	document = PaletteDocument(
		"pc.palette_sort", {{"sort_order", std::string(128, 'R')}, {"order", EnumValue{10}}}, "sorted_palette"
	);
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	CHECK(EvaluateValue(document, plan, "colours", {}, output, diagnostic) == Status::LimitExceeded);
}

TEST_CASE(
	"Palette histogram HSV follows component bounds rather than circular hue interpolation",
	"[imagegraph][source_palette]"
) {
	auto document = PaletteDocument(
		"pc.palette_shrink",
		{{"palette_in",
		  SourcePalette({{255, 0, 0, 16}, {0, 255, 0, 32}, {0, 0, 255, 64}, {255, 255, 255, 128}})},
		 {"amount", int64_t{3}},
		 {"algorithm", EnumValue{0}},
		 {"color_space", EnumValue{1}}}
	);
	CHECK(
		Colours(EvaluatePalette(document)) ==
		std::vector<Colour>{{255, 255, 255, 255}, {128, 255, 128, 255}, {0, 0, 255, 255}}
	);
}
TEST_CASE(
	"Palette trim keeps nested processor rows and complete typed colour trees", "[imagegraph][source_palette]"
) {
	ArrayValue rows{ValueType::Colour, {}};
	rows.Nested = {
		{{Colour{255, 0, 0, 32}, Colour{0, 255, 0, 64}}},
		{{Colour{0, 0, 255, 128}, Colour{255, 255, 255, 255}}}
	};
	auto document = PaletteDocument("pc.palette", {{"palette", rows}, {"trim_range", Vector2{0, .5}}});
	const auto result = EvaluatePalette(document);
	REQUIRE(result.Nested.size() == 2);
	CHECK(std::get<Colour>(result.Nested[0][0]) == Colour{255, 0, 0, 32});
	CHECK(std::get<Colour>(result.Nested[1][0]) == Colour{0, 0, 255, 128});
}

TEST_CASE(
	"Palette clustering admits its comparison bound before growing owned workspace",
	"[imagegraph][source_palette]"
) {
	ArrayValue palette{ValueType::Colour, {}};
	for (size_t i = 0; i < 2048; ++i)
		palette.Elements.emplace_back(Colour{uint8_t(i % 256), 0, 0, 255});
	auto document =
		PaletteDocument("pc.palette_shrink", {{"palette_in", std::move(palette)}, {"amount", int64_t{1024}}});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluatedValue previous;
	previous.Data = std::string{"previous"};
	CHECK(EvaluateValue(document, plan, "colours", {}, previous, diagnostic) == Status::LimitExceeded);
	CHECK(diagnostic.Port == "amount");
	CHECK(std::get<std::string>(previous.Data) == "previous");
	// Each row needs 8,650,752 comparisons; both rows together exceed the 2^24 batch cap.
	ArrayValue rows{ValueType::Colour, {}};
	rows.Nested.resize(2);
	for (auto &row : rows.Nested)
		for (size_t i = 0; i < 1024; ++i)
			row.emplace_back(Colour{uint8_t(i % 256), 0, 0, 255});
	document.Nodes[0].Values = {{"palette_in", std::move(rows)}, {"amount", int64_t{768}}};
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	CHECK(EvaluateValue(document, plan, "colours", {}, previous, diagnostic) == Status::LimitExceeded);
	CHECK(diagnostic.Port == "amount");
	CHECK(std::get<std::string>(previous.Data) == "previous");
}

TEST_CASE("Palette HSV sorting keeps fractional source getters", "[imagegraph][source_palette]") {
	const Colour first{60, 25, 10, 255}, second{64, 26, 10, 255};
	auto document = PaletteDocument(
		"pc.palette_sort",
		{{"palette_in", SourcePalette({second, first})}, {"order", EnumValue{2}}},
		"sorted_palette"
	);
	CHECK(Colours(EvaluatePalette(document)) == std::vector<Colour>{first, second});
	document.Nodes[0].Values[1].Data = EnumValue{10};
	document.Nodes[0].Values.push_back({"sort_order", std::string{"HSV"}});
	CHECK(Colours(EvaluatePalette(document)) == std::vector<Colour>{first, second});
}
TEST_CASE(
	"Palette histogram separates HSV half-up from RGB half-even channel rounding",
	"[imagegraph][source_palette]"
) {
	auto document = PaletteDocument(
		"pc.palette_shrink",
		{{"palette_in",
		  SourcePalette(
			  {{0, 0, 0, 255},
			   {20, 20, 20, 255},
			   {40, 40, 40, 255},
			   {60, 60, 60, 255},
			   {80, 80, 80, 255},
			   {100, 100, 100, 255},
			   {120, 120, 120, 255},
			   {255, 255, 255, 255}}
		  )},
		 {"amount", int64_t{7}},
		 {"algorithm", EnumValue{0}},
		 {"color_space", EnumValue{1}}}
	);
	CHECK(
		Colours(EvaluatePalette(document)) == std::vector<Colour>{
												  {0, 0, 0, 255},
												  {43, 43, 43, 255},
												  {85, 85, 85, 255},
												  {128, 128, 128, 255},
												  {170, 170, 170, 255},
												  {213, 213, 213, 255},
												  {255, 255, 255, 255}
											  }
	);
	document.Nodes[0].Values.back().Data = EnumValue{0};
	CHECK(
		Colours(EvaluatePalette(document)) == std::vector<Colour>{
												  {0, 0, 0, 255},
												  {42, 42, 42, 255},
												  {85, 85, 85, 255},
												  {128, 128, 128, 255},
												  {170, 170, 170, 255},
												  {212, 212, 212, 255},
												  {255, 255, 255, 255}
											  }
	);
}
TEST_CASE(
	"Palette copy and representative routes preserve signed packed leaves and general array identity",
	"[imagegraph][source_palette]"
) {
	ArrayValue packed{ValueType::Integer, {int64_t{-1}, int64_t{0}, int64_t{255}}};
	auto trim = PaletteDocument("pc.palette", {{"palette", packed}, {"trim_range", Vector2{0, 1}}});
	CHECK(EvaluatePalette(trim) == packed);
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(trim), restored, diagnostic) == Status::Ok);
	CHECK(EvaluatePalette(restored) == packed);
	auto sort = PaletteDocument(
		"pc.palette_sort", {{"palette_in", packed}, {"order", EnumValue{6}}}, "sorted_palette"
	);
	CHECK(EvaluatePalette(sort).Elements == std::vector<ElementValue>{int64_t{-1}, int64_t{255}, int64_t{0}});
	auto shrink = PaletteDocument("pc.palette_shrink", {{"palette_in", packed}, {"amount", int64_t{3}}});
	CHECK(EvaluatePalette(shrink) == packed);
	shrink.Nodes[0].Values[1].Data = int64_t{2};
	CHECK(EvaluatePalette(shrink).Elements == std::vector<ElementValue>{int64_t{-1}, int64_t{0}});
	ArrayValue mixed{ValueType::Any, {}};
	mixed.Items.emplace_back(ElementValue{int64_t{-1}});
	mixed.Items.emplace_back(ElementValue{Colour{0, 0, 0, 9}});
	trim.Nodes[0].Values[0].Data = mixed;
	CHECK(EvaluatePalette(trim) == mixed);
	mixed.Items[0].Data = ElementValue{int64_t{-2147483648}};
	auto replace = PaletteDocument(
		"pc.palette_replace",
		{{"palette_in", packed},
		 {"palette_from", SourcePalette({{255, 255, 255, 255}})},
		 {"palette_to", mixed},
		 {"threshold", 0.}},
		"surface_out"
	);
	const auto result = EvaluatePalette(replace);
	REQUIRE(result.Items.size() == 3);
	ArrayValue expected{ValueType::Any, {}};
	for (const auto &leaf : packed.Elements)
		expected.Items.emplace_back(leaf);
	expected.Items[0].Data = ElementValue{int64_t{-2147483648}};
	CHECK(result == expected);
}
TEST_CASE(
	"Singular palette processor rows refuse receipts without row identity", "[imagegraph][source_palette]"
) {
	ArrayValue rows{ValueType::Colour, {}};
	rows.Nested = {
		{{Colour{0, 0, 0, 255}, Colour{255, 255, 255, 255}}},
		{{Colour{255, 0, 0, 255}, Colour{0, 0, 255, 255}}}
	};
	auto document = PaletteDocument(
		"pc.palette_shrink", {{"palette_in", rows}, {"amount", int64_t{1}}, {"algorithm", EnumValue{0}}}
	);
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluatedValue result;
	result.Data = std::string{"retained"};
	CHECK(EvaluateValue(document, plan, "colours", {}, result, diagnostic) == Status::UnsupportedExecution);
	CHECK(diagnostic.Port == "amount");
	CHECK(diagnostic.Message.find("per-row") != std::string::npos);
	CHECK(std::get<std::string>(result.Data) == "retained");
}
