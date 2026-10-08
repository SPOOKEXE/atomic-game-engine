#include "../src/ProcessorBatch.hpp"
#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <limits>

TEST_SUITE_ID("engine.imagegraph.source_tile_random")
using namespace engine::imagegraph;
namespace source_tile_random_test {
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
	float Fraction(float value) {
		return value - std::floor(value);
	}
	// The MIT License
	// Copyright © 2015 Inigo Quilez
	// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
	// associated documentation files (the "Software"), to deal in the Software without restriction,
	// including without limitation the rights to use, copy, modify, merge, publish, distribute,
	// sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
	// furnished to do so, subject to the following conditions: The above copyright notice and this
	// permission notice shall be included in all copies or substantial portions of the Software.
	// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
	// NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
	// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
	// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
	// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
	// Independent nearest-sample CPU oracle for the pinned sh_tile_random.fsh equations.
	SurfacePixel Shader(const Image &source, float u, float v, float randomness) {
		std::array<float, 4> weighted{};
		float weights = 0, squares = 0;
		const float cellX = std::floor(u), cellY = std::floor(v);
		for (int y = -1; y <= 1; ++y)
			for (int x = -1; x <= 1; ++x) {
				const float px = cellX + x, py = cellY + y;
				const std::array<float, 4> noise{
					Fraction(std::sin(1.f + px * 37.f + py * 17.f) * 103.f),
					Fraction(std::sin(2.f + px * 11.f + py * 47.f) * 103.f),
					Fraction(std::sin(3.f + px * 41.f + py * 29.f) * 103.f),
					Fraction(std::sin(4.f + px * 23.f + py * 31.f) * 103.f)
				};
				const float dx = x - Fraction(u) + noise[0], dy = y - Fraction(v) + noise[1];
				const float weight = std::exp(-5.f * (dx * dx + dy * dy));
				const float sampleX = Fraction(u + randomness * noise[2]);
				const float sampleY = Fraction(v + randomness * noise[3]);
				SurfacePixel sample;
				REQUIRE(LoadSurfacePixel(
					source, uint32_t(sampleX * source.Width), uint32_t(sampleY * source.Height), sample
				));
				for (size_t channel = 0; channel < 4; ++channel)
					weighted[channel] += weight * float(sample[channel]);
				weights += weight;
				squares += weight * weight;
			}
		SurfacePixel result;
		for (size_t channel = 0; channel < 4; ++channel) {
			const float average = weighted[channel] / weights;
			const float contrast = .3f + (weighted[channel] - weights * .3f) / std::sqrt(squares);
			result[channel] = average * (1.f - randomness) + contrast * randomness;
		}
		return result;
	}
	Image Oracle(const Image &source, Vector2 raw, double randomness) {
		const uint32_t width = uint32_t(std::max(1., std::nearbyint(raw.X)));
		const uint32_t height = uint32_t(std::max(1., std::nearbyint(raw.Y)));
		Image result{width, height, std::vector<uint8_t>(size_t(width) * height * 4), 0};
		for (uint32_t y = 0; y < height; ++y)
			for (uint32_t x = 0; x < width; ++x) {
				if (x + .5 >= raw.X || y + .5 >= raw.Y) continue;
				const float u = ((float(x) + .5f) / float(raw.X)) * float(raw.X / source.Width);
				const float v = ((float(y) + .5f) / float(raw.Y)) * float(raw.Y / source.Height);
				REQUIRE(StoreSurfacePixel(result, x, y, Shader(source, u, v, float(randomness))));
			}
		return result;
	}
	void SamePixels(const Image &actual, const Image &expected) {
		REQUIRE(actual.Width == expected.Width);
		REQUIRE(actual.Height == expected.Height);
		REQUIRE(actual.Format == expected.Format);
		REQUIRE(actual.Pixels.size() == expected.Pixels.size());
		for (size_t i = 0; i < actual.Pixels.size(); ++i) {
			INFO("byte " << i);
			CHECK(std::abs(int(actual.Pixels[i]) - int(expected.Pixels[i])) <= 1);
		}
	}
	struct Graph {
		Document Doc;
		Image Source = Labels();
		Graph() {
			Doc.FormatVersion = 9;
			Doc.Nodes = {
				{"source", "image.captured", "", {}, {{"source_id", std::string{"source"}}}},
				{"tile",
				 "pc.tile_random",
				 "",
				 {},
				 {{"dimension", Vector2{8, 5}}, {"dimension_unit", EnumValue{0}}, {"randomness", .5}}}
			};
			Doc.Links = {{"source", "image", "tile", "surface_in"}};
			Doc.Outputs = {{"out", "tile", "surface_out"}};
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
using namespace source_tile_random_test;

TEST_CASE("Tile Random nearest CPU pixels follow all nine shader neighbors", "[imagegraph][tile_random]") {
	Graph graph;
	for (double randomness : {0., .5, 1., -.5, 1.5}) {
		INFO(randomness);
		graph.Set("randomness", randomness);
		SamePixels(graph.Run(), Oracle(graph.Source, {8, 5}, randomness));
	}
	graph.Set("randomness", 0.);
	const auto periodic = graph.Run();
	for (uint32_t y = 0; y < 5; ++y)
		for (uint32_t x = 0; x < 8; ++x) {
			SurfacePixel pixel, source;
			REQUIRE(LoadSurfacePixel(periodic, x, y, pixel));
			REQUIRE(LoadSurfacePixel(graph.Source, x % 3, y % 2, source));
			for (size_t channel = 0; channel < 4; ++channel)
				CHECK(pixel[channel] == Catch::Approx(source[channel]).margin(1. / 255));
		}
}

TEST_CASE(
	"Tile Random separates rounded allocation from raw fractional rectangle and shader scale",
	"[imagegraph][tile_random]"
) {
	Graph graph;
	for (Vector2 dimension :
		 {Vector2{3.5, 2.5},
		  Vector2{4.4, 3.6},
		  Vector2{.25, .75},
		  Vector2{0, 0},
		  Vector2{-2, 3},
		  Vector2{3, -2}}) {
		graph.Set("dimension", dimension);
		SamePixels(graph.Run(), Oracle(graph.Source, dimension, .5));
	}
}

TEST_CASE(
	"Tile Random reads every numeric source format and owns RGBA8 shader output", "[imagegraph][tile_random]"
) {
	for (SurfaceFormat format :
		 {SurfaceFormat::RGBA8Unorm,
		  SurfaceFormat::RGBA4Unorm,
		  SurfaceFormat::RGBA16Float,
		  SurfaceFormat::RGBA32Float,
		  SurfaceFormat::R8Unorm,
		  SurfaceFormat::R16Float,
		  SurfaceFormat::R32Float}) {
		INFO(unsigned(format));
		Graph graph;
		graph.Source = Labels(format);
		SamePixels(graph.Run(), Oracle(graph.Source, {8, 5}, .5));
	}
}

TEST_CASE(
	"Tile Random Project dimensions and linked dimensions retain source getter units",
	"[imagegraph][tile_random]"
) {
	Graph graph;
	graph.Doc.Project.emplace();
	graph.Doc.Project->SurfaceWidth = 10;
	graph.Doc.Project->SurfaceHeight = 20;
	graph.Set("dimension", Vector2{.8, .25});
	graph.Set("dimension_unit", EnumValue{1});
	SamePixels(graph.Run(), Oracle(graph.Source, {8, 5}, .5));
	graph.Doc.Junctions = {
		{"dimension", "", ValueType::Vector2, Vector2{8, 5}}, {"randomness", "", ValueType::Scalar, 1.}
	};
	graph.Doc.Links.push_back({"dimension", "value", "tile", "dimension"});
	graph.Doc.Links.push_back({"randomness", "value", "tile", "randomness"});
	SamePixels(graph.Run(), Oracle(graph.Source, {8, 5}, 1.));
}

TEST_CASE(
	"Tile Random scalar inputs run the shader with array processing disabled", "[imagegraph][tile_random]"
) {
	for (SurfaceFormat format :
		 {SurfaceFormat::RGBA8Unorm,
		  SurfaceFormat::RGBA4Unorm,
		  SurfaceFormat::RGBA16Float,
		  SurfaceFormat::RGBA32Float,
		  SurfaceFormat::R8Unorm,
		  SurfaceFormat::R16Float,
		  SurfaceFormat::R32Float}) {
		Graph graph;
		graph.Source = Labels(format);
		graph.Set("attribute_process", false);
		SamePixels(graph.Run(), Oracle(graph.Source, {8, 5}, .5));
	}
}

TEST_CASE(
	"Tile Random animated local controls resolve inside groups at fractional frames",
	"[imagegraph][tile_random]"
) {
	Graph graph;
	graph.Doc.Timeline = TimelineSettings{11, 0, 10, "loop", 11};
	for (auto &node : graph.Doc.Nodes)
		node.GroupId = "group";
	graph.Doc.Groups = {Group{"group", "Group"}};
	graph.Doc.Nodes[1].SourceAnimatedInputs = {"randomness"};
	graph.Doc.Keyframes = {{"tile", "randomness", 0, 0., "linear"}, {"tile", "randomness", 10, 1., "linear"}};
	EvaluationRequest request;
	request.Tick = 4;
	request.Subframe = .5;
	SamePixels(graph.Run(request), Oracle(graph.Source, {8, 5}, .45));
	request.Tick = 0;
	request.Subframe = 0;
	SamePixels(graph.Run(request), Oracle(graph.Source, {8, 5}, 0));
	request.Tick = 4;
	request.Subframe = .5;
	SamePixels(graph.Run(request), Oracle(graph.Source, {8, 5}, .45));
	Document restored;
	Diagnostic error;
	REQUIRE(Read(Write(graph.Doc), restored, error) == Status::Ok);
	graph.Doc = std::move(restored);
	SamePixels(graph.Run(request), Oracle(graph.Source, {8, 5}, .45));
}

TEST_CASE("Tile Random processor modes retain each complete shader row", "[imagegraph][tile_random]") {
	for (int64_t mode = 0; mode < 4; ++mode) {
		Graph graph;
		graph.Set("dimension", ArrayValue{ValueType::Vector2, {Vector2{4, 3}, Vector2{8, 5}}});
		graph.Set("randomness", ArrayValue{ValueType::Scalar, {0., 1.}});
		graph.Set("attribute_array_process", EnumValue{mode});
		const auto rows = graph.Rows();
		REQUIRE(rows.Images.size() == (mode < 2 ? 2 : 4));
		if (mode < 2) {
			SamePixels(rows.Images[0], Oracle(graph.Source, {4, 3}, 0));
			SamePixels(rows.Images[1], Oracle(graph.Source, {8, 5}, 1));
		} else {
			const std::array<Vector2, 4> dimensions{{{4, 3}, {4, 3}, {8, 5}, {8, 5}}};
			// Source inverse includes the scalar surface slot, so its randomness suffix stays zero.
			const std::array<double, 4> randomness =
				mode == 2 ? std::array<double, 4>{0, 1, 0, 1} : std::array<double, 4>{0, 0, 0, 0};
			for (size_t row = 0; row < 4; ++row)
				SamePixels(rows.Images[row], Oracle(graph.Source, dimensions[row], randomness[row]));
		}
	}
}

TEST_CASE(
	"Tile Random admits all rows before any output and preserves caller results on refusal",
	"[imagegraph][tile_random]"
) {
	Graph graph;
	graph.Set("dimension", ArrayValue{ValueType::Vector2, {Vector2{4, 3}, Vector2{4096, 4096}}});
	Diagnostic error;
	const auto plan = graph.CompileNow(error);
	const std::array sources{RequestImageSource{"source", graph.Source}};
	EvaluationRequest request;
	request.ImageSources = sources;
	ImageArray output;
	output.Images = {{1, 1, {11, 22, 33, 255}}};
	const auto before = output;
	CHECK(EvaluateArray(graph.Doc, plan, "out", request, output, error) == Status::LimitExceeded);
	CHECK(output.Images == before.Images);
	CHECK(output.Items == before.Items);
	graph.Set("dimension", Vector2{8, 5});
	const auto smallPlan = graph.CompileNow(error);
	Image retained{1, 1, {11, 22, 33, 255}};
	const auto original = retained;
	CHECK(Evaluate(graph.Doc, smallPlan, "out", request, retained, error, 1) == Status::LimitExceeded);
	CHECK(retained == original);
}

TEST_CASE(
	"Tile Random refuses missing surfaces unsupported units and nonfinite controls without output",
	"[imagegraph][tile_random]"
) {
	const auto source = Labels();
	const auto missing = imagegraph_test::RunNode("pc.tile_random", {}, {{"dimension_unit", EnumValue{0}}});
	CHECK_FALSE(missing.Ok);
	CHECK(missing.Images.empty());
	CHECK(missing.Port == "surface_in");
	for (double randomness :
		 {std::numeric_limits<double>::infinity(),
		  std::numeric_limits<double>::quiet_NaN(),
		  std::numeric_limits<double>::max()}) {
		const auto invalid = imagegraph_test::RunNode(
			"pc.tile_random",
			{{"surface_in", &source}},
			{{"randomness", randomness}, {"dimension_unit", EnumValue{0}}}
		);
		CHECK_FALSE(invalid.Ok);
		CHECK(invalid.Images.empty());
		CHECK(invalid.Port == "randomness");
	}
	const auto mask = imagegraph_test::RunNode(
		"pc.tile_random", {{"surface_in", &source}}, {{"dimension_unit", EnumValue{2}}}
	);
	CHECK_FALSE(mask.Ok);
	CHECK(mask.Code == Status::UnsupportedExecution);
	CHECK(mask.Port == "dimension_unit");
	CHECK(mask.Images.empty());
}

TEST_CASE(
	"Tile Random later row refusal happens before every processor output observer",
	"[imagegraph][tile_random]"
) {
	const auto source = Labels();
	const auto *entry = FindCatalogueEntry("pc.tile_random");
	const auto executor = detail::FindExecutor("pc.tile_random");
	REQUIRE(entry);
	REQUIRE(executor);
	Node node{"tile", "pc.tile_random", "", {}, {}};
	EvaluationRequest request;
	detail::EvaluationBudget budget(Limits::MaximumEvaluationBytes);
	{
		detail::NodeContext context(node, *entry, request, budget);
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		context.Images = {{"surface_in", &source}};
		for (const auto &input : entry->Inputs)
			if (const auto value = CatalogueDefault(input)) context.Values.emplace_back(input.Id, *value);
		for (auto &[port, value] : context.Values) {
			if (port == "dimension")
				value = ArrayValue{ValueType::Vector2, {Vector2{300, 300}, Vector2{300, 300}}};
			if (port == "dimension_unit") value = EnumValue{0};
		}
		context.InputProvenanceResolved = true;
		Status expected = Status::LimitExceeded;
		SECTION("whole batch nine sample work") {}
		SECTION("later nonfinite randomness") {
			for (auto &[port, value] : context.Values) {
				if (port == "dimension") value = Vector2{4, 3};
				if (port == "randomness")
					value = ArrayValue{ValueType::Scalar, {0., std::numeric_limits<double>::infinity()}};
			}
			expected = Status::InvalidValue;
		}
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
	"Tile Random constructor defaults and scalar or surface dimensions use their original getters",
	"[imagegraph][tile_random]"
) {
	Graph graph;
	graph.Doc.Project.emplace();
	graph.Doc.Project->SurfaceWidth = 10;
	graph.Doc.Project->SurfaceHeight = 6;
	graph.Doc.Nodes[1].Values.clear();
	SamePixels(graph.Run(), Oracle(graph.Source, {10, 6}, .5));
	graph.Doc.Junctions = {{"dimension", "", ValueType::Scalar, 4.}};
	graph.Doc.Links.push_back({"dimension", "value", "tile", "dimension"});
	SamePixels(graph.Run(), Oracle(graph.Source, {4, 4}, .5));
	graph.Doc.Links.pop_back();
	graph.Doc.Junctions.clear();
	graph.Doc.Nodes.push_back(
		{"dimension_surface",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{3}}, {"height", int64_t{7}}, {"colour", Colour{255, 255, 255, 255}}}}
	);
	graph.Doc.Links.push_back({"dimension_surface", "image", "tile", "dimension"});
	SamePixels(graph.Run(), Oracle(graph.Source, {3, 7}, .5));
}

TEST_CASE("Tile Random scalar controls pass through a real group input", "[imagegraph][tile_random]") {
	Graph graph;
	for (auto &node : graph.Doc.Nodes)
		node.GroupId = "group";
	graph.Doc.Nodes.push_back(
		{"input",
		 "pc.group_input",
		 "group",
		 {},
		 {{"input_type", EnumValue{11}}, {"subtype", EnumValue{0}}, {"vector_size", EnumValue{0}}}}
	);
	Group group{"group", "Group"};
	group.Ports = {{"input", "input/parent", PortDirection::Input, "input"}};
	graph.Doc.Groups = {group};
	graph.Doc.Junctions = {{"input/parent", "group", ValueType::Any, 1.}};
	graph.Doc.Links.push_back({"input/parent", "value", "input", "parent_value"});
	graph.Doc.Links.push_back({"input", "value", "tile", "randomness"});
	SamePixels(graph.Run(), Oracle(graph.Source, {8, 5}, 1));
}

TEST_CASE(
	"Tile Random rejects Atlas texture bindings and derived shader overflow", "[imagegraph][tile_random]"
) {
	const auto source = Labels();
	AtlasValue atlas;
	atlas.Data.emplace().Kind = AtlasKind::SurfaceAtlas;
	atlas.Data->Surface.Data = source;
	atlas.Data->Dimension = {1, 1};
	const auto rejected = imagegraph_test::RunNode(
		"pc.tile_random",
		{},
		{{"surface_in", atlas}, {"dimension", Vector2{4, 3}}, {"dimension_unit", EnumValue{0}}}
	);
	CHECK_FALSE(rejected.Ok);
	CHECK(rejected.Code == Status::UnsupportedExecution);
	CHECK(rejected.Port == "surface_in");
	CHECK(rejected.Images.empty());
	const auto overflow = imagegraph_test::RunNode(
		"pc.tile_random",
		{{"surface_in", &source}},
		{{"dimension", Vector2{std::numeric_limits<double>::max(), 3}}, {"dimension_unit", EnumValue{0}}}
	);
	CHECK_FALSE(overflow.Ok);
	CHECK(overflow.Images.empty());
	CHECK(overflow.Port == "dimension");
}

TEST_CASE(
	"Tile Random unprocessed outer arrays refuse without replacing caller rows", "[imagegraph][tile_random]"
) {
	Graph graph;
	graph.Set("attribute_process", false);
	graph.Set("randomness", ArrayValue{ValueType::Scalar, {0., 1.}});
	Diagnostic error;
	const auto plan = graph.CompileNow(error);
	const std::array sources{RequestImageSource{"source", graph.Source}};
	EvaluationRequest request;
	request.ImageSources = sources;
	ImageArray retained;
	retained.Images = {{1, 1, {11, 22, 33, 255}}};
	const auto before = retained;
	CHECK(EvaluateArray(graph.Doc, plan, "out", request, retained, error) == Status::UnsupportedExecution);
	CHECK(error.Port == "attribute_process");
	CHECK(retained.Images == before.Images);
	CHECK(retained.Items == before.Items);
}
