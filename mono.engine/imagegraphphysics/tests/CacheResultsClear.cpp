#include "RigidGraphFixture.hpp"

#include <engine/imagegraph/CacheResultsReplay.hpp>
#include <engine/imagegraph/FeedbackHost.hpp>
#include <engine/imagegraphphysics/RigidReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraphphysics.cache_results_clear")
TEST_DEPENDS("engine.imagegraph.cache_results_replay")
using namespace engine::imagegraph;
TEST_CASE(
	"Cache Results clear cannot be undone by the rigid pre-frame checkpoint", "[rigid][cache_results_clear]"
) {
	auto d = engine::imagegraphphysics::testing::RigidGraphFixture();
	d.Timeline = TimelineSettings{5, 0, 4, "loop", 24};
	d.Nodes.push_back({"cache", "pc.cache_results", "rigid", {}, {{"amount", int64_t{1}}}});
	d.Nodes.push_back({"select", "pc.sequence_anim", "rigid", {}, {{"overflow", EnumValue{0}}}});
	d.Links[0] = {"select", "surface_out", "body", "texture"};
	d.Links.push_back({"texture", "image", "cache", "surface_in"});
	d.Links.push_back({"cache", "cache_surfaces", "select", "surface_in"});
	Plan p;
	Diagnostic diagnostic;
	REQUIRE(Compile(d, p, diagnostic) == Status::Ok);
	engine::imagegraphphysics::RigidProvider provider;
	CapturedFeedbackHost host;
	EvaluationRequest request;
	request.RigidProvider = &provider;
	const auto prepare = [&] {
		const bool ok =
			host.Prepare(d, p, 17, 29, request, diagnostic, Limits::MaximumEvaluationBytes, "image");
		INFO(diagnostic.NodeId << ':' << diagnostic.Port << ' ' << diagnostic.Message);
		REQUIRE(ok);
	};
	const auto cache = [&]() -> const DataReplayEntry & {
		for (const auto &row : request.DataReplay->Entries)
			if (row.NodeId == "cache") return row;
		FAIL("missing cache state");
		return request.DataReplay->Entries.front();
	};
	prepare();
	request.Tick = 1;
	request.RigidPlaying = request.RigidFrameProgress = true;
	prepare();
	const auto before = *request.RigidReplay;
	REQUIRE(before.Owners.size() == 1);
	REQUIRE(before.Owners.front().History.Frames.size() >= 2);
	const auto firstFrame = before.Owners.front().History.Frames.front();
	REQUIRE(host.ClearCacheResults(d, p, "cache", 17, 29, diagnostic));
	CHECK(IsFreedCacheResultsSlot(std::get<ArrayValue>(cache().Values[0].Data).Elements[0]));
	request.RigidPlaying = request.RigidFrameProgress = false;
	CHECK_FALSE(host.Prepare(d, p, 17, 29, request, diagnostic, Limits::MaximumEvaluationBytes, "image"));
	CHECK(diagnostic.Code == Status::UnsupportedExecution);
	CHECK(*request.RigidReplay == before);
	// A distinct authored observation rebuilds current tick from its checkpoint.
	request.Subframe = .5;
	prepare();
	CHECK(cache().Subframe == .5);
	CHECK(std::holds_alternative<SurfaceValue>(std::get<ArrayValue>(cache().Values[0].Data).Elements[0]));
	CHECK(request.RigidReplay->Owners.front().History.Frames.front() == firstFrame);
	REQUIRE(host.Output("image"));
	CHECK(
		std::any_of(
			host.Output("image")->Pixels.begin(), host.Output("image")->Pixels.end(), [](uint8_t value) {
				return value != 0;
			}
		)
	);
}
