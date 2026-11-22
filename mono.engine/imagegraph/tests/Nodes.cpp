#include <engine/core/Float16.hpp>
#include <engine/imagegraph/Content.hpp>
#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/Schema.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <set>

TEST_SUITE_ID("engine.imagegraph.nodes")
TEST_DEPENDS("engine.imagegraph.evaluate")

using namespace engine::imagegraph;

namespace {
	using Rgba = std::array<uint8_t, 4>;
	Image Run(Document document, std::string_view output = {}, const MeshResolver &meshes = {}) {
		Plan plan;
		Diagnostic diagnostic;
		REQUIRE(Compile(document, plan, diagnostic));
		Image image;
		const bool ok = EvaluateWith(document, plan, output, {{}, meshes}, image, diagnostic);
		INFO(diagnostic.Node << ": " << diagnostic.Message);
		REQUIRE(ok);
		return image;
	}
	Rgba At(const Image &image, uint32_t x, uint32_t y) {
		const size_t offset = (size_t(y) * image.Width + x) * 4;
		return {
			std::to_integer<uint8_t>(image.Pixels[offset]),
			std::to_integer<uint8_t>(image.Pixels[offset + 1]),
			std::to_integer<uint8_t>(image.Pixels[offset + 2]),
			std::to_integer<uint8_t>(image.Pixels[offset + 3])
		};
	}
	Document One(Operation operation, std::vector<Node> upstream = {}, std::vector<std::string> inputs = {}) {
		Document document;
		document.Nodes = std::move(upstream);
		document.Nodes.push_back({"result", std::move(operation), std::move(inputs), {}});
		document.Outputs = {{"main", "result"}};
		return document;
	}
	Node SolidNode(std::string id, Rgba colour, uint32_t size = 4) {
		return {std::move(id), Solid{size, size, colour}, {}, {}};
	}
}

TEST_CASE("generators are deterministic per seed and stay within their range", "[imagegraph][nodes]") {
	const std::array<Operation, 4> generators{
		Noise{32, 32, NoiseType::Gradient, 6, 5, 2, 0.5, 7},
		Voronoi{32, 32, 5, 1, VoronoiFeature::F2MinusF1, 7},
		Wave{32, 32, WaveType::Rings, WaveProfile::Sine, 3, 0, 0, 0.4, 3, 3, 7},
		Noise{32, 32, NoiseType::Value, 4, 3, 2, 0.6, 7},
	};
	for (const auto &generator : generators) {
		const Image first = Run(One(generator));
		CHECK(Run(One(generator)).Pixels == first.Pixels);
		Operation reseeded = generator;
		REQUIRE(SetProperty(reseeded, "seed", 8.0));
		CHECK(Run(One(reseeded)).Pixels != first.Pixels);
		std::set<uint8_t> values;
		for (uint32_t y = 0; y < 32; ++y)
			for (uint32_t x = 0; x < 32; ++x) {
				const auto pixel = At(first, x, y);
				CHECK(pixel[0] == pixel[1]);
				CHECK(pixel[3] == 255);
				values.insert(pixel[0]);
			}
		CHECK(values.size() > 8);
	}
}

TEST_CASE("voronoi features order and gradients reach their endpoints", "[imagegraph][nodes]") {
	const auto feature = [](VoronoiFeature which) { return Run(One(Voronoi{16, 16, 4, 1, which, 3})); };
	const Image f1 = feature(VoronoiFeature::F1), f2 = feature(VoronoiFeature::F2),
				edge = feature(VoronoiFeature::Edge), cell = feature(VoronoiFeature::Cell);
	for (uint32_t y = 0; y < 16; ++y)
		for (uint32_t x = 0; x < 16; ++x) {
			CHECK(At(f1, x, y)[0] <= At(f2, x, y)[0]);
			CHECK(At(edge, x, y)[0] <= At(f2, x, y)[0]);
		}
	// A cell value is constant across its cell, so neighbours mostly agree.
	size_t same = 0;
	for (uint32_t x = 1; x < 16; ++x)
		same += At(cell, x, 0)[0] == At(cell, x - 1, 0)[0];
	CHECK(same > 8);

	const Image linear = Run(One(Gradient{8, 1, GradientType::Linear, 0, 0, 1, 0}));
	CHECK(At(linear, 0, 0)[0] < 20);
	CHECK(At(linear, 7, 0)[0] > 235);
	for (uint32_t x = 1; x < 8; ++x)
		CHECK(At(linear, x, 0)[0] > At(linear, x - 1, 0)[0]);
	const Image radial = Run(One(Gradient{9, 9, GradientType::Radial, 0.5, 0.5, 1, 0.5}));
	CHECK(At(radial, 4, 4)[0] == 0);
	CHECK(At(radial, 0, 4)[0] > 200);
}

