#include "ImageGraphComposerCadence.hpp"
#include "ImageGraphRigid.hpp"

#include <engine/imagegraph/FeedbackHost.hpp>
#include <engine/imagegraph/FrameCacheReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("studio.imagegraph_frame_cache")
using namespace engine::imagegraph;
TEST_CASE(
	"Studio frozen preview Playing drives real observed Cache Array without filling unobserved frames",
	"[studio][source_frame_cache]"
) {
	Document d;
	d.FormatVersion = 9;
	d.Timeline = TimelineSettings{6, 0, 5, "loop", 24};
	d.Nodes = {
		{"solid",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{44, 0, 0, 255}}}},
		{"cache",
		 "pc.cache_array",
		 "",
		 {},
		 {{"start_frame", int64_t{-1}}, {"stop_frame", int64_t{-1}}, {"step", int64_t{1}}}}
	};
	d.Links = {{"solid", "image", "cache", "surface_in"}};
	d.Outputs = {{"out", "cache", "cache_array"}};
	Plan p;
	Diagnostic e;
	REQUIRE(Compile(d, p, e) == Status::Ok);
	studio::ImageGraphPlayback authoritative;
	authoritative.Playing = true;
	studio::detail::ImageGraphComposerCadence cadence;
	studio::detail::ImageGraphComposerCadence::Identity identity{};
	identity.Output = "out";
	identity.Revision = 1;
	identity.RigidObservation = studio::detail::ImageGraphPlaybackObservation(d, authoritative);
	cadence.Begin(identity, {3}, {}, authoritative.Playing, false);
	authoritative.Playing = false;
	studio::ImageGraphPlayback frozen = authoritative;
	frozen.Playing = cadence.Playing;
	engine::imagegraphphysics::RigidProvider provider;
	EvaluationRequest q;
	q.Tick = 3;
	studio::detail::BindImageGraphRigid(q, provider, frozen);
	REQUIRE(q.SourceCachePlayback);
	CHECK(q.SourceCachePlayback->Playing);
	CHECK(q.SourceCachePlayback->Sampling == SourceCacheSampling::ObservedFrame);
	CapturedFeedbackHost host;
	REQUIRE(host.Prepare(d, p, 1, 1, q, e, Limits::MaximumEvaluationBytes, "out"));
	REQUIRE(q.DataReplay);
	REQUIRE(q.DataReplay->Entries.size() == 1);
	CHECK(q.DataReplay->Entries[0].Values.size() == 3);
	CHECK(q.DataReplay->Entries[0].Values.back().Frame == 5);
	const auto *last = SourceFrameCacheLastOutput(q.DataReplay->Entries[0]);
	REQUIRE(last);
	const auto &array = std::get<ArrayValue>(*last);
	REQUIRE(array.Items.size() == 6);
	CHECK(std::get<int64_t>(std::get<ElementValue>(array.Items[0].Data)) == -1);
	CHECK(std::get<SurfaceValue>(q.DataReplay->Entries[0].Values.back().Data).Data.Pixels[0] == 44);
	const auto played = *q.DataReplay;
	const Value playedOutput = *last;
	auto paused = EvaluationRequest{.Tick = 3};
	studio::detail::BindImageGraphRigid(paused, provider, authoritative);
	REQUIRE(host.Prepare(d, p, 1, 1, paused, e, Limits::MaximumEvaluationBytes, "out"));
	REQUIRE(SourceFrameCacheLastOutput(paused.DataReplay->Entries[0]));
	CHECK(*SourceFrameCacheLastOutput(paused.DataReplay->Entries[0]) == playedOutput);
	CHECK(paused.DataReplay->Entries[0].Values.size() == played.Entries[0].Values.size());
}
TEST_CASE(
	"Source cache playback changes supersede a held request while rigid progress pulses remain held",
	"[studio][source_frame_cache]"
) {
	Document d;
	d.Nodes = {{"cache", "pc.cache", "", {}, {}}, {"owner", "pc.rigid_group_inline", "", {}, {}}};
	studio::ImageGraphPlayback playback;
	playback.Playing = true;
	playback.FrameProgress = true;
	studio::detail::ImageGraphComposerCadence cadence;
	studio::detail::ImageGraphComposerCadence::Identity id{};
	id.RigidObservation = studio::detail::ImageGraphPlaybackObservation(d, playback);
	cadence.Begin(id, {1}, {}, true, true);
	CHECK(id.RigidObservation == 7);
	playback.FrameProgress = false;
	auto noPulse = id;
	noPulse.RigidObservation = studio::detail::ImageGraphPlaybackObservation(d, playback);
	CHECK(cadence.RetainProgressPulse(noPulse) == id);
	Document cacheOnly;
	cacheOnly.Nodes = {d.Nodes.front()};
	playback.Playing = false;
	auto pause = id;
	pause.RigidObservation = studio::detail::ImageGraphPlaybackObservation(cacheOnly, playback);
	CHECK_FALSE(cadence.RetainProgressPulse(pause) == id);
	CHECK_FALSE(cadence.Matches(pause));
}
