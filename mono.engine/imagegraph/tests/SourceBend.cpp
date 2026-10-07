#include "../src/ProcessorBatch.hpp"
#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.source_bend")
using namespace engine::imagegraph;
using imagegraph_test::RunNode;
namespace {
	Image
	Coordinates(uint32_t width = 4, uint32_t height = 4, SurfaceFormat format = SurfaceFormat::RGBA8Unorm) {
		const auto layout = CheckedSurfaceLayout(width, height, format, Limits::MaximumEvaluationBytes);
		if (!layout) return {};
		Image image{width, height, std::vector<uint8_t>(size_t(layout->Bytes)), 0, format};
		for (uint32_t y = 0; y < height; ++y)
			for (uint32_t x = 0; x < width; ++x)
				if (!StoreSurfacePixel(
						image,
						x,
						y,
						{double(x + 1) / width,
						 double(y + 1) / height,
						 double(x + y + 1) / (width + height),
						 .5}
					))
					return {};
		return image;
	}
	Document Graph() {
		Document document;
		document.FormatVersion = 9;
		document.Project = ProjectSettings{};
		document.Nodes = {
			{"source", "image.captured", "", {}, {{"source_id", std::string("source")}}},
			{"bend",
			 "pc.bend",
			 "",
			 {},
			 {{"dimension_type", EnumValue{0}},
			  {"dimension", Vector2{4, 4}},
			  {"dimension_unit", EnumValue{0}}}}
		};
		document.Links = {{"source", "image", "bend", "surface_in"}};
		document.Outputs = {{"out", "bend", "surface_out"}};
		return document;
	}
	AtlasValue SurfaceAtlas(Image source) {
		AtlasValue atlas;
		auto &data = atlas.Data.emplace();
		data.Kind = AtlasKind::SurfaceAtlas;
		data.Surface.Data = std::move(source);
		data.Dimension = {double(data.Surface.Data.Width), double(data.Surface.Data.Height)};
		return atlas;
	}
	Plan Compiled(const Document &document) {
		Plan plan;
		Diagnostic diagnostic;
		const auto status = Compile(document, plan, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
		return plan;
	}
	Image EvaluateOne(const Document &document, const Plan &plan, const Image &source, uint64_t tick = 0) {
		const std::array<RequestImageSource, 1> sources{{{"source", source}}};
		EvaluationRequest request;
		request.Tick = tick;
		request.ImageSources = sources;
		Image output;
		Diagnostic diagnostic;
		const auto status = Evaluate(document, plan, "out", request, output, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << ": " << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		return output;
	}
}

TEST_CASE("Bend zero amount is identity for Arc and Wave on either axis", "[source_2d][bend]") {
	const auto source = Coordinates();
	for (int64_t type = 0; type <= 1; ++type)
		for (int64_t axis = 0; axis <= 1; ++axis) {
			auto run = RunNode(
				"pc.bend",
				{{"surface_in", &source}},
				{{"type", EnumValue{type}},
				 {"axis", EnumValue{axis}},
				 {"amount", 0.},
				 {"dimension_type", EnumValue{0}},
				 {"dimension", Vector2{4, 4}},
				 {"dimension_unit", EnumValue{0}}}
			);
			INFO("type " << type << ", axis " << axis << ": " << run.Message);
			REQUIRE(run.Ok);
			CHECK(run.Output().Width == 4);
			CHECK(run.Output().Height == 4);
			CHECK(run.Output() == source);
		}
	auto fractionalArcAxis = RunNode(
		"pc.bend",
		{{"surface_in", &source}},
		{{"type", EnumValue{0}},
		 {"axis", .5},
		 {"amount", 0.},
		 {"dimension", Vector2{4, 4}},
		 {"dimension_unit", EnumValue{0}}}
	);
	REQUIRE(fractionalArcAxis.Ok);
	CHECK(fractionalArcAxis.Output() == source);
}

TEST_CASE(
	"Bend source grids cap at their axis limits and keep nearest sampling stable", "[source_2d][bend]"
) {
	const auto source = Coordinates(130, 34);
	for (int64_t axis : {0, 1}) {
		auto run = RunNode(
			"pc.bend",
			{{"surface_in", &source}},
			{{"type", EnumValue{1}}, {"axis", EnumValue{axis}}, {"amount", 0.}, {"interpolate", EnumValue{1}}}
		);
		INFO("axis " << axis << ": " << run.Message);
		REQUIRE(run.Ok);
		CHECK(run.Output().Width == source.Width);
		CHECK(run.Output().Height == source.Height);
		CHECK(run.Output() == source);
	}
}

TEST_CASE("Bend Arc and Wave displace both axes in both directions", "[source_2d][bend]") {
	const auto source = Coordinates(6, 6);
	for (int64_t type = 0; type <= 1; ++type)
		for (int64_t axis = 0; axis <= 1; ++axis) {
			const auto run = [&](double amount) {
				return RunNode(
					"pc.bend",
					{{"surface_in", &source}},
					{{"type", EnumValue{type}},
					 {"axis", EnumValue{axis}},
					 {"amount", amount},
					 {"scale", 1.},
					 {"shift", 0.},
					 {"dimension_type", EnumValue{0}},
					 {"dimension", Vector2{6, 6}},
					 {"dimension_unit", EnumValue{0}}}
				);
			};
			auto positive = run(.5), negative = run(-.5);
			INFO("type " << type << ", axis " << axis << ": " << positive.Message);
			REQUIRE(positive.Ok);
			REQUIRE(negative.Ok);
			CHECK(positive.Output() != source);
			CHECK(negative.Output() != source);
			CHECK(positive.Output() != negative.Output());
		}
	const auto fractionalAxis = RunNode(
		"pc.bend",
		{{"surface_in", &source}},
		{{"type", EnumValue{1}},
		 {"axis", .5},
		 {"amount", .5},
		 {"scale", 1.},
		 {"dimension", Vector2{6, 6}},
		 {"dimension_unit", EnumValue{0}}}
	);
	const auto yAxis = RunNode(
		"pc.bend",
		{{"surface_in", &source}},
		{{"type", EnumValue{1}},
		 {"axis", EnumValue{1}},
		 {"amount", .5},
		 {"scale", 1.},
		 {"dimension", Vector2{6, 6}},
		 {"dimension_unit", EnumValue{0}}}
	);
	REQUIRE(fractionalAxis.Ok);
	REQUIRE(yAxis.Ok);
	CHECK(fractionalAxis.Output() == yAxis.Output());
}

TEST_CASE(
	"Bend fixed output rounds dimensions evenly and applies keep ratio and centered scale",
	"[source_2d][bend]"
) {
	const auto source = Coordinates(4, 4);
	const auto draw = [&](bool keepRatio, Vector2 scale) {
		return RunNode(
			"pc.bend",
			{{"surface_in", &source}},
			{{"type", EnumValue{1}},
			 {"axis", EnumValue{0}},
			 {"amount", .5},
			 {"scale", 1.},
			 {"dimension_type", EnumValue{0}},
			 {"dimension", Vector2{8, 6}},
			 {"dimension_unit", EnumValue{0}},
			 {"keep_ratio", keepRatio},
			 {"scale_2", scale}}
		);
	};
	auto fitted = draw(true, {1, 1});
	auto stretched = draw(false, {1, 1});
	auto mirrored = draw(false, {-1, 1});
	auto collapsed = draw(false, {0, 0});
	for (const auto *run : {&fitted, &stretched, &mirrored, &collapsed}) {
		INFO(run->Message);
		REQUIRE(run->Ok);
		CHECK(run->Output().Width == 8);
		CHECK(run->Output().Height == 6);
	}
	CHECK(fitted.Output() != stretched.Output());
	CHECK(stretched.Output() != mirrored.Output());
	size_t collapsedAlpha = 0;
	for (size_t p = 3; p < collapsed.Output().Pixels.size(); p += 4)
		collapsedAlpha += collapsed.Output().Pixels[p] != 0;
	CHECK(collapsedAlpha == 0);
	for (uint32_t y = 0; y < stretched.Output().Height; ++y)
		for (uint32_t x = 0; x < stretched.Output().Width / 2; ++x) {
			const size_t left = (size_t(y) * stretched.Output().Width + x) * 4;
			const size_t right =
				(size_t(y) * stretched.Output().Width + (stretched.Output().Width - 1 - x)) * 4;
			for (size_t c = 0; c < 4; ++c)
				CHECK(mirrored.Output().Pixels[left + c] == stretched.Output().Pixels[right + c]);
		}
	auto halfEven = RunNode(
		"pc.bend",
		{{"surface_in", &source}},
		{{"amount", 0.},
		 {"dimension_type", EnumValue{0}},
		 {"dimension", Vector2{7.5, 5.5}},
		 {"dimension_unit", EnumValue{0}}}
	);
	REQUIRE(halfEven.Ok);
	CHECK(halfEven.Output().Width == 8);
	CHECK(halfEven.Output().Height == 6);
}

TEST_CASE(
	"Bend applies UV mapping and each supported sampler with output reference dimensions", "[source_2d][bend]"
) {
	const auto source = Coordinates(4, 4);
	const auto mapped = [&](int64_t interpolation, Vector2 uvScale, Vector2 uvShift, int64_t oversample = 4) {
		return RunNode(
			"pc.bend",
			{{"surface_in", &source}},
			{{"amount", 0.},
			 {"dimension_type", EnumValue{0}},
			 {"dimension", Vector2{4, 4}},
			 {"dimension_unit", EnumValue{0}},
			 {"uv_scale", uvScale},
			 {"uv_shift", uvShift},
			 {"interpolate", EnumValue{interpolation}},
			 {"oversample", EnumValue{oversample}}}
		);
	};
	auto base = mapped(1, {1, 1}, {0, 0});
	REQUIRE(base.Ok);
	for (int64_t interpolation : {1, 2, 3, 4}) {
		auto filtered = mapped(interpolation, {1.5, 1.25}, {.25, .125});
		INFO(filtered.Message);
		REQUIRE(filtered.Ok);
		CHECK(filtered.Output().Width == 4);
		CHECK(filtered.Output().Height == 4);
		CHECK(filtered.Output() != base.Output());
	}
	auto repeated = mapped(1, {1, 1}, {.25, 0}, 4);
	auto empty = mapped(1, {1, 1}, {.25, 0}, 1);
	REQUIRE(repeated.Ok);
	REQUIRE(empty.Ok);
	CHECK(repeated.Output() != empty.Output());
	const auto zeroFrequency = RunNode(
		"pc.bend",
		{{"surface_in", &source}},
		{{"type", EnumValue{1}},
		 {"axis", EnumValue{0}},
		 {"amount", .5},
		 {"scale", 0.},
		 {"shift", .25},
		 {"dimension_type", EnumValue{0}},
		 {"dimension", Vector2{4, 4}},
		 {"dimension_unit", EnumValue{0}}}
	);
	REQUIRE(zeroFrequency.Ok);
	CHECK(zeroFrequency.Output() == source);
}

TEST_CASE("Bend bicubic sampling uses resized output dimensions as its reference", "[source_2d][bend]") {
	Image source{4, 4, std::vector<uint8_t>(4 * 4 * 4), 0};
	for (uint32_t y = 0; y < 4; ++y)
		for (uint32_t x = 0; x < 4; ++x) {
			const size_t p = (size_t(y) * 4 + x) * 4;
			source.Pixels[p] = std::array<uint8_t, 4>{0, 64, 128, 192}[x];
			source.Pixels[p + 3] = 255;
		}
	auto resized = RunNode(
		"pc.bend",
		{{"surface_in", &source}},
		{{"amount", 0.},
		 {"dimension_type", EnumValue{0}},
		 {"dimension", Vector2{8, 4}},
		 {"dimension_unit", EnumValue{0}},
		 {"keep_ratio", false},
		 {"interpolate", EnumValue{3}}}
	);
	INFO(resized.Message);
	REQUIRE(resized.Ok);
	CHECK(resized.Output().Width == 8);
	CHECK(resized.Output().Height == 4);
	CHECK(resized.Output().Pixels[4] == 16);
}

TEST_CASE(
	"Bend active rendering converts seven source formats and keeps raw RGB and alpha", "[source_2d][bend]"
) {
	constexpr std::array formats{
		SurfaceFormat::RGBA8Unorm,
		SurfaceFormat::RGBA4Unorm,
		SurfaceFormat::RGBA16Float,
		SurfaceFormat::RGBA32Float,
		SurfaceFormat::R8Unorm,
		SurfaceFormat::R16Float,
		SurfaceFormat::R32Float
	};
	for (SurfaceFormat format : formats) {
		auto source = Coordinates(4, 4, format);
		auto run = RunNode(
			"pc.bend",
			{{"surface_in", &source}},
			{{"amount", 0.},
			 {"dimension_type", EnumValue{0}},
			 {"dimension", Vector2{4, 4}},
			 {"dimension_unit", EnumValue{0}}}
		);
		INFO("format " << unsigned(format) << ": " << run.Message);
		REQUIRE(run.Ok);
		CHECK(run.Output().Format == SurfaceFormat::RGBA8Unorm);
		SurfacePixel pixel{};
		REQUIRE(LoadSurfacePixel(run.Output(), 1, 1, pixel));
		if (DescribeSurfaceFormat(format)->Channels == 1) {
			CHECK(pixel[1] == 0);
			CHECK(pixel[2] == 0);
			CHECK(pixel[3] == 1);
			CHECK(pixel[0] > 0);
		} else {
			SurfacePixel original{};
			REQUIRE(LoadSurfacePixel(source, 1, 1, original));
			CHECK(pixel[0] == Catch::Approx(original[0]).margin(1. / 255));
			CHECK(pixel[1] == Catch::Approx(original[1]).margin(1. / 255));
			CHECK(pixel[2] == Catch::Approx(original[2]).margin(1. / 255));
			CHECK(pixel[3] == Catch::Approx(original[3]).margin(1. / 255));
		}
	}
	Image translucent{4, 4, std::vector<uint8_t>(4 * 4 * 4), 0};
	for (size_t p = 0; p < translucent.Pixels.size(); p += 4)
		for (size_t c = 0; c < 4; ++c)
			translucent.Pixels[p + c] = std::array<uint8_t, 4>{80, 120, 200, 128}[c];
	auto blended = RunNode(
		"pc.bend",
		{{"surface_in", &translucent}},
		{{"amount", 0.},
		 {"dimension_type", EnumValue{0}},
		 {"dimension", Vector2{4, 4}},
		 {"dimension_unit", EnumValue{0}}}
	);
	REQUIRE(blended.Ok);
	const size_t center = (size_t(2) * 4 + 2) * 4;
	CHECK(blended.Output().Pixels[center] == 80);
	CHECK(blended.Output().Pixels[center + 1] == 120);
	CHECK(blended.Output().Pixels[center + 2] == 200);
	CHECK(blended.Output().Pixels[center + 3] == 128);
}

TEST_CASE(
	"Bend inactive copies malformed controls while active rejects bad surfaces and modes", "[source_2d][bend]"
) {
	Image source{2, 2, {10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120, 130, 140, 150, 160}, 0};
	auto inactive = RunNode(
		"pc.bend",
		{{"surface_in", &source}},
		{{"active", false},
		 {"type", EnumValue{77}},
		 {"axis", EnumValue{77}},
		 {"amount", std::numeric_limits<double>::quiet_NaN()},
		 {"dimension", Vector2{0, 0}},
		 {"dimension_type", EnumValue{77}}}
	);
	REQUIRE(inactive.Ok);
	CHECK(inactive.Output() == source);
	auto missing = RunNode("pc.bend", {});
	CHECK(missing.Code == Status::InvalidValue);
	CHECK(missing.Port == "surface_in");
	// grug source cannot build its grid with a one-pixel edge.
	auto oneWide = Coordinates(1, 4);
	auto narrow = RunNode("pc.bend", {{"surface_in", &oneWide}}, {{"amount", .5}});
	CHECK_FALSE(narrow.Ok);
	CHECK(narrow.Port == "surface_in");
	CHECK(narrow.Message.find("grid") != std::string::npos);
	Image malformed{4, 4, std::vector<uint8_t>(3), 0};
	auto invalid = RunNode("pc.bend", {{"surface_in", &malformed}});
	CHECK_FALSE(invalid.Ok);
	CHECK(invalid.Port == "surface_in");
	for (const auto &[port, wrong] :
		 std::array<std::pair<std::string_view, Value>, 2>{{{"type", 0.5}, {"dimension_type", 0.5}}}) {
		auto refused = RunNode("pc.bend", {{"surface_in", &source}}, {{port, wrong}});
		CHECK_FALSE(refused.Ok);
		CHECK(refused.Port == port);
	}
}

TEST_CASE("Bend refuses Atlas textures while inactive Atlas copies retain pixels", "[source_2d][bend]") {
	const auto source = Coordinates();
	const auto atlas = SurfaceAtlas(source);
	auto active = RunNode("pc.bend", {}, {{"surface_in", atlas}});
	CHECK_FALSE(active.Ok);
	CHECK(active.Code == Status::UnsupportedExecution);
	CHECK(active.Port == "surface_in");
	auto inactive = RunNode("pc.bend", {}, {{"surface_in", atlas}, {"active", false}});
	INFO(inactive.Message);
	REQUIRE(inactive.Ok);
	CHECK(inactive.Output().Pixels == source.Pixels);
	CHECK(inactive.Output().Format == source.Format);
}

TEST_CASE(
	"Bend dimensions use pixel or project units and captured animation survives an instance save",
	"[source_2d][bend]"
) {
	const auto source = Coordinates();
	auto pixels = Graph();
	pixels.Project->SurfaceWidth = 6;
	pixels.Project->SurfaceHeight = 4;
	pixels.Nodes[1].Values[1].Data = Vector2{6, 4};
	auto project = pixels;
	project.Nodes[1].Values[1].Data = Vector2{1, 1};
	project.Nodes[1].Values[2].Data = EnumValue{1};
	Document restoredProject;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(project), restoredProject, diagnostic) == Status::Ok);
	CHECK(restoredProject == project);
	auto pixelPlan = Compiled(pixels);
	auto projectPlan = Compiled(restoredProject);
	CHECK(EvaluateOne(pixels, pixelPlan, source) == EvaluateOne(restoredProject, projectPlan, source));