TEST_CASE("a vector input drives generator positions", "[imagegraph][nodes]") {
	// Every pixel reads the same position, so the whole pattern is one value.
	const Image flat =
		Run(One(Voronoi{8, 8, 6, 1, VoronoiFeature::F1, 1}, {SolidNode("uv", {77, 140, 0, 255}, 2)}, {"uv"}));
	for (uint32_t y = 0; y < 8; ++y)
		for (uint32_t x = 0; x < 8; ++x)
			CHECK(At(flat, x, y) == At(flat, 0, 0));
}

TEST_CASE("ramp levels math invert and channels map stored values", "[imagegraph][nodes]") {
	const Node grey = SolidNode("grey", {128, 64, 255, 255});
	Ramp ramp;
	ramp.Factor = Channel::Red;
	ramp.Stops = {{0, {0, 0, 0, 255}}, {1, {255, 0, 100, 255}}};
	CHECK(At(Run(One(ramp, {grey}, {"grey"})), 0, 0) == Rgba{128, 0, 50, 255});
	ramp.Interpolation = RampInterpolation::Constant;
	CHECK(At(Run(One(ramp, {grey}, {"grey"})), 0, 0) == Rgba{0, 0, 0, 255});

	Levels levels;
	levels.InputMaximum = 0.5;
	levels.Channels = 1;
	CHECK(At(Run(One(levels, {grey}, {"grey"})), 0, 0) == Rgba{255, 64, 255, 255});
	levels.Clamp = false;
	levels.OutputMaximum = 0.25;
	CHECK(At(Run(One(levels, {grey}, {"grey"})), 0, 0) == Rgba{64, 64, 255, 255});

	Math math;
	math.Operation = MathOperation::Multiply;
	math.Value = {0.5, 0.5, 0.5, 1};
	CHECK(At(Run(One(math, {grey}, {"grey"})), 0, 0) == Rgba{64, 32, 128, 255});
	math.Operation = MathOperation::Divide;
	math.Value = {0, 1, 1, 1};
	CHECK(At(Run(One(math, {grey}, {"grey"})), 0, 0) == Rgba{0, 64, 255, 255});
	math.Operation = MathOperation::Add;
	math.Channels = 2;
	CHECK(
		At(Run(One(math, {grey, SolidNode("other", {10, 10, 10, 10})}, {"grey", "other"})), 0, 0) ==
		Rgba{128, 74, 255, 255}
	);

	CHECK(At(Run(One(Invert{}, {grey}, {"grey"})), 0, 0) == Rgba{127, 191, 0, 255});

	Channels merge;
	merge.Picks = {{{1, 0}, {0, 2}, {-1, 0}, {-2, 0}}};
	CHECK(
		At(Run(One(merge, {grey, SolidNode("red", {9, 0, 0, 0})}, {"grey", "red"})), 0, 0) ==
		Rgba{9, 255, 0, 255}
	);
	merge.Picks[0].Input = 2;
	Document unconnected = One(merge, {grey}, {"grey"});
	Plan plan;
	Diagnostic diagnostic;
	CHECK_FALSE(Compile(unconnected, plan, diagnostic));
}

