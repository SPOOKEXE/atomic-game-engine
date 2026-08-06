#include <engine/core/Metrics.hpp>
#include <engine/core/Random.hpp>
#include <engine/net/Link.hpp>
#include <engine/net/Reliability.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

TEST_SUITE_ID("engine.net.reliability")

using Catch::Approx;
TEST_DEPENDS("engine.net.packet")
TEST_DEPENDS("engine.net.link")
TEST_DEPENDS("engine.core.random")
TEST_DEPENDS("engine.core.metrics")

using engine::core::Metrics;
using engine::core::Random;
using engine::net::ChannelKind;
using engine::net::ConnectionId;
using engine::net::DisconnectReason;
using engine::net::Link;
using engine::net::LinkSettings;
using engine::net::PacketHeader;
using engine::net::ReliabilitySettings;
using engine::net::ReliableReceiver;
using engine::net::ReliableSender;

namespace {
	constexpr ConnectionId ID{1, 1};

	// Short enough that a case states a timeout rather than restating it, and
	// small enough that a bound is reached in a handful of lines.
	ReliabilitySettings Quick() {
		ReliabilitySettings settings;
		settings.RetransmitTimeoutSeconds = 0.1;
		settings.MaximumUnacknowledged = 8;
		settings.MaximumResends = 4;
		settings.MaximumOutOfOrder = 8;
		return settings;
	}

	// A payload that carries which payload it is, so a delivery can be checked
	// for identity rather than only for length.
	std::vector<std::byte> Body(uint32_t value) {
		std::vector<std::byte> bytes(4);
		for (size_t index = 0; index < 4; index++) {
			bytes[index] = static_cast<std::byte>((value >> (index * 8)) & 0xFFu);
		}
		return bytes;
	}

	uint32_t ValueOf(std::span<const std::byte> payload) {
		uint32_t value = 0;
		for (size_t index = 0; index < payload.size() && index < 4; index++) {
			value |= static_cast<uint32_t>(payload[index]) << (index * 8);
		}
		return value;
	}

	// An arriving header carrying nothing but an acknowledgement, which is what
	// a keep-alive from the far side is.
	PacketHeader Ack(uint16_t acknowledge, uint32_t bits = 0) {
		PacketHeader header;
		header.Channel = ChannelKind::Unreliable;
		header.Acknowledge = acknowledge;
		header.AcknowledgeBits = bits;
		return header;
	}

	// The bit that acknowledges `sequence` in a header acknowledging `newest`.
	uint32_t BitFor(uint16_t newest, uint16_t sequence) {
		return 1u << (static_cast<uint16_t>(newest - sequence) - 1);
	}
}

TEST_CASE("a payload is retired by a direct acknowledgement", "[net][reliability]") {
	ReliableSender sender(Quick());
	const std::vector<std::byte> body = Body(7);

	REQUIRE(sender.Track(0, body, 0.0));
	REQUIRE(sender.Waiting() == 1);

	CHECK(sender.OnAcknowledge(Ack(0), 0.0) == 1);
	CHECK(sender.Waiting() == 0);
}

TEST_CASE("a payload is retired by a bit in the acknowledgement window", "[net][reliability]") {
	ReliableSender sender(Quick());
	const std::vector<std::byte> body = Body(1);

	REQUIRE(sender.Track(10, body, 0.0));
	REQUIRE(sender.Track(11, body, 0.0));
	REQUIRE(sender.Track(12, body, 0.0));

	// One packet acknowledges up to 33, which is what stops a single lost
	// acknowledgement forcing a resend of something that did in fact arrive.
	CHECK(sender.OnAcknowledge(Ack(12, BitFor(12, 10)), 0.0) == 2);
	CHECK(sender.Waiting() == 1);

	CHECK(sender.OnAcknowledge(Ack(12, BitFor(12, 11)), 0.0) == 1);
	CHECK(sender.Waiting() == 0);
}

TEST_CASE("an acknowledgement retires nothing it does not name", "[net][reliability]") {
	ReliableSender sender(Quick());
	const std::vector<std::byte> body = Body(1);

	REQUIRE(sender.Track(5, body, 0.0));

	// Older than anything sent, and with an empty window.
	CHECK(sender.OnAcknowledge(Ack(4), 0.0) == 0);

	// Newer, but the bit for 5 is not set — 5 is what was lost.
	CHECK(sender.OnAcknowledge(Ack(9, BitFor(9, 8)), 0.0) == 0);
	CHECK(sender.Waiting() == 1);
}