	auto animated = Graph();
	animated.Nodes[1].SourceAnimatedInputs = {"amount"};
	animated.Nodes.push_back({"instance", "pc.bend", "", {}, {}});
	animated.Nodes.back().InstanceBase = "bend";
	animated.Outputs = {{"out", "instance", "surface_out"}};
	animated.Keyframes = {{"bend", "amount", 0, 0.}, {"bend", "amount", 1, .5}};
	Document restored;
	REQUIRE(Read(Write(animated), restored, diagnostic) == Status::Ok);
	CHECK(restored == animated);
	auto plan = Compiled(restored);
	const auto first = EvaluateOne(restored, plan, source, 0);
	const auto later = EvaluateOne(restored, plan, source, 1);
	CHECK(first.Width == source.Width);
	CHECK(first.Height == source.Height);
	CHECK(first.Format == source.Format);
	CHECK(first.Pixels == source.Pixels);
	CHECK(first.Hash == SurfaceHash(first));
	CHECK(later != first);
}

TEST_CASE("Bend processor amount arrays keep each source row distinct", "[source_2d][bend]") {
	const auto source = Coordinates();
	auto document = Graph();
	Node amounts{"amounts", "pc.array", "", {}, {}, {}};
	amounts.DynamicInputs = {{"input_0", ValueType::Scalar, 0.}, {"input_1", ValueType::Scalar, .5}};
	document.Nodes.push_back(amounts);
	document.Links.push_back({"amounts", "array", "bend", "amount"});
	auto plan = Compiled(document);
	const std::array<RequestImageSource, 1> sources{{{"source", source}}};
	EvaluationRequest request;
	request.ImageSources = sources;
	ImageArray outputs;
	Diagnostic diagnostic;
	const auto status = EvaluateArray(document, plan, "out", request, outputs, diagnostic);
	INFO(diagnostic.NodeId << ":" << diagnostic.Port << ": " << diagnostic.Message);
	REQUIRE(status == Status::Ok);
	REQUIRE(outputs.Images.size() == 2);
	CHECK(outputs.Images[0].Width == source.Width);
	CHECK(outputs.Images[0].Height == source.Height);
	CHECK(outputs.Images[0].Pixels == source.Pixels);
	CHECK(outputs.Images[0].Hash == SurfaceHash(outputs.Images[0]));
	CHECK(outputs.Images[1] != outputs.Images[0]);
}