TEST_CASE("blend modes composite with straight alpha and an optional mask", "[imagegraph][nodes]") {
	const Node back = SolidNode("back", {200, 100, 50, 255}),
			   front = SolidNode("front", {128, 128, 255, 255});
	const auto blend = [&](BlendMode mode, double opacity = 1) {
		return At(Run(One(Blend{opacity, mode}, {back, front}, {"back", "front"})), 0, 0);
	};
	CHECK(blend(BlendMode::Normal) == Rgba{128, 128, 255, 255});
	CHECK(blend(BlendMode::Multiply) == Rgba{100, 50, 50, 255});
	CHECK(blend(BlendMode::Screen) == Rgba{228, 178, 255, 255});
	CHECK(blend(BlendMode::Add) == Rgba{255, 228, 255, 255});
	CHECK(blend(BlendMode::Subtract) == Rgba{72, 0, 0, 255});
	CHECK(blend(BlendMode::Difference) == Rgba{72, 28, 205, 255});
	CHECK(blend(BlendMode::Multiply, 0.5) == Rgba{150, 75, 50, 255});
	const Rgba overlay = blend(BlendMode::Overlay), soft = blend(BlendMode::SoftLight);
	CHECK(overlay == Rgba{200, 100, 100, 255});
	CHECK(soft[2] > 50);

	// A black mask hides the foreground; a white one shows it.
	CHECK(
		At(Run(
			   One(Blend{1, BlendMode::Multiply},
				   {back, front, SolidNode("mask", {0, 0, 0, 255})},
				   {"back", "front", "mask"})
		   ),
		   0,
		   0) == Rgba{200, 100, 50, 255}
	);
	CHECK(
		At(Run(
			   One(Blend{1, BlendMode::Normal},
				   {back, front, SolidNode("mask", {255, 0, 0, 255})},
				   {"back", "front", "mask"})
		   ),
		   0,
		   0) == Rgba{128, 128, 255, 255}
	);
}

TEST_CASE("blur normal and warp filters", "[imagegraph][nodes]") {
	// A centred dot spreads symmetrically and keeps a constant image constant.
	Document dot =
		One(Blur{3},
			{SolidNode("black", {0, 0, 0, 255}, 9),
			 SolidNode("white", {255, 255, 255, 255}, 1),
			 {"placed", Crop{-4, -4, 9, 9}, {"white"}, {}},
			 {"over", Blend{}, {"black", "placed"}, {}}},
			{"over"});
	const Image blurred = Run(dot);
	CHECK(At(blurred, 4, 4)[0] < 255);
	CHECK(At(blurred, 3, 4)[0] > 0);
	CHECK(At(blurred, 3, 4) == At(blurred, 5, 4));
	CHECK(At(blurred, 4, 3) == At(blurred, 4, 5));
	CHECK(
		At(Run(One(Blur{5}, {SolidNode("flat", {40, 80, 120, 255})}, {"flat"})), 2, 2) ==
		Rgba{40, 80, 120, 255}
	);

	// A flat height is a straight-up normal; a horizontal ramp tilts it.
	CHECK(
		At(Run(One(NormalMap{}, {SolidNode("flat", {90, 0, 0, 255})}, {"flat"})), 1, 1) ==
		Rgba{128, 128, 255, 255}
	);
	const Image tilted = Run(
		One(NormalMap{8}, {{"ramp", Gradient{8, 8, GradientType::Linear, 0, 0, 1, 0}, {}, {}}}, {"ramp"})
	);
	CHECK(At(tilted, 4, 4)[0] < 128);
	CHECK(At(tilted, 4, 4)[1] == 128);

	// A neutral vector is identity; a full-scale one shifts by Strength pixels.
	const Node source{"ramp", Gradient{8, 8, GradientType::Linear, 0, 0, 1, 0}, {}, {}};
	const Image ramp = Run(One(Flip{}, {source}, {"ramp"}));
	CHECK(
		Run(One(Warp{2, Sampling::Nearest},
				{source, SolidNode("neutral", {128, 128, 0, 255}, 8)},
				{"ramp", "neutral"}))
			.Pixels == ramp.Pixels
	);
	const Image shifted = Run(One(
		Warp{2, Sampling::Nearest}, {source, SolidNode("right", {255, 128, 0, 255}, 8)}, {"ramp", "right"}
	));
	CHECK(At(shifted, 1, 0) == At(ramp, 3, 0));
}

