#include "../src/PixelBuilderPayload.hpp"
#include "NodeHarness.hpp"

#include <engine/imagegraph/GroupReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
TEST_SUITE_ID("engine.imagegraph.source_path_blur")
using namespace engine::imagegraph;
using imagegraph_test::RunNode;
namespace {
	Document PathDocument() {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {
			{"source",
			 "pc.solid",
			 "",
			 {},
			 {{"dimension", Vector2{2, 1}},
			  {"dimension_unit", EnumValue{0}},
			  {"color", Colour{64, 128, 192, 128}}}},
			{"base", "pc.blur_path", "", {}, {{"resolution", int64_t{2}}}},
			{"blur", "pc.blur_path", "", {}, {}}
		};
		document.Nodes.back().InstanceBase = "base";
		document.Links = {{"source", "surface_out", "base", "surface_in"}};
		document.Outputs = {{"out", "blur", "surface_out"}};
		return document;
	}
	Image CheckedPathGraph(const Document &document, EvaluationRequest request = {}) {
		Plan plan;
		Diagnostic diagnostic;
		const auto compiled = Compile(document, plan, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(compiled == Status::Ok);
		Image image;
		const auto status = Evaluate(document, plan, "out", request, image, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
		return image;
	}
	void CheckOwnedEmpty(const Image &image) {
		CHECK(image.Pixels == std::vector<uint8_t>{128, 255, 255, 128, 128, 255, 255, 128});
	}

	Image SolidImage(uint32_t width, uint32_t height, std::array<uint8_t, 4> colour) {
		Image image;
		image.Width = width;
		image.Height = height;
		image.Pixels.resize(size_t(width) * height * colour.size());
		for (size_t offset = 0; offset < image.Pixels.size(); offset += colour.size())
			for (size_t channel = 0; channel < colour.size(); ++channel)
				image.Pixels[offset + channel] = colour[channel];
		return image;
	}

	Path2D Line() {
		Path2D path;
		path.Anchors = {{{0, 0, 0, 0, 0, 0}, 0}, {{2, 0, 0, 0, 0, 0}, 0}};
		return path;
	}
	Curve Constant(double value) {
		Curve curve;
		curve.Header = {0, 1, 0, 0, 0, 1};
		curve.Anchors = {{0, 0, 0, value, 0, 0}, {0, 0, 1, value, 0, 0}};
		return curve;
	}
	Image Uniform() {
		return SolidImage(2, 1, {64, 128, 192, 128});
	}
}
TEST_CASE(
	"Path Blur source default noone returns its cleared result before shader controls",
	"[imagegraph][source_2d]"
) {
	const Image image = Uniform();
	const auto run = RunNode(
		"pc.blur_path", {{"surface_in", &image}}, {{"scale_modulate", Constant(0)}, {"intensity", 0.}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	CHECK(run.Output().Width == 2);
	CHECK(run.Output().Pixels == std::vector<uint8_t>(8, 0));
	const auto inactive = RunNode("pc.blur_path", {{"surface_in", &image}}, {{"active", false}});
	REQUIRE(inactive.Ok);
	CHECK(inactive.Output().Pixels == image.Pixels);
}
TEST_CASE(
	"Path Blur RGB divides source alpha and intensity only changes average alpha", "[imagegraph][source_2d]"
) {
	const Image image = Uniform();
	for (double intensity : {1., .5}) {
		const auto run = RunNode(
			"pc.blur_path",
			{{"surface_in", &image}},
			{{"blur_path", Line()},
			 {"range", Vector2{0, 0}},
			 {"resolution", int64_t{2}},
			 {"intensity", intensity}}
		);
		INFO(run.Message);
		REQUIRE(run.Ok);
		CHECK(run.Output().Pixels[0] == 128);
		CHECK(run.Output().Pixels[1] == 255);
		CHECK(run.Output().Pixels[2] == 255);
		CHECK(run.Output().Pixels[3] == (intensity == 1 ? 128 : 64));
	}
}
TEST_CASE("Path Blur always consumes its submitted intensity curve in blur mode", "[imagegraph][source_2d]") {
	const Image image = Uniform();
	const auto run = RunNode(
		"pc.blur_path",
		{{"surface_in", &image}},
		{{"blur_path", Line()},
		 {"range", Vector2{0, 0}},
		 {"resolution", int64_t{2}},
		 {"intensity_curved", false},
		 {"intensity_curve", Constant(.5)}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels[0] == 128);
	CHECK(run.Output().Pixels[3] == 64);
}
TEST_CASE(
	"Path Blend preserves source ordering gradient tint and accumulated alpha", "[imagegraph][source_2d]"
) {
	const Image image = SolidImage(2, 1, {255, 255, 255, 128});
	const Gradient color{0, {{0, {255, 0, 0, 255}}, {1, {0, 255, 0, 255}}}};
	for (bool inverted : {false, true}) {
		const auto run = RunNode(
			"pc.blur_path",
			{{"surface_in", &image}},
			{{"blur_path", Line()},
			 {"range", Vector2{0, 0}},
			 {"resolution", int64_t{2}},
			 {"mode", EnumValue{1}},
			 {"color", color},
			 {"inverted", inverted}}
		);
		INFO(run.Message);
		REQUIRE(run.Ok);
		CHECK(run.Output().Pixels[0] == (inverted ? 213 : 170));
		CHECK(run.Output().Pixels[1] == (inverted ? 42 : 85));
		CHECK(run.Output().Pixels[2] == 0);
		CHECK(run.Output().Pixels[3] == 192);
	}
}
TEST_CASE("Path Blend ignores unused intensity and its undefined curve", "[imagegraph][source_2d]") {
	const Image image = Uniform();
	Curve curve = Constant(1);
	curve.Header[1] = 0;
	const Gradient white{0, {{0, {255, 255, 255, 255}}}};
	const auto run = RunNode(
		"pc.blur_path",
		{{"surface_in", &image}},
		{{"blur_path", Line()},
		 {"range", Vector2{0, 0}},
		 {"resolution", int64_t{2}},
		 {"mode", EnumValue{1}},
		 {"color", white},
		 {"intensity", 0.},
		 {"intensity_curve", curve}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels[0] == 64);
	CHECK(run.Output().Pixels[1] == 128);
	CHECK(run.Output().Pixels[2] == 192);
	CHECK(run.Output().Pixels[3] == 192);
}
TEST_CASE(
	"Path Blur clamps resolution and range while retaining source point offsets", "[imagegraph][source_2d]"
) {
	const Image image = imagegraph_test::MakeImage(2, 1, {255, 0, 0, 255, 0, 255, 0, 255});
	const auto run = RunNode(
		"pc.blur_path",
		{{"surface_in", &image}},
		{{"blur_path", Line()},
		 {"resolution", int64_t{0}},
		 {"range", Vector2{0, 1}},
		 {"oversample", EnumValue{4}}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	// Source ratios0 and.99 give offsets0 and1.98 pixels; repeat sampling preserves each pixel.
	CHECK(run.Output().Pixels == image.Pixels);
	const auto high = RunNode(
		"pc.blur_path",
		{{"surface_in", &image}},
		{{"blur_path", Line()}, {"resolution", int64_t{10000}}, {"range", Vector2{0, 0}}}
	);
	REQUIRE(high.Ok);
	CHECK(high.Output().Pixels == image.Pixels);
}
TEST_CASE(
	"Path Blur origin anchor scale rotation and UV remap preserve source transforms",
	"[imagegraph][source_2d]"
) {
	const Image image = imagegraph_test::MakeImage(2, 1, {255, 0, 0, 255, 0, 255, 0, 255});
	const auto shifted = RunNode(
		"pc.blur_path",
		{{"surface_in", &image}},
		{{"blur_path", Line()},
		 {"resolution", int64_t{2}},
		 {"range", Vector2{0, 0}},
		 {"path_origin", .5},
		 {"oversample", EnumValue{4}}}
	);
	INFO(shifted.Message);
	REQUIRE(shifted.Ok);
	CHECK(shifted.Output().Pixels[0] == 0);
	CHECK(shifted.Output().Pixels[1] == 255);
	const auto rotated = RunNode(
		"pc.blur_path",
		{{"surface_in", &image}},
		{{"blur_path", Line()},
		 {"resolution", int64_t{2}},
		 {"range", Vector2{0, 0}},
		 {"rotation_modulate", Vector2{180, 180}}}
	);
	REQUIRE(rotated.Ok);
	CHECK(rotated.Output().Pixels[0] == 0);
	CHECK(rotated.Output().Pixels[1] == 255);
	const auto scaled = RunNode(
		"pc.blur_path",
		{{"surface_in", &image}},
		{{"blur_path", Line()},
		 {"resolution", int64_t{2}},
		 {"range", Vector2{0, 0}},
		 {"anchor", Vector2{0, 0}},
		 {"scale_modulate", Constant(.25)},
		 {"oversample", EnumValue{3}}}
	);
	REQUIRE(scaled.Ok);
	CHECK(scaled.Output().Pixels[0] == 0);
	CHECK(scaled.Output().Pixels[1] == 255);
	const Image uv = SolidImage(1, 1, {255, 255, 0, 0});
	const auto mapped = RunNode(
		"pc.blur_path",
		{{"surface_in", &image}, {"uv_map", &uv}},
		{{"blur_path", Line()},
		 {"resolution", int64_t{2}},
		 {"range", Vector2{0, 0}},
		 {"oversample", EnumValue{3}}}
	);
	REQUIRE(mapped.Ok);
	CHECK(mapped.Output().Pixels[0] == 128);
	CHECK(mapped.Output().Pixels[1] == 128);
	CHECK(mapped.Output().Pixels[3] == 255);
}
TEST_CASE("Path Blur selected source singularities produce port diagnostics", "[imagegraph][source_2d]") {
	const Image image = Uniform();
	const auto scale = RunNode(
		"pc.blur_path", {{"surface_in", &image}}, {{"blur_path", Line()}, {"scale_modulate", Constant(0)}}
	);
	CHECK(scale.Code == Status::UnsupportedExecution);
	CHECK(scale.Port == "scale_modulate");
	const auto zero =
		RunNode("pc.blur_path", {{"surface_in", &image}}, {{"blur_path", Line()}, {"intensity", 0.}});
	CHECK(zero.Code == Status::UnsupportedExecution);
	CHECK(zero.Port == "intensity");
	const Gradient transparent{0, {{0, {255, 0, 0, 0}}}};
	const auto blend = RunNode(
		"pc.blur_path",
		{{"surface_in", &image}},
		{{"blur_path", Line()}, {"mode", EnumValue{1}}, {"color", transparent}}
	);
	CHECK(blend.Code == Status::UnsupportedExecution);
	CHECK(blend.Port == "color");
	const Image empty = SolidImage(1, 1, {0, 0, 0, 0});
	const Gradient white{0, {{0, {255, 255, 255, 255}}}};
	const auto skipped = RunNode(
		"pc.blur_path",
		{{"surface_in", &empty}},
		{{"blur_path", Line()}, {"mode", EnumValue{1}}, {"color", white}}
	);
	REQUIRE(skipped.Ok);
	CHECK(skipped.Output().Pixels == empty.Pixels);
}
TEST_CASE("Path Blur shader curves and gradients retain uniform slot bounds", "[imagegraph][source_2d]") {
	const Image image = Uniform();
	Curve curve = Constant(1);
	curve.Anchors.resize(10, curve.Anchors.back());
	const auto scale =
		RunNode("pc.blur_path", {{"surface_in", &image}}, {{"blur_path", Line()}, {"scale_modulate", curve}});
	CHECK(scale.Code == Status::UnsupportedExecution);
	const auto intensity = RunNode(
		"pc.blur_path", {{"surface_in", &image}}, {{"blur_path", Line()}, {"intensity_curve", curve}}
	);
	CHECK(intensity.Code == Status::UnsupportedExecution);
	Gradient gradient;
	gradient.Keys.resize(65, {0, {255, 255, 255, 255}});
	const auto keys = RunNode(
		"pc.blur_path",
		{{"surface_in", &image}},
		{{"blur_path", Line()}, {"mode", EnumValue{1}}, {"color", gradient}}
	);
	CHECK(keys.Code == Status::UnsupportedExecution);
	CHECK(keys.Port == "color");
}
TEST_CASE(
	"Path Blur common mask mix channel and safe draw semantics remain distinct", "[imagegraph][source_2d]"
) {
	const Image image = Uniform(), black = SolidImage(1, 1, {0, 0, 0, 0});
	const auto masked = RunNode(
		"pc.blur_path",
		{{"surface_in", &image}, {"mask", &black}},
		{{"blur_path", Line()}, {"range", Vector2{0, 0}}}
	);
	REQUIRE(masked.Ok);
	CHECK(masked.Output().Pixels == image.Pixels);
	const auto mixed = RunNode(
		"pc.blur_path",
		{{"surface_in", &image}},
		{{"blur_path", Line()}, {"range", Vector2{0, 0}}, {"mix", .5}, {"channel", int64_t{1}}}
	);
	REQUIRE(mixed.Ok);
	CHECK(mixed.Output().Pixels[0] == 96);
	CHECK(mixed.Output().Pixels[1] == 128);
	Image single;
	single.Width = single.Height = 1;
	single.Format = SurfaceFormat::R8Unorm;
	single.Pixels = {64};
	const auto safe = RunNode(
		"pc.blur_path",
		{{"surface_in", &single}},
		{{"blur_path", Line()}, {"scale_modulate", Constant(0)}, {"attribute_color_depth", EnumValue{3}}}
	);
	REQUIRE(safe.Ok);
	CHECK(safe.Output().Pixels == std::vector<uint8_t>{64, 64, 64, 255});
}
TEST_CASE(
	"Path Blur linked owned empty object executes instead of default noone", "[imagegraph][source_2d]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"source",
		 "pc.solid",
		 "",
		 {},
		 {{"dimension", Vector2{2, 1}},
		  {"dimension_unit", EnumValue{0}},
		  {"color", Colour{64, 128, 192, 128}}}},
		{"path", "pc.path", "", {}, {}},
		{"blur", "pc.blur_path", "", {}, {{"resolution", int64_t{2}}}}
	};
	document.Links = {
		{"source", "surface_out", "blur", "surface_in"}, {"path", "path_data", "blur", "blur_path"}
	};
	document.Outputs = {{"out", "blur", "surface_out"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	Image result;
	const auto status = Evaluate(document, plan, "out", {}, result, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	CHECK(result.Pixels == std::vector<uint8_t>{128, 255, 255, 128, 128, 255, 255, 128});
	const Image prior = result;
	CHECK(Evaluate(document, plan, "out", {}, result, diagnostic, 1) == Status::LimitExceeded);
	CHECK(result == prior);
	document.Links.pop_back();
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	REQUIRE(Evaluate(document, plan, "out", {}, result, diagnostic) == Status::Ok);
	CHECK(result.Pixels == std::vector<uint8_t>(8, 0));
	document.Nodes.back().Values.push_back({"blur_path", Path2D{}});
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	REQUIRE(Evaluate(document, plan, "out", {}, result, diagnostic) == Status::Ok);
	CHECK(result.Pixels == prior.Pixels);
}
TEST_CASE(
	"Path Blur admits total processor-row work before allocating its result", "[imagegraph][source_2d]"
) {
	const auto *entry = FindCatalogueEntry("pc.blur_path");
	const auto executor = engine::imagegraph::detail::FindExecutor("pc.blur_path");
	REQUIRE(entry);
	REQUIRE(executor);
	Node node{"blur", "pc.blur_path", "", {}, {{"blur_path", Line()}}};
	EvaluationRequest request;
	engine::imagegraph::detail::NodeContext context(node, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	context.ProcessorCount = 65536;
	context.InputProvenanceResolved = true;
	const Image image = Uniform();
	context.Images.emplace_back("surface_in", &image);
	for (const auto &input : entry->Inputs)
		if (const auto value = CatalogueDefault(input)) context.Values.emplace_back(input.Id, *value);
	Path2D path = Line();
	Value pathValue = path, resolution = int64_t{128};
	context.ValueViews.emplace_back("blur_path", &pathValue);
	context.ValueViews.emplace_back("resolution", &resolution);
	CHECK_FALSE(executor(context));
	CHECK(context.FailureCode == Status::LimitExceeded);
	CHECK(context.FailurePort == "resolution");
	CHECK(context.OutputImages.empty());
}

TEST_CASE(
	"Path Blur inherited catalogue noone remains cleared through instance chains", "[imagegraph][source_2d]"
) {
	auto document = PathDocument();
	CHECK(CheckedPathGraph(document).Pixels == std::vector<uint8_t>(8, 0));
	Node leaf{"leaf", "pc.blur_path", "", {}, {}};
	leaf.InstanceBase = "blur";
	document.Nodes.push_back(leaf);
	document.Outputs.front().NodeId = "leaf";
	CHECK(CheckedPathGraph(document).Pixels == std::vector<uint8_t>(8, 0));
}
TEST_CASE("Path Blur inherited owned empty path executes as an object", "[imagegraph][source_2d]") {
	auto document = PathDocument();
	document.Nodes[1].Values.push_back({"blur_path", Path2D{}});
	CheckOwnedEmpty(CheckedPathGraph(document));
}
TEST_CASE("Path Blur local instance empty override remains an owned object", "[imagegraph][source_2d]") {
	auto document = PathDocument();
	document.Nodes[1].Values.push_back({"blur_path", Line()});
	document.Nodes[2].InstanceOverrides.push_back("blur_path");
	document.Nodes[2].Values.push_back({"blur_path", Path2D{}});
	CheckOwnedEmpty(CheckedPathGraph(document));
}
TEST_CASE("Path Blur sampled empty keyframe remains owned through an instance", "[imagegraph][source_2d]") {
	auto document = PathDocument();
	document.Keyframes = {{"base", "blur_path", 0, Line()}, {"base", "blur_path", 1, Path2D{}}};
	EvaluationRequest request;
	request.Tick = 1;
	CheckOwnedEmpty(CheckedPathGraph(document, request));
}
TEST_CASE(
	"Path Blur authored document edit distinguishes empty value from missing default",
	"[imagegraph][source_2d]"
) {
	auto document = PathDocument();
	const Image initial = CheckedPathGraph(document);
	CHECK(initial.Pixels == std::vector<uint8_t>(8, 0));
	document.Nodes[1].Values.push_back({"blur_path", Path2D{}});
	CheckOwnedEmpty(CheckedPathGraph(document));
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	Image retained = initial;
	CHECK(Evaluate(document, plan, "out", {}, retained, diagnostic, 1) == Status::LimitExceeded);
	CHECK(retained == initial);
}
TEST_CASE("Path Blur effective instance producer keeps empty path node identity", "[imagegraph][source_2d]") {
	auto document = PathDocument();
	document.Nodes.push_back({"empty", "pc.path", "", {}, {}});
	document.Links.push_back({"empty", "path_data", "base", "blur_path"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	const auto *entry = FindCatalogueEntry("pc.blur_path");
	REQUIRE(entry);
	EvaluationRequest request;
	engine::imagegraph::detail::NodeContext context(document.Nodes[2], *entry, request);
	context.EffectiveInputLinks = plan.EffectiveLinks;
	context.InputOwnerIds.emplace_back("blur_path", "base");
	const auto *producer = context.InputProducer("blur_path");
	REQUIRE(producer);
	CHECK(producer->FromNode == "empty");
	CHECK(producer->FromPort == "path_data");
	CHECK(context.InputProducer("scale_modulate") == nullptr);
	CheckOwnedEmpty(CheckedPathGraph(document));
}
TEST_CASE(
	"Path Blur bound group replay preserves default and owned empty identities", "[imagegraph][source_2d]"
) {
	for (bool owned : {false, true}) {
		auto document = PathDocument();
		if (owned) document.Nodes[1].Values.push_back({"blur_path", Path2D{}});
		GroupReplayState empty, local, bound;
		Diagnostic diagnostic;
		REQUIRE(RebindGroupReplay(document, empty, 1, local, diagnostic) == Status::Ok);
		const GroupSubtypeBinding binding{
			"blur", "base", GroupSubtypeAnimator::Static, GroupSubtypeAnimator::Static, "blur_path"
		};
		REQUIRE(BindGroupReplay(document, {&binding, 1}, local, 1, bound, diagnostic) == Status::Ok);
		EvaluationRequest request;
		request.GroupReplay = &bound;
		request.GroupAuthoringRevision = 1;
		const Image image = CheckedPathGraph(document, request);
		if (owned)
			CheckOwnedEmpty(image);
		else
			CHECK(image.Pixels == std::vector<uint8_t>(8, 0));
	}
}
TEST_CASE(
	"Path Blur Pixel Builder recipe rerender preserves bound empty ownership", "[imagegraph][source_2d]"
) {
	for (bool owned : {false, true}) {
		auto document = PathDocument();
		document.Nodes.push_back(
			{"builder",
			 "pc.pixel_builder",
			 "",
			 {},
			 {{"dimension", Vector2{2, 1}}, {"dimension_unit", EnumValue{0}}}}
		);
		document.Nodes.push_back({"layer", "pc.pb_output", "builder", {}, {}});
		document.Nodes[0].GroupId = document.Nodes[1].GroupId = document.Nodes[2].GroupId = "builder";
		if (owned) document.Nodes[1].Values.push_back({"blur_path", Path2D{}});
		Group group{"builder", "builder"};
		group.OwnerNodeId = "builder";
		document.Groups = {group};
		document.Links.push_back({"blur", "surface_out", "layer", "surface"});
		document.Outputs = {{"recipe", "builder", "dynamic_builder"}};
		Diagnostic diagnostic;
		GroupReplayState empty, local, bound;
		REQUIRE(RebindGroupReplay(document, empty, 1, local, diagnostic) == Status::Ok);
		const GroupSubtypeBinding binding{
			"blur", "base", GroupSubtypeAnimator::Static, GroupSubtypeAnimator::Static, "blur_path"
		};
		REQUIRE(BindGroupReplay(document, {&binding, 1}, local, 1, bound, diagnostic) == Status::Ok);
		Plan plan;
		REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
		EvaluationRequest request;
		request.GroupReplay = &bound;
		request.GroupAuthoringRevision = 1;
		EvaluatedValue value;
		const auto status = EvaluateValue(document, plan, "recipe", request, value, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
		const auto *recipe = std::get_if<DynamicSurfaceValue>(&value.Data);
		REQUIRE(recipe);
		Image image;
		const auto rendered =
			engine::imagegraph::detail::RasterizePixelBuilder(*recipe, {2, 1}, image, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(rendered == Status::Ok);
		REQUIRE(image.Pixels.size() == 8);
		if (owned)
			CHECK(image.Pixels[0] > 0);
		else
			CHECK(image.Pixels == std::vector<uint8_t>(8, 0));
	}
}
TEST_CASE(
	"Path Blur local linked image wins over inherited route without override flag", "[imagegraph][source_2d]"
) {
	auto document = PathDocument();
	document.Nodes[1].Values.push_back({"blur_path", Path2D{}});
	document.Nodes.push_back(
		{"local",
		 "pc.solid",
		 "",
		 {},
		 {{"dimension", Vector2{2, 1}}, {"dimension_unit", EnumValue{0}}, {"color", Colour{32, 64, 96, 255}}}}
	);
	document.Links.push_back({"local", "surface_out", "blur", "surface_in"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	const auto route =
		std::find_if(plan.EffectiveLinks.begin(), plan.EffectiveLinks.end(), [](const Link &link) {
			return link.ToNode == "blur" && link.ToPort == "surface_in";
		});
	REQUIRE(route != plan.EffectiveLinks.end());
	CHECK(route->FromNode == "local");
	CHECK(CheckedPathGraph(document).Pixels == std::vector<uint8_t>{32, 64, 96, 255, 32, 64, 96, 255});
}
TEST_CASE(
	"Path Blur intermediate instance link wins for its leaf producer route", "[imagegraph][source_2d]"
) {
	auto document = PathDocument();
	document.Nodes[1].Values.push_back({"blur_path", Path2D{}});
	document.Nodes.push_back(
		{"local",
		 "pc.solid",
		 "",
		 {},
		 {{"dimension", Vector2{2, 1}}, {"dimension_unit", EnumValue{0}}, {"color", Colour{32, 64, 96, 255}}}}
	);
	document.Links.push_back({"local", "surface_out", "blur", "surface_in"});
	Node leaf{"leaf", "pc.blur_path", "", {}, {}};
	leaf.InstanceBase = "blur";
	document.Nodes.push_back(leaf);
	document.Outputs[0].NodeId = "leaf";
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	const auto route =
		std::find_if(plan.EffectiveLinks.begin(), plan.EffectiveLinks.end(), [](const Link &link) {
			return link.ToNode == "leaf" && link.ToPort == "surface_in";
		});
	REQUIRE(route != plan.EffectiveLinks.end());
	CHECK(route->FromNode == "local");
	CHECK(CheckedPathGraph(document).Pixels == std::vector<uint8_t>{32, 64, 96, 255, 32, 64, 96, 255});
}
TEST_CASE("Path Blur instance override can suppress its inherited image link", "[imagegraph][source_2d]") {
	auto document = PathDocument();
	document.Nodes[2].InstanceOverrides.push_back("surface_in");
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	CHECK(std::none_of(plan.EffectiveLinks.begin(), plan.EffectiveLinks.end(), [](const Link &link) {
		return link.ToNode == "blur" && link.ToPort == "surface_in";
	}));
	Image retained = Uniform(), previous = retained;
	CHECK(Evaluate(document, plan, "out", {}, retained, diagnostic) == Status::InvalidValue);
	CHECK(diagnostic.Port == "surface_in");
	CHECK(retained == previous);
}
TEST_CASE(
	"Path Blur inherited junction default remains owned rather than catalogue noone",
	"[imagegraph][source_2d]"
) {
	auto document = PathDocument();
	document.Junctions.push_back({"empty", "", ValueType::Path2D, Path2D{}});
	document.Links.push_back({"empty", "value", "base", "blur_path"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	CHECK(std::any_of(plan.ResolvedInputs.begin(), plan.ResolvedInputs.end(), [](const ResolvedInput &input) {
		return input.NodeId == "blur" && input.Port == "blur_path";
	}));
	CheckOwnedEmpty(CheckedPathGraph(document));
}
TEST_CASE(
	"Path Blur derived instance link bound preserves the prior compile plan", "[imagegraph][source_2d]"
) {
	auto document = PathDocument();
	document.Links.push_back({"source", "surface_out", "base", "mask"});
	document.Links.push_back({"source", "surface_out", "base", "uv_map"});
	document.Junctions.push_back({"empty", "", ValueType::Path2D, Path2D{}});
	document.Links.push_back({"empty", "value", "base", "blur_path"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	const auto prior = plan.EffectiveLinks;
	const auto priorOrder = plan.NodeOrder;
	for (size_t i = 0; i < 2050; ++i) {
		Node node{"instance_" + std::to_string(i), "pc.blur_path", "", {}, {}};
		node.InstanceBase = "base";
		document.Nodes.push_back(std::move(node));
	}
	CHECK(Compile(document, plan, diagnostic) == Status::LimitExceeded);
	CHECK(diagnostic.Message == "instance input routes exceed native link bound");
	CHECK(plan.EffectiveLinks == prior);
	CHECK(plan.NodeOrder == priorOrder);
}
