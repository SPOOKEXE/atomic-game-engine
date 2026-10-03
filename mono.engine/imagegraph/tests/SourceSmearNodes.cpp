#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/Surface.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("engine.imagegraph.source_smear")
using namespace engine::imagegraph;
namespace {
	Document Graph(std::vector<AuthoredValue> values = {}) {
		if (std::none_of(values.begin(), values.end(), [](const auto &v) { return v.Port == "strength"; }))
			values.push_back({"strength", 0.});
		Document d;
		d.FormatVersion = 9;
		d.Nodes = {
			{"source", "image.captured", "", {}, {{"source_id", std::string("source")}}},
			{"smear", "pc.smear", "", {}, std::move(values)}
		};
		d.Links = {{"source", "image", "smear", "surface_in"}};
		d.Outputs = {{"colour", "smear", "surface_out"}, {"depth", "smear", "depth_pass"}};
		return d;
	}
	void Set(Node &node, std::string port, Value value) {
		for (auto &v : node.Values)
			if (v.Port == port) {
				v.Data = std::move(value);
				return;
			}
		node.Values.push_back({std::move(port), std::move(value)});
	}
	Image Solid(uint32_t w, uint32_t h, std::array<uint8_t, 4> color) {
		Image image{w, h, std::vector<uint8_t>(size_t(w) * h * 4), 0};
		for (size_t i = 0; i < image.Pixels.size(); i++)
			image.Pixels[i] = color[i % 4];
		return image;
	}
	Plan Compiled(const Document &d) {
		Plan p;
		Diagnostic diag;
		const auto status = Compile(d, p, diag);
		INFO(diag.Message);
		REQUIRE(status == Status::Ok);
		return p;
	}
	Image
	Run(const Document &d,
		const Image &source,
		std::string_view output = "colour",
		std::span<const RequestImageSource> extras = {},
		uint64_t tick = 0) {
		auto p = Compiled(d);
		std::vector<RequestImageSource> images{{"source", source}};
		images.insert(images.end(), extras.begin(), extras.end());
		EvaluationRequest request;
		request.ImageSources = images;
		request.Tick = tick;
		Image out;
		Diagnostic diag;
		const auto s = Evaluate(d, p, std::string(output), request, out, diag);
		INFO(diag.Port << " " << diag.Message);
		REQUIRE(s == Status::Ok);
		return out;
	}
	void Refused(
		const Document &d,
		const Image &source,
		std::string_view output,
		std::string_view message,
		Status expected = Status::UnsupportedExecution,
		uint64_t bytes = Limits::MaximumEvaluationBytes
	) {
		auto p = Compiled(d);
		std::array images{RequestImageSource{"source", source}};
		EvaluationRequest request;
		request.ImageSources = images;
		Image prior = Solid(1, 1, {9, 8, 7, 6}), out = prior;
		Diagnostic diag;
		const auto status = Evaluate(d, p, std::string(output), request, out, diag, bytes);
		INFO(diag.Message);
		CHECK(status == expected);
		CHECK(diag.Message.find(message) != std::string::npos);
		CHECK(out == prior);
	}
	Curve Constant(double value) {
		Curve c;
		c.Header = {0, 1, 0, 0, 1, 0};
		c.Anchors = {{0, 0, 0, value, 0, 0}, {0, 0, 1, value, 0, 0}};
		return c;
	}
} // namespace
TEST_CASE("Smear normal premultiplication leaves depth unwritten on a constant sweep", "[smear]") {
	const auto d = Graph();
	const auto source = Solid(4, 1, {255, 128, 64, 128});
	CHECK(
		Run(d, source).Pixels ==
		std::vector<uint8_t>{128, 64, 32, 128, 128, 64, 32, 128, 128, 64, 32, 128, 128, 64, 32, 128}
	);
	Refused(d, source, "depth", "unwritten");
}
TEST_CASE("Smear inverted modes have literal distance and alpha depth goldens", "[smear]") {
	auto d = Graph({{"invert", true}});
	const auto source = Solid(4, 1, {255, 255, 255, 128});
	CHECK(Run(d, source).Pixels == Solid(4, 1, {128, 128, 128, 255}).Pixels);
	CHECK(Run(d, source, "depth").Pixels == Solid(4, 1, {254, 254, 254, 255}).Pixels);
	Set(d.Nodes.back(), "render_mode", EnumValue{1});
	CHECK(Run(d, source).Pixels == Solid(4, 1, {254, 254, 254, 255}).Pixels);
	Set(d.Nodes.back(), "mode", EnumValue{1});
	Set(d.Nodes.back(), "render_mode", EnumValue{0});
	CHECK(Run(d, source).Pixels == Solid(4, 1, {255, 255, 255, 128}).Pixels);
}
TEST_CASE("Smear base color applies mode attenuation and ignores unused side texture", "[smear]") {
	auto d = Graph({{"invert", true}, {"render_mode", EnumValue{2}}});
	const auto source = Solid(2, 1, {128, 128, 128, 255});
	CHECK(Run(d, source).Pixels == Solid(2, 1, {64, 64, 64, 255}).Pixels);
	Set(d.Nodes.back(), "modulate_strength", EnumValue{2});
	CHECK(Run(d, source).Pixels == source.Pixels);
	d.Nodes.push_back({"side", "image.captured", "", {}, {{"source_id", std::string("side")}}});
	d.Links.push_back({"side", "image", "smear", "side_texture"});
	const std::array extras{RequestImageSource{"side", Solid(1, 1, {0, 0, 0, 0})}};
	CHECK(Run(d, source, "colour", extras).Pixels == source.Pixels);
}
TEST_CASE(
	"Smear additive spread preserves floating color and depth remains "
	"unmodified by mix",
	"[smear]"
) {
	auto d = Graph(
		{{"invert", true},
		 {"spread", 1.},
		 {"blend_mode", EnumValue{1}},
		 {"attribute_color_depth", EnumValue{5}}}
	);
	const auto source = Solid(2, 1, {255, 255, 255, 255});
	const auto image = Run(d, source);
	std::array<double, 4> pixel{};
	REQUIRE(LoadSurfacePixel(image, 0, 0, pixel));
	CHECK(pixel == std::array<double, 4>{3, 3, 3, 3});
	Set(d.Nodes.back(), "mix", 0.);
	CHECK(
		Run(d, source, "depth").Pixels ==
		Run(Graph({{"invert", true}, {"attribute_color_depth", EnumValue{5}}}), source, "depth").Pixels
	);
}
TEST_CASE(
	"Smear final texture reads the last qualifying position and missing "
	"positions refuse",
	"[smear]"
) {
	auto d = Graph({{"invert", true}, {"render_mode", EnumValue{3}}});
	d.Nodes.push_back({"texture", "image.captured", "", {}, {{"source_id", std::string("texture")}}});
	d.Links.push_back({"texture", "image", "smear", "texture"});
	const std::array extras{RequestImageSource{"texture", Solid(1, 1, {32, 64, 128, 192})}};
	CHECK(
		Run(d, Solid(2, 1, {255, 255, 255, 255}), "colour", extras).Pixels ==
		Solid(2, 1, {32, 64, 128, 192}).Pixels
	);
	Set(d.Nodes[1], "invert", false);
	auto p = Compiled(d);
	std::array images{RequestImageSource{"source", Solid(2, 1, {255, 255, 255, 255})}, extras[0]};
	EvaluationRequest request;
	request.ImageSources = images;
	Image image;
	Diagnostic diag;
	CHECK(Evaluate(d, p, "colour", request, image, diag) == Status::UnsupportedExecution);
	CHECK(diag.Message.find("base position") != std::string::npos);
}
TEST_CASE("Smear curves and scalar array rows retain source independent modulation", "[smear]") {
	auto d = Graph({{"invert", true}, {"strength_curved", true}, {"strength_curve", Constant(.5)}});
	const auto source = Solid(2, 1, {255, 255, 255, 255});
	CHECK(Run(d, source).Pixels == Solid(2, 1, {128, 128, 128, 255}).Pixels);
	ArrayValue spread;
	spread.ElementType = ValueType::Scalar;
	spread.Elements = {0., 1.};
	Set(d.Nodes.back(), "spread", spread);
	Set(d.Nodes.back(), "blend_mode", EnumValue{1});
	auto p = Compiled(d);
	std::array images{RequestImageSource{"source", source}};
	EvaluationRequest request;
	request.ImageSources = images;
	ImageArray output;
	Diagnostic diag;
	REQUIRE(EvaluateArray(d, p, "colour", request, output, diag) == Status::Ok);
	REQUIRE(output.Images.size() == 2);
	CHECK(output.Images[0].Pixels == Solid(2, 1, {128, 128, 128, 255}).Pixels);
	CHECK(output.Images[1].Pixels == Solid(2, 1, {255, 255, 255, 255}).Pixels);
}
TEST_CASE("Smear undefined resolution and spread curve refuse atomically", "[smear]") {
	auto d = Graph({{"resolution", int64_t{0}}});
	Refused(d, Solid(2, 1, {255, 255, 255, 255}), "colour", "resolution");
	d = Graph({{"spread_curved", true}, {"spread_curve", Constant(1)}});
	Refused(d, Solid(2, 1, {255, 255, 255, 255}), "colour", "divides by zero");
}
TEST_CASE("Smear inactive copy preserves source while prior depth is explicit", "[smear]") {
	auto d = Graph({{"active", false}, {"resolution", int64_t{0}}});
	const auto source = Solid(2, 1, {123, 45, 67, 89});
	CHECK(Run(d, source).Pixels == source.Pixels);
	Refused(d, source, "depth", "prior depth");
}
TEST_CASE(
	"Smear expensive second resolution row refuses before first output "
	"allocation",
	"[smear]"
) {
	ArrayValue resolution;
	resolution.ElementType = ValueType::Integer;
	resolution.Elements = {int64_t{1}, int64_t{1000000}};
	auto d = Graph({{"resolution", resolution}, {"attribute_color_depth", EnumValue{5}}});
	Refused(
		d,
		Solid(96, 96, {255, 255, 255, 255}),
		"colour",
		"whole processor batch",
		Status::LimitExceeded,
		131072
	);
	Set(d.Nodes[1], "resolution", int64_t{1});
	Refused(d, Solid(96, 96, {255, 255, 255, 255}), "colour", "budget", Status::LimitExceeded, 131072);
}
TEST_CASE(
	"Smear authored graph roundtrips and actual downstream consumer sees "
	"its color",
	"[smear]"
) {
	auto d = Graph({{"invert", true}});
	d.Nodes.push_back({"consumer", "image.invert", "", {}, {{"include_alpha", false}}});
	d.Links.push_back({"smear", "surface_out", "consumer", "image"});
	d.Outputs.push_back({"consumer", "consumer", "image"});
	Document copy;
	Diagnostic diag;
	REQUIRE(Read(Write(d), copy, diag) == Status::Ok);
	CHECK(
		Run(copy, Solid(2, 1, {255, 255, 255, 255}), "consumer").Pixels == Solid(2, 1, {0, 0, 0, 255}).Pixels
	);
}
TEST_CASE("Smear point-filtered direction map preserves spatial mapped range", "[smear]") {
	auto d = Graph(
		{{"invert", true},
		 {"render_mode", EnumValue{3}},
		 {"direction_mapped", true},
		 {"direction_map_range", Vector2{0, 180}},
		 {"strength", 1.},
		 {"oversample", EnumValue{3}}}
	);
	// Remove fixture's zero strength so the authored mapped sweep spans the
	// source.
	Set(d.Nodes[1], "strength", 1.);
	d.Nodes.push_back({"map", "image.captured", "", {}, {{"source_id", std::string("map")}}});
	d.Links.push_back({"map", "image", "smear", "direction_map"});
	d.Nodes.push_back({"texture", "image.captured", "", {}, {{"source_id", std::string("texture")}}});
	d.Links.push_back({"texture", "image", "smear", "texture"});
	Image texture{2, 1, {255, 0, 0, 255, 0, 0, 255, 255}, 0};
	Image map{2, 1, {0, 0, 0, 255, 255, 255, 255, 255}, 0};
	std::array extras{RequestImageSource{"texture", texture}, RequestImageSource{"map", map}};
	CHECK(
		Run(d, Solid(2, 1, {255, 255, 255, 255}), "colour", extras).Pixels ==
		std::vector<uint8_t>{0, 0, 255, 255, 255, 0, 0, 255}
	);
}
TEST_CASE("Smear UV sampling scales remap by sweep progress", "[smear]") {
	auto d = Graph({{"invert", true}});
	Image source{2, 1, {0, 0, 0, 255, 255, 255, 255, 255}, 0};
	CHECK(Run(d, source).Pixels == std::vector<uint8_t>{0, 0, 0, 255, 255, 255, 255, 255});
	d.Nodes.push_back({"uv", "image.captured", "", {}, {{"source_id", std::string("uv")}}});
	d.Links.push_back({"uv", "image", "smear", "uv_map"});
	const std::array extras{RequestImageSource{"uv", Solid(1, 1, {191, 128, 0, 255})}};
	CHECK(Run(d, source, "colour", extras).Pixels == Solid(2, 1, {255, 255, 255, 255}).Pixels);
	Set(d.Nodes[1], "uv_mix", 0.);
	CHECK(Run(d, source, "colour", extras).Pixels == Run(Graph({{"invert", true}}), source).Pixels);
}
TEST_CASE("Smear red-only safe draw replaces shader before modifier finish", "[smear]") {
	auto d = Graph({{"attribute_color_depth", EnumValue{3}}, {"mix", .5}, {"channel", int64_t{1}}});
	Image source{2, 1, {64, 128}, 0};
	source.Format = SurfaceFormat::R8Unorm;
	// Only red is selected; channel finish retains the source's other RGBA read
	// channels.
	const auto image = Run(d, source);
	CHECK(image.Pixels == std::vector<uint8_t>{64, 0, 0, 255, 128, 0, 0, 255});
	Refused(d, source, "depth", "does not write depth");
}
TEST_CASE("Smear color mask and channels do not alter depth", "[smear]") {
	auto d = Graph({{"invert", true}, {"channel", int64_t{1}}, {"mix", .5}});
	const auto source = Solid(2, 1, {255, 255, 255, 255});
	d.Nodes.push_back({"mask", "image.captured", "", {}, {{"source_id", std::string("mask")}}});
	d.Links.push_back({"mask", "image", "smear", "mask"});
	const std::array extras{RequestImageSource{"mask", Solid(1, 1, {0, 0, 0, 255})}};
	CHECK(Run(d, source, "depth", extras).Pixels == source.Pixels);
}

