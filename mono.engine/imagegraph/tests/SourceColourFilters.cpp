#include "../src/nodes/Processor.hpp"
#include "NodeHarness.hpp"

#include <engine/imagegraph/HostCapture.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>

TEST_SUITE_ID("engine.imagegraph.source_colour_filters")
using namespace engine::imagegraph;
namespace {
	ArrayValue Palette(std::initializer_list<Colour> colours) {
		ArrayValue value{ValueType::Colour, {}};
		for (auto colour : colours)
			value.Elements.emplace_back(colour);
		return value;
	}
	struct ColourGraph {
		Document Authored;
		Plan Compiled;
		HostNodeCapture Source;
		EvaluationRequest Request;
		Diagnostic Failure;
		ColourGraph(std::string_view type, Image image, std::vector<AuthoredValue> controls = {}) {
			Authored.FormatVersion = 9;
			Authored.Nodes = {
				{"source", "pc.image", "", {}, {}}, {"filter", std::string(type), "", {}, std::move(controls)}
			};
			Authored.Links = {{"source", "surface_out", "filter", "surface_in"}};
			Authored.Outputs = {{"image", "filter", "surface_out"}};
			auto status = Compile(Authored, Compiled, Failure);
			INFO(Failure.Message);
			REQUIRE(status == Status::Ok);
			REQUIRE(PrepareHostCapture(Authored, Compiled, "source", {}, Source, Failure) == Status::Ok);
			Source.Outputs = {
				{"path", std::string{}}, {"dimension", Vector2{double(image.Width), double(image.Height)}}
			};
			image.Hash = SurfaceHash(image);
			Source.Images = {{"surface_out", std::move(image)}};
			Request.HostCaptures = std::span<const HostNodeCapture>(&Source, 1);
		}
		Image Render() {
			Image image;
			const auto status = Evaluate(Authored, Compiled, "image", Request, image, Failure);
			INFO(Failure.Message);
			REQUIRE(status == Status::Ok);
			return image;
		}
		ImageArray RenderArray() {
			ImageArray images;
			const auto status = EvaluateArray(Authored, Compiled, "image", Request, images, Failure);
			INFO(Failure.Message);
			REQUIRE(status == Status::Ok);
			return images;
		}
	};
}

