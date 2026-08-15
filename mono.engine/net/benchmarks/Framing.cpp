// What framing costs per packet, at the packet rates a server actually sees.
//
// **The unit that matters here is packets per second, not nanoseconds.** A
// server with 100 players at 60 Hz is 6000 inbound packets a second before
// anybody has shot at anybody; the numbers in this file divide into a budget,
// and the honest way to read them is "at this cost, one core frames N packets
// a second and does nothing else".
//
// Two things are measured that a naive suite would merge. `Read` parses the
// whole header and validates five separate things; `PeekChannel` reads three
// fields and stops, because the router that decides where a datagram goes does
// not have a connection to hand it to yet. If those two ever cost the same,
// `PeekChannel` has stopped being a shortcut and the router is paying full
// price on every datagram including the ones it is about to throw away.
//
// **The refusal rows are the point of the suite, not a footnote.** Everything
// on this path faces the open internet, and the cost of *rejecting* garbage is
// what decides whether a flood of malformed datagrams is an annoyance or an
// outage. A parser that costs more to say no than to say yes is an
// amplification factor, so every refusal reason is measured next to the
// success it mirrors.

#include <engine/core/Bytes.hpp>
#include <engine/net/Enums.hpp>
#include <engine/net/Packet.hpp>
#include <engine/testing/Bench.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

TEST_SUITE_ID("engine.net.bench.framing")

using engine::core::ByteReader;
using engine::core::ByteWriter;
using engine::net::ChannelKind;
using engine::net::Packet;
using engine::net::PacketHeader;
using engine::testing::Consume;

namespace framing_bench {

	// Packets per row.
	//
	// 10 000 is between one second of a 100-player server and one second of a
	// busy 160-player one, so the reported per-packet figure multiplies straight
	// back into a real deployment without anybody rescaling in their head.
	constexpr size_t PACKETS = 10'000;

	// A full-size payload, which is what a snapshot packet is.
	//
	// Benchmarking an empty payload would measure the header and call it
	// framing. Most packets a game sends are at or near the MTU by
	// construction - the snapshot writer fills them - so full size is the
	// typical case rather than the worst one.
	const std::vector<std::byte> &Payload() {
		static const std::vector<std::byte> payload = [] {
			std::vector<std::byte> built(Packet::MAXIMUM_PAYLOAD_BYTES);
			for (size_t index = 0; index < built.size(); index++) {
				built[index] = static_cast<std::byte>(index * 7u);
			}
			return built;
		}();
		return payload;
	}

	// A header with every field non-zero, so no path is flattered by a value the
	// encoder might special-case.
	PacketHeader HeaderAt(size_t index) {
		PacketHeader header;
		header.Channel = ChannelKind::Unreliable;
		header.Sequence = static_cast<uint16_t>(index);
		header.Acknowledge = static_cast<uint16_t>(index - 1);
		header.AcknowledgeBits = 0xDEAD'BEEFu;
		header.Counter = static_cast<uint64_t>(index) << 8;
		return header;
	}

	// One well-formed full-size datagram, built once.
	const std::vector<std::byte> &Datagram() {
		static const std::vector<std::byte> datagram = [] {
			ByteWriter writer;
			Packet::Write(writer, HeaderAt(1234), Payload());
			const std::span<const std::byte> bytes = writer.Bytes();
			return std::vector<std::byte>(bytes.begin(), bytes.end());
		}();
		return datagram;
	}

	// The same datagram with one byte of the magic changed - the cheapest
	// possible refusal, and the one a port scanner produces.
	const std::vector<std::byte> &WrongMagic() {
		static const std::vector<std::byte> wrong = [] {
			std::vector<std::byte> built = Datagram();
			built[0] = static_cast<std::byte>(0x00);
			return built;
		}();
		return wrong;
	}

	// A datagram whose header is intact and whose length field promises far more
	// payload than the buffer holds.
	//
	// **The important refusal.** A parser that trusts this field allocates from
	// it, and a length field an attacker controls is the classic way to turn a
	// 40-byte datagram into a 4-gigabyte allocation. `Packet::Read` must refuse
	// on the length alone, in constant time, and this row is where that is
	// visible as a number rather than as a claim in a header comment.
	const std::vector<std::byte> &LyingLength() {
		static const std::vector<std::byte> lying = [] {
			ByteWriter writer;
			Packet::WriteHeader(writer, HeaderAt(1234), Packet::MAXIMUM_PAYLOAD_BYTES);
			// Header only - the length field says a full payload follows and
			// there is not one byte of it.
			const std::span<const std::byte> bytes = writer.Bytes();
			return std::vector<std::byte>(bytes.begin(), bytes.end());
		}();
		return lying;
	}

	// Two bytes. Everything a parser needs is missing.
	const std::vector<std::byte> &Runt() {
		static const std::vector<std::byte> runt(2);
		return runt;
	}

	// A writer held across samples, the way a real sender holds one: `Clear`
	// keeps the capacity so a per-packet send stops allocating after the first.
	ByteWriter &Reused() {
		static ByteWriter writer(2048);
		return writer;
	}
}

using namespace framing_bench;

// --- writing ------------------------------------------------------------------

BENCH("Write · 10k full-size packets", PACKETS) {
	ByteWriter &writer = Reused();
	const std::vector<std::byte> &payload = Payload();
	for (size_t index = 0; index < PACKETS; index++) {
		writer.Clear();
		Consume(Packet::Write(writer, HeaderAt(index), payload));
	}
}

BENCH("Write · 10k empty packets", PACKETS) {
	// A packet carrying only an acknowledgement, which is how a quiet
	// connection stays alive. Against the row above, the difference is the
	// payload copy and everything else is the header - so this row is the
	// per-packet fixed cost and that one is the fixed cost plus the bytes.
	ByteWriter &writer = Reused();
	for (size_t index = 0; index < PACKETS; index++) {
		writer.Clear();
		Consume(Packet::Write(writer, HeaderAt(index), {}));
	}
}

