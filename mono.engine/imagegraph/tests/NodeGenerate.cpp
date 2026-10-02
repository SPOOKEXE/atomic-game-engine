#include "../src/ProcessorBatch.hpp"
#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>

TEST_SUITE_ID("engine.imagegraph.node_generate")

using namespace engine::imagegraph;
using imagegraph_test::RunNode;

TEST_CASE("Solid defaults inherit project size and preserve straight RGBA", "[imagegraph][node_generate]") {
	const auto run = RunNode("pc.solid", {}, {{"color", Colour{10, 20, 30, 40}}});
	REQUIRE(run.Ok);
	CHECK(run.Output().Width == 32);
	CHECK(run.Output().Height == 32);
	std::vector<uint8_t> expected;
	for (size_t pixel = 0; pixel < 32 * 32; pixel++)
		expected.insert(expected.end(), {10, 20, 30, 40});
	CHECK(run.Output().Pixels == expected);
	CHECK(
		RunNode("pc.solid", {}, {{"attribute_process", false}, {"color", Colour{10, 20, 30, 40}}})
			.Output()
			.Pixels == expected
	);
}

TEST_CASE(
	"Solid shader foreground normalizes UV forces alpha and mask edits alpha only",
	"[imagegraph][node_generate]"
) {
	const Image foreground{2, 1, {200, 100, 0, 128, 1, 2, 3, 0}, 0};
	const Image mask{1, 1, {255, 0, 0, 128}, 0};
	const auto run = RunNode(
		"pc.solid",
		{{"foreground", &foreground}, {"mask", &mask}},
		{{"use_mask_dimension", false},
		 {"dimension", Vector2{4, 1}},
		 {"dimension_unit", EnumValue{0}},
		 {"color", Colour{0, 100, 200, 20}}}
	);
	REQUIRE(run.Ok);
	CHECK(
		run.Output().Pixels ==
		std::vector<uint8_t>{100, 100, 100, 43, 100, 100, 100, 43, 0, 100, 200, 43, 0, 100, 200, 43}
	);
	const auto alpha = RunNode(
		"pc.solid", {{"mask", &mask}}, {{"mask_alpha_only", true}, {"color", Colour{10, 20, 30, 128}}}
	);
	REQUIRE(alpha.Ok);
	CHECK(alpha.Output().Pixels == std::vector<uint8_t>{10, 20, 30, 64});
	const Image clear{1, 1, {0, 0, 0, 0}, 0};
	const auto transparent = RunNode("pc.solid", {{"mask", &clear}}, {{"color", Colour{10, 20, 30, 255}}});
	REQUIRE(transparent.Ok);
	CHECK(transparent.Output().Pixels == std::vector<uint8_t>{10, 20, 30, 0});
}

