#include <engine/core/Name.hpp>
#include <engine/core/Random.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/ViewChannel.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

TEST_SUITE_ID("engine.world.viewchannel")

using engine::core::Name;
using engine::core::Random;
using engine::world::ViewChannel;
using engine::world::ViewHeader;

namespace view_test {
	std::vector<std::byte> Bytes(std::string_view text) {
		std::vector<std::byte> payload(text.size());
		std::memcpy(payload.data(), text.data(), text.size());
		return payload;
	}

	std::string Text(const std::vector<std::byte> &payload) {
		return std::string(reinterpret_cast<const char *>(payload.data()), payload.size());
	}

	ViewHeader Frame(uint64_t tick, float alpha = 0.0f) {
		ViewHeader header;
		header.World = Name("view.world");
		header.SourceTick = tick;
		header.Alpha = alpha;
		return header;
	}
}

using namespace view_test;

TEST_CASE("a fresh channel has nothing to take", "[world]") {
	ViewChannel channel(1024);

	REQUIRE_FALSE(channel.Ready());
	REQUIRE(channel.Frames() == 0);
	REQUIRE(channel.Dropped() == 0);

	ViewHeader header;
	std::vector<std::byte> payload;
	REQUIRE_FALSE(channel.Acquire(header, payload));
}

TEST_CASE("a published frame comes back whole", "[world]") {
	ViewChannel channel(1024);

	REQUIRE(channel.Publish(Frame(42, 0.25f), Bytes("draw-list")));
	REQUIRE(channel.Ready());

	ViewHeader header;
	std::vector<std::byte> payload;
	REQUIRE(channel.Acquire(header, payload));

	REQUIRE(header.World == Name("view.world"));
	REQUIRE(header.SourceTick == 42);
	REQUIRE(header.Alpha == 0.25f);
	REQUIRE(header.PayloadBytes == 9);
	REQUIRE(Text(payload) == "draw-list");
}

TEST_CASE("a frame is taken once", "[world]") {
	ViewChannel channel(1024);
	channel.Publish(Frame(1), Bytes("one"));

	ViewHeader header;
	std::vector<std::byte> payload;
	REQUIRE(channel.Acquire(header, payload));

	// The compositor draws what it took; asking again before the producer has
	// published must not hand it the same frame a second time.
	REQUIRE_FALSE(channel.Acquire(header, payload));
	REQUIRE_FALSE(channel.Ready());
}

TEST_CASE("serials are assigned by the channel and always advance", "[world]") {
	// Filled in here rather than by the caller, so they are monotonic whatever
	// the producer does - and a consumer seeing a repeat can skip the work.
	ViewChannel channel(1024);

	ViewHeader written = Frame(1);
	written.Serial = 999; // ignored

	channel.Publish(written, {});

	ViewHeader header;
	std::vector<std::byte> payload;
	REQUIRE(channel.Acquire(header, payload));
	const uint32_t first = header.Serial;
	REQUIRE(first != 999);

	channel.Publish(Frame(2), {});
	REQUIRE(channel.Acquire(header, payload));
	REQUIRE(header.Serial > first);
}

TEST_CASE("a slow consumer takes the newest frame and drops the rest", "[world]") {
	// The whole point of the triple buffer: a compositor that cannot keep up
	// misses frames rather than throttling the world producing them.
	ViewChannel channel(1024);

	for (uint64_t tick = 1; tick <= 10; tick++) {
		REQUIRE(channel.Publish(Frame(tick), Bytes(std::to_string(tick))));
	}

	ViewHeader header;
	std::vector<std::byte> payload;
	REQUIRE(channel.Acquire(header, payload));

	// The newest, not the oldest and not a queue of ten.
	REQUIRE(header.SourceTick == 10);
	REQUIRE(Text(payload) == "10");

	REQUIRE(channel.Frames() == 10);
	REQUIRE(channel.Dropped() == 9);
	REQUIRE_FALSE(channel.Acquire(header, payload));
}