TEST_CASE("Bend refuses full-array work before publishing any row", "[source_2d][bend]") {
	const auto source = Coordinates();
	auto document = Graph();
	document.Nodes[1].Values[1].Data = Vector2{100, 100};
	ArrayValue amounts;
	amounts.ElementType = ValueType::Scalar;
	amounts.Elements = std::vector<ElementValue>(Limits::MaximumArrayElements, ElementValue{.5});
	document.Junctions = {{"amounts", "", ValueType::Array, amounts}};
	document.Links.push_back({"amounts", "value", "bend", "amount"});
	auto plan = Compiled(document);
	const std::array<RequestImageSource, 1> sources{{{"source", source}}};
	EvaluationRequest request;
	request.ImageSources = sources;
	ImageArray outputs;
	outputs.Images = {Image{1, 1, {9, 8, 7, 6}, 0}};
	const auto prior = outputs;
	Diagnostic diagnostic;
	const auto status = EvaluateArray(document, plan, "out", request, outputs, diagnostic);
	CHECK(status == Status::LimitExceeded);
	CHECK(diagnostic.NodeId == "bend");
	CHECK(diagnostic.Message.find("complete batch") != std::string::npos);
	CHECK(outputs.Images == prior.Images);
	CHECK(outputs.Items == prior.Items);
}