TEST_CASE(
	"Empty Solid copies unscaled foreground top left and ignores mask color", "[imagegraph][node_generate]"
) {
	const Image foreground{2, 1, {1, 2, 3, 4, 5, 6, 7, 8}, 0};
	const Image mask{3, 2, std::vector<uint8_t>(24, 0), 0};
	const auto run = RunNode("pc.solid", {{"foreground", &foreground}, {"mask", &mask}}, {{"empty", true}});
	REQUIRE(run.Ok);
	CHECK(run.Output().Width == 3);
	CHECK(run.Output().Height == 2);
	CHECK(run.Output().Pixels == std::vector<uint8_t>{1, 2, 3, 4, 5, 6, 7, 8, 0, 0, 0, 0,
													  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
	const auto clear = RunNode(
		"pc.solid", {}, {{"empty", true}, {"dimension_unit", EnumValue{0}}, {"dimension", Vector2{1, 1}}}
	);
	REQUIRE(clear.Ok);
	CHECK(clear.Output().Pixels == std::vector<uint8_t>{0, 0, 0, 0});
	const auto clipped = RunNode(
		"pc.solid",
		{{"foreground", &foreground}},
		{{"empty", true}, {"dimension_unit", EnumValue{0}}, {"dimension", Vector2{1, 1}}}
	);
	REQUIRE(clipped.Ok);
	CHECK(clipped.Output().Pixels == std::vector<uint8_t>{1, 2, 3, 4});
}

TEST_CASE(
	"Solid dimension controls round ties even clamp minimum and enforce budgets",
	"[imagegraph][node_generate]"
) {
	const Image mask{2, 3, std::vector<uint8_t>(24, 255), 0};
	const auto overridden = RunNode("pc.solid", {{"mask", &mask}}, {{"dimension", Vector2{100, 100}}});
	REQUIRE(overridden.Ok);
	CHECK(overridden.Output().Width == 2);
	CHECK(overridden.Output().Height == 3);
	const auto scaled = RunNode(
		"pc.solid",
		{{"mask", &mask}},
		{{"use_mask_dimension", false}, {"dimension_unit", EnumValue{0}}, {"dimension", Vector2{2.5, 3.5}}}
	);
	REQUIRE(scaled.Ok);
	CHECK(scaled.Output().Width == 2);
	CHECK(scaled.Output().Height == 4);
	const auto minimum =
		RunNode("pc.solid", {}, {{"dimension_unit", EnumValue{0}}, {"dimension", Vector2{-1, .1}}});
	REQUIRE(minimum.Ok);
	CHECK(minimum.Output().Width == 1);
	CHECK(minimum.Output().Height == 1);
	CHECK_FALSE(RunNode("pc.solid", {}, {{"dimension_unit", EnumValue{2}}}).Ok);
	CHECK_FALSE(RunNode("pc.solid", {}, {{"dimension_unit", EnumValue{3}}}).Ok);
	CHECK_FALSE(
		RunNode("pc.solid", {}, {{"dimension", Vector2{std::numeric_limits<double>::infinity(), 1}}}).Ok
	);
	const auto tooLarge = RunNode(
		"pc.solid",
		{},
		{{"dimension_unit", EnumValue{0}}, {"dimension", Vector2{double(Limits::MaximumDimension) + 1, 1}}}
	);
	CHECK(tooLarge.Code == Status::LimitExceeded);
	const auto *entry = FindCatalogueEntry("pc.solid");
	REQUIRE(entry);
	Node node{"n", "pc.solid", "", {}, {}, {}};
	EvaluationRequest request;
	detail::NodeContext context(node, *entry, request);
	context.ByteBudget = 3;
	context.Values = {{"dimension", Vector2{1, 1}}, {"dimension_unit", EnumValue{0}}};
	REQUIRE(detail::FindExecutor("pc.solid"));
	CHECK_FALSE(detail::FindExecutor("pc.solid")(context));
	CHECK(context.FailureCode == Status::LimitExceeded);
	CHECK(context.OutputImages.empty());
	const Image malformed{1, 1, {}, 0};
	CHECK(RunNode("pc.solid", {{"foreground", &malformed}}).Code == Status::InvalidValue);
	for (int64_t depth : {0})
		CHECK(
			RunNode("pc.solid", {}, {{"attribute_color_depth", EnumValue{depth}}}).Code ==
			Status::UnsupportedExecution
		);
	for (int64_t depth : {2, 3, 4, 5, 6, 7, 8}) {
		const auto generated = RunNode("pc.solid", {}, {{"attribute_color_depth", EnumValue{depth}}});
		INFO(generated.Message);
		REQUIRE(generated.Ok);
		CHECK(generated.Output().Format == *SourceSurfaceFormat(depth));
		CHECK(ValidSurfaceLayout(generated.Output(), Limits::MaximumDimension, Limits::MaximumOutputBytes));
	}
}

TEST_CASE(
	"Solid project inheritance evaluates and routes after document roundtrip", "[imagegraph][node_generate]"
) {
	Document document;
	document.FormatVersion = 7;
	document.Project = ProjectSettings{.SurfaceWidth = 8, .SurfaceHeight = 3};
	document.Nodes = {
		{"solid", "pc.solid", "", {}, {{"dimension", Vector2{.5, 1}}, {"color", Colour{9, 8, 7, 6}}}, {}}
	};
	document.Outputs = {{"pixels", "solid", "surface_out"}};
	Diagnostic diagnostic;
	Document roundtrip;
	REQUIRE(Read(Write(document), roundtrip, diagnostic) == Status::Ok);
	CHECK(roundtrip == document);
	Plan plan;
	REQUIRE(Compile(roundtrip, plan, diagnostic) == Status::Ok);
	Image output;
	INFO(diagnostic.Message);
	REQUIRE(Evaluate(roundtrip, plan, "pixels", {}, output, diagnostic) == Status::Ok);
	CHECK(output.Width == 4);
	CHECK(output.Height == 3);
	CHECK(
		std::vector<uint8_t>(output.Pixels.begin(), output.Pixels.begin() + 4) ==
		std::vector<uint8_t>{9, 8, 7, 6}
	);
}

TEST_CASE(
	"Solid linked scalar vector and junction defaults bypass Project units", "[imagegraph][node_generate]"
) {
	Document document;
	document.FormatVersion = 7;
	document.Project = ProjectSettings{.SurfaceWidth = 8, .SurfaceHeight = 3};
	document.Nodes = {
		{"number", "pc.number", "", {}, {{"value", 2.0}}, {}},
		{"source", "pc.solid", "", {}, {{"dimension", Vector2{2, 3}}, {"dimension_unit", EnumValue{0}}}, {}},
		{"size", "pc.surface_data", "", {}, {}, {}},
		{"solid", "pc.solid", "", {}, {{"dimension", Vector2{.5, 1}}, {"dimension_unit", EnumValue{1}}}, {}}
	};
	document.Outputs = {{"pixels", "solid", "surface_out"}};
	const auto evaluate = [&](uint32_t width, uint32_t height) {
		Diagnostic diagnostic;
		Plan plan;
		INFO(diagnostic.Message);
		REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
		Image output;
		REQUIRE(Evaluate(document, plan, "pixels", {}, output, diagnostic) == Status::Ok);
		CHECK(output.Width == width);
		CHECK(output.Height == height);
	};
	evaluate(4, 3);
	document.Links = {{"number", "number", "solid", "dimension"}};
	evaluate(2, 2);
	document.Links = {
		{"source", "surface_out", "size", "surface"}, {"size", "dimension", "solid", "dimension"}
	};
	evaluate(2, 3);
	document.Junctions = {{"route", "", ValueType::Vector2, std::nullopt}};
	document.Links = {
		{"source", "surface_out", "size", "surface"},
		{"size", "dimension", "route", "value"},
		{"route", "value", "solid", "dimension"}
	};
	evaluate(2, 3);
	document.Links = {{"route", "value", "solid", "dimension"}};
	document.Junctions[0].Default = Vector2{3, 2};
	evaluate(3, 2);
	document.Links = {{"source", "surface_out", "solid", "dimension"}};
	evaluate(2, 3);
}

TEST_CASE(
	"captured HDR survives persisted Solid and mesh material evaluation",
	"[imagegraph][node_generate][surface]"
) {
	RequestImageSource source{"hdr", Image{1, 1, std::vector<uint8_t>(16), 0, SurfaceFormat::RGBA32Float}};
	REQUIRE(StoreSurfacePixel(source.Data, 0, 0, {-2, 4, .25, 1}));
	const auto unchanged = source.Data.Pixels;
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"capture", "image.captured", "", {}, {{"source_id", std::string{"hdr"}}}},
		{"solid",
		 "pc.solid",
		 "",
		 {},
		 {{"dimension", Vector2{1, 1}},
		  {"dimension_unit", EnumValue{0}},
		  {"attribute_color_depth", EnumValue{5}},
		  {"empty", true}}},
		{"material", "pc.3_d_material", "", {}, {}},
		{"plane", "pc.3_d_mesh_plane", "", {}, {}}
	};
	document.Links = {
		{"capture", "image", "solid", "foreground"},
		{"solid", "surface_out", "material", "texture"},
		{"material", "material", "plane", "material"}
	};
	document.Outputs = {{"mesh", "plane", "mesh"}, {"pixels", "solid", "surface_out"}};
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
	CHECK(restored == document);
	Plan plan;
	REQUIRE(Compile(restored, plan, diagnostic) == Status::Ok);
	EvaluationRequest request;
	request.ImageSources = std::span<const RequestImageSource>(&source, 1);
	Image image;
	const auto imageStatus = Evaluate(restored, plan, "pixels", request, image, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(imageStatus == Status::Ok);
	CHECK(image.Format == SurfaceFormat::RGBA32Float);
	CHECK(image.Pixels == unchanged);
	EvaluatedValue output;
	const auto status = EvaluateValue(restored, plan, "mesh", request, output, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	const auto &mesh = std::get<MeshValue3D>(output.Data);
	REQUIRE(mesh.Data);
	REQUIRE(mesh.Data->Materials.size() == 1);
	const auto &material = mesh.Data->Materials.front().Get();
	REQUIRE(material.Surface);
	CHECK(material.Surface->Format == SurfaceFormat::RGBA32Float);
	CHECK(material.Surface->Pixels == unchanged);
	CHECK(source.Data.Pixels == unchanged);
	SurfacePixel sample{};
	REQUIRE(LoadSurfacePixel(*material.Surface, 0, 0, sample));
	CHECK(sample == SurfacePixel{-2, 4, .25, 1});
}

TEST_CASE(
	"captured images reject duplicate invalid and nonfinite sources atomically",
	"[imagegraph][node_generate][surface]"
) {
	Document document;
	document.Nodes = {{"capture", "image.captured", "", {}, {{"source_id", std::string{"hdr"}}}}};
	document.Outputs = {{"out", "capture", "image"}};
	Diagnostic diagnostic;
	Plan plan;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	RequestImageSource sources[2]{{"hdr", {1, 1, {1, 2, 3, 4}, 0}}, {"hdr", {1, 1, {5, 6, 7, 8}, 0}}};
	EvaluationRequest request;
	request.ImageSources = sources;
	Image output{1, 1, {9, 8, 7, 6}, 123};
	const Image retained = output;
	CHECK(Evaluate(document, plan, "out", request, output, diagnostic) == Status::DuplicateId);
	CHECK(output == retained);
	request.ImageSources = std::span<const RequestImageSource>(sources, 1);
	for (int invalid = 0; invalid < 3; ++invalid) {
		if (invalid == 0) sources[0].Data = {1, 1, {1, 2, 3}, 0};
		if (invalid == 1) sources[0].Data = {1, 1, {1, 2, 3, 4}, 0, static_cast<SurfaceFormat>(255)};
		if (invalid == 2) sources[0].Data = {1, 1, {0, 0, 128, 127}, 0, SurfaceFormat::R32Float};
		CHECK(Evaluate(document, plan, "out", request, output, diagnostic) == Status::InvalidValue);
		CHECK(output == retained);
	}
}

TEST_CASE(
	"Solid float output admission overlaps prior owned pixel storage", "[imagegraph][node_generate][surface]"
) {
	const auto *entry = FindCatalogueEntry("pc.solid");
	REQUIRE(entry);
	Node node{"solid", "pc.solid", "", {}, {}};
	EvaluationRequest request;
	Image previous{1, 1, std::vector<uint8_t>(16), 0, SurfaceFormat::RGBA32Float};
	REQUIRE(StoreSurfacePixel(previous, 0, 0, {-2, 4, .25, 1}));
	const auto original = previous.Pixels;
	uint64_t peak = 0;
	const auto run = [&](uint64_t limit, bool expected) {
		detail::EvaluationBudget budget(limit);
		auto previousCharge = budget.Reserve(previous.Pixels.capacity());
		REQUIRE(previousCharge);
		{
			detail::NodeContext context(node, *entry, request, budget);
			context.ByteBudget = limit;
			context.Images = {{"foreground", &previous}};
			context.Values = {
				{"dimension", Vector2{1, 1}},
				{"dimension_unit", EnumValue{0}},
				{"attribute_color_depth", EnumValue{4}},
				{"empty", true}
			};
			const bool success = detail::RunProcessorBatch(context, detail::FindExecutor(node.Type));
			INFO(context.FailureMessage);
			CHECK(success == expected);
			if (expected) {
				REQUIRE(context.OutputImages.size() == 1);
				CHECK(context.OutputImages.front().second.Format == SurfaceFormat::RGBA16Float);
				SurfacePixel sample{};
				REQUIRE(LoadSurfacePixel(context.OutputImages.front().second, 0, 0, sample));
				CHECK(sample == SurfacePixel{-2, 4, .25, 1});
			} else {
				CHECK(context.FailureCode == Status::LimitExceeded);
				CHECK(context.OutputImages.empty());
			}
			peak = budget.Peak();
		}
		CHECK(budget.Used() == previous.Pixels.capacity());
		CHECK(previous.Pixels == original);
	};
	run(Limits::MaximumEvaluationBytes, true);
	const uint64_t exact = peak;
	REQUIRE(exact > previous.Pixels.capacity() + 8);
	run(exact - 1, false);
	run(exact, true);
	CHECK(peak == exact);
}