TEST_CASE("a receiver that has heard nothing acknowledges nothing", "[net][reliability]") {
	ReliableReceiver receiver(Quick());
	ReliableSender sender(Quick());
	REQUIRE(sender.Track(0, Body(1), 0.0));

	// The trap: a window starting at zero acknowledges sequence zero — the
	// first reliable payload a Link ever sends — before it has arrived, and
	// that payload is then never resent. It starts one behind instead.
	const PacketHeader header = receiver.Acknowledging(PacketHeader{});
	CHECK(header.Acknowledge == 0xFFFF);
	CHECK(sender.OnAcknowledge(header, 0.0) == 0);
	CHECK(sender.Waiting() == 1);
}

TEST_CASE("a payload is resent after the timeout and not before it", "[net][reliability]") {
	ReliableSender sender(Quick());
	REQUIRE(sender.Track(3, Body(42), 1.0));

	CHECK(sender.Due(1.09).empty());

	const auto due = sender.Due(1.1);
	REQUIRE(due.size() == 1);

	// Under the sequence it first went out with. A resend under a fresh one is
	// a hole the receiver's ordering waits on until the idle timeout.
	CHECK(due[0].Sequence == 3);
	CHECK(ValueOf(due[0].Payload) == 42);
}

TEST_CASE("a resend restarts the clock and stops once acknowledged", "[net][reliability]") {
	ReliableSender sender(Quick());
	REQUIRE(sender.Track(3, Body(42), 0.0));

	REQUIRE(sender.Due(0.1).size() == 1);
	REQUIRE(sender.OnResent(3, 0.1));
	CHECK(sender.Retransmissions() == 1);

	CHECK(sender.Due(0.15).empty());
	CHECK(sender.Due(0.2).size() == 1);

	REQUIRE(sender.OnAcknowledge(Ack(3), 0.0) == 1);
	CHECK(sender.Due(100.0).empty());
	CHECK(sender.OnResent(3, 100.0) == false);
}

TEST_CASE("a resend the budget refuses is held and offered again", "[net][reliability][link]") {
	LinkSettings paced;
	paced.PacketsPerTick = 1;
	Link link(ID, 0.0, paced);
	REQUIRE(link.CompleteHandshake(0.0));

	ReliableSender sender(Quick());
	REQUIRE(sender.Track(0, Body(1), 0.0));
	REQUIRE(sender.Track(1, Body(2), 0.0));

	// A resend is not exempt from the per-tick budget. The first fits, the
	// second does not, and the second is simply not marked as sent.
	size_t sent = 0;
	for (const auto &due : sender.Due(0.1)) {
		if (!link.Reserve(due.Payload.size())) {
			break;
		}
		REQUIRE(sender.OnResent(due.Sequence, 0.1));
		sent++;
	}

	CHECK(sent == 1);
	CHECK(link.Stats().SendsOverBudget == 1);

	// Its clock never restarted, so it is still due — and still held.
	const auto again = sender.Due(0.1);
	REQUIRE(again.size() == 1);
	CHECK(again[0].Sequence == 1);
	CHECK(sender.Waiting() == 2);
}

TEST_CASE("a full send window is backpressure rather than an ending", "[net][reliability]") {
	ReliableSender sender(Quick());

	for (uint16_t sequence = 0; sequence < 8; sequence++) {
		REQUIRE(sender.HasRoom());
		REQUIRE(sender.Track(sequence, Body(sequence), 0.0));
	}

	// The window is what keeps every resend inside the far side's 33-wide
	// acknowledgement window. Reaching it says nothing about the peer.
	CHECK_FALSE(sender.HasRoom());
	CHECK_FALSE(sender.Track(8, Body(8), 0.0));
	CHECK(sender.Overflow() == DisconnectReason::None);
	CHECK(sender.Waiting() == 8);

	REQUIRE(sender.OnAcknowledge(Ack(0), 0.0) == 1);
	CHECK(sender.HasRoom());
}

