#include <engine/core/Random.hpp>
#include <engine/parallel/Channel.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

TEST_SUITE_ID("engine.parallel.channel")

using engine::core::Random;
using engine::parallel::Channel;
using engine::parallel::ChannelSettings;
using engine::parallel::ChannelStatus;
using engine::parallel::MakeLocalChannel;

namespace channel_test {
	std::vector<std::byte> Bytes(std::string_view text) {
		std::vector<std::byte> frame(text.size());
		std::memcpy(frame.data(), text.data(), text.size());
		return frame;
	}

	std::string Text(const std::vector<std::byte> &frame) {
		return std::string(reinterpret_cast<const char *>(frame.data()), frame.size());
	}

	// A frame of `size` bytes whose contents are a function of `seed`, so a
	// mismatch says which frame rather than only that one was wrong.
	std::vector<std::byte> Pattern(uint32_t seed, size_t size) {
		std::vector<std::byte> frame(size);
		for (size_t index = 0; index < size; index++) {
			frame[index] = static_cast<std::byte>(Random::Bits(seed, static_cast<uint32_t>(index)));
		}
		return frame;
	}
}

using namespace channel_test;

TEST_CASE("a frame goes in whole and comes out whole", "[channel]") {
	auto [left, right] = MakeLocalChannel();

	REQUIRE(left->Send(Bytes("hello")) == ChannelStatus::Ok);

	std::vector<std::byte> frame;
	REQUIRE(right->Receive(frame) == ChannelStatus::Ok);
	REQUIRE(Text(frame) == "hello");
}

TEST_CASE("frames arrive in the order they were sent", "[channel]") {
	// Ordering is the property the whole bus design rests on: the barrier
	// applies traffic in a defined order, and a transport that reordered would
	// undo that after the fact.
	auto [left, right] = MakeLocalChannel();

	for (int index = 0; index < 100; index++) {
		REQUIRE(left->Send(Bytes(std::to_string(index))) == ChannelStatus::Ok);
	}

	for (int index = 0; index < 100; index++) {
		std::vector<std::byte> frame;
		REQUIRE(right->Receive(frame) == ChannelStatus::Ok);
		REQUIRE(Text(frame) == std::to_string(index));
	}
}

TEST_CASE("the two directions are independent", "[channel]") {
	// A full inbound queue must not stop the outbound one, or a busy host would
	// silence the driver that is trying to tell it to slow down.
	auto [left, right] = MakeLocalChannel();

	REQUIRE(left->Send(Bytes("to-right")) == ChannelStatus::Ok);
	REQUIRE(right->Send(Bytes("to-left")) == ChannelStatus::Ok);

	std::vector<std::byte> frame;
	REQUIRE(right->Receive(frame) == ChannelStatus::Ok);
	REQUIRE(Text(frame) == "to-right");

	REQUIRE(left->Receive(frame) == ChannelStatus::Ok);
	REQUIRE(Text(frame) == "to-left");
}

TEST_CASE("an empty channel reports Empty rather than blocking", "[channel]") {
	// A send or receive that waited would stall a job worker, and with it every
	// other world in the host.
	auto [left, right] = MakeLocalChannel();

	std::vector<std::byte> frame;
	REQUIRE(right->Receive(frame) == ChannelStatus::Empty);
	REQUIRE(right->Pending() == 0);
	REQUIRE(right->PendingBytes() == 0);
}

TEST_CASE("an empty frame is a frame", "[channel]") {
	// Not a sentinel for "nothing". A bus signal with no payload is a real
	// message and has to arrive as one.
	auto [left, right] = MakeLocalChannel();

	REQUIRE(left->Send({}) == ChannelStatus::Ok);
	REQUIRE(right->Pending() == 1);

	std::vector<std::byte> frame{std::byte{0xAB}};
	REQUIRE(right->Receive(frame) == ChannelStatus::Ok);
	REQUIRE(frame.empty());
}

TEST_CASE("pending counts track what is queued", "[channel]") {
	auto [left, right] = MakeLocalChannel();

	left->Send(Bytes("aaa"));
	left->Send(Bytes("bbbbb"));

	REQUIRE(right->Pending() == 2);
	REQUIRE(right->PendingBytes() == 8);

	std::vector<std::byte> frame;
	right->Receive(frame);
	REQUIRE(right->Pending() == 1);
	REQUIRE(right->PendingBytes() == 5);
}

// --- bounds ---------------------------------------------------------------

