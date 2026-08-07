// The rate arithmetic behind the Network panel.
//
// **Reachable headlessly because none of it needs a frame**, which is the same
// split `cdn::Dashboard` draws against its terminal: the panel is imgui and the
// thing that can be *wrong* is a ring buffer and a division. A rate that reads
// low, reads zero while a transfer is moving, or spikes on the frame the panel
// opens is a number somebody makes a decision on, and none of those failures
// needs a window to reproduce.

#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <studio/Editor.hpp>

TEST_SUITE_ID("studio.network")

using studio::NetworkRates;
using studio::NetworkSamples;

TEST_CASE("there is no rate until there is a window", "[studio][network]") {
	NetworkSamples samples;

	// **Zero samples and one sample both answer "no window".** A panel drawing
	// a zero here would be saying "idle", which is a different claim from "not
	// measured yet" and the wrong one on the frame it opens.
	CHECK(samples.Rates().WindowSeconds == 0.0);

	samples.Observe(0.0, 1000, 0);
	CHECK(samples.Rates().WindowSeconds == 0.0);
	CHECK(samples.Rates().DownPerSecond == 0.0);
}

TEST_CASE("the first observation seeds rather than measuring", "[studio][network]") {
	// **A client that has been running for a while has a large total**, and a
	// ring whose first sample counted all of it would show one enormous spike
	// on the frame the panel is first opened — which is exactly the moment
	// somebody is deciding whether to believe the number.
	NetworkSamples samples;
	samples.Observe(120.0, 500'000'000, 0);
	samples.Observe(121.0, 500'000'000, 0);

	const NetworkRates rates = samples.Rates();
	CHECK(rates.WindowSeconds == Catch::Approx(1.0));
	CHECK(rates.DownPerSecond == Catch::Approx(0.0));
}

TEST_CASE("a rate is what crossed inside the window", "[studio][network]") {
	NetworkSamples samples;
	for (int second = 0; second <= 4; second++) {
		samples.Observe(static_cast<double>(second), static_cast<uint64_t>(second) * 2048, 0);
	}

	const NetworkRates rates = samples.Rates();
	CHECK(rates.WindowSeconds == Catch::Approx(4.0));
	CHECK(rates.DownPerSecond == Catch::Approx(2048.0));
}

TEST_CASE("samples closer together than the interval are dropped", "[studio][network]") {
	// A panel is drawn sixty times a second and the ring holds seconds, so most
	// calls have to do nothing at all — otherwise the window would be a sixth
	// of a second and the rate would be one frame's noise.
	NetworkSamples samples;
	samples.Observe(0.0, 0, 0);
	for (int frame = 1; frame < 60; frame++) {
		samples.Observe(static_cast<double>(frame) / 60.0, static_cast<uint64_t>(frame) * 10, 0);
	}
	CHECK(samples.Filled == 1);

	samples.Observe(1.0, 600, 0);
	CHECK(samples.Filled == 2);
	CHECK(samples.Rates().DownPerSecond == Catch::Approx(600.0));
}

TEST_CASE("the window stops growing once the ring is full", "[studio][network]") {
	// **Bounded, which is the whole reason it is a ring.** An editor left open
	// for a day must not accumulate a day of samples, and the window it reports
	// must stay the few seconds a person is actually looking at.
	NetworkSamples samples;
	for (int second = 0; second < 100; second++) {
		samples.Observe(static_cast<double>(second), static_cast<uint64_t>(second) * 100, 0);
	}

	CHECK(samples.Filled == NetworkSamples::CAPACITY);

	const NetworkRates rates = samples.Rates();
	CHECK(rates.WindowSeconds == Catch::Approx(static_cast<double>(NetworkSamples::CAPACITY - 1)));

	// Still the right rate: a full ring measures the same slope a partial one
	// does, over a shorter span.
	CHECK(rates.DownPerSecond == Catch::Approx(100.0));
}

TEST_CASE("a transfer that stops reads as zero rather than as its total", "[studio][network]") {
	// **The failure that makes a total useless.** Bytes-since-start divided by
	// uptime never returns to zero, so a panel built on one says a finished
	// download is still moving — for as long as the editor stays open.
	NetworkSamples samples;
	for (int second = 0; second < 4; second++) {
		samples.Observe(static_cast<double>(second), static_cast<uint64_t>(second) * 5000, 0);
	}
	CHECK(samples.Rates().DownPerSecond == Catch::Approx(5000.0));

	// Nothing moves for the length of the ring.
	for (int second = 4; second < 4 + static_cast<int>(NetworkSamples::CAPACITY); second++) {
		samples.Observe(static_cast<double>(second), 15000, 0);
	}
	CHECK(samples.Rates().DownPerSecond == Catch::Approx(0.0));
}

TEST_CASE("up and down are measured apart", "[studio][network]") {
	// One direction being busy must not make the other look busy: an upload
	// running while nothing downloads is the ordinary state of seeding an
	// origin, and a panel that reported it as both would be unreadable.
	NetworkSamples samples;
	for (int second = 0; second < 3; second++) {
		samples.Observe(static_cast<double>(second), 4096, static_cast<uint64_t>(second) * 300);
	}

	const NetworkRates rates = samples.Rates();
	CHECK(rates.DownPerSecond == Catch::Approx(0.0));
	CHECK(rates.UpPerSecond == Catch::Approx(300.0));
}
