#include <engine/imagegraph/StatefulReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.stateful_replay")
using namespace engine::imagegraph;

namespace {
	Document InterlaceGraph() {
		Document document;
		document.FormatVersion = 9;
		document.Timeline = TimelineSettings{};
		document.Timeline->Frames = 4;
		document.Nodes = {
			{"input", "image.captured", "", {}, {{"source_id", std::string{"input"}}}},
			{"interlace", "pc.interlaced", "", {}, {{"size", 1.0}, {"delay", int64_t{1}}}}
		};
		document.Links = {{"input", "image", "interlace", "surface_in"}};
		document.Outputs = {{"out", "interlace", "surface_out"}};
		return document;
	}
	Image SolidImage(uint8_t red) {
		Image image;
		image.Width = image.Height = 2;
		image.Pixels = {red, 0, 0, 255, red, 0, 0, 255, red, 0, 0, 255, red, 0, 0, 255};
		image.Hash = SurfaceHash(image);
		return image;
	}
}

TEST_CASE("Interlace caches source input rather than edited output", "[imagegraph][stateful_replay]") {
	auto document = InterlaceGraph();
	Plan plan;
	Diagnostic diagnostic;
	const auto compiled = Compile(document, plan, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(compiled == Status::Ok);
	std::vector<RequestImageSource> sources{{"input", SolidImage(40)}};
	EvaluationRequest request;
	request.ImageSources = sources;
	StatefulEvaluationResult state;
	REQUIRE(EvaluateStateful(document, plan, "out", request, state, diagnostic) == Status::Ok);
	const auto &first = std::get<Image>(state.Output);
	CHECK(first.Pixels[0] == 40);
	CHECK(first.Pixels[8] == 0);
	REQUIRE(state.Surfaces.Entries.size() == 1);
	CHECK(state.Surfaces.Entries.front().Input.Pixels[8] == 40);
	sources[0].Data = SolidImage(90);
	request.Tick = 1;
	request.SurfaceReplay = &state.Surfaces;
	request.SimulationReplay = &state.Simulation;
	REQUIRE(EvaluateStateful(document, plan, "out", request, state, diagnostic) == Status::Ok);
	CHECK(std::get<Image>(state.Output).Pixels[0] == 90);
	CHECK(std::get<Image>(state.Output).Pixels[8] == 40);
	sources[0].Data = SolidImage(140);
	request.Tick = 2;
	REQUIRE(EvaluateStateful(document, plan, "out", request, state, diagnostic) == Status::Ok);
	CHECK(std::get<Image>(state.Output).Pixels[8] == 90);
	const auto previousOutput = std::get<Image>(state.Output);
	const auto previous = state.Surfaces;
	request.Tick = 4;
	CHECK(EvaluateStateful(document, plan, "out", request, state, diagnostic) == Status::InvalidValue);
	CHECK(std::get<Image>(state.Output) == previousOutput);
	CHECK(state.Surfaces == previous);
	request.Tick = 3;
	CHECK(EvaluateStateful(document, plan, "out", request, state, diagnostic, 1) == Status::LimitExceeded);
	CHECK(state.Surfaces == previous);
}

TEST_CASE("Interlace axis and invert select exact source shader parity", "[imagegraph][stateful_replay]") {
	auto document = InterlaceGraph();
	document.Nodes[1].Values.push_back({"axis", EnumValue{1}});
	document.Nodes[1].Values.push_back({"invert", true});
	Plan plan;
	Diagnostic diagnostic;
	const auto compiled = Compile(document, plan, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(compiled == Status::Ok);
	std::vector<RequestImageSource> sources{{"input", SolidImage(75)}};
	EvaluationRequest request;
	request.ImageSources = sources;
	StatefulEvaluationResult state;
	REQUIRE(EvaluateStateful(document, plan, "out", request, state, diagnostic) == Status::Ok);
	const auto &image = std::get<Image>(state.Output);
	CHECK(image.Pixels[0] == 0);
	CHECK(image.Pixels[4] == 75);
	CHECK(image.Pixels[8] == 0);
	CHECK(image.Pixels[12] == 75);
}

TEST_CASE(
	"Interlace loop reads retained wrapped input history after a clock restart",
	"[imagegraph][stateful_replay]"
) {
	auto document = InterlaceGraph();
	document.Nodes[1].Values.push_back({"loop", true});
	Plan plan;
	Diagnostic diagnostic;
	const auto compiled = Compile(document, plan, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(compiled == Status::Ok);
	SurfaceFrameReplayState history;
	history.Tick = 3;
	history.Initialized = true;
	history.Entries.push_back({"interlace", 3, 0, SolidImage(200)});
	std::vector<RequestImageSource> sources{{"input", SolidImage(40)}};
	EvaluationRequest request;
	request.ImageSources = sources;
	request.SurfaceReplay = &history;
	request.ResetSurfaceReplay = true;
	StatefulEvaluationResult state;
	REQUIRE(EvaluateStateful(document, plan, "out", request, state, diagnostic) == Status::Ok);
	CHECK(std::get<Image>(state.Output).Pixels[0] == 40);
	CHECK(std::get<Image>(state.Output).Pixels[8] == 200);
	CHECK(state.Surfaces.Entries.size() == 2);
	CHECK(history.Entries.size() == 1);
	auto malformed = history;
	malformed.Entries.push_back(malformed.Entries.front());
	CHECK(
		ValidateSurfaceFrameReplay(malformed, Limits::MaximumEvaluationBytes, diagnostic) ==
		Status::DuplicateId
	);
	CHECK(ValidateSurfaceFrameReplay(history, 1, diagnostic) == Status::LimitExceeded);
}
