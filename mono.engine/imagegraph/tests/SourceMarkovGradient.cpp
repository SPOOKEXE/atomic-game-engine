#include "../src/ProcessorBatch.hpp"
#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstring>
#include <limits>

TEST_SUITE_ID("engine.imagegraph.source_markov_gradient")
using namespace engine::imagegraph;
namespace source_markov_gradient_test {
	Image Labels(SurfaceFormat format = SurfaceFormat::RGBA32Float) {
		const auto layout = CheckedSurfaceLayout(4, 3, format, Limits::MaximumEvaluationBytes);
		REQUIRE(layout);
		Image image{4, 3, std::vector<uint8_t>(layout->Bytes), 0, format};
		const std::array<SurfacePixel, 4> colors{
			{{1, 0, 0, .2}, {0, 1, 0, .4}, {0, 0, 1, .6}, {1, 1, 0, .8}}
		};
		for (uint32_t y = 0; y < 3; ++y)
			for (uint32_t x = 0; x < 4; ++x)
				REQUIRE(StoreSurfacePixel(image, x, y, colors[x]));
		return image;
	}
	const std::array<Colour, 3> COLORS{{{255, 0, 0, 17}, {0, 255, 0, 73}, {0, 0, 255, 211}}};
	ArrayValue Palette(std::span<const Colour> colors = COLORS) {
		ArrayValue value{ValueType::Colour, {}};
		for (const auto &color : colors)
			value.Elements.emplace_back(color);
		return value;
	}
	float Random(float u, float v, float seed) {
		const float modulo = seed - std::floor(seed / 100000.f) * 100000.f;
		const float offset = modulo / 10.f;
		const float phase = (u + offset) * 853.98598f + (v + offset) * 78.2345543f;
		const float value = std::sin(phase) * 47.687523f;
		return value - std::floor(value);
	}
	SurfacePixel RawSample(const Image &image, float u, float v) {
		SurfacePixel value;
		const auto x = uint32_t(std::clamp(std::floor(double(u) * image.Width), 0., double(image.Width - 1)));
		const auto y =
			uint32_t(std::clamp(std::floor(double(v) * image.Height), 0., double(image.Height - 1)));
		REQUIRE(LoadSurfacePixel(image, x, y, value));
		for (size_t channel = 0; channel < 4; ++channel)
			value[channel] = float(value[channel]);
		return value;
	}
	Image Oracle(
		const Image &source,
		std::span<const Colour> colors = COLORS,
		double threshold = .1,
		double seed = 2,
		double frame = 0,
		Vector2 chance = {1, 1},
		const Image *map = nullptr
	) {
		Image out{
			source.Width, source.Height, std::vector<uint8_t>(size_t(source.Width) * source.Height * 4), 0
		};
		for (uint32_t y = 0; y < source.Height; ++y)
			for (uint32_t x = 0; x < source.Width; ++x) {
				const float u = (float(x) + .5f) / source.Width, v = (float(y) + .5f) / source.Height;
				auto color = RawSample(source, u, v);
				float probability = float(chance.X);
				if (map) {
					const auto pixel = RawSample(*map, u, v);
					const float mean = (float(pixel[0]) + float(pixel[1]) + float(pixel[2])) / 3.f;
					probability = float(chance.X) * (1.f - mean) + float(chance.Y) * mean;
				}
				if (Random(u, v, float(seed + frame)) <= probability)
					for (size_t i = 0; i + 1 < colors.size(); ++i) {
						const float dx = float(color[0]) - float(colors[i].Red) / 255.f;
						const float dy = float(color[1]) - float(colors[i].Green) / 255.f;
						const float dz = float(color[2]) - float(colors[i].Blue) / 255.f;
						if (std::sqrt(dx * dx + dy * dy + dz * dz) <= float(threshold)) {
							const auto next = colors[i + 1];
							color = {
								float(next.Red) / 255.f,
								float(next.Green) / 255.f,
								float(next.Blue) / 255.f,
								float(next.Alpha) / 255.f
							};
							break;
						}
					}
				REQUIRE(StoreSurfacePixel(out, x, y, color));
			}
		return out;
	}
	void SamePixels(const Image &actual, const Image &expected) {
		REQUIRE(actual.Width == expected.Width);
		REQUIRE(actual.Height == expected.Height);
		REQUIRE(actual.Format == expected.Format);
		REQUIRE(actual.Pixels.size() == expected.Pixels.size());
		for (size_t byte = 0; byte < actual.Pixels.size(); ++byte) {
			INFO("byte " << byte);
			CHECK(std::abs(int(actual.Pixels[byte]) - int(expected.Pixels[byte])) <= 1);
		}
	}
	struct Graph {
		Document Doc;
		Image Source = Labels();
		std::optional<Image> Map;
		void UseMap(Image image) {
			Map = std::move(image);
			Doc.Nodes.push_back({"map", "image.captured", "", {}, {{"source_id", std::string{"map"}}}});
			Doc.Links.push_back({"map", "image", "markov", "replace_chance_map"});
		}
		Graph() {
			Doc.FormatVersion = 9;
			Doc.Nodes = {
				{"source", "image.captured", "", {}, {{"source_id", std::string{"source"}}}},
				{"markov", "pc.markov_gradient", "", {}, {{"seed", 2.}, {"colors", Palette()}}}
			};
			Doc.Links = {{"source", "image", "markov", "surface_in"}};
			Doc.Outputs = {{"out", "markov", "surface_out"}};
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
			std::vector<RequestImageSource> sources{{"source", Source}};
			if (Map) sources.push_back({"map", *Map});
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
			std::vector<RequestImageSource> sources{{"source", Source}};
			if (Map) sources.push_back({"map", *Map});
			request.ImageSources = sources;
			ImageArray output;
			const auto status = EvaluateArray(Doc, plan, "out", request, output, error);
			INFO(error.Message << " " << error.NodeId << ":" << error.Port);
			REQUIRE(status == Status::Ok);
			return output;
		}
	};
}
using namespace source_markov_gradient_test;

