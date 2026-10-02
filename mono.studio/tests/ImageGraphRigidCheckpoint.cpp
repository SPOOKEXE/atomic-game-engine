#include "../../mono.engine/imagegraphphysics/tests/RigidGraphFixture.hpp"
#include "../src/ImageGraphRigid.hpp"

#include <engine/imagegraph/FeedbackHost.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("studio.imagegraph_rigid_checkpoint52")
TEST_DEPENDS("engine.imagegraphphysics.rigid_graph")

namespace {
	using namespace engine::imagegraph;
	Document MixedRigidTexture() {
		auto document = engine::imagegraphphysics::testing::RigidGraphFixture();
		document.Nodes.push_back(
			{"sim",
			 "pc.verlet_sim_inline",
			 "",
			 {},
			 {{"dimension_unit", EnumValue{0}},
			  {"dimension", Vector2{8, 8}},
			  {"gravity", Vector2{0, 10}},
			  {"substep", int64_t{1}},
			  {"wall", int64_t{0}}}}
		);
		document.Nodes.push_back(
			{"grid",
			 "pc.verlet_sim_mesh_grid",
			 "sim-scope",
			 {},
			 {{"subdivision", Vector2{1, 1}}, {"area_unit", EnumValue{0}}, {"area", Area{2, 2, 2, 2}}}}
		);
		document.Nodes.push_back(
			{"push",
			 "pc.verlet_sim_force",
			 "sim-scope",
			 {},
			 {{"area_unit", EnumValue{0}},
			  {"area", Area{4, 4, 4, 4}},
			  {"push", Vector2{1, 0}},
			  {"strength", 1.}}}
		);
		document.Nodes.push_back(
			{"sim-render", "pc.verlet_sim_render", "sim-scope", {}, {{"type", EnumValue{0}}}}
		);
		Group group{"sim-scope", "Verlet texture"};
		group.OwnerNodeId = "sim";
		document.Groups.push_back(group);
		std::erase_if(document.Links, [](const auto &link) {
			return link.ToNode == "body" && link.ToPort == "texture";
		});
		document.Links.push_back({"grid", "mesh", "push", "mesh"});
		document.Links.push_back({"push", "mesh", "sim-render", "mesh"});
		document.Links.push_back({"sim-render", "surface_out", "body", "texture"});
		return document;
	}
	Image Preview(
		CapturedFeedbackHost &host,
		const Document &document,
		const Plan &plan,
		const studio::ImageGraphPlayback &playback,
		RigidReplayState *journal = nullptr,
		SimulationReplayState *simulation = nullptr
	) {
		engine::imagegraphphysics::RigidProvider provider;
		EvaluationRequest request;
		studio::detail::BindImageGraphRigid(request, provider, playback);
		REQUIRE(
			SetFrameTime(request, FrameTime{playback.CurrentTick, playback.Subframe, playback.NegativeFrame})
		);
		Diagnostic diagnostic;
		const auto prepared =
			host.Prepare(document, plan, 19, 1, request, diagnostic, Limits::MaximumEvaluationBytes, "image");
		INFO(diagnostic.NodeId << ':' << diagnostic.Port << ' ' << diagnostic.Message);
		REQUIRE(prepared);
		const auto *output = host.Output("image");
		REQUIRE(output);
		REQUIRE(request.RigidReplay);
		if (journal) *journal = *request.RigidReplay;
		if (simulation) {
			REQUIRE(request.SimulationReplay);
			*simulation = *request.SimulationReplay;
		}
		return *output;
	}
} // namespace

