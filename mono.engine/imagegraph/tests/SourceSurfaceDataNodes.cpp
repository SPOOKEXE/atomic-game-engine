#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/HostCapture.hpp>
#include <engine/imagegraph/Surface.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bit>
#include <limits>
TEST_SUITE_ID("engine.imagegraph.source_surface_data")
using namespace engine::imagegraph;
namespace {
	struct SurfaceDataGraph {
		Document Authored;
		Plan Compiled;
		HostNodeCapture Capture;
		EvaluationRequest Request;
		Diagnostic Failure;
		SurfaceDataGraph(
			std::string_view type,
			Image image,
			std::vector<AuthoredValue> controls = {},
			std::string_view input = "surface_in",
			std::string_view output = "colors"
		) {
			Authored.FormatVersion = 9;
			Authored.Nodes = {
				{"source", "pc.image", "", {}, {}}, {"data", std::string(type), "", {}, std::move(controls)}
			};
			Authored.Links = {{"source", "surface_out", "data", std::string(input)}};
			Authored.Outputs = {{"result", "data", std::string(output)}};
			REQUIRE(Compile(Authored, Compiled, Failure) == Status::Ok);
			REQUIRE(PrepareHostCapture(Authored, Compiled, "source", {}, Capture, Failure) == Status::Ok);
			Capture.Outputs = {
				{"path", std::string{}}, {"dimension", Vector2{double(image.Width), double(image.Height)}}
			};
			image.Hash = SurfaceHash(image);
			Capture.Images = {{"surface_out", std::move(image)}};
			Request.HostCaptures = std::span<const HostNodeCapture>(&Capture, 1);
		}
		Value Run() {
			EvaluatedValue value;
			const auto status = EvaluateValue(Authored, Compiled, "result", Request, value, Failure);
			INFO(Failure.Message);
			REQUIRE(status == Status::Ok);
			return value.Data;
		}
		ArrayValue Array() {
			auto value = Run();
			REQUIRE(std::holds_alternative<ArrayValue>(value));
			return std::get<ArrayValue>(value);
		}
	};
	Image ByteSurface() {
		return {2, 2, {0, 0, 0, 0, 0, 0, 0, 255, 255, 0, 0, 128, 0, 255, 0, 255}, 0};
	}
	Image FloatSurface(SurfaceFormat format, std::span<const SurfacePixel> pixels) {
		const auto layout = CheckedSurfaceLayout(uint32_t(pixels.size()), 1, format, 1000);
		REQUIRE(layout);
		Image image{uint32_t(pixels.size()), 1, std::vector<uint8_t>(layout->Bytes), 0, format};
		for (size_t i = 0; i < pixels.size(); ++i)
			REQUIRE(StoreSurfacePixel(image, uint32_t(i), 0, pixels[i]));
		return image;
	}
}
TEST_CASE(
	"Pixel Extract keeps packed u32 order and distinct black versus empty skips",
	"[imagegraph][source_surface_data]"
) {
	SurfaceDataGraph graph("pc.pixel_extract", ByteSurface());
	const std::vector<ElementValue> all{
		int64_t{0}, int64_t{0xff000000}, int64_t{0x800000ff}, int64_t{0xff00ff00}
	};
	CHECK(graph.Array().ElementType == ValueType::Integer);
	CHECK(graph.Array().Elements == all);
	SurfaceDataGraph black("pc.pixel_extract", ByteSurface(), {{"skip", EnumValue{1}}});
	CHECK(black.Array().Elements == std::vector<ElementValue>{all[2], all[3]});
	SurfaceDataGraph empty("pc.pixel_extract", ByteSurface(), {{"skip", EnumValue{2}}});
	CHECK(empty.Array().Elements == std::vector<ElementValue>{all[1], all[2], all[3]});
}
TEST_CASE(
	"Pixel Extract traverses raw float words rather than quantized logical texels",
	"[imagegraph][source_surface_data]"
) {
	const std::array<SurfacePixel, 2> pixels{{{1, .5, .25, 1}, {0, 0, 0, 0}}};
	SurfaceDataGraph graph("pc.pixel_extract", FloatSurface(SurfaceFormat::RGBA32Float, pixels));
	CHECK(graph.Array().Elements == std::vector<ElementValue>{int64_t{0x3f800000}, int64_t{0x3f000000}});
}
TEST_CASE(
	"Find Pixel uses first row-major match and ignores transparent texels by default",
	"[imagegraph][source_surface_data]"
) {
	SurfaceDataGraph first("pc.find_pixel", ByteSurface(), {}, "surface_in", "position");
	CHECK(std::get<Vector2>(first.Run()) == Vector2{1, 0});
	SurfaceDataGraph transparent(
		"pc.find_pixel",
		ByteSurface(),
		{{"include_alpha", true}, {"search_color", Colour{0, 0, 0, 0}}, {"alpha_tolerance", 0.}},
		"surface_in",
		"position"
	);
	CHECK(std::get<Vector2>(transparent.Run()) == Vector2{0, 0});
	SurfaceDataGraph absent(
		"pc.find_pixel", ByteSurface(), {{"search_color", Colour{0, 0, 255, 255}}}, "surface_in", "position"
	);
	CHECK(std::get<Vector2>(absent.Run()) == Vector2{-1, -1});
}
TEST_CASE(
	"Find Pixel includes tolerance equality and returns bounded all-match positions",
	"[imagegraph][source_surface_data]"
) {
	SurfaceDataGraph graph(
		"pc.find_pixel", ByteSurface(), {{"find_all", true}, {"tolerance", 1. / 3}}, "surface_in", "position"
	);
	CHECK(graph.Array().Elements == std::vector<ElementValue>{Vector2{1, 0}, Vector2{0, 1}, Vector2{1, 1}});
	SurfaceDataGraph alpha(
		"pc.find_pixel",
		ByteSurface(),
		{{"search_color", Colour{255, 0, 0, 255}}, {"include_alpha", true}, {"alpha_tolerance", 127. / 255}},
		"surface_in",
		"position"
	);
	CHECK(std::get<Vector2>(alpha.Run()) == Vector2{0, 1});
}
TEST_CASE(
	"Surface To Points maps RG pairs and limits the output before allocation",
	"[imagegraph][source_surface_data]"
) {
	SurfaceDataGraph graph(
		"pc.surface_to_points",
		ByteSurface(),
		{{"range_min", Vector2{2, 4}},
		 {"range_max", Vector2{6, 10}},
		 {"range_min_unit", EnumValue{0}},
		 {"range_max_unit", EnumValue{0}},
		 {"max_amount", int64_t{3}}},
		"surface",
		"points"
	);
	CHECK(graph.Array().Elements == std::vector<ElementValue>{Vector2{2, 4}, Vector2{2, 4}, Vector2{6, 4}});
	SurfaceDataGraph defaults("pc.surface_to_points", ByteSurface(), {}, "surface", "points");
	CHECK(
		defaults.Array().Elements ==
		std::vector<ElementValue>{Vector2{0, 0}, Vector2{0, 0}, Vector2{32, 0}, Vector2{0, 32}}
	);
}
TEST_CASE(
	"Surface To Points reads consecutive single-channel float pairs and preserves HDR coordinates",
	"[imagegraph][source_surface_data]"
) {
	const std::array<SurfacePixel, 4> pixels{{{.25, 0, 0, 1}, {.5, 0, 0, 1}, {2, 0, 0, 1}, {-1, 0, 0, 1}}};
	for (auto format : {SurfaceFormat::R16Float, SurfaceFormat::R32Float}) {
		SurfaceDataGraph graph(
			"pc.surface_to_points",
			FloatSurface(format, pixels),
			{{"range_min_unit", EnumValue{0}}, {"range_max_unit", EnumValue{0}}},
			"surface",
			"points"
		);
		CHECK(graph.Array().Elements == std::vector<ElementValue>{Vector2{.25, .5}, Vector2{2, -1}});
	}
}
TEST_CASE(
	"Sampler defaults read one texel and source raw edge labels control square averaging",
	"[imagegraph][source_surface_data]"
) {
	SurfaceDataGraph defaults("pc.sampler", ByteSurface(), {}, "surface_in", "color");
	CHECK(std::get<Colour>(defaults.Run()) == Colour{0, 0, 0, 255});
	SurfaceDataGraph clamp(
		"pc.sampler",
		ByteSurface(),
		{{"sampling_size", int64_t{2}}, {"oversample", EnumValue{1}}, {"alpha", true}},
		"surface_in",
		"color"
	);
	CHECK(std::get<Colour>(clamp.Run()) == Colour{57, 28, 0, 113});
	SurfaceDataGraph black(
		"pc.sampler",
		ByteSurface(),
		{{"sampling_size", int64_t{2}}, {"oversample", EnumValue{3}}, {"alpha", true}},
		"surface_in",
		"color"
	);
	CHECK(std::get<Colour>(black.Run()) == Colour{28, 28, 0, 213});
	SurfaceDataGraph repeat(
		"pc.sampler",
		ByteSurface(),
		{{"position", Vector2{2, 0}},
		 {"position_unit", EnumValue{0}},
		 {"oversample", EnumValue{2}},
		 {"alpha", true}},
		"surface_in",
		"color"
	);
	CHECK(std::get<Colour>(repeat.Run()) == Colour{0, 0, 0, 0});
}
TEST_CASE(
	"Surface readback refusal preserves caller value for incomplete words, odd pairs and workload limits",
	"[imagegraph][source_surface_data]"
) {
	Image shortImage{2, 1, {0, 0}, 0, SurfaceFormat::R8Unorm};
	SurfaceDataGraph shortRead("pc.pixel_extract", shortImage);
	EvaluatedValue retained;
	retained.Data = std::string{"retained"};
	CHECK(
		EvaluateValue(
			shortRead.Authored, shortRead.Compiled, "result", shortRead.Request, retained, shortRead.Failure
		) == Status::UnsupportedExecution
	);
	CHECK(std::get<std::string>(retained.Data) == "retained");
	const std::array<SurfacePixel, 1> pixel{{{.5, 0, 0, 1}}};
	SurfaceDataGraph odd(
		"pc.surface_to_points", FloatSurface(SurfaceFormat::R32Float, pixel), {}, "surface", "points"
	);
	CHECK(
		EvaluateValue(odd.Authored, odd.Compiled, "result", odd.Request, retained, odd.Failure) ==
		Status::UnsupportedExecution
	);
	CHECK(odd.Failure.Message.find("odd") != std::string::npos);
	SurfaceDataGraph bound("pc.pixel_extract", Image{65, 65, std::vector<uint8_t>(65 * 65 * 4), 0});
	CHECK(
		EvaluateValue(bound.Authored, bound.Compiled, "result", bound.Request, retained, bound.Failure) ==
		Status::LimitExceeded
	);
}
TEST_CASE(
	"Sampler refuses unresolved negative wrap readbacks and excessive sample counts atomically",
	"[imagegraph][source_surface_data]"
) {
	SurfaceDataGraph graph(
		"pc.sampler",
		ByteSurface(),
		{{"position", Vector2{-1, 0}}, {"position_unit", EnumValue{0}}, {"oversample", EnumValue{2}}},
		"surface_in",
		"color"
	);
	// width-1 is one, so the source safe_mod returns negative zero and reads the first texel.
	CHECK(std::get<Colour>(graph.Run()) == Colour{0, 0, 0, 255});
	SurfaceDataGraph negative(
		"pc.sampler",
		Image{3, 1, std::vector<uint8_t>(12), 0},
		{{"position", Vector2{-1, 0}}, {"position_unit", EnumValue{0}}, {"oversample", EnumValue{2}}},
		"surface_in",
		"color"
	);
	EvaluatedValue retained;
	retained.Data = int64_t{99};
	CHECK(
		EvaluateValue(
			negative.Authored, negative.Compiled, "result", negative.Request, retained, negative.Failure
		) == Status::UnsupportedExecution
	);
	CHECK(negative.Failure.Port == "oversample");
	CHECK(std::get<int64_t>(retained.Data) == 99);
	SurfaceDataGraph excessive(
		"pc.sampler", ByteSurface(), {{"sampling_size", int64_t{2049}}}, "surface_in", "color"
	);
	CHECK(
		EvaluateValue(
			excessive.Authored, excessive.Compiled, "result", excessive.Request, retained, excessive.Failure
		) == Status::LimitExceeded
	);
}
TEST_CASE(
	"Pixel Extract processor modes come from a real array and survive graph persistence",
	"[imagegraph][source_surface_data]"
) {
	SurfaceDataGraph graph("pc.pixel_extract", ByteSurface());
	Node modes{"modes", "pc.array", "", {}, {{"type", EnumValue{0}}}};
	modes.DynamicInputs = {
		{"input_0", ValueType::Scalar, 0.},
		{"input_1", ValueType::Scalar, 1.},
		{"input_2", ValueType::Scalar, 2.}
	};
	graph.Authored.Nodes.insert(graph.Authored.Nodes.begin(), std::move(modes));
	graph.Authored.Links.push_back({"modes", "array", "data", "skip"});
	REQUIRE(Compile(graph.Authored, graph.Compiled, graph.Failure) == Status::Ok);
	const auto result = graph.Array();
	REQUIRE(result.Nested.size() == 3);
	CHECK(result.Nested[0].size() == 4);
	CHECK(result.Nested[1].size() == 2);
	CHECK(result.Nested[2].size() == 3);
	CHECK(std::get<int64_t>(result.Nested[0][1]) == 0xff000000);
	Document restored;
	REQUIRE(Read(Write(graph.Authored), restored, graph.Failure) == Status::Ok);
	Plan restoredPlan;
	REQUIRE(Compile(restored, restoredPlan, graph.Failure) == Status::Ok);
	EvaluatedValue restoredValue;
	REQUIRE(
		EvaluateValue(restored, restoredPlan, "result", graph.Request, restoredValue, graph.Failure) ==
		Status::Ok
	);
	CHECK(std::get<ArrayValue>(restoredValue.Data) == result);
}
TEST_CASE(
	"Sampler nonpositive square sizes perform no source loop iterations", "[imagegraph][source_surface_data]"
) {
	for (int64_t size : {int64_t{0}, int64_t{-4}, std::numeric_limits<int64_t>::min()}) {
		SurfaceDataGraph graph(
			"pc.sampler", ByteSurface(), {{"sampling_size", size}, {"alpha", true}}, "surface_in", "color"
		);
		CHECK(std::get<Colour>(graph.Run()) == Colour{0, 0, 0, 0});
	}
}
TEST_CASE(
	"Surface data batch work is admitted before traversing samples or publishing arrays",
	"[imagegraph][source_surface_data]"
) {
	SurfaceDataGraph sampler("pc.sampler", ByteSurface(), {}, "surface_in", "color");
	Node sizes{"sizes", "pc.array", "", {}, {{"type", EnumValue{0}}}};
	sizes.DynamicInputs = {{"input_0", ValueType::Scalar, 1500.}, {"input_1", ValueType::Scalar, 1500.}};
	sampler.Authored.Nodes.insert(sampler.Authored.Nodes.begin(), std::move(sizes));
	sampler.Authored.Links.push_back({"sizes", "array", "data", "sampling_size"});
	REQUIRE(Compile(sampler.Authored, sampler.Compiled, sampler.Failure) == Status::Ok);
	EvaluatedValue retained;
	retained.Data = std::string{"retained"};
	// Each 2999-square row is below the cap, but both rows need 17,988,002 samples.
	CHECK(
		EvaluateValue(
			sampler.Authored, sampler.Compiled, "result", sampler.Request, retained, sampler.Failure
		) == Status::LimitExceeded
	);
	CHECK(sampler.Failure.Port == "sampling_size");
	CHECK(sampler.Failure.Message.find("batch") != std::string::npos);
	CHECK(std::get<std::string>(retained.Data) == "retained");
	SurfaceDataGraph finder(
		"pc.find_pixel", Image{513, 513, std::vector<uint8_t>(513 * 513 * 4), 0}, {}, "surface_in", "position"
	);
	Node tolerances{"tolerances", "pc.array", "", {}, {{"type", EnumValue{0}}}};
	for (size_t i = 0; i < 64; ++i)
		tolerances.DynamicInputs.push_back({"input_" + std::to_string(i), ValueType::Scalar, 0.});
	finder.Authored.Nodes.insert(finder.Authored.Nodes.begin(), std::move(tolerances));
	finder.Authored.Links.push_back({"tolerances", "array", "data", "tolerance"});
	const auto compileStatus = Compile(finder.Authored, finder.Compiled, finder.Failure);
	INFO(finder.Failure.Message);
	REQUIRE(compileStatus == Status::Ok);
	CHECK(
		EvaluateValue(finder.Authored, finder.Compiled, "result", finder.Request, retained, finder.Failure) ==
		Status::LimitExceeded
	);
	CHECK(finder.Failure.Port == "surface_in");
	CHECK(finder.Failure.Message.find("batch") != std::string::npos);
	CHECK(std::get<std::string>(retained.Data) == "retained");
}
