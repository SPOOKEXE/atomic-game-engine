#include "../src/ProcessorBatch.hpp"
#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstring>
#include <limits>

TEST_SUITE_ID("engine.imagegraph.source_cross_section")
using namespace engine::imagegraph;
namespace source_cross_section_test {
	Image Labels(SurfaceFormat format = SurfaceFormat::RGBA32Float) {
		const auto layout = CheckedSurfaceLayout(4, 3, format, Limits::MaximumEvaluationBytes);
		REQUIRE(layout);
		Image image{4, 3, std::vector<uint8_t>(layout->Bytes), 0, format};
		for (uint32_t y = 0; y < 3; ++y)
			for (uint32_t x = 0; x < 4; ++x)
				REQUIRE(StoreSurfacePixel(
					image, x, y, {.1 + .2 * x, .15 + .3 * y, .05 + .05 * (4 * y + x), .2 + .2 * x}
				));
		return image;
	}
	SurfacePixel Sample(const Image &image, float u, float v, bool linear) {
		const auto pixel = [&](int x, int y) {
			SurfacePixel result;
			REQUIRE(LoadSurfacePixel(
				image,
				uint32_t(std::clamp(x, 0, int(image.Width) - 1)),
				uint32_t(std::clamp(y, 0, int(image.Height) - 1)),
				result
			));
			return result;
		};
		if (!linear)
			return pixel(int(std::floor(double(u) * image.Width)), int(std::floor(double(v) * image.Height)));
		const double px = double(u) * image.Width - .5, py = double(v) * image.Height - .5;
		const int x = int(std::floor(px)), y = int(std::floor(py));
		const double ax = px - x, ay = py - y;
		const auto a = pixel(x, y), b = pixel(x + 1, y), c = pixel(x, y + 1), d = pixel(x + 1, y + 1);
		SurfacePixel result;
		for (size_t channel = 0; channel < 4; ++channel)
			result[channel] = (a[channel] * (1 - ax) + b[channel] * ax) * (1 - ay) +
							  (c[channel] * (1 - ax) + d[channel] * ax) * ay;
		return result;
	}
	Image Oracle(
		const Image &source,
		int axis = 0,
		double position = 0,
		int mode = 0,
		bool aa = false,
		bool toAlpha = false,
		Vector2 level = {0, 1},
		const Image *mask = nullptr
	) {
		Image base{
			source.Width, source.Height, std::vector<uint8_t>(size_t(source.Width) * source.Height * 4), 0
		};
		const bool grey = source.Format == SurfaceFormat::R8Unorm ||
						  source.Format == SurfaceFormat::R16Float ||
						  source.Format == SurfaceFormat::R32Float;
		for (uint32_t y = 0; y < source.Height; ++y)
			for (uint32_t x = 0; x < source.Width; ++x) {
				const float u = (float(x) + .5f) / float(source.Width),
							v = (float(y) + .5f) / float(source.Height);
				const auto sampled = Sample(
					source,
					grey		? u
					: axis == 0 ? u
								: float(position),
					grey		? v
					: axis == 0 ? float(position)
								: v,
					aa
				);
				SurfacePixel result;
				if (grey)
					result = {sampled[0], sampled[0], sampled[0], 1};
				else {
					const float luminance =
						float(sampled[0]) * .2126f + float(sampled[1]) * .7152f + float(sampled[2]) * .0722f;
					const float brightness = 1.f - luminance * float(sampled[3]);
					const float threshold = float(level.X) * (1.f - brightness) + float(level.Y) * brightness;
					const float bw = axis == 0 ? v : 1.f - u;
					const float res = bw >= threshold ? 1.f : 0.f;
					result = mode == 0 ? SurfacePixel{res, res, res, 1} : sampled;
					if (toAlpha) result[3] = res;
				}
				REQUIRE(StoreSurfacePixel(base, x, y, result));
			}
		if (mask)
			for (uint32_t y = 0; y < source.Height; ++y)
				for (uint32_t x = 0; x < source.Width; ++x) {
					SurfacePixel result;
					REQUIRE(LoadSurfacePixel(base, x, y, result));
					const auto m = Sample(
						*mask, (float(x) + .5f) / source.Width, (float(y) + .5f) / source.Height, false
					);
					const float alpha = (float(m[0]) + float(m[1]) + float(m[2])) / 3.f * float(m[3]);
					result[3] = float(result[3]) * alpha;
					REQUIRE(StoreSurfacePixel(base, x, y, result));
				}
		return base;
	}
	void SamePixels(const Image &actual, const Image &expected) {
		REQUIRE(actual.Width == expected.Width);
		REQUIRE(actual.Height == expected.Height);
		REQUIRE(actual.Format == SurfaceFormat::RGBA8Unorm);
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
				{"section", "pc.cross_section", "", {}, {}}
			};
			Doc.Links = {{"source", "image", "section", "surface_in"}};
			Doc.Outputs = {{"out", "section", "surface_out"}};
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
using namespace source_cross_section_test;

TEST_CASE(
	"Cross Section axes modes filtering alpha and raw levels follow pinned shader",
	"[imagegraph][cross_section]"
) {
	Graph graph;
	for (int axis = 0; axis < 2; ++axis)
		for (int mode = 0; mode < 2; ++mode)
			for (bool aa : {false, true})
				for (bool alpha : {false, true})
					for (const Vector2 level : {Vector2{0, 1}, Vector2{1, 0}, Vector2{-.4, 1.5}}) {
						CAPTURE(axis, mode, aa, alpha, level.X, level.Y);
						graph.Set("axis", EnumValue{axis});
						graph.Set("mode", EnumValue{mode});
						graph.Set("position", .41);
						graph.Set("anti_aliasing", aa);
						graph.Set("to_alpha", alpha);
						graph.Set("level", level);
						SamePixels(graph.Run(), Oracle(graph.Source, axis, .41, mode, aa, alpha, level));
					}
}

TEST_CASE(
	"Cross Section defaults depth ignored and endpoint sampling clamp are source behavior",
	"[imagegraph][cross_section]"
) {
	Graph graph;
	SamePixels(graph.Run(), Oracle(graph.Source));
	graph.Set("mode", EnumValue{1});
	for (bool aa : {false, true})
		for (double position : {-2., 0., 1., 2.}) {
			graph.Set("position", position);
			graph.Set("anti_aliasing", aa);
			SamePixels(graph.Run(), Oracle(graph.Source, 0, position, 1, aa));
		}
	graph.Set("position", .41);
	graph.Set("attribute_process", false);
	for (int64_t depth = 0; depth <= 8; ++depth) {
		graph.Set("attribute_color_depth", EnumValue{depth});
		SamePixels(graph.Run(), Oracle(graph.Source, 0, .41, 1, true));
	}
	graph.Set("anti_aliasing", false);
	const auto nearest = graph.Run();
	graph.Set("anti_aliasing", true);
	const auto linear = graph.Run();
	CHECK(nearest.Pixels != linear.Pixels);
}

TEST_CASE(
	"Cross Section threshold equality is included and alpha affects luminance", "[imagegraph][cross_section]"
) {
	Graph graph;
	graph.Source = Image{1, 1, std::vector<uint8_t>(16), 0, SurfaceFormat::RGBA32Float};
	REQUIRE(StoreSurfacePixel(graph.Source, 0, 0, {1, 1, 1, .5}));
	const auto equal = graph.Run();
	CHECK(equal.Pixels == std::vector<uint8_t>{255, 255, 255, 255});
	REQUIRE(StoreSurfacePixel(graph.Source, 0, 0, {1, 1, 1, 0}));
	const auto transparent = graph.Run();
	CHECK(transparent.Pixels == std::vector<uint8_t>{0, 0, 0, 255});
	graph.Set("mode", EnumValue{1});
	graph.Set("to_alpha", true);
	SamePixels(graph.Run(), Oracle(graph.Source, 0, 0, 1, false, true));
}

TEST_CASE(
	"Cross Section source formats include helper override for red only surfaces",
	"[imagegraph][cross_section]"
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
		for (int axis = 0; axis < 2; ++axis) {
			graph.Set("axis", EnumValue{axis});
			graph.Set("position", .41);
			graph.Set("mode", EnumValue{1});
			graph.Set("to_alpha", true);
			graph.Set("anti_aliasing", true);
			graph.Set("level", Vector2{1, 0});
			SamePixels(graph.Run(), Oracle(graph.Source, axis, .41, 1, true, true, {1, 0}));
		}
	}
}