TEST_CASE("a peer that never acknowledges ends the connection", "[net][reliability]") {
	ReliableSender sender(Quick());
	REQUIRE(sender.Track(0, Body(1), 0.0));

	double now = 0.0;
	for (uint32_t attempt = 0; attempt < 4; attempt++) {
		now += 0.5;
		REQUIRE(sender.Due(now).size() == 1);
		REQUIRE(sender.OnResent(0, now));
	}

	// Every one of those attempts was inside the window that could have
	// acknowledged it. Holding the backlog longer is holding it forever.
	CHECK(sender.Overflow() == DisconnectReason::TimedOut);
	CHECK(sender.Due(now + 1.0).empty());
	CHECK_FALSE(sender.HasRoom());
}

TEST_CASE("out-of-order payloads are released in order", "[net][reliability]") {
	ReliableReceiver receiver(Quick());

	REQUIRE(receiver.Accept(1, Body(1)));
	REQUIRE(receiver.Accept(2, Body(2)));

	// Nothing at all while the gap ahead is unfilled, however much is queued
	// behind it. That is the whole promise, and why the sender resends.
	CHECK(receiver.Drain().empty());
	CHECK(receiver.Holding() == 2);
	CHECK(receiver.Expecting() == 0);

	REQUIRE(receiver.Accept(0, Body(0)));

	const auto delivered = receiver.Drain();
	REQUIRE(delivered.size() == 3);
	for (size_t index = 0; index < 3; index++) {
		CHECK(delivered[index].Sequence == index);
		CHECK(ValueOf(delivered[index].Payload) == index);
	}
	CHECK(receiver.Holding() == 0);
	CHECK(receiver.Expecting() == 3);
}

TEST_CASE("a duplicate resend is not delivered twice", "[net][reliability]") {
	ReliableReceiver receiver(Quick());

	REQUIRE(receiver.Accept(0, Body(9)));
	REQUIRE(receiver.Drain().size() == 1);

	// The resend of something already delivered, which is what a lost
	// acknowledgement looks like from this end.
	CHECK_FALSE(receiver.Accept(0, Body(9)));
	CHECK(receiver.Drain().empty());
	CHECK(receiver.Duplicates() == 1);

	// And the resend of something already held, before either is delivered.
	REQUIRE(receiver.Accept(2, Body(2)));
	CHECK_FALSE(receiver.Accept(2, Body(2)));
	CHECK(receiver.Holding() == 1);
	CHECK(receiver.Duplicates() == 2);
}

TEST_CASE("a resend is acknowledged even though it is refused", "[net][reliability]") {
	ReliableReceiver receiver(Quick());
	ReliableSender sender(Quick());

	REQUIRE(sender.Track(0, Body(0), 0.0));
	REQUIRE(receiver.Accept(0, Body(0)));
	REQUIRE(receiver.Drain().size() == 1);

	// The far side resends precisely because it has not heard that the
	// original arrived. Refusing the duplicate and also refusing to
	// acknowledge it would leave it resending until it gave up.
	CHECK_FALSE(receiver.Accept(0, Body(0)));
	CHECK(sender.OnAcknowledge(receiver.Acknowledging(PacketHeader{}), 0.0) == 1);
}

TEST_CASE("the receiver acknowledges a run with a gap in it", "[net][reliability]") {
	ReliableReceiver receiver(Quick());
	ReliableSender sender(Quick());

	for (uint16_t sequence = 0; sequence < 4; sequence++) {
		REQUIRE(sender.Track(sequence, Body(sequence), 0.0));
	}

	REQUIRE(receiver.Accept(0, Body(0)));
	REQUIRE(receiver.Accept(1, Body(1)));
	REQUIRE(receiver.Accept(3, Body(3))); // 2 was lost

	const PacketHeader header = receiver.Acknowledging(PacketHeader{});
	CHECK(header.Acknowledge == 3);
	CHECK((header.AcknowledgeBits & BitFor(3, 2)) == 0);

	// Three retired, and 2 left to be resent.
	CHECK(sender.OnAcknowledge(header, 0.0) == 3);
	REQUIRE(sender.Waiting() == 1);
	CHECK(sender.Due(0.1)[0].Sequence == 2);
}

TEST_CASE("the out-of-order queue is bounded", "[net][reliability]") {
	ReliableReceiver receiver(Quick());

	// A peer that sends 1..n and never sends 0 is asking for unbounded
	// storage, and never exceeds a per-tick budget doing it.
	for (uint16_t sequence = 1; sequence <= 8; sequence++) {
		REQUIRE(receiver.Accept(sequence, Body(sequence)));
	}
	REQUIRE(receiver.Holding() == 8);

	CHECK_FALSE(receiver.Accept(9, Body(9)));
	CHECK(receiver.Overflow() == DisconnectReason::BudgetExceeded);
	CHECK(receiver.Holding() == 8);

	// Refused from here on, so the queue cannot grow past the bound even if
	// the caller has not closed the link yet.
	CHECK_FALSE(receiver.Accept(10, Body(10)));
	CHECK(receiver.Holding() == 8);
}

