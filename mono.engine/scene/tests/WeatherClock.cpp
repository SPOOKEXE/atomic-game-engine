#include <engine/core/Bytes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/WeatherClock.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

TEST_SUITE_ID("engine.scene.weather-clock")

using namespace engine::scene;
using Catch::Approx;

TEST_CASE(
	"physical weather elapsed preserves division residue and large-age subtraction", "[scene][weather]"
) {
	for (const uint32_t rate : {1u, 7u, 20u, 999u, 1000u}) {
		WeatherClock direct, segmented;
		direct.TickRate = segmented.TickRate = rate;
		REQUIRE(AdvanceWeatherElapsed(direct, UINT64_MAX));
		REQUIRE(AdvanceWeatherElapsed(segmented, UINT64_MAX - 1000));
		REQUIRE(AdvanceWeatherElapsed(segmented, 1000));
		CHECK(direct.ElapsedSeconds == segmented.ElapsedSeconds);
		CHECK(direct.ElapsedFractionQ32 == segmented.ElapsedFractionQ32);
		CHECK(direct.ElapsedRemainder == segmented.ElapsedRemainder);
		segmented.Tick = UINT64_MAX;
		for (uint64_t delta = 0; delta < 20; ++delta) {
			WeatherClock earlier;
			earlier.TickRate = rate;
			REQUIRE(AdvanceWeatherElapsed(earlier, UINT64_MAX - delta));
			const auto reconstructed = WeatherElapsedAtTick(segmented, UINT64_MAX - delta);
			REQUIRE(reconstructed);
			CHECK(reconstructed->Seconds == earlier.ElapsedSeconds);
			CHECK(reconstructed->FractionQ32 == earlier.ElapsedFractionQ32);
		}
		WeatherClock perTick, oneSecond;
		perTick.TickRate = oneSecond.TickRate = rate;
		for (uint32_t tick = 0; tick < rate; ++tick)
			REQUIRE(AdvanceWeatherElapsed(perTick, 1));
		REQUIRE(AdvanceWeatherElapsed(oneSecond, rate));
		CHECK(perTick.ElapsedSeconds == 1);
		CHECK(perTick.ElapsedFractionQ32 == 0);
		CHECK(perTick.ElapsedRemainder == 0);
		CHECK(perTick.ElapsedSeconds == oneSecond.ElapsedSeconds);
	}
	WeatherClock malformed;
	malformed.TickRate = 0;
	CHECK_FALSE(AdvanceWeatherElapsed(malformed, 1));
	CHECK_FALSE(WeatherElapsedAtTick(malformed, 0));
}

TEST_CASE(
	"replica acoustic elapsed follows its RTT-adjusted visual tick and freezes on world pause",
	"[scene][weather]"
) {
	RegisterWeatherClockComponents();
	engine::ecs::Store store("visual_elapsed");
	const auto provider = store.Create();
	WeatherClock clock;
	clock.Sequence = 1;
	clock.Tick = 20;
	clock.ElapsedSeconds = 1;
	store.Set(provider, clock);
	SynchronizeWeatherClocks(store, 0, 0.1);
	const auto initial = WeatherClockOf(store, provider);
	CHECK(initial.Tick == 21);
	CHECK(initial.ElapsedSeconds == 1);
	CHECK(initial.ElapsedFractionQ32 == 214748364);
	CHECK(initial.ElapsedRemainder == 16);
	const auto accepted = WeatherElapsedAtTick(initial, 20);
	REQUIRE(accepted);
	CHECK(accepted->Seconds == 1);
	CHECK(accepted->FractionQ32 == 0);
	SynchronizeWeatherClocks(store, 0.45, 0.1);
	const auto advanced = WeatherClockOf(store, provider);
	CHECK(advanced.Tick == 30);
	CHECK(advanced.ElapsedSeconds == 1);
	CHECK(advanced.ElapsedFractionQ32 == (uint32_t{1} << 31));
	clock = advanced;
	clock.Sequence = 2;
	clock.WorldPaused = true;
	store.Set(provider, clock);
	SynchronizeWeatherClocks(store, 1, 0.1);
	SynchronizeWeatherClocks(store, 100, 0.1);
	const auto suspended = WeatherClockOf(store, provider);
	CHECK(suspended.Tick == advanced.Tick);
	CHECK(suspended.ElapsedSeconds == advanced.ElapsedSeconds);
	CHECK(suspended.ElapsedFractionQ32 == advanced.ElapsedFractionQ32);
}