TEST_CASE(
	"Cross Section masks raw RGB mean after base quantization and ignores alpha only metadata",
	"[imagegraph][cross_section]"
) {
	const auto source = Labels();
	for (const auto format : {SurfaceFormat::RGBA32Float, SurfaceFormat::R32Float}) {
		const auto layout = CheckedSurfaceLayout(2, 1, format, Limits::MaximumEvaluationBytes);
		REQUIRE(layout);
		Image mask{2, 1, std::vector<uint8_t>(layout->Bytes), 0, format};
		REQUIRE(StoreSurfacePixel(mask, 0, 0, {1.4, .1, .3, .61}));
		REQUIRE(StoreSurfacePixel(mask, 1, 0, {-.2, .8, 1.1, .37}));
		for (bool alphaOnly : {false, true}) {
			const auto run = imagegraph_test::RunNode(
				"pc.cross_section",
				{{"surface_in", &source}, {"mask", &mask}},
				{{"mode", EnumValue{1}},
				 {"position", .41},
				 {"anti_aliasing", true},
				 {"mask_alpha_only", alphaOnly}}
			);
			INFO(run.Message << " " << run.Port);
			REQUIRE(run.Ok);
			SamePixels(run.Output(), Oracle(source, 0, .41, 1, true, false, {0, 1}, &mask));
		}
	}
	Image precise{1, 1, std::vector<uint8_t>(16), 0, SurfaceFormat::RGBA32Float};
	Image mask{1, 1, std::vector<uint8_t>(16), 0, SurfaceFormat::RGBA32Float};
	REQUIRE(StoreSurfacePixel(precise, 0, 0, {.27, .39, .48, .498}));
	REQUIRE(StoreSurfacePixel(mask, 0, 0, {.5, .5, .5, 1}));
	const auto run = imagegraph_test::RunNode(
		"pc.cross_section", {{"surface_in", &precise}, {"mask", &mask}}, {{"mode", EnumValue{1}}}
	);
	REQUIRE(run.Ok);
	const auto expected = Oracle(precise, 0, 0, 1, false, false, {0, 1}, &mask);
	CHECK(run.Output().Pixels == expected.Pixels);
	CHECK(run.Output().Pixels[3] != uint8_t(std::lround(.498 * .5 * 255)));
}

