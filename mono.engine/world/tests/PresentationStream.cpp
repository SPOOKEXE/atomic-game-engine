#include <engine/testing/Suite.hpp>
#include <engine/world/PresentationStream.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.world.presentationstream")
TEST_DEPENDS("engine.world.presentationbus")

namespace {
	using namespace engine;
	using namespace engine::world;
	PresentationStreamFrame Image(size_t bytes = 128 * 128 * 8) {
		PresentationStreamFrame frame;
		frame.Message.From = {"far", "images", 10, 1};
		frame.Message.To = {"near", "replies", 20, 1};
		frame.Message.Sequence = 1;
		frame.Message.Correlation = 17;
		frame.Message.Payload.resize(bytes);
		for (size_t i = 0; i < bytes; ++i)
			frame.Message.Payload[i] = static_cast<std::byte>(i % 251);
		return frame;
	}
	std::vector<std::vector<std::byte>> Packets(PresentationStream &stream) {
		std::vector<std::vector<std::byte>> packets;
		while (stream.Outgoing().Messages) {
			REQUIRE(stream.Flush([&](auto bytes) {
				packets.emplace_back(bytes.begin(), bytes.end());
				return true;
			}) > 0);
		}
		return packets;
	}
}

TEST_CASE(
	"presentation stream carries large images and retries packet backpressure exactly",
	"[world][presentation-stream]"
) {
	PresentationStream sender, receiver;
	const auto original = Image();
	REQUIRE(sender.Queue(original) == PresentationStatus::Ok);
	std::vector<std::byte> refused;
	size_t accepted = 0;
	REQUIRE(sender.Flush([&](auto packet) {
		CHECK(packet.size() <= PresentationStream::PACKET_BYTES);
		if (accepted == 3) {
			refused.assign(packet.begin(), packet.end());
			return false;
		}
		accepted++;
		REQUIRE(receiver.Receive(packet) == PresentationStreamReceive::Accepted);
		return true;
	}) == 3);
	CHECK(receiver.Take().empty());
	CHECK(receiver.Incoming().Messages == 1);
	CHECK(sender.Outgoing().Messages == 1);
	bool retry = true;
	while (sender.Outgoing().Messages) {
		const auto count = sender.Flush(
			[&](auto packet) {
				if (retry) {
					CHECK(std::vector<std::byte>(packet.begin(), packet.end()) == refused);
					retry = false;
				}
				REQUIRE(receiver.Receive(packet) == PresentationStreamReceive::Accepted);
				return true;
			},
			5
		);
		REQUIRE(count > 0);
		CHECK(count <= 5);
	}
	const auto received = receiver.Take();
	REQUIRE(received.size() == 1);
	CHECK(received[0].Message.From == original.Message.From);
	CHECK(received[0].Message.To == original.Message.To);
	CHECK(received[0].Message.Sequence == original.Message.Sequence);
	CHECK(received[0].Message.Correlation == original.Message.Correlation);
	CHECK(received[0].Message.Payload == original.Message.Payload);
	CHECK(receiver.Incoming().Bytes == 0);
	CHECK(sender.Outgoing().Bytes == 0);
}

TEST_CASE(
	"presentation stream preserves directory and route receipt sessions in order",
	"[world][presentation-stream]"
) {
	PresentationStream sender, receiver;
	PresentationStreamFrame directory;
	directory.Kind = PresentationStreamKind::Directory;
	directory.Directory = {10, 2, {{"far", "images", 10, 1}}};
	REQUIRE(sender.Queue(directory) == PresentationStatus::Ok);
	auto routes = directory;
	routes.Kind = PresentationStreamKind::Routes;
	routes.Directory.Endpoints.push_back({"near", "replies", 20, 1});
	REQUIRE(sender.Queue(routes) == PresentationStatus::Ok);
	REQUIRE(sender.Queue(Image(0)) == PresentationStatus::Ok);
	for (const auto &packet : Packets(sender))
		REQUIRE(receiver.Receive(packet) == PresentationStreamReceive::Accepted);
	const auto received = receiver.Take();
	REQUIRE(received.size() == 3);
	CHECK(received[0].Kind == PresentationStreamKind::Directory);
	CHECK(received[0].Directory == directory.Directory);
	CHECK(received[1].Kind == PresentationStreamKind::Routes);
	CHECK(received[1].Directory == routes.Directory);
	CHECK(received[2].Kind == PresentationStreamKind::Message);
}