TEST_CASE(
	"Color Adjust uses the shader's numeric blend branches and ignores Channel",
	"[imagegraph][source_colour_filters]"
) {
	// Grey inputs give independent closed-form goldens for every menu index, including unused shader indices.
	const std::array<int, 25> expected{128, 192, 0,	  32,  160, 64,	 64,  64,  128, 128, 64,  128, 64,
									   128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128};
	for (int64_t mode = 0; mode < 25; ++mode) {
		ColourGraph graph(
			"pc.color_adjust",
			Image{1, 1, {64, 64, 64, 128}},
			{{"blend_mode", EnumValue{mode}},
			 {"blend", Colour{128, 128, 128, 255}},
			 {"blend_amount", 1.},
			 {"alpha", .5},
			 {"channel", int64_t{0}},
			 {"mask_feather", 0.},
			 {"attribute_color_depth", EnumValue{3}}}
		);
		const auto image = graph.Render();
		for (size_t channel = 0; channel < 3; ++channel)
			CHECK(int(image.Pixels[channel]) == expected[size_t(mode)]);
		CHECK(image.Pixels[3] == 64);
	}
}
TEST_CASE(
	"Color Adjust maps controls through alpha premultiplied scratch pixels and colored masks",
	"[imagegraph][source_colour_filters]"
) {
	const Image black{1, 1, {0, 0, 0, 128}}, map{1, 1, {255, 255, 255, 128}}, mask{1, 1, {255, 0, 0, 255}};
	const auto mapped = imagegraph_test::RunNode(
		"pc.color_adjust",
		{{"surface_in", &black}, {"brightness_map", &map}},
		{{"brightness_mapped", true},
		 {"brightness_map_range", Vector2{0, .5}},
		 {"mask_feather", 0.},
		 {"attribute_color_depth", EnumValue{3}}}
	);
	INFO(mapped.Message);
	REQUIRE(mapped.Ok);
	CHECK(mapped.Output().Pixels == std::vector<uint8_t>{64, 64, 64, 128});
	const auto masked = imagegraph_test::RunNode(
		"pc.color_adjust",
		{{"surface_in", &black}, {"mask", &mask}},
		{{"brightness", 1.},
		 {"alpha", 0.},
		 {"mask_feather", 0.},
		 {"mask_alpha_only", true},
		 {"attribute_color_depth", EnumValue{3}}}
	);
	REQUIRE(masked.Ok);
	// mask_modify's early return preserves the colored mask when invert=false and feather=0.
	CHECK(masked.Output().Pixels == std::vector<uint8_t>{255, 0, 0, 0});
	const auto inverted = imagegraph_test::RunNode(
		"pc.color_adjust",
		{{"surface_in", &black}, {"mask", &mask}},
		{{"brightness", 1.},
		 {"invert_mask", true},
		 {"mask_feather", 0.},
		 {"attribute_color_depth", EnumValue{3}}}
	);
	REQUIRE(inverted.Ok);
	CHECK(inverted.Output().Pixels == std::vector<uint8_t>{170, 170, 170, 128});
	for (int64_t depth = 2; depth <= 8; ++depth) {
		const auto typed = imagegraph_test::RunNode(
			"pc.color_adjust",
			{{"surface_in", &black}},
			{{"brightness", .25}, {"attribute_color_depth", EnumValue{depth}}}
		);
		INFO(typed.Message);
		REQUIRE(typed.Ok);
		CHECK(typed.Output().Format == *SourceSurfaceFormat(depth));
	}
}
TEST_CASE(
	"Color Adjust palette controls preserve source alpha and ignored surface options",
	"[imagegraph][source_colour_filters]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"filter",
		 "pc.color_adjust",
		 "",
		 {},
		 {{"input_type", EnumValue{1}},
		  {"color", Palette({Colour{128, 128, 128, 64}})},
		  {"brightness", .25},
		  {"alpha", 0.},
		  {"blend_mode", EnumValue{12}},
		  {"channel", int64_t{0}}}}
	};
	document.Outputs = {{"colours", "filter", "color_out"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluatedValue result;
	auto status = EvaluateValue(document, plan, "colours", {}, result, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	const auto &palette = std::get<ArrayValue>(result.Data);
	REQUIRE(palette.Elements.size() == 1);
	CHECK(std::get<Colour>(palette.Elements[0]) == Colour{192, 192, 192, 64});
	document.Nodes[0].Values.push_back({"brightness_mapped", true});
	document.Nodes[0].Values.push_back({"brightness_map_range", Vector2{0, 0}});
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	REQUIRE(EvaluateValue(document, plan, "colours", {}, result, diagnostic) == Status::Ok);
	CHECK(std::get<Colour>(std::get<ArrayValue>(result.Data).Elements[0]) == Colour{192, 192, 192, 64});
	// Imported endpoint arrays have only their range projection, so the palette takes endpoint zero.
	document.Nodes[0].Values.erase(document.Nodes[0].Values.begin() + 2);
	document.Nodes[0].Values.back().Data = Vector2{.5, .75};
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	REQUIRE(EvaluateValue(document, plan, "colours", {}, result, diagnostic) == Status::Ok);
	CHECK(std::get<Colour>(std::get<ArrayValue>(result.Data).Elements[0]) == Colour{255, 255, 255, 64});

	Document restored;
	REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
	REQUIRE(Compile(restored, plan, diagnostic) == Status::Ok);
	EvaluatedValue second;
	REQUIRE(EvaluateValue(restored, plan, "colours", {}, second, diagnostic) == Status::Ok);
	CHECK(second.Data == result.Data);
}
TEST_CASE(
	"Replace Palette supports Order Closest Color soft distance and processor masks",
	"[imagegraph][source_colour_filters]"
) {
	const auto from = Palette({Colour{255, 255, 255, 255}, Colour{0, 0, 0, 255}});
	const auto to = Palette({Colour{255, 0, 0, 32}, Colour{0, 255, 0, 64}});
	ColourGraph order(
		"pc.color_replace",
		Image{2, 1, {255, 255, 255, 128, 0, 0, 0, 64}},
		{{"from", from}, {"to", to}, {"multiply_alpha", false}, {"attribute_color_depth", EnumValue{3}}}
	);
	CHECK(order.Render().Pixels == std::vector<uint8_t>{255, 0, 0, 128, 0, 255, 0, 64});
	ColourGraph closest(
		"pc.color_replace",
		Image{1, 1, {0, 0, 255, 255}},
		{{"from", Palette({Colour{0, 0, 255, 255}})},
		 {"to", Palette({Colour{255, 0, 0, 255}, Colour{0, 0, 255, 16}})},
		 {"mode", EnumValue{2}},
		 {"attribute_color_depth", EnumValue{3}}}
	);
	CHECK(closest.Render().Pixels == std::vector<uint8_t>{0, 0, 255, 255});
	const Image grey{1, 1, {128, 128, 128, 255}};
	const auto soft = imagegraph_test::RunNode(
		"pc.color_replace",
		{{"surface_in", &grey}},
		{{"from", from},
		 {"to", to},
		 {"threshold", 1.},
		 {"hard_replace", false},
		 {"multiply_alpha", false},
		 {"attribute_color_depth", EnumValue{3}}}
	);
	REQUIRE(soft.Ok);
	CHECK(soft.Output().Pixels[0] == 192);
	CHECK(soft.Output().Pixels[1] == 64);
	CHECK(soft.Output().Pixels[2] == 64);
}
TEST_CASE(
	"Replace Colors chooses RGB distance ignores Threshold and multiplies target alpha",
	"[imagegraph][source_colour_filters]"
) {
	ColourGraph graph(
		"pc.colors_replace",
		Image{2, 1, {255, 0, 0, 128, 0, 0, 255, 255}},
		{{"palette_from", Palette({Colour{255, 0, 0, 255}, Colour{0, 0, 255, 255}})},
		 {"palette_to", Palette({Colour{0, 255, 0, 128}, Colour{255, 255, 0, 64}})},
		 {"threshold", 0.},
		 {"attribute_color_depth", EnumValue{3}}}
	);
	CHECK(graph.Render().Pixels == std::vector<uint8_t>{0, 255, 0, 64, 255, 255, 0, 64});
	graph.Authored.Nodes[1].Values[2].Data = 1.;
	REQUIRE(Compile(graph.Authored, graph.Compiled, graph.Failure) == Status::Ok);
	CHECK(graph.Render().Pixels == std::vector<uint8_t>{0, 255, 0, 64, 255, 255, 0, 64});
}
TEST_CASE(
	"Separate Color extracts unique nontransparent pixels and matches Lab ties by last color",
	"[imagegraph][source_colour_filters]"
) {
	ColourGraph all("pc.color_separate", Image{3, 1, {0, 0, 0, 255, 255, 255, 255, 128, 4, 8, 12, 0}});
	const auto frames = all.RenderArray();
	REQUIRE(frames.Images.size() == 2);
	CHECK(frames.Images[0].Pixels == std::vector<uint8_t>{0, 0, 0, 0, 255, 255, 255, 128, 0, 0, 0, 0});
	CHECK(frames.Images[1].Pixels == std::vector<uint8_t>{0, 0, 0, 255, 0, 0, 0, 0, 0, 0, 0, 0});
	ColourGraph tie(
		"pc.color_separate",
		Image{1, 1, {255, 0, 0, 128}},
		{{"all_colors", false}, {"colors", Palette({Colour{255, 0, 0, 255}, Colour{255, 0, 0, 64}})}}
	);
	const auto tied = tie.RenderArray();
	REQUIRE(tied.Images.size() == 2);
	CHECK(tied.Images[0].Pixels == std::vector<uint8_t>{0, 0, 0, 0});
	CHECK(tied.Images[1].Pixels == std::vector<uint8_t>{255, 0, 0, 128});
	tie.Authored.Nodes[1].Values.push_back({"match_all", false});
	REQUIRE(Compile(tie.Authored, tie.Compiled, tie.Failure) == Status::Ok);
	const auto exact = tie.RenderArray();
	CHECK(exact.Images[0].Pixels == std::vector<uint8_t>{0, 0, 0, 0});
	CHECK(exact.Images[1].Pixels == std::vector<uint8_t>{0, 0, 0, 0});
}
TEST_CASE(
	"Colour filters keep captures exact and fail publication atomically at byte bounds",
	"[imagegraph][source_colour_filters]"
) {
	ColourGraph graph(
		"pc.color_replace",
		Image{1, 1, {255, 255, 255, 255}},
		{{"mode", EnumValue{1}}, {"attribute_color_depth", EnumValue{3}}}
	);
	Image output{1, 1, {12, 34, 56, 78}};
	const Image unchanged = output;
	CHECK(
		Evaluate(graph.Authored, graph.Compiled, "image", graph.Request, output, graph.Failure) ==
		Status::UnsupportedExecution
	);
	CHECK(output == unchanged);
	HostNodeCapture permutation;
	REQUIRE(
		PrepareHostCapture(
			graph.Authored, graph.Compiled, "filter", graph.Request, permutation, graph.Failure
		) == Status::Ok
	);
	permutation.Images = {{"surface_out", Image{1, 1, {255, 0, 0, 255}}}};
	std::array<HostNodeCapture, 2> records{graph.Source, permutation};
	graph.Request.HostCaptures = records;
	CHECK(graph.Render().Pixels == std::vector<uint8_t>{255, 0, 0, 255});
	records[1].InputImages[0].Hash ^= 1;
	output = unchanged;
	CHECK(
		Evaluate(graph.Authored, graph.Compiled, "image", graph.Request, output, graph.Failure) ==
		Status::InvalidValue
	);
	CHECK(output == unchanged);
	ColourGraph separate(
		"pc.color_separate",
		Image{1, 1, {255, 0, 0, 255}},
		{{"all_colors", false}, {"colors", Palette({Colour{255, 0, 0, 255}, Colour{0, 255, 0, 255}})}}
	);
	// 128 owned 512x512 frames exceed the shared 128 MiB ledger before any output frame is drawn.
	separate.Source.Images[0].Data = Image{512, 512, std::vector<uint8_t>(512 * 512 * 4, 255)};
	separate.Source.Images[0].Data.Hash = SurfaceHash(separate.Source.Images[0].Data);
	separate.Source.Outputs[1].Data = Vector2{512, 512};
	ArrayValue many{ValueType::Colour, {}};
	for (uint8_t i = 0; i < 128; ++i)
		many.Elements.emplace_back(Colour{i, 0, 0, 255});
	separate.Authored.Nodes[1].Values[1].Data = std::move(many);
	REQUIRE(Compile(separate.Authored, separate.Compiled, separate.Failure) == Status::Ok);
	ImageArray previous;
	previous.Images.push_back(unchanged);
	const auto retained = previous;
	CHECK(
		EvaluateArray(
			separate.Authored, separate.Compiled, "image", separate.Request, previous, separate.Failure
		) == Status::LimitExceeded
	);
	CHECK(previous.Images == retained.Images);
	CHECK(previous.Items == retained.Items);
	const Image sample{1, 1, {128, 128, 128, 255}};
	const auto unsafe = imagegraph_test::RunNode(
		"pc.color_adjust",
		{{"surface_in", &sample}},
		{{"contrast", std::numeric_limits<double>::max()}, {"attribute_color_depth", EnumValue{3}}}
	);
	CHECK_FALSE(unsafe.Ok);
	CHECK(unsafe.Code == Status::InvalidValue);
}

TEST_CASE(
	"Color Adjust batches linked scalar controls through compiled source array rows",
	"[imagegraph][source_colour_filters]"
) {
	ColourGraph graph(
		"pc.color_adjust", Image{1, 1, {0, 0, 0, 255}}, {{"attribute_color_depth", EnumValue{3}}}
	);
	Node levels{"levels", "pc.array", "", {}, {{"type", EnumValue{0}}}};
	levels.DynamicInputs = {{"input_0", ValueType::Scalar, .25}, {"input_1", ValueType::Scalar, .75}};
	graph.Authored.Nodes.push_back(std::move(levels));
	graph.Authored.Links.push_back({"levels", "array", "filter", "brightness"});
	REQUIRE(Compile(graph.Authored, graph.Compiled, graph.Failure) == Status::Ok);
	const auto rows = graph.RenderArray();
	REQUIRE(rows.Images.size() == 2);
	CHECK(rows.Images[0].Pixels == std::vector<uint8_t>{64, 64, 64, 255});
	CHECK(rows.Images[1].Pixels == std::vector<uint8_t>{191, 191, 191, 255});
}

TEST_CASE(
	"Color Adjust source HSV and exposure controls retain alpha independently",
	"[imagegraph][source_colour_filters]"
) {
	const Image red{1, 1, {255, 0, 0, 64}};
	for (const auto &[control, value, expected] :
		 std::array<std::tuple<std::string_view, double, std::array<uint8_t, 4>>, 5>{
			 std::tuple{"hue", 1. / 3., std::array<uint8_t, 4>{0, 255, 0, 64}},
			 {"hue", -1. / 3., {0, 0, 255, 64}},
			 // The source modifies Value with the old Saturation before setting Saturation to zero.
			 {"saturation", -1., {128, 128, 128, 64}},
			 {"value", -.5, {128, 0, 0, 64}},
			 {"exposure", .5, {128, 0, 0, 64}}
		 }) {
		const auto run = imagegraph_test::RunNode(
			"pc.color_adjust",
			{{"surface_in", &red}},
			{{control, value}, {"attribute_color_depth", EnumValue{3}}}
		);
		INFO(control);
		INFO(run.Message);
		REQUIRE(run.Ok);
		REQUIRE(run.Output().Pixels.size() == expected.size());
		for (size_t channel = 0; channel < expected.size(); ++channel)
			CHECK(int(run.Output().Pixels[channel]) == int(expected[channel]));
	}
}
