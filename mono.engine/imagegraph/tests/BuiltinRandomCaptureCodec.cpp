#include <engine/imagegraph/BuiltinRandomCaptureCodec.hpp>
#include <engine/imagegraph/Catalogue.hpp>
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
	CHECK(text.starts_with("imagegraph-builtin-random 3\n"));
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
TEST_CASE(
	"Builtin capture v5 preserves local Vec2 defaults, inactive axes and prior outputs on budget refusal",
	"[builtin_random_codec]"
) {
	SourceBuiltinRandomCapture capture;
	capture.Authored = {"mirror", "pc.mirror_polar", "", {}, {{"center", Vector2{.9, .9}}}};
	capture.Authored.SourceSeparatedVec2Animators.emplace().Inputs.push_back({"center", {}});
	capture.Authored.SourceSeparatedVec2Animators->Inputs.front().Separated = false;
	capture.Authored.SourceSeparatedVec2Animators->Inputs.front().Axes[0].Keys = {
		{"mirror", "center", 0, .25, "source", KeyframeEase{}}
	};
	capture.Authored.SourceSeparatedVec2Animators->Inputs.front().Axes[1].Keys = {
		{"mirror", "center", 0, 3.0, "source", KeyframeEase{}}
	};
	for (size_t axis = 0; axis < 2; ++axis)
		capture.Authored.SourceSeparatedVec2Animators->Inputs.front().Axes[axis].Keys.front().SourceKeyId =
			"mirror-axis-" + std::to_string(axis);
	capture.Authored.SourceVec2Defaults.emplace().Inputs.push_back({"center", Vector2{7, 8}});
	capture.Inputs = {{"center", Vector2{4, 2}}};

	std::string text;
	Diagnostic diagnostic;
	REQUIRE(WriteBuiltinRandomCapture({&capture, 1}, text, diagnostic) == Status::Ok);
	CHECK(text.starts_with("imagegraph-builtin-random 5\n"));
	std::vector<SourceBuiltinRandomCapture> restored;
	REQUIRE(ReadBuiltinRandomCapture(text, restored, diagnostic) == Status::Ok);
	REQUIRE(restored.size() == 1);
	CHECK(restored.front() == capture);
	CHECK_FALSE(restored.front().Authored.SourceSeparatedVec2Animators->Inputs.front().Separated);
	CHECK(restored.front().Authored.SourceVec2Defaults->Inputs.front().Data == Vector2{7, 8});

	std::string priorText = "last good capture";
	CHECK(WriteBuiltinRandomCapture({&capture, 1}, priorText, diagnostic, 1) == Status::LimitExceeded);
	CHECK(priorText == "last good capture");
	auto priorCaptures = restored;
	CHECK(ReadBuiltinRandomCapture(text, priorCaptures, diagnostic, 1) == Status::LimitExceeded);
	CHECK(priorCaptures == restored);
}
TEST_CASE(
	"Builtin capture v6 preserves cold separated axes and keeps initialized empty axes distinct",
	"[builtin_random_codec]"
) {
	SourceBuiltinRandomCapture cold;
	cold.Authored = {"mirror", "pc.mirror_polar", "", {}, {{"center", Vector2{99, 88}}}};
	cold.Authored.SourceSeparatedVec2Animators.emplace().Inputs.push_back({"center", {}});
	auto &coldAxes = cold.Authored.SourceSeparatedVec2Animators->Inputs.front();
	coldAxes.Separated = true;
	coldAxes.Initialized = false;
	cold.Authored.SourceVec2Defaults.emplace().Inputs.push_back({"center", Vector2{7, 8}});

	std::string text;
	Diagnostic diagnostic;
	REQUIRE(WriteBuiltinRandomCapture({&cold, 1}, text, diagnostic) == Status::Ok);
	CHECK(text.starts_with("imagegraph-builtin-random 6\n"));
	std::vector<SourceBuiltinRandomCapture> restored;
	REQUIRE(ReadBuiltinRandomCapture(text, restored, diagnostic) == Status::Ok);
	REQUIRE(restored.size() == 1);
	CHECK(restored.front() == cold);
	CHECK_FALSE(restored.front().Authored.SourceSeparatedVec2Animators->Inputs.front().Initialized);
	CHECK(restored.front().Authored.SourceSeparatedVec2Animators->Inputs.front().Axes[0].Keys.empty());
	CHECK(restored.front().Authored.SourceVec2Defaults->Inputs.front().Data == Vector2{7, 8});

	auto prior = restored;
	CHECK(ReadBuiltinRandomCapture(text, prior, diagnostic, 1) == Status::LimitExceeded);
	CHECK(prior == restored);

	SourceBuiltinRandomCapture warm;
	warm.Authored = cold.Authored;
	warm.Authored.SourceSeparatedVec2Animators->Inputs.front().Separated = false;
	warm.Authored.SourceSeparatedVec2Animators->Inputs.front().Initialized = true;
	warm.Authored.SourceVec2Defaults = {};
	REQUIRE(WriteBuiltinRandomCapture({&warm, 1}, text, diagnostic) == Status::Ok);
	CHECK(text.starts_with("imagegraph-builtin-random 4\n"));
	restored.clear();
	REQUIRE(ReadBuiltinRandomCapture(text, restored, diagnostic) == Status::Ok);
	REQUIRE(restored.size() == 1);
	CHECK(restored.front() == warm);
	CHECK(restored.front().Authored.SourceSeparatedVec2Animators->Inputs.front().Initialized);
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

TEST_CASE("Builtin captures retain canonical dynamic input origin metadata", "[builtin_random_codec]") {
	auto c = Fixture();
	c.Authored.DynamicInputs[0].SourceInputId = "pxc:input:17";
	c.Authored.DynamicInputs.push_back(
		{"second", ValueType::Scalar, Value{.5}, "other layer", "pxc:input:18446744073709551615"}
	);
	std::vector<SourceBuiltinRandomCapture> captures{c}, restored;
	std::string text;
	Diagnostic d;
	REQUIRE(WriteBuiltinRandomCapture(captures, text, d) == Status::Ok);
	CHECK(text.starts_with("imagegraph-builtin-random 3\n"));
	CHECK(text.find("pxc:input:17") != std::string::npos);
	REQUIRE(ReadBuiltinRandomCapture(text, restored, d) == Status::Ok);
	CHECK(restored == captures);
	auto reference = c;
	std::string{}.swap(reference.Authored.DynamicInputs[0].SourceInputId);
	std::string{}.swap(reference.Authored.DynamicInputs[1].SourceInputId);
	uint64_t withOrigins = 0, withoutOrigins = 0;
	REQUIRE(
		ValidateBuiltinRandomCaptures(captures, Limits::MaximumEvaluationBytes, withOrigins, d) == Status::Ok
	);
	REQUIRE(
		ValidateBuiltinRandomCaptures(
			std::span{&reference, size_t{1}}, Limits::MaximumEvaluationBytes, withoutOrigins, d
		) == Status::Ok
	);
	CHECK(withOrigins > withoutOrigins);
}
TEST_CASE(
	"Legacy builtin capture versions one and two retain complete dynamic defaults", "[builtin_random_codec]"
) {
	std::vector<SourceBuiltinRandomCapture> source{Fixture()}, restored;
	std::string v2;
	Diagnostic d;
	REQUIRE(WriteBuiltinRandomCapture(source, v2, d) == Status::Ok);
	const auto layer = v2.find("12:source layer\n0:\n");
	REQUIRE(layer != std::string::npos);
	// Strip the version-three absent-axis marker after the complete authored mesh property.
	const auto meshName = v2.find("14:attribute_mesh\n");
	REQUIRE(meshName != std::string::npos);
	const auto payload = meshName + std::string_view("14:attribute_mesh\n").size();
	const auto colon = v2.find(':', payload);
	REQUIRE(colon != std::string::npos);
	const auto length = std::stoull(v2.substr(payload, colon - payload));
	const auto axes = colon + 1 + length + 1;
	REQUIRE(v2.substr(axes, 8) == "5:false\n");
	v2.erase(axes, 8);
	auto legacyTwo = v2;
	legacyTwo.replace(
		0, std::string_view("imagegraph-builtin-random 3\n").size(), "imagegraph-builtin-random 2\n"
	);
	REQUIRE(ReadBuiltinRandomCapture(legacyTwo, restored, d) == Status::Ok);
	CHECK(restored == source);
	// Removing the appended version-two origin token reproduces the prior writer's field order.
	auto v1 = v2;
	v1.erase(layer + std::string_view("12:source layer\n").size(), 3);
	v1.replace(0, std::string_view("imagegraph-builtin-random 3\n").size(), "imagegraph-builtin-random 1\n");
	REQUIRE(ReadBuiltinRandomCapture(v1, restored, d) == Status::Ok);
	CHECK(restored == source);
	CHECK(restored[0].Authored.DynamicInputs[0].SourceInputId.empty());
	std::string upgraded;
	REQUIRE(WriteBuiltinRandomCapture(restored, upgraded, d) == Status::Ok);
	CHECK(upgraded.starts_with("imagegraph-builtin-random 3\n"));
	std::vector<SourceBuiltinRandomCapture> upgradedOwned;
	REQUIRE(ReadBuiltinRandomCapture(upgraded, upgradedOwned, d) == Status::Ok);
	CHECK(upgradedOwned == source);
}
TEST_CASE(
	"Malformed origin capture fields preserve prior text and copied observations", "[builtin_random_codec]"
) {
	auto c = Fixture();
	c.Authored.DynamicInputs[0].SourceInputId = "pxc:input:17";
	std::vector<SourceBuiltinRandomCapture> source{c}, restored{Fixture()};
	const auto prior = restored;
	std::string good;
	Diagnostic d;
	REQUIRE(WriteBuiltinRandomCapture(source, good, d) == Status::Ok);
	for (const auto invalid :
		 {"pxc:input:017",
		  "pxc:input:-1",
		  "pxc:input:18446744073709551616",
		  "pxc:input:",
		  "17",
		  "pxc:input:17x"}) {
		auto bad = good;
		const auto token = bad.find("12:source layer\n12:pxc:input:17\n");
		REQUIRE(token != std::string::npos);
		const auto offset = token + std::string_view("12:source layer\n").size();
		bad.replace(
			offset,
			std::string_view("12:pxc:input:17\n").size(),
			std::to_string(std::string_view(invalid).size()) + ":" + invalid + "\n"
		);
		CHECK(ReadBuiltinRandomCapture(bad, restored, d) == Status::InvalidValue);
		CHECK(restored == prior);
		auto invalidSource = source;
		invalidSource[0].Authored.DynamicInputs[0].SourceInputId = invalid;
		auto text = good;
		CHECK(WriteBuiltinRandomCapture(invalidSource, text, d) == Status::InvalidValue);
		CHECK(text == good);
	}
	auto duplicate = source;
	duplicate[0].Authored.DynamicInputs.push_back(
		{"duplicate", ValueType::Scalar, Value{1.}, "", "pxc:input:17"}
	);
	auto text = good;
	CHECK(WriteBuiltinRandomCapture(duplicate, text, d) == Status::DuplicateId);
	CHECK(text == good);
	auto oversize = good;
	const auto token = oversize.find("12:pxc:input:17\n");
	REQUIRE(token != std::string::npos);
	oversize.replace(
		token, std::string_view("12:pxc:input:17\n").size(), "65:" + std::string(65, 'x') + "\n"
	);
	CHECK(ReadBuiltinRandomCapture(oversize, restored, d) == Status::LimitExceeded);
	CHECK(restored == prior);
}

TEST_CASE(
	"Origin strings with retained spare capacity obey capture replacement residency", "[builtin_random_codec]"
) {
	auto c = Fixture();
	c.Authored.DynamicInputs[0].SourceInputId = "pxc:input:17";
	std::vector<SourceBuiltinRandomCapture> source{c};
	Diagnostic d;
	uint64_t bytes = 0;
	REQUIRE(ValidateBuiltinRandomCaptures(source, Limits::MaximumEvaluationBytes, bytes, d) == Status::Ok);
	source[0].Authored.DynamicInputs[0].SourceInputId.reserve(65536);
	uint64_t retained = 0;
	REQUIRE(ValidateBuiltinRandomCaptures(source, Limits::MaximumEvaluationBytes, retained, d) == Status::Ok);
	CHECK(retained >= bytes + 65000);
	std::string text = "prior text";
	CHECK(WriteBuiltinRandomCapture(source, text, d, 32768) == Status::LimitExceeded);
	CHECK(text == "prior text");
}
TEST_CASE(
	"Native input origins survive actual authored-node capture preparation and durable reload",
	"[builtin_random_codec]"
) {
	Document doc;
	doc.FormatVersion = 9;
	doc.Nodes = {{"merge", "pc.string_merge", "", {}, {}}};
	doc.Nodes[0].DynamicInputs = {
		{"text_0", ValueType::Text, std::string("a"), {}, "pxc:input:0"},
		{"text_1", ValueType::Text, std::string("b"), {}, "pxc:input:1"}
	};
	doc.Outputs = {{"out", "merge", "text"}};
	Plan plan;
	Diagnostic d;
	const auto compiled = Compile(doc, plan, d);
	INFO(d.Message);
	REQUIRE(compiled == Status::Ok);
	SourceBuiltinRandomCapture capture;
	const auto prepared = PrepareSourceBuiltinRandomCapture(doc, plan, "merge", {}, capture, d);
	INFO(d.Message);
	REQUIRE(prepared == Status::Ok);
	CHECK(capture.Authored == doc.Nodes[0]);
	CHECK(capture.Draws.empty());
	std::string text;
	REQUIRE(WriteBuiltinRandomCapture(std::span{&capture, size_t{1}}, text, d) == Status::Ok);
	std::vector<SourceBuiltinRandomCapture> restored;
	REQUIRE(ReadBuiltinRandomCapture(text, restored, d) == Status::Ok);
	REQUIRE(restored.size() == 1);
	CHECK(restored[0] == capture);
	Document native;
	native.FormatVersion = 9;
	native.Nodes = {restored[0].Authored};
	native.Outputs = doc.Outputs;
	Document roundtrip;
	REQUIRE(Read(Write(native), roundtrip, d) == Status::Ok);
	CHECK(roundtrip == doc);
}
TEST_CASE(
	"Duplicate durable origins and unsupported envelope versions preserve previous captures",
	"[builtin_random_codec]"
) {
	auto c = Fixture();
	c.Authored.DynamicInputs[0].SourceInputId = "pxc:input:17";
	c.Authored.DynamicInputs.push_back({"second", ValueType::Scalar, Value{1.}, {}, "pxc:input:18"});
	std::vector<SourceBuiltinRandomCapture> source{c}, prior{Fixture()};
	const auto before = prior;
	std::string text;
	Diagnostic d;
	REQUIRE(WriteBuiltinRandomCapture(source, text, d) == Status::Ok);
	auto duplicate = text;
	const auto position = duplicate.find("12:pxc:input:18\n");
	REQUIRE(position != std::string::npos);
	duplicate[position + std::string_view("12:pxc:input:1").size()] = '7';
	CHECK(ReadBuiltinRandomCapture(duplicate, prior, d) == Status::DuplicateId);
	CHECK(prior == before);
	auto future = text;
	future.replace(
		0, std::string_view("imagegraph-builtin-random 3\n").size(), "imagegraph-builtin-random 7\n"
	);
	CHECK(ReadBuiltinRandomCapture(future, prior, d) == Status::UnsupportedVersion);
	CHECK(prior == before);
}

TEST_CASE(
	"Builtin capture origin validation handles all complete PB groups and a final duplicate",
	"[builtin_random_codec]"
) {
	SourceBuiltinRandomCapture capture;
	capture.Authored = {"rectangle", "pc.pb_draw_rectangle", "", {}, {}};
	const auto *entry = FindCatalogueEntry(capture.Authored.Type);
	REQUIRE(entry);
	REQUIRE(
		entry->DynamicTemplate.size() ==
		Limits::PixelBuilderEffectInputsPerGroup + Limits::PixelBuilderAuthoredMetadataInputsPerGroup
	);
	for (size_t group = 0; group < Limits::MaximumPixelBuilderEffectGroups; ++group)
		for (const auto &input : entry->DynamicTemplate) {
			DynamicInput port{
				std::string(input.Id) + "_" + std::to_string(group), input.Type, CatalogueDefault(input)
			};
			port.SourceInputId = "pxc:input:" + std::to_string(capture.Authored.DynamicInputs.size());
			capture.Authored.DynamicInputs.push_back(std::move(port));
		}
	REQUIRE(capture.Authored.DynamicInputs.size() == Limits::MaximumPixelBuilderDynamicInputsPerNode);
	capture.Authored.DynamicInputs.back().SourceInputId = "pxc:input:18446744073709551615";
	Document document;
	document.FormatVersion = 9;
	document.Nodes.push_back(capture.Authored);
	const auto imagePort = std::find_if(entry->Outputs.begin(), entry->Outputs.end(), [](const auto &port) {
		return port.Type == ValueType::Image;
	});
	REQUIRE(imagePort != entry->Outputs.end());
	document.Outputs = {{"image", capture.Authored.Id, std::string(imagePort->Id)}};
	Plan plan;
	Diagnostic diagnostic;
	const auto compiled = Compile(document, plan, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(compiled == Status::Ok);
	std::string text;
	REQUIRE(WriteBuiltinRandomCapture({&capture, 1}, text, diagnostic) == Status::Ok);
	std::vector<SourceBuiltinRandomCapture> restored;
	REQUIRE(ReadBuiltinRandomCapture(text, restored, diagnostic) == Status::Ok);
	CHECK(restored == std::vector<SourceBuiltinRandomCapture>{capture});
	const auto prior = restored;
	const std::string last = capture.Authored.DynamicInputs.back().SourceInputId;
	const std::string token = std::to_string(last.size()) + ":" + last + "\n";
	const auto position = text.find(token);
	REQUIRE(position != std::string::npos);
	text.replace(position, token.size(), "11:pxc:input:0\n");
	CHECK(ReadBuiltinRandomCapture(text, restored, diagnostic) == Status::DuplicateId);
	CHECK(restored == prior);
	capture.Authored.DynamicInputs.back().SourceInputId =
		capture.Authored.DynamicInputs.front().SourceInputId;
	text = "prior publication";
	CHECK(WriteBuiltinRandomCapture({&capture, 1}, text, diagnostic) == Status::DuplicateId);
	CHECK(text == "prior publication");
}