TEST_CASE(
	"Smear linked mapped Vector2 is one source range rather than "
	"processor rows",
	"[smear]"
) {
	auto d = Graph(
		{{"invert", true},
		 {"render_mode", EnumValue{3}},
		 {"strength_mapped", true},
		 {"strength_map_range", Vector2{0, 0}},
		 {"oversample", EnumValue{3}}}
	);
	d.Nodes.push_back({"range", "pc.vector2", "", {}, {{"x", 1.}, {"y", 1.}}});
	d.Links.push_back({"range", "vector", "smear", "strength"});
	d.Nodes.push_back({"map", "image.captured", "", {}, {{"source_id", std::string("map")}}});
	d.Links.push_back({"map", "image", "smear", "strength_map"});
	d.Nodes.push_back({"texture", "image.captured", "", {}, {{"source_id", std::string("texture")}}});
	d.Links.push_back({"texture", "image", "smear", "texture"});
	const std::array extras{
		RequestImageSource{"map", Solid(1, 1, {255, 255, 255, 255})},
		RequestImageSource{"texture", Image{2, 1, {255, 0, 0, 255, 0, 0, 255, 255}, 0}}
	};
	CHECK(
		Run(d, Solid(2, 1, {255, 255, 255, 255}), "colour", extras).Pixels ==
		Solid(2, 1, {0, 0, 255, 255}).Pixels
	);
}
TEST_CASE("Smear negative spread skips all sweeps while depth target is defined", "[smear]") {
	auto d = Graph({{"spread", -.5}});
	const auto source = Solid(2, 1, {255, 255, 255, 255});
	CHECK(Run(d, source).Pixels == Solid(2, 1, {0, 0, 0, 0}).Pixels);
	CHECK(Run(d, source, "depth").Pixels == Solid(2, 1, {0, 0, 0, 255}).Pixels);
}
TEST_CASE(
	"Smear blend-side tint and color modulation preserve alpha-squared "
	"brightness",
	"[smear]"
) {
	auto d = Graph({{"invert", true}, {"blend_side", Colour{128, 64, 32, 128}}});
	const auto source = Solid(2, 1, {255, 255, 255, 255});
	CHECK(Run(d, source).Pixels == source.Pixels);
	Set(d.Nodes[1], "invert", false);
	Set(d.Nodes[1], "modulate_strength", EnumValue{1});
	CHECK(Run(d, Solid(2, 1, {255, 255, 255, 128})).Pixels == Solid(2, 1, {32, 32, 32, 128}).Pixels);
}

