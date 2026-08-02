#include <engine/core/FrameGraph.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/net/Link.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string_view>
#include <vector>

TEST_SUITE_ID("engine.net.link")
TEST_DEPENDS("engine.net.packet")
TEST_DEPENDS("engine.core.framegraph")
TEST_DEPENDS("engine.core.metrics")

using engine::core::FrameGraph;
using engine::core::Metrics;
using engine::net::ChannelKind;
using engine::net::ConnectionId;
using engine::net::ConnectionState;
using engine::net::DisconnectReason;
using engine::net::Link;
using engine::net::LinkSettings;
using engine::net::PacketHeader;

namespace {
	constexpr ConnectionId ID{1, 1};

	LinkSettings Quick() {
		LinkSettings settings;
		settings.HandshakeTimeoutSeconds = 2.0;
		settings.IdleTimeoutSeconds = 5.0;
		settings.KeepAliveSeconds = 1.0;
		settings.BytesPerTick = 1000;
		settings.PacketsPerTick = 4;
		return settings;
	}

	// Connected at t=0, so a case about anything else does not restate the
	// handshake.
	Link Connected(const LinkSettings &settings = Quick()) {
		Link link(ID, 0.0, settings);
		REQUIRE(link.CompleteHandshake(0.0));
		return link;
	}

	PacketHeader Arrival(uint16_t sequence, ChannelKind channel = ChannelKind::Unreliable) {
		PacketHeader header;
		header.Channel = channel;
		header.Sequence = sequence;
		return header;
	}
}

TEST_CASE("a link opens connecting and completes its handshake", "[net][link]") {
	Link link(ID, 0.0, Quick());

	CHECK(link.State() == ConnectionState::Connecting);
	CHECK(link.Reason() == DisconnectReason::None);
	CHECK(link.Id() == ID);

	REQUIRE(link.CompleteHandshake(0.5));
	CHECK(link.State() == ConnectionState::Connected);
}

TEST_CASE("a handshake does not complete twice", "[net][link]") {
	Link link = Connected();

	// Not idempotent, deliberately: a second completion is either a replayed
	// packet or two code paths both thinking they own the transition, and
	// quietly accepting it hides both.
	CHECK_FALSE(link.CompleteHandshake(1.0));
	CHECK(link.State() == ConnectionState::Connected);
}

TEST_CASE("a handshake that never completes times out", "[net][link]") {
	Link link(ID, 0.0, Quick());

	link.Advance(1.9);
	CHECK(link.State() == ConnectionState::Connecting);

	link.Advance(2.0);
	CHECK(link.State() == ConnectionState::Disconnected);
	CHECK(link.Reason() == DisconnectReason::HandshakeFailed);
}

TEST_CASE("a silent connection times out", "[net][link]") {
	Link link = Connected();

	link.Advance(4.9);
	CHECK(link.State() == ConnectionState::Connected);

	link.Advance(5.0);
	CHECK(link.State() == ConnectionState::Disconnected);
	CHECK(link.Reason() == DisconnectReason::TimedOut);
}

TEST_CASE("traffic keeps a connection alive", "[net][link]") {
	Link link = Connected();

	for (double now = 1.0; now < 20.0; now += 1.0) {
		CHECK(link.OnPacket(Arrival(static_cast<uint16_t>(now)), 16, now));
		link.Advance(now);
	}

	CHECK(link.State() == ConnectionState::Connected);
}

TEST_CASE("a refused packet still proves the peer is alive", "[net][link]") {
	Link link = Connected();

	CHECK(link.OnPacket(Arrival(10), 16, 1.0));

	// Stale, so the payload is not acted on — but something arrived, and the
	// timeout is about the peer rather than about whether its last packet was
	// useful.
	CHECK_FALSE(link.OnPacket(Arrival(5), 16, 4.0));
	link.Advance(6.0);
	CHECK(link.State() == ConnectionState::Connected);
}

TEST_CASE("a graceful disconnect passes through disconnecting", "[net][link]") {
	Link link = Connected();

	REQUIRE(link.Disconnect(DisconnectReason::Requested));

	// Not straight to Disconnected: a goodbye has to be able to reach the far
	// side, or every clean exit costs the other end a full idle timeout.
	CHECK(link.State() == ConnectionState::Disconnecting);
	CHECK(link.Reason() == DisconnectReason::Requested);

	link.Close(DisconnectReason::Requested);
	CHECK(link.State() == ConnectionState::Disconnected);
}

TEST_CASE("a disconnect does not happen twice and always has a reason", "[net][link]") {
	Link link = Connected();

	CHECK_FALSE(link.Disconnect(DisconnectReason::None));
	CHECK(link.State() == ConnectionState::Connected);

	REQUIRE(link.Disconnect(DisconnectReason::Shutdown));
	CHECK_FALSE(link.Disconnect(DisconnectReason::Requested));
	CHECK(link.Reason() == DisconnectReason::Shutdown);
}

