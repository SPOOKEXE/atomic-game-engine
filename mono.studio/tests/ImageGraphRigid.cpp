#include "ImageGraphRigid.hpp"

#include "../../mono.engine/imagegraphphysics/tests/RigidGraphFixture.hpp"

#include <engine/imagegraph/FeedbackHost.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("studio.imagegraph_rigid")
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
	Document MixedRigidFluidTexture() {
		auto document = engine::imagegraphphysics::testing::RigidGraphFixture();
		document.Nodes.push_back(
			{"fluid",
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
			  {"time_step", .1}}}
		);
		document.Nodes.push_back(
			{"fill",
			 "pc.flip_fill",
			 "",
			 {},
			 {{"spawn_area_unit", EnumValue{0}}, {"spawn_area", Area{8, 8, 4, 4}}, {"density", .5}}}
		);
		document.Nodes.push_back(
			{"fluid-render",
			 "pc.flip_render",
			 "",
			 {},
			 {{"update_step", int64_t{1}},
			  {"particle_size", 2.},
			  {"draw_obstracles", false},
			  {"threshold", false},
			  {"alpha", Vector2{1, 1}},
			  {"lifespan", Vector2{0, 0}}}}
		);
		std::erase_if(document.Links, [](const auto &l) {
			return l.ToNode == "body" && l.ToPort == "texture";
		});
		document.Links.push_back({"fluid", "domain", "fill", "domain"});
		document.Links.push_back({"fill", "domain", "fluid-render", "domain"});
		document.Links.push_back({"fluid-render", "rendered", "body", "texture"});
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
		REQUIRE(SetFrameTime(request, studio::GetImageGraphFrame(playback)));
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
}

TEST_CASE(
	"Studio rigid same-frame pause and play observes the existing playback owner", "[studio][rigid_host]"
) {
	const auto document = engine::imagegraphphysics::testing::RigidGraphFixture();
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	studio::ImageGraphPlayback owner;
	owner.FrameProgress = true;
	CapturedFeedbackHost retained;
	const auto paused = Preview(retained, document, plan, owner);
	studio::ImageGraphPreviewCache cache;
	const auto pausedObservation = studio::detail::ImageGraphPlaybackObservation(document, owner);
	REQUIRE(pausedObservation == 2);
	REQUIRE(cache.Store(19, 0, 0, paused, 0, false, pausedObservation));
	REQUIRE(cache.Find(19, 0, 0, 0, false, pausedObservation));
	CHECK(*cache.Find(19, 0, 0, 0, false, pausedObservation) == paused);
	owner.Playing = true;
	const auto playedObservation = studio::detail::ImageGraphPlaybackObservation(document, owner);
	REQUIRE(playedObservation == 3);
	CHECK_FALSE(cache.Find(19, 0, 0, 0, false, playedObservation));
	CapturedFeedbackHost independent;
	const auto played = Preview(independent, document, plan, owner);
	REQUIRE(paused.Pixels != played.Pixels);
	CHECK(Preview(retained, document, plan, owner) == played);
	REQUIRE(cache.Store(19, 0, 0, played, 0, false, playedObservation));
	REQUIRE(cache.Find(19, 0, 0, 0, false, playedObservation));
	CHECK(*cache.Find(19, 0, 0, 0, false, playedObservation) == played);
	const auto bytes = cache.HeldBytes();
	CHECK_FALSE(cache.Store(19, 0, 0, paused, 0, false, 4));
	CHECK_FALSE(cache.Find(19, 0, 0, 0, false, 4));
	CHECK(cache.HeldBytes() == bytes);
	owner.FrameProgress = false;
	CHECK_FALSE(
		cache.Find(19, 0, 0, 0, false, studio::detail::ImageGraphPlaybackObservation(document, owner))
	);
	owner.FrameProgress = true;
	owner.Playing = false;
	REQUIRE(cache.Find(19, 0, 0, 0, false, studio::detail::ImageGraphPlaybackObservation(document, owner)));
	CHECK(
		*cache.Find(19, 0, 0, 0, false, studio::detail::ImageGraphPlaybackObservation(document, owner)) ==
		paused
	);
	CHECK(Preview(retained, document, plan, owner) == paused);
}

