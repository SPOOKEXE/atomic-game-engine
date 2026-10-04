#include "ComplexGeneratorFixture.hpp"

#include <engine/testing/Suite.hpp>

#include <cfenv>
#include <cmath>
TEST_SUITE_ID("engine.imagegraph.source_kuwahara")
using namespace complex_generator_test;
namespace {
	Document Kuwahara(int type = 0) {
		Document d;
		d.FormatVersion = 9;
		MatrixValue matrix{4, 4, {}};
		ArrayValue palette{ValueType::Colour, {}};
		for (int i = 0; i < 16; ++i) {
			matrix.Values.push_back(i);
			palette.Elements.emplace_back(
				Colour{uint8_t(i * 13), uint8_t((15 - i) * 13), uint8_t((i % 4) * 60), 255}
			);
		}
		d.Nodes = {
			{"generator",
			 "pc.kuwahara",
			 "",
			 {},
			 {{"types", EnumValue{type}},
			  {"radius", int64_t{2}},
			  {"interpolate", EnumValue{1}},
			  {"oversample", EnumValue{3}}}},
			{"source",
			 "pc.interpret_matrix",
			 "",
			 {},
			 {{"matrix", matrix},
			  {"palette", palette},
			  {"mode", EnumValue{1}},
			  {"dimension", Vector2{4, 4}},
			  {"dimension_unit", EnumValue{0}},
			  {"attribute_color_depth", EnumValue{3}}}}
		};
		if (type == 2) Set(d, "zero_crossing", 2.5);
		d.Links = {{"source", "surface_out", "generator", "surface_in"}};
		d.Outputs = {{"out", "generator", "surface_out"}};
		return d;
	}
	Image Source(Document d) {
		d.Outputs = {{"out", "source", "surface_out"}};
		return Draw(d);
	}
	void Same(const Image &actual, const Image &expected) {
		CHECK(actual.Width == expected.Width);
		CHECK(actual.Height == expected.Height);
		CHECK(actual.Format == expected.Format);
		const bool pixels = actual.Pixels == expected.Pixels;
		CHECK(pixels);
		CHECK(actual.Hash == SurfaceHash(actual));
		CHECK(actual == expected);
	}
	void Erase(Document &d, std::string_view port) {
		auto &v = d.Nodes[0].Values;
		std::erase_if(v, [&](const auto &n) { return n.Port == port; });
	}
	Image Golden(int type) {
		static const std::array<std::vector<uint8_t>, 3> pixels{
			std::vector<uint8_t>{6,	  189, 7,	255, 16,  179, 50,	255, 33,  162, 126, 255, 42,
								 153, 173, 255, 43,	 152, 9,   255, 56,	 139, 54,  255, 72,	 123,
								 124, 255, 79,	116, 170, 255, 116, 79,	 10,  255, 123, 72,	 56,
								 255, 139, 56,	126, 255, 152, 43,	171, 255, 153, 42,	7,	 255,
								 162, 33,  54,	255, 179, 16,  130, 255, 189, 6,   173, 255},
			std::vector<uint8_t>{0,	  195, 0, 255, 13,	182, 60, 255, 26,  169, 120, 255, 39,  156, 180, 255,
								 52,  143, 0, 255, 65,	130, 60, 255, 78,  117, 120, 255, 91,  104, 180, 255,
								 104, 91,  0, 255, 117, 78,	 60, 255, 130, 65,	120, 255, 143, 52,	180, 255,
								 156, 39,  0, 255, 169, 26,	 60, 255, 182, 13,	120, 255, 195, 0,	180, 255},
			std::vector<uint8_t>{1,	  194, 1, 255, 14,	181, 60, 255, 27,  168, 120, 255, 39,  156, 179, 255,
								 52,  143, 1, 255, 65,	130, 60, 255, 78,  117, 120, 255, 91,  104, 179, 255,
								 104, 91,  1, 255, 117, 78,	 60, 255, 130, 65,	120, 255, 143, 52,	179, 255,
								 156, 39,  1, 255, 168, 27,	 60, 255, 181, 14,	120, 255, 194, 1,	179, 255}
		};
		Image image{4, 4, pixels[size_t(type)], 0};
		image.Hash = SurfaceHash(image);
		return image;
	}
}
TEST_CASE("Kuwahara three shader modes match independent ordered source literals", "[source_kuwahara]") {
	for (int type = 0; type < 3; ++type)
		Same(Draw(Kuwahara(type)), Golden(type));
}
TEST_CASE("Kuwahara inactive scalar Active copies source for every mode", "[source_kuwahara]") {
	for (int type = 0; type < 3; ++type) {
		auto d = Kuwahara(type);
		Set(d, "active", false);
		Same(Draw(d), Source(d));
	}
}
TEST_CASE("Kuwahara general default polynomial zero sum preserves previous output", "[source_kuwahara]") {
	auto d = Kuwahara(2);
	Set(d, "zero_crossing", .58);
	Refuse(d, Status::UnsupportedExecution, "zero_crossing", Limits::MaximumEvaluationBytes, "not finite");
}
TEST_CASE("Kuwahara anisotropic zero Alpha names the undefined source denominator", "[source_kuwahara]") {
	auto d = Kuwahara(1);
	Set(d, "alpha", 0.);
	Refuse(d, Status::UnsupportedExecution, "alpha", Limits::MaximumEvaluationBytes, "zero Alpha");
}
TEST_CASE("Kuwahara zero crossing denominator refuses both polynomial modes", "[source_kuwahara]") {
	for (int type : {1, 2}) {
		auto d = Kuwahara(type);
		Set(d, "zero_crossing", 0.);
		Refuse(
			d, Status::UnsupportedExecution, "zero_crossing", Limits::MaximumEvaluationBytes, "denominator"
		);
	}
}
TEST_CASE("Kuwahara Basic keeps Alpha crossing hardness sharpness and Unused inert", "[source_kuwahara]") {
	auto d = Kuwahara();
	const auto expected = Draw(d);
	for (std::string port : {"alpha", "zero_crossing", "hardness", "sharpness", "unused"}) {
		Set(d, port, 0.);
		Same(Draw(d), expected);
	}
}
TEST_CASE("Kuwahara Generalized keeps Alpha inert", "[source_kuwahara]") {
	auto d = Kuwahara(2);
	const auto expected = Draw(d);
	Set(d, "alpha", 0.);
	Same(Draw(d), expected);
}
TEST_CASE("Kuwahara radius validator clamps numeric negative values before rounding", "[source_kuwahara]") {
	auto d = Kuwahara();
	Set(d, "radius", int64_t{-1000000000});
	auto expected = d;
	Set(expected, "radius", int64_t{1});
	Same(Draw(d), Draw(expected));
}
TEST_CASE("Kuwahara linked scalar Radius applies source half-even integer getter", "[source_kuwahara]") {
	for (const auto &[radius, rounded] :
		 std::array{std::pair{1.5, int64_t{2}}, std::pair{2.5, int64_t{2}}, std::pair{3.5, int64_t{4}}}) {
		auto d = Kuwahara();
		d.Nodes.push_back({"radius_source", "pc.number_simple", "", {}, {{"value", radius}}});
		d.Links.push_back({"radius_source", "number", "generator", "radius"});
		auto expected = Kuwahara();
		Set(expected, "radius", rounded);
		Same(Draw(d), Draw(expected));
	}
}
TEST_CASE("Kuwahara mapped Radius absent map uses validated first endpoint", "[source_kuwahara]") {
	auto d = Kuwahara();
	Erase(d, "radius");
	Set(d, "radius_mapped", true);
	Set(d, "radius_map_range", Vector2{.5, 3.5});
	auto expected = Kuwahara();
	Set(expected, "radius", int64_t{1});
	Same(Draw(d), Draw(expected));
}
TEST_CASE("Kuwahara linked physical mapped pair precedes synthetic endpoints", "[source_kuwahara]") {
	auto d = Kuwahara();
	Set(d, "radius_mapped", true);
	Set(d, "radius_map_range", Vector2{9, 12});
	d.Nodes.push_back(Array("ranges", ValueType::Scalar, 2.5, 3.5));
	d.Links.push_back({"ranges", "array", "generator", "radius"});
	Same(Draw(d), Golden(0));
}
TEST_CASE("Kuwahara mapped white Radius chooses rounded high endpoint", "[source_kuwahara]") {
	auto d = Kuwahara();
	Erase(d, "radius");
	Set(d, "radius_mapped", true);
	Set(d, "radius_map_range", Vector2{.5, 2.5});
	d.Nodes.push_back(Solid("map", {1, 1}, {255, 255, 255, 0}));
	d.Links.push_back({"map", "surface_out", "generator", "radius_map"});
	Same(Draw(d), Golden(0));
}
TEST_CASE("Kuwahara unmapped Radius leaves linked map inert", "[source_kuwahara]") {
	auto d = Kuwahara();
	d.Nodes.push_back(Solid("map", {1, 1}, {0, 0, 0, 255}));
	d.Links.push_back({"map", "surface_out", "generator", "radius_map"});
	Same(Draw(d), Golden(0));
}
TEST_CASE("Kuwahara linked Surface Radius dimensions precede integer processing", "[source_kuwahara]") {
	auto d = Kuwahara();
	d.Nodes.push_back(Solid("dimension_source", {2, 3}, {255, 255, 255, 255}));
	d.Links.push_back({"dimension_source", "surface_out", "generator", "radius"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(d, plan, diagnostic) == Status::Ok);
	ImageArray rows;
	REQUIRE(EvaluateArray(d, plan, "out", {}, rows, diagnostic) == Status::Ok);
	REQUIRE(rows.Images.size() == 2);
	Same(rows.Images[0], Golden(0));
	auto second = Kuwahara();
	Set(second, "radius", int64_t{3});
	Same(rows.Images[1], Draw(second));
}
TEST_CASE("Kuwahara mask black preserves input and inversion restores filtered pixels", "[source_kuwahara]") {
	auto d = Kuwahara();
	d.Nodes.push_back(Solid("mask", {1, 1}, {0, 0, 0, 255}));
	d.Links.push_back({"mask", "surface_out", "generator", "mask"});
	Same(Draw(d), Source(d));
	Set(d, "invert_mask", true);
	Same(Draw(d), Golden(0));
}
TEST_CASE("Kuwahara zero Mix and no channels restore source", "[source_kuwahara]") {
	auto d = Kuwahara();
	Set(d, "mix", 0.);
	Same(Draw(d), Source(d));
	Set(d, "mix", 1.);
	Set(d, "channel", int64_t{0});
	Same(Draw(d), Source(d));
}
TEST_CASE("Kuwahara mask alpha only retains named common processing semantics", "[source_kuwahara]") {
	auto d = Kuwahara();
	d.Nodes.push_back(Solid("mask", {1, 1}, {0, 0, 0, 255}));
	d.Links.push_back({"mask", "surface_out", "generator", "mask"});
	Set(d, "mask_alpha_only", true);
	Same(Draw(d), Golden(0));
}
TEST_CASE("Kuwahara absent UV Map leaves UV Mix inert", "[source_kuwahara]") {
	auto d = Kuwahara();
	Set(d, "uv_mix", 100.);
	Same(Draw(d), Golden(0));
}
TEST_CASE("Kuwahara zero UV Mix keeps finite source sampler unchanged", "[source_kuwahara]") {
	auto d = Kuwahara();
	d.Nodes.push_back(Solid("uv", {1, 1}, {0, 0, 0, 255}));
	d.Links.push_back({"uv", "surface_out", "generator", "uv_map"});
	Set(d, "uv_mix", 0.);
	Same(Draw(d), Golden(0));
}
TEST_CASE(
	"Kuwahara anisotropic zero ellipse UV amplitude has an explicit source refusal", "[source_kuwahara]"
) {
	auto d = Kuwahara(1);
	Set(d, "radius", int64_t{1});
	d.Nodes.push_back(Solid("uv", {1, 1}, {255, 255, 0, 255}));
	d.Links.push_back({"uv", "surface_out", "generator", "uv_map"});
	Refuse(d, Status::UnsupportedExecution, "radius", Limits::MaximumEvaluationBytes, "zero ellipse");
}
TEST_CASE("Kuwahara Basic preserves opaque alpha while filtering source RGB", "[source_kuwahara]") {
	auto d = Kuwahara();
	for (auto &v : d.Nodes[1].Values)
		if (v.Port == "palette")
			for (auto &c : std::get<ArrayValue>(v.Data).Elements)
				std::get<Colour>(c).Alpha = 0;
	Same(Draw(d), Golden(0));
}
TEST_CASE("Kuwahara output depth changes storage without replacing source input", "[source_kuwahara]") {
	for (int depth : {2, 3, 4, 5, 6, 7, 8}) {
		auto d = Kuwahara();
		Set(d, "attribute_color_depth", EnumValue{depth});
		const auto out = Draw(d);
		CHECK(out.Format == *SourceSurfaceFormat(depth));
		CHECK(ValidSurfaceLayout(out, Limits::MaximumDimension, Limits::MaximumOutputBytes));
	}
}
TEST_CASE("Kuwahara red safe draw bypasses the filter before mask finishing", "[source_kuwahara]") {
	for (int depth : {6, 7, 8}) {
		auto d = Kuwahara();
		for (auto &v : d.Nodes[1].Values)
			if (v.Port == "attribute_color_depth") v.Data = EnumValue{depth};
		Set(d, "attribute_color_depth", EnumValue{3});
		const auto source = Source(d), out = Draw(d);
		for (uint32_t y = 0; y < out.Height; ++y)
			for (uint32_t x = 0; x < out.Width; ++x) {
				SurfacePixel p;
				REQUIRE(LoadSurfacePixel(source, x, y, p));
				const size_t offset = (size_t(y) * out.Width + x) * 4;
				const uint8_t red = uint8_t(std::floor(std::clamp(p[0], 0., 1.) * 255 + .5));
				CHECK(out.Pixels[offset] == red);
				CHECK(out.Pixels[offset + 1] == red);
				CHECK(out.Pixels[offset + 2] == red);
				CHECK(out.Pixels[offset + 3] == 255);
			}
	}
}
TEST_CASE(
	"Kuwahara animated integer Radius changes graph pixels at exact authored ticks", "[source_kuwahara]"
) {
	auto d = Kuwahara();
	d.Keyframes = {{"generator", "radius", 0, int64_t{1}}, {"generator", "radius", 2, int64_t{3}}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(d, plan, diagnostic) == Status::Ok);
	for (uint64_t tick : {0, 2}) {
		EvaluationRequest request;
		request.Tick = tick;
		Image out;
		REQUIRE(Evaluate(d, plan, "out", request, out, diagnostic) == Status::Ok);
		auto expected = Kuwahara();
		Set(expected, "radius", int64_t(tick ? 3 : 1));
		Same(out, Draw(expected));
	}
}
TEST_CASE("Kuwahara native serialization preserves three source shader choices", "[source_kuwahara]") {
	for (int type = 0; type < 3; ++type) {
		auto d = Kuwahara(type);
		std::string text;
		Diagnostic diagnostic;
		text = Write(d);
		Document restored;
		REQUIRE(Read(text, restored, diagnostic) == Status::Ok);
		Same(Draw(restored), Golden(type));
	}
}
TEST_CASE("Kuwahara later Radius rows are admitted before any first output", "[source_kuwahara]") {
	auto d = Kuwahara(2);
	d.Nodes.push_back(Array("radii", ValueType::Integer, int64_t{1}, int64_t{1000000000}));
	d.Links.push_back({"radii", "array", "generator", "radius"});
	Refuse(d, Status::LimitExceeded, "radius", Limits::MaximumEvaluationBytes, "whole-array work");
}
TEST_CASE("Kuwahara later mode cannot bypass unbounded generalized work admission", "[source_kuwahara]") {
	auto d = Kuwahara();
	Set(d, "radius", int64_t{1000000000});
	d.Nodes.push_back(Array("modes", ValueType::Enum, EnumValue{0}, EnumValue{2}));
	d.Links.push_back({"modes", "array", "generator", "types"});
	Refuse(d, Status::LimitExceeded, "radius", Limits::MaximumEvaluationBytes, "whole-array work");
}
TEST_CASE("Kuwahara later source pixels participate in whole array admission", "[source_kuwahara]") {
	auto d = Kuwahara();
	d.Nodes[1] = Solid("source", {1, 1}, {128, 64, 32, 255});
	d.Nodes.push_back(Solid("large", {256, 256}, {128, 64, 32, 255}));
	Node list{"list", "value.array", "", {}, {}};
	list.DynamicInputs = {{"a", ValueType::Image, std::nullopt}, {"b", ValueType::Image, std::nullopt}};
	d.Nodes.push_back(list);
	d.Links = {
		{"source", "surface_out", "list", "a"},
		{"large", "surface_out", "list", "b"},
		{"list", "array", "generator", "surface_in"}
	};
	Refuse(d, Status::LimitExceeded, "radius", Limits::MaximumEvaluationBytes, "whole-array work");
}
TEST_CASE("Kuwahara byte refusal preserves previous public image", "[source_kuwahara]") {
	auto d = Kuwahara();
	Refuse(d, Status::LimitExceeded, "", 1);
}
TEST_CASE("Kuwahara inactive first row still preflights later original input work", "[source_kuwahara]") {
	const auto *entry = FindCatalogueEntry("pc.kuwahara");
	const auto executor = engine::imagegraph::detail::FindExecutor("pc.kuwahara");
	REQUIRE(entry);
	REQUIRE(executor);
	Node authored{"generator", "pc.kuwahara", "", {}, {}};
	EvaluationRequest request;
	engine::imagegraph::detail::NodeContext c(authored, *entry, request);
	c.ByteBudget = Limits::MaximumEvaluationBytes;
	const auto first = imagegraph_test::MakeImage(1, 1, {1, 2, 3, 255});
	ImageArray original;
	original.Images = {first, imagegraph_test::MakeImage(256, 256, std::vector<uint8_t>(256 * 256 * 4, 255))};
	c.Images = {{"surface_in", &first}};
	c.ImageArrays = {{"surface_in", &original}};
	c.ProcessorCount = 2;
	c.Values = {{"active", false}, {"radius", int64_t{2}}};
	CHECK_FALSE(executor(c));
	CHECK(c.FailureCode == Status::LimitExceeded);
	CHECK(c.OutputImages.empty());
	CHECK(c.FailureMessage.find("whole-array work") != std::string::npos);
}

TEST_CASE("Kuwahara UV distance blend matches independent source pixels", "[source_kuwahara]") {
	auto d = Kuwahara();
	d.Nodes.push_back(Solid("uv", {1, 1}, {255, 0, 0, 255}));
	d.Links.push_back({"uv", "surface_out", "generator", "uv_map"});
	Image expected{
		4,
		4,
		std::vector<uint8_t>{99,  96, 92, 255, 104, 91, 119, 255, 111, 84, 158, 255, 116, 79, 180, 255,
							 122, 73, 92, 255, 127, 68, 119, 255, 134, 61, 159, 255, 138, 57, 180, 255,
							 157, 38, 89, 255, 163, 32, 116, 255, 172, 23, 158, 255, 179, 16, 180, 255,
							 175, 20, 89, 255, 181, 14, 114, 255, 191, 4,  161, 255, 195, 0,  180, 255},
		0
	};
	expected.Hash = SurfaceHash(expected);
	Same(Draw(d), expected);
}

TEST_CASE("Kuwahara mapped Radius grayscale preserves the fractional shader radius", "[source_kuwahara]") {
	auto d = Kuwahara();
	Erase(d, "radius");
	Set(d, "radius_mapped", true);
	Set(d, "radius_map_range", Vector2{2, 4});
	d.Nodes.push_back(Solid("map", {1, 1}, {128, 128, 128, 0}));
	d.Links.push_back({"map", "surface_out", "generator", "radius_map"});
	Image expected{
		4,
		4,
		std::vector<uint8_t>{8,	  187, 8,  255, 15,	 180, 37, 255, 38,	157, 140, 255, 44,	151, 172, 255,
							 31,  164, 14, 255, 42,	 153, 45, 255, 61,	134, 128, 255, 63,	132, 164, 255,
							 132, 63,  16, 255, 134, 61,  52, 255, 153, 42,	 135, 255, 164, 31,	 166, 255,
							 151, 44,  8,  255, 157, 38,  40, 255, 180, 15,	 143, 255, 187, 8,	 172, 255},
		0
	};
	expected.Hash = SurfaceHash(expected);
	Same(Draw(d), expected);
}

TEST_CASE(
	"Kuwahara accepted heterogeneous Radius rows retain independent scalar behavior", "[source_kuwahara]"
) {
	auto d = Kuwahara();
	d.Nodes.push_back(Array("radii", ValueType::Integer, int64_t{1}, int64_t{3}));
	d.Links.push_back({"radii", "array", "generator", "radius"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(d, plan, diagnostic) == Status::Ok);
	ImageArray rows;
	REQUIRE(EvaluateArray(d, plan, "out", {}, rows, diagnostic) == Status::Ok);
	REQUIRE(rows.Images.size() == 2);
	for (size_t i = 0; i < 2; ++i) {
		auto expected = Kuwahara();
		Set(expected, "radius", int64_t(i ? 3 : 1));
		Same(rows.Images[i], Draw(expected));
	}
}

TEST_CASE(
	"Kuwahara Basic keeps native16 tap limit while retaining authored Radius32 normalization",
	"[source_kuwahara]"
) {
	auto d = Kuwahara();
	Set(d, "radius", int64_t{32});
	Image expected{
		4,
		4,
		std::vector<uint8_t>{14,  181, 14, 255, 20,	 175, 35, 255, 39,	156, 120, 255, 47,	148, 163, 255,
							 32,  163, 18, 255, 43,	 152, 45, 255, 69,	126, 115, 255, 76,	119, 158, 255,
							 119, 76,  22, 255, 126, 69,  65, 255, 152, 43,	 135, 255, 163, 32,	 162, 255,
							 148, 47,  17, 255, 156, 39,  60, 255, 175, 20,	 145, 255, 181, 14,	 166, 255},
		0
	};
	expected.Hash = SurfaceHash(expected);
	Same(Draw(d), expected);
}

TEST_CASE("Kuwahara numeric half-even getter is independent of ambient rounding mode", "[source_kuwahara]") {
	struct RestoreRound {
		int Original = std::fegetround();
		~RestoreRound() {
			std::fesetround(Original);
		}
	} restore;
	for (int mode : {FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
		REQUIRE(std::fesetround(mode) == 0);
		for (const auto &[radius, rounded] :
			 std::array{std::pair{1.5, int64_t{2}}, std::pair{2.5, int64_t{2}}, std::pair{3.5, int64_t{4}}}) {
			auto linked = Kuwahara();
			linked.Nodes.push_back({"radius_source", "pc.number_simple", "", {}, {{"value", radius}}});
			linked.Links.push_back({"radius_source", "number", "generator", "radius"});
			auto expected = Kuwahara();
			Set(expected, "radius", rounded);
			Same(Draw(linked), Draw(expected));
		}
	}
}
TEST_CASE("Kuwahara unmapped huge synthetic range remains inert in Generalized mode", "[source_kuwahara]") {
	auto d = Kuwahara(2);
	Set(d, "radius_map_range", Vector2{1e12, 1e12});
	Same(Draw(d), Golden(2));
}
TEST_CASE("Kuwahara linked mapped physical pair bypasses huge synthetic work extent", "[source_kuwahara]") {
	auto d = Kuwahara(2);
	Set(d, "radius_mapped", true);
	Set(d, "radius_map_range", Vector2{1e12, 1e12});
	d.Nodes.push_back(Array("ranges", ValueType::Scalar, 2., 2.));
	d.Links.push_back({"ranges", "array", "generator", "radius"});
	Same(Draw(d), Golden(2));
}

TEST_CASE("Kuwahara source mapped toggle is static rather than a processor array", "[source_kuwahara]") {
	const auto *entry = FindCatalogueEntry("pc.kuwahara");
	REQUIRE(entry);
	const auto *input = FindCatalogueInput(*entry, "radius_mapped");
	REQUIRE(input);
	CHECK(input->SourceIndex == -1);
	CHECK(input->SourceKind == "MapToggle");
	ArrayValue flags{ValueType::Boolean, {false, true}};
	CHECK_FALSE(CatalogueAuthoredArray(*entry, *input, flags));
	auto authored = Kuwahara();
	Set(authored, "radius_mapped", flags);
	Plan p;
	Diagnostic diag;
	CHECK(Compile(authored, p, diag) == Status::TypeMismatch);
	CHECK(diag.Port == "radius_mapped");
	auto linked = Kuwahara();
	linked.Nodes.push_back(Array("flags", ValueType::Boolean, false, true));
	linked.Links.push_back({"flags", "array", "generator", "radius_mapped"});
	REQUIRE(Compile(linked, p, diag) == Status::Ok);
	Image output{1, 1, {9, 8, 7, 6}, 0};
	const auto prior = output;
	CHECK(Evaluate(linked, p, "out", {}, output, diag) == Status::UnsupportedExecution);
	CHECK(diag.NodeId == "generator");
	CHECK(diag.Port == "radius_mapped");
	CHECK(diag.Message.find("reader cannot consume an array input") != std::string::npos);
	CHECK(output == prior);
}