TEST_CASE("mesh attributes rasterize into a UDIM tile with padding", "[imagegraph][nodes]") {
	// Two triangles covering u 1.25..1.75 of tile 1001+1; attribute x ramps with u.
	MeshAttributeData quad;
	quad.UVs = {{1.25f, 0.25f}, {1.75f, 0.25f}, {1.75f, 0.75f}, {1.25f, 0.75f}};
	quad.Values = {{0, 0.5f, 0, 0}, {1, 0.5f, 0, 0}, {1, 0.5f, 0, 0}, {0, 0.5f, 0, 0}};
	quad.Indices = {0, 1, 2, 0, 2, 3};
	MeshAttribute node;
	node.Width = node.Height = 8;
	node.Mesh = "editable-mesh://5";
	node.Components = "xy";
	node.TileU = 1;
	node.Padding = 0;
	const auto meshes = [&](const MeshAttribute &asked, MeshAttributeData &out, std::string &) {
		CHECK(asked.Mesh == "editable-mesh://5");
		out = quad;
		return true;
	};
	const Image tile = Run(One(node), {}, meshes);
	CHECK(At(tile, 0, 0)[3] == 0);
	CHECK(At(tile, 2, 2)[3] == 255);
	CHECK(At(tile, 2, 2)[1] == 128);
	CHECK(At(tile, 2, 2)[0] < At(tile, 5, 2)[0]);
	node.Padding = 2;
	const Image padded = Run(One(node), {}, meshes);
	CHECK(At(padded, 0, 2)[3] == 255);
	CHECK(At(padded, 0, 2)[0] == At(padded, 2, 2)[0]);
	node.TileU = 0;
	CHECK(At(Run(One(node), {}, meshes), 4, 4)[3] == 0);

	Document missing = One(node);
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(missing, plan, diagnostic));
	Image image;
	CHECK_FALSE(EvaluateWith(missing, plan, {}, {}, image, diagnostic));
	CHECK(diagnostic.Node == "result");
}

TEST_CASE("wide precision keeps falloffs that eight-bit intermediates band", "[imagegraph][nodes]") {
	// A shallow ramp squeezed into 1/16 of the range and stretched back.
	Document document =
		One(Levels{0, 1, 0, 16, 1, true, 7},
			{{"ramp", Gradient{256, 1, GradientType::Linear, 0, 0, 1, 0}, {}, {}},
			 {"squeeze", Levels{0, 1, 0, 1.0 / 16, 1, true, 7}, {"ramp"}, {}}},
			{"squeeze"});
	document.Outputs[0].Format = OutputFormat::R16F;
	const auto distinct = [](const Image &image) {
		std::set<uint16_t> values;
		for (size_t pixel = 0; pixel < 256; ++pixel)
			values.insert(
				uint16_t(std::to_integer<uint8_t>(image.Pixels[pixel * 2])) |
				uint16_t(std::to_integer<uint8_t>(image.Pixels[pixel * 2 + 1])) << 8
			);
		return values.size();
	};
	const Image narrow = Run(document);
	CHECK(narrow.Format == OutputFormat::R16F);
	CHECK(narrow.Pixels.size() == 256 * 2);
	document.Storage = Precision::Wide;
	const Image wide = Run(document);
	CHECK(distinct(narrow) <= 17);
	CHECK(distinct(wide) > 128);
	document.Outputs[0].Format = OutputFormat::RGBA16F;
	const Image full = Run(document);
	CHECK(full.Pixels.size() == 256 * 8);
	CHECK(
		engine::core::DecodeFloat16(
			uint16_t(std::to_integer<uint8_t>(full.Pixels[255 * 8 + 6])) |
			uint16_t(std::to_integer<uint8_t>(full.Pixels[255 * 8 + 7])) << 8
		) == 1.0f
	);
}