TEST_CASE(
	"Markov Gradient replaces the first RGB match with exactly the next RGBA entry",
	"[imagegraph][markov_gradient]"
) {
	Graph graph;
	SamePixels(graph.Run(), Oracle(graph.Source));
	const auto output = graph.Run();
	SurfacePixel first, second, last;
	REQUIRE(LoadSurfacePixel(output, 0, 0, first));
	REQUIRE(LoadSurfacePixel(output, 1, 0, second));
	REQUIRE(LoadSurfacePixel(output, 2, 0, last));
	CHECK(first[1] == 1);
	CHECK(first[3] == Catch::Approx(73. / 255));
	CHECK(second[2] == 1);
	CHECK(second[3] == Catch::Approx(211. / 255));
	CHECK(last[2] == 1);
	CHECK(last[3] == Catch::Approx(.6).margin(1. / 255));
	const std::array<Colour, 3> duplicates{{COLORS[0], COLORS[0], COLORS[1]}};
	graph.Set("colors", Palette(duplicates));
	SamePixels(graph.Run(), Oracle(graph.Source, duplicates));
	const auto duplicate = graph.Run();
	SurfacePixel unchanged;
	REQUIRE(LoadSurfacePixel(duplicate, 0, 0, unchanged));
	CHECK(unchanged[0] == 1);
	CHECK(unchanged[1] == 0);
	CHECK(unchanged[3] == Catch::Approx(17. / 255));
}

TEST_CASE(
	"Markov Gradient RGB distance ignores alpha and includes the raw threshold boundary",
	"[imagegraph][markov_gradient]"
) {
	Graph graph;
	graph.Source = Image{1, 1, std::vector<uint8_t>(16), 0, SurfaceFormat::RGBA32Float};
	REQUIRE(StoreSurfacePixel(graph.Source, 0, 0, {0, 0, 0, .7}));
	for (double threshold : {-1., 0., double(std::nextafter(1.f, 0.f)), 1., 2.}) {
		CAPTURE(threshold);
		graph.Set("threshold", threshold);
		SamePixels(graph.Run(), Oracle(graph.Source, COLORS, threshold));
	}
	graph.Set("threshold", 0.);
	for (double alpha : {0., .4, 1.}) {
		REQUIRE(StoreSurfacePixel(graph.Source, 0, 0, {1, 0, 0, alpha}));
		SamePixels(graph.Run(), Oracle(graph.Source, COLORS, 0));
	}
}

