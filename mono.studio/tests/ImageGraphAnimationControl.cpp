#include "ImageGraphAnimationControl.hpp"

#include "ImageGraphPreview.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>

TEST_SUITE_ID("studio.imagegraph_animation_control")
TEST_DEPENDS("studio.imagegraph")
namespace {
	using namespace engine::imagegraph;
	using namespace studio;
	Document SimulationGraph() {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {
			{"owner",
			 "pc.verlet_sim_inline",
			 "",
			 {},
			 {{"dimension_unit", EnumValue{0}},
			  {"dimension", Vector2{4, 4}},
			  {"gravity", Vector2{0, 10}},
			  {"substep", int64_t{1}},
			  {"wall", int64_t{0}}}},
			{"grid",
			 "pc.verlet_sim_mesh_grid",
			 "scope",
			 {},
			 {{"subdivision", Vector2{1, 1}}, {"area_unit", EnumValue{0}}, {"area", Area{2, 2, 2, 2}}}},
			{"render", "pc.verlet_sim_render", "scope", {}, {{"type", EnumValue{0}}}},
			{"cache", "pc.verlet_sim_mesh_cache", "scope", {}, {}}
		};
		Group group{"scope", "scope"};
		group.OwnerNodeId = "owner";
		document.Groups = {group};
		document.Links = {{"grid", "mesh", "render", "mesh"}, {"grid", "mesh", "cache", "mesh"}};
		document.Outputs = {{"image", "render", "surface_out"}};
		return document;
	}
} // namespace
TEST_CASE(
	"animation control changes the existing owner and callbacks see "
	"intermediate clocks",
	"[studio][animation_control_host]"
) {
	ImageGraphPlayback owner;
	owner.CurrentTick = 9;
	owner.RealFrame = 9;
	owner.StartTick = 3;
	owner.EndTick = 12;
	owner.Accumulator = .01;
	owner.LastTime = .2;
	owner.RealTime = 4;
	owner.Direction = -1;
	AnimationControlInputs inputs;
	inputs.PlayFromStart = true;
	inputs.SkipFrames = true;
	inputs.SkipFramesCount = -5;
	AnimationControlResult result;
	Diagnostic diagnostic;
	REQUIRE(
		BuildAnimationControl(inputs, detail::AnimationPlayback(owner), result, diagnostic) == Status::Ok
	);
	std::vector<std::pair<AnimationControlEffectKind, double>> events;
	REQUIRE(
		detail::ApplyAnimationControl(
			owner,
			result,
			[&](auto kind) { events.emplace_back(kind, double(FrameTimeToReal(GetImageGraphFrame(owner)))); },
			diagnostic
		)
	);
	REQUIRE(events.size() == 4);
	CHECK(events[0].second == 3);
	CHECK(events[1].second == 3);
	CHECK(events[2].second == 3);
	CHECK(events[3].second == -2);
	CHECK(owner.Playing);
	CHECK(owner.Direction == 1);
	CHECK(owner.Accumulator == 0);
	CHECK(owner.LastTime == 0);
	CHECK(owner.RealTime == 0);
	CHECK(GetImageGraphFrame(owner) == FrameTime{2, 0, true});
	const auto priorFrame = GetImageGraphFrame(owner);
	result.Effects[0].Playback.CurrentFrame = double(Limits::MaximumTick) + 1;
	const auto count = events.size();
	CHECK_FALSE(
		detail::ApplyAnimationControl(
			owner, result, [&](auto kind) { events.emplace_back(kind, 0); }, diagnostic
		)
	);
	CHECK(GetImageGraphFrame(owner) == priorFrame);
	CHECK(events.size() == count);
}
TEST_CASE(
	"render once traverses its complete range and restores authored loop "
	"options",
	"[studio][animation_control_host]"
) {
	ImageGraphPlayback owner;
	owner.TotalFrames = 6;
	owner.StartTick = 3;
	owner.EndTick = 5;
	owner.CurrentTick = 4;
	owner.RealFrame = 4;
	owner.Loop = true;
	owner.PingPong = true;
	owner.FramesPerSecond = 10;
	AnimationControlInputs inputs;
	inputs.PlayOnce = true;
	AnimationControlResult result;
	Diagnostic diagnostic;
	REQUIRE(
		BuildAnimationControl(inputs, detail::AnimationPlayback(owner), result, diagnostic) == Status::Ok
	);
	REQUIRE(detail::ApplyAnimationControl(owner, result, [](auto) {}, diagnostic));
	CHECK(owner.Rendering);
	CHECK(owner.Playing);
	CHECK(owner.CurrentTick == 3);
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {{"number", "value.number", "", {}, {{"value", 0.0}}}};
	document.Outputs = {{"out", "number", "number"}};
	document.Keyframes = {{"number", "value", 3, 3.0, "linear"}, {"number", "value", 5, 5.0, "linear"}};
	Plan plan;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	std::vector<double> rendered;
	const auto render = [&] {
		EvaluationRequest request;
		(void)SetFrameTime(request, GetImageGraphFrame(owner));
		studio::ImageGraphPreviewValue preview;
		REQUIRE(
			studio::EvaluateImageGraphPreview(document, plan, "out", request, preview, diagnostic) ==
			Status::Ok
		);
		REQUIRE(std::holds_alternative<EvaluatedValue>(preview));
		rendered.push_back(std::get<double>(std::get<EvaluatedValue>(preview).Data));
	};
	render();
	std::vector<uint64_t> frames{owner.CurrentTick};
	for (int i = 0; i < 3; ++i) {
		REQUIRE(detail::AdvanceAnimationPlayback(owner, .1));
		if (owner.Playing) {
			frames.push_back(owner.CurrentTick);
			render();
		}
	}
	CHECK(frames == std::vector<uint64_t>{3, 4, 5});
	CHECK(rendered == std::vector<double>{3, 4, 5});
	CHECK_FALSE(owner.Rendering);
	CHECK_FALSE(owner.Playing);
	CHECK(owner.CurrentTick == 5);
	CHECK(owner.Loop);
	CHECK(owner.PingPong);
	CHECK(owner.LastTime == 0);
}
TEST_CASE(
	"animation start resets real simulation replay and cached frames "
	"before rendering tick zero",
	"[studio][animation_control_host]"
) {
	const auto document = SimulationGraph();
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	CapturedFeedbackHost fresh;
	EvaluationRequest first;
	first.Seed = 42;
	REQUIRE(fresh.Prepare(document, plan, 1, 1, first, diagnostic, Limits::MaximumEvaluationBytes, "image"));
	REQUIRE(first.SimulationReplay);
	const auto freshGrid = std::find_if(
		first.SimulationReplay->Entries.begin(),
		first.SimulationReplay->Entries.end(),
		[](const auto &entry) { return entry.NodeId == "grid"; }
	);
	REQUIRE(freshGrid != first.SimulationReplay->Entries.end());
	const auto firstState = freshGrid->State;
	// The source inline renderer performs its first solver step at frame zero.
	for (const auto &point : firstState.Mesh.Points) {
		CHECK(point.Position.X == Catch::Approx(point.Original.X).epsilon(0).margin(1e-12));
		CHECK(point.Position.Y == Catch::Approx(point.Original.Y + 1).epsilon(0).margin(1e-12));
	}
	CapturedFeedbackHost replay;
	EvaluationRequest request;
	request.Tick = 3;
	request.Seed = 42;
	const std::array<std::string_view, 1> capture{"cache"};
	request.SimulationCacheCaptures = capture;
	REQUIRE(
		replay.Prepare(document, plan, 1, 1, request, diagnostic, Limits::MaximumEvaluationBytes, "image")
	);
	REQUIRE(request.SimulationReplay);
	REQUIRE_FALSE(request.SimulationReplay->Entries.empty());
	auto grid = std::find_if(
		request.SimulationReplay->Entries.begin(), request.SimulationReplay->Entries.end(), [](auto &entry) {
			return entry.NodeId == "grid";
		}
	);
	REQUIRE(grid != request.SimulationReplay->Entries.end());
	REQUIRE_FALSE(grid->State.Mesh.Points.empty());
	CHECK(grid->State.Mesh.Points[0].Position != grid->State.Mesh.Points[0].Original);
	const auto captured = std::find_if(
		request.SimulationReplay->Entries.begin(),
		request.SimulationReplay->Entries.end(),
		[](const auto &entry) { return entry.NodeId == "cache"; }
	);
	REQUIRE(captured != request.SimulationReplay->Entries.end());
	REQUIRE(captured->Cache);
	const auto manualCache = *captured->Cache;
	ImageGraphPreviewCache cache;
	REQUIRE(replay.Output("image"));
	REQUIRE(cache.Store(1, 0, 3, *replay.Output("image")));
	REQUIRE(cache.HeldBytes() > 0);
	ImageGraphPlayback owner;
	owner.CurrentTick = 3;
	owner.RealFrame = 3;
	owner.StartTick = 2;
	owner.EndTick = 5;
	owner.Simulating = true;
	AnimationControlInputs inputs;
	inputs.PlayFromStart = true;
	AnimationControlResult result;
	REQUIRE(
		BuildAnimationControl(inputs, detail::AnimationPlayback(owner), result, diagnostic) == Status::Ok
	);
	bool restarted = false;
	REQUIRE(
		detail::ApplyAnimationControl(
			owner,
			result,
			[&](auto kind) {
				if (kind == AnimationControlEffectKind::AnimationStart) {
					detail::RestartAnimationReplay(replay, cache);
					restarted = true;
					CHECK(cache.HeldBytes() == 0);
				}
				if (kind == AnimationControlEffectKind::RenderAll && restarted) {
					EvaluationRequest clock;
					clock.Seed = 42;
					(void)SetFrameTime(clock, GetImageGraphFrame(owner));
					REQUIRE(replay.Prepare(
						document, plan, 1, 1, clock, diagnostic, Limits::MaximumEvaluationBytes, "image"
					));
					REQUIRE(clock.SimulationReplay);
					const auto retained = std::find_if(
						clock.SimulationReplay->Entries.begin(),
						clock.SimulationReplay->Entries.end(),
						[](const auto &entry) { return entry.NodeId == "cache"; }
					);
					REQUIRE(retained != clock.SimulationReplay->Entries.end());
					CHECK(retained->Cache == std::optional{manualCache});
					const auto resetGrid = std::find_if(
						clock.SimulationReplay->Entries.begin(),
						clock.SimulationReplay->Entries.end(),
						[](const auto &entry) { return entry.NodeId == "grid"; }
					);
					REQUIRE(resetGrid != clock.SimulationReplay->Entries.end());
					CHECK(resetGrid->State == firstState);
				}
			},
			diagnostic
		)
	);
	REQUIRE(restarted);
	CHECK(owner.CurrentTick == 0);
	CHECK(owner.Playing);
	REQUIRE(detail::AdvanceAnimationPlayback(owner, 1. / owner.FramesPerSecond));
	CHECK(owner.CurrentTick == 1);
}