TEST_CASE("new kinds round trip through version 3 json and property access", "[imagegraph][nodes]") {
	Ramp ramp;
	ramp.Stops = {{0, {1, 2, 3, 4}}, {0.5, {5, 6, 7, 8}}};
	Channels channels;
	channels.Picks = {{{0, 3}, {-1, 0}, {-2, 0}, {0, 0}}};
	MeshAttribute mesh;
	mesh.Mesh = "editable-mesh://9";
	mesh.Components = "zw";
	Document document{
		.Nodes =
			{{"noise", Noise{}, {}, {1, 2}},
			 {"cells", Voronoi{}, {"noise"}, {}},
			 {"gradient", Gradient{}, {}, {}},
			 {"wave", Wave{}, {}, {}},
			 {"ramp", ramp, {"cells"}, {}},
			 {"levels", Levels{}, {"ramp"}, {}},
			 {"math", Math{MathOperation::Power, {2, 2, 2, 1}, 7}, {"levels", "wave"}, {}},
			 {"channels", channels, {"math"}, {}},
			 {"invert", Invert{5}, {"channels"}, {}},
			 {"blur", Blur{1.5}, {"invert"}, {}},
			 {"normal", NormalMap{2, Channel::Green, true}, {"blur"}, {}},
			 {"warp", Warp{}, {"normal", "gradient"}, {}},
			 {"mesh", mesh, {}, {}},
			 {"blend", Blend{0.5, BlendMode::SoftLight}, {"warp", "gradient", "mesh"}, {}}},
		.Outputs = {{"image", "blend", OutputSpace::Linear, OutputFormat::RGBA16F}},
		.Storage = Precision::Wide
	};
	std::string encoded;
	Diagnostic diagnostic;
	REQUIRE(Write(document, encoded, diagnostic));
	CHECK(encoded.find("\"version\": 3") != std::string::npos);
	CHECK(encoded.find("\"precision\": \"wide\"") != std::string::npos);
	Document read;
	REQUIRE(Read(encoded, read, diagnostic));
	std::string again;
	REQUIRE(Write(read, again, diagnostic));
	CHECK(again == encoded);
	CHECK(std::get<Ramp>(read.Nodes[4].Value).Stops == ramp.Stops);
	CHECK(std::get<Channels>(read.Nodes[7].Value).Picks == channels.Picks);
	CHECK(std::get<Blend>(read.Nodes[13].Value).Mode == BlendMode::SoftLight);

	// Unknown and missing properties are refused like every other kind.
	std::string extra = encoded;
	extra.replace(extra.find("\"radius\""), 8, "\"radiusx\"");
	CHECK_FALSE(Read(extra, read, diagnostic));

	// A plain normal blend without precision keeps the version 1 encoding.
	Document plain{
		.Nodes = {{"a", Solid{}, {}, {}}, {"b", Blend{}, {"a", "a"}, {}}}, .Outputs = {{"o", "b"}}
	};
	REQUIRE(Write(plain, encoded, diagnostic));
	CHECK(encoded.find("\"version\": 1") != std::string::npos);
	CHECK(encoded.find("mode") == std::string::npos);

	InputValue value;
	Operation operation = ramp;
	REQUIRE(GetProperty(operation, "stops", value));
	CHECK(std::get<std::string>(value) == "0:01020304 0.5:05060708");
	REQUIRE(SetProperty(operation, "stops", std::string("0:000000ff 1:ffffffff")));
	CHECK(std::get<Ramp>(operation).Stops.size() == 2);
	CHECK_FALSE(SetProperty(operation, "stops", std::string("nonsense")));
	CHECK_FALSE(SetProperty(operation, "factor", 1.0));
	CHECK(PropertyType(operation, "factor") == 3);
	CHECK(PropertyType(Operation{Source{}}, "interpretation") == -1);
	for (const auto kind : NODE_KIND_IDS) {
		Operation fresh;
		REQUIRE(DefaultOperation(kind, fresh));
		CHECK(Kind(fresh) == kind);
		for (const auto &spec : DescribeKind(kind)) {
			InputValue current;
			REQUIRE(GetProperty(fresh, spec.Key, current));
			CHECK(SetProperty(fresh, spec.Key, current));
		}
	}
}

TEST_CASE("runtime documents admit local references that signed content refuses", "[imagegraph][nodes]") {
	Document document{
		.Nodes =
			{{"image", Source{"editable-image://3", SourceInterpretation::Data}, {}, {}},
			 {"uv", MeshAttribute{64, 64, "editable-mesh://4", 1, "xy"}, {}, {}},
			 {"cells", Voronoi{64, 64}, {"uv"}, {}},
			 {"tinted", Blend{1, BlendMode::Multiply}, {"image", "cells"}, {}}},
		.Outputs = {{"albedo", "tinted"}},
		.Parameters = {},
		.Bindings = {}
	};
	std::string encoded;
	Diagnostic diagnostic;
	REQUIRE(Write(document, encoded, diagnostic));
	Document admitted;
	REQUIRE(AdmitRuntimeDocument(encoded, admitted, diagnostic));
	CHECK(admitted.Nodes.size() == 4);
	Content content;
	CHECK_FALSE(content.Admit("graph.aimagegraph", "root", encoded, diagnostic));

	document.Nodes[1].Value = MeshAttribute{64, 64, "mesh.obj"};
	REQUIRE(Write(document, encoded, diagnostic));
	CHECK_FALSE(AdmitRuntimeDocument(encoded, admitted, diagnostic));
	CHECK(diagnostic.Node == "uv");
	CHECK_FALSE(AdmitRuntimeDocument("{\"version\": 2}", admitted, diagnostic));
}
