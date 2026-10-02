#include <engine/imagegraph/FeedbackHost.hpp>
#include <engine/imagegraph/SimulationReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
TEST_SUITE_ID("engine.imagegraph.flip_render")
using namespace engine::imagegraph;
Document FlipRenderScene(int64_t updateSteps = 1) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"domain",
		 "pc.flip_domain",
		 "",
		 {},
		 {{"dimension_unit", EnumValue{0}},
		  {"dimension", Vector2{16, 16}},
		  {"particle_size", int64_t{2}},
		  {"attribute_max_particles", 16.},
		  {"attribute_iteration", 2.},
		  {"attribute_iteration_particle", 0.},
		  {"attribute_skip_incompressible", true},
		  {"gravity", 5.},
		  {"time_step", .1}}},
		{"fill",
		 "pc.flip_fill",
		 "",
		 {},
		 {{"spawn_area_unit", EnumValue{0}}, {"spawn_area", Area{8, 8, 4, 4}}, {"density", .5}}},
		{"render",
		 "pc.flip_render",
		 "",
		 {},
		 {{"update_step", updateSteps},
		  {"particle_size", 2.},
		  {"draw_obstracles", false},
		  {"threshold", false},
		  {"alpha", Vector2{1, 1}},
		  {"lifespan", Vector2{0, 0}}}}
	};
	document.Links = {{"domain", "domain", "fill", "domain"}, {"fill", "domain", "render", "domain"}};
	document.Outputs = {{"image", "render", "rendered"}};
	return document;
}
TEST_CASE(
	"Source FLIP image graph steps readback mirrors and repeats "
	"fixed-seed reset pixels",
	"[imagegraph]"
) {
	const auto document = FlipRenderScene();
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluationRequest request;
	request.Seed = 12345;
	request.SimulationAuthoringRevision = 4;
	SimulationEvaluationResult first;
	REQUIRE(EvaluateSimulation(document, plan, "image", request, first, diagnostic) == Status::Ok);
	const auto firstPixels = std::get<Image>(first.Output).Pixels;
	CHECK(std::any_of(firstPixels.begin(), firstPixels.end(), [](uint8_t byte) { return byte != 0; }));
	REQUIRE(first.Replay.Entries.size() == 1);
	CHECK(first.Replay.Entries[0].Fluid.Data->ReadbackLife.size() == 4);
	request.Tick = 1;
	request.SimulationReplay = &first.Replay;
	REQUIRE(EvaluateSimulation(document, plan, "image", request, first, diagnostic) == Status::Ok);
	const auto secondPixels = std::get<Image>(first.Output).Pixels;
	request.Tick = 0;
	request.SimulationReplay = nullptr;
	SimulationEvaluationResult reset;
	REQUIRE(EvaluateSimulation(document, plan, "image", request, reset, diagnostic) == Status::Ok);
	CHECK(std::get<Image>(reset.Output).Pixels == firstPixels);
	request.Tick = 1;
	request.SimulationReplay = &reset.Replay;
	REQUIRE(EvaluateSimulation(document, plan, "image", request, reset, diagnostic) == Status::Ok);
	CHECK(std::get<Image>(reset.Output).Pixels == secondPixels);
	const auto previous = reset.Replay;
	const auto previousPixels = std::get<Image>(reset.Output).Pixels;
	request.Tick = 2;
	request.RequireSourceGpuRasterCoverage = true;
	CHECK(
		EvaluateSimulation(document, plan, "image", request, reset, diagnostic) ==
		Status::UnsupportedExecution
	);
	CHECK(reset.Replay == previous);
	CHECK(std::get<Image>(reset.Output).Pixels == previousPixels);
}
TEST_CASE("FLIP Fill cannot refresh source readback mirrors without Step", "[imagegraph]") {
	const auto document = FlipRenderScene(0);
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	SimulationEvaluationResult result;
	REQUIRE(EvaluateSimulation(document, plan, "image", {}, result, diagnostic) == Status::Ok);
	const auto &image = std::get<Image>(result.Output);
	CHECK(std::all_of(image.Pixels.begin(), image.Pixels.end(), [](uint8_t byte) { return byte == 0; }));
	REQUIRE(result.Replay.Entries.size() == 1);
	CHECK(result.Replay.Entries[0].Fluid.Data->ParticleCount == 4);
	CHECK(result.Replay.Entries[0].Fluid.Data->ReadbackPositions.empty());
}