TEST_CASE(
	"Markov Gradient raw chance and negative floor modulo include probability equality",
	"[imagegraph][markov_gradient]"
) {
	Graph graph;
	for (double seed : {-100001.25, -2.5, 0., 2., 100002.5})
		for (double chance : {-1., 0., .25, .5, 1., 2.}) {
			CAPTURE(seed, chance);
			graph.Set("seed", seed);
			graph.Set("replace_chance", chance);
			SamePixels(graph.Run(), Oracle(graph.Source, COLORS, .1, seed, 0, {chance, chance}));
		}
	graph.Source = Image{1, 1, std::vector<uint8_t>(16), 0, SurfaceFormat::RGBA32Float};
	REQUIRE(StoreSurfacePixel(graph.Source, 0, 0, {1, 0, 0, .2}));
	graph.Set("seed", 2.);
	const float boundary = Random(.5f, .5f, 2);
	graph.Set("replace_chance", double(boundary));
	SamePixels(graph.Run(), Oracle(graph.Source, COLORS, .1, 2, 0, {boundary, boundary}));
	SurfacePixel replaced;
	REQUIRE(LoadSurfacePixel(graph.Run(), 0, 0, replaced));
	CHECK(replaced[1] == 1);
	graph.Set("replace_chance", double(std::nextafter(boundary, -std::numeric_limits<float>::infinity())));
	SurfacePixel preserved;
	REQUIRE(LoadSurfacePixel(graph.Run(), 0, 0, preserved));
	CHECK(preserved[0] == 1);
}

TEST_CASE(
	"Markov Gradient source global frame includes subframes and signed time without row seed offsets",
	"[imagegraph][markov_gradient]"
) {
	Graph graph;
	graph.Set("replace_chance", .5);
	graph.Doc.Timeline = TimelineSettings{11, 0, 10, "loop", 11};
	EvaluationRequest request;
	for (const auto &[tick, subframe] :
		 std::array<std::pair<uint64_t, double>, 3>{{{0, 0}, {4, .5}, {14, .25}}}) {
		request.Tick = tick;
		request.Subframe = subframe;
		SamePixels(
			graph.Run(request), Oracle(graph.Source, COLORS, .1, 2, double(tick) + subframe, {.5, .5})
		);
	}
	request.Tick = 2;
	request.Subframe = .25;
	request.NegativeFrame = true;
	SamePixels(graph.Run(request), Oracle(graph.Source, COLORS, .1, 2, -2.25, {.5, .5}));
	request = {};
	graph.Set("seed", ArrayValue{ValueType::Scalar, {2., 2.}});
	const auto rows = graph.Rows(request);
	REQUIRE(rows.Images.size() == 2);
	SamePixels(rows.Images[0], Oracle(graph.Source, COLORS, .1, 2, 0, {.5, .5}));
	CHECK(rows.Images[0] == rows.Images[1]);
}

TEST_CASE(
	"Markov Gradient map samples nearest raw RGB ignoring alpha and red only swizzle",
	"[imagegraph][markov_gradient]"
) {
	for (const auto format : {SurfaceFormat::RGBA32Float, SurfaceFormat::R32Float}) {
		const auto layout = CheckedSurfaceLayout(2, 1, format, Limits::MaximumEvaluationBytes);
		REQUIRE(layout);
		Image map{2, 1, std::vector<uint8_t>(layout->Bytes), 0, format};
		REQUIRE(StoreSurfacePixel(map, 0, 0, {1.5, -.2, .2, 0}));
		REQUIRE(StoreSurfacePixel(map, 1, 0, {.2, .8, 1.4, 1}));
		Graph graph;
		graph.UseMap(map);
		graph.Set("replace_chance_mapped", true);
		graph.Set("replace_chance_map_range", Vector2{-.2, .8});
		SamePixels(graph.Run(), Oracle(graph.Source, COLORS, .1, 2, 0, {-.2, .8}, &map));
		graph.Set("replace_chance_mapped", false);
		SamePixels(graph.Run(), Oracle(graph.Source));
		graph.Set("replace_chance_mapped", true);
		graph.Doc.Junctions = {{"chance", "", ValueType::Scalar, .4}};
		graph.Doc.Links.push_back({"chance", "value", "markov", "replace_chance"});
		SamePixels(graph.Run(), Oracle(graph.Source, COLORS, .1, 2, 0, {.4, .4}, &map));
		graph.Doc.Junctions[0].Type = ValueType::Vector2;
		graph.Doc.Junctions[0].Default = Vector2{.1, .7};
		SamePixels(graph.Run(), Oracle(graph.Source, COLORS, .1, 2, 0, {.1, .7}, &map));
	}
	Graph graph;
	graph.Set("replace_chance_mapped", true);
	graph.Set("replace_chance_map_range", Vector2{0, .5});
	SamePixels(graph.Run(), Oracle(graph.Source, COLORS, .1, 2, 0, {0, .5}));
}

