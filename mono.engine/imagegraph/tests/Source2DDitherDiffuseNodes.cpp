#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <tuple>
TEST_SUITE_ID("engine.imagegraph.source_dither_diffuse")
using namespace engine::imagegraph;
using imagegraph_test::RunNode;
namespace {
	Image Pattern() {
		constexpr std::array<uint8_t, 24> values{19, 67,  128, 190, 240, 59, 151, 80, 122, 131, 90,	 199,
												 38, 201, 115, 128, 130, 63, 217, 48, 160, 84,	179, 108};
		Image image{8, 3, std::vector<uint8_t>(96), 0};
		for (size_t p = 0; p < values.size(); ++p) {
			image.Pixels[p * 4] = values[p];
			image.Pixels[p * 4 + 1] = 255 - values[p];
			image.Pixels[p * 4 + 2] = 128;
			image.Pixels[p * 4 + 3] = 255;
		}
		return image;
	}
	Document Graph(int64_t mode) {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {
			{"source",
			 "pc.solid",
			 "",
			 {},
			 {{"dimension", Vector2{3, 2}},
			  {"dimension_unit", EnumValue{0}},
			  {"color", Colour{128, 64, 192, 255}}}},
			{"dither", "pc.dither_diffuse", "", {}, {{"type", EnumValue{mode}}, {"seed", 0.0}}}
		};
		document.Links = {{"source", "surface_out", "dither", "surface_in"}};
		document.Outputs = {{"out", "dither", "surface_out"}};
		return document;
	}
}
TEST_CASE(
	"Error diffuse four source kernels retain asymmetric literal goldens",
	"[imagegraph][source_2d][source_dither_diffuse]"
) {
	const auto input = Pattern();
	constexpr std::array<std::array<std::string_view, 3>, 4> goldens{
		{{{"..###.#.", "#..#.#.#", ".##.#.#."}},
		 {{"..###.#.", ".#.#.#.#", "#.#.#.#."}},
		 {{"..###.#.", "#..#.#..", "#.#.#.##"}},
		 {{"..###..#", ".#.#.##.", "#.#..#.#"}}}
	};
	for (size_t mode = 0; mode < 4; ++mode) {
		auto run = RunNode(
			"pc.dither_diffuse",
			{{"surface_in", &input}},
			{{"type", EnumValue{int64_t(mode)}}, {"greyscale", true}}
		);
		INFO(run.Message);
		REQUIRE(run.Ok);
		for (size_t y = 0; y < 3; ++y)
			for (size_t x = 0; x < 8; ++x)
				for (size_t c = 0; c < 4; ++c)
					CHECK(
						run.Output().Pixels[(y * 8 + x) * 4 + c] ==
						(c == 3 || goldens[mode][y][x] == '#' ? 255 : 0)
					);
	}
}
TEST_CASE(
	"Error diffuse strict threshold applies to alpha and each RGB byte",
	"[imagegraph][source_2d][source_dither_diffuse]"
) {
	const auto input = imagegraph_test::MakeImage(1, 1, {128, 129, 127, 128});
	auto run = RunNode("pc.dither_diffuse", {{"surface_in", &input}});
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels == std::vector<uint8_t>{0, 255, 0, 0});
}
TEST_CASE(
	"Error diffuse linear transport truncates each signed16 write and resets at row boundaries",
	"[imagegraph][source_2d][source_dither_diffuse]"
) {
	const auto input = imagegraph_test::MakeImage(3, 2, {128, 0, 0, 255, 0, 0, 0, 255, 1, 0, 0, 255,
														 128, 0, 0, 255, 0, 0, 0, 255, 1, 0, 0, 255});
	auto run =
		RunNode("pc.dither_diffuse", {{"surface_in", &input}}, {{"type", EnumValue{3}}, {"greyscale", true}});
	REQUIRE(run.Ok);
	for (size_t p = 0; p < 6; ++p)
		CHECK(run.Output().Pixels[p * 4] == (p % 3 == 2 ? 255 : 0));
}
TEST_CASE(
	"Error diffuse seed is inert and greyscale reads only red",
	"[imagegraph][source_2d][source_dither_diffuse]"
) {
	const auto input = Pattern();
	auto first = RunNode("pc.dither_diffuse", {{"surface_in", &input}}, {{"seed", 1.0}, {"greyscale", true}});
	auto second =
		RunNode("pc.dither_diffuse", {{"surface_in", &input}}, {{"seed", 123456.0}, {"greyscale", true}});
	REQUIRE(first.Ok);
	REQUIRE(second.Ok);
	CHECK(first.Output().Pixels == second.Output().Pixels);
	Image changed = input;
	for (size_t p = 0; p < 24; ++p)
		for (size_t c = 1; c < 4; ++c)
			changed.Pixels[p * 4 + c] = 17;
	auto red = RunNode("pc.dither_diffuse", {{"surface_in", &changed}}, {{"greyscale", true}});
	REQUIRE(red.Ok);
	CHECK(red.Output().Pixels == first.Output().Pixels);
}
TEST_CASE(
	"Error diffuse processor mix channel and inactive preserve source",
	"[imagegraph][source_2d][source_dither_diffuse]"
) {
	const auto input = Pattern();
	for (auto control : std::array<std::string_view, 2>{"mix", "channel"}) {
		auto run = RunNode("pc.dither_diffuse", {{"surface_in", &input}}, {{control, int64_t{0}}});
		REQUIRE(run.Ok);
		CHECK(run.Output().Pixels == input.Pixels);
	}
	auto inactive =
		RunNode("pc.dither_diffuse", {{"surface_in", &input}}, {{"active", false}, {"type", EnumValue{99}}});
	REQUIRE(inactive.Ok);
	CHECK(inactive.Output().Pixels == input.Pixels);
}
TEST_CASE(
	"Error diffuse unsupported raw format remains explicit", "[imagegraph][source_2d][source_dither_diffuse]"
) {
	Image input{1, 1, {128}, 0};
	input.Format = SurfaceFormat::R8Unorm;
	auto run = RunNode("pc.dither_diffuse", {{"surface_in", &input}});
	CHECK_FALSE(run.Ok);
	CHECK(run.Code == Status::UnsupportedExecution);
	CHECK(run.Port == "surface_in");
}
TEST_CASE(
	"Error diffuse actual graph persists every mode and completes downstream",
	"[imagegraph][source_2d][source_dither_diffuse]"
) {
	for (int64_t mode = 0; mode < 4; ++mode) {
		auto document = Graph(mode);
		Document restored;
		Diagnostic diagnostic;
		REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
		restored.Nodes.push_back({"invert", "pc.invert", "", {}, {}});
		restored.Links.push_back({"dither", "surface_out", "invert", "surface_in"});
		restored.Outputs.push_back({"next", "invert", "surface_out"});
		Plan plan;
		REQUIRE(Compile(restored, plan, diagnostic) == Status::Ok);
		Image output;
		auto status = Evaluate(restored, plan, "out", {}, output, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
		REQUIRE(output.Pixels.size() == 24);
		for (auto byte : output.Pixels)
			CHECK((byte == 0 || byte == 255));
		Image downstream;
		REQUIRE(Evaluate(restored, plan, "next", {}, downstream, diagnostic) == Status::Ok);
		CHECK(downstream.Width == 3);
		CHECK(downstream.Height == 2);
	}
}
TEST_CASE(
	"Error diffuse byte refusal preserves earlier public result",
	"[imagegraph][source_2d][source_dither_diffuse]"
) {
	auto document = Graph(1);
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	Image result = imagegraph_test::MakeImage(1, 1, {31, 32, 33, 34});
	const Image prior = result;
	CHECK(Evaluate(document, plan, "out", {}, result, diagnostic, 16) == Status::LimitExceeded);
	CHECK(result.Pixels == prior.Pixels);
	CHECK(result.Width == prior.Width);
}
TEST_CASE(
	"Error diffuse work bound admits complete processor batch before scratch",
	"[imagegraph][source_2d][source_dither_diffuse]"
) {
	const auto *entry = FindCatalogueEntry("pc.dither_diffuse");
	REQUIRE(entry);
	const auto executor = detail::FindExecutor(entry->Type);
	REQUIRE(executor);
	Node authored{"dither", std::string(entry->Type), "", {}, {}};
	EvaluationRequest request;
	detail::NodeContext context(authored, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	context.ProcessorCount = 64000000;
	const auto input = Pattern();
	context.Images.emplace_back("surface_in", &input);
	REQUIRE_FALSE(executor(context));
	CHECK(context.FailureCode == Status::LimitExceeded);
	CHECK(context.OutputImages.empty());
}

TEST_CASE(
	"Error diffuse fractional negative writes truncate before later threshold",
	"[imagegraph][source_2d][source_dither_diffuse]"
) {
	const auto input = imagegraph_test::MakeImage(2, 1, {129, 0, 0, 255, 184, 0, 0, 255});
	auto run =
		RunNode("pc.dither_diffuse", {{"surface_in", &input}}, {{"type", EnumValue{0}}, {"greyscale", true}});
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels == std::vector<uint8_t>{255, 255, 255, 255, 0, 0, 0, 255});
}
TEST_CASE(
	"Error diffuse actual array rows match independent source mode evaluations",
	"[imagegraph][source_2d][source_dither_diffuse]"
) {
	auto document = Graph(0);
	Node modes{"modes", "pc.array", "", {}, {}, {}};
	for (size_t i = 0; i < 4; ++i)
		modes.DynamicInputs.push_back({"input_" + std::to_string(i), ValueType::Scalar, double(i)});
	document.Nodes.push_back(std::move(modes));
	document.Links.push_back({"modes", "array", "dither", "type"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	ImageArray result;
	auto status = EvaluateArray(document, plan, "out", {}, result, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	REQUIRE(result.Images.size() == 4);
	for (size_t i = 0; i < 4; ++i) {
		auto single = Graph(int64_t(i));
		Plan singlePlan;
		REQUIRE(Compile(single, singlePlan, diagnostic) == Status::Ok);
		Image image;
		REQUIRE(Evaluate(single, singlePlan, "out", {}, image, diagnostic) == Status::Ok);
		CHECK(result.Images[i].Pixels == image.Pixels);
	}
}
TEST_CASE(
	"Error diffuse mask and invert mask are applied after buffer diffusion",
	"[imagegraph][source_2d][source_dither_diffuse]"
) {
	const auto input = Pattern();
	Image mask{8, 3, std::vector<uint8_t>(96, 255), 0};
	for (size_t p = 0; p < 24; ++p)
		for (size_t c = 0; c < 3; ++c)
			mask.Pixels[p * 4 + c] = 0;
	auto hidden =
		RunNode("pc.dither_diffuse", {{"surface_in", &input}, {"mask", &mask}}, {{"mask_alpha_only", false}});
	REQUIRE(hidden.Ok);
	CHECK(hidden.Output().Pixels == input.Pixels);
	auto visible = RunNode(
		"pc.dither_diffuse",
		{{"surface_in", &input}, {"mask", &mask}},
		{{"mask_alpha_only", false}, {"invert_mask", true}}
	);
	auto plain = RunNode("pc.dither_diffuse", {{"surface_in", &input}});
	REQUIRE(visible.Ok);
	REQUIRE(plain.Ok);
	CHECK(visible.Output().Pixels == plain.Output().Pixels);
}
TEST_CASE(
	"Error diffuse expensive later feather is admitted before allocating the first active row",
	"[imagegraph][source_2d][source_dither_diffuse]"
) {
	auto document = Graph(3);
	document.Nodes[0].Values[0].Data = Vector2{256, 256};
	for (const auto &[port, first, second] : std::array<std::tuple<std::string_view, double, double>, 3>{
			 {{"greyscale", 1, 0}, {"type", 3, 1}, {"mask_feather", 0, 64}}
		 }) {
		Node values{std::string(port), "pc.array", "", {}, {}, {}};
		values.DynamicInputs = {
			{"input_0", ValueType::Scalar, first}, {"input_1", ValueType::Scalar, second}
		};
		document.Nodes.push_back(std::move(values));
		document.Links.push_back({std::string(port), "array", "dither", std::string(port)});
	}
	document.Links.push_back({"source", "surface_out", "dither", "mask"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	Image result = imagegraph_test::MakeImage(1, 1, {13, 14, 15, 16});
	const Image prior = result;
	// The producer fits; remaining bytes cannot fit the first row's 256x256
	// output. Later work must be admitted before allocating its scratch or output.
	auto status = Evaluate(document, plan, "out", {}, result, diagnostic, 500000);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::LimitExceeded);
	CHECK(diagnostic.NodeId == "dither");
	CHECK(diagnostic.Message.find("work budget") != std::string::npos);
	CHECK(result == prior);
}
TEST_CASE(
	"Error diffuse heterogeneous dimensions and modes match standalone row oracles",
	"[imagegraph][source_2d][source_dither_diffuse]"
) {
	auto document = Graph(0);
	Node sizes{"sizes", "pc.array", "", {}, {}, {}};
	sizes.DynamicInputs = {
		{"input_0", ValueType::Vector2, Vector2{1, 1}}, {"input_1", ValueType::Vector2, Vector2{8, 3}}
	};
	document.Nodes.push_back(std::move(sizes));
	document.Links.push_back({"sizes", "array", "source", "dimension"});
	for (const auto &[port, first, second] :
		 std::array<std::tuple<std::string_view, double, double>, 2>{{{"greyscale", 1, 0}, {"type", 3, 1}}}) {
		Node values{std::string(port), "pc.array", "", {}, {}, {}};
		values.DynamicInputs = {
			{"input_0", ValueType::Scalar, first}, {"input_1", ValueType::Scalar, second}
		};
		document.Nodes.push_back(std::move(values));
		document.Links.push_back({std::string(port), "array", "dither", std::string(port)});
	}
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	ImageArray rows;
	auto status = EvaluateArray(document, plan, "out", {}, rows, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	REQUIRE(rows.Images.size() == 2);
	for (size_t row = 0; row < 2; ++row) {
		auto single = Graph(row == 0 ? 3 : 1);
		single.Nodes[0].Values[0].Data = row == 0 ? Vector2{1, 1} : Vector2{8, 3};
		single.Nodes[1].Values.push_back({"greyscale", row == 0});
		Plan compiled;
		REQUIRE(Compile(single, compiled, diagnostic) == Status::Ok);
		Image oracle;
		REQUIRE(Evaluate(single, compiled, "out", {}, oracle, diagnostic) == Status::Ok);
		CHECK(rows.Images[row] == oracle);
	}
}

TEST_CASE(
	"Error diffuse private original rows preflight before a disabled row copy",
	"[imagegraph][source_2d][source_dither_diffuse]"
) {
	const auto *entry = FindCatalogueEntry("pc.dither_diffuse");
	REQUIRE(entry);
	const auto executor = detail::FindExecutor(entry->Type);
	REQUIRE(executor);
	Node authored{"dither", std::string(entry->Type), "", {}, {}};
	EvaluationRequest request;
	detail::NodeContext context(authored, *entry, request);
	context.ByteBudget = 1;
	context.ProcessorCount = 2;
	ImageArray surfaces;
	surfaces.Images.push_back(imagegraph_test::MakeImage(1, 1, {128, 128, 128, 255}));
	surfaces.Images.push_back(Image{256, 256, std::vector<uint8_t>(256 * 256 * 4, 255), 0});
	context.Images = {{"surface_in", &surfaces.Images[0]}, {"mask", &surfaces.Images[0]}};
	context.ImageArrays = {{"surface_in", &surfaces}, {"mask", &surfaces}};
	context.Values = {{"active", false}, {"greyscale", true}, {"type", EnumValue{3}}, {"mask_feather", 0.0}};
	// Source Active explicitly rejectArray()s. This is a defensive private-context
	// test, not a claim that those Active arrays are accepted by public evaluation.
	const std::array<Value, 4> values{
		ArrayValue{ValueType::Boolean, {false, true}},
		ArrayValue{ValueType::Boolean, {true, false}},
		ArrayValue{ValueType::Integer, {int64_t{3}, int64_t{1}}},
		ArrayValue{ValueType::Scalar, {0.0, 64.0}}
	};
	const std::array<std::pair<std::string_view, const Value *>, 4> originals{
		{{"active", &values[0]},
		 {"greyscale", &values[1]},
		 {"type", &values[2]},
		 {"mask_feather", &values[3]}}
	};
	context.ProcessorOriginalValues = originals;
	REQUIRE_FALSE(executor(context));
	CHECK(context.FailureCode == Status::LimitExceeded);
	CHECK(context.FailureMessage.find("work budget") != std::string::npos);
	CHECK(context.OutputImages.empty());
}