TEST_CASE(
	"FLIP Render host seek matches sequential fixed ticks and refuses fractional advance",
	"[imagegraph][feedback][flip_render]"
) {
	auto document = FlipRenderScene();
	// Exercise the pinned source default.
	auto &controls = document.Nodes.back().Values;
	controls.erase(
		std::remove_if(
			controls.begin(),
			controls.end(),
			[](const AuthoredValue &value) { return value.Port == "update_step"; }
		),
		controls.end()
	);
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	CapturedFeedbackHost sequential, seek;
	EvaluationRequest sequentialRequest, seekRequest;
	sequentialRequest.Seed = seekRequest.Seed = 12345;
	for (uint64_t tick = 0; tick <= 3; ++tick) {
		sequentialRequest.Tick = tick;
		INFO(diagnostic.Message);
		REQUIRE(sequential.Prepare(
			document, plan, 4, 1, sequentialRequest, diagnostic, Limits::MaximumEvaluationBytes, "image"
		));
	}
	seekRequest.Tick = 3;
	REQUIRE(
		seek.Prepare(document, plan, 4, 1, seekRequest, diagnostic, Limits::MaximumEvaluationBytes, "image")
	);
	REQUIRE(sequential.Output("image"));
	REQUIRE(seek.Output("image"));
	CHECK(*seek.Output("image") == *sequential.Output("image"));
	REQUIRE(seekRequest.SimulationReplay);
	REQUIRE(sequentialRequest.SimulationReplay);
	CHECK(*seekRequest.SimulationReplay == *sequentialRequest.SimulationReplay);
	const auto priorImage = *seek.Output("image");
	const auto priorReplay = *seekRequest.SimulationReplay;
	seekRequest.Subframe = .5;
	CHECK_FALSE(
		seek.Prepare(document, plan, 4, 1, seekRequest, diagnostic, Limits::MaximumEvaluationBytes, "image")
	);
	CHECK(diagnostic.Code == Status::InvalidValue);
	CHECK(*seek.Output("image") == priorImage);
	CHECK(*seekRequest.SimulationReplay == priorReplay);
}
TEST_CASE(
	"FLIP Render zero-step remains a nonadvancing fractional host sample",
	"[imagegraph][feedback][flip_render]"
) {
	const auto document = FlipRenderScene(0);
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	CapturedFeedbackHost host;
	EvaluationRequest request;
	request.Tick = 2;
	request.Subframe = .5;
	request.Seed = 12345;
	INFO(diagnostic.Message);
	REQUIRE(host.Prepare(document, plan, 4, 1, request, diagnostic, Limits::MaximumEvaluationBytes, "image"));
	REQUIRE(host.Output("image"));
	CHECK(
		std::all_of(
			host.Output("image")->Pixels.begin(), host.Output("image")->Pixels.end(), [](uint8_t byte) {
				return byte == 0;
			}
		)
	);
	REQUIRE(request.SimulationReplay);
	REQUIRE(request.SimulationReplay->Entries.size() == 1);
	CHECK(request.SimulationReplay->Entries.front().Fluid.Data->ReadbackPositions.empty());
}

TEST_CASE(
	"FLIP Render linked zero-step preserves nonadvancing fractional sampling",
	"[imagegraph][feedback][flip_render]"
) {
	auto document = FlipRenderScene(1);
	document.Nodes.push_back({"zero", "value.number", "", {}, {{"value", 0.0}}});
	document.Links.push_back({"zero", "number", "render", "update_step"});
	Plan plan;
	Diagnostic diagnostic;
	const auto compiled = Compile(document, plan, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(compiled == Status::Ok);
	CapturedFeedbackHost host;
	EvaluationRequest request;
	request.Tick = 2;
	request.Subframe = .5;
	request.Seed = 12345;
	REQUIRE(host.Prepare(document, plan, 4, 1, request, diagnostic, Limits::MaximumEvaluationBytes, "image"));
	REQUIRE(host.Output("image"));
	CHECK(
		std::all_of(
			host.Output("image")->Pixels.begin(), host.Output("image")->Pixels.end(), [](uint8_t byte) {
				return byte == 0;
			}
		)
	);
	REQUIRE(request.SimulationReplay);
	REQUIRE(request.SimulationReplay->Entries.size() == 1);
	CHECK(request.SimulationReplay->Entries.front().Fluid.Data->ReadbackPositions.empty());
}