TEST_CASE("the packet the queue is waiting for is taken even when the queue is full", "[net][reliability]") {
	ReliableReceiver receiver(Quick());

	for (uint16_t sequence = 1; sequence <= 8; sequence++) {
		REQUIRE(receiver.Accept(sequence, Body(sequence)));
	}

	// Refusing it would refuse the one packet that empties the queue the bound
	// is about.
	REQUIRE(receiver.Accept(0, Body(0)));
	CHECK(receiver.Drain().size() == 9);
	CHECK(receiver.Overflow() == DisconnectReason::None);
}

TEST_CASE("sequences wrap across 65536", "[net][reliability]") {
	// The case that passes in testing and breaks in a long match: a 16-bit
	// counter wraps every eighteen minutes at sixty packets a second.
	ReliableReceiver receiver(Quick(), 65'533);
	ReliableSender sender(Quick());

	for (uint16_t offset = 0; offset < 6; offset++) {
		REQUIRE(sender.Track(static_cast<uint16_t>(65'533 + offset), Body(offset), 0.0));
	}

	// Delivered out of order, and straddling the wrap.
	REQUIRE(receiver.Accept(2, Body(5)));
	REQUIRE(receiver.Accept(65'535, Body(2)));
	REQUIRE(receiver.Accept(0, Body(3)));
	CHECK(receiver.Drain().empty());

	REQUIRE(receiver.Accept(65'533, Body(0)));
	CHECK(receiver.Drain().size() == 1);

	REQUIRE(receiver.Accept(65'534, Body(1)));
	const auto delivered = receiver.Drain();
	REQUIRE(delivered.size() == 3);
	CHECK(delivered[0].Sequence == 65'534);
	CHECK(delivered[1].Sequence == 65'535);
	CHECK(delivered[2].Sequence == 0);
	CHECK(receiver.Expecting() == 1);

	// And the acknowledgement retires across the wrap too: a plain comparison
	// reads 65'533 as newer than an acknowledgement of 2 and retires nothing
	// for the rest of the connection.
	const PacketHeader header = receiver.Acknowledging(PacketHeader{});
	CHECK(header.Acknowledge == 2);
	CHECK(sender.OnAcknowledge(header, 0.0) == 5);
	REQUIRE(sender.Waiting() == 1);
	CHECK(sender.Due(0.1)[0].Sequence == 1);
}

TEST_CASE("invalid settings fall back to the defaults", "[net][reliability]") {
	ReliabilitySettings wide;
	wide.MaximumUnacknowledged = 64; // wider than the acknowledgement window

	// A window wider than the 32 sequences AcknowledgeBits covers puts the
	// oldest unacknowledged payload outside the only window that can ever
	// acknowledge it.
	CHECK_FALSE(wide.IsValid());
	CHECK(
		ReliableSender(wide).Settings().MaximumUnacknowledged == ReliabilitySettings{}.MaximumUnacknowledged
	);

	ReliabilitySettings instant;
	instant.RetransmitTimeoutSeconds = 0.0;
	CHECK_FALSE(instant.IsValid());
	CHECK(
		ReliableReceiver(instant).Settings().RetransmitTimeoutSeconds ==
		ReliabilitySettings{}.RetransmitTimeoutSeconds
	);

	CHECK(ReliabilitySettings{}.IsValid());
}

TEST_CASE("the resend layer reports itself to the metrics sink", "[net][reliability]") {
	Metrics::Clear();

	ReliableSender sender(Quick());
	REQUIRE(sender.Track(0, Body(1), 0.0));
	REQUIRE(sender.OnResent(0, 0.1));

	ReliableReceiver receiver(Quick());
	for (uint16_t sequence = 1; sequence <= 9; sequence++) {
		receiver.Accept(sequence, Body(sequence));
	}
	REQUIRE(receiver.Overflow() == DisconnectReason::BudgetExceeded);

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

	CHECK(total("net.reliability.resend") == 1.0);
	CHECK(total("net.reliability.overflow") == 1.0);
}

// ---------------------------------------------------------------------------
// The fuzz case.
//
// Every case above states one situation. This one drives thousands of sends
// through scripted loss and reordering and asserts the only property that
// matters end to end: every reliable payload arrives exactly once, in the order
// it was sent. `core::Random` rather than a standard generator, so a failure
// reproduces from its salt on any machine.
// ---------------------------------------------------------------------------

namespace {
	struct Scripted {
		uint32_t Messages = 4'000;
		uint32_t Salt = 1;
		uint16_t FirstSequence = 0;
		uint32_t LossPercent = 20;
		uint32_t AcknowledgeLossPercent = 20;
		uint32_t JitterTicks = 3;
		uint32_t PacketsPerTick = 8;
	};

	struct Outcome {
		std::vector<uint32_t> Delivered;
		uint64_t Retransmissions = 0;
		uint64_t Duplicates = 0;
		DisconnectReason SenderOverflow = DisconnectReason::None;
		DisconnectReason ReceiverOverflow = DisconnectReason::None;
		bool Settled = false; // everything sent was delivered and acknowledged
	};

	// A packet in flight, and the tick it lands on. Two packets sent in one
	// tick with different jitter arrive in the other order, which is the
	// reordering this exists to produce.
	struct InFlight {
		uint32_t ArrivesAtTick = 0;
		uint16_t Sequence = 0;
		std::vector<std::byte> Payload;
	};

	struct InFlightAcknowledgement {
		uint32_t ArrivesAtTick = 0;
		PacketHeader Header;
	};

	Outcome Run(const Scripted &script) {
		ReliabilitySettings settings;
		settings.RetransmitTimeoutSeconds = 0.1;
		settings.MaximumResends = 64; // the run is lossy, not dead
		ReliableSender sender(settings);
		ReliableReceiver receiver(settings, script.FirstSequence);

		Outcome outcome;
		std::vector<InFlight> toReceiver;
		std::vector<InFlightAcknowledgement> toSender;

		uint32_t next = 0;
		uint16_t sequence = script.FirstSequence;
		uint32_t roll = 0;

		const auto chance = [&roll, &script](uint32_t percent) {
			return Random::Bits(roll++, script.Salt) % 100 < percent;
		};
		const auto jitter = [&roll, &script](uint32_t tick) {
			return tick + 1 + Random::Bits(roll++, script.Salt + 1) % (script.JitterTicks + 1);
		};

		// Bounded rather than open-ended: a run that has stopped making
		// progress should fail the case, not hang the suite.
		constexpr uint32_t MAXIMUM_TICKS = 200'000;

		for (uint32_t tick = 0; tick < MAXIMUM_TICKS; tick++) {
			const double now = static_cast<double>(tick) / 60.0;
			uint32_t budget = script.PacketsPerTick;

			// Resends first: everything behind a hole waits on the hole.
			for (const auto &due : sender.Due(now)) {
				if (budget == 0) {
					break;
				}
				budget--;
				if (!chance(script.LossPercent)) {
					toReceiver.push_back(
						InFlight{jitter(tick), due.Sequence, {due.Payload.begin(), due.Payload.end()}}
					);
				}
				sender.OnResent(due.Sequence, now);
			}

			while (next < script.Messages && budget > 0 && sender.HasRoom()) {
				const std::vector<std::byte> body = Body(next);
				REQUIRE(sender.Track(sequence, body, now));
				budget--;
				if (!chance(script.LossPercent)) {
					toReceiver.push_back(InFlight{jitter(tick), sequence, body});
				}
				sequence++;
				next++;
			}

			for (auto at = toReceiver.begin(); at != toReceiver.end();) {
				if (at->ArrivesAtTick > tick) {
					++at;
					continue;
				}
				receiver.Accept(at->Sequence, at->Payload);
				at = toReceiver.erase(at);
			}

			for (const auto &delivery : receiver.Drain()) {
				outcome.Delivered.push_back(ValueOf(delivery.Payload));
			}

			// The acknowledgement rides whatever the far side is sending, so it
			// goes every tick and is lost as easily as anything else.
			if (!chance(script.AcknowledgeLossPercent)) {
				toSender.push_back(
					InFlightAcknowledgement{jitter(tick), receiver.Acknowledging(PacketHeader{})}
				);
			}

			for (auto at = toSender.begin(); at != toSender.end();) {
				if (at->ArrivesAtTick > tick) {
					++at;
					continue;
				}
				sender.OnAcknowledge(at->Header, 0.0);
				at = toSender.erase(at);
			}

			if (sender.Overflow() != DisconnectReason::None ||
				receiver.Overflow() != DisconnectReason::None) {
				break;
			}
			if (next == script.Messages && sender.Waiting() == 0) {
				outcome.Settled = true;
				break;
			}
		}

		outcome.Retransmissions = sender.Retransmissions();
		outcome.Duplicates = receiver.Duplicates();
		outcome.SenderOverflow = sender.Overflow();
		outcome.ReceiverOverflow = receiver.Overflow();
		return outcome;
	}

	// The number of positions where the delivered stream is not 0, 1, 2, ...
	//
	// Counted rather than asserted per payload, because a run that has gone
	// wrong is usually wrong in thousands of places and a CHECK per row buries
	// the first one.
	size_t OutOfOrder(const std::vector<uint32_t> &delivered) {
		size_t wrong = 0;
		for (size_t index = 0; index < delivered.size(); index++) {
			if (delivered[index] != index) {
				wrong++;
			}
		}
		return wrong;
	}
}

TEST_CASE("every reliable payload arrives exactly once and in order under loss", "[net][reliability][fuzz]") {
	Scripted script;
	const Outcome outcome = Run(script);

	REQUIRE(outcome.Settled);
	REQUIRE(outcome.SenderOverflow == DisconnectReason::None);
	REQUIRE(outcome.ReceiverOverflow == DisconnectReason::None);
	REQUIRE(outcome.Delivered.size() == script.Messages);
	REQUIRE(OutOfOrder(outcome.Delivered) == 0);

	// The run has to have exercised what it claims to: a fifth of everything
	// was dropped, so payloads were resent and duplicates arrived.
	REQUIRE(outcome.Retransmissions > 0);
	REQUIRE(outcome.Duplicates > 0);
}

TEST_CASE("a stream that wraps mid-run still arrives exactly once and in order", "[net][reliability][fuzz]") {
	// Starting 500 short of the wrap, so the run crosses 65535 to 0 with
	// payloads in flight, unacknowledged, and held out of order across it.
	Scripted script;
	script.Messages = 2'000;
	script.Salt = 17;
	script.FirstSequence = 65'036;

	const Outcome outcome = Run(script);

	REQUIRE(outcome.Settled);
	REQUIRE(outcome.SenderOverflow == DisconnectReason::None);
	REQUIRE(outcome.Delivered.size() == script.Messages);
	REQUIRE(OutOfOrder(outcome.Delivered) == 0);
}

TEST_CASE("heavy loss and heavy reordering change nothing about the guarantee", "[net][reliability][fuzz]") {
	Scripted script;
	script.Messages = 1'000;
	script.Salt = 99;
	script.LossPercent = 50;
	script.AcknowledgeLossPercent = 50;
	script.JitterTicks = 10;

	const Outcome outcome = Run(script);

	REQUIRE(outcome.Settled);
	REQUIRE(outcome.Delivered.size() == script.Messages);
	REQUIRE(OutOfOrder(outcome.Delivered) == 0);
}

TEST_CASE("many independent streams all arrive intact", "[net][reliability][fuzz]") {
	// One long run explores one path through the state space; several shorter
	// ones with different salts explore several.
	size_t failures = 0;

	for (uint32_t salt = 200; salt < 220; salt++) {
		Scripted script;
		script.Messages = 300;
		script.Salt = salt;
		script.FirstSequence = static_cast<uint16_t>(salt * 3'137);
		script.LossPercent = 10 + salt % 40;
		script.JitterTicks = salt % 6;

		const Outcome outcome = Run(script);
		if (!outcome.Settled || outcome.Delivered.size() != script.Messages ||
			OutOfOrder(outcome.Delivered) != 0) {
			failures++;
		}
	}

	REQUIRE(failures == 0);
}

// --- the round trip ----------------------------------------------------------
//
// **This is what `ConnectionStats::RoundTripMilliseconds` was declared for and
// never filled in.** The field sat at zero from v0.3 to v0.9 while
// `replication::Rewind::TickSeenBy` read it, so lag compensation corrected for
// the interpolation delay and not for travel — a conservative rewind, and one
// nothing said was conservative.

TEST_CASE("an acknowledgement measures the round trip", "[net][reliability]") {
	ReliableSender sender(Quick());

	// Nothing measured yet reads as zero, which a caller must take as
	// "unknown" rather than as "instant".
	CHECK(sender.SmoothedRoundTripSeconds() == 0.0);

	REQUIRE(sender.Track(0, Body(4), 1.0));
	REQUIRE(sender.OnAcknowledge(Ack(0), 1.05) == 1);

	// The first sample is taken whole. Smoothing towards zero would make the
	// estimate say "instant" for the first several round trips of every
	// connection, which is exactly when a game is deciding how much to
	// interpolate.
	CHECK(sender.SmoothedRoundTripSeconds() == Approx(0.05));
}

TEST_CASE("the estimate is smoothed rather than jumping to the last sample", "[net][reliability]") {
	ReliableSender sender(Quick());

	REQUIRE(sender.Track(0, Body(1), 0.0));
	REQUIRE(sender.OnAcknowledge(Ack(0), 0.1) == 1);
	REQUIRE(sender.SmoothedRoundTripSeconds() == Approx(0.1));

	// One spike, which is whatever the far side happened to be doing. A number
	// that jumped forty milliseconds between two reads is one no interface can
	// show.
	REQUIRE(sender.Track(1, Body(1), 1.0));
	REQUIRE(sender.OnAcknowledge(Ack(1), 1.9) == 1);

	// RFC 6298's one eighth: 0.1 * 0.875 + 0.9 * 0.125.
	const double smoothed = sender.SmoothedRoundTripSeconds();
	CHECK(smoothed == Approx(0.1 * 0.875 + 0.9 * 0.125));

	// **Nearer the settled value than the spike**, which is the property the
	// exact number above happens to have and the reason for smoothing at all.
	// Stated as a comparison rather than as a bound: the bound this was first
	// written with was `< 0.2`, and the answer is exactly 0.2.
	CHECK(std::abs(smoothed - 0.1) < std::abs(smoothed - 0.9));
}

TEST_CASE("a resent payload is never measured, which is Karn's rule", "[net][reliability]") {
	// **The detail that decides whether this is right or merely present.** An
	// acknowledgement of a resent packet does not say *which* transmission it
	// answers, so a sample from one is either the true trip or the trip plus a
	// retransmit timeout — and there is no way to tell. Measuring them makes
	// the estimate worst on exactly the links that need it most.
	ReliableSender sender(Quick());

	REQUIRE(sender.Track(0, Body(1), 0.0));

	// Time it out and resend it.
	const std::span<const ReliableSender::Unacknowledged> due = sender.Due(10.0);
	REQUIRE(due.size() == 1);
	REQUIRE(sender.OnResent(0, 10.0));

	// The acknowledgement arrives. It may be answering either transmission.
	REQUIRE(sender.OnAcknowledge(Ack(0), 10.01) == 1);
	CHECK(sender.SmoothedRoundTripSeconds() == 0.0);

	// A packet that was never resent still measures.
	REQUIRE(sender.Track(1, Body(1), 20.0));
	REQUIRE(sender.OnAcknowledge(Ack(1), 20.02) == 1);
	CHECK(sender.SmoothedRoundTripSeconds() == Approx(0.02));
}

TEST_CASE("a window acknowledgement measures too", "[net][reliability]") {
	// Retiring through the bitfield is the common case on a busy link — one
	// packet retires up to thirty-three — so measuring only the direct
	// acknowledgement would sample a small and unrepresentative slice.
	ReliableSender sender(Quick());

	REQUIRE(sender.Track(10, Body(1), 0.0));
	REQUIRE(sender.Track(11, Body(1), 0.0));

	REQUIRE(sender.OnAcknowledge(Ack(11, BitFor(11, 10)), 0.04) == 2);
	CHECK(sender.SmoothedRoundTripSeconds() > 0.0);
}

TEST_CASE("a clock that went backwards is not folded in", "[net][reliability]") {
	// One bad sample survives in a smoothed value for dozens of good ones.
	ReliableSender sender(Quick());

	REQUIRE(sender.Track(0, Body(1), 5.0));
	REQUIRE(sender.OnAcknowledge(Ack(0), 1.0) == 1);
	CHECK(sender.SmoothedRoundTripSeconds() == 0.0);

	REQUIRE(sender.Track(1, Body(1), 10.0));
	REQUIRE(sender.OnAcknowledge(Ack(1), 10.03) == 1);
	CHECK(sender.SmoothedRoundTripSeconds() == Approx(0.03));
}
