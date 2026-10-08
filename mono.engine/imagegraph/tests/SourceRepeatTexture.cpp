#include "../src/ProcessorBatch.hpp"
#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <limits>

TEST_SUITE_ID("engine.imagegraph.source_repeat_texture")
using namespace engine::imagegraph;
namespace source_repeat_texture_test {
	Image Labels(SurfaceFormat format = SurfaceFormat::RGBA32Float) {
		const auto layout = CheckedSurfaceLayout(3, 2, format, Limits::MaximumEvaluationBytes);
		REQUIRE(layout);
		Image image{3, 2, std::vector<uint8_t>(layout->Bytes), 0, format};
		for (uint32_t y = 0; y < 2; ++y)
			for (uint32_t x = 0; x < 3; ++x)
				REQUIRE(StoreSurfacePixel(
					image, x, y, {.1 + .1 * x, .2 + .2 * y, .1 + .07 * (3 * y + x), .4 + .1 * x}
				));
		return image;
	}
	float Fract(float x) {
		return x - std::floor(x);
	}
	std::array<float, 4> Hash(float x, float y, float seed, float randomness) {
		const std::array<float, 4> a{37, 11, 41, 23}, b{17, 47, 29, 31};
		std::array<float, 4> result{};
		for (size_t c = 0; c < 4; ++c)
			result[c] = Fract(std::sin(float(c + 1) + seed + (x * a[c] + y * b[c])) * 103.f) * randomness;
		return result;
	}
	SurfacePixel Texel(const Image &source, float x, float y) {
		SurfacePixel result;
		REQUIRE(LoadSurfacePixel(
			source, uint32_t(Fract(x) * source.Width), uint32_t(Fract(y) * source.Height), result
		));
		for (size_t c = 0; c < 4; ++c)
			result[c] = float(result[c]);
		return result;
	}
	float Smooth(float x) {
		const float t = std::clamp((x - .25f) / .5f, 0.f, 1.f);
		return t * t * (3.f - 2.f * t);
	}
	// Independent nearest CPU oracle for the pinned sh_texture_repeat.fsh equations.
	// The upstream shader credits Copyright © 2015 Inigo Quilez; GPU parity is not asserted.
	SurfacePixel Shader(const Image &source, float x, float y, int mode, float seed, float randomness) {
		if (mode == 0) return Texel(source, x, y);
		SurfacePixel result{};
		const float cellX = std::floor(x), cellY = std::floor(y);
		if (mode == 1) {
			std::array<SurfacePixel, 4> corners;
			for (size_t corner = 0; corner < 4; ++corner) {
				const auto h = Hash(cellX + float(corner % 2), cellY + float(corner / 2), seed, randomness);
				const auto sign = [](float value) { return float((value > 0) - (value < 0)); };
				corners[corner] = Texel(source, x * sign(h[2] - .5f) + h[0], y * sign(h[3] - .5f) + h[1]);
			}
			const float bx = Smooth(Fract(x)), by = Smooth(Fract(y));
			for (size_t c = 0; c < 4; ++c) {
				const float top = float(corners[0][c]) * (1.f - bx) + float(corners[1][c]) * bx;
				const float bottom = float(corners[2][c]) * (1.f - bx) + float(corners[3][c]) * bx;
				result[c] = top * (1.f - by) + bottom * by;
			}
		} else {
			std::array<float, 3> colours{};
			float total = 0;
			for (int j = -1; j <= 1; ++j)
				for (int i = -1; i <= 1; ++i) {
					const auto h = Hash(cellX + i, cellY + j, seed, randomness);
					const float dx = float(i) - Fract(x) + h[0], dy = float(j) - Fract(y) + h[1];
					const float weight = std::exp(-5.f * (dx * dx + dy * dy));
					const auto colour = Texel(source, x + 4.f * h[2], y + 4.f * h[3]);
					for (size_t c = 0; c < 3; ++c)
						colours[c] += weight * float(colour[c]);
					total += weight;
				}
			for (size_t c = 0; c < 3; ++c)
				result[c] = colours[c] / total;
			result[3] = 1;
		}
		return result;
	}
	Image Oracle(
		const Image &source,
		Vector2 raw,
		int mode = 1,
		double seed = 2,
		double randomness = 1,
		SurfaceFormat format = SurfaceFormat::RGBA32Float
	) {
		const uint32_t width = uint32_t(std::max(1., std::nearbyint(raw.X))),
					   height = uint32_t(std::max(1., std::nearbyint(raw.Y)));
		const auto layout = CheckedSurfaceLayout(width, height, format, Limits::MaximumEvaluationBytes);
		REQUIRE(layout);
		Image result{width, height, std::vector<uint8_t>(layout->Bytes), 0, format};
		for (uint32_t y = 0; y < height; ++y)
			for (uint32_t x = 0; x < width; ++x) {
				const float u = ((float(x) + .5f) / width) * (float(raw.X) / source.Width);
				const float v = ((float(y) + .5f) / height) * (float(raw.Y) / source.Height);
				REQUIRE(StoreSurfacePixel(
					result, x, y, Shader(source, u, v, mode, float(seed), float(randomness))
				));
			}
		return result;
	}
	void SamePixels(const Image &actual, const Image &expected) {
		REQUIRE(actual.Width == expected.Width);
		REQUIRE(actual.Height == expected.Height);
		REQUIRE(actual.Format == expected.Format);
		for (uint32_t y = 0; y < actual.Height; ++y)
			for (uint32_t x = 0; x < actual.Width; ++x) {
				SurfacePixel a, b;
				REQUIRE(LoadSurfacePixel(actual, x, y, a));
				REQUIRE(LoadSurfacePixel(expected, x, y, b));
				for (size_t c = 0; c < 4; ++c) {
					INFO(x << "," << y << " channel " << c);
					CHECK(
						a[c] == Catch::Approx(b[c]).margin(
									actual.Format == SurfaceFormat::RGBA4Unorm ? 1. / 15 : .0002
								)
					);
				}
			}
	}
	struct Graph {
		Document Doc;
		Image Source = Labels();
		Graph() {
			Doc.FormatVersion = 9;
			Doc.Nodes = {
				{"source", "image.captured", "", {}, {{"source_id", std::string{"source"}}}},
				{"repeat",
				 "pc.repeat_texture",
				 "",
				 {},
				 {{"target_dimension", Vector2{8, 5}},
				  {"target_dimension_unit", EnumValue{0}},
				  {"randomness", 1.},
				  {"seed", 2.},
				  {"type", EnumValue{1}}}}
			};
			Doc.Links = {{"source", "image", "repeat", "surface_in"}};
			Doc.Outputs = {{"out", "repeat", "surface_out"}};
		}
		void Set(std::string_view port, Value value) {
			for (auto &input : Doc.Nodes[1].Values)
				if (input.Port == port) {
					input.Data = std::move(value);
					return;
				}
			Doc.Nodes[1].Values.push_back({std::string(port), std::move(value)});
		}
		Plan CompileNow(Diagnostic &error) {
			Plan plan;
			const auto status = Compile(Doc, plan, error);
			INFO(error.Message << " " << error.NodeId << ":" << error.Port);
			REQUIRE(status == Status::Ok);
			return plan;
		}
		Image Run(EvaluationRequest request = {}) {
			Diagnostic error;
			const auto plan = CompileNow(error);
			const std::array sources{RequestImageSource{"source", Source}};
			request.ImageSources = sources;
			Image output;
			const auto status = Evaluate(Doc, plan, "out", request, output, error);
			INFO(error.Message << " " << error.NodeId << ":" << error.Port);
			REQUIRE(status == Status::Ok);
			return output;
		}
		ImageArray Rows(EvaluationRequest request = {}) {
			Diagnostic error;
			const auto plan = CompileNow(error);
			const std::array sources{RequestImageSource{"source", Source}};
			request.ImageSources = sources;
			ImageArray output;
			const auto status = EvaluateArray(Doc, plan, "out", request, output, error);
			INFO(error.Message << " " << error.NodeId << ":" << error.Port);
			REQUIRE(status == Status::Ok);
			return output;
		}
	};
}
using namespace source_repeat_texture_test;

