#include <engine/imagegraph/PendingHostObservations.hpp>
#include <engine/imagegraph/SourceTimeline.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.source_timeline")
namespace {
	using namespace engine::imagegraph;
	SourceAuthoringFrameBound Explicit(double value) {
		FrameTime at;
		REQUIRE(SplitFrameTime(value, at, false, Limits::MaximumTick + 1));
		return {SourceFrameBoundPresence::Explicit, at};
	}
	Document Graph() {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {{"number", "value.number", "", {}, {{"value", 7.}}}};
		document.Outputs = {{"out", "number", "number"}};
		document.Timeline = TimelineSettings{2, 0, 1, "loop", 30};
		return document;
	}
}
TEST_CASE(
	"Source timeline retains independent saved endpoints through native codec and playback projection",
	"[imagegraph][source_timeline]"
) {
	auto original = Graph();
	original.Timeline->SourceBounds =
		SourceAuthoringFrameBounds{{SourceFrameBoundPresence::Null, {}}, Explicit(12)};
	Diagnostic diagnostic;
	REQUIRE(ProjectSourceTimelineWindow(*original.Timeline, diagnostic) == Status::Ok);
	CHECK(original.Timeline->Last == 1);
	CHECK(SourceTimelineLastFrame(*original.Timeline) == 11.);
	const auto text = Write(original);
	REQUIRE(text.find("source_timeline_bounds null explicit 12 0 0") != std::string::npos);
	Document parsed;
	REQUIRE(Read(text, parsed, diagnostic) == Status::Ok);
	CHECK(parsed == original);
	Plan plan;
	REQUIRE(Compile(parsed, plan, diagnostic) == Status::Ok);
	EvaluatedValue number;
	REQUIRE(EvaluateValue(parsed, plan, "out", {}, number, diagnostic) == Status::Ok);
	CHECK(std::get<double>(number.Data) == 7.);
	auto normalized = *parsed.Timeline;
	REQUIRE(NormalizeSourceTimelineBounds(normalized, diagnostic) == Status::Ok);
	CHECK(normalized.SourceBounds->Start == Explicit(0));
	CHECK(normalized.SourceBounds->End == Explicit(2));
	CHECK(SourceTimelineFirstFrame(normalized) == -1.);
	CHECK(SourceTimelineLastFrame(normalized) == 1.);
	CHECK(parsed.Timeline->SourceBounds->End == Explicit(12));
	const auto old = Graph();
	Document legacy;
	REQUIRE(Read(Write(old), legacy, diagnostic) == Status::Ok);
	CHECK_FALSE(legacy.Timeline->SourceBounds);
	CHECK(SourceTimelineFirstFrame(*legacy.Timeline) == 0.);
	CHECK(SourceTimelineLastFrame(*legacy.Timeline) == 1.);
}
TEST_CASE(
	"Source step preserves truthy gate and clears equal values independently of source presence",
	"[imagegraph][source_timeline]"
) {
	Diagnostic diagnostic;
	auto timeline = *Graph().Timeline;
	timeline.SourceBounds = SourceAuthoringFrameBounds{Explicit(0), Explicit(0)};
	REQUIRE(NormalizeSourceTimelineBounds(timeline, diagnostic) == Status::Ok);
	CHECK(timeline.SourceBounds->Start == Explicit(0));
	CHECK(timeline.SourceBounds->End == Explicit(0));
	CHECK(SourceTimelineFirstFrame(timeline) == -1.);
	timeline.SourceBounds = SourceAuthoringFrameBounds{Explicit(1.5), Explicit(1.5)};
	REQUIRE(NormalizeSourceTimelineBounds(timeline, diagnostic) == Status::Ok);
	CHECK(timeline.SourceBounds->Start.Presence == SourceFrameBoundPresence::Null);
	CHECK(timeline.SourceBounds->End.Presence == SourceFrameBoundPresence::Null);
	CHECK(SourceTimelineLastFrame(timeline) == 1.);
	timeline.SourceBounds = SourceAuthoringFrameBounds{Explicit(-2.5), Explicit(-4)};
	REQUIRE(NormalizeSourceTimelineBounds(timeline, diagnostic) == Status::Ok);
	CHECK(timeline.SourceBounds->Start == Explicit(0));
	CHECK(timeline.SourceBounds->End == Explicit(-2.5));
	CHECK(SourceTimelineLastFrame(timeline) == -3.5);
	timeline.SourceBounds = SourceAuthoringFrameBounds{};
	CHECK(SourceTimelineFirstFrame(timeline, 4.) == 3.);
	CHECK(SourceTimelineLastFrame(timeline, 5.5) == 4.5);
	timeline.SourceBounds->Start = Explicit(-1.5);
	CHECK(SourceTimelineFirstFrame(timeline, 4.) == -2.5);
}
TEST_CASE(
	"Source native codec rejects malformed duplicate and older-version bounds atomically",
	"[imagegraph][source_timeline]"
) {
	const auto original = Graph();
	const auto base = Write(original);
	Diagnostic diagnostic;
	for (const auto &line :
		 {"source_timeline_bounds missing null extra\n",
		  "source_timeline_bounds explicit 1 1 0 null\n",
		  "source_timeline_bounds explicit 0 0 1 null\n",
		  "source_timeline_bounds strange null\n",
		  "source_timeline_bounds missing null\nsource_timeline_bounds null missing\n"}) {
		auto result = original;
		CHECK(Read(base + line, result, diagnostic) != Status::Ok);
		CHECK(result == original);
	}
	auto result = original;
	CHECK(Read("imagegraph 9\nsource_timeline_bounds missing null\n", result, diagnostic) != Status::Ok);
	CHECK(result == original);
	auto old = original;
	old.FormatVersion = 8;
	old.Timeline->SourceBounds = SourceAuthoringFrameBounds{};
	CHECK(Write(old).empty());
	Plan plan;
	CHECK(Compile(old, plan, diagnostic) == Status::UnsupportedVersion);
	auto malformed = original;
	malformed.Timeline->SourceBounds = SourceAuthoringFrameBounds{};
	malformed.Timeline->SourceBounds->Start.Value.Tick = 1;
	CHECK_FALSE(DocumentRetainedPayloadBytes(malformed));
	CHECK(Write(malformed).empty());
	CHECK(Compile(malformed, plan, diagnostic) == Status::InvalidValue);
}
TEST_CASE(
	"Pending host timeline bounds identity prevents a repeated effect after metadata change",
	"[imagegraph][source_timeline]"
) {
	struct Capability final : HostNodeProvider {
		unsigned Calls = 0;
		bool Capture(const HostNodeInvocation &invocation, HostNodeCapture &output, std::string &) override {
			Diagnostic diagnostic;
			uint64_t held = 0;
			if (PrepareResolvedHostCapture(
					invocation, invocation.MaximumOperationBytes, output, held, diagnostic
				) != Status::Ok)
				return false;
			++Calls;
			output.Outputs.push_back({"content", std::string("observed")});
			return true;
		}
	} capability;
	Node node{"read", "pc.text_file_read", "", {}, {{"path", std::string("owned.txt")}}};
	EvaluationRequest request;
	auto timeline = *Graph().Timeline;
	timeline.SourceBounds = SourceAuthoringFrameBounds{};
	HostNodeInvocation invocation{node, request, node.Values, {}, 1024 * 1024};
	invocation.Timeline = &timeline;
	PendingHostObservations observations;
	HostNodeCapture capture;
	std::string failure;
	observations.BeginAttempt();
	REQUIRE(observations.CaptureSequenced(invocation, capability, capture, failure));
	CHECK(capability.Calls == 1);
	observations.BeginAttempt();
	timeline.SourceBounds->End = Explicit(12);
	CHECK_FALSE(observations.CaptureSequenced(invocation, capability, capture, failure));
	CHECK(capability.Calls == 1);
	CHECK(capture.Outputs[0].Data == Value{std::string("observed")});
}
