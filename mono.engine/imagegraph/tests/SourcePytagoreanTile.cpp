#include "ComplexGeneratorFixture.hpp"
#include "NodeHarness.hpp"

#include <engine/imagegraph/Surface.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.source_pytagorean_tile")
using namespace complex_generator_test;
using namespace engine::imagegraph;
using imagegraph_test::NodeRun;

namespace {
	Image FloatImage(uint32_t width, uint32_t height, std::span<const SurfacePixel> pixels) {
		Image image{
			width, height, std::vector<uint8_t>(size_t(width) * height * 16), 0, SurfaceFormat::RGBA32Float
		};
		for (uint32_t y = 0; y < height; ++y)
			for (uint32_t x = 0; x < width; ++x)
				REQUIRE(StoreSurfacePixel(image, x, y, pixels[size_t(y) * width + x]));
		return image;
	}
	SurfacePixel Pixel(const Image &image, uint32_t x = 0, uint32_t y = 0) {
		SurfacePixel pixel{};
		REQUIRE(LoadSurfacePixel(image, x, y, pixel));
		return pixel;
	}
	Image UvImage() {
		const std::array<SurfacePixel, 1> pixel{{{.23, .38, 0, .6}}};
		return FloatImage(1, 1, pixel);
	}
	Image TextureImage() {
		const std::array<SurfacePixel, 4> pixels{
			{{.1, .2, .3, .4}, {.8, .1, .2, .5}, {.2, .9, .4, .7}, {.6, .5, .9, .8}}
		};
		return FloatImage(2, 2, pixels);
	}
	NodeRun TileRun(
		std::initializer_list<std::pair<std::string_view, Value>> overrides = {},
		std::initializer_list<std::pair<std::string_view, const Image *>> extraImages = {},
		bool useUv = true
	) {
		const Image uv = UvImage();
		const Image texture = TextureImage();
		std::vector<std::pair<std::string_view, Value>> values{
			{"dimension", Vector2{8, 6}},
			{"dimension_unit", EnumValue{0}},
			{"position", Vector2{.25, -.125}},
			{"position_unit", EnumValue{0}},
			{"scale", Vector2{2, 2}},
			{"scale_unit", EnumValue{0}},
			{"rotation", 0.0},
			{"phase", 90.0},
			{"seed", 17.25},
			{"render_type", EnumValue{0}},
			{"gap", .25},
			{"tile_color", Gradient{0, {{0, Colour{20, 80, 200, 64}}, {1, Colour{230, 160, 40, 192}}}}},
			{"shift", .2},
			{"gap_color", Colour{13, 26, 38, 204}},
			{"level_in", Vector2{0, 1}},
			{"level_out", Vector2{0, 1}},
			{"truchet", false},
			{"texture_seed", int64_t{41}},
			{"random_position", Vector4{0, 0, 0, 0}},
			{"random_angle", Vector2{0, 0}},
			{"random_scale", Vector4{1, 1, 1, 1}},
			{"flip_threshold", .5},
			{"anti_aliasing", false},
			{"uv_mix", 1.0},
			{"attribute_color_depth", EnumValue{5}},
			{"oversample", EnumValue{0}}
		};
		for (const auto &[port, value] : overrides) {
			auto found = std::find_if(values.begin(), values.end(), [&](const auto &entry) {
				return entry.first == port;
			});
			if (found == values.end())
				values.emplace_back(port, value);
			else
				found->second = value;
		}
		std::vector<std::pair<std::string_view, const Image *>> images;
		if (useUv) images.emplace_back("uv_map", &uv);
		images.emplace_back("texture", &texture);
		images.insert(images.end(), extraImages.begin(), extraImages.end());
		const CatalogueEntry *entry = FindCatalogueEntry("pc.pytagorean_tile");
		const auto executor = detail::FindExecutor("pc.pytagorean_tile");
		REQUIRE(entry);
		REQUIRE(executor);
		Node node{"node", "pc.pytagorean_tile", "", {}, {}};
		EvaluationRequest request;
		detail::NodeContext context(node, *entry, request);
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		for (const auto &[port, image] : images)
			context.Images.emplace_back(port, image);
		for (const CatalogueInput &input : entry->Inputs) {
			const auto given = std::find_if(values.begin(), values.end(), [&](const auto &value) {
				return value.first == input.Id;
			});
			if (given != values.end())
				context.Values.emplace_back(input.Id, given->second);
			else if (const auto fallback = CatalogueDefault(input)) {
				context.Values.emplace_back(input.Id, *fallback);
				context.CatalogueDefaultInputs.emplace_back(input.Id);
			}
		}
		for (const auto &[port, value] : context.Values)
			context.ValueViews.emplace_back(port, &value);
		context.InputProvenanceResolved = true;
		NodeRun run;
		const bool ok = executor(context);
		run.Ok = ok && context.FailureCode == Status::Ok;
		run.Code = context.FailureCode;
		run.Port = context.FailurePort;
		run.Message = context.FailureMessage;
		run.Images = std::move(context.OutputImages);
		run.Values = std::move(context.OutputValues);
		return run;
	}
	void CheckGolden(const NodeRun &run, SurfacePixel expected, uint32_t x = 0, uint32_t y = 0) {
		INFO(run.Port << ": " << run.Message);
		REQUIRE(run.Ok);
		const auto actual = Pixel(run.Output(), x, y);
		for (size_t channel = 0; channel < 4; ++channel)
			CHECK(actual[channel] == Catch::Approx(expected[channel]).margin(5e-5));
	}
	Document Pytagorean() {
		auto document = Graph("pc.pytagorean_tile", {8, 8});
		Set(document, "position", Vector2{.5, .5});
		Set(document, "position_unit", EnumValue{0});
		Set(document, "scale", Vector2{2, 2});
		Set(document, "scale_unit", EnumValue{0});
		Set(document, "phase", 90.0);
		Set(document, "gap", .2);
		Set(document, "gap_color", Colour{12, 34, 56, 255});
		Set(document,
			"tile_color",
			Gradient{0, {{0, Colour{220, 30, 40, 255}}, {1, Colour{20, 80, 230, 255}}}});
		Set(document, "render_type", EnumValue{0});
		Set(document, "anti_aliasing", false);
		Set(document, "level_in", Vector2{0, 1});
		Set(document, "level_out", Vector2{0, 1});
		return document;
	}
	void Erase(Document &document, std::string_view port) {
		std::erase_if(document.Nodes[0].Values, [&](const auto &entry) { return entry.Port == port; });
	}
	void Same(const Image &left, const Image &right) {
		CHECK(left.Width == right.Width);
		CHECK(left.Height == right.Height);
		CHECK(left.Format == right.Format);
		CHECK(left.Pixels == right.Pixels);
	}
	void WhiteMap(Document &document, std::string_view port) {
		document.Nodes.push_back(Solid("map", {1, 1}, {255, 255, 255, 255}));
		document.Links.push_back({"map", "surface_out", "generator", std::string(port)});
	}
	ImageArray DrawRows(const Document &document) {
		Plan plan;
		Diagnostic diagnostic;
		REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
		ImageArray rows;
		const auto status = EvaluateArray(document, plan, "out", {}, rows, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << ": " << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		return rows;
	}
}

TEST_CASE(
	"Pytagorean Tile colored height and texture modes have distinct outputs", "[source_pytagorean_tile]"
) {
	auto colored = Pytagorean();
	auto height = colored;
	auto texture = colored;
	Set(height, "render_type", EnumValue{1});
	Set(texture, "render_type", EnumValue{2});
	Set(height, "level_out", Vector2{.1, .9});

	const auto colorImage = Draw(colored);
	const auto heightImage = Draw(height);
	const auto textureImage = Draw(texture);
	CHECK(colorImage.Width == 8);
	CHECK(colorImage.Height == 8);
	CHECK(colorImage.Pixels != heightImage.Pixels);
	CHECK(colorImage.Pixels != textureImage.Pixels);
	CHECK(heightImage.Pixels != textureImage.Pixels);
}

TEST_CASE("Pytagorean Tile height mode maps cell distance through levels", "[source_pytagorean_tile]") {
	auto document = Pytagorean();
	Set(document, "render_type", EnumValue{1});
	Set(document, "level_out", Vector2{.2, .8});
	const auto mapped = Draw(document);
	Set(document, "level_out", Vector2{0, 1});
	const auto identity = Draw(document);
	CHECK(mapped.Pixels != identity.Pixels);
}

TEST_CASE("Pytagorean Tile matches independent binary32 shader samples", "[source_pytagorean_tile]") {
	constexpr std::array<SurfacePixel, 7> gradientExpected{
		{{.6810394526, .5432904959, .3251838088, .3709706068},
		 {.0784313753, .3137255013, .7843137383, .1505882442},
		 {.8704006076, .1341156662, .2547142208, .3709706068},
		 {.6842892170, .5729525089, .4467836618, .3709706068},
		 {.7831891775, .5637378693, .4464450479, .3709706068},
		 {.5421820879, .8704006076, .1341156662, .3709706068},
		 {.6602549553, .5364627242, .3442612886, .3709706068}}
	};
	for (int64_t mode = 0; mode < int64_t(gradientExpected.size()); ++mode) {
		CheckGolden(
			TileRun(
				{{"tile_color",
				  Gradient{uint8_t(mode), {{0, Colour{20, 80, 200, 64}}, {1, Colour{230, 160, 40, 192}}}}}}
			),
			gradientExpected[size_t(mode)]
		);
	}
	CheckGolden(
		TileRun({{"render_type", EnumValue{1}}}), {.0440251492, .0440251492, .0440251492, .6000000238}
	);
	CheckGolden(
		TileRun(
			{{"render_type", EnumValue{1}}, {"level_in", Vector2{-.2, .5}}, {"level_out", Vector2{.8, -.3}}}
		),
		{.4165318906, .4165318906, .4165318906, .6000000238}
	);
	CheckGolden(TileRun({{"render_type", EnumValue{2}}}), {.6, .5, .9, .4800000191});
	CheckGolden(
		TileRun(
			{{"render_type", EnumValue{2}},
			 {"truchet", true},
			 {"random_position", Vector4{-.1, -.2, .3, .4}},
			 {"random_angle", Vector2{-35, 55}},
			 {"random_scale", Vector4{.7, 1.3, .5, 1.2}}}
		),
		{.2, .9, .4, .4200000167}
	);
	CheckGolden(TileRun({{"seed", -17.25}}), {.5815162063, .5053768754, .4010109901, .3345735669});
	CheckGolden(TileRun({{"scale", Vector2{-2, 2}}}), {.4399721026, .4514552951, .5088541508, .2828088701});
	CheckGolden(TileRun({{"phase", 225.0}}), {.0509803928, .1019607857, .1490196139, .4800000191});
	CheckGolden(TileRun({{"phase", -90.0}}), {.1044079736, .3236213326, .7645220160, .1600882560});
	CheckGolden(TileRun({{"rotation", 37.0}}), {.4399721026, .4514552951, .5088541508, .2828088701});
	CheckGolden(TileRun({{"anti_aliasing", true}}), {.6810394526, .5432904959, .3251838088, .3709706068});
	CheckGolden(TileRun({{"gap", -.3}}), {.6810394526, .5432904959, .3251838088, .3709706068});
	CheckGolden(TileRun({{"gap", 1.4}}), {.0509803928, .1019607857, .1490196139, .4800000191});
	CheckGolden(TileRun({{"uv_mix", 0.0}}), {.5978018045, .5115809441, .3886029124, .3405294418});
	const auto fractional = TileRun(
		{{"dimension", Vector2{3.6, 2.6}}, {"position", Vector2{.125, .25}}, {"scale", Vector2{1.2, .8}}},
		{},
		false
	);
	REQUIRE(fractional.Ok);
	CHECK(fractional.Output().Width == 4);
	CHECK(fractional.Output().Height == 3);
	CheckGolden(fractional, {.5302466750, .4858456254, .4400734901, .5263726115}, 1, 1);

	constexpr std::array<SurfacePixel, 13> outsideExpected{
		{{.2, .9, .4, .4200000167},
		 {0, 0, 0, 0},
		 {0, 0, 0, .6000000238},
		 {.1, .2, .3, .2400000095},
		 {.2, .9, .4, .4200000167},
		 {0, 0, 0, 0},
		 {0, 0, 0, 0},
		 {0, 0, 0, .6000000238},
		 {.1, .2, .3, .2400000095},
		 {0, 0, 0, 0},
		 {0, 0, 0, 0},
		 {0, 0, 0, .6000000238},
		 {.2, .9, .4, .4200000167}}
	};
	for (int64_t sampler = 0; sampler < int64_t(outsideExpected.size()); ++sampler) {
		CheckGolden(
			TileRun(
				{{"render_type", EnumValue{2}},
				 {"truchet", true},
				 {"random_position", Vector4{2, 2, 2, 2}},
				 {"oversample", EnumValue{sampler}}}
			),
			outsideExpected[size_t(sampler)]
		);
	}
	for (const int64_t sampler : {6, 7}) {
		CheckGolden(
			TileRun(
				{{"render_type", EnumValue{2}},
				 {"truchet", true},
				 {"random_position", Vector4{2, 0, 2, 0}},
				 {"oversample", EnumValue{sampler}}}
			),
			{.2, .9, .4, .4200000167}
		);
	}
	for (const int64_t sampler : {10, 11}) {
		CheckGolden(
			TileRun(
				{{"render_type", EnumValue{2}},
				 {"truchet", true},
				 {"random_position", Vector4{0, 2, 0, 2}},
				 {"oversample", EnumValue{sampler}}}
			),
			{.2, .9, .4, .4200000167}
		);
	}
	const Image gradientMap = TextureImage();
	CheckGolden(
		TileRun(
			{{"tile_color_mapped", true}, {"tile_color_map_range", Vector4{.1, .2, .9, .8}}},
			{{"tile_color_map", &gradientMap}}
		),
		{.5840926766, .4543192685, .6972561479, .4323024154}
	);
	CheckGolden(
		TileRun(
			{{"tile_color_mapped", true}, {"tile_color_map_range", Vector4{-2, -1, -2, -1}}},
			{{"tile_color_map", &gradientMap}}
		),
		{.1, .2, .3, .2400000095}
	);
}

TEST_CASE("Pytagorean Tile evaluates each gradient blend mode", "[source_pytagorean_tile]") {
	auto document = Pytagorean();
	Set(document, "tile_color", Gradient{0, {{0, Colour{220, 30, 40, 255}}, {1, Colour{20, 80, 230, 255}}}});
	Set(document, "attribute_color_depth", EnumValue{5});
	Image first;
	for (int mode = 0; mode < 7; ++mode) {
		Set(document,
			"tile_color",
			Gradient{uint8_t(mode), {{0, Colour{220, 30, 40, 255}}, {1, Colour{20, 80, 230, 255}}}});
		auto image = Draw(document);
		if (mode == 0)
			first = std::move(image);
		else
			CHECK(image.Pixels != first.Pixels);
	}
}

TEST_CASE(
	"Pytagorean Tile mapped scale rotation and gap match their white-map endpoints",
	"[source_pytagorean_tile]"
) {
	auto mappedScale = Pytagorean();
	Set(mappedScale, "scale", Vector2{2, 4});
	Set(mappedScale, "scale_mapped", true);
	WhiteMap(mappedScale, "scale_map");
	auto scaleEndpoint = Pytagorean();
	Set(scaleEndpoint, "scale", Vector2{4, 4});
	Same(Draw(mappedScale), Draw(scaleEndpoint));

	auto mappedRotation = Pytagorean();
	Set(mappedRotation, "rotation_mapped", true);
	Set(mappedRotation, "rotation_map_range", Vector2{0, 45});
	WhiteMap(mappedRotation, "rotation_map");
	auto rotationEndpoint = Pytagorean();
	Set(rotationEndpoint, "rotation", 45.0);
	Same(Draw(mappedRotation), Draw(rotationEndpoint));

	auto mappedGap = Pytagorean();
	Set(mappedGap, "gap", .4);
	Set(mappedGap, "gap_mapped", true);
	WhiteMap(mappedGap, "gap_map");
	auto gapEndpoint = Pytagorean();
	Set(gapEndpoint, "gap", .4);
	Same(Draw(mappedGap), Draw(gapEndpoint));
}

TEST_CASE(
	"Pytagorean Tile preserves requested depths and mask scratch precision", "[source_pytagorean_tile]"
) {
	for (int64_t depth : {2, 3, 4, 5, 6, 7, 8}) {
		const auto run = TileRun({{"attribute_color_depth", EnumValue{depth}}});
		REQUIRE(run.Ok);
		CHECK(run.Output().Format == *SourceSurfaceFormat(depth));
		CHECK(ValidSurfaceLayout(run.Output(), Limits::MaximumDimension, Limits::MaximumOutputBytes));
	}

	const auto original = TileRun();
	REQUIRE(original.Ok);
	const std::array<SurfacePixel, 1> maskPixel{{{.3, .4, .5, .6}}};
	const Image mask = FloatImage(1, 1, maskPixel);
	const auto masked = TileRun({}, {{"mask", &mask}});
	REQUIRE(masked.Ok);
	CHECK(masked.Output().Format == SurfaceFormat::RGBA32Float);
	const auto actual = Pixel(masked.Output());
	CHECK(actual[0] == Catch::Approx(174.0 / 255.0).margin(1e-6));
	CHECK(actual[1] == Catch::Approx(139.0 / 255.0).margin(1e-6));
	CHECK(actual[2] == Catch::Approx(83.0 / 255.0).margin(1e-6));
	CHECK(actual[3] == Catch::Approx(23.0 / 255.0).margin(1e-6));
}

TEST_CASE("Pytagorean Tile projects source surface values and dimensions", "[source_pytagorean_tile]") {
	for (const std::string_view port : {"position", "scale"}) {
		auto linked = Pytagorean();
		linked.Nodes.push_back(Solid("linked_surface", {2, 1}, {255, 255, 255, 255}));
		linked.Links.push_back({"linked_surface", "surface_out", "generator", std::string(port)});
		auto direct = Pytagorean();
		Set(direct, std::string(port), Vector2{2, 1});
		Set(direct, std::string(port) + "_unit", EnumValue{0});
		CHECK(Draw(linked) == Draw(direct));
	}
	for (const std::string_view port : {"position", "scale"}) {
		auto linkedArray = Pytagorean();
		SurfaceRows(linkedArray, port);
		auto direct = Pytagorean();
		Set(direct, std::string(port), Vector2{1, 1});
		Set(direct, std::string(port) + "_unit", EnumValue{0});
		CHECK(Draw(linkedArray) == Draw(direct));
	}

	auto referencePosition = Pytagorean();
	Set(referencePosition, "position", Vector2{.25, .125});
	Set(referencePosition, "position_unit", EnumValue{1});
	auto pixelPosition = Pytagorean();
	Set(pixelPosition, "position", Vector2{2, 1});
	Set(pixelPosition, "position_unit", EnumValue{0});
	CHECK(Draw(referencePosition) == Draw(pixelPosition));

	auto firstDimensionReference = Pytagorean();
	SurfaceRows(firstDimensionReference, "dimension");
	Set(firstDimensionReference, "position", Vector2{.5, 1});
	Set(firstDimensionReference, "position_unit", EnumValue{1});
	auto firstDimensionPixels = firstDimensionReference;
	Set(firstDimensionPixels, "position", Vector2{1, 1});
	Set(firstDimensionPixels, "position_unit", EnumValue{0});
	const auto referenceRows = DrawRows(firstDimensionReference);
	const auto pixelRows = DrawRows(firstDimensionPixels);
	REQUIRE(referenceRows.Images.size() == 2);
	REQUIRE(pixelRows.Images.size() == 2);
	CHECK(referenceRows.Images[0] == pixelRows.Images[0]);
	CHECK(referenceRows.Images[1] == pixelRows.Images[1]);

	auto varying = Pytagorean();
	SurfaceRows(varying, "dimension");
	const auto rows = DrawRows(varying);
	REQUIRE(rows.Images.size() == 2);
	CHECK(rows.Images[0].Width == 2);
	CHECK(rows.Images[0].Height == 1);
	CHECK(rows.Images[1].Width == 3);
	CHECK(rows.Images[1].Height == 2);

	auto equal = Pytagorean();
	equal.Nodes.push_back(Solid("first", {2, 1}, {255, 255, 255, 255}));
	equal.Nodes.push_back(Solid("second", {2, 1}, {255, 255, 255, 255}));
	Node surfaces{"surfaces", "value.array", "", {}, {}};
	surfaces.DynamicInputs = {
		{"first", ValueType::Image, std::nullopt}, {"second", ValueType::Image, std::nullopt}
	};
	equal.Nodes.push_back(std::move(surfaces));
	equal.Links.push_back({"first", "surface_out", "surfaces", "first"});
	equal.Links.push_back({"second", "surface_out", "surfaces", "second"});
	equal.Links.push_back({"surfaces", "array", "generator", "dimension"});
	auto scalar = Pytagorean();
	Set(scalar, "dimension", Vector2{2, 1});
	Set(scalar, "dimension_unit", EnumValue{0});
	CHECK(Draw(equal) == Draw(scalar));
}

TEST_CASE("Pytagorean Tile UV mix zero retains UV-map alpha", "[source_pytagorean_tile]") {
	auto noMap = Pytagorean();
	auto zeroMix = Pytagorean();
	Set(zeroMix, "uv_mix", 0.0);
	zeroMix.Nodes.push_back(Solid("uv", {1, 1}, {128, 64, 0, 96}));
	zeroMix.Links.push_back({"uv", "surface_out", "generator", "uv_map"});
	auto fullMix = zeroMix;
	Set(fullMix, "uv_mix", 1.0);
	const auto unmodified = Draw(noMap);
	const auto alphaOnly = Draw(zeroMix);
	const auto redirected = Draw(fullMix);
	CHECK(alphaOnly.Pixels != unmodified.Pixels);
	CHECK(redirected.Pixels != alphaOnly.Pixels);
}

TEST_CASE(
	"Pytagorean Tile texture mode with no linked texture samples the source white surface",
	"[source_pytagorean_tile]"
) {
	auto document = Pytagorean();
	Set(document, "render_type", EnumValue{2});
	const auto missing = Draw(document);
	auto explicitWhite = document;
	explicitWhite.Nodes.push_back(Solid("white", {1, 1}, {255, 255, 255, 255}));
	explicitWhite.Links.push_back({"white", "surface_out", "generator", "texture"});
	Same(missing, Draw(explicitWhite));
}

TEST_CASE(
	"Pytagorean Tile refuses invalid render type and consumed numeric edges", "[source_pytagorean_tile]"
) {
	const auto clampedMode = TileRun({{"render_type", EnumValue{3}}});
	const auto lastDefinedMode = TileRun({{"render_type", EnumValue{2}}});
	REQUIRE(clampedMode.Ok);
	REQUIRE(lastDefinedMode.Ok);
	CHECK(clampedMode.Output() == lastDefinedMode.Output());
	const auto fractionalMode = TileRun({{"render_type", 1.5}});
	CHECK_FALSE(fractionalMode.Ok);
	CHECK(fractionalMode.Code == Status::UnsupportedExecution);
	CHECK(fractionalMode.Port == "render_type");

	auto zeroScale = Pytagorean();
	Set(zeroScale, "scale", Vector2{0, 1});
	Refuse(zeroScale, Status::UnsupportedExecution, "scale");

	auto zeroPhase = Pytagorean();
	Set(zeroPhase, "phase", 0.0);
	Refuse(zeroPhase, Status::UnsupportedExecution, "phase");

	auto equalLevels = Pytagorean();
	Set(equalLevels, "render_type", EnumValue{1});
	Set(equalLevels, "level_in", Vector2{.5, .5});
	Refuse(equalLevels, Status::UnsupportedExecution, "level_in");
}

TEST_CASE("Pytagorean Tile requires seed only when a covered mode consumes it", "[source_pytagorean_tile]") {
	auto covered = Pytagorean();
	Erase(covered, "seed");
	Refuse(covered, Status::UnsupportedExecution, "seed");

	auto empty = Pytagorean();
	Set(empty, "dimension", Vector2{.25, .25});
	Erase(empty, "seed");
	CHECK(Draw(empty).Pixels.size() == 4);
}

TEST_CASE("Pytagorean Tile rejects raw Atlas map bindings", "[source_pytagorean_tile]") {
	const auto rawMap = TileRun({{"scale_mapped", true}, {"scale_map", AtlasValue{}}});
	CHECK_FALSE(rawMap.Ok);
	CHECK(rawMap.Code == Status::UnsupportedExecution);
	CHECK(rawMap.Port == "scale_map");
}