TEST_CASE(
	"Smear heterogeneous image rows retain every dimension and "
	"independent result",
	"[smear]"
) {
	auto d = Graph({{"invert", true}});
	d.Nodes.push_back({"second", "image.captured", "", {}, {{"source_id", std::string("second")}}});
	Node array{"array", "value.array", "", {}, {{"spread", false}}};
	array.DynamicInputs = {{"a", ValueType::Image, std::nullopt}, {"b", ValueType::Image, std::nullopt}};
	d.Nodes.push_back(array);
	d.Links.erase(d.Links.begin());
	d.Links.push_back({"source", "image", "array", "a"});
	d.Links.push_back({"second", "image", "array", "b"});
	d.Links.push_back({"array", "array", "smear", "surface_in"});
	auto plan = Compiled(d);
	std::array images{
		RequestImageSource{"source", Solid(2, 1, {255, 255, 255, 255})},
		RequestImageSource{"second", Solid(4, 2, {255, 255, 255, 255})}
	};
	EvaluationRequest request;
	request.ImageSources = images;
	ImageArray output;
	Diagnostic diag;
	auto status = EvaluateArray(d, plan, "colour", request, output, diag);
	INFO(diag.Message);
	REQUIRE(status == Status::Ok);
	REQUIRE(output.Images.size() == 2);
	CHECK(output.Images[0] == Run(Graph({{"invert", true}}), images[0].Data));
	CHECK(output.Images[1] == Run(Graph({{"invert", true}}), images[1].Data));
	images[1].Data = Solid(256, 256, {255, 255, 255, 255});
	const auto priorImages = output.Images;
	const auto priorItems = output.Items;
	CHECK(EvaluateArray(d, plan, "colour", request, output, diag) == Status::LimitExceeded);
	CHECK(diag.Message.find("whole processor batch") != std::string::npos);
	CHECK(output.Images == priorImages);
	CHECK(output.Items == priorItems);
}
TEST_CASE("Smear persisted animation seeks restore mapped sweep positions", "[smear]") {
	auto d = Graph({{"invert", true}, {"render_mode", EnumValue{3}}, {"oversample", EnumValue{3}}});
	d.Nodes.push_back({"texture", "image.captured", "", {}, {{"source_id", std::string("texture")}}});
	d.Links.push_back({"texture", "image", "smear", "texture"});
	d.Keyframes = {{"smear", "strength", 0, 0., "linear"}, {"smear", "strength", 10, 1., "linear"}};
	const std::array extras{RequestImageSource{"texture", Image{2, 1, {255, 0, 0, 255, 0, 0, 255, 255}, 0}}};
	const auto source = Solid(2, 1, {255, 255, 255, 255});
	Document copy;
	Diagnostic diag;
	REQUIRE(Read(Write(d), copy, diag) == Status::Ok);
	const auto first = Run(copy, source, "colour", extras, 0);
	CHECK(first.Pixels == extras[0].Data.Pixels);
	CHECK(Run(copy, source, "colour", extras, 10).Pixels == Solid(2, 1, {0, 0, 255, 255}).Pixels);
	CHECK(Run(copy, source, "colour", extras, 0) == first);
}
TEST_CASE("Smear bilinear source keeps map and final texture stages point filtered", "[smear]") {
	auto d = Graph(
		{{"invert", true},
		 {"render_mode", EnumValue{3}},
		 {"interpolate", EnumValue{2}},
		 {"oversample", EnumValue{3}},
		 {"strength", 1.},
		 {"direction_mapped", true},
		 {"direction_map_range", Vector2{0, 180}}}
	);
	d.Nodes.push_back({"map", "image.captured", "", {}, {{"source_id", std::string("map")}}});
	d.Links.push_back({"map", "image", "smear", "direction_map"});
	d.Nodes.push_back({"texture", "image.captured", "", {}, {{"source_id", std::string("texture")}}});
	d.Links.push_back({"texture", "image", "smear", "texture"});
	const std::array extras{
		RequestImageSource{"map", Image{2, 1, {0, 0, 0, 255, 255, 255, 255, 255}, 0}},
		RequestImageSource{"texture", Image{2, 1, {255, 0, 0, 255, 0, 0, 255, 255}, 0}}
	};
	CHECK(
		Run(d, Solid(4, 1, {255, 255, 255, 255}), "colour", extras).Pixels ==
		std::vector<uint8_t>{0, 0, 255, 255, 0, 0, 255, 255, 255, 0, 0, 255, 255, 0, 0, 255}
	);
}