TEST_CASE("weather clock keeps uint64 ticks exact and refuses malformed encoding", "[scene][weather]") {
	WeatherClock clock;
	clock.Epoch = 8;
	clock.Sequence = UINT64_MAX;
	clock.Tick = UINT64_MAX;
	clock.AnchorTick = UINT64_MAX - 1;
	clock.PhaseTick = UINT64_MAX - 2;
	clock.DisplacementQ8 = {UINT32_MAX, 15, 0};
	clock.WindQ20 = {-12, -1073741824, 1073741824};
	clock.DisplacementRemainderQ12 = {4095, 17, 0};
	clock.ElapsedSeconds = UINT64_MAX;
	clock.ElapsedFractionQ32 = UINT32_MAX;
	clock.ElapsedRemainder = 9;
	engine::core::ByteWriter encoded;
	WriteWeatherClock(encoded, clock);
	engine::core::ByteReader reader(encoded.Bytes());
	WeatherClock decoded;
	REQUIRE(ReadWeatherClock(reader, decoded));
	CHECK(decoded.Tick == UINT64_MAX);
	CHECK(decoded.AnchorTick == clock.AnchorTick);
	CHECK(decoded.PhaseTick == clock.PhaseTick);
	CHECK(decoded.DisplacementQ8 == clock.DisplacementQ8);
	CHECK(decoded.WindQ20 == clock.WindQ20);
	CHECK(decoded.DisplacementRemainderQ12 == clock.DisplacementRemainderQ12);
	CHECK(decoded.ElapsedSeconds == clock.ElapsedSeconds);
	CHECK(decoded.ElapsedFractionQ32 == clock.ElapsedFractionQ32);
	CHECK(decoded.ElapsedRemainder == clock.ElapsedRemainder);
	for (size_t length = 0; length < encoded.Size(); ++length) {
		engine::core::ByteReader truncated(encoded.Bytes().first(length));
		WeatherClock unchanged;
		unchanged.Tick = 123;
		CHECK_FALSE(ReadWeatherClock(truncated, unchanged));
		CHECK(unchanged.Tick == 123);
	}
	for (int invalid = 0; invalid < 8; ++invalid) {
		WeatherClock bad;
		if (invalid == 0) bad.Fraction = std::numeric_limits<double>::quiet_NaN();
		if (invalid == 1) bad.TickRate = 0;
		if (invalid == 2) bad.AnchorTick = 1;
		if (invalid == 3) bad.TimeScaleQ16 = 16 * 65536 + 1;
		if (invalid == 4) bad.Epoch = 0;
		if (invalid == 5) bad.DisplacementRemainderQ12[0] = 4096;
		if (invalid == 6) bad.WindQ20[0] = 1073741825;
		if (invalid == 7) bad.ElapsedRemainder = bad.TickRate;
		engine::core::ByteWriter bytes;
		WriteWeatherClock(bytes, bad);
		engine::core::ByteReader malformed(bytes.Bytes());
		CHECK_FALSE(ReadWeatherClock(malformed, decoded));
		CHECK(decoded.Tick == UINT64_MAX);
	}
	std::vector<std::byte> unsupportedBytes(encoded.Bytes().begin(), encoded.Bytes().end());
	engine::core::ByteWriter unsupportedVersion;
	unsupportedVersion.WriteUInt32(WEATHER_CLOCK_ENCODING_VERSION + 1);
	std::copy(unsupportedVersion.Bytes().begin(), unsupportedVersion.Bytes().end(), unsupportedBytes.begin());
	engine::core::ByteReader unsupported(unsupportedBytes);
	CHECK_FALSE(ReadWeatherClock(unsupported, decoded));
	CHECK(decoded.Tick == UINT64_MAX);
	std::vector<std::byte> malformedFlags(encoded.Bytes().begin(), encoded.Bytes().end());
	// WorldPaused is the last explicitly encoded byte, independent of in-memory padding.
	malformedFlags.back() = std::byte{2};
	engine::core::ByteReader malformed(malformedFlags);
	CHECK_FALSE(ReadWeatherClock(malformed, decoded));
	CHECK(decoded.Tick == UINT64_MAX);
}