TEST_CASE("Repeat Texture nearest CPU oracle covers Tile Scatter and Cell", "[imagegraph][repeat_texture]") {
	Graph graph;
	for (int mode = 0; mode < 3; ++mode)
		for (double randomness : {0., .5, 1., -.5, 1.5})
			for (double seed : {0., 2., -.25}) {
				INFO(mode << " seed " << seed << " randomness " << randomness);
				graph.Set("type", EnumValue{mode});
				graph.Set("seed", seed);
				graph.Set("randomness", randomness);
				SamePixels(graph.Run(), Oracle(graph.Source, {8, 5}, mode, seed, randomness));
			}
}

TEST_CASE(
	"Repeat Texture zero randomness flips Scatter and retains Cell alpha", "[imagegraph][repeat_texture]"
) {
	Graph graph;
	graph.Set("type", EnumValue{1});
	graph.Set("randomness", 0.);
	const auto scatter = graph.Run();
	for (uint32_t y = 0; y < 5; ++y)
		for (uint32_t x = 0; x < 8; ++x) {
			SurfacePixel actual, expected;
			REQUIRE(LoadSurfacePixel(scatter, x, y, actual));
			expected = Texel(graph.Source, -(x + .5f) / 3.f, -(y + .5f) / 2.f);
			for (size_t c = 0; c < 4; ++c)
				CHECK(actual[c] == Catch::Approx(expected[c]).margin(.0002));
		}
	graph.Set("type", EnumValue{2});
	const auto cell = graph.Run();
	for (uint32_t y = 0; y < 5; ++y)
		for (uint32_t x = 0; x < 8; ++x) {
			SurfacePixel pixel;
			REQUIRE(LoadSurfacePixel(cell, x, y, pixel));
			CHECK(pixel[3] == 1);
		}
}

