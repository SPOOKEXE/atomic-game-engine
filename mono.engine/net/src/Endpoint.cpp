#include <engine/net/Endpoint.hpp>

#include <array>
// asio parses and formats addresses here and nowhere else in this module's
// public reach. It is used for that alone — no socket, no io_context — because
// v6 text is not a format worth reimplementing: `::` compression, an embedded v4
// tail and a scope suffix are three chances to disagree with every other tool
// that prints an address.
#include <asio/ip/address.hpp>
#include <charconv>
#include <cstring>
#include <system_error>

namespace engine::net {

	const char *Describe(AddressFamily family) {
		switch (family) {
		case AddressFamily::None:
			return "none";
		case AddressFamily::IPv4:
			return "ipv4";
		case AddressFamily::IPv6:
			return "ipv6";
		}
		// No default label, so adding a family is a compiler warning here.
		return "?";
	}

	namespace {
		asio::ip::address ToAsioAddress(const Endpoint &endpoint) {
			if (endpoint.Family == AddressFamily::IPv4) {
				asio::ip::address_v4::bytes_type bytes{};
				std::memcpy(bytes.data(), endpoint.Address.data(), bytes.size());
				return asio::ip::address_v4(bytes);
			}

			asio::ip::address_v6::bytes_type bytes{};
			std::memcpy(bytes.data(), endpoint.Address.data(), bytes.size());
			return asio::ip::address_v6(bytes);
		}
	}

	std::string Endpoint::Text() const {
		if (!IsValid()) {
			return "none";
		}

		const std::string address = ToAsioAddress(*this).to_string();
		if (Family == AddressFamily::IPv6) {
			// Bracketed, because a v6 address and the port before it are
			// separated by the same character the address itself is full of.
			return "[" + address + "]:" + std::to_string(Port);
		}
		return address + ":" + std::to_string(Port);
	}

	Endpoint Endpoint::FromIPv4(const std::array<uint8_t, 4> &address, uint16_t port) {
		Endpoint endpoint;
		endpoint.Family = AddressFamily::IPv4;
		endpoint.Port = port;
		std::memcpy(endpoint.Address.data(), address.data(), address.size());
		return endpoint;
	}

	Endpoint Endpoint::FromIPv6(const std::array<uint8_t, 16> &address, uint16_t port) {
		Endpoint endpoint;
		endpoint.Family = AddressFamily::IPv6;
		endpoint.Port = port;
		std::memcpy(endpoint.Address.data(), address.data(), address.size());
		return endpoint;
	}

	Endpoint Endpoint::LoopbackIPv4(uint16_t port) {
		return FromIPv4({127, 0, 0, 1}, port);
	}

	Endpoint Endpoint::BroadcastIPv4(uint16_t port) {
		return FromIPv4({255, 255, 255, 255}, port);
	}

	std::optional<Endpoint> Endpoint::Parse(std::string_view text) {
		if (text.empty()) {
			return std::nullopt;
		}

		std::string_view host;
		std::string_view portText;

		if (text.front() == '[') {
			const size_t close = text.find(']');
			if (close == std::string_view::npos || close + 1 >= text.size() || text[close + 1] != ':') {
				return std::nullopt;
			}
			host = text.substr(1, close - 1);
			portText = text.substr(close + 2);
		} else {
			const size_t colon = text.rfind(':');
			if (colon == std::string_view::npos) {
				return std::nullopt;
			}
			host = text.substr(0, colon);
			portText = text.substr(colon + 1);

			// An unbracketed v6 address is refused rather than guessed at. Its
			// last colon and the port separator are the same character, so
			// `::1:53` is either the address `::1:53` or the address `::1` on
			// port 53 and nothing in the text says which.
			if (host.find(':') != std::string_view::npos) {
				return std::nullopt;
			}
		}

		if (host.empty() || portText.empty()) {
			return std::nullopt;
		}

		// Parsed as 32 bits and range-checked afterwards, so `70000` is refused
		// rather than wrapping to a port somebody is listening on.
		uint32_t port = 0;
		const char *const last = portText.data() + portText.size();
		const std::from_chars_result parsed = std::from_chars(portText.data(), last, port);
		if (parsed.ec != std::errc{} || parsed.ptr != last || port > 0xFFFFu) {
			return std::nullopt;
		}

		std::error_code failure;
		const asio::ip::address address = asio::ip::make_address(std::string(host), failure);
		if (failure) {
			return std::nullopt;
		}

		if (address.is_v4()) {
			return FromIPv4(address.to_v4().to_bytes(), static_cast<uint16_t>(port));
		}
		if (address.is_v6()) {
			return FromIPv6(address.to_v6().to_bytes(), static_cast<uint16_t>(port));
		}
		return std::nullopt;
	}
}

namespace std {

	size_t hash<engine::net::Endpoint>::operator()(const engine::net::Endpoint &endpoint) const noexcept {
		// FNV-1a over the address bytes the family actually uses, then the port
		// and the family. Mixing every byte matters here: a subnet's addresses
		// differ only in their last one, and a hash that folded the first four
		// together would put a whole LAN in one bucket.
		constexpr size_t OFFSET = 1469598103934665603ull;
		constexpr size_t PRIME = 1099511628211ull;

		size_t hashed = OFFSET;
		const auto fold = [&hashed](uint8_t byte) {
			hashed ^= byte;
			hashed *= PRIME;
		};

		const size_t width = endpoint.Family == engine::net::AddressFamily::IPv6 ? 16u : 4u;
		for (size_t index = 0; index < width; ++index) {
			fold(static_cast<uint8_t>(endpoint.Address[index]));
		}
		fold(static_cast<uint8_t>(endpoint.Port & 0xFFu));
		fold(static_cast<uint8_t>(endpoint.Port >> 8));
		fold(static_cast<uint8_t>(endpoint.Family));
		return hashed;
	}
}
