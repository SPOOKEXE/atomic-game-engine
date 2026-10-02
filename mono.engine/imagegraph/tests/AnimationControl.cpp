#include <engine/imagegraph/AnimationControl.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>
TEST_SUITE_ID("engine.imagegraph.animation_control")
using namespace engine::imagegraph;
TEST_CASE(
	"Animation control resolves linked controls outside selected output "
	"cone transactionally",
	"[imagegraph][animation_control]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"trigger", "value.boolean", "", {}, {{"value", true}}},
		{"control", "pc.animation_control", "", {}, {{"skip_frames_count", int64_t{-4}}}},
		{"out", "value.number", "", {}, {{"value", 1.}}}
	};
	document.Links = {{"trigger", "boolean", "control", "skip_frames"}};
	document.Outputs = {{"number", "out", "number"}};
	Document parsed;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(document), parsed, diagnostic) == Status::Ok);
	Plan plan;
	const auto compiled = Compile(parsed, plan, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(compiled == Status::Ok);
	AnimationPlaybackState prior;
	prior.CurrentFrame = 2;
	prior.RealFrame = 2;
	AnimationControlResult result;
	EvaluationRequest request;
	request.Tick = 11;
	request.Subframe = .5;
	const auto resolved =
		ResolveAnimationControl(parsed, plan, "control", request, prior, 100000, result, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(resolved == Status::Ok);
	CHECK(result.Playback.CurrentFrame == -2);
	CHECK(result.EffectCount == 1);
	EvaluationSnapshot snapshot;
	REQUIRE(EvaluateNodeInputs(parsed, plan, "control", request, snapshot, diagnostic) == Status::Ok);
	parsed.Nodes.front().Values.front().Data = false;
	AnimationControlResult reused;
	REQUIRE(ResolveAnimationControl(snapshot, "control", prior, 100000, reused, diagnostic) == Status::Ok);
	CHECK(reused == result);
	const auto retained = result;
	CHECK(
		ResolveAnimationControl(parsed, plan, "control", request, prior, 1, result, diagnostic) ==
		Status::LimitExceeded
	);
	CHECK(result == retained);
	CHECK(
		ResolveAnimationControl(parsed, plan, "out", request, prior, 100000, result, diagnostic) ==
		Status::InvalidValue
	);
	CHECK(result == retained);
}
TEST_CASE(
	"Animation controls preserve overlapping command order and callback clock",
	"[imagegraph][animation_control]"
) {
	AnimationPlaybackState prior;
	prior.CurrentFrame = 9;
	prior.RealFrame = 9;
	prior.LastTime = .5;
	prior.RealTime = 4;
	prior.Direction = -1;
	prior.FrameRangeStart = 4;
	prior.SelectionFrameStart = 8;
	AnimationControlInputs inputs;
	inputs.PlayPause = inputs.Pause = inputs.Resume = inputs.PlayFromStart = inputs.PlayOnce =
		inputs.SkipFrames = true;
	inputs.SkipFramesCount = -5;
	AnimationControlResult result;
	Diagnostic diagnostic;
	REQUIRE(BuildAnimationControl(inputs, prior, result, diagnostic) == Status::Ok);
	CHECK(result.Playback.Playing);
	CHECK(result.Playback.Rendering);
	CHECK(result.Playback.CurrentFrame == -2);
	CHECK(result.Playback.RealFrame == -2);
	CHECK(result.Playback.Direction == 1);
	CHECK(result.Playback.RealTime == 0);
	REQUIRE(result.EffectCount == 5);
	CHECK(result.Effects[0].Kind == AnimationControlEffectKind::RenderAll);
	CHECK(result.Effects[0].Playback.CurrentFrame == 3);
	CHECK(result.Effects[1].Kind == AnimationControlEffectKind::AnimationStart);
	CHECK_FALSE(result.Effects[1].Playback.Playing);
	CHECK(result.Effects[2].Kind == AnimationControlEffectKind::RenderAll);
	CHECK(result.Effects[2].Playback.Playing);
	CHECK_FALSE(result.Effects[2].Playback.Rendering);
	CHECK(result.Effects[3].Kind == AnimationControlEffectKind::RenderingStart);
	CHECK(result.Effects[3].Playback.Rendering);
	CHECK(result.Effects[4].Kind == AnimationControlEffectKind::RenderAll);
	CHECK(result.Effects[4].Playback.CurrentFrame == -2);
}
TEST_CASE(
	"Animation controls suppress commands during render and preserve "
	"repeated levels",
	"[imagegraph][animation_control]"
) {
	AnimationPlaybackState prior;
	prior.Rendering = true;
	prior.Playing = true;
	prior.CurrentFrame = 7;
	prior.RealFrame = 7;
	AnimationControlInputs inputs;
	inputs.PlayPause = inputs.Pause = inputs.Resume = inputs.PlayFromStart = inputs.PlayOnce =
		inputs.SkipFrames = true;
	AnimationControlResult result;
	Diagnostic diagnostic;
	REQUIRE(BuildAnimationControl(inputs, prior, result, diagnostic) == Status::Ok);
	CHECK(result.Playback == prior);
	CHECK(result.EffectCount == 0);
	prior.Rendering = false;
	inputs = {};
	inputs.PlayPause = true;
	REQUIRE(BuildAnimationControl(inputs, prior, result, diagnostic) == Status::Ok);
	CHECK_FALSE(result.Playback.Playing);
	REQUIRE(BuildAnimationControl(inputs, result.Playback, result, diagnostic) == Status::Ok);
	CHECK(result.Playback.Playing);
	CHECK(result.EffectCount == 0);
}
TEST_CASE(
	"Animation restarts use range precedence simulation zero and half "
	"even clock",
	"[imagegraph][animation_control]"
) {
	AnimationPlaybackState prior;
	prior.CurrentFrame = 12;
	prior.RealFrame = 12;
	prior.SelectionFrameStart = -1.5;
	AnimationControlInputs inputs;
	inputs.PlayFromStart = true;
	AnimationControlResult result;
	Diagnostic diagnostic;
	REQUIRE(BuildAnimationControl(inputs, prior, result, diagnostic) == Status::Ok);
	CHECK(result.Playback.CurrentFrame == -2);
	CHECK(result.Playback.RealFrame == -2.5);
	prior.FrameRangeStart = 4.5;
	REQUIRE(BuildAnimationControl(inputs, prior, result, diagnostic) == Status::Ok);
	CHECK(result.Playback.CurrentFrame == 4);
	CHECK(result.Playback.RealFrame == 3.5);
	prior.Simulating = true;
	REQUIRE(BuildAnimationControl(inputs, prior, result, diagnostic) == Status::Ok);
	CHECK(result.Playback.CurrentFrame == 0);
	CHECK(result.Playback.RealFrame == 0);
	REQUIRE(result.EffectCount == 4);
	CHECK(result.Effects[0].Playback.CurrentFrame == 4);
	CHECK(result.Effects[1].Playback.CurrentFrame == 0);
	CHECK(result.Effects[2].Kind == AnimationControlEffectKind::AnimationStart);
	const auto unchanged = result;
	inputs.SkipFrames = true;
	inputs.SkipFramesCount = std::numeric_limits<int64_t>::max();
	REQUIRE(BuildAnimationControl(inputs, prior, result, diagnostic) == Status::LimitExceeded);
	CHECK(result == unchanged);
}