TEST_CASE(
	"presentation stream bounds send receive and partial assembly memory", "[world][presentation-stream]"
) {
	PresentationStream sender, receiver;
	const auto tiny = Image(0);
	for (size_t i = 0; i < 64; ++i)
		REQUIRE(sender.Queue(tiny) == PresentationStatus::Ok);
	CHECK(sender.Queue(tiny) == PresentationStatus::Full);
	for (const auto &packet : Packets(sender))
		REQUIRE(receiver.Receive(packet) == PresentationStreamReceive::Accepted);
	CHECK(receiver.Incoming().Messages == 64);
	REQUIRE(sender.Queue(tiny) == PresentationStatus::Ok);
	const auto overflow = Packets(sender);
	REQUIRE(overflow.size() == 1);
	CHECK(receiver.Receive(overflow[0]) == PresentationStreamReceive::Refused);
	CHECK_FALSE(receiver.Open());
	CHECK(receiver.Incoming().Bytes == 0);
	CHECK(receiver.Take().empty());
	CHECK(sender.Queue(Image(MAX_PRESENTATION_PAYLOAD + 1)) == PresentationStatus::Invalid);
	const auto large = Image(MAX_PRESENTATION_PAYLOAD);
	for (size_t i = 0; i < 7; ++i)
		REQUIRE(sender.Queue(large) == PresentationStatus::Ok);
	CHECK(sender.Queue(large) == PresentationStatus::Full);
	CHECK(sender.Outgoing().Bytes <= 32u * 1024u * 1024u);
	sender.Close();
	CHECK(sender.Outgoing().Bytes == 0);
	CHECK(sender.Queue(tiny) == PresentationStatus::Invalid);
}

TEST_CASE(
	"presentation stream refuses reordered altered and oversized fragments without authority changes",
	"[world][presentation-stream]"
) {
	PresentationStream sender, receiver;
	REQUIRE(sender.Queue(Image(2048)) == PresentationStatus::Ok);
	auto packets = Packets(sender);
	REQUIRE(packets.size() > 1);
	SECTION("wrong sequence") {
		packets[0][4] = std::byte{2};
	}
	SECTION("oversized allocation claim") {
		packets[0][15] = std::byte{255};
	}
	SECTION("missing first fragment") {
		packets[0] = packets[1];
	}
	SECTION("truncated header") {
		packets[0].resize(8);
	}
	SECTION("altered total mid frame") {
		REQUIRE(receiver.Receive(packets[0]) == PresentationStreamReceive::Accepted);
		packets[0] = packets[1];
		packets[0][12] ^= std::byte{1};
	}
	CHECK(receiver.Receive(packets[0]) == PresentationStreamReceive::Refused);
	CHECK_FALSE(receiver.Open());
	CHECK(receiver.Refused() == 1);
	CHECK(receiver.Incoming().Bytes == 0);
	CHECK(receiver.Take().empty());
}

TEST_CASE("presentation stream leaves unrelated play messages untouched", "[world][presentation-stream]") {
	PresentationStream stream;
	CHECK(stream.Receive({}) == PresentationStreamReceive::Other);
	CHECK(stream.Receive(std::vector<std::byte>(32, std::byte{5})) == PresentationStreamReceive::Other);
	CHECK(stream.Open());
	CHECK(stream.Refused() == 0);
}

TEST_CASE(
	"presentation stream rejects unsupported inner commands after bounded assembly",
	"[world][presentation-stream]"
) {
	PresentationStream sender, receiver;
	REQUIRE(sender.Queue(Image(2048)) == PresentationStatus::Ok);
	auto packets = Packets(sender);
	packets[0][20] = std::byte{255};
	for (size_t i = 0; i < packets.size(); ++i) {
		const auto expected = i + 1 == packets.size() ? PresentationStreamReceive::Refused
													  : PresentationStreamReceive::Accepted;
		CHECK(receiver.Receive(packets[i]) == expected);
	}
	CHECK_FALSE(receiver.Open());
	CHECK(receiver.Take().empty());
	CHECK(receiver.Incoming().Bytes == 0);
}
