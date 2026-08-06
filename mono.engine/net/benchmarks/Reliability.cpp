// What the reliable channel costs to keep, and what it costs when the network
// is bad.
//
// **Every structure here is bounded to 32 by construction**, so nothing in this
// file can go quadratic in any interesting way — and that is precisely why the
// numbers are worth having. The bound is what makes the design safe; these rows
// are what say whether a linear scan over 32 entries per packet is *cheap*, or
// whether 32 has quietly become the constant that dominates the per-packet
// budget on a hundred-player server.
//
// The interesting comparison is not one row against another but the whole file
// against `engine.net.bench.crypto`. If keeping the reliable window costs a
// meaningful fraction of what sealing a packet costs, the bookkeeping is
// competing with the cryptography, which nobody expects and nobody would notice
// without a figure.
//
// **The loss rows are the ones a good network never runs.** A steady state
// where everything is acknowledged on the next packet exercises the cheap path
// only; a 20% loss rate is where `Due` actually returns something, where the
// receiver holds payloads behind a gap, and where the retransmit clock is
// consulted for every entry. A benchmark that only measured the good day would
// report that the bad day is free.

#include <engine/net/Enums.hpp>
#include <engine/net/Packet.hpp>
#include <engine/net/Reliability.hpp>
#include <engine/testing/Bench.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

TEST_SUITE_ID("engine.net.bench.reliability")

using engine::net::ChannelKind;
using engine::net::PacketHeader;
using engine::net::ReliabilitySettings;
using engine::net::ReliableReceiver;
using engine::net::ReliableSender;
using engine::testing::Consume;

namespace reliability_bench {

	// Packets per row. One second of a 100-player server at 60 Hz is 6000; this
	// is a bit under two of those seconds, so the figure divides back cleanly.
	constexpr size_t PACKETS = 10'000;

	// A reliable payload is an event, not a snapshot — a door opening, a shot
	// fired — so it is small. Sizing it at the MTU would measure a memcpy the
	// real path does not do.
	constexpr size_t PAYLOAD_BYTES = 128;

	const std::vector<std::byte> &Payload() {
		static const std::vector<std::byte> payload = [] {
			std::vector<std::byte> built(PAYLOAD_BYTES);
			for (size_t index = 0; index < built.size(); index++) {
				built[index] = static_cast<std::byte>(index * 3u);
			}
			return built;
		}();
		return payload;
	}

	// A header acknowledging `sequence` and the 32 before it, all bits set.
	//
	// **The worst case for `OnAcknowledge`, and the ordinary one.** Every bit
	// set means the retire loop walks the whole window rather than exiting
	// early, and a healthy connection genuinely does look like this: everything
	// arrived, so everything is acknowledged.
	PacketHeader AcknowledgingAll(uint16_t sequence) {
		PacketHeader header;
		header.Channel = ChannelKind::Reliable;
		header.Acknowledge = sequence;
		header.AcknowledgeBits = 0xFFFF'FFFFu;
		return header;
	}
}

using namespace reliability_bench;

// --- the good day -------------------------------------------------------------
//
// Everything sent is acknowledged by the next packet. The window never fills,
// `Due` never returns anything, and the receiver never holds. This is what the
// reliable channel costs when the network is behaving, and it is the number the
// loss rows are read against.

BENCH("sender · track then acknowledge, 10k packets", PACKETS) {
	ReliableSender sender;
	double now = 0.0;
	const std::vector<std::byte> &payload = Payload();
	size_t retired = 0;

	for (size_t index = 0; index < PACKETS; index++) {
		const auto sequence = static_cast<uint16_t>(index);
		Consume(sender.Track(sequence, payload, now));
		retired += sender.OnAcknowledge(AcknowledgingAll(sequence));
		now += 1.0 / 60.0;
	}
	Consume(retired);
}