TEST_CASE("Bend workspace refusal happens before it publishes pixels", "[source_2d][bend]") {
	const auto source = Coordinates();
	const auto *entry = FindCatalogueEntry("pc.bend");
	REQUIRE(entry != nullptr);
	Node node{"bend", "pc.bend", "", {}, {}};
	EvaluationRequest request;
	detail::NodeContext context(node, *entry, request);
	context.ByteBudget = 1;
	context.Images = {{"surface_in", &source}};
	context.InputProvenanceResolved = true;
	const auto execute = detail::FindExecutor("pc.bend");
	REQUIRE(execute != nullptr);
	CHECK_FALSE(execute(context));
	CHECK(context.FailureCode == Status::LimitExceeded);
	CHECK(context.OutputImages.empty());
}

TEST_CASE("Bend Arc keeps its hand-derived half-circle band and empty center", "[source_2d][bend]") {
	Image source{4, 4, std::vector<uint8_t>(64), 0};
	constexpr std::array<uint8_t, 4> colour{80, 120, 200, 128};
	for (size_t p = 0; p < source.Pixels.size(); ++p)
		source.Pixels[p] = colour[p % 4];
	for (int64_t axis : {0, 1}) {
		auto run = RunNode(
			"pc.bend",
			{{"surface_in", &source}},
			{{"type", EnumValue{0}},
			 {"axis", EnumValue{axis}},
			 {"amount", 1.},
			 {"dimension_type", EnumValue{1}},
			 {"interpolate", EnumValue{1}}}
		);
		INFO(run.Message);
		REQUIRE(run.Ok);
		CHECK(run.Output().Width == (axis == 0 ? 12 : 6));
		CHECK(run.Output().Height == (axis == 0 ? 6 : 12));
		// grug two-cell grid makes radius-six outer V and radius-two inner V.
		const uint32_t bandX = axis == 0 ? 5 : 4, bandY = axis == 0 ? 1 : 5;
		const uint32_t holeX = axis == 0 ? 6 : 0, holeY = axis == 0 ? 5 : 6;
		const size_t band = (size_t(bandY) * run.Output().Width + bandX) * 4;
		const size_t hole = (size_t(holeY) * run.Output().Width + holeX) * 4;
		for (size_t c = 0; c < 4; ++c) {
			CHECK(run.Output().Pixels[band + c] == colour[c]);
			CHECK(run.Output().Pixels[hole + c] == 0);
		}
	}
}