TEST_CASE(
	"Markov Gradient active output is RGBA8 and red surfaces remain raw channels",
	"[imagegraph][markov_gradient]"
) {
	for (const auto format :
		 {SurfaceFormat::RGBA4Unorm,
		  SurfaceFormat::RGBA8Unorm,
		  SurfaceFormat::RGBA16Float,
		  SurfaceFormat::RGBA32Float,
		  SurfaceFormat::R8Unorm,
		  SurfaceFormat::R16Float,
		  SurfaceFormat::R32Float}) {
		Graph graph;
		graph.Source = Labels(format);
		SamePixels(graph.Run(), Oracle(graph.Source));
		graph.Set("replace_chance", -1.);
		SamePixels(graph.Run(), Oracle(graph.Source, COLORS, .1, 2, 0, {-1, -1}));
		graph.Set("attribute_process", false);
		SamePixels(graph.Run(), Oracle(graph.Source, COLORS, .1, 2, 0, {-1, -1}));
	}
}

TEST_CASE(
	"Markov Gradient omitted palette uses durable project colors and authored colors take precedence",
	"[imagegraph][markov_gradient]"
) {
	Graph graph;
	graph.Doc.Project.emplace();
	const std::array<Colour, 2> project{{COLORS[0], Colour{255, 255, 255, 101}}};
	graph.Doc.Project->Palette.assign(project.begin(), project.end());
	graph.Doc.Nodes[1].Values = {{"seed", 2.}};
	SamePixels(graph.Run(), Oracle(graph.Source, project));
	Document restored;
	Diagnostic error;
	REQUIRE(Read(Write(graph.Doc), restored, error) == Status::Ok);
	graph.Doc = std::move(restored);
	SamePixels(graph.Run(), Oracle(graph.Source, project));
	graph.Set("colors", Palette());
	SamePixels(graph.Run(), Oracle(graph.Source));
	graph.Doc.Junctions = {{"palette", "", ValueType::Array, Palette(project)}};
	graph.Doc.Links.push_back({"palette", "value", "markov", "colors"});
	SamePixels(graph.Run(), Oracle(graph.Source, project));
}

TEST_CASE(
	"Markov Gradient palette depth one is data and depth two selects full rows",
	"[imagegraph][markov_gradient]"
) {
	const std::array<Colour, 2> second{{COLORS[0], Colour{255, 255, 0, 129}}};
	for (int64_t process = 0; process < 4; ++process) {
		Graph graph;
		SamePixels(graph.Run(), Oracle(graph.Source));
		graph.Set(
			"colors",
			ArrayValue{ValueType::Colour, {}, {{COLORS[0], COLORS[1], COLORS[2]}, {second[0], second[1]}}}
		);
		graph.Set("attribute_array_process", EnumValue{process});
		const auto rows = graph.Rows();
		REQUIRE(rows.Images.size() == 2);
		SamePixels(rows.Images[0], Oracle(graph.Source));
		SamePixels(rows.Images[1], Oracle(graph.Source, second));
	}
	Graph graph;
	graph.Set("colors", ArrayValue{ValueType::Colour, {}, {{COLORS[0], COLORS[1]}, {second[0], second[1]}}});
	graph.Set("attribute_process", false);
	Diagnostic error;
	const auto plan = graph.CompileNow(error);
	const std::array sources{RequestImageSource{"source", graph.Source}};
	EvaluationRequest request;
	request.ImageSources = sources;
	ImageArray retained;
	retained.Images = {graph.Source};
	const auto before = retained;
	CHECK(EvaluateArray(graph.Doc, plan, "out", request, retained, error) == Status::UnsupportedExecution);
	CHECK(retained.Images == before.Images);
}