TEST_CASE("Smear sample and feather work share one aggregate ceiling", "[smear]") {
	auto d = Graph({{"invert", true}, {"mask_feather", 60.}});
	d.Nodes.push_back({"mask", "image.captured", "", {}, {{"source_id", std::string("mask")}}});
	d.Links.push_back({"mask", "image", "smear", "mask"});
	const std::array images{
		RequestImageSource{"source", Solid(32, 32, {255, 255, 255, 255})},
		RequestImageSource{"mask", Solid(1024, 512, {255, 255, 255, 255})}
	};
	// 1,554,432 sample units +62,914,560 feather units; each alone fits64million.
	auto p = Compiled(d);
	EvaluationRequest request;
	request.ImageSources = images;
	Image prior = Solid(1, 1, {9, 8, 7, 6}), output = prior;
	Diagnostic diag;
	CHECK(Evaluate(d, p, "colour", request, output, diag) == Status::LimitExceeded);
	CHECK(diag.Port == "mask_feather");
	CHECK(diag.Message.find("aggregate") != std::string::npos);
	CHECK(output == prior);
	Set(d.Nodes[1], "mask_feather", 0.);
	CHECK(Run(d, images[0].Data, "colour", std::span(images).subspan(1)).Pixels == images[0].Data.Pixels);
}
TEST_CASE("Smear finite direction overflow probe", "[smear]") {
	for (int64_t interpolation = 1; interpolation <= 4; ++interpolation) {
		auto d = Graph({{"direction", 1e308}, {"strength", 1.}, {"interpolate", EnumValue{interpolation}}});
		Refused(d, Solid(2, 1, {255, 255, 255, 255}), "colour", "undefined");
	}
}
TEST_CASE("Smear finite UV product overflow probe", "[smear]") {
	auto d = Graph({{"invert", true}, {"uv_mix", 1e308}});
	d.Nodes.push_back({"uv", "image.captured", "", {}, {{"source_id", std::string("uv")}}});
	d.Links.push_back({"uv", "image", "smear", "uv_map"});
	Image uv;
	uv.Width = uv.Height = 1;
	uv.Format = SurfaceFormat::RGBA32Float;
	uv.Pixels.resize(16);
	REQUIRE(StoreSurfacePixel(uv, 0, 0, {3e38, -3e38, 0, 1}));
	const std::array extras{RequestImageSource{"uv", uv}};
	auto p = Compiled(d);
	std::array images{RequestImageSource{"source", Solid(2, 1, {255, 255, 255, 255})}, extras[0]};
	EvaluationRequest request;
	request.ImageSources = images;
	Image prior = Solid(1, 1, {1, 2, 3, 4}), image = prior;
	Diagnostic diag;
	CHECK(Evaluate(d, p, "colour", request, image, diag) == Status::UnsupportedExecution);
	CHECK(image == prior);
}
TEST_CASE("Smear high finite strength keeps final texture interpolation coordinates bounded", "[smear]") {
	for (int64_t interpolation = 1; interpolation <= 4; ++interpolation) {
		auto d = Graph(
			{{"invert", true},
			 {"render_mode", EnumValue{3}},
			 {"strength", 1e308},
			 {"interpolate", EnumValue{interpolation}}}
		);
		d.Nodes.push_back({"texture", "image.captured", "", {}, {{"source_id", std::string("texture")}}});
		d.Links.push_back({"texture", "image", "smear", "texture"});
		const std::array extras{RequestImageSource{"texture", Solid(3, 2, {32, 64, 128, 192})}};
		CHECK(
			Run(d, Solid(2, 1, {255, 255, 255, 255}), "colour", extras).Pixels ==
			Solid(2, 1, {32, 64, 128, 192}).Pixels
		);
	}
}