TEST_CASE("Bend Wave interpolates its sampled peak and source UVs", "[source_2d][bend]") {
	const auto source = Coordinates();
	for (int64_t axis : {0, 1}) {
		auto run = RunNode(
			"pc.bend",
			{{"surface_in", &source}},
			{{"type", EnumValue{1}},
			 {"axis", EnumValue{axis}},
			 {"amount", 1.},
			 {"scale", 1.},
			 {"shift", 0.},
			 {"dimension_type", EnumValue{1}},
			 {"interpolate", EnumValue{1}}}
		);
		INFO(run.Message);
		REQUIRE(run.Ok);
		CHECK(run.Output().Width == (axis == 0 ? 6 : 4));
		CHECK(run.Output().Height == (axis == 0 ? 4 : 6));
		// grug peak is two pixels. halfway to peak shifts by one and a half.
		const uint32_t x = axis == 0 ? 2 : 1, y = axis == 0 ? 1 : 2;
		const size_t output = (size_t(y) * run.Output().Width + x) * 4;
		const size_t input = (size_t(1) * source.Width + 1) * 4;
		const size_t empty = (axis == 0 ? size_t(1) * run.Output().Width : size_t(1)) * 4;
		for (size_t c = 0; c < 4; ++c) {
			CHECK(run.Output().Pixels[output + c] == source.Pixels[input + c]);
			CHECK(run.Output().Pixels[empty + c] == 0);
		}
	}
}