TEST_CASE(
	"Repeat Texture raw dimensions scale normalized allocated pixels including negative and zero",
	"[imagegraph][repeat_texture]"
) {
	Graph graph;
	for (const Vector2 raw :
		 {Vector2{7.5, 4.5}, Vector2{7.49, 4.51}, Vector2{.4, .2}, Vector2{0, 0}, Vector2{-3, -2}}) {
		graph.Set("target_dimension", raw);
		for (int mode = 0; mode < 3; ++mode) {
			graph.Set("type", EnumValue{mode});
			SamePixels(graph.Run(), Oracle(graph.Source, raw, mode));
		}
	}
	graph.Set("type", EnumValue{0});
	graph.Set("target_dimension", Vector2{8, 5});
	const auto tile = graph.Run();
	for (uint32_t y = 0; y < 5; ++y)
		for (uint32_t x = 0; x < 8; ++x) {
			SurfacePixel a, b;
			REQUIRE(LoadSurfacePixel(tile, x, y, a));
			REQUIRE(LoadSurfacePixel(graph.Source, x % 3, y % 2, b));
			for (size_t c = 0; c < 4; ++c)
				CHECK(a[c] == Catch::Approx(b[c]).margin(.0002));
		}
}

TEST_CASE(
	"Repeat Texture all source formats and explicit output depths retain shader pixels",
	"[imagegraph][repeat_texture]"
) {
	const std::array formats{
		SurfaceFormat::RGBA4Unorm,
		SurfaceFormat::RGBA8Unorm,
		SurfaceFormat::RGBA16Float,
		SurfaceFormat::RGBA32Float,
		SurfaceFormat::R8Unorm,
		SurfaceFormat::R16Float,
		SurfaceFormat::R32Float
	};
	for (const auto input : formats)
		for (int mode = 0; mode < 3; ++mode) {
			Graph graph;
			graph.Source = Labels(input);
			graph.Set("type", EnumValue{mode});
			graph.Set("attribute_process", false);
			SamePixels(graph.Run(), Oracle(graph.Source, {8, 5}, mode, 2, 1, input));
			for (size_t depth = 0; depth < formats.size(); ++depth) {
				graph.Set("attribute_color_depth", EnumValue{int64_t(depth + 2)});
				SamePixels(graph.Run(), Oracle(graph.Source, {8, 5}, mode, 2, 1, formats[depth]));
			}
		}
	const auto source = Labels();
	const auto *entry = FindCatalogueEntry("pc.repeat_texture");
	REQUIRE(entry);
	Node node{"repeat", "pc.repeat_texture", "", {}, {}};
	EvaluationRequest request;
	detail::NodeContext context(node, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	context.Images = {{"surface_in", &source}};
	context.Values = {
		{"target_dimension", Vector2{8, 5}},
		{"target_dimension_unit", EnumValue{0}},
		{"seed", 2.},
		{"type", EnumValue{1}},
		{"randomness", 1.},
		{"attribute_color_depth", EnumValue{1}}
	};
	context.InheritedSurfaceFormat = SurfaceFormat::RGBA16Float;
	context.InputProvenanceResolved = true;
	REQUIRE(detail::FindExecutor("pc.repeat_texture")(context));
	REQUIRE(context.OutputImages.size() == 1);
	SamePixels(context.OutputImages[0].second, Oracle(source, {8, 5}, 1, 2, 1, SurfaceFormat::RGBA16Float));
}

TEST_CASE(
	"Repeat Texture Project and linked Dimension getters keep source units", "[imagegraph][repeat_texture]"
) {
	Graph graph;
	graph.Doc.Project.emplace();
	graph.Doc.Project->SurfaceWidth = 10;
	graph.Doc.Project->SurfaceHeight = 20;
	graph.Set("target_dimension", Vector2{.8, .25});
	graph.Set("target_dimension_unit", EnumValue{1});
	SamePixels(graph.Run(), Oracle(graph.Source, {8, 5}));
	graph.Doc.Junctions = {
		{"dimension", "", ValueType::Scalar, 4.},
		{"seed", "", ValueType::Scalar, -.25},
		{"randomness", "", ValueType::Scalar, .5}
	};
	graph.Doc.Links.push_back({"dimension", "value", "repeat", "target_dimension"});
	graph.Doc.Links.push_back({"seed", "value", "repeat", "seed"});
	graph.Doc.Links.push_back({"randomness", "value", "repeat", "randomness"});
	SamePixels(graph.Run(), Oracle(graph.Source, {4, 4}, 1, -.25, .5));
	graph.Doc.Junctions[0].Type = ValueType::Vector2;
	graph.Doc.Junctions[0].Default = Vector2{6, 3};
	SamePixels(graph.Run(), Oracle(graph.Source, {6, 3}, 1, -.25, .5));
	graph.Doc.Links.erase(graph.Doc.Links.begin() + 1);
	graph.Doc.Nodes.push_back(
		{"dimension_image",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{3}}, {"height", int64_t{7}}, {"colour", Colour{255, 255, 255, 255}}}}
	);
	graph.Doc.Links.push_back({"dimension_image", "image", "repeat", "target_dimension"});
	SamePixels(graph.Run(), Oracle(graph.Source, {3, 7}, 1, -.25, .5));
}

