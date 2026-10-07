#include "../src/AtlasPayload.hpp"
#include "ComplexGeneratorFixture.hpp"
#include "NodeHarness.hpp"

#include <engine/imagegraph/Surface.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.source_weave")
using namespace engine::imagegraph;
using namespace complex_generator_test;
using imagegraph_test::NodeRun;
namespace {
	Image WeaveMap(uint8_t red, uint8_t alpha = 255) {
		return Image{1, 1, {red, red, red, alpha}, 0};
	}
	Image FloatPixel(SurfacePixel pixel) {
		Image image{1, 1, std::vector<uint8_t>(16), 0, SurfaceFormat::RGBA32Float};
		REQUIRE(StoreSurfacePixel(image, 0, 0, pixel));
		return image;
	}
	void CheckGolden(const NodeRun &run, std::array<double, 4> expected) {
		INFO(run.Port << ": " << run.Message);
		REQUIRE(run.Ok);
		SurfacePixel actual{};
		REQUIRE(LoadSurfacePixel(run.Output(), 0, 0, actual));
		for (size_t channel = 0; channel < actual.size(); ++channel)
			CHECK(actual[channel] == Catch::Approx(expected[channel]).margin(1e-5));
	}
	std::vector<std::pair<std::string_view, Value>> BaseValues() {
		return {
			{"dimension", Vector2{8, 8}},
			{"dimension_unit", EnumValue{0}},
			{"seed", 17.0},
			{"position", Vector2{.25, .25}},
			{"position_unit", EnumValue{0}},
			{"scale", Vector2{2, 2}},
			{"scale_unit", EnumValue{0}},
			{"width", Vector2{.8, .9}},
			{"shade_span", .8},
			{"shading_curved", true},
			{"attribute_color_depth", EnumValue{5}}
		};
	}
	NodeRun Weave(
		const std::vector<std::pair<std::string_view, Value>> &values,
		const std::vector<std::pair<std::string_view, const Image *>> &images,
		bool includeDefaults
	);
	NodeRun GoldenWeave(
		std::initializer_list<std::pair<std::string_view, Value>> overrides = {},
		const Image *weaveMap = nullptr,
		bool useUv = true,
		bool includeSeed = true
	) {
		const Image uvMap = FloatPixel({.23, .38, 0, .6});
		const Gradient gradient{0, {{0, Colour{20, 80, 200, 64}}, {1, Colour{230, 160, 40, 192}}}};
		Curve curve;
		curve.Header = {0, 1, 0, 0, 1, 0};
		curve.Anchors = {{0, 0, 0, 0, 1. / 3., 1. / 3.}, {-1. / 3., -1. / 3., 1, 1, 0, 0}};
		std::vector<std::pair<std::string_view, Value>> values{
			{"dimension", Vector2{8, 6}},
			{"dimension_unit", EnumValue{0}},
			{"position", Vector2{.25, -.125}},
			{"position_unit", EnumValue{0}},
			{"scale", Vector2{2, 2}},
			{"scale_unit", EnumValue{0}},
			{"angle", 0.0},
			{"seed", 17.25},
			{"width", Vector2{.8, .9}},
			{"weave_pattern", EnumValue{1}},
			{"color_type", EnumValue{1}},
			{"bg_color", Colour{13, 26, 38, 204}},
			{"color", Colour{204, 102, 51, 179}},
			{"color_2", Colour{51, 153, 230, 102}},
			{"random_color", gradient},
			{"shift", .2},
			{"shade_color", Colour{26, 51, 77, 30}},
			{"shade_span", .8},
			{"shading", .65},
			{"shading_curved", true},
			{"shading_curve", curve},
			{"uv_mix", 1.0},
			{"attribute_color_depth", EnumValue{5}}
		};
		for (const auto &[port, value] : overrides) {
			auto found = std::find_if(values.begin(), values.end(), [&](const auto &entry) {
				return entry.first == port;
			});
			REQUIRE(found != values.end());
			found->second = value;
		}
		if (!includeSeed) std::erase_if(values, [](const auto &entry) { return entry.first == "seed"; });
		std::vector<std::pair<std::string_view, const Image *>> images;
		if (useUv) images.emplace_back("uv_map", &uvMap);
		if (weaveMap) images.emplace_back("weave_map", weaveMap);
		return Weave(values, images, includeSeed);
	}
	NodeRun Weave(
		const std::vector<std::pair<std::string_view, Value>> &values = {},
		const std::vector<std::pair<std::string_view, const Image *>> &images = {},
		bool includeDefaults = true
	) {
		NodeRun run;
		const CatalogueEntry *entry = FindCatalogueEntry("pc.weave");
		const auto executor = detail::FindExecutor("pc.weave");
		if (!entry || !executor) {
			run.Message = "weave executor is unavailable";
			return run;
		}
		Node node{"node", "pc.weave", "", {}, {}};
		EvaluationRequest request;
		detail::NodeContext context(node, *entry, request);
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		for (const auto &[port, image] : images)
			context.Images.emplace_back(port, image);
		for (const CatalogueInput &input : entry->Inputs) {
			const auto given = std::find_if(values.begin(), values.end(), [&](const auto &value) {
				return value.first == input.Id;
			});
			if (given != values.end())
				context.Values.emplace_back(input.Id, given->second);
			else if (includeDefaults) {
				const auto fallback = CatalogueDefault(input);
				if (!fallback) continue;
				context.Values.emplace_back(input.Id, *fallback);
				context.CatalogueDefaultInputs.emplace_back(input.Id);
			}
		}
		for (const auto &[port, value] : context.Values)
			context.ValueViews.emplace_back(port, &value);
		context.InputProvenanceResolved = true;
		const bool ok = executor(context);
		run.Ok = ok && context.FailureCode == Status::Ok;
		run.Code = context.FailureCode;
		run.Message = context.FailureMessage;
		run.Port = context.FailurePort;
		run.Images = std::move(context.OutputImages);
		run.Values = std::move(context.OutputValues);
		return run;
	}
	Document WeaveDocument(Vector2 dimension = {8, 6}) {
		auto document = Graph("pc.weave", dimension);
		Set(document, "dimension_unit", EnumValue{0});
		Set(document, "attribute_color_depth", EnumValue{5});
		Set(document, "position", Vector2{.25, .25});
		Set(document, "position_unit", EnumValue{0});
		Set(document, "scale", Vector2{2, 2});
		Set(document, "scale_unit", EnumValue{0});
		Set(document, "width", Vector2{.8, .9});
		Set(document, "weave_pattern", EnumValue{1});
		Set(document, "color_type", EnumValue{1});
		Set(document, "shading_curved", true);
		return document;
	}
	ImageArray DrawRows(const Document &document) {
		Plan plan;
		Diagnostic diagnostic;
		REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
		ImageArray rows;
		const Status status = EvaluateArray(document, plan, "out", {}, rows, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << ": " << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		return rows;
	}
	void LinkSurface(Document &document, std::string_view port, Vector2 size) {
		document.Nodes.push_back(Solid("linked_surface", size, {255, 255, 255, 255}));
		document.Links.push_back({"linked_surface", "surface_out", "generator", std::string(port)});
	}
}

TEST_CASE(
	"Weave keeps Random Checker and mapped-axis source branches distinct", "[imagegraph][source_weave]"
) {
	const Image black = WeaveMap(0), white = WeaveMap(255);
	const Image uv = FloatPixel({.23, .38, 0, 1});
	auto base = BaseValues();
	base.emplace_back("color_type", EnumValue{1});
	base.emplace_back("color", Colour{240, 32, 16, 255});
	base.emplace_back("color_2", Colour{16, 64, 240, 255});
	const auto baseline = Weave(base);
	INFO(baseline.Port << ": " << baseline.Message);
	REQUIRE(baseline.Ok);

	base.emplace_back("weave_pattern", EnumValue{1});
	const auto checker = Weave(base);
	base.back().second = EnumValue{0};
	const auto random = Weave(base);
	base.back().second = EnumValue{2};
	const auto mappedBlack = Weave(base, {{"uv_map", &uv}, {"weave_map", &black}});
	const auto mappedWhite = Weave(base, {{"uv_map", &uv}, {"weave_map", &white}});
	for (const auto *run : {&checker, &random, &mappedBlack, &mappedWhite}) {
		INFO(run->Port << ": " << run->Message);
		REQUIRE(run->Ok);
	}
	CHECK(checker.Output().Pixels != random.Output().Pixels);
	CHECK(mappedBlack.Output().Pixels != mappedWhite.Output().Pixels);
}

TEST_CASE("Weave color modes and curve shading follow their source controls", "[imagegraph][source_weave]") {
	const Colour background{12, 34, 56, 255};
	const Colour first{240, 32, 16, 255};
	const Colour second{16, 64, 240, 255};
	auto values = BaseValues();
	values.insert(
		values.end(),
		{{"bg_color", background},
		 {"color", first},
		 {"color_2", second},
		 {"weave_pattern", EnumValue{1}},
		 {"shading", 0.0}}
	);
	const auto solid = Weave(values);
	values.emplace_back("color_type", EnumValue{1});
	const auto axis = Weave(values);
	values.back().second = EnumValue{2};
	const auto randomColor = Weave(values);
	for (const auto *run : {&solid, &axis, &randomColor}) {
		INFO(run->Port << ": " << run->Message);
		REQUIRE(run->Ok);
	}
	CHECK(solid.Output().Pixels != axis.Output().Pixels);
	CHECK(axis.Output().Pixels != randomColor.Output().Pixels);

	values.back().second = EnumValue{0};
	const auto unshaded = Weave(values);
	for (auto &value : values)
		if (value.first == "shading") value.second = 1.0;
	values.emplace_back("shade_color", Colour{0, 0, 0, 255});
	const auto shaded = Weave(values);
	REQUIRE(unshaded.Ok);
	INFO(shaded.Port << ": " << shaded.Message);
	REQUIRE(shaded.Ok);
	CHECK(unshaded.Output().Pixels != shaded.Output().Pixels);
}

TEST_CASE("Weave UV alpha is independent of UV mix and mask alpha is applied", "[imagegraph][source_weave]") {
	const Image uv = WeaveMap(0, 128), mask = WeaveMap(255, 128);
	auto values = BaseValues();
	values.insert(
		values.end(),
		{{"bg_color", Colour{80, 120, 160, 255}},
		 {"color", Colour{80, 120, 160, 255}},
		 {"color_2", Colour{80, 120, 160, 255}},
		 {"shading", 0.0},
		 {"uv_mix", 0.0}}
	);
	const auto unwarped = Weave(values, {{"uv_map", &uv}});
	for (auto &value : values)
		if (value.first == "uv_mix") value.second = 1.0;
	const auto warped = Weave(values, {{"uv_map", &uv}});
	const auto masked = Weave(values, {{"uv_map", &uv}, {"mask", &mask}});
	for (const auto *run : {&unwarped, &warped, &masked}) {
		INFO(run->Port << ": " << run->Message);
		REQUIRE(run->Ok);
	}
	CHECK(unwarped.Output().Pixels == warped.Output().Pixels);
	CHECK(masked.Output().Pixels != warped.Output().Pixels);
	SurfacePixel unwarpedPixel{}, maskedPixel{};
	REQUIRE(LoadSurfacePixel(unwarped.Output(), 0, 0, unwarpedPixel));
	REQUIRE(LoadSurfacePixel(masked.Output(), 0, 0, maskedPixel));
	CHECK(unwarpedPixel[3] == Catch::Approx(128.0 / 255.0).margin(1e-6));
	CHECK(maskedPixel[3] == Catch::Approx(64.0 / 255.0).margin(1e-6));
}

TEST_CASE("Weave keeps requested output surface formats", "[imagegraph][source_weave]") {
	for (const int64_t depth : {2, 3, 4, 5, 6, 7, 8}) {
		const auto run = Weave(
			{{"dimension", Vector2{2, 2}},
			 {"dimension_unit", EnumValue{0}},
			 {"shading_curved", true},
			 {"attribute_color_depth", EnumValue{depth}}}
		);
		INFO(run.Port << ": " << run.Message);
		REQUIRE(run.Ok);
		CHECK(run.Output().Format == *SourceSurfaceFormat(depth));
		CHECK(ValidSurfaceLayout(run.Output(), Limits::MaximumDimension, Limits::MaximumOutputBytes));
	}
}

TEST_CASE("Weave refuses a curve larger than the shader uniform", "[imagegraph][source_weave]") {
	Curve curve;
	curve.Header = {0, 1, 0, 0, 0, 1};
	for (size_t i = 0; i < 10; ++i)
		curve.Anchors.push_back({0, 0, double(i) / 9, double(i) / 9, 0, 0});
	auto values = BaseValues();
	values.emplace_back("shading_curve", curve);
	const auto run = Weave(values);
	CHECK_FALSE(run.Ok);
	CHECK(run.Code == Status::UnsupportedExecution);
	CHECK(run.Port == "shading_curve");
}

TEST_CASE("Weave names unresolved source branches and consumed divisors", "[imagegraph][source_weave]") {
	auto missingMapValues = BaseValues();
	missingMapValues.emplace_back("weave_pattern", EnumValue{2});
	const auto missingMap = Weave(missingMapValues);
	CHECK_FALSE(missingMap.Ok);
	CHECK(missingMap.Code == Status::UnsupportedExecution);
	CHECK(missingMap.Port == "weave_map");

	auto missingSeedValues = BaseValues();
	std::erase_if(missingSeedValues, [](const auto &entry) { return entry.first == "seed"; });
	missingSeedValues.emplace_back("weave_pattern", EnumValue{0});
	const auto missingSeed = Weave(missingSeedValues, {}, false);
	CHECK_FALSE(missingSeed.Ok);
	CHECK(missingSeed.Code == Status::UnsupportedExecution);
	CHECK(missingSeed.Port == "seed");

	auto uncurvedValues = BaseValues();
	for (auto &value : uncurvedValues)
		if (value.first == "shading_curved") value.second = false;
	const auto uncurved = Weave(uncurvedValues);
	CHECK_FALSE(uncurved.Ok);
	CHECK(uncurved.Code == Status::UnsupportedExecution);
	CHECK(uncurved.Port == "shading_curved");

	auto zeroScaleValues = BaseValues();
	for (auto &value : zeroScaleValues)
		if (value.first == "scale") value.second = Vector2{0, 2};
	const auto zeroScale = Weave(zeroScaleValues);
	CHECK_FALSE(zeroScale.Ok);
	CHECK(zeroScale.Code == Status::UnsupportedExecution);
	CHECK(zeroScale.Port == "scale");

	const Image map = FloatPixel({.9, 0, 0, .8});
	const auto outsideMap = GoldenWeave(
		{{"scale", Vector2{16, 12}}, {"width", Vector2{1.4, 1.4}}, {"weave_pattern", EnumValue{2}}}, &map
	);
	CHECK_FALSE(outsideMap.Ok);
	CHECK(outsideMap.Code == Status::UnsupportedExecution);
	CHECK(outsideMap.Port == "weave_map");
	CHECK(outsideMap.Images.empty());

	AtlasValue atlas;
	auto &atlasData = atlas.Data.emplace();
	atlasData.Kind = AtlasKind::SurfaceAtlas;
	atlasData.Surface.Data = {1, 1, {64, 128, 192, 255}, 0};
	atlasData.Scale = {1, 1};
	atlasData.Dimension = {1, 1};
	REQUIRE(detail::ValidAtlasPayload(atlas));
	for (const std::string_view port : {"uv_map", "mask", "weave_map"}) {
		auto values = BaseValues();
		values.emplace_back(port, atlas);
		values.emplace_back("weave_pattern", EnumValue{2});
		const auto rawAtlas = Weave(values);
		CHECK_FALSE(rawAtlas.Ok);
		CHECK(rawAtlas.Code == Status::UnsupportedExecution);
		CHECK(rawAtlas.Port == port);
		CHECK(rawAtlas.Images.empty());
	}

	const auto nonfiniteMapCoordinate = GoldenWeave(
		{{"dimension", Vector2{1, 1}},
		 {"position", Vector2{-3e38, -1.5e38}},
		 {"scale", Vector2{2e38, 3e38}},
		 {"width", Vector2{1.4, 1.4}},
		 {"weave_pattern", EnumValue{2}}},
		&map
	);
	CHECK_FALSE(nonfiniteMapCoordinate.Ok);
	CHECK(nonfiniteMapCoordinate.Code == Status::InvalidValue);
	CHECK(nonfiniteMapCoordinate.Port == "weave_map");
	CHECK(nonfiniteMapCoordinate.Images.empty());
}

TEST_CASE("Weave matches independent float shader samples", "[imagegraph][source_weave]") {
	CheckGolden(GoldenWeave(), {.1594888121, .4347143769, .6540323496, .2400000095});
	CheckGolden(
		GoldenWeave({{"color_type", EnumValue{0}}}), {.5115603805, .3173571825, .2421316355, .4211764932}
	);
	CheckGolden(
		GoldenWeave({{"weave_pattern", EnumValue{0}}}), {.1594888121, .4347143769, .6540323496, .2400000095}
	);
	CheckGolden(
		GoldenWeave({{"weave_pattern", EnumValue{0}}, {"seed", -17.25}}),
		{.6922401786, .3691250086, .2157402039, .4211764932}
	);
	CheckGolden(
		GoldenWeave({{"weave_pattern", EnumValue{0}}, {"seed", 17.75}}),
		{.1594888121, .4347143769, .6540323496, .2400000095}
	);

	constexpr std::array<std::array<double, 4>, 7> gradientSamples{
		{{{.4781593680, .4153059721, .2878518105, .3936590254}},
		 {{.0881540626, .2667325139, .5849987268, .1505882442}},
		 {{.5580709577, .1908379644, .2071457505, .3936590254}},
		 {{.4790798724, .4287271499, .3546311259, .3936590254}},
		 {{.5224987864, .4244322777, .3542556167, .3936590254}},
		 {{.4639278650, .5985821486, .2071457505, .3936590254}},
		 {{.4684852362, .4121279716, .2967314422, .3936590254}}}
	};
	for (int64_t mode = 0; mode < int64_t(gradientSamples.size()); ++mode) {
		Gradient gradient{uint8_t(mode), {{0, Colour{20, 80, 200, 64}}, {1, Colour{230, 160, 40, 192}}}};
		CheckGolden(
			GoldenWeave({{"color_type", EnumValue{2}}, {"random_color", gradient}}),
			gradientSamples[size_t(mode)]
		);
	}

	CheckGolden(
		GoldenWeave(
			{{"width", Vector2{0, 0}},
			 {"weave_pattern", EnumValue{2}},
			 {"color_type", EnumValue{2}},
			 {"shading_curved", false}},
			nullptr,
			true,
			false
		),
		{.0509803928, .1019607857, .1490196139, .4800000191}
	);
	CheckGolden(GoldenWeave({{"uv_mix", 0.0}}), {.6014951468, .3431250155, .2289950997, .4211764932});
	CheckGolden(GoldenWeave({{"angle", 37.0}}), {.5643610358, .3324854672, .2344191819, .4211764932});
	CheckGolden(
		GoldenWeave({{"scale", Vector2{-2, 2}}}), {.69224011898, .3691249788, .2157402039, .4211764932}
	);
	const auto fractional = GoldenWeave(
		{{"dimension", Vector2{3.6, 2.6}}, {"position", Vector2{.125, .25}}, {"scale", Vector2{1.2, .8}}},
		nullptr,
		false
	);
	INFO(fractional.Port << ": " << fractional.Message);
	REQUIRE(fractional.Ok);
	CHECK(fractional.Output().Width == 4);
	CHECK(fractional.Output().Height == 3);
	SurfacePixel fractionalPixel{};
	REQUIRE(LoadSurfacePixel(fractional.Output(), 1, 1, fractionalPixel));
	for (size_t channel = 0; channel < fractionalPixel.size(); ++channel)
		CHECK(
			fractionalPixel[channel] ==
			Catch::Approx(std::array{.6251266599, .3498958349, .2255432904, .7019608021}[channel])
				.margin(1e-5)
		);

	const Image horizontal = FloatPixel({.1, .8, 0, .8});
	const Image vertical = FloatPixel({.9, .8, 0, .8});
	const Image equalThreshold = FloatPixel({.625, .8, 0, .8});
	CheckGolden(
		GoldenWeave({{"weave_pattern", EnumValue{2}}}, &horizontal),
		{.8000000119, .4000000059, .2000000030, .4211764932}
	);
	CheckGolden(
		GoldenWeave({{"weave_pattern", EnumValue{2}}}, &vertical),
		{.2000000030, .6000000238, .9019607902, .2400000095}
	);
	CheckGolden(
		GoldenWeave({{"weave_pattern", EnumValue{2}}}, &equalThreshold),
		{.8000000119, .4000000059, .2000000030, .4211764932}
	);
	CheckGolden(
		GoldenWeave({{"width", Vector2{1.4, 1.2}}}, nullptr),
		{.1362745166, .3400000334, .5119608045, .2400000095}
	);
	CheckGolden(GoldenWeave({{"shading", -.25}}), {.2155812383, .6635714769, .9973179102, .2400000095});

	Curve range;
	range.Header = {.1, .8, 0, .2, .9, 0};
	range.Anchors = {{0, 0, 0, 0, 1. / 3., 1. / 3.}, {-1. / 3., -1. / 3., 1, 1, 0, 0}};
	CheckGolden(
		GoldenWeave({{"shading_curve", range}}), {.1648713350, .4566751122, .6869734526, .2400000095}
	);
	Curve step;
	step.Header = {0, 1, 1, 0, 1, 0};
	step.Anchors = {{0, 0, 0, .2, 0, 0}, {0, 0, .5, .7, 0, 0}, {0, 0, 1, .9, 0, 0}};
	CheckGolden(GoldenWeave({{"shading_curve", step}}), {.1490196139, .3920000196, .5899608135, .2400000095});
	Curve bezier;
	bezier.Header = {0, 1, 0, 0, 1, 0};
	bezier.Anchors = {{0, 0, 0, 0, .2, .8}, {-.25, -.1, 1, 1, 0, 0}};
	CheckGolden(
		GoldenWeave({{"shading_curve", bezier}}), {.1787676662, .5133721232, .7720189095, .2400000095}
	);
}

TEST_CASE("Weave projects linked surface values and surface arrays", "[imagegraph][source_weave]") {
	for (const std::string_view port : {"position", "scale", "width"}) {
		auto linked = WeaveDocument();
		LinkSurface(linked, port, {3, 2});
		auto direct = WeaveDocument();
		Set(direct, std::string(port), Vector2{3, 2});
		if (port != "width") Set(direct, std::string(port) + "_unit", EnumValue{0});
		CHECK(Draw(linked) == Draw(direct));
	}

	for (const std::string_view port : {"position", "scale"}) {
		auto reference = WeaveDocument();
		Set(reference, std::string(port), Vector2{.25, .5});
		Set(reference, std::string(port) + "_unit", EnumValue{1});
		auto pixels = WeaveDocument();
		Set(pixels, std::string(port), Vector2{2, 3});
		CHECK(Draw(reference) == Draw(pixels));

		reference.Nodes.push_back(Array("dimensions", ValueType::Vector2, Vector2{4, 3}, Vector2{8, 6}));
		reference.Links.push_back({"dimensions", "array", "generator", "dimension"});
		pixels.Nodes.push_back(Array("dimensions", ValueType::Vector2, Vector2{4, 3}, Vector2{8, 6}));
		pixels.Links.push_back({"dimensions", "array", "generator", "dimension"});
		Set(pixels, std::string(port), Vector2{1, 1.5});
		const auto referenceRows = DrawRows(reference), pixelRows = DrawRows(pixels);
		REQUIRE(referenceRows.Images.size() == 2);
		CHECK(referenceRows.Images == pixelRows.Images);
	}

	auto scalarSurfaceArray = WeaveDocument();
	SurfaceRows(scalarSurfaceArray, "position");
	auto directOne = WeaveDocument();
	Set(directOne, "position", Vector2{1, 1});
	Set(directOne, "position_unit", EnumValue{0});
	CHECK(Draw(scalarSurfaceArray) == Draw(directOne));

	for (const std::string_view port : {"uv_mix", "shading", "shade_span", "shift"}) {
		auto linked = WeaveDocument();
		LinkSurface(linked, port, {2, 1});
		if (port == "uv_mix") {
			linked.Nodes.push_back(Solid("uv_surface", {1, 1}, {64, 191, 128, 128}));
			linked.Links.push_back({"uv_surface", "surface_out", "generator", "uv_map"});
		}
		if (port == "shift") Set(linked, "color_type", EnumValue{2});
		const auto rows = DrawRows(linked);
		REQUIRE(rows.Images.size() == 2);
		for (size_t row = 0; row < rows.Images.size(); ++row) {
			auto direct = WeaveDocument();
			Set(direct, std::string(port), row == 0 ? 2.0 : 1.0);
			if (port == "uv_mix") {
				direct.Nodes.push_back(Solid("uv_surface", {1, 1}, {64, 191, 128, 128}));
				direct.Links.push_back({"uv_surface", "surface_out", "generator", "uv_map"});
			}
			if (port == "shift") Set(direct, "color_type", EnumValue{2});
			CHECK(rows.Images[row] == Draw(direct));
		}
	}

	auto varyingDimensions = WeaveDocument();
	Set(varyingDimensions, "scale", Vector2{1, .5});
	SurfaceRows(varyingDimensions, "dimension");
	const auto varying = DrawRows(varyingDimensions);
	REQUIRE(varying.Images.size() == 2);
	CHECK(varying.Images[0].Width == 2);
	CHECK(varying.Images[0].Height == 1);
	CHECK(varying.Images[1].Width == 3);
	CHECK(varying.Images[1].Height == 2);

	auto equalDimensions = WeaveDocument();
	Set(equalDimensions, "scale", Vector2{1, .5});
	equalDimensions.Nodes.push_back(Solid("first", {2, 1}, {255, 255, 255, 255}));
	equalDimensions.Nodes.push_back(Solid("second", {2, 1}, {255, 255, 255, 255}));
	Node equalSurfaces{"surfaces", "value.array", "", {}, {}};
	equalSurfaces.DynamicInputs = {
		{"first", ValueType::Image, std::nullopt}, {"second", ValueType::Image, std::nullopt}
	};
	equalDimensions.Nodes.push_back(std::move(equalSurfaces));
	equalDimensions.Links.push_back({"first", "surface_out", "surfaces", "first"});
	equalDimensions.Links.push_back({"second", "surface_out", "surfaces", "second"});
	equalDimensions.Links.push_back({"surfaces", "array", "generator", "dimension"});
	auto directDimension = WeaveDocument();
	Set(directDimension, "scale", Vector2{1, .5});
	Set(directDimension, "dimension", Vector2{2, 1});
	Set(directDimension, "dimension_unit", EnumValue{0});
	CHECK(Draw(equalDimensions) == Draw(directDimension));
}