TEST_CASE("the reason a disconnect started with wins", "[net][link]") {
	Link link = Connected();

	REQUIRE(link.Disconnect(DisconnectReason::BudgetExceeded));

	// Close reports that the goodbye finished; it does not re-decide why it
	// started.
	link.Close(DisconnectReason::Requested);
	CHECK(link.Reason() == DisconnectReason::BudgetExceeded);
}

TEST_CASE("a peer that stops answering mid-goodbye is still timed out", "[net][link]") {
	Link link = Connected();
	REQUIRE(link.Disconnect(DisconnectReason::Requested));

	// The graceful path is exactly where it is easiest to forget the timeout,
	// and a slot held open forever is the cost.
	link.Advance(10.0);
	CHECK(link.State() == ConnectionState::Disconnected);
}

TEST_CASE("a stale unreliable packet is refused and counted", "[net][link]") {
	Link link = Connected();

	CHECK(link.OnPacket(Arrival(10), 8, 1.0));
	CHECK(link.OnPacket(Arrival(11), 8, 1.0));

	// Applying it would move the world backwards: the newer one is already in.
	CHECK_FALSE(link.OnPacket(Arrival(9), 8, 1.0));
	CHECK(link.Stats().PacketsStale == 1);
}

TEST_CASE("a late reliable packet is still delivered", "[net][link]") {
	Link link = Connected();

	CHECK(link.OnPacket(Arrival(10, ChannelKind::Reliable), 8, 1.0));
	CHECK(link.OnPacket(Arrival(11, ChannelKind::Reliable), 8, 1.0));

	// A reliable packet arriving late is a resend that still has to be
	// delivered in order. Discarding it would silently drop an event the sender
	// believes was acknowledged.
	CHECK(link.OnPacket(Arrival(9, ChannelKind::Reliable), 8, 1.0));
	CHECK(link.Stats().PacketsStale == 0);
}

TEST_CASE("a gap in sequences is counted as loss", "[net][link]") {
	Link link = Connected();

	CHECK(link.OnPacket(Arrival(1), 8, 1.0));
	CHECK(link.OnPacket(Arrival(5), 8, 1.0));

	// Three never arrived: 2, 3 and 4.
	CHECK(link.Stats().PacketsLost == 3);
}

TEST_CASE("a late arrival corrects the loss estimate", "[net][link]") {
	Link link = Connected();

	CHECK(link.OnPacket(Arrival(1), 8, 1.0));
	CHECK(link.OnPacket(Arrival(5), 8, 1.0));
	REQUIRE(link.Stats().PacketsLost == 3);

	// It was counted lost when the gap opened. It arrived after all, so the
	// estimate is corrected rather than left to overstate loss for the life of
	// the connection.
	CHECK_FALSE(link.OnPacket(Arrival(3), 8, 1.0));
	CHECK(link.Stats().PacketsLost == 2);
}

TEST_CASE("the byte budget is enforced and its overflow is visible", "[net][link]") {
	Link link = Connected();

	CHECK(link.Reserve(600));
	CHECK(link.Reserve(400));

	// Spent. Without the counter an enforced budget and a congested link look
	// identical from a game's point of view.
	CHECK_FALSE(link.Reserve(1));
	CHECK(link.Stats().SendsOverBudget == 1);

	link.ResetBudget();
	CHECK(link.Reserve(1000));
}

TEST_CASE("the packet budget is enforced separately from the byte budget", "[net][link]") {
	Link link = Connected();

	// Four packets of one byte: no bandwidth at all, and all the per-packet
	// overhead the packet budget exists to bound.
	for (int index = 0; index < 4; ++index) {
		CHECK(link.Reserve(1));
	}
	CHECK_FALSE(link.Reserve(1));
	CHECK(link.Stats().SendsOverBudget == 1);
}

TEST_CASE("an oversized send is refused rather than fragmented", "[net][link]") {
	LinkSettings generous = Quick();
	generous.BytesPerTick = 1'000'000;
	Link link = Connected(generous);

	using engine::net::Packet;

	// A fragmented datagram is lost entirely when any one fragment is, which
	// multiplies the loss rate the unreliable channel is designed around.
	CHECK(link.Reserve(Packet::MAXIMUM_PAYLOAD_BYTES));
	CHECK_FALSE(link.Reserve(Packet::MAXIMUM_PAYLOAD_BYTES + 1));
}

TEST_CASE("nothing may be sent before or after the connection", "[net][link]") {
	Link connecting(ID, 0.0, Quick());
	CHECK_FALSE(connecting.Reserve(10));

	Link link = Connected();
	REQUIRE(link.Disconnect(DisconnectReason::Requested));
	CHECK_FALSE(link.Reserve(10));
}

TEST_CASE("each channel has its own sequence", "[net][link]") {
	Link link = Connected();

	const auto firstUnreliable = link.NextHeader(ChannelKind::Unreliable);
	const auto firstReliable = link.NextHeader(ChannelKind::Reliable);
	const auto secondUnreliable = link.NextHeader(ChannelKind::Unreliable);

	// A reliable resend must not make an unreliable packet look stale, which is
	// what one shared counter would do.
	CHECK(firstUnreliable.Sequence == 0);
	CHECK(firstReliable.Sequence == 0);
	CHECK(secondUnreliable.Sequence == 1);
	CHECK(firstUnreliable.Channel == ChannelKind::Unreliable);
	CHECK(firstReliable.Channel == ChannelKind::Reliable);
}