TEST_CASE(
	"Cross Section linked controls and grouped animation preserve shader values",
	"[imagegraph][cross_section]"
) {
	Graph graph;
	graph.Doc.Junctions = {
		{"position", "", ValueType::Scalar, .41}, {"level", "", ValueType::Vector2, Vector2{1, 0}}
	};
	graph.Doc.Links.push_back({"position", "value", "section", "position"});
	graph.Doc.Links.push_back({"level", "value", "section", "level"});
	graph.Set("axis", EnumValue{1});
	graph.Set("to_alpha", true);
	SamePixels(graph.Run(), Oracle(graph.Source, 1, .41, 0, false, true, {1, 0}));
	graph.Doc.Junctions.clear();
	graph.Doc.Links.resize(1);
	graph.Doc.Timeline = TimelineSettings{11, 0, 10, "loop", 11};
	graph.Doc.Groups = {Group{"group", "Group"}};
	for (auto &node : graph.Doc.Nodes)
		node.GroupId = "group";
	graph.Doc.Nodes[1].SourceAnimatedInputs = {"position"};
	graph.Doc.Keyframes = {
		{"section", "position", 0, 0., "linear"}, {"section", "position", 10, 1., "linear"}
	};
	graph.Set("anti_aliasing", true);
	EvaluationRequest request;
	request.Tick = 4;
	request.Subframe = .5;
	SamePixels(graph.Run(request), Oracle(graph.Source, 1, .45, 0, true, true));
	Document restored;
	Diagnostic error;
	REQUIRE(Read(Write(graph.Doc), restored, error) == Status::Ok);
	graph.Doc = std::move(restored);
	SamePixels(graph.Run(request), Oracle(graph.Source, 1, .45, 0, true, true));
}

