#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.source_surface_replace")
using namespace engine::imagegraph;
using imagegraph_test::RunNode;
namespace {
	Image ReplaceSolid(uint32_t w, uint32_t h, Colour c) {
		Image image{w, h, std::vector<uint8_t>(size_t(w) * h * 4), 0};
		for (size_t i = 0; i < size_t(w) * h; ++i) {
			image.Pixels[i * 4] = c.Red;
			image.Pixels[i * 4 + 1] = c.Green;
			image.Pixels[i * 4 + 2] = c.Blue;
			image.Pixels[i * 4 + 3] = c.Alpha;
		}
		return image;
	}
	Document ReplaceGraph(bool fast = true, uint32_t dimension = 2) {
		Document d;
		d.FormatVersion = 9;
		const auto solid = [&](std::string id, Colour c) {
			return Node{
				std::move(id),
				"pc.solid",
				"",
				{},
				{{"dimension", Vector2{double(dimension), 1}}, {"dimension_unit", EnumValue{0}}, {"color", c}}
			};
		};
		d.Nodes = {
			solid("base", {255, 0, 0, 255}),
			solid("target", {255, 0, 0, 255}),
			solid("replacement", {0, 255, 0, 255}),
			{"replace", "pc.surface_replace", "", {}, {{"fast_mode", fast}, {"seed", 23.0}}}
		};
		d.Links = {
			{"base", "surface_out", "replace", "base_image"},
			{"target", "surface_out", "replace", "target_image"},
			{"replacement", "surface_out", "replace", "replacement_image"}
		};
		d.Outputs = {{"out", "replace", "surface_out"}};
		return d;
	}
	void ReplaceExpect(const Image &image, Colour c) {
		REQUIRE(image.Pixels.size() == size_t(image.Width) * image.Height * 4);
		for (size_t i = 0; i < image.Pixels.size(); i += 4) {
			CHECK(image.Pixels[i] == c.Red);
			CHECK(image.Pixels[i + 1] == c.Green);
			CHECK(image.Pixels[i + 2] == c.Blue);
			CHECK(image.Pixels[i + 3] == c.Alpha);
		}
	}
}
TEST_CASE("Surface Replace fast shader replaces all opaque matching origins", "[source_surface_replace]") {
	const auto base = ReplaceSolid(3, 1, {255, 0, 0, 255}), target = ReplaceSolid(1, 1, {255, 0, 0, 255}),
			   replacement = ReplaceSolid(1, 1, {0, 255, 0, 255});
	for (bool fast : {false, true}) {
		auto run = RunNode(
			"pc.surface_replace",
			{{"base_image", &base}, {"target_image", &target}, {"replacement_image", &replacement}},
			{{"fast_mode", fast}}
		);
		INFO(run.Message);
		REQUIRE(run.Ok);
		ReplaceExpect(run.Output(), {0, 255, 0, 255});
	}
}
TEST_CASE(
	"Surface Replace full match preserves source template coordinate mapping", "[source_surface_replace]"
) {
	const auto base = ReplaceSolid(2, 1, {255, 0, 0, 255}), target = ReplaceSolid(2, 1, {255, 0, 0, 255});
	Image replacement{2, 1, {0, 255, 0, 255, 0, 0, 255, 255}, 0};
	auto run = RunNode(
		"pc.surface_replace",
		{{"base_image", &base}, {"target_image", &target}, {"replacement_image", &replacement}},
		{{"fast_mode", false}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels == replacement.Pixels);
}
TEST_CASE(
	"Surface Replace fast find clamps template reads at the base boundary", "[source_surface_replace]"
) {
	const auto base = ReplaceSolid(2, 1, {255, 0, 0, 255}), target = ReplaceSolid(2, 1, {255, 0, 0, 255}),
			   replacement = ReplaceSolid(1, 1, {0, 255, 0, 255});
	auto run = RunNode(
		"pc.surface_replace",
		{{"base_image", &base}, {"target_image", &target}, {"replacement_image", &replacement}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	ReplaceExpect(run.Output(), {0, 255, 0, 255});
}
TEST_CASE(
	"Surface Replace normal MRT blend and source alpha composition preserve fractional alpha",
	"[source_surface_replace]"
) {
	const auto base = ReplaceSolid(1, 1, {255, 0, 0, 255}), target = base,
			   replacement = ReplaceSolid(1, 1, {0, 255, 0, 128});
	for (bool fast : {false, true})
		for (bool draw : {false, true})
			for (bool empty : {false, true}) {
				auto run = RunNode(
					"pc.surface_replace",
					{{"base_image", &base}, {"target_image", &target}, {"replacement_image", &replacement}},
					{{"fast_mode", fast}, {"draw_base_image", draw}, {"replace_empty", empty}}
				);
				INFO(run.Message);
				REQUIRE(run.Ok);
				// Both shaders emit replacement alpha .502; bm_normal stores alpha .252 (64/255).
				// Fast eraser stores .502 RGB/.252 alpha; full eraser is opaque white.
				const uint8_t red =
					draw ? (empty ? (fast ? uint8_t(95) : uint8_t(0)) : uint8_t(191)) : uint8_t(0);
				const uint8_t alpha = draw && !(empty && !fast) ? uint8_t(255) : uint8_t(64);
				ReplaceExpect(run.Output(), {red, 128, 0, alpha});
			}
}
TEST_CASE(
	"Surface Replace exact RGBA distance and pixel threshold affect match admission",
	"[source_surface_replace]"
) {
	const auto base = ReplaceSolid(1, 1, {255, 0, 0, 255}), target = ReplaceSolid(1, 1, {0, 0, 255, 255}),
			   replacement = ReplaceSolid(1, 1, {0, 255, 0, 255});
	auto accepted = RunNode(
		"pc.surface_replace",
		{{"base_image", &base}, {"target_image", &target}, {"replacement_image", &replacement}},
		{{"color_threshold", 1.0}}
	);
	REQUIRE(accepted.Ok);
	ReplaceExpect(accepted.Output(), {0, 255, 0, 255});
	auto refused = RunNode(
		"pc.surface_replace",
		{{"base_image", &base}, {"target_image", &target}, {"replacement_image", &replacement}},
		{{"color_threshold", .1}}
	);
	CHECK_FALSE(refused.Ok);
	CHECK(refused.Code == Status::UnsupportedExecution);
	CHECK(refused.Message.find("unwritten") != std::string::npos);
	auto full = RunNode(
		"pc.surface_replace",
		{{"base_image", &base}, {"target_image", &target}, {"replacement_image", &replacement}},
		{{"fast_mode", false}, {"pixel_threshold", 1.0}}
	);
	INFO(full.Message);
	REQUIRE(full.Ok);
	ReplaceExpect(full.Output(), {0, 255, 0, 255});
}
TEST_CASE(
	"Surface Replace undefined transparent replacement MRT fragments are diagnosed",
	"[source_surface_replace]"
) {
	const auto base = ReplaceSolid(1, 1, {255, 0, 0, 255}), target = base,
			   replacement = ReplaceSolid(1, 1, {0, 255, 0, 0});
	for (bool fast : {false, true}) {
		auto run = RunNode(
			"pc.surface_replace",
			{{"base_image", &base}, {"target_image", &target}, {"replacement_image", &replacement}},
			{{"fast_mode", fast}}
		);
		CHECK_FALSE(run.Ok);
		CHECK(run.Code == Status::UnsupportedExecution);
	}
}
TEST_CASE(
	"Surface Replace randomized shader index with one replacement is defined", "[source_surface_replace]"
) {
	const auto base = ReplaceSolid(1, 1, {255, 0, 0, 255}), target = base,
			   replacement = ReplaceSolid(1, 1, {0, 255, 0, 255});
	auto run = RunNode(
		"pc.surface_replace",
		{{"base_image", &base}, {"target_image", &target}, {"replacement_image", &replacement}},
		{{"array_mode", EnumValue{1}}, {"seed", 23.0}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	ReplaceExpect(run.Output(), {0, 255, 0, 255});
}
TEST_CASE(
	"Surface Replace missing surfaces and unresolved randomized seed are explicit", "[source_surface_replace]"
) {
	const auto base = ReplaceSolid(1, 1, {255, 0, 0, 255});
	auto missing = RunNode("pc.surface_replace", {});
	CHECK_FALSE(missing.Ok);
	CHECK(missing.Code == Status::InvalidValue);
	auto empty = RunNode("pc.surface_replace", {{"base_image", &base}});
	CHECK_FALSE(empty.Ok);
	CHECK(empty.Code == Status::UnsupportedExecution);
	auto random = RunNode(
		"pc.surface_replace",
		{{"base_image", &base}, {"target_image", &base}, {"replacement_image", &base}},
		{{"array_mode", EnumValue{1}}}
	);
	CHECK_FALSE(random.Ok);
	CHECK(random.Code == Status::InvalidValue);
}
TEST_CASE(
	"Surface Replace actual graphs persist complete fast and full controls", "[source_surface_replace]"
) {
	for (bool fast : {false, true}) {
		auto d = ReplaceGraph(fast);
		Diagnostic diagnostic;
		Document restored;
		REQUIRE(Read(Write(d), restored, diagnostic) == Status::Ok);
		CHECK(restored == d);
		Plan plan;
		REQUIRE(Compile(restored, plan, diagnostic) == Status::Ok);
		Image image;
		EvaluationRequest request;
		const auto status = Evaluate(restored, plan, "out", request, image, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
		ReplaceExpect(image, {0, 255, 0, 255});
	}
}
TEST_CASE("Surface Replace public byte refusal preserves prior image", "[source_surface_replace]") {
	auto d = ReplaceGraph();
	Diagnostic diagnostic;
	Plan plan;
	REQUIRE(Compile(d, plan, diagnostic) == Status::Ok);
	Image image = ReplaceSolid(1, 1, {3, 5, 7, 11}), prior = image;
	EvaluationRequest request;

	const auto status = Evaluate(d, plan, "out", request, image, diagnostic, 1);
	CHECK(status == Status::LimitExceeded);
	CHECK(image == prior);
}
TEST_CASE("Surface Replace target arrays remain one source template list", "[source_surface_replace]") {
	for (bool fast : {false, true}) {
		auto d = ReplaceGraph(fast, 1);
		d.Nodes.push_back(
			{"miss",
			 "pc.solid",
			 "",
			 {},
			 {{"dimension", Vector2{1, 1}},
			  {"dimension_unit", EnumValue{0}},
			  {"color", Colour{0, 0, 255, 255}}}}
		);
		Node list{"targets", "value.array", "", {}, {}};
		list.DynamicInputs = {
			{"first", ValueType::Image, std::nullopt}, {"second", ValueType::Image, std::nullopt}
		};
		d.Nodes.push_back(std::move(list));
		std::erase_if(d.Links, [](const auto &l) { return l.ToPort == "target_image"; });
		d.Links.push_back({"target", "surface_out", "targets", "first"});
		d.Links.push_back({"miss", "surface_out", "targets", "second"});
		d.Links.push_back({"targets", "array", "replace", "target_image"});
		Plan plan;
		Diagnostic diag;
		REQUIRE(Compile(d, plan, diag) == Status::Ok);
		Image out;
		const auto status = Evaluate(d, plan, "out", {}, out, diag);
		INFO(diag.Message);
		REQUIRE(status == Status::Ok);
		ReplaceExpect(out, {0, 255, 0, 255});
	}
}
TEST_CASE("Surface Replace base image arrays produce heterogeneous output rows", "[source_surface_replace]") {
	auto d = ReplaceGraph(true, 1);
	d.Nodes.push_back(
		{"large",
		 "pc.solid",
		 "",
		 {},
		 {{"dimension", Vector2{3, 2}}, {"dimension_unit", EnumValue{0}}, {"color", Colour{255, 0, 0, 255}}}}
	);
	Node list{"bases", "value.array", "", {}, {}};
	list.DynamicInputs = {
		{"first", ValueType::Image, std::nullopt}, {"second", ValueType::Image, std::nullopt}
	};
	d.Nodes.push_back(std::move(list));
	std::erase_if(d.Links, [](const auto &l) { return l.ToPort == "base_image"; });
	d.Links.push_back({"base", "surface_out", "bases", "first"});
	d.Links.push_back({"large", "surface_out", "bases", "second"});
	d.Links.push_back({"bases", "array", "replace", "base_image"});
	Plan plan;
	Diagnostic diag;
	REQUIRE(Compile(d, plan, diag) == Status::Ok);
	ImageArray out;
	const auto status = EvaluateArray(d, plan, "out", {}, out, diag);
	INFO(diag.Message);
	REQUIRE(status == Status::Ok);
	REQUIRE(out.Images.size() == 2);
	CHECK(out.Images[0].Width == 1);
	CHECK(out.Images[1].Width == 3);
	CHECK(out.Images[1].Height == 2);
	for (const auto &image : out.Images)
		ReplaceExpect(image, {0, 255, 0, 255});
}
TEST_CASE(
	"Surface Replace later full-match work is admitted before first row scratch", "[source_surface_replace]"
) {
	auto d = ReplaceGraph(true, 64);
	d.Nodes[0].Values[0].Data = Vector2{64, 64};
	d.Nodes[1].Values[0].Data = Vector2{8, 8};
	d.Nodes[2].Values[0].Data = Vector2{1, 1};
	Node modes{"modes", "pc.array", "", {}, {}};
	modes.DynamicInputs = {{"input_0", ValueType::Boolean, true}, {"input_1", ValueType::Boolean, false}};
	d.Nodes.push_back(std::move(modes));
	d.Links.push_back({"modes", "array", "replace", "fast_mode"});
	Plan plan;
	Diagnostic diag;
	REQUIRE(Compile(d, plan, diag) == Status::Ok);
	Image out = ReplaceSolid(1, 1, {3, 5, 7, 11}), prior = out;
	const auto status = Evaluate(d, plan, "out", {}, out, diag, 60000);
	INFO(diag.Message);
	CHECK(status == Status::LimitExceeded);
	CHECK(diag.NodeId == "replace");
	CHECK(diag.Message.find("complete processor batch exceeds work budget") != std::string::npos);
	CHECK(out == prior);
}
TEST_CASE("Surface Replace clamps the base draw before compositing into RGBA8", "[source_surface_replace]") {
	Image base;
	base.Width = base.Height = 1;
	base.Format = SurfaceFormat::RGBA32Float;
	base.Pixels.resize(16);
	REQUIRE(StoreSurfacePixel(base, 0, 0, {2, 0, 0, 1}));
	const auto target = base, replacement = ReplaceSolid(1, 1, {0, 255, 0, 128});
	auto run = RunNode(
		"pc.surface_replace",
		{{"base_image", &base}, {"target_image", &target}, {"replacement_image", &replacement}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	CHECK(run.Output().Format == SurfaceFormat::RGBA8Unorm);
	ReplaceExpect(run.Output(), {191, 128, 0, 255});
}
TEST_CASE(
	"Surface Replace replacement lists retain pass-index and undefined MRT boundaries",
	"[source_surface_replace]"
) {
	auto d = ReplaceGraph(true, 1);
	d.Nodes.push_back(
		{"second",
		 "pc.solid",
		 "",
		 {},
		 {{"dimension", Vector2{1, 1}}, {"dimension_unit", EnumValue{0}}, {"color", Colour{0, 0, 255, 255}}}}
	);
	Node list{"replacements", "value.array", "", {}, {}};
	list.DynamicInputs = {
		{"first", ValueType::Image, std::nullopt}, {"second", ValueType::Image, std::nullopt}
	};
	d.Nodes.push_back(std::move(list));
	std::erase_if(d.Links, [](const auto &l) { return l.ToPort == "replacement_image"; });
	d.Links.push_back({"replacement", "surface_out", "replacements", "first"});
	d.Links.push_back({"second", "surface_out", "replacements", "second"});
	d.Links.push_back({"replacements", "array", "replace", "replacement_image"});
	Plan plan;
	Diagnostic diag;
	REQUIRE(Compile(d, plan, diag) == Status::Ok);
	Image out = ReplaceSolid(1, 1, {3, 5, 7, 11}), prior = out;
	const auto status = Evaluate(d, plan, "out", {}, out, diag);
	INFO(diag.Message);
	CHECK(status == Status::UnsupportedExecution);
	CHECK(diag.NodeId == "replace");
	CHECK(diag.Port == "replacement_image");
	CHECK(out == prior);
}
TEST_CASE(
	"Surface Replace original scalar uniform arrays are refused only when read", "[source_surface_replace]"
) {
	const auto base = ReplaceSolid(1, 1, {255, 0, 0, 255}), target = base,
			   replacement = ReplaceSolid(1, 1, {0, 255, 0, 255});
	const ArrayValue modes{ValueType::Enum, {EnumValue{0}, EnumValue{1}}},
		seeds{ValueType::Scalar, {23.0, 31.0}};
	auto mode = RunNode(
		"pc.surface_replace",
		{{"base_image", &base}, {"target_image", &target}, {"replacement_image", &replacement}},
		{{"array_mode", modes}}
	);
	CHECK_FALSE(mode.Ok);
	CHECK(mode.Code == Status::UnsupportedExecution);
	CHECK(mode.Port == "array_mode");
	auto seed = RunNode(
		"pc.surface_replace",
		{{"base_image", &base}, {"target_image", &target}, {"replacement_image", &replacement}},
		{{"array_mode", EnumValue{1}}, {"seed", seeds}}
	);
	CHECK_FALSE(seed.Ok);
	CHECK(seed.Code == Status::UnsupportedExecution);
	CHECK(seed.Port == "seed");
	auto unused = RunNode(
		"pc.surface_replace",
		{{"base_image", &base}, {"target_image", &target}, {"replacement_image", &replacement}},
		{{"seed", seeds}}
	);
	INFO(unused.Message);
	REQUIRE(unused.Ok);
	ReplaceExpect(unused.Output(), {0, 255, 0, 255});
}