TEST_CASE(
	"Markov Gradient scalar packed palettes wrap and typed packed arrays retain alpha distinction",
	"[imagegraph][markov_gradient]"
) {
	Image source{1, 1, std::vector<uint8_t>(16), 0, SurfaceFormat::RGBA32Float};
	REQUIRE(StoreSurfacePixel(source, 0, 0, {1, 1, 1, .4}));
	for (const Value &value : {Value{double{16777215}}, Value{int64_t{16777215}}}) {
		const auto *entry = FindCatalogueEntry("pc.markov_gradient");
		REQUIRE(entry);
		Node node{"markov", "pc.markov_gradient", "", {}, {}};
		EvaluationRequest request;
		detail::NodeContext context(node, *entry, request);
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		context.Images = {{"surface_in", &source}};
		context.Values = {
			{"active", true}, {"seed", 2.}, {"colors", value}, {"threshold", .1}, {"replace_chance", 1.}
		};
		context.InputProvenanceResolved = true;
		const bool processed = detail::RunProcessorBatch(context, detail::FindExecutor("pc.markov_gradient"));
		INFO(context.FailureMessage << " " << context.FailurePort);
		REQUIRE(processed);
		REQUIRE(context.OutputImages.size() == 1);
		const std::array<Colour, 1> singleton{{{255, 255, 255, 255}}};
		SamePixels(context.OutputImages[0].second, Oracle(source, singleton));
	}
	for (bool integer : {false, true}) {
		Graph graph;
		graph.Source = source;
		graph.Set(
			"colors",
			integer ? ArrayValue{ValueType::Integer, {int64_t{16777215}, int64_t{65280}}}
					: ArrayValue{ValueType::Scalar, {double{16777215}, double{65280}}}
		);
		const std::array<Colour, 2> expected{
			{{255, 255, 255, uint8_t(integer ? 0 : 255)}, {0, 255, 0, uint8_t(integer ? 0 : 255)}}
		};
		SamePixels(graph.Run(), Oracle(source, expected));
		SurfacePixel pixel;
		REQUIRE(LoadSurfacePixel(graph.Run(), 0, 0, pixel));
		CHECK(pixel[3] == (integer ? 0 : 1));
	}
}

TEST_CASE(
	"Markov Gradient palette bounds admit one and 256 but refuse empty and larger",
	"[imagegraph][markov_gradient]"
) {
	const auto source = Labels();
	for (size_t count : {size_t{1}, size_t{256}}) {
		const std::vector<Colour> colors(count, COLORS[0]);
		const auto run = imagegraph_test::RunNode(
			"pc.markov_gradient", {{"surface_in", &source}}, {{"seed", 2.}, {"colors", Palette(colors)}}
		);
		INFO(run.Message << " " << run.Port);
		REQUIRE(run.Ok);
		SamePixels(run.Output(), Oracle(source, colors));
	}
	for (size_t count : {size_t{0}, size_t{257}}) {
		const std::vector<Colour> colors(count, COLORS[0]);
		const auto run = imagegraph_test::RunNode(
			"pc.markov_gradient", {{"surface_in", &source}}, {{"seed", 2.}, {"colors", Palette(colors)}}
		);
		CHECK_FALSE(run.Ok);
		CHECK(run.Code == (count == 0 ? Status::UnsupportedExecution : Status::LimitExceeded));
		CHECK(run.Port == "colors");
		CHECK(run.Images.empty());
	}
}

TEST_CASE(
	"Markov Gradient grouped fractional animation remains distinct from global seed time",
	"[imagegraph][markov_gradient]"
) {
	Graph graph;
	graph.Doc.Groups = {Group{"group", "Group"}};
	for (auto &node : graph.Doc.Nodes)
		node.GroupId = "group";
	graph.Doc.Timeline = TimelineSettings{11, 0, 10, "loop", 11};
	graph.Doc.Nodes[1].SourceAnimatedInputs = {"replace_chance"};
	graph.Doc.Keyframes = {
		{"markov", "replace_chance", 0, 0., "linear"}, {"markov", "replace_chance", 10, 1., "linear"}
	};
	EvaluationRequest request;
	request.Tick = 4;
	request.Subframe = .5;
	SamePixels(graph.Run(request), Oracle(graph.Source, COLORS, .1, 2, 4.5, {.45, .45}));
	Document restored;
	Diagnostic error;
	REQUIRE(Read(Write(graph.Doc), restored, error) == Status::Ok);
	graph.Doc = std::move(restored);
	SamePixels(graph.Run(request), Oracle(graph.Source, COLORS, .1, 2, 4.5, {.45, .45}));
}