TEST_CASE(
	"Cross Section processor modes retain every selected axis and mode row", "[imagegraph][cross_section]"
) {
	for (int64_t process = 0; process < 4; ++process) {
		Graph graph;
		graph.Set("axis", ArrayValue{ValueType::Integer, {int64_t{0}, int64_t{1}}});
		graph.Set("mode", ArrayValue{ValueType::Integer, {int64_t{0}, int64_t{1}}});
		graph.Set("position", .41);
		graph.Set("attribute_array_process", EnumValue{process});
		const auto rows = graph.Rows();
		REQUIRE(rows.Images.size() == (process < 2 ? 2 : 4));
		for (size_t row = 0; row < rows.Images.size(); ++row) {
			const int axis = process < 2 ? int(row) : process == 2 ? int(row / 2) : int(row % 2);
			const int mode = process < 2 ? int(row) : process == 2 ? int(row % 2) : int(row / 2);
			SamePixels(rows.Images[row], Oracle(graph.Source, axis, .41, mode));
		}
	}
	Graph graph;
	graph.Set("axis", ArrayValue{ValueType::Integer, {int64_t{0}, int64_t{1}}});
	graph.Set("attribute_process", false);
	Diagnostic error;
	const auto plan = graph.CompileNow(error);
	EvaluationRequest request;
	const std::array sources{RequestImageSource{"source", graph.Source}};
	request.ImageSources = sources;
	ImageArray retained;
	retained.Images = {graph.Source};
	const auto before = retained;
	CHECK(EvaluateArray(graph.Doc, plan, "out", request, retained, error) == Status::UnsupportedExecution);
	CHECK(retained.Images == before.Images);
}

TEST_CASE(
	"Cross Section scalar choices clamp but array choices refuse undefined shader branches",
	"[imagegraph][cross_section]"
) {
	const auto source = Labels();
	for (const std::string port : {"axis", "mode"}) {
		for (const auto &[raw, normalized] : std::array<std::pair<double, int>, 2>{{{3, 1}, {-1, 0}}}) {
			const auto run =
				imagegraph_test::RunNode("pc.cross_section", {{"surface_in", &source}}, {{port, raw}});
			INFO(run.Message << " " << run.Port);
			REQUIRE(run.Ok);
			SamePixels(
				run.Output(),
				Oracle(source, port == "axis" ? normalized : 0, 0, port == "mode" ? normalized : 0)
			);
		}
		const auto fraction =
			imagegraph_test::RunNode("pc.cross_section", {{"surface_in", &source}}, {{port, .5}});
		CHECK_FALSE(fraction.Ok);
		CHECK(fraction.Code == Status::UnsupportedExecution);
		CHECK(fraction.Port == port);
		CHECK(fraction.Images.empty());
		const auto *entry = FindCatalogueEntry("pc.cross_section");
		REQUIRE(entry);
		for (double raw : {3., -1., .5}) {
			CAPTURE(port, raw);
			Node node{"section", "pc.cross_section", "", {}, {}};
			EvaluationRequest request;
			detail::NodeContext context(node, *entry, request);
			context.ByteBudget = Limits::MaximumEvaluationBytes;
			context.Images = {{"surface_in", &source}};
			for (const auto &input : entry->Inputs)
				if (const auto value = CatalogueDefault(input)) context.Values.emplace_back(input.Id, *value);
			for (auto &[id, value] : context.Values)
				if (id == port) value = ArrayValue{ValueType::Scalar, {raw}};
			context.InputProvenanceResolved = true;
			const bool processed =
				detail::RunProcessorBatch(context, detail::FindExecutor("pc.cross_section"));
			INFO(context.FailureMessage << " " << context.FailurePort);
			CHECK_FALSE(processed);
			CHECK(context.FailureCode == Status::UnsupportedExecution);
			CHECK(context.FailurePort == port);
			CHECK(context.OutputImages.empty());
			CHECK(context.OutputImageArrays.empty());
		}
	}
}