TEST_CASE("weather tick addition handles maximum ticks without floating conversion", "[scene][weather]") {
	uint64_t tick = UINT64_MAX - 2;
	double fraction = 0.75;
	REQUIRE(AdvanceWeatherTick(tick, fraction, 0.5));
	CHECK(tick == UINT64_MAX - 1);
	CHECK(fraction == Approx(0.25));
	REQUIRE(AdvanceWeatherTick(tick, fraction, 3));
	CHECK(tick == UINT64_MAX);
	CHECK(fraction == 0);
	CHECK_FALSE(AdvanceWeatherTick(tick, fraction, -1));
	CHECK_FALSE(AdvanceWeatherTick(tick, fraction, std::numeric_limits<double>::infinity()));
	CHECK(tick == UINT64_MAX);
}

TEST_CASE("late weather join uses measured RTT and monotonic receipt time", "[scene][weather]") {
	WeatherClock sample;
	sample.Tick = 3000;
	sample.Sequence = 100;
	WeatherClockEstimate estimate;
	REQUIRE(SampleWeatherClock(estimate, sample, 999, 0.2));
	CHECK(estimate.Tick == 3002);
	CHECK(estimate.RoundTripSeconds == Approx(0.2));
	AdvanceWeatherClock(estimate, 999.5);
	CHECK(estimate.Tick == 3012);
	AdvanceWeatherClock(estimate, 2000);
	CHECK(estimate.Tick == 3042);
	AdvanceWeatherClock(estimate, 999);
	CHECK(estimate.Tick == 3042);
	CHECK_FALSE(SampleWeatherClock(estimate, sample, 2001, 0.2));
	sample.Sequence = 99;
	CHECK_FALSE(SampleWeatherClock(estimate, sample, 2001, 0.2));
	sample.Epoch = 2;
	sample.Sequence = 1;
	sample.Tick = 20;
	REQUIRE(SampleWeatherClock(estimate, sample, 2001, 0));
	CHECK(estimate.Tick == 20);
	CHECK(estimate.HistoryRevision == 2);
	sample.Epoch = 1;
	sample.Sequence = 1000;
	CHECK_FALSE(SampleWeatherClock(estimate, sample, 2002, 0));
}

TEST_CASE("small weather offsets slew monotonically while large offsets reconstruct", "[scene][weather]") {
	WeatherClock sample;
	sample.Tick = 100;
	sample.Sequence = 1;
	WeatherClockEstimate estimate;
	REQUIRE(SampleWeatherClock(estimate, sample, 0, 0));
	sample.Sequence = 2;
	REQUIRE(SampleWeatherClock(estimate, sample, 1, 0));
	CHECK(estimate.Tick == 120);
	CHECK(estimate.CorrectionTicks == Approx(-20));
	AdvanceWeatherClock(estimate, 1.5);
	CHECK(estimate.Tick == 129);
	CHECK(estimate.HistoryRevision == 1);
	sample.Sequence = 3;
	sample.Tick = 10000;
	REQUIRE(SampleWeatherClock(estimate, sample, 2, 0));
	CHECK(estimate.Tick == 10000);
	CHECK(estimate.HistoryRevision == 2);
	CHECK_FALSE(SampleWeatherClock(estimate, sample, 3, -0.1));
	CHECK_FALSE(SampleWeatherClock(estimate, sample, 3, 11));
}