TEST_CASE(
	"Repeat Texture defaults require observed seed and preserve constructor controls",
	"[imagegraph][repeat_texture]"
) {
	Graph graph;
	graph.Doc.Project.emplace();
	graph.Doc.Project->SurfaceWidth = 6;
	graph.Doc.Project->SurfaceHeight = 4;
	graph.Doc.Nodes[1].Values = {{"seed", 2.}};
	SamePixels(graph.Run(), Oracle(graph.Source, {6, 4}));
	graph.Doc.Nodes[1].Values.clear();
	Diagnostic error;
	const auto plan = graph.CompileNow(error);
	EvaluationRequest request;
	const std::array sources{RequestImageSource{"source", graph.Source}};
	request.ImageSources = sources;
	Image output{1, 1, {1, 2, 3, 4}};
	const auto original = output;
	CHECK(Evaluate(graph.Doc, plan, "out", request, output, error) == Status::UnsupportedExecution);
	CHECK(error.Port == "seed");
	CHECK(output == original);
}

TEST_CASE("Repeat Texture array rows use raw seed without row offsets", "[imagegraph][repeat_texture]") {
	for (int64_t process = 0; process < 4; ++process) {
		Graph graph;
		graph.Set("type", ArrayValue{ValueType::Integer, {int64_t{0}, int64_t{1}, int64_t{2}}});
		graph.Set("attribute_array_process", EnumValue{process});
		const auto rows = graph.Rows();
		REQUIRE(rows.Images.size() == 3);
		for (int mode = 0; mode < 3; ++mode)
			SamePixels(rows.Images[size_t(mode)], Oracle(graph.Source, {8, 5}, mode));
	}
	Graph graph;
	graph.Set("target_dimension", ArrayValue{ValueType::Vector2, {Vector2{4, 3}, Vector2{8, 5}}});
	graph.Set("seed", ArrayValue{ValueType::Scalar, {2., 2.}});
	graph.Set("randomness", ArrayValue{ValueType::Scalar, {.5, .5}});
	const auto rows = graph.Rows();
	REQUIRE(rows.Images.size() == 2);
	SamePixels(rows.Images[0], Oracle(graph.Source, {4, 3}, 1, 2, .5));
	SamePixels(rows.Images[1], Oracle(graph.Source, {8, 5}, 1, 2, .5));
	graph.Set("attribute_process", false);
	Diagnostic error;
	const auto plan = graph.CompileNow(error);
	const std::array sources{RequestImageSource{"source", graph.Source}};
	EvaluationRequest request;
	request.ImageSources = sources;
	ImageArray retained;
	retained.Images = {graph.Source};
	const auto original = retained;
	CHECK(EvaluateArray(graph.Doc, plan, "out", request, retained, error) == Status::UnsupportedExecution);
	CHECK(retained.Images == original.Images);
}