TEST_CASE(
	"Markov Gradient inactive clones the whole nested surface array exactly once",
	"[imagegraph][markov_gradient]"
) {
	ImageArray sources;
	sources.Images = {Labels(SurfaceFormat::RGBA16Float), Labels(SurfaceFormat::R32Float)};
	sources.Items = {{std::vector<ImageArrayItem>{{size_t{1}}, {size_t{0}}}}, {size_t{1}}};
	ImageArray expected;
	expected.Images = {sources.Images[1], sources.Images[0], sources.Images[1]};
	expected.Items = {{std::vector<ImageArrayItem>{{size_t{0}}, {size_t{1}}}}, {size_t{2}}};
	const auto *entry = FindCatalogueEntry("pc.markov_gradient");
	REQUIRE(entry);
	for (const Value &active : {Value{false}, Value{double{.5}}, Value{double{-1}}}) {
		Node node{"markov", "pc.markov_gradient", "", {}, {}};
		EvaluationRequest request;
		detail::NodeContext context(node, *entry, request);
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		context.ImageArrays = {{"surface_in", &sources}};
		context.Values = {
			{"active", active},
			{"attribute_process", false},
			{"colors", ArrayValue{ValueType::Colour, {}}},
			{"threshold", std::numeric_limits<double>::quiet_NaN()},
			{"replace_chance", ArrayValue{ValueType::Scalar, {0., 1.}}}
		};
		context.InputProvenanceResolved = true;
		size_t observed = 0;
		const auto observer = [](detail::NodeContext &, void *state) {
			++*static_cast<size_t *>(state);
			return true;
		};
		const bool processed = detail::RunProcessorBatch(
			context, detail::FindExecutor("pc.markov_gradient"), observer, &observed
		);
		INFO(context.FailureMessage << " " << context.FailurePort);
		REQUIRE(processed);
		CHECK(observed == 1);
		REQUIRE(context.OutputImageArrays.size() == 1);
		CHECK(context.OutputImageArrays[0].second.Images == expected.Images);
		CHECK(context.OutputImageArrays[0].second.Items == expected.Items);
	}
	for (const auto limit : {uint32_t{0}, uint32_t{3}, uint32_t(Limits::MaximumDimension + 1)}) {
		Node node{"markov", "pc.markov_gradient", "", {}, {}};
		EvaluationRequest request;
		request.MaximumImageDimension = limit;
		detail::NodeContext context(node, *entry, request);
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		context.ImageArrays = {{"surface_in", &sources}};
		context.Values = {{"active", false}};
		context.InputProvenanceResolved = true;
		CHECK_FALSE(detail::RunProcessorBatch(context, detail::FindExecutor("pc.markov_gradient")));
		CHECK(context.FailureCode == (limit == 3 ? Status::LimitExceeded : Status::InvalidValue));
		CHECK(context.OutputImageArrays.empty());
	}
}

TEST_CASE(
	"Markov Gradient Active rejects arrays and finite numbers use the pinned HTML5 boundary",
	"[imagegraph][markov_gradient]"
) {
	const auto source = Labels();
	for (double active : {0., .5, std::nextafter(.5, 1.), 1.}) {
		const auto run = imagegraph_test::RunNode(
			"pc.markov_gradient",
			{{"surface_in", &source}},
			{{"active", active}, {"seed", 2.}, {"colors", Palette()}}
		);
		INFO(run.Message << " " << run.Port);
		REQUIRE(run.Ok);
		if (active > .5)
			SamePixels(run.Output(), Oracle(source));
		else
			CHECK(run.Output() == source);
	}
	const auto activeArray = imagegraph_test::RunNode(
		"pc.markov_gradient",
		{{"surface_in", &source}},
		{{"active", ArrayValue{ValueType::Boolean, {false, true}}}, {"seed", 2.}, {"colors", Palette()}}
	);
	CHECK_FALSE(activeArray.Ok);
	CHECK(activeArray.Code == Status::UnsupportedExecution);
	CHECK(activeArray.Port == "active");
	CHECK(activeArray.Images.empty());
	const auto missingSeed =
		imagegraph_test::RunNode("pc.markov_gradient", {{"surface_in", &source}}, {{"colors", Palette()}});
	CHECK_FALSE(missingSeed.Ok);
	CHECK(missingSeed.Code == Status::UnsupportedExecution);
	CHECK(missingSeed.Port == "seed");
	CHECK(missingSeed.Images.empty());
}

