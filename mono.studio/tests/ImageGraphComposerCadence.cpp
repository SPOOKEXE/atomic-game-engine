#include "ImageGraphComposerCadence.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("studio.imagegraph.composer_cadence")
TEST_DEPENDS("studio.imagegraph")

TEST_CASE(
	"A pending exact shader preview finishes despite advancing playback", "[studio][composer_cadence]"
) {
	using namespace studio::detail;
	ImageGraphComposerCadence cadence;
	ImageGraphComposerCadence::Identity identity{engine::core::Name("preview-owner"), "pixels", 7, 4, 3};
	ImageGraphObservations observations;
	std::tm calendar{};
	observations.Capture(7, {12}, "scene", 5.25, calendar);
	REQUIRE(cadence.Begin(identity, {12}, observations) == engine::imagegraph::FrameTime{12});
	cadence.Pending();
	for (uint64_t playback = 13; playback <= 16; ++playback) {
		observations.Capture(7, {playback}, "scene", double(playback), calendar);
		CHECK(cadence.Begin(identity, {playback}, observations) == engine::imagegraph::FrameTime{12});
		CHECK(std::get<double>(cadence.Observations.Values.front().Data) == 5.25);
		CHECK_FALSE(cadence.Displayed);
	}
	CHECK_FALSE(cadence.Complete({16}));
	CHECK(cadence.Held);
	REQUIRE(cadence.Complete({12}));
	CHECK(cadence.Displayed == engine::imagegraph::FrameTime{12});
	CHECK(cadence.Begin(identity, {17}, observations) == engine::imagegraph::FrameTime{17});
	CHECK(cadence.Displayed == engine::imagegraph::FrameTime{12});
}
TEST_CASE(
	"Authored owner input and rigid observation changes replace pending identity",
	"[studio][composer_cadence]"
) {
	using namespace studio::detail;
	ImageGraphComposerCadence cadence;
	ImageGraphComposerCadence::Identity identity{engine::core::Name("old-owner"), "pixels", 7, 4, 3};
	ImageGraphObservations observations;
	const auto replace = [&](auto change) {
		cadence.Cancel();
		cadence.Begin(identity, {1}, observations);
		auto next = identity;
		change(next);
		CHECK_FALSE(cadence.Matches(next));
		CHECK(cadence.Begin(next, {2}, observations) == engine::imagegraph::FrameTime{2});
		CHECK_FALSE(cadence.Complete({1}));
		CHECK_FALSE(cadence.Displayed);
		REQUIRE(cadence.Complete({2}));
		cadence.Displayed.reset();
	};
	replace([](auto &next) { ++next.Revision; });
	replace([](auto &next) { ++next.InputRevision; });
	replace([](auto &next) { next.Owner = engine::core::Name("new-owner"); });
	replace([](auto &next) { next.Output = "different-pixels"; });
	replace([](auto &next) { next.RigidObservation = 0; });
	cadence.Cancel();
	CHECK_FALSE(cadence.Held);
}
TEST_CASE(
	"Pending rigid progress pulse stays captured after the UI clears it", "[studio][composer_cadence]"
) {
	using namespace studio::detail;
	ImageGraphComposerCadence cadence;
	ImageGraphComposerCadence::Identity captured{engine::core::Name("owner"), "out", 2, 3, 2};
	ImageGraphObservations observations;
	cadence.Begin(captured, {12}, observations, false, true);
	cadence.Pending();
	auto cleared = captured;
	cleared.RigidObservation = 0;
	const auto frozen = cadence.RetainProgressPulse(cleared);
	CHECK(frozen == captured);
	CHECK(cadence.Begin(frozen, {12}, observations) == engine::imagegraph::FrameTime{12});
	CHECK(cadence.Current.RigidObservation == 2);
	CHECK(cadence.FrameProgress);
	CHECK_FALSE(cadence.Playing);
	auto playing = cleared;
	playing.RigidObservation = 1;
	CHECK(cadence.RetainProgressPulse(playing) == playing);
	CHECK_FALSE(cadence.Matches(playing));
	REQUIRE(cadence.Complete({12}));
	CHECK(cadence.RetainProgressPulse(cleared) == cleared);
	cadence.Begin(cleared, {12}, observations, false, false);
	CHECK(cadence.Current.RigidObservation == 0);
	CHECK_FALSE(cadence.FrameProgress);
}
