#include <engine/imagegraph/FeedbackHost.hpp>
#include <engine/imagegraph/FrameTime.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
TEST_SUITE_ID("engine.imagegraph.feedback_host")
using namespace engine::imagegraph;
TEST_CASE(
	"Feedback host persists authored output binding and preserves generations on failed steps",
	"[imagegraph][feedback]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"prior", "image.captured", "", {}, {{"source_id", std::string{"feedback:out"}}}},
		{"invert", "image.invert", "", {}, {{"include_alpha", false}}}
	};
	document.Links = {{"prior", "image", "invert", "image"}};
	document.Outputs = {{"out", "invert", "image"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	CapturedFeedbackHost host;
	EvaluationRequest request;
	REQUIRE(host.Prepare(document, plan, 1, 1, request, diagnostic));
	REQUIRE(host.Output("out") != nullptr);
	CHECK(host.Output("out")->Pixels[0] == 255);
	REQUIRE(request.ImageSources.size() == 1);
	CHECK(request.ImageSources[0].Data.Pixels[0] == 0);
	request = {};
	request.Tick = 1;
	REQUIRE(host.Prepare(document, plan, 1, 1, request, diagnostic));
	CHECK(host.Output("out")->Pixels[0] == 0);
	REQUIRE(request.ImageSources.size() == 1);
	CHECK(request.ImageSources[0].Data.Pixels[0] == 255);
	request = {};
	request.Tick = 1;
	REQUIRE(host.Prepare(document, plan, 1, 1, request, diagnostic));
	CHECK(host.Output("out")->Pixels[0] == 0);
	request.Subframe = .5;
	CHECK_FALSE(host.Prepare(document, plan, 1, 1, request, diagnostic));
	CHECK(host.Output("out")->Pixels[0] == 0);
	request = {};
	request.Tick = 3;
	REQUIRE(host.Prepare(document, plan, 1, 1, request, diagnostic));
	CHECK(host.Output("out")->Pixels[0] == 0);
	request = {};
	REQUIRE(host.Prepare(document, plan, 1, 2, request, diagnostic));
	CHECK(host.Output("out")->Pixels[0] == 255);
	request = {};
	request.Tick = 1;
	CHECK_FALSE(host.Prepare(document, plan, 1, 2, request, diagnostic, 1));
	CHECK(diagnostic.Code == Status::LimitExceeded);
	CHECK(host.Output("out")->Pixels[0] == 255);
	host.Clear();
	CHECK_FALSE(host.Active());
}

TEST_CASE("Persistent feedback streams beyond seek work bounds", "[imagegraph][feedback]") {
	Document document;
	document.FormatVersion = 9;
	document.Project = ProjectSettings{};
	document.Project->SurfaceWidth = 1;
	document.Project->SurfaceHeight = 1;
	document.Nodes = {
		{"prior", "image.captured", "", {}, {{"source_id", std::string{"feedback:out"}}}},
		{"invert", "image.invert", "", {}, {{"include_alpha", false}}}
	};
	document.Links = {{"prior", "image", "invert", "image"}};
	document.Outputs = {{"out", "invert", "image"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	CapturedFeedbackHost host;
	for (uint64_t tick = 0; tick <= 4096; tick++) {
		EvaluationRequest request;
		request.Tick = tick;
		REQUIRE(host.Prepare(document, plan, 1, 1, request, diagnostic));
	}
	REQUIRE(host.Output("out") != nullptr);
	CHECK(host.Output("out")->Pixels[0] == 255);
}

TEST_CASE(
	"shared host carries original interlace inputs through playback edit and reset",
	"[imagegraph][feedback][stateful_replay]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Timeline = TimelineSettings{};
	document.Timeline->Frames = 4;
	document.Nodes = {
		{"input", "image.captured", "", {}, {{"source_id", std::string{"input"}}}},
		{"interlace", "pc.interlaced", "", {}, {{"loop", true}}}
	};
	document.Links = {{"input", "image", "interlace", "surface_in"}};
	document.Outputs = {{"out", "interlace", "surface_out"}};
	Plan plan;
	Diagnostic diagnostic;
	const auto compiled = Compile(document, plan, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(compiled == Status::Ok);
	CapturedFeedbackHost host;
	Image image;
	image.Width = image.Height = 2;
	image.Pixels.resize(16);
	std::vector<RequestImageSource> sources{{"input", image}};
	for (uint64_t tick = 0; tick < 4; ++tick) {
		for (size_t pixel = 0; pixel < 4; ++pixel) {
			sources[0].Data.Pixels[pixel * 4] = static_cast<uint8_t>(40 + tick * 30);
			sources[0].Data.Pixels[pixel * 4 + 3] = 255;
		}
		sources[0].Data.Hash = SurfaceHash(sources[0].Data);
		EvaluationRequest request;
		request.Tick = tick;
		request.ImageSources = sources;
		const auto prepared =
			host.Prepare(document, plan, 1, 1, request, diagnostic, Limits::MaximumEvaluationBytes, "out");
		INFO(diagnostic.Message);
		REQUIRE(prepared);
		REQUIRE(host.Output("out"));
		CHECK(host.Output("out")->Pixels[0] == 40 + tick * 30);
		CHECK(host.Output("out")->Pixels[8] == (tick ? 40 + (tick - 1) * 30 : 0));
	}
	EvaluationRequest request;
	request.ImageSources = sources;
	REQUIRE(host.Prepare(document, plan, 2, 1, request, diagnostic, Limits::MaximumEvaluationBytes, "out"));
	CHECK(host.Output("out")->Pixels[8] == 130);
	const auto previous = *host.Output("out");
	request = {};
	request.Tick = 1;
	request.ImageSources = sources;
	CHECK_FALSE(host.Prepare(document, plan, 2, 1, request, diagnostic, 1, "out"));
	CHECK(*host.Output("out") == previous);
	host.Clear();
	request = {};
	request.ImageSources = sources;
	REQUIRE(host.Prepare(document, plan, 2, 1, request, diagnostic, Limits::MaximumEvaluationBytes, "out"));
	CHECK(host.Output("out")->Pixels[8] == 0);
}

TEST_CASE(
	"shared host captures renderer inputs and feedback outputs in one frame transaction",
	"[imagegraph][feedback]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Project = ProjectSettings{};
	document.Project->SurfaceWidth = document.Project->SurfaceHeight = 1;
	document.Nodes = {
		{"prior", "image.captured", "", {}, {{"source_id", std::string{"feedback:out"}}}},
		{"invert", "image.invert", "", {}, {{"include_alpha", false}}},
		{"renderer", "image.transform_3d", "", {}, {}},
		{"grid", "pc.verlet_sim_mesh_grid", "", {}, {{"subdivision", Vector2{1, 1}}}},
		{"cache", "pc.verlet_sim_mesh_cache", "", {}, {}}
	};
	document.Links = {{"prior", "image", "invert", "image"}, {"invert", "image", "renderer", "surface"}};
	document.Links.push_back({"grid", "mesh", "cache", "mesh"});
	document.Outputs = {{"out", "invert", "image"}, {"rendered", "renderer", "rendered"}};
	Plan plan;
	Diagnostic diagnostic;
	const auto compiled = Compile(document, plan, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(compiled == Status::Ok);
	CapturedFeedbackHost host;
	EvaluationRequest request;
	REQUIRE(host.PrepareNodeInputs(document, plan, 1, 1, "renderer", request, diagnostic));
	REQUIRE(host.Snapshot().Images().size() == 1);
	REQUIRE(host.Output("out"));
	CHECK(host.Snapshot().Images()[0].Data == *host.Output("out"));
	CHECK(host.Snapshot().Images()[0].Data.Pixels[0] == 255);
	request = {};
	request.Tick = 1;
	REQUIRE(host.PrepareNodeInputs(document, plan, 1, 1, "renderer", request, diagnostic));
	CHECK(host.Snapshot().Images()[0].Data.Pixels[0] == 0);
	request = {};
	request.Tick = 1;
	REQUIRE(host.PrepareNodeInputs(document, plan, 1, 1, "renderer", request, diagnostic));
	CHECK(host.Snapshot().Images()[0].Data.Pixels[0] == 0);
	const std::array<std::string_view, 1> capture{"cache"};
	request.SimulationCacheCaptures = capture;
	REQUIRE(host.PrepareNodeInputs(document, plan, 1, 1, "renderer", request, diagnostic));
	REQUIRE(host.Output("out"));
	CHECK(host.Output("out")->Pixels[0] == 0);
	CHECK(host.Snapshot().Images()[0].Data.Pixels[0] == 0);
	request.SimulationCacheCaptures = {};
	const auto previous = host.Snapshot().Images()[0].Data;
	request.Tick = 2;
	CHECK_FALSE(host.PrepareNodeInputs(document, plan, 1, 1, "renderer", request, diagnostic, 1));
	CHECK(host.Snapshot().Images()[0].Data == previous);
	CHECK(*host.Output("out") == previous);
}

TEST_CASE(
	"differential host preserves sampled history through edits signed rewinds and repeated frames",
	"[imagegraph][feedback][source_data]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {{"d", "pc.differential", "", {}, {{"value", 10.0}}}};
	document.Outputs = {{"out", "d", "result"}};
	Plan plan;
	Diagnostic diagnostic;
	CapturedFeedbackHost host;
	uint64_t revision = 0;
	const auto sample =
		[&](double value, FrameTime frame, uint64_t maximumBytes = Limits::MaximumEvaluationBytes) {
			document.Nodes[0].Values[0].Data = value;
			const auto compiled = Compile(document, plan, diagnostic);
			INFO(diagnostic.Message);
			REQUIRE(compiled == Status::Ok);
			EvaluationRequest request;
			REQUIRE(SetFrameTime(request, frame));
			return host.Prepare(document, plan, ++revision, 1, request, diagnostic, maximumBytes, "out");
		};
	const auto result = [&]() {
		const auto *output = host.Value("out");
		REQUIRE(output != nullptr);
		return std::get<double>(std::get<EvaluatedValue>(output->Output).Data);
	};
	REQUIRE(sample(10, {2}));
	CHECK(result() == 5);
	REQUIRE(sample(20, {4}));
	CHECK(result() == 5);
	REQUIRE(sample(14, {2, 0, true}));
	CHECK(result() == 1);
	REQUIRE(sample(22, {2}));
	CHECK(result() == 2);
	REQUIRE(sample(30, {2}));
	CHECK(result() == 0);
	REQUIRE(sample(40, {3}));
	CHECK(result() == 10);
	CHECK_FALSE(sample(50, {4}, 1));
	CHECK(result() == 10);
	REQUIRE(sample(50, {4}));
	CHECK(result() == 10);
	REQUIRE(sample(60, {5000}));
	CHECK(result() == Catch::Approx(10.0 / 4996));
}
