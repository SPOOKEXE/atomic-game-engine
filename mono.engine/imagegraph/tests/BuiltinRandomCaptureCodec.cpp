#include <engine/imagegraph/BuiltinRandomCaptureCodec.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <climits>
TEST_SUITE_ID("engine.imagegraph.builtin_random_capture_codec")
using namespace engine::imagegraph;
namespace {
	SourceBuiltinRandomCapture Fixture() {
		SourceBuiltinRandomCapture c;
		c.Authored = {
			"spawn",
			"pc.rigid_object_spawner",
			"rigid",
			{1.25, -2.75},
			{{"amount", int64_t{3}}, {"seed", double{17}}}
		};
		c.Authored.DynamicInputs = {{"extra", ValueType::Vector2, Value{Vector2{2, 3}}, "source layer"}};
		c.Authored.InstanceBase = "base";
		c.Authored.InstanceOverrides = {"amount"};
		c.Authored.SourceAnimatedInputs = {"spawn_area"};
		c.Authored.SourceStaticInputs = {"seed"};
		c.Authored.DynamicOutputs = {{"extra_output", ValueType::Colour}};
		c.Authored.SourceDisplayName = "A quoted \"source\"\nlabel";
		c.Authored.SourceInternalName = "source_spawn";
		c.Authored.SourceInputExpressions = {
			{"amount", "global.inputs.count", true}, {"seed", "disabled + 1", false}
		};
		ArrayValue mesh;
		mesh.ElementType = ValueType::Any;
		mesh.Items = {
			{ElementValue{Vector2{-1, -1}}}, {ElementValue{Vector2{1, -1}}}, {ElementValue{Vector2{0, 1}}}
		};
		c.Authored.SourceProperties = {{"attribute_mesh", mesh}};
		c.Tick = 12;
		c.Subframe = .25;
		c.ProcessorRow = 2;
		c.Inputs = {
			{"amount", int64_t{3}},
			{"color", Colour{5, 8, 13, 255}},
			{"label", std::string("nul\0text", 8)},
			{"mesh", mesh}
		};
		RigidValue alias;
		alias.Data.emplace();
		alias.Data->OwnerId = "owner";
		alias.Data->BodyId = "body/0";
		ArrayValue objects;
		objects.ElementType = ValueType::Rigid;
		objects.Elements = {alias};
		c.Inputs.push_back({"object", objects});
		Image rgba4{1, 1, {0x10, 0xf3}, 77, SurfaceFormat::RGBA4Unorm};
		Image r32{1, 1, {0, 0, 0x80, 0x3f}, 19, SurfaceFormat::R32Float};
		c.InputImages = {{"texture", rgba4}, {"mask", r32}};
		AtlasValue atlas;
		auto &a = atlas.Data.emplace();
		a.Kind = AtlasKind::SurfaceAtlas;
		a.Surface = SurfaceValue{rgba4};
		a.OriginalSurface = SurfaceValue{r32};
		a.Position = {1, -2};
		a.Scale = {-1, .25};
		a.Dimension = {1, 1};
		a.OriginalDimension = {2, 3};
		a.RotationDegrees = -.5;
		a.Alpha = .25;
		c.Inputs.push_back({"atlas", atlas});
		c.Inputs.push_back({"particle_empty", ParticleValue{}});
		c.Inputs.push_back({"tileset_empty", TilesetValue{}});

		c.Draws = {
			{SourceBuiltinRandomOperation::Random, 0, 1, .5},
			{SourceBuiltinRandomOperation::IRandom, 0, 3, 3},
			{SourceBuiltinRandomOperation::IRandomRange, -2, 3, -2},
			{SourceBuiltinRandomOperation::CRand, 0, 0, INT_MAX},
			{SourceBuiltinRandomOperation::SeedObservation, 0, 0, -.25},
			{SourceBuiltinRandomOperation::RandomRange, -2, 3, -2},
			{SourceBuiltinRandomOperation::RandomRange, -2, 3, 3},
			{SourceBuiltinRandomOperation::RandomRange, 4, 4, 4},
			{SourceBuiltinRandomOperation::RandomRange, 3, -2, .125}
		};
		return c;
	}
}
TEST_CASE(
	"Durable builtin captures preserve complete authored controls images and named source calls",
	"[builtin_random_codec]"
) {
	std::vector<SourceBuiltinRandomCapture> captures{Fixture()};
	auto second = Fixture();
	second.Authored.Id = "other";
	second.Tick = 0;
	second.Subframe = .25;
	second.NegativeFrame = true;
	captures.push_back(second);
	std::string text;
	Diagnostic d;
	const auto status = WriteBuiltinRandomCapture(captures, text, d);
	INFO(d.Message);
	REQUIRE(status == Status::Ok);
	CHECK(text.starts_with("imagegraph-builtin-random 1\n"));
	CHECK(text.find("random_get_seed") != std::string::npos);
	CHECK(text.find("random_range") != std::string::npos);
	std::vector<SourceBuiltinRandomCapture> restored;
	const auto read = ReadBuiltinRandomCapture(text, restored, d);
	INFO(d.Message);
	REQUIRE(read == Status::Ok);
	CHECK(restored == captures);
	CHECK(restored[0].InputImages[0].Data.Hash == 77);
	CHECK(restored[0].InputImages[1].Data.Format == SurfaceFormat::R32Float);
	std::string repeated;
	REQUIRE(WriteBuiltinRandomCapture(restored, repeated, d) == Status::Ok);
	CHECK(repeated == text);
	for (size_t index = 0; index < 6; ++index) {
		const auto operation = static_cast<SourceBuiltinRandomOperation>(index);
		CHECK(ParseBuiltinRandomOperationName(BuiltinRandomOperationName(operation)) == operation);
	}
	CHECK(BuiltinRandomOperationName(static_cast<SourceBuiltinRandomOperation>(255)).empty());
	CHECK_FALSE(ParseBuiltinRandomOperationName("5"));
}
TEST_CASE("Malformed durable builtin observations preserve prior owned captures", "[builtin_random_codec]") {
	std::vector<SourceBuiltinRandomCapture> source{Fixture()}, prior{Fixture()};
	prior[0].Authored.Id = "prior";
	const auto expected = prior;
	std::string good;
	Diagnostic d;
	REQUIRE(WriteBuiltinRandomCapture(source, good, d) == Status::Ok);
	for (auto bad : std::array<std::string, 5>{
			 good.substr(0, good.size() - 1),
			 good + "trailing",
			 std::string("imagegraph-builtin-random 2\n"),
			 std::string("imagegraph-builtin-random 1\n20:18446744073709551615\n"),
			 std::string("imagegraph-builtin-random 1\n1:-1\n")
		 }) {
		CHECK(ReadBuiltinRandomCapture(bad, prior, d) != Status::Ok);
		CHECK(prior == expected);
		CHECK_FALSE(d.Message.empty());
	}
	auto bad = good;
	auto position = bad.find("15:random_get_seed");
	REQUIRE(position != std::string::npos);
	bad.replace(position, 18, "15:invented_random");
	CHECK(ReadBuiltinRandomCapture(bad, prior, d) == Status::Malformed);
	CHECK(prior == expected);
	CHECK(ReadBuiltinRandomCapture(good, prior, d, 1) == Status::LimitExceeded);
	CHECK(prior == expected);
	CHECK(ReadBuiltinRandomCapture(good, prior, d, 0) == Status::LimitExceeded);
	CHECK(prior == expected);

	bad = good;
	position = bad.find("12:random_range\n1:4\n1:4\n1:4\n");
	REQUIRE(position != std::string::npos);
	bad[position + std::string_view("12:random_range\n1:4\n1:4\n1:").size()] = '5';
	CHECK(ReadBuiltinRandomCapture(bad, prior, d) == Status::InvalidValue);
	CHECK(prior == expected);
	CHECK(d.NodeId == "spawn");
	prior.reserve(1024);
	const auto capacity = prior.capacity();
	CHECK(ReadBuiltinRandomCapture(good, prior, d, good.size() + 16384) == Status::LimitExceeded);
	CHECK(prior == expected);
	CHECK(prior.capacity() == capacity);
	std::string saved = "prior encoded capture";
	CHECK(WriteBuiltinRandomCapture(source, saved, d, 1) == Status::LimitExceeded);
	CHECK(saved == "prior encoded capture");
}
TEST_CASE(
	"Unsupported runtime closures and malformed draws cannot become durable builtin files",
	"[builtin_random_codec]"
) {
	auto capture = Fixture();
	std::string previous = "last good capture";
	Diagnostic d;
	SurfaceValue surface{Image{1, 1, {1, 2, 3, 255}}};
	capture.Inputs.push_back({"runtime_surface", surface});
	CHECK(WriteBuiltinRandomCapture({&capture, 1}, previous, d) == Status::UnsupportedExecution);
	CHECK(previous == "last good capture");
	CHECK(d.NodeId == "spawn");
	CHECK(d.Port == "runtime_surface");
	capture = Fixture();
	capture.Draws.back().Result = 4;
	CHECK(WriteBuiltinRandomCapture({&capture, 1}, previous, d) == Status::InvalidValue);
	CHECK(previous == "last good capture");
	capture = Fixture();
	capture.Draws[4].Lower = 1;
	CHECK(WriteBuiltinRandomCapture({&capture, 1}, previous, d) == Status::InvalidValue);
	CHECK(previous == "last good capture");
}
TEST_CASE(
	"Loaded builtin observations drive the real MK Sparkle CPU consumer and reject stale controls",
	"[builtin_random_codec]"
) {
	Document document;
	document.FormatVersion = 9;
	ArrayValue colors;
	colors.ElementType = ValueType::Colour;
	colors.Elements = {Colour{255, 80, 40, 255}};
	document.Nodes = {
		{"spark",
		 "pc.mk_sparkle",
		 "",
		 {},
		 {{"seed", 7.}, {"colors", colors}, {"size", int64_t{8}}, {"amount", .5}}}
	};
	document.Outputs = {{"image", "spark", "surface_out"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluationRequest request;
	SourceBuiltinRandomCapture capture;
	REQUIRE(
		PrepareSourceBuiltinRandomCapture(document, plan, "spark", request, capture, diagnostic) == Status::Ok
	);
	capture.Draws = {{SourceBuiltinRandomOperation::IRandom, 0, 2, 0}};
	for (int i = 0; i < 3; ++i) {
		for (const auto &draw : std::array<SourceBuiltinRandomDraw, 7>{
				 {{SourceBuiltinRandomOperation::Random, 0, 1, .1},
				  {SourceBuiltinRandomOperation::Random, 0, 1, .6},
				  {SourceBuiltinRandomOperation::IRandomRange, 1, 1, 1},
				  {SourceBuiltinRandomOperation::IRandomRange, 1, 1, 1},
				  {SourceBuiltinRandomOperation::IRandomRange, 1, 2, 2},
				  {SourceBuiltinRandomOperation::Random, 0, 1, .9},
				  {SourceBuiltinRandomOperation::Random, 0, 1, .9}}
			 })
			capture.Draws.push_back(draw);
	}
	request.BuiltinRandomCaptures = {&capture, 1};
	Image before;
	auto status = Evaluate(document, plan, "image", request, before, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	CHECK(std::any_of(before.Pixels.begin(), before.Pixels.end(), [](auto value) { return value != 0; }));
	std::string text;
	REQUIRE(WriteBuiltinRandomCapture({&capture, 1}, text, diagnostic) == Status::Ok);
	std::vector<SourceBuiltinRandomCapture> loaded;
	REQUIRE(ReadBuiltinRandomCapture(text, loaded, diagnostic) == Status::Ok);
	request.BuiltinRandomCaptures = loaded;
	Image after;
	REQUIRE(Evaluate(document, plan, "image", request, after, diagnostic) == Status::Ok);
	CHECK(after == before);
	document.Nodes[0].Values[0].Data = 8.;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	CHECK(Evaluate(document, plan, "image", request, after, diagnostic) == Status::InvalidValue);
	CHECK(after == before);
	CHECK(diagnostic.Port == "seed");
}
