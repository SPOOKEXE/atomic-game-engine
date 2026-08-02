// The channel that actually crosses a process boundary.
//
// `Channel.cpp` covers the queue semantics against the local implementation.
// What is worth checking here is only what changes when the bytes go through
// the kernel: partial writes, partial reads, a frame larger than a socket
// buffer, and both ends noticing the other going away — which is the property
// a supervisor's heartbeat is ultimately built on.
//
// The child is this test binary run again with a hidden case. Spawning a system
// tool would make these depend on what is installed and where; the one
// executable certain to exist is the one running.

#include <engine/core/Paths.hpp>
#include <engine/parallel/Channel.hpp>
#include <engine/parallel/Process.hpp>
#include <engine/parallel/ProcessChannel.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

TEST_SUITE_ID("engine.parallel.processchannel")

using engine::parallel::AdoptInheritedChannel;
using engine::parallel::Channel;
using engine::parallel::ChannelSettings;
using engine::parallel::ChannelStatus;
using engine::parallel::HasInheritedChannel;
using engine::parallel::MakeProcessChannel;
using engine::parallel::Process;
using engine::parallel::ProcessChannel;

namespace process_channel_test {
	std::vector<std::byte> Bytes(std::string_view text) {
		std::vector<std::byte> frame(text.size());
		if (!text.empty()) {
			std::memcpy(frame.data(), text.data(), text.size());
		}
		return frame;
	}

	std::string Text(std::span<const std::byte> frame) {
		return std::string(reinterpret_cast<const char *>(frame.data()), frame.size());
	}

	std::filesystem::path Self() {
#if defined(_WIN32)
		return engine::core::Paths::Base() / "test_parallel.exe";
#else
		return engine::core::Paths::Base() / "test_parallel";
#endif
	}