TEST_CASE("moving weather clocks converge at different frame rates and RTTs", "[scene][weather]") {
	constexpr uint64_t base = uint64_t{1} << 60;
	std::array<WeatherClockEstimate, 2> clients;
	const std::array<uint32_t, 2> frameRates{30, 144};
	const std::array<double, 2> roundTrips{0.08, 0.32};
	const std::array<double, 2> localOrigins{1000, 4000};
	for (size_t client = 0; client < clients.size(); ++client) {
		auto &estimate = clients[client];
		WeatherClock sample;
		sample.Tick = base;
		sample.Sequence = 1;
		const double delay = roundTrips[client] * 0.5;
		REQUIRE(SampleWeatherClock(estimate, sample, localOrigins[client] + delay, roundTrips[client]));
		if (client == 0)
			estimate.Tick += 15;
		else
			estimate.Tick -= 15;
		double previousError = 15;
		for (uint32_t second = 1; second <= 12; ++second) {
			for (uint32_t frame = 1; frame <= frameRates[client]; ++frame) {
				const double before = static_cast<double>(estimate.Tick - (base - 100)) + estimate.Fraction;
				const double now = localOrigins[client] + delay + second - 1 +
								   static_cast<double>(frame) / frameRates[client];
				AdvanceWeatherClock(estimate, now);
				const double advanced =
					static_cast<double>(estimate.Tick - (base - 100)) + estimate.Fraction - before;
				CHECK(advanced >= 0.9 * sample.TickRate / frameRates[client] - 1e-8);
				CHECK(advanced <= 1.1 * sample.TickRate / frameRates[client] + 1e-8);
			}
			sample.Tick = base + second * sample.TickRate;
			sample.Sequence = second + 1;
			const double receipt = localOrigins[client] + second + delay;
			REQUIRE(SampleWeatherClock(estimate, sample, receipt, roundTrips[client]));
			const double error = std::abs(estimate.CorrectionTicks);
			CHECK(error <= previousError + 1e-7);
			previousError = error;
			auto stale = sample;
			--stale.Sequence;
			stale.Tick = base + 9999;
			CHECK_FALSE(SampleWeatherClock(estimate, stale, receipt, roundTrips[client]));
			CHECK_FALSE(SampleWeatherClock(estimate, sample, receipt, roundTrips[client]));
			if (second >= 10) CHECK(std::abs(estimate.CorrectionTicks) < 1e-7);
		}
		CHECK(estimate.HistoryRevision == 1);
		AdvanceWeatherClock(estimate, localOrigins[client] + 14);
		CHECK(static_cast<double>(estimate.Tick - base) + estimate.Fraction == Approx(280).margin(1e-7));
		AdvanceWeatherClock(estimate, localOrigins[client] + 15);
		const auto stoppedTick = estimate.Tick;
		const auto stoppedFraction = estimate.Fraction;
		AdvanceWeatherClock(estimate, localOrigins[client] + 100);
		CHECK(estimate.Tick == stoppedTick);
		CHECK(estimate.Fraction == stoppedFraction);
	}
}

TEST_CASE("paused weather does not extrapolate or pay an RTT progression offset", "[scene][weather]") {
	WeatherClock sample;
	sample.Tick = 77;
	sample.Sequence = 1;
	sample.Paused = true;
	WeatherClockEstimate estimate;
	REQUIRE(SampleWeatherClock(estimate, sample, 0, 0.4));
	AdvanceWeatherClock(estimate, 100);
	CHECK(estimate.Tick == 77);
	sample.Sequence = 2;
	sample.Paused = false;
	sample.TimeScaleQ16 = 0;
	REQUIRE(SampleWeatherClock(estimate, sample, 101, 0.4));
	AdvanceWeatherClock(estimate, 200);
	CHECK(estimate.Tick == 77);
}

