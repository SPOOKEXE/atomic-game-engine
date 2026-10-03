#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.source_padding_nodes")
using namespace engine::imagegraph;
using imagegraph_test::RunNode;
namespace {
	Image Solid(uint32_t width, uint32_t height, Colour colour) {
		Image image{width, height, std::vector<uint8_t>(size_t(width) * height * 4), 0};
		for (size_t at = 0; at < image.Pixels.size(); at += 4) {
			image.Pixels[at] = colour.Red;
			image.Pixels[at + 1] = colour.Green;
			image.Pixels[at + 2] = colour.Blue;
			image.Pixels[at + 3] = colour.Alpha;
		}
		return image;
	}
}
TEST_CASE(
	"Padding Empty preserves raw alpha and right top left bottom order",
	"[imagegraph][source_2d][source_padding]"
) {
	const auto input = Solid(2, 2, {64, 128, 192, 128});
	auto run = RunNode(
		"pc.padding",
		{{"surface_in", &input}},
		{{"padding", Vector4{2, 1, 1, 2}}, {"padding_unit", EnumValue{0}}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	CHECK(run.Output().Width == 5);
	CHECK(run.Output().Height == 5);
	const auto at = (size_t{1} * 5 + 1) * 4;
	CHECK(run.Output().Pixels[at] == 64);
	CHECK(run.Output().Pixels[at + 3] == 128);
	CHECK(run.Output().Pixels[0] == 0);
	CHECK(run.Output().Pixels[3] == 0);
}
TEST_CASE(
	"Padding reference values use source half even integer getter", "[imagegraph][source_2d][source_padding]"
) {
	const auto input = Solid(2, 2, {255, 0, 0, 255});
	auto run = RunNode("pc.padding", {{"surface_in", &input}}, {{"padding", Vector4{.25, .75, .25, .75}}});
	REQUIRE(run.Ok);
	CHECK(run.Output().Width == 2);
	CHECK(run.Output().Height == 6);
	CHECK(run.Output().Pixels[2 * 2 * 4] == 255);
}
TEST_CASE(
	"Padding Solid applies normal alpha factors to all four channels",
	"[imagegraph][source_2d][source_padding]"
) {
	const auto input = Solid(2, 2, {255, 0, 0, 128});
	auto run = RunNode(
		"pc.padding",
		{{"surface_in", &input}},
		{{"fill_method", EnumValue{1}}, {"fill_color", Colour{0, 0, 255, 128}}}
	);
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels[0] == 128);
	CHECK(run.Output().Pixels[2] == 127);
	CHECK(run.Output().Pixels[3] == 128);
}
TEST_CASE(
	"Padding Pad to size uses alignment before dimension rounding", "[imagegraph][source_2d][source_padding]"
) {
	const auto input = Solid(2, 2, {255, 0, 0, 255});
	for (int64_t alignment = 0; alignment < 3; ++alignment) {
		auto run = RunNode(
			"pc.padding",
			{{"surface_in", &input}},
			{{"pad_mode", EnumValue{1}},
			 {"dimension", Vector2{4, 4}},
			 {"h_align", EnumValue{alignment}},
			 {"v_align", EnumValue{alignment}}}
		);
		REQUIRE(run.Ok);
		const auto at = (size_t(alignment) * 4 + alignment) * 4;
		CHECK(run.Output().Pixels[at] == 255);
		CHECK(run.Output().Width == 4);
	}
}
TEST_CASE(
	"Padding Pixel Expand opaque pixels avoid the unset resolution uniform",
	"[imagegraph][source_2d][source_padding]"
) {
	const auto input = Solid(2, 2, {64, 128, 192, 255});
	auto run = RunNode("pc.padding", {{"surface_in", &input}}, {{"fill_method", EnumValue{2}}});
	INFO(run.Message);
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels == input.Pixels);
}
TEST_CASE(
	"Padding Pixel Expand transparent padding diagnoses missing ambient uniform",
	"[imagegraph][source_2d][source_padding]"
) {
	const auto input = Solid(2, 2, {64, 128, 192, 255});
	auto run = RunNode(
		"pc.padding",
		{{"surface_in", &input}},
		{{"fill_method", EnumValue{2}}, {"padding", Vector4{1, 1, 1, 1}}, {"padding_unit", EnumValue{0}}}
	);
	CHECK_FALSE(run.Ok);
	CHECK(run.Code == Status::UnsupportedExecution);
}
TEST_CASE(
	"Padding Pad out small dimensions require the source previous cached output",
	"[imagegraph][source_2d][source_padding]"
) {
	const auto input = Solid(2, 2, {255, 0, 0, 255});
	auto run = RunNode(
		"pc.padding",
		{{"surface_in", &input}},
		{{"padding", Vector4{-1, 0, 0, 0}}, {"padding_unit", EnumValue{0}}}
	);
	CHECK_FALSE(run.Ok);
	CHECK(run.Code == Status::UnsupportedExecution);
}
TEST_CASE(
	"Padding actual graph survives authored persistence and selected evaluation",
	"[imagegraph][source_2d][source_padding]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"source",
		 "pc.solid",
		 "",
		 {},
		 {{"dimension", Vector2{2, 2}}, {"dimension_unit", EnumValue{0}}, {"color", Colour{255, 0, 0, 255}}}},
		{"pad", "pc.padding", "", {}, {{"padding", Vector4{1, 1, 1, 1}}, {"padding_unit", EnumValue{0}}}}
	};
	document.Links = {{"source", "surface_out", "pad", "surface_in"}};
	document.Outputs = {{"out", "pad", "surface_out"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	Document restored;
	REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
	REQUIRE(Compile(restored, plan, diagnostic) == Status::Ok);
	Image output;
	const auto status = Evaluate(restored, plan, "out", {}, output, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	CHECK(output.Width == 4);
	CHECK(output.Height == 4);
	const auto previous = output;
	CHECK(Evaluate(restored, plan, "out", {}, output, diagnostic, 1) == Status::LimitExceeded);
	CHECK(output == previous);
}
TEST_CASE(
	"Padding full processor batch admits bounded work before output allocation",
	"[imagegraph][source_2d][source_padding]"
) {
	const auto input = Solid(2, 2, {255, 0, 0, 255});
	const auto *entry = FindCatalogueEntry("pc.padding");
	REQUIRE(entry);
	Node node{"pad", "pc.padding", "", {}, {}};
	EvaluationRequest request;
	engine::imagegraph::detail::NodeContext context(node, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	context.ProcessorCount = 4096;
	context.Images.emplace_back("surface_in", &input);
	context.Values = {
		{"pad_mode", EnumValue{1}}, {"dimension", Vector2{128, 128}}, {"fill_method", EnumValue{0}}
	};
	const auto execute = engine::imagegraph::detail::FindExecutor("pc.padding");
	REQUIRE(execute);
	CHECK_FALSE(execute(context));
	CHECK(context.FailureCode == Status::LimitExceeded);
	CHECK(context.OutputImages.empty());
}
TEST_CASE(
	"Padding linked surface Dimension bypasses Reference unit conversion",
	"[imagegraph][source_2d][source_padding]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"source",
		 "pc.solid",
		 "",
		 {},
		 {{"dimension", Vector2{2, 2}}, {"dimension_unit", EnumValue{0}}, {"color", Colour{255, 0, 0, 255}}}},
		{"size", "pc.solid", "", {}, {{"dimension", Vector2{3, 4}}, {"dimension_unit", EnumValue{0}}}},
		{"pad", "pc.padding", "", {}, {{"pad_mode", EnumValue{1}}, {"dimension_unit", EnumValue{1}}}}
	};
	document.Links = {
		{"source", "surface_out", "pad", "surface_in"}, {"size", "surface_out", "pad", "dimension"}
	};
	document.Outputs = {{"out", "pad", "surface_out"}};
	Diagnostic diagnostic;
	Plan plan;
	const auto compiled = Compile(document, plan, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(compiled == Status::Ok);
	Image output;
	const auto status = Evaluate(document, plan, "out", {}, output, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	CHECK(output.Width == 3);
	CHECK(output.Height == 4);
}