TEST_CASE(
	"Repeat Texture group animation samples nonzero subframes reproducibly", "[imagegraph][repeat_texture]"
) {
	Graph graph;
	graph.Doc.Timeline = TimelineSettings{11, 0, 10, "loop", 11};
	graph.Doc.Groups = {Group{"group", "Group"}};
	for (auto &node : graph.Doc.Nodes)
		node.GroupId = "group";
	graph.Doc.Nodes[1].SourceAnimatedInputs = {"randomness", "seed"};
	graph.Doc.Keyframes = {
		{"repeat", "randomness", 0, 0., "linear"},
		{"repeat", "randomness", 10, 1., "linear"},
		{"repeat", "seed", 0, 0., "linear"},
		{"repeat", "seed", 10, 4., "linear"}
	};
	EvaluationRequest request;
	request.Tick = 4;
	request.Subframe = .5;
	SamePixels(graph.Run(request), Oracle(graph.Source, {8, 5}, 1, 1.8, .45));
	Document restored;
	Diagnostic error;
	REQUIRE(Read(Write(graph.Doc), restored, error) == Status::Ok);
	graph.Doc = std::move(restored);
	SamePixels(graph.Run(request), Oracle(graph.Source, {8, 5}, 1, 1.8, .45));
}

TEST_CASE(
	"Repeat Texture refuses unavailable bindings and nonfinite shader controls",
	"[imagegraph][repeat_texture]"
) {
	const auto source = Labels();
	const auto missing = imagegraph_test::RunNode("pc.repeat_texture", {}, {{"seed", 2.}});
	CHECK_FALSE(missing.Ok);
	CHECK(missing.Port == "surface_in");
	CHECK(missing.Images.empty());
	AtlasValue atlas;
	atlas.Data.emplace().Kind = AtlasKind::SurfaceAtlas;
	atlas.Data->Surface.Data = source;
	atlas.Data->Dimension = {1, 1};
	const auto unsupported =
		imagegraph_test::RunNode("pc.repeat_texture", {}, {{"surface_in", atlas}, {"seed", 2.}});
	CHECK_FALSE(unsupported.Ok);
	CHECK(unsupported.Code == Status::UnsupportedExecution);
	CHECK(unsupported.Port == "surface_in");
	CHECK(unsupported.Images.empty());
	for (const std::string port : {"seed", "randomness"})
		for (double value :
			 {std::numeric_limits<double>::infinity(),
			  std::numeric_limits<double>::quiet_NaN(),
			  std::numeric_limits<double>::max()}) {
			const auto rejected = imagegraph_test::RunNode(
				"pc.repeat_texture",
				{{"surface_in", &source}},
				{{"seed", port == "seed" ? value : 2.},
				 {"randomness", port == "randomness" ? value : 1.},
				 {"target_dimension_unit", EnumValue{0}}}
			);
			CHECK_FALSE(rejected.Ok);
			CHECK(rejected.Code == Status::InvalidValue);
			CHECK(rejected.Port == port);
			CHECK(rejected.Images.empty());
		}
	for (const auto raw :
		 {Vector2{std::numeric_limits<double>::infinity(), 3},
		  Vector2{-std::numeric_limits<double>::max(), 3}}) {
		const auto rejected = imagegraph_test::RunNode(
			"pc.repeat_texture",
			{{"surface_in", &source}},
			{{"seed", 2.}, {"target_dimension", raw}, {"target_dimension_unit", EnumValue{0}}}
		);
		CHECK_FALSE(rejected.Ok);
		CHECK(rejected.Port == "target_dimension");
		CHECK(rejected.Images.empty());
	}
	for (const auto type : {.5}) {
		CAPTURE(type);
		const auto rejected = imagegraph_test::RunNode(
			"pc.repeat_texture", {{"surface_in", &source}}, {{"seed", 2.}, {"type", type}}
		);
		CHECK_FALSE(rejected.Ok);
		CHECK(rejected.Port == "type");
		CHECK(rejected.Images.empty());
	}
	for (const auto &[raw, normalized] : std::array<std::pair<double, int>, 2>{{{3., 2}, {-.5, 0}}}) {
		CAPTURE(raw, normalized);
		const auto clamped = imagegraph_test::RunNode(
			"pc.repeat_texture",
			{{"surface_in", &source}},
			{{"seed", 2.},
			 {"type", raw},
			 {"target_dimension", Vector2{4, 3}},
			 {"target_dimension_unit", EnumValue{0}}}
		);
		INFO(clamped.Message << " " << clamped.Port);
		REQUIRE(clamped.Ok);
		SamePixels(clamped.Output(), Oracle(source, {4, 3}, normalized));
	}
	const auto mask = imagegraph_test::RunNode(
		"pc.repeat_texture",
		{{"surface_in", &source}},
		{{"seed", 2.}, {"target_dimension_unit", EnumValue{2}}}
	);
	CHECK_FALSE(mask.Ok);
	CHECK(mask.Code == Status::UnsupportedExecution);
	CHECK(mask.Port == "target_dimension_unit");
}