TEST_CASE(
	"Markov Gradient refuses unavailable bindings and nonfinite controls before output",
	"[imagegraph][markov_gradient]"
) {
	const auto source = Labels();
	const auto missing =
		imagegraph_test::RunNode("pc.markov_gradient", {}, {{"seed", 2.}, {"colors", Palette()}});
	CHECK_FALSE(missing.Ok);
	CHECK(missing.Code == Status::UnsupportedExecution);
	CHECK(missing.Port == "surface_in");
	AtlasValue atlas;
	atlas.Data.emplace().Kind = AtlasKind::SurfaceAtlas;
	atlas.Data->Surface.Data = source;
	atlas.Data->Dimension = {1, 1};
	for (const std::string port : {"surface_in", "replace_chance_map"}) {
		const auto run = imagegraph_test::RunNode(
			"pc.markov_gradient",
			{{"surface_in", &source}},
			{{"seed", 2.}, {"colors", Palette()}, {"replace_chance_mapped", true}, {port, atlas}}
		);
		CHECK_FALSE(run.Ok);
		CHECK(run.Code == Status::UnsupportedExecution);
		CHECK(run.Port == port);
		CHECK(run.Images.empty());
	}
	for (const std::string port : {"seed", "threshold", "replace_chance"})
		for (double value :
			 {std::numeric_limits<double>::infinity(),
			  std::numeric_limits<double>::quiet_NaN(),
			  std::numeric_limits<double>::max()}) {
			const auto run = imagegraph_test::RunNode(
				"pc.markov_gradient",
				{{"surface_in", &source}},
				{{"colors", Palette()},
				 {"seed", port == "seed" ? value : 2.},
				 {"threshold", port == "threshold" ? value : .1},
				 {"replace_chance", port == "replace_chance" ? value : 1.}}
			);
			INFO(run.Message << " " << run.Port);
			CHECK_FALSE(run.Ok);
			CHECK(run.Code == Status::InvalidValue);
			CHECK(run.Port == port);
			CHECK(run.Images.empty());
		}
	const auto activeNaN = imagegraph_test::RunNode(
		"pc.markov_gradient",
		{{"surface_in", &source}},
		{{"active", std::numeric_limits<double>::quiet_NaN()}}
	);
	CHECK_FALSE(activeNaN.Ok);
	CHECK(activeNaN.Code == Status::InvalidValue);
	CHECK(activeNaN.Port == "active");
}

TEST_CASE(
	"Markov Gradient sampled nonfinite pixels and finite arithmetic overflow refuse atomically",
	"[imagegraph][markov_gradient]"
) {
	const auto source = Labels();
	const auto refusal = [](const imagegraph_test::NodeRun &run, std::string_view port) {
		INFO(run.Message << " " << run.Port);
		CHECK_FALSE(run.Ok);
		CHECK(run.Code == Status::InvalidValue);
		CHECK(run.Port == port);
		CHECK(run.Images.empty());
	};
	for (float invalid : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
		auto badSource = Labels();
		std::memcpy(badSource.Pixels.data(), &invalid, sizeof(invalid));
		refusal(
			imagegraph_test::RunNode(
				"pc.markov_gradient", {{"surface_in", &badSource}}, {{"seed", 2.}, {"colors", Palette()}}
			),
			"surface_in"
		);
		Image map{1, 1, std::vector<uint8_t>(16), 0, SurfaceFormat::RGBA32Float};
		REQUIRE(StoreSurfacePixel(map, 0, 0, {1, 1, 1, 1}));
		std::memcpy(map.Pixels.data(), &invalid, sizeof(invalid));
		refusal(
			imagegraph_test::RunNode(
				"pc.markov_gradient",
				{{"surface_in", &source}, {"replace_chance_map", &map}},
				{{"seed", 2.}, {"colors", Palette()}, {"replace_chance_mapped", true}}
			),
			"replace_chance_map"
		);
	}
	Image hdr{1, 1, std::vector<uint8_t>(16), 0, SurfaceFormat::RGBA32Float};
	REQUIRE(StoreSurfacePixel(hdr, 0, 0, {1e20, 1e20, 1e20, 1}));
	refusal(
		imagegraph_test::RunNode(
			"pc.markov_gradient", {{"surface_in", &hdr}}, {{"seed", 2.}, {"colors", Palette()}}
		),
		"surface_in"
	);
	const float maximum = std::numeric_limits<float>::max();
	Image map{1, 1, std::vector<uint8_t>(16), 0, SurfaceFormat::RGBA32Float};
	REQUIRE(StoreSurfacePixel(map, 0, 0, {maximum, maximum, maximum, 1}));
	refusal(
		imagegraph_test::RunNode(
			"pc.markov_gradient",
			{{"surface_in", &source}, {"replace_chance_map", &map}},
			{{"seed", 2.}, {"colors", Palette()}, {"replace_chance_mapped", true}}
		),
		"replace_chance"
	);
	REQUIRE(StoreSurfacePixel(map, 0, 0, {2, 2, 2, 1}));
	refusal(
		imagegraph_test::RunNode(
			"pc.markov_gradient",
			{{"surface_in", &source}, {"replace_chance_map", &map}},
			{{"seed", 2.},
			 {"colors", Palette()},
			 {"replace_chance_mapped", true},
			 {"replace_chance", Vector2{maximum, -maximum}}}
		),
		"replace_chance"
	);
}