BENCH("sender · Due with nothing due, 10k calls", PACKETS) {
	// **Called every tick whether or not anything is waiting**, which makes it
	// the one function here whose empty case is the common case. It has to walk
	// the held entries to find out that none of them have timed out, so this row
	// is that walk — with the window full, because a walk over an empty window
	// measures nothing.
	ReliableSender sender;
	const std::vector<std::byte> &payload = Payload();
	for (size_t index = 0; index < 32; index++) {
		Consume(sender.Track(static_cast<uint16_t>(index), payload, 0.0));
	}

	size_t due = 0;
	for (size_t index = 0; index < PACKETS; index++) {
		// Well inside the 100 ms timeout, so nothing is ever due.
		due += sender.Due(0.001).size();
	}
	Consume(due);
}

BENCH("receiver · accept in order, 10k packets", PACKETS) {
	ReliableReceiver receiver;
	const std::vector<std::byte> &payload = Payload();
	size_t delivered = 0;

	for (size_t index = 0; index < PACKETS; index++) {
		Consume(receiver.Accept(static_cast<uint16_t>(index), payload));
		delivered += receiver.Drain().size();
	}
	Consume(delivered);
}

BENCH("receiver · Acknowledging, 100k headers", 100'000) {
	// Stamped onto **every** outgoing header whatever its channel, because a
	// game is mostly one-way and a reliable stream acknowledged only by reliable
	// traffic going the other way would hardly be acknowledged at all. So this
	// is paid per outgoing packet, not per reliable one, and it is the row most
	// likely to be underestimated.
	ReliableReceiver receiver;
	const std::vector<std::byte> &payload = Payload();
	for (size_t index = 0; index < 40; index++) {
		Consume(receiver.Accept(static_cast<uint16_t>(index), payload));
	}

	PacketHeader outgoing;
	outgoing.Channel = ChannelKind::Unreliable;
	uint32_t bits = 0;
	for (size_t index = 0; index < 100'000; index++) {
		outgoing.Sequence = static_cast<uint16_t>(index);
		bits ^= receiver.Acknowledging(outgoing).AcknowledgeBits;
	}
	Consume(bits);
}

// --- the bad day --------------------------------------------------------------
//
// One packet in five is lost. **This is the configuration the reliable channel
// exists for**, and it is the only one where the expensive paths run at all:
// `Due` returns entries, the sender resends, the receiver holds payloads behind
// a gap and releases a run of them when the gap fills.

BENCH("sender · 20% loss, 10k packets", PACKETS) {
	ReliableSender sender;
	double now = 0.0;
	const std::vector<std::byte> &payload = Payload();
	size_t resent = 0;

	for (size_t index = 0; index < PACKETS; index++) {
		const auto sequence = static_cast<uint16_t>(index);
		if (sender.HasRoom()) {
			Consume(sender.Track(sequence, payload, now));
		}

		// Four packets in five are acknowledged. The fifth is not, so it stays
		// held until its retransmit clock expires.
		if (index % 5 != 0) {
			Consume(sender.OnAcknowledge(AcknowledgingAll(sequence)));
		}

		now += 1.0 / 60.0;

		// The tick's resend pass, exactly as a `Link` runs it.
		const std::span<const ReliableSender::Unacknowledged> due = sender.Due(now);
		for (const ReliableSender::Unacknowledged &entry : due) {
			Consume(sender.OnResent(entry.Sequence, now));
			resent++;
		}
	}
	Consume(resent);
}

BENCH("receiver · 20% reordered, 10k packets", PACKETS) {
	// Every fifth packet arrives one place late, so the receiver is holding
	// something behind a gap almost continuously and `Drain` releases a run
	// rather than a single payload. That release loop is the one piece of this
	// module with a shape worth measuring: it has to be correct across the
	// 16-bit wrap without a special case, and "correct across a wrap" is the
	// kind of thing that gets implemented as a search.
	ReliableReceiver receiver;
	const std::vector<std::byte> &payload = Payload();
	size_t delivered = 0;

	for (size_t index = 0; index < PACKETS; index += 5) {
		// Deliver 1..4 of this group, then 0 — so four payloads wait on the
		// fifth and all five come out together.
		for (size_t offset = 1; offset < 5; offset++) {
			Consume(receiver.Accept(static_cast<uint16_t>(index + offset), payload));
			delivered += receiver.Drain().size();
		}
		Consume(receiver.Accept(static_cast<uint16_t>(index), payload));
		delivered += receiver.Drain().size();
	}
	Consume(delivered);
}

