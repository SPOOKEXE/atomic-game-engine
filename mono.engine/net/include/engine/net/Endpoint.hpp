#pragma once

// Where a datagram came from, and where one is going.
//
// This is the transport's half of an address and nothing more. It is a value:
// copyable, comparable, hashable, and carrying no handle to anything. A
// `Transport` hands one back with every datagram it receives, and takes one with
// every datagram it sends.
//
// **It names no vendor type, and that is the whole reason it exists.** asio is
// `VENDOR` in this module rather than `VENDOR_PUBLIC`, so a public header here
// may not name a socket, an `io_context`, an `error_code` or an asio endpoint —
// `net/AGENTS.md` states it and the build cannot check it. Passing an
// `asio::ip::udp::endpoint` across this boundary would put asio's headers in
// every module that links `net` and would pin the transport to one library
// permanently.
//
// **Sixteen bytes and a family, so IPv6 is not a later migration.** A four-byte
// address would fit today's tests and would have to be widened by every caller
// the day a v6 socket appears. The address is stored big-endian — the order the
// numbering in `127.0.0.1` reads in, and the order every wire format and every
// socket API already uses — so a conversion is a copy rather than a byte swap
// somebody forgets on one side.
//
// **A `Connection` is not an `Endpoint`.** An address is where bytes go; a
// `ConnectionId` is who is on the other end and whether they are still there.
// Two peers behind one NAT share an address, and one peer that reconnects from a
// new port keeps its identity — so the transport routes by this and the
// lifecycle above it keys by that.
//
// @tier L11 · shared

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
// For std::hash. Specialising it without the primary template in scope is a
// wall of errors pointing at the standard library rather than at this file.
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace engine::net {

	// Which kind of address an Endpoint holds, and therefore how many of its
	// bytes mean anything.
	//
	// @since v0.3
	enum class AddressFamily : uint8_t {
		// No address at all. What a default-constructed Endpoint carries, so a
		// partly filled record does not read as "somewhere valid".
		None,

		// Four address bytes, in the first four. The rest are zero.
		IPv4,

		// All sixteen address bytes.
		IPv6,
	};

	// Returns a stable, human-readable name for an address family.
	//
	// @param family The family to name.
	// @return A view valid for the lifetime of the process.
	const char *Describe(AddressFamily family);

	// One peer's address and port, as a value with no socket behind it.
	//
	// @since v0.3
	struct Endpoint {
		// The address, big-endian, zero-padded past the family's width.
		//
		// Big-endian rather than host order because that is what every socket
		// API and every wire format already holds, so both conversions are a
		// copy. A host-order field would be byte-swapped on one side and, on a
		// little-endian machine, would look correct anyway until somebody built
		// for a big-endian one.
		std::array<std::byte, 16> Address{};

		// The port, in host order. Nothing writes it to a wire, so there is
		// nothing for it to be the wrong way round for.
		uint16_t Port = 0;

		// Which of Address's bytes are part of the address.
		AddressFamily Family = AddressFamily::None;

		// Whether this names somewhere.
		//
		// Says the value was filled in, not that anything is listening there —
		// on an unreliable transport nothing can answer the second question
		// without sending something and waiting.
		//
		// @return `true` unless the family is `None`.
		bool IsValid() const {
			return Family != AddressFamily::None;
		}

		// Whether two endpoints name the same address and port.
		bool operator==(const Endpoint &other) const = default;

		// Ordering, so an endpoint can key a sorted container without a caller
		// inventing a comparator that disagrees with somebody else's.
		auto operator<=>(const Endpoint &other) const = default;

		// Renders as `127.0.0.1:7777`, or `[::1]:7777` for a v6 address.
		//
		// The brackets are not decoration: without them the colons of a v6
		// address and the colon before the port are the same character, and
		// every log line becomes ambiguous. Parse reads back what this writes.
		//
		// @return The text, or `none` for an invalid endpoint.
		std::string Text() const;

		// Builds an endpoint from four big-endian address bytes.
		//
		// @param address The address, in the order it is written in.
		// @param port The port, in host order.
		// @return The endpoint.
		static Endpoint FromIPv4(const std::array<uint8_t, 4> &address, uint16_t port);

		// Builds an endpoint from sixteen big-endian address bytes.
		//
		// @param address The address, in the order it is written in.
		// @param port The port, in host order.
		// @return The endpoint.
		static Endpoint FromIPv6(const std::array<uint8_t, 16> &address, uint16_t port);

		// The IPv4 loopback address, 127.0.0.1, on `port`.
		//
		// @param port The port, in host order.
		// @return The endpoint.
		static Endpoint LoopbackIPv4(uint16_t port);

		// Parses what Text produced, refusing anything else.
		//
		// **The text is hostile.** It arrives from a command line, a
		// configuration file or a server list, so a missing port, a port over
		// 65535, an unbracketed v6 address and trailing rubbish are each
		// refused rather than half-read into a partly filled endpoint. A host
		// *name* is refused too: resolving one is a blocking call to a network
		// service, and nothing at this layer may block.
		//
		// @param text An address and port, as `127.0.0.1:7777` or `[::1]:7777`.
		// @return The endpoint, or nothing when the text is not one.
		static std::optional<Endpoint> Parse(std::string_view text);
	};
}

namespace std {

	// Hashing, so an Endpoint keys an unordered container.
	//
	// Over the whole value rather than the address alone: two peers behind one
	// NAT differ only in their port, and a hash that ignored the port would put
	// every player at a LAN party in one bucket.
	template <> struct hash<engine::net::Endpoint> {
		// @param endpoint The endpoint to hash.
		// @return Its hash.
		size_t operator()(const engine::net::Endpoint &endpoint) const noexcept;
	};
}