TEST_CASE(
	"Repeat Texture whole batch work and variable format bytes refuse before observers",
	"[imagegraph][repeat_texture]"
) {
	const auto source = Labels();
	const auto *entry = FindCatalogueEntry("pc.repeat_texture");
	const auto executor = detail::FindExecutor("pc.repeat_texture");
	REQUIRE(entry);
	REQUIRE(executor);
	Node node{"repeat", "pc.repeat_texture", "", {}, {}};
	EvaluationRequest request;
	detail::EvaluationBudget budget(Limits::MaximumEvaluationBytes);
	{
		detail::NodeContext context(node, *entry, request, budget);
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		context.Images = {{"surface_in", &source}};
		context.Values = {
			{"seed", 2.},
			{"randomness", 1.},
			{"type", EnumValue{2}},
			{"target_dimension_unit", EnumValue{0}},
			{"attribute_color_depth", EnumValue{0}},
			{"target_dimension", ArrayValue{ValueType::Vector2, {Vector2{300, 300}, Vector2{300, 300}}}}
		};
		Status expected = Status::LimitExceeded;
		SECTION("aggregate Cell work exceeds two individually admissible rows") {}
		SECTION("two individually admissible outputs exceed aggregate bytes") {
			context.ByteBudget = 7000;
			context.Values.back().second = ArrayValue{ValueType::Vector2, {Vector2{20, 20}, Vector2{10, 10}}};
			context.Values[4].second = EnumValue{5};
		}
		SECTION("later nonfinite control is preflighted") {
			context.Values.back().second = Vector2{4, 3};
			context.Values[1].second =
				ArrayValue{ValueType::Scalar, {0., std::numeric_limits<double>::infinity()}};
			expected = Status::InvalidValue;
		}
		context.InputProvenanceResolved = true;
		size_t observed = 0;
		const auto observer = [](detail::NodeContext &, void *state) {
			++*static_cast<size_t *>(state);
			return true;
		};
		CHECK_FALSE(detail::RunProcessorBatch(context, executor, observer, &observed));
		CHECK(context.FailureCode == expected);
		CHECK(observed == 0);
		CHECK(context.OutputImages.empty());
		CHECK(context.OutputImageArrays.empty());
	}
	CHECK(budget.Used() == 0);
}

