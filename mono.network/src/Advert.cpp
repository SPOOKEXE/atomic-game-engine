#include "Codec.hpp"

#include <engine/core/Bytes.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/SecureWipe.hpp>

#include <algorithm>
#include <array>
#include <cryptopp/osrng.h>
#include <network/Advert.hpp>

// One record, one encoding, three ways of travelling. The header carries the
// argument; what is here is the layout and the checking.

namespace network {

	namespace {
		// The frame's magic, so a datagram that is not an advert fails at its
		// first four bytes rather than somewhere inside a string length.
		constexpr uint32_t ADVERT_MAGIC = 0x414E5441; // "ATNA"

		// The frame version. Refused when unknown, for the reason a manifest's
		// is: a reader that guesses at a version mis-parses hostile bytes.
		//
		// **Not `Advert::Protocol`.** This is the shape of the datagram and
		// belongs to this module; that is the shape of the *game* and belongs
		// to whoever is announcing. Two builds of a game that changed its input
		// encoding share this version and differ in that one.
		//
		// **2 - the frame gained `Advert::Transports`.** A version 1 reader
		// would take that byte as the first byte of the name's length prefix
		// and lose everything after it, so the frame is refused whole rather
		// than read forgivingly.
		constexpr uint16_t ADVERT_VERSION = 2;

		// The one flag: whether a tag follows the body.
		constexpr uint8_t FLAG_TAGGED = 0x01;

		// The smallest a well-formed frame can be - every fixed field, plus the
		// two length prefixes of two empty strings. Checked before anything is
		// read so a truncated datagram is refused rather than half-parsed.
		constexpr size_t MINIMUM_BYTES =
			4 + 2 + 1 + SessionId::BYTES + 1 + 1 + 1 + 4 + ENDPOINT_BYTES + 2 + 2 + 4 + 4;

		// Text as it goes on the wire: capped, and cut at the cap rather than
		// refused. A name is cosmetic and dropping a whole announcement over
		// one is worse than a shortened name.
		std::string_view Capped(std::string_view text) {
			return text.substr(0, std::min(text.size(), Advert::MAXIMUM_TEXT_BYTES));
		}

		// Whether a byte is one of a closed list's values.
		bool KnownPurpose(uint8_t value) {
			return value <= static_cast<uint8_t>(Purpose::Content);
		}

		bool KnownAccess(uint8_t value) {
			return value <= static_cast<uint8_t>(Access::Private);
		}

		bool KnownTransports(uint8_t value) {
			return value <= static_cast<uint8_t>(engine::net::WireMode::Both);
		}

		// Everything except the tag, which is what the tag commits to.
		void WriteBody(engine::core::ByteWriter &writer, const Advert &advert, bool tagged) {
			writer.WriteUInt32(ADVERT_MAGIC);
			writer.WriteUInt16(ADVERT_VERSION);
			writer.WriteUInt8(tagged ? FLAG_TAGGED : 0u);
			WriteSessionId(writer, advert.Session);
			writer.WriteUInt8(static_cast<uint8_t>(advert.Use));
			writer.WriteUInt8(static_cast<uint8_t>(advert.Admits));
			writer.WriteUInt8(static_cast<uint8_t>(advert.Transports));
			writer.WriteUInt32(advert.Protocol);
			WriteEndpoint(writer, advert.At);
			writer.WriteUInt16(advert.Peers);
			writer.WriteUInt16(advert.PeerLimit);
			writer.WriteString(Capped(advert.Name));
			writer.WriteString(Capped(advert.Detail));
		}
	}

	bool SessionId::IsValid() const {
		for (const std::byte value : Value) {
			if (value != std::byte{0}) {
				return true;
			}
		}
		return false;
	}

	std::string SessionId::Text() const {
		if (!IsValid()) {
			return "none";
		}

		static constexpr char DIGITS[] = "0123456789abcdef";
		std::string text;
		text.reserve(BYTES * 2);
		for (const std::byte value : Value) {
			const auto byte = static_cast<uint8_t>(value);
			text.push_back(DIGITS[byte >> 4]);
			text.push_back(DIGITS[byte & 0x0Fu]);
		}
		return text;
	}

	std::optional<SessionId> SessionId::Parse(std::string_view text) {
		if (text.size() != BYTES * 2) {
			return std::nullopt;
		}

		const auto digit = [](char character) -> int {
			if (character >= '0' && character <= '9') {
				return character - '0';
			}
			if (character >= 'a' && character <= 'f') {
				return character - 'a' + 10;
			}
			if (character >= 'A' && character <= 'F') {
				return character - 'A' + 10;
			}
			return -1;
		};

		SessionId id;
		for (size_t index = 0; index < BYTES; ++index) {
			const int high = digit(text[index * 2]);
			const int low = digit(text[index * 2 + 1]);
			if (high < 0 || low < 0) {
				return std::nullopt;
			}
			id.Value[index] = static_cast<std::byte>((high << 4) | low);
		}
		return id;
	}