BENCH("WriteHeader · 10k", PACKETS) {
	// The two-step path a real sender takes: the header is the sealed frame's
	// associated data, so it has to be written before the payload exists. This
	// is what that split costs against the one-shot `Write`.
	ByteWriter &writer = Reused();
	for (size_t index = 0; index < PACKETS; index++) {
		writer.Clear();
		Consume(Packet::WriteHeader(writer, HeaderAt(index), Packet::MAXIMUM_PAYLOAD_BYTES));
	}
}

// --- reading ------------------------------------------------------------------

BENCH("Read · 10k well-formed packets", PACKETS) {
	const std::vector<std::byte> &datagram = Datagram();
	size_t bytes = 0;
	for (size_t index = 0; index < PACKETS; index++) {
		ByteReader reader(datagram);
		const std::optional<Packet::Inbound> inbound = Packet::Read(reader);
		bytes += inbound ? inbound->Payload.size() : 0;
	}
	Consume(bytes);
}

BENCH("PeekChannel · 10k", PACKETS) {
	// **The router's fast path.** It reads the magic, the version and the
	// channel byte and stops, because on a server the sender of a handshake
	// datagram is by definition not in the connection table yet. It must be
	// dramatically cheaper than `Read` or the shortcut is not one.
	const std::vector<std::byte> &datagram = Datagram();
	uint32_t handshakes = 0;
	for (size_t index = 0; index < PACKETS; index++) {
		const std::optional<ChannelKind> channel = Packet::PeekChannel(datagram);
		handshakes += (channel && *channel == ChannelKind::Handshake) ? 1u : 0u;
	}
	Consume(handshakes);
}

BENCH("IsNewer · 100k", 100'000) {
	// Called once per inbound packet per channel, and the reason a session does
	// not die eighteen minutes in when the 16-bit sequence wraps. It is a
	// half-range comparison and should be immeasurably cheap; this row exists so
	// that a change making it *not* cheap is visible, because it sits inside the
	// per-packet path where nothing else is allowed to be slow either.
	uint32_t newer = 0;
	for (size_t index = 0; index < 100'000; index++) {
		newer += Packet::IsNewer(static_cast<uint16_t>(index), static_cast<uint16_t>(index - 7)) ? 1u : 0u;
	}
	Consume(newer);
}

// --- the hostile path ---------------------------------------------------------
//
// **Every row here must be no dearer than `Read · 10k well-formed packets`.**
// That is the whole invariant: an attacker choosing the cheapest datagram to
// send should not be choosing the most expensive one to parse. Read each
// refusal against the success above and against the others - a refusal that
// stands out is a refusal doing work before it decided to refuse.

BENCH("Read · 10k datagrams with the wrong magic", PACKETS) {
	// Four bytes read and done. The floor for a refusal, and what an internet
	// port scan costs a listening server.
	const std::vector<std::byte> &wrong = WrongMagic();
	uint32_t accepted = 0;
	for (size_t index = 0; index < PACKETS; index++) {
		ByteReader reader(wrong);
		accepted += Packet::Read(reader).has_value() ? 1u : 0u;
	}
	Consume(accepted);
}

BENCH("Read · 10k two-byte runts", PACKETS) {
	const std::vector<std::byte> &runt = Runt();
	uint32_t accepted = 0;
	for (size_t index = 0; index < PACKETS; index++) {
		ByteReader reader(runt);
		accepted += Packet::Read(reader).has_value() ? 1u : 0u;
	}
	Consume(accepted);
}

BENCH("Read · 10k headers claiming a payload that is not there", PACKETS) {
	// **The allocation-from-a-length-field case.** The header parses, the length
	// field says 1176 bytes follow, and nothing does. If this row is flat and
	// cheap the length is being checked against the buffer before anything acts
	// on it; if it ever grows with the claimed length, the check has moved to
	// after the act.
	const std::vector<std::byte> &lying = LyingLength();
	uint32_t accepted = 0;
	for (size_t index = 0; index < PACKETS; index++) {
		ByteReader reader(lying);
		accepted += Packet::Read(reader).has_value() ? 1u : 0u;
	}
	Consume(accepted);
}

BENCH("PeekChannel · 10k datagrams with the wrong magic", PACKETS) {
	// The router's refusal, which is the very first thing an unsolicited
	// datagram meets. Cheaper than everything else here or the router is the
	// attack surface.
	const std::vector<std::byte> &wrong = WrongMagic();
	uint32_t routed = 0;
	for (size_t index = 0; index < PACKETS; index++) {
		routed += Packet::PeekChannel(wrong).has_value() ? 1u : 0u;
	}
	Consume(routed);
}

// --- a server's inbound second ------------------------------------------------

BENCH("inbound second · 6000 packets peeked then read", 6000) {
	// **100 players at 60 Hz, framed the way the server actually frames them**:
	// peek to route, then read. One iteration is one packet, so the figure is
	// what one core spends per packet on framing alone before a byte has been
	// decrypted or applied - and six thousand times it is the share of a second
	// this layer takes at that population.
	const std::vector<std::byte> &datagram = Datagram();
	size_t bytes = 0;
	for (size_t index = 0; index < 6000; index++) {
		const std::optional<ChannelKind> channel = Packet::PeekChannel(datagram);
		if (!channel) {
			continue;
		}
		ByteReader reader(datagram);
		const std::optional<Packet::Inbound> inbound = Packet::Read(reader);
		bytes += inbound ? inbound->Payload.size() : 0;
	}
	Consume(bytes);
}