TEST_CASE("a frame past the maximum is refused whole", "[channel]") {
	// Refused rather than truncated: half a frame is worse than none, because
	// the reader cannot tell it is half.
	ChannelSettings settings;
	settings.MaximumFrame = 16;
	auto [left, right] = MakeLocalChannel(settings);

	REQUIRE(left->Send(Pattern(1, 17)) == ChannelStatus::TooLarge);
	REQUIRE(right->Pending() == 0);

	// And the boundary itself is accepted.
	REQUIRE(left->Send(Pattern(2, 16)) == ChannelStatus::Ok);
}

TEST_CASE("a full channel refuses rather than growing", "[channel]") {
	// A producer that outran its consumer would otherwise grow the queue until
	// the host died, which is a crash a long way from its cause.
	ChannelSettings settings;
	settings.Capacity = 100;
	auto [left, right] = MakeLocalChannel(settings);

	size_t accepted = 0;
	for (int attempt = 0; attempt < 50; attempt++) {
		if (left->Send(Pattern(static_cast<uint32_t>(attempt), 10)) == ChannelStatus::Ok) {
			accepted++;
		}
	}

	REQUIRE(accepted == 10);
	REQUIRE(right->PendingBytes() == 100);
	REQUIRE(left->Send(Bytes("x")) == ChannelStatus::Full);
}

TEST_CASE("draining a full channel makes room again", "[channel]") {
	ChannelSettings settings;
	settings.Capacity = 30;
	auto [left, right] = MakeLocalChannel(settings);

	REQUIRE(left->Send(Pattern(1, 30)) == ChannelStatus::Ok);
	REQUIRE(left->Send(Bytes("x")) == ChannelStatus::Full);

	std::vector<std::byte> frame;
	REQUIRE(right->Receive(frame) == ChannelStatus::Ok);
	REQUIRE(left->Send(Bytes("x")) == ChannelStatus::Ok);
}

// --- closing --------------------------------------------------------------

TEST_CASE("closing one end stops sends from both", "[channel]") {
	auto [left, right] = MakeLocalChannel();

	left->Close();

	REQUIRE_FALSE(left->Open());
	REQUIRE_FALSE(right->Open());
	REQUIRE(left->Send(Bytes("x")) == ChannelStatus::Closed);
	REQUIRE(right->Send(Bytes("x")) == ChannelStatus::Closed);
}

TEST_CASE("a closed channel still drains what was already queued", "[channel]") {
	// A host that exits cleanly should not strip the driver of the last thing
	// it said.
	auto [left, right] = MakeLocalChannel();

	left->Send(Bytes("first"));
	left->Send(Bytes("second"));
	left->Close();

	std::vector<std::byte> frame;
	REQUIRE(right->Receive(frame) == ChannelStatus::Ok);
	REQUIRE(Text(frame) == "first");
	REQUIRE(right->Receive(frame) == ChannelStatus::Ok);
	REQUIRE(Text(frame) == "second");

	// Only once drained does it report closed.
	REQUIRE(right->Receive(frame) == ChannelStatus::Closed);
}

TEST_CASE("closing twice is harmless", "[channel]") {
	auto [left, right] = MakeLocalChannel();
	left->Close();
	left->Close();
	right->Close();
	REQUIRE_FALSE(left->Open());
}

TEST_CASE("an endpoint outliving its peer does not crash", "[channel]") {
	// The peer's destructor closes it. The survivor still has to be usable —
	// a supervisor holding a channel to a host that died must be able to notice
	// rather than fault.
	auto [left, right] = MakeLocalChannel();

	left->Send(Bytes("last words"));
	right.reset();

	REQUIRE_FALSE(left->Open());
	REQUIRE(left->Send(Bytes("x")) == ChannelStatus::Closed);

	std::vector<std::byte> frame;
	REQUIRE(left->Receive(frame) == ChannelStatus::Closed);
}

// --- concurrency ----------------------------------------------------------

TEST_CASE("a producer and a consumer on different threads agree", "[channel]") {
	// The realistic shape: a host thread writing while the driver polls. Every
	// frame must arrive whole, once, and in order.
	auto [left, right] = MakeLocalChannel();

	constexpr int FRAMES = 5'000;
	std::atomic<bool> done{false};

	std::thread producer([&, sender = left.get()] {
		for (int index = 0; index < FRAMES;) {
			if (sender->Send(Bytes(std::to_string(index))) == ChannelStatus::Ok) {
				index++;
			}
		}
		done.store(true, std::memory_order_release);
	});

	std::vector<std::string> received;
	std::vector<std::byte> frame;
	while (received.size() < FRAMES) {
		if (right->Receive(frame) == ChannelStatus::Ok) {
			received.push_back(Text(frame));
		} else if (done.load(std::memory_order_acquire) && right->Pending() == 0) {
			break;
		}
	}
	producer.join();

	REQUIRE(received.size() == FRAMES);
	size_t outOfOrder = 0;
	for (int index = 0; index < FRAMES; index++) {
		if (received[static_cast<size_t>(index)] != std::to_string(index)) {
			outOfOrder++;
		}
	}
	REQUIRE(outOfOrder == 0);
}