TEST_CASE("publishing never blocks and never fails for want of room", "[world]") {
	// A producer stalled on a full buffer would be a client stalling a
	// simulation, which the frame budget cannot absorb.
	ViewChannel channel(64);

	for (int index = 0; index < 10'000; index++) {
		REQUIRE(channel.Publish(Frame(static_cast<uint64_t>(index)), Bytes("x")));
	}

	REQUIRE(channel.Frames() == 10'000);
	REQUIRE(channel.Dropped() == 9'999);
}

TEST_CASE("an oversized payload is refused rather than truncated", "[world]") {
	ViewChannel channel(16);

	std::vector<std::byte> big(17);
	REQUIRE_FALSE(channel.Publish(Frame(1), big));
	REQUIRE_FALSE(channel.Ready());
	REQUIRE(channel.Frames() == 0);

	// The boundary itself is accepted.
	std::vector<std::byte> exact(16);
	REQUIRE(channel.Publish(Frame(1), exact));
}

TEST_CASE("an empty payload is a frame", "[world]") {
	// A view with nothing to draw is still a view: the compositor needs to know
	// the world is alive and empty rather than gone.
	ViewChannel channel(1024);
	REQUIRE(channel.Publish(Frame(7), {}));

	ViewHeader header;
	std::vector<std::byte> payload{std::byte{1}};
	REQUIRE(channel.Acquire(header, payload));

	REQUIRE(payload.empty());
	REQUIRE(header.PayloadBytes == 0);
	REQUIRE(header.SourceTick == 7);
}

TEST_CASE("the consumer's buffer keeps its capacity", "[world]") {
	// A compositor acquiring every frame must not allocate every frame.
	ViewChannel channel(4096);

	ViewHeader header;
	std::vector<std::byte> payload;

	channel.Publish(Frame(1), std::vector<std::byte>(4096));
	REQUIRE(channel.Acquire(header, payload));
	const size_t grown = payload.capacity();
	const void *address = payload.data();

	channel.Publish(Frame(2), std::vector<std::byte>(4096));
	REQUIRE(channel.Acquire(header, payload));

	REQUIRE(payload.capacity() == grown);
	REQUIRE(payload.data() == address);
}

// --- concurrency ----------------------------------------------------------

TEST_CASE("a producer and a consumer never tear a frame", "[world]") {
	// The property the whole scheme rests on: the consumer either sees a frame
	// whole or does not see it. A slot handed back to the producer mid-read
	// would show up as a payload that does not match its own header.
	ViewChannel channel(512);

	std::atomic<bool> stop{false};
	std::atomic<size_t> torn{0};
	std::atomic<size_t> taken{0};

	std::thread producer([&] {
		for (uint64_t tick = 1; !stop.load(std::memory_order_acquire); tick++) {
			// Every byte of the payload is derived from the tick, so a frame
			// assembled from two different publishes is detectable.
			std::vector<std::byte> payload(256, static_cast<std::byte>(tick & 0xFF));
			channel.Publish(Frame(tick), payload);
		}
	});

	ViewHeader header;
	std::vector<std::byte> payload;

	while (taken.load() < 20'000) {
		if (!channel.Acquire(header, payload)) {
			continue;
		}
		taken.fetch_add(1, std::memory_order_relaxed);

		const auto expected = static_cast<std::byte>(header.SourceTick & 0xFF);
		for (const std::byte value : payload) {
			if (value != expected) {
				torn.fetch_add(1, std::memory_order_relaxed);
				break;
			}
		}
	}

	stop.store(true, std::memory_order_release);
	producer.join();

	REQUIRE(taken.load() >= 20'000);
	REQUIRE(torn.load() == 0);
}

TEST_CASE("serials never go backwards under contention", "[world]") {
	// A consumer skipping repeated work relies on this, and it is the property
	// most likely to break if the slot bookkeeping is wrong.
	ViewChannel channel(128);

	std::atomic<bool> stop{false};
	std::thread producer([&] {
		for (uint64_t tick = 1; !stop.load(std::memory_order_acquire); tick++) {
			channel.Publish(Frame(tick), std::vector<std::byte>(64));
		}
	});

	ViewHeader header;
	std::vector<std::byte> payload;

	uint32_t previous = 0;
	size_t backwards = 0;
	size_t seen = 0;

	while (seen < 10'000) {
		if (!channel.Acquire(header, payload)) {
			continue;
		}
		if (header.Serial <= previous) {
			backwards++;
		}
		previous = header.Serial;
		seen++;
	}

	stop.store(true, std::memory_order_release);
	producer.join();

	REQUIRE(backwards == 0);
}

TEST_CASE("counts add up under contention", "[world]") {
	// Everything published was either taken or dropped. A discrepancy means a
	// slot was lost, which would eventually starve the producer.
	ViewChannel channel(64);

	std::atomic<bool> stop{false};
	std::atomic<size_t> taken{0};

	std::thread consumer([&] {
		ViewHeader header;
		std::vector<std::byte> payload;
		while (!stop.load(std::memory_order_acquire)) {
			if (channel.Acquire(header, payload)) {
				taken.fetch_add(1, std::memory_order_relaxed);
			}
		}
		// Drain whatever is left after the producer stopped.
		while (channel.Acquire(header, payload)) {
			taken.fetch_add(1, std::memory_order_relaxed);
		}
	});

	constexpr uint64_t FRAMES = 50'000;
	for (uint64_t tick = 1; tick <= FRAMES; tick++) {
		channel.Publish(Frame(tick), {});
	}

	stop.store(true, std::memory_order_release);
	consumer.join();

	REQUIRE(channel.Frames() == FRAMES);
	REQUIRE(taken.load() + channel.Dropped() >= FRAMES - 1);
	REQUIRE(taken.load() <= FRAMES);
}

// --- fuzz -----------------------------------------------------------------

TEST_CASE("random payloads round-trip byte for byte", "[world][fuzz]") {
	ViewChannel channel(1024);
	size_t mismatches = 0;

	for (uint32_t index = 0; index < 5'000; index++) {
		const size_t size = Random::Bits(index, 3) % 1024;
		std::vector<std::byte> sent(size);
		for (size_t at = 0; at < size; at++) {
			sent[at] = static_cast<std::byte>(Random::Bits(index, static_cast<uint32_t>(at) + 5));
		}

		REQUIRE(channel.Publish(Frame(index), sent));

		ViewHeader header;
		std::vector<std::byte> got;
		REQUIRE(channel.Acquire(header, got));

		if (got != sent || header.PayloadBytes != size) {
			mismatches++;
		}
	}

	REQUIRE(mismatches == 0);
}

TEST_CASE("random publish and acquire interleavings stay consistent", "[world][fuzz]") {
	ViewChannel channel(256);

	uint64_t published = 0;
	uint64_t lastSeen = 0;
	size_t regressions = 0;

	ViewHeader header;
	std::vector<std::byte> payload;

	for (uint32_t step = 0; step < 30'000; step++) {
		if (Random::Bits(step, 17) % 2 == 0) {
			published++;
			channel.Publish(Frame(published), std::vector<std::byte>(8));
		} else if (channel.Acquire(header, payload)) {
			// Never older than something already seen, and never newer than
			// anything published.
			if (header.SourceTick < lastSeen || header.SourceTick > published) {
				regressions++;
			}
			lastSeen = header.SourceTick;
		}
	}

	REQUIRE(regressions == 0);
	REQUIRE(channel.Frames() == published);
}