	SessionId SessionId::Draw() {
		SessionId id;
		try {
			CryptoPP::OS_GenerateRandomBlock(
				false, reinterpret_cast<CryptoPP::byte *>(id.Value.data()), BYTES
			);
		} catch (const CryptoPP::Exception &) {
			// The null id, which fails IsValid. A session that could not be
			// named is one that cannot be announced - the alternative would be
			// a predictable id, and every host that could not draw one would
			// announce as the same session.
			return {};
		}
		return id;
	}

	bool Advert::IsValid() const {
		if (!Session.IsValid()) {
			return false;
		}
		return Name.size() <= MAXIMUM_TEXT_BYTES && Detail.size() <= MAXIMUM_TEXT_BYTES;
	}

	std::vector<std::byte> Encode(const Advert &advert, const SessionKey *key) {
		engine::core::ByteWriter writer;
		WriteBody(writer, advert, key != nullptr);

		std::vector<std::byte> datagram(writer.Bytes().begin(), writer.Bytes().end());
		if (key != nullptr) {
			const std::array<std::byte, SessionKey::TAG_BYTES> tag = key->Tag(datagram);
			datagram.insert(datagram.end(), tag.begin(), tag.end());
		}
		return datagram;
	}

	std::optional<DecodedAdvert>
	Decode(std::span<const std::byte> datagram, std::span<const SessionKey> keys) {
		// **Eight refusals share one `Tally.Malformed`.** "I cannot see the
		// server" and "I can see it and will not list it" are the same number
		// from outside, so each refusal names itself. Rate-limited because an
		// open UDP port carries whatever anybody sends it.
		const auto refuse = [](const char *why) -> std::optional<DecodedAdvert> {
			ENGINE_DEBUG_EVERY(1.0, "advert refused: {}", why);
			return std::nullopt;
		};

		if (datagram.size() < MINIMUM_BYTES) {
			return refuse("shorter than the smallest frame");
		}

		engine::core::ByteReader reader(datagram);
		if (reader.ReadUInt32() != ADVERT_MAGIC || reader.ReadUInt16() != ADVERT_VERSION) {
			return refuse("the magic or the frame version is not this build's");
		}

		const uint8_t flags = reader.ReadUInt8();
		const bool tagged = (flags & FLAG_TAGGED) != 0;
		// Any other bit is a frame a later version wrote. Refused rather than
		// ignored: a flag this build does not know about may say the fields
		// after it are laid out differently, and reading them anyway is exactly
		// the guess the version check exists to prevent.
		if ((flags & ~FLAG_TAGGED) != 0) {
			return refuse("a flag bit this build does not know");
		}

		DecodedAdvert decoded;
		Advert &advert = decoded.Session;
		advert.Session = ReadSessionId(reader);

		const uint8_t use = reader.ReadUInt8();
		const uint8_t admits = reader.ReadUInt8();
		const uint8_t transports = reader.ReadUInt8();
		if (!KnownPurpose(use) || !KnownAccess(admits) || !KnownTransports(transports)) {
			return refuse("a purpose, access or transport outside its closed list");
		}
		advert.Use = static_cast<Purpose>(use);
		advert.Admits = static_cast<Access>(admits);
		advert.Transports = static_cast<engine::net::WireMode>(transports);

		advert.Protocol = reader.ReadUInt32();
		advert.At = ReadEndpoint(reader);
		advert.Peers = reader.ReadUInt16();
		advert.PeerLimit = reader.ReadUInt16();

		const std::string_view name = reader.ReadString();
		const std::string_view detail = reader.ReadString();
		if (reader.Failed() || name.size() > Advert::MAXIMUM_TEXT_BYTES ||
			detail.size() > Advert::MAXIMUM_TEXT_BYTES) {
			return refuse("truncated, or a name or detail past the cap");
		}
		advert.Name.assign(name);
		advert.Detail.assign(detail);

		if (!advert.Session.IsValid()) {
			return refuse("the session id is all zeros");
		}

		const size_t body = reader.Position();
		if (!tagged) {
			// Trailing rubbish is a refusal. A frame whose fields ended before
			// its bytes did is one somebody appended to, and accepting it would
			// let two datagrams that decode identically carry different bytes -
			// which is the last thing a format a tag commits to should allow.
			return body == datagram.size() ? std::optional<DecodedAdvert>(std::move(decoded))
										   : refuse("bytes after the last field, on an untagged frame");
		}

		if (datagram.size() != body + SessionKey::TAG_BYTES) {
			return refuse("the tagged frame is not exactly one tag longer than its body");
		}

		const std::span<const std::byte> covered = datagram.first(body);
		const std::span<const std::byte> tag = datagram.subspan(body);
		for (const SessionKey &key : keys) {
			if (key.Admits(covered, tag)) {
				decoded.Authenticated = true;
				break;
			}
		}
		return decoded;
	}
}

namespace std {

	size_t hash<network::SessionId>::operator()(const network::SessionId &id) const noexcept {
		// The id is already uniformly random, so the hash is the first
		// `size_t` of it rather than a mixing function over sixteen bytes that
		// are their own digest. FNV over random bytes buys nothing.
		size_t value = 0;
		for (size_t index = 0; index < sizeof(size_t) && index < network::SessionId::BYTES; ++index) {
			value |= static_cast<size_t>(static_cast<uint8_t>(id.Value[index])) << (index * 8);
		}
		return value;
	}
}
