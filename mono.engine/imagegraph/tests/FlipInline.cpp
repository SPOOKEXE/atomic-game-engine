#include <engine/imagegraph/FeedbackHost.hpp>
#include <engine/imagegraph/SimulationReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
TEST_SUITE_ID("engine.imagegraph.flip_inline")
using namespace engine::imagegraph;
Document InlineFluidScene(int64_t updateSteps = 1) {
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
namespace {
	Document WrappedFluidScene() {
		auto document = InlineFluidScene();
		document.Nodes.insert(document.Nodes.begin(), {"fluid-scope", "pc.flip_group_inline", "", {}, {}});
		Group collection;
		collection.Id = "fluid-scope/inline";
		collection.OwnerNodeId = "fluid-scope";
		document.Groups.push_back(collection);
		Group nested;
		nested.Id = "nested";
		nested.ParentId = collection.Id;
		document.Groups.push_back(nested);
		for (auto &node : document.Nodes)
			if (node.Id != "fluid-scope") node.GroupId = node.Id == "render" ? "nested" : collection.Id;
		return document;
	}
} // namespace
TEST_CASE("FLIP Inline preserves nested fluid stepping reset and host seek", "[imagegraph][flip_inline]") {
	auto wrapped = WrappedFluidScene();
	auto plain = InlineFluidScene();
	Plan wrappedPlan, plainPlan;
	Diagnostic diagnostic;
	REQUIRE(Compile(wrapped, wrappedPlan, diagnostic) == Status::Ok);
	REQUIRE(Compile(plain, plainPlan, diagnostic) == Status::Ok);
	REQUIRE(wrappedPlan.InlineOwnerDependencies.size() == 3);
	for (const auto &route : wrappedPlan.InlineOwnerDependencies)
		CHECK(route.Owner == 0);
	EvaluationRequest wrappedRequest, plainRequest;
	wrappedRequest.Seed = plainRequest.Seed = 12345;
	wrappedRequest.SimulationAuthoringRevision = plainRequest.SimulationAuthoringRevision = 4;
	SimulationEvaluationResult wrappedResult, plainResult;
	for (uint64_t tick = 0; tick <= 3; ++tick) {
		wrappedRequest.Tick = plainRequest.Tick = tick;
		INFO(diagnostic.Message);
		REQUIRE(
			EvaluateSimulation(wrapped, wrappedPlan, "image", wrappedRequest, wrappedResult, diagnostic) ==
			Status::Ok
		);
		REQUIRE(
			EvaluateSimulation(plain, plainPlan, "image", plainRequest, plainResult, diagnostic) == Status::Ok
		);
		CHECK(wrappedResult.Output == plainResult.Output);
		CHECK(wrappedResult.Replay == plainResult.Replay);
		wrappedRequest.SimulationReplay = &wrappedResult.Replay;
		plainRequest.SimulationReplay = &plainResult.Replay;
	}
	CapturedFeedbackHost seek;
	EvaluationRequest seekRequest;
	seekRequest.Seed = 12345;
	seekRequest.Tick = 3;
	REQUIRE(seek.Prepare(
		wrapped, wrappedPlan, 4, 1, seekRequest, diagnostic, Limits::MaximumEvaluationBytes, "image"
	));
	REQUIRE(seek.Output("image"));
	CHECK(*seek.Output("image") == std::get<Image>(wrappedResult.Output));
	REQUIRE(seekRequest.SimulationReplay);
	CHECK(*seekRequest.SimulationReplay == wrappedResult.Replay);
	wrappedRequest.Tick = 0;
	wrappedRequest.SimulationReplay = nullptr;
	SimulationEvaluationResult reset;
	REQUIRE(
		EvaluateSimulation(wrapped, wrappedPlan, "image", wrappedRequest, reset, diagnostic) == Status::Ok
	);
	plainRequest.Tick = 0;
	plainRequest.SimulationReplay = nullptr;
	REQUIRE(
		EvaluateSimulation(plain, plainPlan, "image", plainRequest, plainResult, diagnostic) == Status::Ok
	);
	CHECK(reset.Output == plainResult.Output);
	CHECK(reset.Replay == plainResult.Replay);
	const auto checkpoint = reset.Replay;
	const auto checkpointImage = std::get<Image>(reset.Output);
	wrappedRequest.Tick = 1;
	wrappedRequest.SimulationReplay = &reset.Replay;
	CHECK(
		EvaluateSimulation(wrapped, wrappedPlan, "image", wrappedRequest, reset, diagnostic, 1) ==
		Status::LimitExceeded
	);
	CHECK(reset.Replay == checkpoint);
	CHECK(std::get<Image>(reset.Output) == checkpointImage);
}
TEST_CASE(
	"FLIP Inline unrelated membership does not run another fluid closure", "[imagegraph][flip_inline]"
) {
	auto document = WrappedFluidScene();
	auto unused = InlineFluidScene();
	for (auto &node : unused.Nodes) {
		node.Id += "-unused";
		node.GroupId = "fluid-scope/inline";
		document.Nodes.push_back(node);
	}
	for (auto link : unused.Links) {
		link.FromNode += "-unused";
		link.ToNode += "-unused";
		document.Links.push_back(link);
	}
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	SimulationEvaluationResult result;
	REQUIRE(EvaluateSimulation(document, plan, "image", {}, result, diagnostic) == Status::Ok);
	REQUIRE(result.Replay.Entries.size() == 1);
	CHECK(result.Replay.Entries.front().NodeId == "domain");
}
