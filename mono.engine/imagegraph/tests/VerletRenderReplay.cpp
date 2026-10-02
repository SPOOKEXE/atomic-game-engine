#include <engine/imagegraph/FeedbackHost.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

TEST_SUITE_ID("engine.imagegraph.verlet_render_replay")
using namespace engine::imagegraph;
namespace {
	Document RenderGraph(bool step = true) {
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
			{"collide",
			 "pc.verlet_sim_collide",
			 "scope",
			 {},
			 {{"area_unit", EnumValue{0}}, {"area", Area{0, 1, .25, .25}}, {"shape", EnumValue{0}}}},
			{"render", "pc.verlet_sim_render", "scope", {}, {{"type", EnumValue{0}}}}
		};
		if (!step) document.Nodes.back().Values.push_back({"step", false});
		Group scope{"scope", "scope"};
		scope.OwnerNodeId = "owner";
		document.Groups = {scope};
		document.Links = {{"grid", "mesh", "render", "mesh"}};
		document.Outputs = {{"image", "render", "surface_out"}};
		return document;
	}
	void CheckNonadvancingFractionalRender(const Document &document) {
		Plan plan;
		Diagnostic diagnostic;
		const auto compiled = Compile(document, plan, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(compiled == Status::Ok);
		CapturedFeedbackHost host;
		EvaluationRequest request;
		request.Tick = 2;
		request.Subframe = .5;
		request.Seed = 42;
		REQUIRE(
			host.Prepare(document, plan, 1, 1, request, diagnostic, Limits::MaximumEvaluationBytes, "image")
		);
		REQUIRE(host.Output("image"));
		CHECK(host.Output("image")->Width == 4);
		CHECK(host.Output("image")->Height == 4);
		REQUIRE(request.SimulationReplay);
		const auto origin = std::find_if(
			request.SimulationReplay->Entries.begin(),
			request.SimulationReplay->Entries.end(),
			[](const auto &entry) { return entry.NodeId == "grid"; }
		);
		REQUIRE(origin != request.SimulationReplay->Entries.end());
		REQUIRE(origin->State.Mesh.Points.size() == 4);
		for (const auto &point : origin->State.Mesh.Points)
			CHECK(point.Position == point.Original);
	}
}
TEST_CASE(
	"Verlet Render integer seek matches sequential Collide replay and refuses fractional advance",
	"[imagegraph][verlet_render_replay]"
) {
	const auto document = RenderGraph();
	Plan plan;
	Diagnostic diagnostic;
	const auto compiled = Compile(document, plan, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(compiled == Status::Ok);
	CapturedFeedbackHost sequential, seek;
	EvaluationRequest sequentialRequest, seekRequest;
	sequentialRequest.Seed = seekRequest.Seed = 42;
	for (uint64_t tick = 0; tick <= 3; ++tick) {
		sequentialRequest.Tick = tick;
		REQUIRE(sequential.Prepare(
			document, plan, 1, 1, sequentialRequest, diagnostic, Limits::MaximumEvaluationBytes, "image"
		));
	}
	seekRequest.Tick = 3;
	REQUIRE(
		seek.Prepare(document, plan, 1, 1, seekRequest, diagnostic, Limits::MaximumEvaluationBytes, "image")
	);
	REQUIRE(sequential.Output("image"));
	REQUIRE(seek.Output("image"));
	CHECK(*seek.Output("image") == *sequential.Output("image"));
	REQUIRE(sequentialRequest.SimulationReplay);
	REQUIRE(seekRequest.SimulationReplay);
	CHECK(*seekRequest.SimulationReplay == *sequentialRequest.SimulationReplay);
	const auto previousImage = *seek.Output("image");
	const auto previousReplay = *seekRequest.SimulationReplay;
	seekRequest.Subframe = .5;
	CHECK_FALSE(
		seek.Prepare(document, plan, 1, 1, seekRequest, diagnostic, Limits::MaximumEvaluationBytes, "image")
	);
	CHECK(diagnostic.Code == Status::InvalidValue);
	CHECK(*seek.Output("image") == previousImage);
	CHECK(*seekRequest.SimulationReplay == previousReplay);
}
TEST_CASE(
	"Verlet Render literal false Step preserves fractional nonadvancing poses",
	"[imagegraph][verlet_render_replay]"
) {
	CheckNonadvancingFractionalRender(RenderGraph(false));
}
TEST_CASE(
	"Verlet Render linked false Step preserves fractional nonadvancing poses",
	"[imagegraph][verlet_render_replay]"
) {
	auto document = RenderGraph();
	document.Nodes.push_back({"control", "value.boolean", "", {}, {{"value", false}}});
	document.Links.push_back({"control", "boolean", "render", "step"});
	CheckNonadvancingFractionalRender(document);
}