TEST_CASE(
	"weather clocks belong to layers and local receipt state is discarded on load", "[scene][weather]"
) {
	RegisterWeatherClockComponents();
	engine::ecs::Store store("clock_lifecycle");
	store.AdvanceTick(10);
	const auto first = store.Create();
	const auto second = store.Create();
	WeatherClock clock;
	clock.Tick = 200;
	clock.Sequence = 1;
	store.Set(first, clock);
	clock.Tick = 400;
	store.Set(second, clock);
	SynchronizeWeatherClocks(store, 1234, 0.1);
	CHECK(WeatherClockOf(store, first).Tick == 201);
	CHECK(WeatherClockOf(store, second).Tick == 401);
	CHECK(store.Time().Elapsed == 10);
	engine::core::ByteWriter snapshot;
	REQUIRE(store.Save(snapshot));
	engine::ecs::Store restored("clock_restored");
	engine::core::ByteReader reader(snapshot.Bytes());
	REQUIRE(restored.Apply(reader, engine::ecs::ApplyMode::Authoritative));
	CHECK(WeatherClockOf(restored, first).Tick == 200);
	CHECK_FALSE(restored.Get<WeatherClockEstimate>(first)->Ready);
	CHECK(WeatherClockOf(store, engine::ecs::NULL_ENTITY).Tick == 0);
	store.Destroy(first);
	CHECK(WeatherClockOf(store, first).Tick == 0);
	CHECK(WeatherClockOf(store, second).Tick == 401);
}

TEST_CASE("reliable weather anchors refuse truncation and repeated or stale epochs", "[scene][weather]") {
	RegisterWeatherClockComponents();
	engine::ecs::Store store("reliable_weather_anchor");
	const auto provider = store.Create();
	WeatherClock clock;
	clock.Tick = 100;
	clock.Sequence = 1;
	store.Set(provider, clock);
	store.SetAdoptOnly(true);
	clock.Sequence = 2;
	clock.Paused = true;
	clock.WorldPaused = true;
	engine::core::ByteWriter update;
	WriteWeatherClockUpdate(update, provider, clock);
	REQUIRE(AcceptWeatherClockUpdate(store, update.Bytes(), 5, 0.2));
	CHECK(WeatherClockOf(store, provider).Tick == 100);
	CHECK(WeatherClockOf(store, provider).WorldPaused);
	const auto history = store.Get<WeatherClockEstimate>(provider)->HistoryRevision;
	REQUIRE(AcceptWeatherClockUpdate(store, update.Bytes(), 6, 0.2));
	CHECK(store.Get<WeatherClockEstimate>(provider)->HistoryRevision == history);
	clock.Epoch = 2;
	clock.Sequence = 1;
	clock.Tick = 200;
	update.Clear();
	WriteWeatherClockUpdate(update, provider, clock);
	REQUIRE(AcceptWeatherClockUpdate(store, update.Bytes().first(update.Size() - 1), 7, 0.2));
	CHECK(WeatherClockOf(store, provider).Tick == 100);
	REQUIRE(AcceptWeatherClockUpdate(store, update.Bytes(), 7, 0.2));
	CHECK(WeatherClockOf(store, provider).Tick == 200);
	clock.Epoch = 1;
	clock.Sequence = 1000;
	update.Clear();
	WriteWeatherClockUpdate(update, provider, clock);
	REQUIRE(AcceptWeatherClockUpdate(store, update.Bytes(), 8, 0.2));
	CHECK(WeatherClockOf(store, provider).Tick == 200);
	engine::core::ByteWriter unrelated;
	unrelated.WriteString("other.application.message");
	CHECK_FALSE(AcceptWeatherClockUpdate(store, unrelated.Bytes(), 8, 0.2));
	CHECK(store.Time().Elapsed == 0);
}

