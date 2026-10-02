#include <engine/imagegraph/FeedbackReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.feedback")
using namespace engine::imagegraph;

TEST_CASE(
	"Feedback uses preceding generation and resets deterministically across seeks", "[imagegraph][feedback]"
) {
	Document document;
	document.Nodes = {
		{"previous", "image.captured", "", {}, {{"source_id", std::string{"loop"}}}},
		{"invert", "image.invert", "", {}, {{"include_alpha", false}}}
	};
	document.Links = {{"previous", "image", "invert", "image"}};
	document.Outputs = {{"result", "invert", "image"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	std::vector<FeedbackBinding> bindings{{"loop", "result"}};
	std::vector<RequestImageSource> seeds{{"loop", Image{1, 1, {10, 20, 30, 255}}}};
	FeedbackReplayState state;
	EvaluationRequest request;
	REQUIRE(
		ReplayFeedbackFrame(document, plan, bindings, seeds, request, state, 7, true, state, diagnostic) ==
		Status::Ok
	);
	CHECK(state.Sources[0].Data.Pixels == std::vector<uint8_t>{245, 235, 225, 255});
	request.Tick = 1;
	REQUIRE(
		ReplayFeedbackFrame(document, plan, bindings, seeds, request, state, 7, false, state, diagnostic) ==
		Status::Ok
	);
	CHECK(state.Sources[0].Data.Pixels == seeds[0].Data.Pixels);
	request.Tick = 3;
	CHECK(
		ReplayFeedbackFrame(document, plan, bindings, seeds, request, state, 7, false, state, diagnostic) ==
		Status::InvalidValue
	);
	CHECK(state.Tick == 1);
	request.Tick = 2;
	CHECK(
		ReplayFeedbackFrame(document, plan, bindings, seeds, request, state, 8, false, state, diagnostic) ==
		Status::InvalidValue
	);
	CHECK(state.Tick == 1);
	request.Tick = 0;
	REQUIRE(
		ReplayFeedbackFrame(document, plan, bindings, seeds, request, state, 8, true, state, diagnostic) ==
		Status::Ok
	);
	request.Tick = 3;
	REQUIRE(SeekFeedbackReplay(document, plan, bindings, seeds, request, 8, state, diagnostic) == Status::Ok);
	CHECK(state.Sources[0].Data.Pixels == seeds[0].Data.Pixels);
	CHECK(
		SeekFeedbackReplay(document, plan, bindings, seeds, request, 8, state, diagnostic, 3) ==
		Status::LimitExceeded
	);
	request.Tick = 0;
	REQUIRE(SeekFeedbackReplay(document, plan, bindings, seeds, request, 8, state, diagnostic) == Status::Ok);
	CHECK(state.AuthoringRevision == 8);
	CHECK(state.Sources[0].Data.Pixels == std::vector<uint8_t>{245, 235, 225, 255});
}

TEST_CASE(
	"Feedback commits all bindings together and refuses invalid or over-budget replay",
	"[imagegraph][feedback]"
) {
	Document document;
	document.Nodes = {
		{"left", "image.captured", "", {}, {{"source_id", std::string{"a"}}}},
		{"right", "image.captured", "", {}, {{"source_id", std::string{"b"}}}}
	};
	document.Outputs = {{"left", "left", "image"}, {"right", "right", "image"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	std::vector<FeedbackBinding> bindings{{"a", "right"}, {"b", "left"}};
	std::vector<RequestImageSource> seeds{
		{"a", Image{1, 1, {1, 2, 3, 255}}}, {"b", Image{1, 1, {4, 5, 6, 255}}}
	};
	EvaluationRequest request;
	FeedbackReplayState prior, state;
	REQUIRE(
		ReplayFeedbackFrame(document, plan, bindings, seeds, request, prior, 1, true, state, diagnostic) ==
		Status::Ok
	);
	CHECK(state.Sources[0].Data.Pixels == seeds[1].Data.Pixels);
	CHECK(state.Sources[1].Data.Pixels == seeds[0].Data.Pixels);
	bindings[1].OutputId = "missing";
	CHECK(
		ReplayFeedbackFrame(document, plan, bindings, seeds, request, prior, 1, true, state, diagnostic) !=
		Status::Ok
	);
	CHECK(state.Sources[0].Data.Pixels == seeds[1].Data.Pixels);
	bindings[1].OutputId = "left";
	CHECK(
		ReplayFeedbackFrame(
			document, plan, bindings, seeds, request, prior, 1, true, state, diagnostic, 64
		) == Status::LimitExceeded
	);
	request.Subframe = .5;
	CHECK(
		ReplayFeedbackFrame(document, plan, bindings, seeds, request, prior, 1, true, state, diagnostic) ==
		Status::InvalidValue
	);
	request.Subframe = 0;
	bindings[1].SourceId = "a";
	CHECK(
		ReplayFeedbackFrame(document, plan, bindings, seeds, request, prior, 1, true, state, diagnostic) ==
		Status::DuplicateId
	);
}