TEST_CASE(
	"Repeat Texture surface rows inherit the original first leaf depth", "[imagegraph][repeat_texture]"
) {
	ImageArray sources;
	sources.Images = {Labels(SurfaceFormat::RGBA4Unorm), Labels(SurfaceFormat::RGBA32Float)};
	sources.Items = {{size_t{0}}, {size_t{1}}};
	const auto *entry = FindCatalogueEntry("pc.repeat_texture");
	REQUIRE(entry);
	Node node{"repeat", "pc.repeat_texture", "", {}, {}};
	EvaluationRequest request;
	detail::NodeContext context(node, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	context.ImageArrays = {{"surface_in", &sources}};
	context.Values = {
		{"seed", 2.},
		{"randomness", .5},
		{"type", EnumValue{1}},
		{"target_dimension", Vector2{4, 3}},
		{"target_dimension_unit", EnumValue{0}},
		{"attribute_color_depth", EnumValue{0}}
	};
	context.InputProvenanceResolved = true;
	std::vector<Image> observed;
	const auto observer = [](detail::NodeContext &row, void *state) {
		if (row.OutputImages.size() != 1) return false;
		static_cast<std::vector<Image> *>(state)->push_back(row.OutputImages[0].second);
		return true;
	};
	const bool processed =
		detail::RunProcessorBatch(context, detail::FindExecutor("pc.repeat_texture"), observer, &observed);
	INFO(context.FailureMessage << " " << context.FailurePort);
	REQUIRE(processed);
	REQUIRE(observed.size() == 2);
	for (size_t row = 0; row < 2; ++row)
		SamePixels(observed[row], Oracle(sources.Images[row], {4, 3}, 1, 2, .5, SurfaceFormat::RGBA4Unorm));
}

TEST_CASE("Repeat Texture request limits preserve caller output", "[imagegraph][repeat_texture]") {
	Graph graph;
	Diagnostic error;
	const auto plan = graph.CompileNow(error);
	const std::array sources{RequestImageSource{"source", graph.Source}};
	EvaluationRequest request;
	request.ImageSources = sources;
	Image retained{1, 1, {17, 18, 19, 20}};
	const auto original = retained;
	request.MaximumImageDimension = 7;
	CHECK(Evaluate(graph.Doc, plan, "out", request, retained, error) == Status::LimitExceeded);
	CHECK(retained == original);
	request.MaximumImageDimension = Limits::MaximumDimension;
	CHECK(Evaluate(graph.Doc, plan, "out", request, retained, error, 1) == Status::LimitExceeded);
	CHECK(retained == original);
}

TEST_CASE(
	"Repeat Texture Scatter signed axis boundary and finite extremes are explicit",
	"[imagegraph][repeat_texture]"
) {
	Graph graph;
	graph.Set("target_dimension", Vector2{1, 1});
	const float boundary = .5f / Hash(0, 0, 2, 1)[2];
	REQUIRE(Hash(0, 0, 2, boundary)[2] == .5f);
	graph.Set("randomness", double(boundary));
	SamePixels(graph.Run(), Oracle(graph.Source, {1, 1}, 1, 2, boundary));
	graph.Set("seed", double(std::numeric_limits<float>::max()));
	graph.Set("randomness", 1.);
	SamePixels(graph.Run(), Oracle(graph.Source, {1, 1}, 1, std::numeric_limits<float>::max()));
	const auto source = Labels();
	const auto unused = imagegraph_test::RunNode(
		"pc.repeat_texture",
		{{"surface_in", &source}},
		{{"seed", 2.},
		 {"type", EnumValue{0}},
		 {"randomness", double(std::numeric_limits<float>::max())},
		 {"target_dimension", Vector2{4, 3}},
		 {"target_dimension_unit", EnumValue{0}}}
	);
	REQUIRE(unused.Ok);
	SamePixels(unused.Output(), Oracle(source, {4, 3}, 0));
	const auto coordinates = imagegraph_test::RunNode(
		"pc.repeat_texture",
		{{"surface_in", &source}},
		{{"seed", 2.},
		 {"type", EnumValue{1}},
		 {"target_dimension", Vector2{-double(std::numeric_limits<float>::max()), 3}},
		 {"target_dimension_unit", EnumValue{0}}}
	);
	CHECK_FALSE(coordinates.Ok);
	CHECK(coordinates.Code == Status::InvalidValue);
	CHECK(coordinates.Images.empty());
	const auto weighted = imagegraph_test::RunNode(
		"pc.repeat_texture",
		{{"surface_in", &source}},
		{{"seed", 2.},
		 {"type", EnumValue{2}},
		 {"randomness", double(std::numeric_limits<float>::max())},
		 {"target_dimension", Vector2{4, 3}},
		 {"target_dimension_unit", EnumValue{0}}}
	);
	CHECK_FALSE(weighted.Ok);
	CHECK(weighted.Code == Status::InvalidValue);
	CHECK(weighted.Images.empty());
}

TEST_CASE("Repeat Texture array choices bypass source widget clamping", "[imagegraph][repeat_texture]") {
	const auto source = Labels();
	const auto *entry = FindCatalogueEntry("pc.repeat_texture");
	REQUIRE(entry);
	for (const double raw : {3., -.5, .5}) {
		CAPTURE(raw);
		Node node{"repeat", "pc.repeat_texture", "", {}, {}};
		EvaluationRequest request;
		detail::NodeContext context(node, *entry, request);
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		context.Images = {{"surface_in", &source}};
		context.Values = {
			{"seed", 2.},
			{"randomness", 1.},
			{"type", ArrayValue{ValueType::Scalar, {raw}}},
			{"target_dimension", Vector2{4, 3}},
			{"target_dimension_unit", EnumValue{0}},
			{"attribute_color_depth", EnumValue{0}}
		};
		context.InputProvenanceResolved = true;
		size_t observed = 0;
		const auto observer = [](detail::NodeContext &, void *state) {
			++*static_cast<size_t *>(state);
			return true;
		};
		const bool processed = detail::RunProcessorBatch(
			context, detail::FindExecutor("pc.repeat_texture"), observer, &observed
		);
		INFO(context.FailureMessage << " " << context.FailurePort);
		CHECK_FALSE(processed);
		CHECK(context.FailureCode == Status::UnsupportedExecution);
		CHECK(context.FailurePort == "type");
		CHECK(observed == 0);
		CHECK(context.OutputImages.empty());
		CHECK(context.OutputImageArrays.empty());
	}
}