TEST_CASE(
	"Markov Gradient whole batch work bytes and later refusals publish no rows",
	"[imagegraph][markov_gradient]"
) {
	const auto *entry = FindCatalogueEntry("pc.markov_gradient");
	REQUIRE(entry);
	Node node{"markov", "pc.markov_gradient", "", {}, {}};
	EvaluationRequest request;
	detail::EvaluationBudget budget(Limits::MaximumEvaluationBytes);
	ImageArray sources;
	sources.Images = {
		Image{500, 500, std::vector<uint8_t>(500 * 500 * 4), 0},
		Image{500, 500, std::vector<uint8_t>(500 * 500 * 4), 0}
	};
	sources.Items = {{size_t{0}}, {size_t{1}}};
	{
		detail::NodeContext context(node, *entry, request, budget);
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		context.ImageArrays = {{"surface_in", &sources}};
		context.Values = {
			{"active", true}, {"seed", 2.}, {"colors", Palette()}, {"threshold", .1}, {"replace_chance", 1.}
		};
		Status expected = Status::LimitExceeded;
		SECTION("two individually admissible rows exceed aggregate comparison work") {}
		SECTION("complete outputs exceed aggregate bytes") {
			context.ByteBudget = 700000;
			sources.Images = {
				Image{300, 300, std::vector<uint8_t>(300 * 300 * 4), 0},
				Image{300, 300, std::vector<uint8_t>(300 * 300 * 4), 0}
			};
		}
		SECTION("last selected seed is nonfinite") {
			sources.Images = {Labels(), Labels()};
			context.Values[1].second =
				ArrayValue{ValueType::Scalar, {2., std::numeric_limits<double>::infinity()}};
			expected = Status::InvalidValue;
		}
		SECTION("last selected palette is empty") {
			sources.Images = {Labels(), Labels()};
			context.Values[2].second = ArrayValue{ValueType::Colour, {}, {{COLORS[0], COLORS[1]}, {}}};
			expected = Status::UnsupportedExecution;
		}
		SECTION("last source has malformed storage") {
			sources.Images = {Labels(), Image{2, 2, {1, 2, 3, 4}}};
			expected = Status::InvalidValue;
		}
		context.InputProvenanceResolved = true;
		size_t observed = 0;
		const auto observer = [](detail::NodeContext &, void *state) {
			++*static_cast<size_t *>(state);
			return true;
		};
		const bool processed = detail::RunProcessorBatch(
			context, detail::FindExecutor("pc.markov_gradient"), observer, &observed
		);
		INFO(context.FailureMessage << " " << context.FailurePort);
		CHECK_FALSE(processed);
		CHECK(context.FailureCode == expected);
		CHECK(observed == 0);
		CHECK(context.OutputImages.empty());
		CHECK(context.OutputImageArrays.empty());
	}
	CHECK(budget.Used() == 0);
}

TEST_CASE(
	"Markov Gradient active and inactive caller outputs survive request refusal",
	"[imagegraph][markov_gradient]"
) {
	Graph graph;
	Diagnostic error;
	EvaluationRequest request;
	const std::array sources{RequestImageSource{"source", graph.Source}};
	request.ImageSources = sources;
	for (bool active : {true, false}) {
		graph.Set("active", active);
		const auto plan = graph.CompileNow(error);
		Image retained{1, 1, {17, 18, 19, 20}};
		const auto before = retained;
		CHECK(Evaluate(graph.Doc, plan, "out", request, retained, error, 1) == Status::LimitExceeded);
		CHECK(retained == before);
		request.MaximumImageDimension = 3;
		CHECK(Evaluate(graph.Doc, plan, "out", request, retained, error) == Status::InvalidValue);
		CHECK(error.NodeId == "source");
		CHECK(error.Port == "source_id");
		CHECK(retained == before);
		request.MaximumImageDimension = Limits::MaximumDimension;
	}
}