TEST_CASE("an outgoing header carries the acknowledgement", "[net][link]") {
	Link link = Connected();

	CHECK(link.OnPacket(Arrival(40), 8, 1.0));
	CHECK(link.OnPacket(Arrival(42), 8, 1.0));

	// Rides every packet rather than travelling as its own message: an
	// acknowledgement needing a packet of its own doubles the packet rate of a
	// conversation that is mostly one-way, and a game is mostly one-way.
	const auto header = link.NextHeader(ChannelKind::Unreliable);
	CHECK(header.Acknowledge == 42);
	CHECK((header.AcknowledgeBits & 0x2u) != 0);
}

TEST_CASE("a quiet connection asks for a keep-alive", "[net][link]") {
	Link link = Connected();

	CHECK_FALSE(link.NeedsKeepAlive(0.5));
	CHECK(link.NeedsKeepAlive(1.0));

	link.OnSent(0, 1.0);
	CHECK_FALSE(link.NeedsKeepAlive(1.5));
	CHECK(link.NeedsKeepAlive(2.0));
}

TEST_CASE("a link that is not connected asks for no keep-alive", "[net][link]") {
	Link connecting(ID, 0.0, Quick());
	CHECK_FALSE(connecting.NeedsKeepAlive(100.0));
}

TEST_CASE("totals accumulate in both directions", "[net][link]") {
	Link link = Connected();

	CHECK(link.OnPacket(Arrival(1), 100, 1.0));
	CHECK(link.OnPacket(Arrival(2), 50, 1.0));
	link.OnSent(200, 1.0);

	CHECK(link.Stats().PacketsReceived == 2);
	CHECK(link.Stats().BytesReceived == 150);
	CHECK(link.Stats().PacketsSent == 1);
	CHECK(link.Stats().BytesSent == 200);
}

TEST_CASE("time since the last receive is reported", "[net][link]") {
	Link link = Connected();

	CHECK(link.OnPacket(Arrival(1), 8, 1.0));
	link.Advance(3.0);
	CHECK(link.Stats().SinceLastReceiveSeconds == 2.0f);

	CHECK(link.OnPacket(Arrival(2), 8, 3.5));
	CHECK(link.Stats().SinceLastReceiveSeconds == 0.0f);
}

TEST_CASE("invalid settings fall back to the defaults", "[net][link]") {
	LinkSettings backwards = Quick();
	backwards.KeepAliveSeconds = 100.0; // longer than the idle timeout

	// One lost keep-alive would otherwise drop a healthy connection.
	Link link(ID, 0.0, backwards);
	CHECK(link.Settings().KeepAliveSeconds == LinkSettings{}.KeepAliveSeconds);

	CHECK(LinkSettings{}.IsValid());
	CHECK_FALSE(backwards.IsValid());

	LinkSettings zeroed;
	zeroed.BytesPerTick = 0;
	CHECK_FALSE(zeroed.IsValid());
}

TEST_CASE("a closed link ignores everything", "[net][link]") {
	Link link = Connected();
	link.Close(DisconnectReason::Shutdown);

	CHECK_FALSE(link.OnPacket(Arrival(1), 8, 1.0));
	CHECK_FALSE(link.Reserve(10));
	CHECK(link.State() == ConnectionState::Disconnected);

	// Advancing a dead link does nothing rather than re-deciding its reason.
	link.Advance(1000.0);
	CHECK(link.Reason() == DisconnectReason::Shutdown);
}

TEST_CASE("the link reports itself to the frame graph and the metrics sink", "[net][link][framegraph]") {
	Metrics::Clear();
	FrameGraph::SetEnabled(true);
	FrameGraph::BeginFrame();

	Link link(ID, 0.0, Quick());
	REQUIRE(link.CompleteHandshake(0.0));
	CHECK(link.OnPacket(Arrival(1), 8, 0.1));
	CHECK(link.Reserve(1000));
	CHECK_FALSE(link.Reserve(1));
	link.Close(DisconnectReason::Requested);

	FrameGraph::EndFrame();
	const std::vector<engine::core::FrameSpan> spans(FrameGraph::Spans().begin(), FrameGraph::Spans().end());
	FrameGraph::SetEnabled(false);

	CHECK(std::any_of(spans.begin(), spans.end(), [](const auto &span) {
		return span.Name == "Link::OnPacket";
	}));

	const auto counters = Metrics::Drain();
	const auto total = [&counters](std::string_view name) {
		double sum = 0.0;
		for (const auto &counter : counters) {
			if (counter.Name == engine::core::Name(name)) {
				sum += counter.Value;
			}
		}
		return sum;
	};

	CHECK(total("net.link.connected") == 1.0);
	CHECK(total("net.link.closed") == 1.0);
	CHECK(total("net.link.overbudget") == 1.0);
}