BENCH("receiver · 10k duplicates against a full window", PACKETS) {
	// A resend that did not need to happen — the far side resends precisely
	// because it has not heard that the original arrived, so a lossy link
	// produces these continuously. Rejecting one has to be cheap and, more to
	// the point, must not hold anything: if this row's cost climbs across
	// samples, duplicates are accumulating somewhere and a peer can grow the
	// receiver without bound by resending one packet forever.
	ReliableReceiver receiver;
	const std::vector<std::byte> &payload = Payload();
	for (size_t index = 0; index < 40; index++) {
		Consume(receiver.Accept(static_cast<uint16_t>(index), payload));
		Consume(receiver.Drain().size());
	}

	uint32_t accepted = 0;
	for (size_t index = 0; index < PACKETS; index++) {
		accepted += receiver.Accept(7, payload) ? 1u : 0u;
	}
	Consume(accepted);
	Consume(receiver.Duplicates());
}

BENCH("receiver · 10k payloads against a stalled gap", PACKETS) {
	// **The hostile case the bound exists for**: a peer that sends 1..n and
	// never sends 0. Nothing can ever be delivered, `MaximumOutOfOrder` is
	// reached, and every arrival after that is refused. A flat row means the
	// refusal is decided before the payload is copied; a row that grows means a
	// peer is buying storage with bytes it never has to follow up.
	ReliableReceiver receiver;
	const std::vector<std::byte> &payload = Payload();
	uint32_t accepted = 0;
	for (size_t index = 0; index < PACKETS; index++) {
		// Sequence 0 is never sent, so nothing ahead of it can be released.
		accepted += receiver.Accept(static_cast<uint16_t>(1 + (index % 40)), payload) ? 1u : 0u;
		Consume(receiver.Drain().size());
	}
	Consume(accepted);
	Consume(receiver.Holding());
}

BENCH("sender · 10k tracks against a full window", PACKETS) {
	// The sender's twin: nothing is ever acknowledged, so after 32 payloads
	// `HasRoom` is false forever. Refusing has to be cheaper than holding, or a
	// peer that stops acknowledging costs the sender more per packet than one
	// that behaves.
	ReliableSender sender;
	const std::vector<std::byte> &payload = Payload();
	uint32_t held = 0;
	for (size_t index = 0; index < PACKETS; index++) {
		held += sender.Track(static_cast<uint16_t>(index), payload, 0.0) ? 1u : 0u;
	}
	Consume(held);
	Consume(sender.Waiting());
}

// --- a server's tick ----------------------------------------------------------

BENCH("tick · 100 connections, reliable bookkeeping", 100) {
	// **What one server tick costs in reliability bookkeeping alone**, across a
	// hundred connections that each have a payload in flight and a healthy
	// window. One iteration is one connection, so the figure multiplies by
	// population and then by 60 to become a share of a second.
	//
	// The senders are built once and kept, because a hundred fresh
	// `ReliableSender`s per sample would measure the allocator: each holds a
	// 32-entry window of payload vectors, and constructing that is not something
	// a running server ever does.
	static std::vector<ReliableSender> senders(100);
	static std::vector<ReliableReceiver> receivers(100);
	static uint16_t sequence = 0;

	const std::vector<std::byte> &payload = Payload();
	const double now = static_cast<double>(sequence) / 60.0;

	for (size_t connection = 0; connection < 100; connection++) {
		ReliableSender &sender = senders[connection];
		ReliableReceiver &receiver = receivers[connection];

		if (sender.HasRoom()) {
			Consume(sender.Track(sequence, payload, now));
		}
		Consume(sender.OnAcknowledge(AcknowledgingAll(sequence)));
		Consume(sender.Due(now).size());

		Consume(receiver.Accept(sequence, payload));
		Consume(receiver.Drain().size());

		PacketHeader outgoing;
		outgoing.Channel = ChannelKind::Unreliable;
		outgoing.Sequence = sequence;
		Consume(receiver.Acknowledging(outgoing).AcknowledgeBits);
	}
	sequence++;
}
