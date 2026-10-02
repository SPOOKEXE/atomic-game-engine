#include "NodeExecutors.hpp"

#include <engine/imagegraph/FeedbackHost.hpp>
#include <engine/imagegraph/StatefulReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("engine.imagegraph.time_remap")
using namespace engine::imagegraph;
namespace {
	Document Graph(bool loop = false) {
		Document doc;
		doc.FormatVersion = 9;
		doc.Timeline = TimelineSettings{4, 0, 3, "loop", 30};
		doc.Nodes = {
			{"input",
			 "image.solid",
			 "",
			 {},
			 {{"width", int64_t{4}}, {"height", int64_t{1}}, {"colour", Colour{20, 0, 0, 255}}}},
			{"map", "image.captured", "", {}, {{"source_id", std::string{"map"}}}},
			{"remap", "pc.time_remap", "", {}, {{"max_life", int64_t{2}}, {"loop", loop}}}
		};
		doc.Keyframes = {
			{"input", "colour", 0, Colour{20, 0, 0, 255}, "linear"},
			{"input", "colour", 3, Colour{80, 0, 0, 255}, "linear"}
		};
		doc.Links = {{"input", "image", "remap", "surface_in"}, {"map", "image", "remap", "map"}};
		doc.Outputs = {{"out", "remap", "surface_out"}};
		return doc;
	}
	RequestImageSource Map() {
		Image image;
		image.Width = 4;
		image.Height = 1;
		image.Format = SurfaceFormat::RGBA32Float;
		image.Pixels.resize(64);
		REQUIRE(StoreSurfacePixel(image, 0, 0, {0, 0, 0, 1}));
		REQUIRE(StoreSurfacePixel(image, 1, 0, {.5, .5, .5, 1}));
		REQUIRE(StoreSurfacePixel(image, 2, 0, {1, 1, 1, 1}));
		REQUIRE(StoreSurfacePixel(image, 3, 0, {1, 1, 1, .5}));
		image.Hash = SurfaceHash(image);
		return {"map", std::move(image)};
	}
	void Reds(const Image &image, std::array<uint8_t, 4> expected) {
		REQUIRE(image.Width == 4);
		REQUIRE(image.Height == 1);
		for (size_t i = 0; i < 4; ++i)
			CHECK(image.Pixels[i * 4] == expected[i]);
	}
}
TEST_CASE("Time Remap caches inputs before inclusive luminance-alpha bands", "[imagegraph][time_remap]") {
	const auto document = Graph();
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	const auto map = Map();
	EvaluationRequest request;
	request.ImageSources = std::span(&map, 1);
	StatefulEvaluationResult result;
	for (request.Tick = 0; request.Tick <= 2; ++request.Tick) {
		request.SurfaceReplay = request.Tick ? &result.Surfaces : nullptr;
		const auto status = EvaluateStateful(document, plan, "out", request, result, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
	}
	Reds(std::get<Image>(result.Output), {60, 40, 20, 40});
	REQUIRE(result.Surfaces.Entries.size() == 3);
	CHECK(result.Surfaces.Entries.back().Input.Pixels[0] == 60);
	CHECK(result.Surfaces.Entries.back().Input.Format == SurfaceFormat::RGBA8Unorm);
	const auto retained = result;
	request.Subframe = .5;
	CHECK(EvaluateStateful(document, plan, "out", request, result, diagnostic) == Status::InvalidValue);
	CHECK(std::get<Image>(result.Output) == std::get<Image>(retained.Output));
	CHECK(result.Surfaces == retained.Surfaces);
	request.Subframe = 0;
	CHECK(EvaluateStateful(document, plan, "out", request, result, diagnostic, 1) == Status::LimitExceeded);
	CHECK(std::get<Image>(result.Output) == std::get<Image>(retained.Output));
	CHECK(result.Surfaces == retained.Surfaces);
}
TEST_CASE(
	"Time Remap loop uses source single wrap and retained cycle input frames", "[imagegraph][time_remap]"
) {
	const auto document = Graph(true);
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	const auto map = Map();
	EvaluationRequest request;
	request.ImageSources = std::span(&map, 1);
	StatefulEvaluationResult result;
	for (request.Tick = 0; request.Tick < 4; ++request.Tick) {
		request.SurfaceReplay = request.Tick ? &result.Surfaces : nullptr;
		REQUIRE(EvaluateStateful(document, plan, "out", request, result, diagnostic) == Status::Ok);
	}
	request.Tick = 0;
	request.SurfaceReplay = &result.Surfaces;
	request.ResetSurfaceReplay = true;
	REQUIRE(EvaluateStateful(document, plan, "out", request, result, diagnostic) == Status::Ok);
	Reds(std::get<Image>(result.Output), {20, 60, 40, 60});
	CHECK(result.Surfaces.Entries.size() == 4);
}
TEST_CASE(
	"Time Remap host direct seek equals sequential seek and resets atomically", "[imagegraph][time_remap]"
) {
	const auto document = Graph();
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	const auto map = Map();
	EvaluationRequest request;
	request.ImageSources = std::span(&map, 1);
	CapturedFeedbackHost direct, sequential;
	request.Tick = 2;
	REQUIRE(direct.Prepare(document, plan, 1, 1, request, diagnostic, Limits::MaximumEvaluationBytes, "out"));
	REQUIRE(direct.Output("out"));
	const auto expected = *direct.Output("out");
	Reds(expected, {60, 40, 20, 40});
	for (request.Tick = 0; request.Tick <= 2; ++request.Tick)
		REQUIRE(sequential.Prepare(
			document, plan, 1, 1, request, diagnostic, Limits::MaximumEvaluationBytes, "out"
		));
	REQUIRE(sequential.Output("out"));
	CHECK(*sequential.Output("out") == expected);
	request.Tick = 0;
	REQUIRE(direct.Prepare(document, plan, 1, 1, request, diagnostic, Limits::MaximumEvaluationBytes, "out"));
	Reds(*direct.Output("out"), {20, 20, 20, 20});
	request.Tick = 2;
	CHECK_FALSE(direct.Prepare(document, plan, 1, 1, request, diagnostic, 1, "out"));
	Reds(*direct.Output("out"), {20, 20, 20, 20});
	REQUIRE(direct.Prepare(document, plan, 1, 1, request, diagnostic, Limits::MaximumEvaluationBytes, "out"));
	CHECK(*direct.Output("out") == expected);
}

TEST_CASE(
	"Time Remap negative life still caches input and zero life refuses undefined shader uniforms",
	"[imagegraph][time_remap]"
) {
	auto document = Graph();
	document.Nodes.back().Values[0].Data = int64_t{-2};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	const auto map = Map();
	EvaluationRequest request;
	request.ImageSources = std::span(&map, 1);
	StatefulEvaluationResult result;
	REQUIRE(EvaluateStateful(document, plan, "out", request, result, diagnostic) == Status::Ok);
	Reds(std::get<Image>(result.Output), {0, 0, 0, 0});
	REQUIRE(result.Surfaces.Entries.size() == 1);
	CHECK(result.Surfaces.Entries.front().Input.Pixels[0] == 20);
	const auto retained = result;
	document.Nodes.back().Values[0].Data = int64_t{0};
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	request.SurfaceReplay = &result.Surfaces;
	CHECK(
		EvaluateStateful(document, plan, "out", request, result, diagnostic) == Status::UnsupportedExecution
	);
	CHECK(std::get<Image>(result.Output) == std::get<Image>(retained.Output));
	CHECK(result.Surfaces == retained.Surfaces);
}

TEST_CASE(
	"Time Remap input-depth default preserves float output while caching normalized RGBA8",
	"[imagegraph][time_remap]"
) {
	auto document = Graph();
	document.Keyframes.clear();
	document.Nodes.front() = {"input", "image.captured", "", {}, {{"source_id", std::string{"input"}}}};
	Image input;
	input.Width = 4;
	input.Height = 1;
	input.Format = SurfaceFormat::RGBA32Float;
	input.Pixels.resize(64);
	for (uint32_t x = 0; x < 4; ++x)
		REQUIRE(StoreSurfacePixel(input, x, 0, {1.5, -.2, .5, .5}));
	const std::array<RequestImageSource, 2> sources{RequestImageSource{"input", input}, Map()};
	EvaluationRequest request;
	request.ImageSources = sources;
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	StatefulEvaluationResult result;
	REQUIRE(EvaluateStateful(document, plan, "out", request, result, diagnostic) == Status::Ok);
	const auto &output = std::get<Image>(result.Output);
	CHECK(output.Format == SurfaceFormat::RGBA32Float);
	REQUIRE(result.Surfaces.Entries.size() == 1);
	CHECK(result.Surfaces.Entries.front().Input.Format == SurfaceFormat::RGBA8Unorm);
	SurfacePixel pixel;
	REQUIRE(LoadSurfacePixel(output, 0, 0, pixel));
	CHECK(pixel[0] == 1);
	CHECK(pixel[1] == 0);
	CHECK(pixel[2] == Catch::Approx(128. / 255));
	CHECK(pixel[3] == Catch::Approx(128. / 255));
}

TEST_CASE(
	"Time Remap authored depth uses real source attribute and project inheritance", "[imagegraph][time_remap]"
) {
	const auto map = Map();
	EvaluationRequest request;
	request.ImageSources = std::span(&map, 1);
	for (const auto &[choice, expected] :
		 {std::pair{3, SurfaceFormat::RGBA8Unorm},
		  std::pair{5, SurfaceFormat::RGBA32Float},
		  std::pair{6, SurfaceFormat::R8Unorm},
		  std::pair{1, SurfaceFormat::RGBA16Float}}) {
		auto document = Graph();
		document.Project = ProjectSettings{};
		document.Project->ColorDepth = 2;
		document.Nodes.back().Values.push_back({"attribute_color_depth", EnumValue{choice}});
		Plan plan;
		Diagnostic diagnostic;
		REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
		StatefulEvaluationResult result;
		INFO(diagnostic.Message);
		REQUIRE(EvaluateStateful(document, plan, "out", request, result, diagnostic) == Status::Ok);
		CHECK(std::get<Image>(result.Output).Format == expected);
		REQUIRE(result.Surfaces.Entries.size() == 1);
		CHECK(result.Surfaces.Entries.front().Input.Format == SurfaceFormat::RGBA8Unorm);
	}
}

TEST_CASE(
	"Time Remap source cache invalidation clears old revisions atomically", "[imagegraph][time_remap]"
) {
	const auto document = Graph(true);
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	const auto map = Map();
	EvaluationRequest request;
	request.ImageSources = std::span(&map, 1);
	CapturedFeedbackHost host;
	for (request.Tick = 0; request.Tick < 4; ++request.Tick)
		REQUIRE(
			host.Prepare(document, plan, 1, 1, request, diagnostic, Limits::MaximumEvaluationBytes, "out")
		);
	const Image last = *host.Output("out");
	request.Tick = 0;
	CHECK_FALSE(host.Prepare(document, plan, 2, 1, request, diagnostic, 1, "out"));
	CHECK(*host.Output("out") == last);
	REQUIRE(host.Prepare(document, plan, 2, 1, request, diagnostic, Limits::MaximumEvaluationBytes, "out"));
	Reds(*host.Output("out"), {20, 20, 0, 20});
	REQUIRE(request.SurfaceReplay);
	CHECK(request.SurfaceReplay->Entries.size() == 1);
	// A restart at the same revision preserves manually owned input generations.
	for (request.Tick = 1; request.Tick < 4; ++request.Tick)
		REQUIRE(
			host.Prepare(document, plan, 2, 1, request, diagnostic, Limits::MaximumEvaluationBytes, "out")
		);
	host.RestartCycle();
	request.Tick = 0;
	REQUIRE(host.Prepare(document, plan, 2, 1, request, diagnostic, Limits::MaximumEvaluationBytes, "out"));
	Reds(*host.Output("out"), {20, 60, 40, 60});
	CHECK(request.SurfaceReplay->Entries.size() == 4);
}

TEST_CASE(
	"Time Remap input revision reloads its generation while Interlace retains source history",
	"[imagegraph][time_remap]"
) {
	auto document = Graph(true);
	document.Keyframes.clear();
	document.Nodes.front() = {"input", "image.captured", "", {}, {{"source_id", std::string{"input"}}}};
	Image input{4, 1, std::vector<uint8_t>(16)};
	std::array<RequestImageSource, 2> sources{RequestImageSource{"input", input}, Map()};
	const auto red = [&](uint8_t value) {
		for (size_t pixel = 0; pixel < 4; ++pixel) {
			sources[0].Data.Pixels[pixel * 4] = value;
			sources[0].Data.Pixels[pixel * 4 + 3] = 255;
		}
		sources[0].Data.Hash = SurfaceHash(sources[0].Data);
	};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	CapturedFeedbackHost host;
	EvaluationRequest request;
	request.ImageSources = sources;
	for (request.Tick = 0; request.Tick < 4; ++request.Tick) {
		red(uint8_t(20 + 20 * request.Tick));
		REQUIRE(
			host.Prepare(document, plan, 1, 1, request, diagnostic, Limits::MaximumEvaluationBytes, "out")
		);
	}
	const Image last = *host.Output("out");
	request.Tick = 0;
	sources[0].Data.Pixels.pop_back();
	CHECK_FALSE(
		host.Prepare(document, plan, 1, 2, request, diagnostic, Limits::MaximumEvaluationBytes, "out")
	);
	CHECK(*host.Output("out") == last);
	sources[0].Data.Pixels.resize(16);
	red(7);
	REQUIRE(host.Prepare(document, plan, 1, 2, request, diagnostic, Limits::MaximumEvaluationBytes, "out"));
	Reds(*host.Output("out"), {7, 7, 0, 7});
	REQUIRE(request.SurfaceReplay);
	CHECK(request.SurfaceReplay->Entries.size() == 1);

	// An unrelated source cache has clearCacheOnChange=false and retains the prior cycle.
	document.Nodes.back() = {
		"interlace", "pc.interlaced", "", {}, {{"loop", true}, {"size", 1.0}, {"axis", EnumValue{1}}}
	};
	document.Links = {{"input", "image", "interlace", "surface_in"}};
	document.Outputs = {{"out", "interlace", "surface_out"}};
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	CapturedFeedbackHost interlace;
	for (request.Tick = 0; request.Tick < 4; ++request.Tick) {
		red(uint8_t(20 + 20 * request.Tick));
		REQUIRE(interlace.Prepare(
			document, plan, 1, 1, request, diagnostic, Limits::MaximumEvaluationBytes, "out"
		));
	}
	request.Tick = 0;
	red(7);
	REQUIRE(
		interlace.Prepare(document, plan, 2, 2, request, diagnostic, Limits::MaximumEvaluationBytes, "out")
	);
	Reds(*interlace.Output("out"), {7, 80, 7, 80});
	CHECK(request.SurfaceReplay->Entries.size() == 4);
}

TEST_CASE(
	"Time Remap bounds complete batch and retained history work before publication",
	"[imagegraph][time_remap]"
) {
	auto document = Graph();
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	const auto map = Map();
	EvaluationRequest request;
	request.ImageSources = std::span(&map, 1);
	StatefulEvaluationResult result;
	REQUIRE(EvaluateStateful(document, plan, "out", request, result, diagnostic) == Status::Ok);
	const auto retained = result;
	SurfaceFrameReplayState history;
	const Image cached{1, 1, {20, 0, 0, 255}};
	for (uint64_t frame = 0; frame < 4096; ++frame)
		history.Entries.push_back({"remap", frame, 0, cached});
	history.Tick = 4094;
	history.Initialized = true;
	const auto retainedHistory = history;
	document.Timeline = TimelineSettings{4096, 0, 4095, "loop", 30};
	document.Nodes.front().Values[0].Data = int64_t{128};
	document.Nodes.front().Values[1].Data = int64_t{96};
	document.Nodes.back().Values[0].Data = int64_t{4096};
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	request.Tick = 4095;
	request.SurfaceReplay = &history;
	request.ResetSurfaceReplay = false;
	CHECK(EvaluateStateful(document, plan, "out", request, result, diagnostic) == Status::LimitExceeded);
	CHECK(diagnostic.Message == "Time Remap exceeds bounded batch and history work");
	CHECK(std::get<Image>(result.Output) == std::get<Image>(retained.Output));
	CHECK(result.Surfaces == retained.Surfaces);
	CHECK(history == retainedHistory);

	// Time Remap is a plain source Node. Exercise the private batch admission seam directly,
	// without claiming its authored schema has a processor toggle or accepts image arrays.
	const auto *entry = FindCatalogueEntry("pc.time_remap");
	const auto executor = engine::imagegraph::detail::FindExecutor("pc.time_remap");
	REQUIRE(entry);
	REQUIRE(executor);
	Node node{"remap", "pc.time_remap", "", {}, {}};
	EvaluationRequest clock;
	engine::imagegraph::detail::NodeContext context(node, *entry, clock);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	context.CurrentSurfaces = &history;
	context.ProcessorCount = 64;
	const Image input{4, 1, std::vector<uint8_t>(16)};
	context.Images.emplace_back("surface_in", &input);
	context.Images.emplace_back("map", &map.Data);
	for (const auto &control : entry->Inputs)
		if (const auto value = CatalogueDefault(control)) context.Values.emplace_back(control.Id, *value);
	for (auto &[id, value] : context.Values)
		if (id == "max_life") value = int64_t{4096};
	CHECK_FALSE(executor(context));
	CHECK(context.FailureCode == Status::LimitExceeded);
	CHECK(context.OutputImages.empty());
	CHECK(context.SurfaceUpdates.empty());
	CHECK(history == retainedHistory);
}
