#include <engine/core/Log.hpp>
#include <engine/net/Packet.hpp>
#include <engine/net/Wire.hpp>
#include <engine/net/quic/Connection.hpp>

namespace engine::net {

	namespace {
		// RFC 9000 §17.2: bit 7 of the first byte is the Header Form and bit 6
		// is the Fixed Bit. A long header sets both, so every Initial packet
		// begins in `0xC0..0xFF`. `Packet::MAGIC` is `0x314E5441` written
		// little-endian, so a datagram-stack packet begins with `0x41` and has
		// bit 7 clear. See `Wire.hpp` for why that one bit is enough.
		constexpr std::byte LONG_HEADER_FORM{0x80};
	}

	const char *Describe(WireKind kind) {
		switch (kind) {
		case WireKind::Datagram:
			return "datagram";
		case WireKind::Quic:
			return "quic";
		}
		return "?";
	}

	const char *Describe(WireMode mode) {
		switch (mode) {
		case WireMode::Quic:
			return "quic";
		case WireMode::Datagram:
			return "datagram";
		case WireMode::Both:
			return "both";
		}
		return "?";
	}

	bool Serves(WireMode mode, WireKind kind) {
		switch (mode) {
		case WireMode::Quic:
			return kind == WireKind::Quic;
		case WireMode::Datagram:
			return kind == WireKind::Datagram;
		case WireMode::Both:
			return true;
		}
		return false;
	}

	std::optional<WireMode> ParseWireMode(std::string_view text) {
		if (text == "quic") {
			return WireMode::Quic;
		}
		if (text == "datagram") {
			return WireMode::Datagram;
		}
		if (text == "both") {
			return WireMode::Both;
		}
		return std::nullopt;
	}

	std::optional<WireKind> WireOf(std::span<const std::byte> datagram) {
		if (datagram.empty()) {
			return std::nullopt;
		}

		// **The top of the demux for every inbound datagram on a listener.** A
		// build talking to a peer that speaks a different version claims neither
		// stack here, and the symptom is a client that connects to nothing with
		// no packet ever reaching a `Link`. Rate-limited because this runs per
		// datagram and the failing case is usually every datagram.
		const auto unclaimed = [&datagram]() -> std::optional<WireKind> {
			ENGINE_DEBUG_EVERY(
				1.0,
				"a {} byte datagram beginning {:#04x} belongs to neither stack",
				datagram.size(),
				static_cast<unsigned>(datagram[0])
			);
			return std::nullopt;
		};

		if ((datagram[0] & LONG_HEADER_FORM) != std::byte{0}) {
			// A long header. `Accepts` is stricter than this test needs to be -
			// it also checks the version and the packet type - and that is
			// wanted: a long-header packet that is not a client's opening
			// Initial is not something this end can stand a connection up from,
			// so calling it QUIC would only move the refusal one step later.
			return quic::Accepts(datagram) ? std::optional<WireKind>(WireKind::Quic) : unclaimed();
		}

		// Short header, or the datagram stack. `PeekChannel` reads the whole
		// magic and the version before it answers, so it is the check that
		// separates the two rather than the first byte.
		if (Packet::PeekChannel(datagram).has_value()) {
			return WireKind::Datagram;
		}
		return unclaimed();
	}
}