TEST_CASE(
	"Cross Section rejects missing malformed atlas and nonfinite data without output",
	"[imagegraph][cross_section]"
) {
	const auto source = Labels();
	const auto missing = imagegraph_test::RunNode("pc.cross_section", {});
	CHECK_FALSE(missing.Ok);
	CHECK(missing.Port == "surface_in");
	CHECK(missing.Images.empty());
	const Image malformed{2, 2, {1, 2, 3, 4}};
	const auto bad = imagegraph_test::RunNode("pc.cross_section", {{"surface_in", &malformed}});
	CHECK_FALSE(bad.Ok);
	CHECK(bad.Port == "surface_in");
	CHECK(bad.Images.empty());
	AtlasValue atlas;
	atlas.Data.emplace().Kind = AtlasKind::SurfaceAtlas;
	atlas.Data->Surface.Data = source;
	atlas.Data->Dimension = {1, 1};
	for (const std::string port : {"surface_in", "mask"}) {
		const auto run =
			imagegraph_test::RunNode("pc.cross_section", {{"surface_in", &source}}, {{port, atlas}});
		CHECK_FALSE(run.Ok);
		CHECK(run.Code == Status::UnsupportedExecution);
		CHECK(run.Port == port);
		CHECK(run.Images.empty());
	}
	for (double value :
		 {std::numeric_limits<double>::infinity(),
		  std::numeric_limits<double>::quiet_NaN(),
		  std::numeric_limits<double>::max()}) {
		const auto position =
			imagegraph_test::RunNode("pc.cross_section", {{"surface_in", &source}}, {{"position", value}});
		CHECK_FALSE(position.Ok);
		CHECK(position.Code == Status::InvalidValue);
		CHECK(position.Port == "position");
		CHECK(position.Images.empty());
		const auto level = imagegraph_test::RunNode(
			"pc.cross_section", {{"surface_in", &source}}, {{"level", Vector2{0, value}}}
		);
		CHECK_FALSE(level.Ok);
		CHECK(level.Code == Status::InvalidValue);
		CHECK(level.Port == "level");
		CHECK(level.Images.empty());
	}
	for (bool aa : {false, true}) {
		const auto extreme = imagegraph_test::RunNode(
			"pc.cross_section",
			{{"surface_in", &source}},
			{{"mode", EnumValue{1}},
			 {"position", double(std::numeric_limits<float>::max())},
			 {"anti_aliasing", aa}}
		);
		INFO(extreme.Message);
		REQUIRE(extreme.Ok);
		SamePixels(extreme.Output(), Oracle(source, 0, 2, 1, aa));
	}
}

TEST_CASE(
	"Cross Section whole batch work byte and malformed last row refuse before observers",
	"[imagegraph][cross_section]"
) {
	const auto *entry = FindCatalogueEntry("pc.cross_section");
	REQUIRE(entry);
	Node node{"section", "pc.cross_section", "", {}, {}};
	EvaluationRequest request;
	detail::EvaluationBudget budget(Limits::MaximumEvaluationBytes);
	ImageArray sources;
	const auto big = [] {
		Image image{600, 600, std::vector<uint8_t>(600 * 600 * 4), 0};
		for (size_t i = 3; i < image.Pixels.size(); i += 4)
			image.Pixels[i] = 255;
		return image;
	};
	sources.Images = {big(), big()};
	sources.Items = {{size_t{0}}, {size_t{1}}};
	{
		detail::NodeContext context(node, *entry, request, budget);
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		context.ImageArrays = {{"surface_in", &sources}};
		for (const auto &input : entry->Inputs)
			if (const auto value = CatalogueDefault(input)) context.Values.emplace_back(input.Id, *value);
		Status expected = Status::LimitExceeded;
		SECTION("aggregate work") {}
		SECTION("aggregate live output bytes") {
			context.ByteBudget = 700000;
			sources.Images = {
				Image{300, 300, std::vector<uint8_t>(300 * 300 * 4), 0},
				Image{300, 300, std::vector<uint8_t>(300 * 300 * 4), 0}
			};
		}
		SECTION("later invalid surface") {
			sources.Images = {Labels(), Image{2, 2, {1, 2, 3, 4}}};
			expected = Status::InvalidValue;
		}
		SECTION("later nonfinite control") {
			sources.Images = {Labels(), Labels()};
			for (auto &[id, value] : context.Values)
				if (id == "position")
					value = ArrayValue{ValueType::Scalar, {0., std::numeric_limits<double>::infinity()}};
			expected = Status::InvalidValue;
		}
		context.InputProvenanceResolved = true;
		size_t observed = 0;
		const auto observer = [](detail::NodeContext &, void *state) {
			++*static_cast<size_t *>(state);
			return true;
		};
		const bool processed =
			detail::RunProcessorBatch(context, detail::FindExecutor("pc.cross_section"), observer, &observed);
		INFO(context.FailureMessage << " " << context.FailurePort);
		CHECK_FALSE(processed);
		CHECK(context.FailureCode == expected);
		CHECK(observed == 0);
		CHECK(context.OutputImages.empty());
		CHECK(context.OutputImageArrays.empty());
	}
	CHECK(budget.Used() == 0);
}