TEST_CASE("both directions under load stay separate", "[channel]") {
	auto [left, right] = MakeLocalChannel();

	constexpr int FRAMES = 2'000;
	std::atomic<size_t> leftReceived{0};
	std::atomic<size_t> rightReceived{0};

	const auto pump = [](Channel *channel, const char *tag, int count, std::atomic<size_t> &counter) {
		int sent = 0;
		std::vector<std::byte> frame;

		while (sent < count || counter.load() < static_cast<size_t>(count)) {
			if (sent < count &&
				channel->Send(Bytes(std::string(tag) + std::to_string(sent))) == ChannelStatus::Ok) {
				sent++;
			}
			if (channel->Receive(frame) == ChannelStatus::Ok) {
				counter.fetch_add(1, std::memory_order_relaxed);
			}
		}
	};

	std::thread first([&] { pump(left.get(), "L", FRAMES, leftReceived); });
	pump(right.get(), "R", FRAMES, rightReceived);
	first.join();

	REQUIRE(leftReceived.load() == FRAMES);
	REQUIRE(rightReceived.load() == FRAMES);
}

// --- fuzz -----------------------------------------------------------------

TEST_CASE("random frames round-trip byte for byte", "[channel][fuzz]") {
	// `core::Random` so a failure reproduces from the seed on any machine.
	auto [left, right] = MakeLocalChannel();

	constexpr uint32_t FRAMES = 3'000;
	size_t mismatches = 0;

	for (uint32_t index = 0; index < FRAMES; index++) {
		const size_t size = Random::Bits(index, 7) % 512;
		const std::vector<std::byte> sent = Pattern(index, size);

		REQUIRE(left->Send(sent) == ChannelStatus::Ok);

		std::vector<std::byte> got;
		REQUIRE(right->Receive(got) == ChannelStatus::Ok);

		if (got != sent) {
			mismatches++;
		}
	}

	REQUIRE(mismatches == 0);
}

TEST_CASE("random interleaving never loses or duplicates a frame", "[channel][fuzz]") {
	// Sends and receives mixed arbitrarily, with the queue filling and draining
	// under a small cap so the Full path is exercised too.
	//
	// Each frame carries its own sequence number in its first four bytes rather
	// than being identified by searching for a matching pattern. The search
	// version of this case was quadratic over a SHA-256-backed generator and
	// took eight minutes; the identity belongs *in* the frame.
	ChannelSettings settings;
	settings.Capacity = 4'096;
	auto [left, right] = MakeLocalChannel(settings);

	const auto stamp = [](uint32_t value) {
		std::vector<std::byte> frame(32);
		std::memcpy(frame.data(), &value, sizeof(value));
		for (size_t index = sizeof(value); index < frame.size(); index++) {
			frame[index] = static_cast<std::byte>(value + index);
		}
		return frame;
	};

	const auto recover = [](const std::vector<std::byte> &frame) {
		uint32_t value = 0;
		std::memcpy(&value, frame.data(), sizeof(value));
		return value;
	};

	uint32_t next = 0;
	std::vector<uint32_t> received;
	std::vector<std::byte> frame;

	for (uint32_t step = 0; step < 20'000; step++) {
		if (Random::Bits(step, 11) % 2 == 0) {
			if (left->Send(stamp(next)) == ChannelStatus::Ok) {
				next++;
			}
		} else if (right->Receive(frame) == ChannelStatus::Ok) {
			REQUIRE(frame.size() == 32);
			received.push_back(recover(frame));
		}
	}

	while (right->Receive(frame) == ChannelStatus::Ok) {
		received.push_back(recover(frame));
	}

	// Every frame sent arrived exactly once, in order, and none was invented.
	REQUIRE(received.size() == next);
	size_t wrong = 0;
	for (uint32_t index = 0; index < next; index++) {
		if (received[index] != index) {
			wrong++;
		}
	}
	REQUIRE(wrong == 0);
}