	// Receives one frame, polling until it arrives or the deadline passes.
	//
	// A poll rather than a wait, because the interface never blocks — which is
	// the whole reason a world tick can hold a channel without stalling its
	// neighbours.
	ChannelStatus Await(
		Channel &channel,
		std::vector<std::byte> &frame,
		std::chrono::milliseconds limit = std::chrono::seconds(10)
	) {
		const auto deadline = std::chrono::steady_clock::now() + limit;
		while (true) {
			const ChannelStatus status = channel.Receive(frame);
			if (status != ChannelStatus::Empty) {
				return status;
			}
			if (std::chrono::steady_clock::now() >= deadline) {
				return ChannelStatus::Empty;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}
}

using namespace process_channel_test;

// The child. Hidden from a normal run by the leading dot in its tag, and named
// on the command line by the cases that spawn it.
TEST_CASE("process channel echo child", "[.child]") {
	auto channel = AdoptInheritedChannel();
	if (channel == nullptr) {
		// Run directly rather than spawned. Nothing to do, and saying so beats
		// failing a case a person ran on purpose.
		return;
	}

	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
	std::vector<std::byte> frame;

	while (std::chrono::steady_clock::now() < deadline) {
		const ChannelStatus status = channel->Receive(frame);
		if (status == ChannelStatus::Closed) {
			break;
		}
		if (status == ChannelStatus::Empty) {
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			continue;
		}

		if (Text(frame) == "bye") {
			break;
		}
		channel->Send(frame);
	}

	// Deliberate: the driver's side has to see the close, and the destructor
	// running at exit is what a real host relies on too.
	channel->Close();
}

TEST_CASE("a spawned child receives, answers, and closes", "[parallel]") {
	ProcessChannel pair = MakeProcessChannel();
	REQUIRE(pair.Valid());

	Process child;
	REQUIRE(child.Start(Self(), {"process channel echo child"}, std::move(pair.Remote)));

	// Ordinary frames, in order.
	for (int index = 0; index < 16; index++) {
		const std::string sent = "frame-" + std::to_string(index);
		REQUIRE(pair.Local->Send(Bytes(sent)) == ChannelStatus::Ok);

		std::vector<std::byte> back;
		REQUIRE(Await(*pair.Local, back) == ChannelStatus::Ok);
		REQUIRE(Text(back) == sent);
	}

	REQUIRE(pair.Local->Send(Bytes("bye")) == ChannelStatus::Ok);

	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (child.Poll().Alive() && std::chrono::steady_clock::now() < deadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	REQUIRE_FALSE(child.Poll().Alive());
}

TEST_CASE("an empty frame arrives as an empty frame", "[parallel]") {
	// The case a length-prefixed stream gets wrong by treating zero as nothing
	// to send. A heartbeat with no payload is exactly this.
	ProcessChannel pair = MakeProcessChannel();
	REQUIRE(pair.Valid());

	Process child;
	REQUIRE(child.Start(Self(), {"process channel echo child"}, std::move(pair.Remote)));

	REQUIRE(pair.Local->Send({}) == ChannelStatus::Ok);

	std::vector<std::byte> back;
	back.push_back(std::byte{0xAB}); // must be cleared by the receive
	REQUIRE(Await(*pair.Local, back) == ChannelStatus::Ok);
	REQUIRE(back.empty());

	pair.Local->Send(Bytes("bye"));
}

TEST_CASE("a frame larger than the socket buffer crosses whole", "[parallel]") {
	// The partial-write case, which is the one a stream socket forces and the
	// one a caller must never see. A megabyte is far past any socket buffer, so
	// this cannot pass by accident.
	ProcessChannel pair = MakeProcessChannel();
	REQUIRE(pair.Valid());

	Process child;
	REQUIRE(child.Start(Self(), {"process channel echo child"}, std::move(pair.Remote)));

	std::vector<std::byte> big(4u * 1024u * 1024u);
	for (size_t index = 0; index < big.size(); index++) {
		big[index] = static_cast<std::byte>(index * 31u);
	}

	REQUIRE(pair.Local->Send(big) == ChannelStatus::Ok);

	std::vector<std::byte> back;
	REQUIRE(Await(*pair.Local, back, std::chrono::seconds(30)) == ChannelStatus::Ok);
	REQUIRE(back.size() == big.size());
	REQUIRE(back == big);

	pair.Local->Send(Bytes("bye"));
}

TEST_CASE("many frames pipelined at once all arrive, in order", "[parallel]") {
	// Sent without waiting for any answer, so the socket buffer fills and the
	// implementation has to hold the rest. If the outbound buffer were dropped
	// or reordered this is where it shows.
	ProcessChannel pair = MakeProcessChannel();
	REQUIRE(pair.Valid());

	Process child;
	REQUIRE(child.Start(Self(), {"process channel echo child"}, std::move(pair.Remote)));

	constexpr int COUNT = 500;
	for (int index = 0; index < COUNT; index++) {
		const std::string sent = std::to_string(index) + std::string(index % 97, 'x');
		REQUIRE(pair.Local->Send(Bytes(sent)) == ChannelStatus::Ok);
	}

	for (int index = 0; index < COUNT; index++) {
		std::vector<std::byte> back;
		REQUIRE(Await(*pair.Local, back, std::chrono::seconds(30)) == ChannelStatus::Ok);
		REQUIRE(Text(back) == std::to_string(index) + std::string(index % 97, 'x'));
	}

	pair.Local->Send(Bytes("bye"));
}

TEST_CASE("a frame past the maximum is refused rather than truncated", "[parallel]") {
	ChannelSettings settings;
	settings.MaximumFrame = 1024;

	ProcessChannel pair = MakeProcessChannel(settings);
	REQUIRE(pair.Valid());

	const std::vector<std::byte> big(2048);
	REQUIRE(pair.Local->Send(big) == ChannelStatus::TooLarge);

	// Still usable. A refused frame is not a broken channel.
	REQUIRE(pair.Local->Open());
	REQUIRE(pair.Local->Send(Bytes("small")) == ChannelStatus::Ok);
}

TEST_CASE("a driver notices a host that died", "[parallel]") {
	// What a heartbeat deadline is the fallback for, not the primary signal. A
	// host that segfaults closes its handles as it dies, and the driver's end
	// should say so on the next call rather than at the next deadline.
	ProcessChannel pair = MakeProcessChannel();
	REQUIRE(pair.Valid());

	Process child;
	REQUIRE(child.Start(Self(), {"process channel echo child"}, std::move(pair.Remote)));

	// Confirm it is up before killing it, so the case cannot pass by racing.
	REQUIRE(pair.Local->Send(Bytes("hello")) == ChannelStatus::Ok);
	std::vector<std::byte> back;
	REQUIRE(Await(*pair.Local, back) == ChannelStatus::Ok);

	REQUIRE(child.Kill());
	child.Wait();

	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (pair.Local->Open() && std::chrono::steady_clock::now() < deadline) {
		std::vector<std::byte> ignored;
		pair.Local->Receive(ignored);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}

	REQUIRE_FALSE(pair.Local->Open());
	REQUIRE(pair.Local->Send(Bytes("nobody there")) == ChannelStatus::Closed);
}

TEST_CASE("a host notices a driver that died", "[parallel]") {
	// The other direction, and the one that needs close-on-exec to be right: if
	// the child inherited a copy of the driver's own end, the socket would
	// still have a peer after the driver went away and the host would wait
	// forever for a message from a process that no longer exists.
	ProcessChannel pair = MakeProcessChannel();
	REQUIRE(pair.Valid());

	Process child;
	REQUIRE(child.Start(Self(), {"process channel echo child"}, std::move(pair.Remote)));

	REQUIRE(pair.Local->Send(Bytes("hello")) == ChannelStatus::Ok);
	std::vector<std::byte> back;
	REQUIRE(Await(*pair.Local, back) == ChannelStatus::Ok);

	// The driver goes away without saying goodbye.
	pair.Local.reset();

	// The child's `Receive` reports `Closed`, so its loop ends and it exits.
	// If it hangs here, a copy of the driver's end survived the spawn.
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
	while (child.Poll().Alive() && std::chrono::steady_clock::now() < deadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	REQUIRE_FALSE(child.Poll().Alive());
}

TEST_CASE("a process with no inherited channel says so", "[parallel]") {
	// This binary, run by the test runner, has nothing at the slot. A false
	// positive here would make an ordinary program try to speak a host protocol
	// down whatever handle it happened to have open.
	REQUIRE_FALSE(HasInheritedChannel());
	REQUIRE(AdoptInheritedChannel() == nullptr);
}