TEST_CASE("Bend full batch admission refuses before any executor row", "[source_2d][bend]") {
	const auto source = Coordinates();
	const auto *entry = FindCatalogueEntry("pc.bend");
	const auto execute = detail::FindExecutor("pc.bend");
	REQUIRE(entry);
	REQUIRE(execute);
	Node node{"bend", "pc.bend", "", {}, {}};
	EvaluationRequest request;
	detail::NodeContext context(node, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	context.Images = {{"surface_in", &source}};
	context.Values = {
		{"amount",
		 ArrayValue{
			 ValueType::Scalar, std::vector<ElementValue>(Limits::MaximumArrayElements, ElementValue{.5})
		 }},
		{"dimension_type", EnumValue{0}},
		{"dimension", Vector2{100, 100}},
		{"dimension_unit", EnumValue{0}},
		{"interpolate", EnumValue{1}}
	};
	context.InputProvenanceResolved = true;
	size_t observedRows = 0;
	const auto observe = [](detail::NodeContext &, void *state) {
		++*static_cast<size_t *>(state);
		return true;
	};
	CHECK_FALSE(detail::RunProcessorBatch(context, execute, observe, &observedRows));
	CHECK(context.FailureCode == Status::LimitExceeded);
	CHECK(context.FailureMessage.find("complete batch") != std::string::npos);
	CHECK(observedRows == 0);
	CHECK(context.OutputImages.empty());
	CHECK(context.OutputImageArrays.empty());
}