TEST_CASE("Cross Section byte refusal preserves caller pixels", "[imagegraph][cross_section]") {
	Graph graph;
	Diagnostic error;
	const auto plan = graph.CompileNow(error);
	const std::array sources{RequestImageSource{"source", graph.Source}};
	EvaluationRequest request;
	request.ImageSources = sources;
	Image output{1, 1, {1, 2, 3, 4}};
	const auto before = output;
	CHECK(Evaluate(graph.Doc, plan, "out", request, output, error, 1) == Status::LimitExceeded);
	CHECK(output == before);
}

TEST_CASE(
	"Cross Section rejects sampled nonfinite texels and finite arithmetic overflow before publication",
	"[imagegraph][cross_section]"
) {
	const auto source = Labels();
	const auto requireRefusal = [](const imagegraph_test::NodeRun &run, std::string_view port) {
		INFO(run.Message << " " << run.Port);
		CHECK_FALSE(run.Ok);
		CHECK(run.Code == Status::InvalidValue);
		CHECK(run.Port == port);
		CHECK(run.Images.empty());
	};
	const Image malformed{2, 2, {1, 2, 3, 4}};
	requireRefusal(
		imagegraph_test::RunNode("pc.cross_section", {{"surface_in", &source}, {"mask", &malformed}}), "mask"
	);
	for (float value : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
		CAPTURE(value);
		auto invalidSource = Labels();
		std::memcpy(invalidSource.Pixels.data(), &value, sizeof(value));
		requireRefusal(
			imagegraph_test::RunNode("pc.cross_section", {{"surface_in", &invalidSource}}), "surface_in"
		);
		Image invalidMask{1, 1, std::vector<uint8_t>(16), 0, SurfaceFormat::RGBA32Float};
		REQUIRE(StoreSurfacePixel(invalidMask, 0, 0, {1, 1, 1, 1}));
		std::memcpy(invalidMask.Pixels.data(), &value, sizeof(value));
		requireRefusal(
			imagegraph_test::RunNode("pc.cross_section", {{"surface_in", &source}, {"mask", &invalidMask}}),
			"mask"
		);
	}
	Image hdr{1, 1, std::vector<uint8_t>(16), 0, SurfaceFormat::RGBA32Float};
	const double maximum = std::numeric_limits<float>::max();
	REQUIRE(StoreSurfacePixel(hdr, 0, 0, {maximum, maximum, maximum, maximum}));
	requireRefusal(imagegraph_test::RunNode("pc.cross_section", {{"surface_in", &hdr}}), "level");
	REQUIRE(StoreSurfacePixel(hdr, 0, 0, {2, 2, 2, 1}));
	requireRefusal(
		imagegraph_test::RunNode(
			"pc.cross_section", {{"surface_in", &hdr}}, {{"level", Vector2{maximum, -maximum}}}
		),
		"level"
	);
	Image hdrMask{1, 1, std::vector<uint8_t>(16), 0, SurfaceFormat::RGBA32Float};
	REQUIRE(StoreSurfacePixel(hdrMask, 0, 0, {1e20, 1e20, 1e20, 1e20}));
	requireRefusal(
		imagegraph_test::RunNode("pc.cross_section", {{"surface_in", &source}, {"mask", &hdrMask}}), "mask"
	);
	const auto flags = imagegraph_test::RunNode(
		"pc.cross_section",
		{{"surface_in", &source}},
		{{"mode", EnumValue{1}}, {"position", .41}, {"anti_aliasing", 1e-100}, {"to_alpha", 1e-100}}
	);
	INFO(flags.Message);
	REQUIRE(flags.Ok);
	SamePixels(flags.Output(), Oracle(source, 0, .41, 1, true, true));
}