TEST_CASE(
	"Studio rigid host owns journals across stack providers and repeated seeks", "[studio][rigid_host]"
) {
	const auto document = engine::imagegraphphysics::testing::RigidGraphFixture();
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	studio::ImageGraphPlayback owner;
	owner.Playing = owner.FrameProgress = true;
	CapturedFeedbackHost host;
	const auto initial = Preview(host, document, plan, owner);
	for (uint64_t tick = 1; tick <= 12; ++tick) {
		owner.CurrentTick = tick;
		(void)Preview(host, document, plan, owner);
	}
	const auto settled = Preview(host, document, plan, owner);
	CHECK(settled.Pixels != initial.Pixels);
	owner.CurrentTick = 0;
	CHECK(Preview(host, document, plan, owner) == initial);
	for (uint64_t tick = 1; tick <= 12; ++tick) {
		owner.CurrentTick = tick;
		(void)Preview(host, document, plan, owner);
	}
	CHECK(Preview(host, document, plan, owner) == settled);
}

TEST_CASE(
	"Studio rigid nonzero observation refresh preserves past frames and replaces the current step",
	"[studio][rigid_host]"
) {
	const auto document = engine::imagegraphphysics::testing::RigidGraphFixture();
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	studio::ImageGraphPlayback owner;
	owner.Playing = owner.FrameProgress = true;
	CapturedFeedbackHost host;
	RigidReplayState prefix;
	for (uint64_t tick = 0; tick < 4; ++tick) {
		owner.CurrentTick = tick;
		(void)Preview(host, document, plan, owner, &prefix);
	}
	REQUIRE(prefix.Owners.size() == 1);
	REQUIRE(prefix.Owners[0].History.Frames.size() == 4);
	owner.CurrentTick = 4;
	const auto expected = [&](bool playing) {
		engine::imagegraphphysics::RigidProvider provider;
		EvaluationRequest request;
		request.Tick = 4;
		request.RigidProvider = &provider;
		request.RigidReplay = &prefix;
		request.RigidPlaying = playing;
		request.RigidFrameProgress = true;
		request.RigidAuthoringRevision = 19;
		StatefulEvaluationResult result;
		const auto status = EvaluateStateful(document, plan, "image", request, result, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
		return result;
	};
	const auto paused = expected(false), played = expected(true);
	REQUIRE(std::get<Image>(paused.Output).Pixels != std::get<Image>(played.Output).Pixels);
	RigidReplayState current;
	owner.Playing = false;
	CHECK(Preview(host, document, plan, owner, &current) == std::get<Image>(paused.Output));
	CHECK(current == paused.Rigid);
	owner.Playing = true;
	CHECK(Preview(host, document, plan, owner, &current) == std::get<Image>(played.Output));
	CHECK(current == played.Rigid);
	REQUIRE(current.Owners.size() == 1);
	const auto &history = current.Owners[0].History.Frames;
	REQUIRE(history.size() == 5);
	CHECK(
		std::equal(
			prefix.Owners[0].History.Frames.begin(), prefix.Owners[0].History.Frames.end(), history.begin()
		)
	);
	CHECK(Preview(host, document, plan, owner, &current) == std::get<Image>(played.Output));
	CHECK(current == played.Rigid);
	owner.Playing = false;
	CHECK(Preview(host, document, plan, owner, &current) == std::get<Image>(paused.Output));
	CHECK(current == paused.Rigid);
}

TEST_CASE(
	"Studio rigid playback refresh retains current Verlet texture and applies downstream rendering",
	"[studio][rigid_host]"
) {
	const auto document = MixedRigidTexture();
	Plan plan;
	Diagnostic diagnostic;
	const auto status = Compile(document, plan, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	studio::ImageGraphPlayback owner;
	owner.FrameProgress = true;
	CapturedFeedbackHost host;
	SimulationReplayState before, after;
	const auto paused = Preview(host, document, plan, owner, nullptr, &before);
	const auto grid = std::find_if(before.Entries.begin(), before.Entries.end(), [](const auto &entry) {
		return entry.NodeId == "grid";
	});
	REQUIRE(grid != before.Entries.end());
	REQUIRE_FALSE(grid->State.Mesh.Points.empty());
	CHECK(grid->State.Mesh.Points[0].Position.X > grid->State.Mesh.Points[0].Original.X);
	owner.Playing = true;
	CapturedFeedbackHost independent;
	const auto played = Preview(independent, document, plan, owner);
	REQUIRE(played.Pixels != paused.Pixels);
	CHECK(Preview(host, document, plan, owner, nullptr, &after) == played);
	CHECK(after == before);
	CHECK(Preview(host, document, plan, owner, nullptr, &after) == played);
	CHECK(after == before);
	owner.Playing = false;
	CHECK(Preview(host, document, plan, owner, nullptr, &after) == paused);
	CHECK(after == before);
}

TEST_CASE(
	"Studio rigid observation refresh preserves current FLIP fill and solver texture", "[studio][rigid_host]"
) {
	const auto document = MixedRigidFluidTexture();
	Plan plan;
	Diagnostic diagnostic;
	const auto compiled = Compile(document, plan, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(compiled == Status::Ok);
	studio::ImageGraphPlayback owner;
	owner.FrameProgress = true;
	CapturedFeedbackHost host;
	SimulationReplayState before, after;
	const auto paused = Preview(host, document, plan, owner, nullptr, &before);
	REQUIRE(before.Entries.size() == 1);
	REQUIRE(before.Entries[0].Fluid.Data);
	REQUIRE(before.Entries[0].Fluid.Data->ParticleCount == 4);
	owner.Playing = true;
	CapturedFeedbackHost independent;
	const auto played = Preview(independent, document, plan, owner);
	REQUIRE(played.Pixels != paused.Pixels);
	CHECK(Preview(host, document, plan, owner, nullptr, &after) == played);
	CHECK(after == before);
	CHECK(Preview(host, document, plan, owner, nullptr, &after) == played);
	CHECK(after == before);
	owner.Playing = false;
	CHECK(Preview(host, document, plan, owner, nullptr, &after) == paused);
	CHECK(after == before);
	// Advancing one tick must still run FLIP exactly once. Refresh admission only
	// preserves an already captured frame, never suppresses the next simulation step.
	owner.CurrentTick = 1;
	const auto next = Preview(host, document, plan, owner, nullptr, &after);
	const auto nextState = after;
	CHECK(nextState != before);
	owner.Playing = true;
	(void)Preview(host, document, plan, owner, nullptr, &after);
	CHECK(after == nextState);
	owner.Playing = false;
	CHECK(Preview(host, document, plan, owner, nullptr, &after) == next);
	CHECK(after == nextState);
}

TEST_CASE(
	"rigid observation preview keys retain the bounded LRU and generic default contract",
	"[studio][rigid_host]"
) {
	Image image;
	image.Width = image.Height = 1;
	image.Pixels = {255, 20, 40, 255};
	image.Hash = SurfaceHash(image);
	studio::ImageGraphPreviewCache cache;
	REQUIRE(cache.Store(1, 0, 0, image));
	REQUIRE(cache.Find(1, 0, 0));
	CHECK(cache.Find(1, 0, 0, 0, false, 0) == cache.Find(1, 0, 0));
	for (uint64_t tick = 1; tick <= 8; ++tick)
		REQUIRE(cache.Store(1, 0, tick, image, 0, false, uint8_t(tick % 4)));
	CHECK_FALSE(cache.Find(1, 0, 0));
	for (uint64_t tick = 1; tick <= 8; ++tick)
		REQUIRE(cache.Find(1, 0, tick, 0, false, uint8_t(tick % 4)));
	studio::ImageGraphPlayback owner;
	owner.Playing = owner.FrameProgress = true;
	CHECK(studio::detail::ImageGraphPlaybackObservation(Document{}, owner) == 0);
}
