#include "RigidGraphFixture.hpp"

#include <engine/imagegraph/FeedbackHost.hpp>
#include <engine/imagegraph/FrameCacheReplay.hpp>
#include <engine/imagegraphphysics/RigidReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

TEST_SUITE_ID("engine.imagegraphphysics.frame_cache_replay")
TEST_DEPENDS("engine.imagegraphphysics.rigid_graph")
using namespace engine::imagegraph;
TEST_CASE(
	"Source cache-only playback refresh rerenders rigid prefix without accumulating another contact step",
	"[imagegraphphysics][source_frame_cache]"
) {
	auto d = engine::imagegraphphysics::testing::RigidGraphFixture();
	d.Timeline = TimelineSettings{6, 0, 5, "loop", 24};
	d.Nodes.push_back({"cache", "pc.cache", "rigid", {}, {{"animated", true}}});
	d.Links[0] = {"cache", "cache_surface", "body", "texture"};
	d.Links.push_back({"texture", "image", "cache", "surface_in"});
	Plan p;
	Diagnostic e;
	REQUIRE(Compile(d, p, e) == Status::Ok);
	engine::imagegraphphysics::RigidProvider provider;
	CapturedFeedbackHost host;
	EvaluationRequest q;
	q.RigidProvider = &provider;
	q.RigidPlaying = q.RigidFrameProgress = true;
	q.SourceCachePlayback = SourceCachePlaybackObservation{true, SourceCacheSampling::ObservedFrame, true};
	for (uint64_t tick = 0; tick <= 3; ++tick) {
		q.Tick = tick;
		REQUIRE(host.Prepare(d, p, 1, 1, q, e, Limits::MaximumEvaluationBytes, "image"));
	}
	REQUIRE(q.RigidReplay);
	REQUIRE(q.DataReplay);
	const auto rigid = *q.RigidReplay;
	const auto data = *q.DataReplay;
	REQUIRE(rigid.Owners.size() == 1);
	REQUIRE(data.Entries.size() == 1);
	CHECK(data.Entries[0].Values.size() == 6);
	const auto *image = host.Output("image");
	REQUIRE(image);
	const Image pixels = *image;
	CHECK(std::any_of(pixels.Pixels.begin(), pixels.Pixels.end(), [](uint8_t value) { return value != 0; }));
	q.SourceCachePlayback->Playing = false;
	REQUIRE(host.Prepare(d, p, 1, 1, q, e, Limits::MaximumEvaluationBytes, "image"));
	CHECK(*q.RigidReplay == rigid);
	CHECK(q.DataReplay->Entries[0].Values == data.Entries[0].Values);
	REQUIRE(host.Output("image"));
	CHECK(*host.Output("image") == pixels);
	const auto frozen = *q.DataReplay;
	q.SourceCachePlayback->Playing = true;
	REQUIRE(host.Prepare(d, p, 1, 1, q, e, Limits::MaximumEvaluationBytes, "image"));
	CHECK(*q.RigidReplay == rigid);
	CHECK(*q.DataReplay == frozen);
	CHECK_FALSE(host.Prepare(d, p, 1, 1, q, e, 1, "image"));
	CHECK(e.Code == Status::LimitExceeded);
	CHECK(*host.Output("image") == pixels);
	host.RestartCycle();
	q.Tick = 2;
	CHECK_FALSE(host.Prepare(d, p, 1, 1, q, e, Limits::MaximumEvaluationBytes, "image"));
	CHECK(e.Code == Status::UnsupportedExecution);
	CHECK(*host.Output("image") == pixels);
}
