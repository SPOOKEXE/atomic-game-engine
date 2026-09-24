#include "../src/PresentationReplyQueue.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("server.presentationreplyqueue")
TEST_DEPENDS("engine.world.presentationstream")

namespace {
	engine::world::PresentationMessage Reply(uint64_t sequence) {
		engine::world::PresentationMessage message;
		message.From = {"server.world", "portal-image-requests", 1, 1};
		message.To = {"client.world", "portal-image-replies/67", 2, 2};
		message.Sequence = sequence;
		message.Correlation = sequence;
		message.Payload = {std::byte{0x31}};
		return message;
	}
}

TEST_CASE(
	"full player presentation stream retains replies until flush makes room",
	"[server][presentation-reply-queue]"
) {
	using namespace engine::world;
	PresentationStream sender, receiver;
	for (uint64_t sequence = 1; sequence <= 64; ++sequence) {
		PresentationStreamFrame frame;
		frame.Message = Reply(sequence);
		REQUIRE(sender.Queue(frame) == PresentationStatus::Ok);
	}
	std::deque<PresentationMessage> pending;
	pending.push_back(Reply(65));
	pending.push_back(Reply(66));
	const auto blocked = server::DrainPresentationReplies(pending, sender);
	CHECK(blocked.Full);
	CHECK(blocked.Queued == 0);
	CHECK(blocked.Invalid == 0);
	REQUIRE(pending.size() == 2);
	CHECK(pending.front().Correlation == 65);

	while (sender.Outgoing().Messages) {
		REQUIRE(sender.Flush([&](auto packet) {
			return receiver.Receive(packet) == PresentationStreamReceive::Accepted;
		}) > 0);
	}
	const auto first = receiver.Take();
	REQUIRE(first.size() == 64);
	for (size_t i = 0; i < first.size(); ++i)
		CHECK(first[i].Message.Correlation == i + 1);

	const auto retried = server::DrainPresentationReplies(pending, sender);
	CHECK_FALSE(retried.Full);
	CHECK(retried.Queued == 2);
	CHECK(retried.Invalid == 0);
	CHECK(pending.empty());
	while (sender.Outgoing().Messages) {
		REQUIRE(sender.Flush([&](auto packet) {
			return receiver.Receive(packet) == PresentationStreamReceive::Accepted;
		}) > 0);
	}
	const auto last = receiver.Take();
	REQUIRE(last.size() == 2);
	CHECK(last[0].Message.Correlation == 65);
	CHECK(last[1].Message.Correlation == 66);
}

TEST_CASE(
	"retired client receipts and closed streams clear pending presentation replies",
	"[server][presentation-reply-queue]"
) {
	using namespace engine::world;
	std::deque<PresentationMessage> pending;
	pending.push_back(Reply(1));
	auto replacement = Reply(2);
	replacement.To.Generation++;
	pending.push_back(replacement);
	PresentationDirectory current;
	current.Endpoints.push_back(replacement.To);
	CHECK(server::RetirePresentationReplies(pending, current) == 1);
	REQUIRE(pending.size() == 1);
	CHECK(pending.front().Correlation == 2);

	PresentationStream stream;
	CHECK(server::RetireClosedPresentationReplies(pending, stream) == 0);
	stream.Close();
	CHECK(server::RetireClosedPresentationReplies(pending, stream) == 1);
	CHECK(pending.empty());
}

TEST_CASE(
	"invalid presentation reply does not prevent a following valid reply",
	"[server][presentation-reply-queue]"
) {
	using namespace engine::world;
	std::deque<PresentationMessage> pending;
	auto invalid = Reply(1);
	invalid.To.Channel.clear();
	pending.push_back(std::move(invalid));
	pending.push_back(Reply(2));
	PresentationStream stream;
	const auto drained = server::DrainPresentationReplies(pending, stream);
	CHECK(drained.Invalid == 1);
	CHECK(drained.Queued == 1);
	CHECK_FALSE(drained.Full);
	CHECK(pending.empty());
	CHECK(stream.Outgoing().Messages == 1);
}

TEST_CASE(
	"transfer eye replies preempt routine endpoint heads without reordering either endpoint",
	"[server][presentation-reply-queue]"
) {
	using namespace engine::world;
	PresentationStream sender, receiver;
	std::deque<PresentationMessage> pending;
	auto routine = Reply(1);
	routine.Payload.resize(128u * 1024u, std::byte{0x41});
	pending.push_back(std::move(routine));
	auto laterRoutine = Reply(2);
	laterRoutine.Payload.resize(128u * 1024u, std::byte{0x42});
	pending.push_back(std::move(laterRoutine));

	const auto firstDrain = server::DrainPresentationReplies(pending, sender);
	CHECK(firstDrain.Queued == 1);
	CHECK(firstDrain.RoutineDeferred == 1);
	REQUIRE(pending.size() == 1);
	CHECK(sender.Outgoing().Bytes >= server::ROUTINE_PRESENTATION_WINDOW_BYTES);

	// The urgent reply arrives after routine work was already deferred. It must be
	// collected and selected without waiting for the routine endpoint to drain.
	auto eye = Reply(1);
	eye.To = {"client.world", "portal-image-replies/68", 2, 2};
	eye.Priority = PresentationPriority::TransferEye;
	pending.push_back(std::move(eye));
	const auto secondDrain = server::DrainPresentationReplies(pending, sender);
	CHECK(secondDrain.Queued == 1);
	CHECK(secondDrain.TransferEyes == 1);
	CHECK(secondDrain.RoutineDeferred == 1);
	REQUIRE(pending.size() == 1);
	CHECK(pending.front().Correlation == 2);

	while (sender.Outgoing().Messages) {
		REQUIRE(sender.Flush([&](auto packet) {
			return receiver.Receive(packet) == PresentationStreamReceive::Accepted;
		}) > 0);
	}
	const auto received = receiver.Take();
	REQUIRE(received.size() == 2);
	CHECK(received[0].Message.To.Channel == "portal-image-replies/67");
	CHECK(received[0].Message.Correlation == 1);
	CHECK(received[1].Message.To.Channel == "portal-image-replies/68");
	CHECK(received[1].Message.Priority == PresentationPriority::TransferEye);
}

TEST_CASE(
	"transfer eye priority never passes an earlier reply to its endpoint",
	"[server][presentation-reply-queue]"
) {
	using namespace engine::world;
	PresentationStream sender, receiver;
	std::deque<PresentationMessage> pending;
	pending.push_back(Reply(1));
	auto eye = Reply(2);
	eye.Priority = PresentationPriority::TransferEye;
	pending.push_back(std::move(eye));

	const auto drained = server::DrainPresentationReplies(pending, sender);
	CHECK(drained.Queued == 2);
	CHECK(drained.TransferEyes == 1);
	CHECK(pending.empty());
	while (sender.Outgoing().Messages) {
		REQUIRE(sender.Flush([&](auto packet) {
			return receiver.Receive(packet) == PresentationStreamReceive::Accepted;
		}) > 0);
	}
	const auto received = receiver.Take();
	REQUIRE(received.size() == 2);
	CHECK(received[0].Message.Correlation == 1);
	CHECK(received[1].Message.Correlation == 2);
}