TEST_CASE(
	"frame-start replacement preserves explicit Cache Mesh controls and "
	"refusal transaction",
	"[studio][checkpoint52]"
) {
	auto document = MixedRigidTexture();
	document.Nodes.push_back({"cache", "pc.verlet_sim_mesh_cache", "sim-scope", {}, {}});
	document.Links.push_back({"push", "mesh", "cache", "mesh"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	CapturedFeedbackHost host;
	engine::imagegraphphysics::RigidProvider provider;
	EvaluationRequest request;
	request.RigidProvider = &provider;
	request.RigidFrameProgress = true;
	const std::array<std::string_view, 1> action{"cache"};
	request.SimulationCacheCaptures = action;
	const auto run = [&](uint64_t budget = Limits::MaximumEvaluationBytes) {
		return host.Prepare(document, plan, 19, 1, request, diagnostic, budget, "image");
	};
	REQUIRE(run());
	const auto cacheValue = [&] {
		REQUIRE(request.SimulationReplay);
		const auto found = std::find_if(
			request.SimulationReplay->Entries.begin(),
			request.SimulationReplay->Entries.end(),
			[](const auto &entry) { return entry.NodeId == "cache" && entry.ProcessorRow == 0; }
		);
		REQUIRE(found != request.SimulationReplay->Entries.end());
		REQUIRE(found->Cache);
		return *found;
	};
	const auto manual = cacheValue();
	request.SimulationCacheCaptures = {};
	request.RigidPlaying = true;
	REQUIRE(run());
	CHECK(cacheValue() == manual);
	const auto played = *host.Output("image");
	const auto simulation = *request.SimulationReplay;
	const auto rigid = *request.RigidReplay;
	request.RigidPlaying = false;
	CHECK_FALSE(run(1));
	CHECK(diagnostic.Code == Status::LimitExceeded);
	CHECK(*host.Output("image") == played);
	CHECK(*request.SimulationReplay == simulation);
	CHECK(*request.RigidReplay == rigid);
	REQUIRE(run());
	CHECK(cacheValue() == manual);
	request.RigidPlaying = true;
	REQUIRE(run());
	CHECK(*host.Output("image") == played);
	CHECK(cacheValue() == manual);
	// A later manual action changes the control payload once. Subsequent refresh
	// keeps that deliberate capture even though the frame-start snapshot predates
	// it.
	request.Tick = 1;
	request.SimulationCacheCaptures = action;
	REQUIRE(run());
	const auto later = cacheValue();
	REQUIRE(later.Cache != manual.Cache);
	request.SimulationCacheCaptures = {};
	request.RigidPlaying = false;
	REQUIRE(run());
	CHECK(cacheValue() == later);
	request.RigidPlaying = true;
	REQUIRE(run());
	CHECK(cacheValue() == later);
}

TEST_CASE("frame-start rigid refresh keeps the feedback input generation", "[studio][checkpoint52]") {
	auto document = engine::imagegraphphysics::testing::RigidGraphFixture();
	document.Nodes.push_back(
		{"previous", "image.captured", "", {}, {{"source_id", std::string{"feedback:feedback"}}}}
	);
	document.Nodes.push_back({"inverted", "image.invert", "", {}, {{"include_alpha", true}}});
	document.Links.push_back({"previous", "image", "inverted", "image"});
	document.Outputs.push_back({"feedback", "inverted", "image"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	CapturedFeedbackHost host;
	studio::ImageGraphPlayback owner;
	owner.FrameProgress = true;
	(void)Preview(host, document, plan, owner);
	REQUIRE(host.Output("feedback"));
	const auto first = *host.Output("feedback");
	CHECK(first.Pixels[0] == 255);
	owner.Playing = true;
	(void)Preview(host, document, plan, owner);
	CHECK(*host.Output("feedback") == first);
	owner.Playing = false;
	(void)Preview(host, document, plan, owner);
	CHECK(*host.Output("feedback") == first);
	owner.CurrentTick = 1;
	(void)Preview(host, document, plan, owner);
	const auto second = *host.Output("feedback");
	CHECK(second.Pixels[0] == 0);
	owner.Playing = true;
	(void)Preview(host, document, plan, owner);
	CHECK(*host.Output("feedback") == second);
	owner.Playing = false;
	(void)Preview(host, document, plan, owner);
	CHECK(*host.Output("feedback") == second);
}

TEST_CASE("fractional rigid replacement preserves the integer frame-start prefix", "[studio][checkpoint52]") {
	const auto document = engine::imagegraphphysics::testing::RigidGraphFixture();
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	CapturedFeedbackHost host;
	studio::ImageGraphPlayback owner;
	owner.FrameProgress = true;
	RigidReplayState prefix, actual;
	(void)Preview(host, document, plan, owner, &prefix);
	owner.CurrentTick = 1;
	for (const auto &observation : {std::pair{.25, true}, std::pair{.75, false}, std::pair{.5, true}}) {
		owner.Subframe = observation.first;
		owner.Playing = observation.second;
		engine::imagegraphphysics::RigidProvider provider;
		EvaluationRequest request;
		studio::detail::BindImageGraphRigid(request, provider, owner);
		REQUIRE(SetFrameTime(request, FrameTime{owner.CurrentTick, owner.Subframe, owner.NegativeFrame}));
		request.RigidReplay = &prefix;
		request.RigidAuthoringRevision = 19;
		StatefulEvaluationResult expected;
		REQUIRE(EvaluateStateful(document, plan, "image", request, expected, diagnostic) == Status::Ok);
		CHECK(Preview(host, document, plan, owner, &actual) == std::get<Image>(expected.Output));
		CHECK(actual == expected.Rigid);
		REQUIRE(actual.Owners.size() == 1);
		REQUIRE(actual.Owners[0].History.Frames.size() == 2);
		CHECK(actual.Owners[0].History.Frames.front() == prefix.Owners[0].History.Frames.front());
	}
}