TEST_CASE(
	"pause anchors arriving before a joining provider retain their final copied state", "[scene][weather]"
) {
	RegisterWeatherClockComponents();
	engine::ecs::Store store("weather_join_inbox");
	engine::ecs::Store authority("weather_join_source");
	const auto provider = authority.Create();
	WeatherClock sample;
	sample.Tick = 456;
	sample.Sequence = 2;
	sample.Paused = true;
	sample.WorldPaused = true;
	engine::core::ByteWriter message;
	WriteWeatherClockUpdate(message, provider, sample);
	REQUIRE(AcceptWeatherClockUpdate(store, message.Bytes(), 0, 0.2));
	REQUIRE(store.Resource<WeatherClockInbox>());
	CHECK(store.Resource<WeatherClockInbox>()->Count == 1);
	REQUIRE(AcceptWeatherClockUpdate(store, message.Bytes(), 0.1, 0.2));
	CHECK(store.Resource<WeatherClockInbox>()->Count == 1);
	REQUIRE(store.CreateAt(provider));
	REQUIRE(store.Alive(provider));
	WeatherClock captured;
	captured.Tick = 400;
	captured.Sequence = 1;
	store.Set(provider, captured);
	SynchronizeWeatherClocks(store, 5, 0.2);
	CHECK(WeatherClockOf(store, provider).Tick == 456);
	CHECK(WeatherClockOf(store, provider).WorldPaused);
	CHECK(store.Resource<WeatherClockInbox>()->Count == 1);
	CHECK(store.Time().Elapsed == 0);
}

TEST_CASE(
	"weather receipt history is bounded and survives provider retirement without reviving old handles",
	"[scene][weather]"
) {
	RegisterWeatherClockComponents();
	engine::ecs::Store authority("receipt_authority"), replica("receipt_replica");
	engine::ecs::Entity first;
	WeatherClock sample;
	sample.Sequence = 2;
	sample.Tick = 100;
	sample.Paused = true;
	for (uint32_t index = 0; index < 32; ++index) {
		const auto provider = authority.Create();
		if (index == 0) first = provider;
		REQUIRE(replica.CreateAt(provider));
		replica.Set(provider, WeatherClock{});
		engine::core::ByteWriter message;
		WriteWeatherClockUpdate(message, provider, sample);
		REQUIRE(AcceptWeatherClockUpdate(replica, message.Bytes(), index, 0));
	}
	REQUIRE(replica.Resource<WeatherClockInbox>()->Count == 32);
	replica.Destroy(first);
	authority.Destroy(first);
	const auto replacement = authority.Create();
	REQUIRE(replacement != first);
	REQUIRE(replica.CreateAt(replacement));
	replica.Set(replacement, WeatherClock{});
	engine::core::ByteWriter replacementMessage;
	WriteWeatherClockUpdate(replacementMessage, replacement, sample);
	REQUIRE(AcceptWeatherClockUpdate(replica, replacementMessage.Bytes(), 33, 0));
	CHECK(replica.Resource<WeatherClockInbox>()->Count == 32);
	auto retiredSample = sample;
	retiredSample.Epoch = 99;
	retiredSample.Tick = 999;
	engine::core::ByteWriter retiredMessage;
	WriteWeatherClockUpdate(retiredMessage, first, retiredSample);
	REQUIRE(AcceptWeatherClockUpdate(replica, retiredMessage.Bytes(), 34, 0));
	SynchronizeWeatherClocks(replica, 34, 0);
	CHECK(WeatherClockOf(replica, replacement).Tick == 100);
	CHECK_FALSE(replica.Alive(first));
	CHECK(replica.Resource<WeatherClockInbox>()->Count == 32);
	engine::core::ByteWriter saved;
	REQUIRE(replica.Save(saved));
	engine::ecs::Store loaded("loaded_receipts");
	engine::core::ByteReader reader(saved.Bytes());
	REQUIRE(loaded.Load(reader));
	CHECK(loaded.Resource<WeatherClockInbox>()->Count == 0);
	CHECK(WeatherClockOf(loaded, replacement).Tick == 100);
}
