#include <engine/core/Metrics.hpp>
#include <engine/core/SecureWipe.hpp>

#include <array>
#include <cctype>
#include <cryptopp/hmac.h>
#include <cryptopp/misc.h>
#include <cryptopp/osrng.h>
#include <cryptopp/pwdbased.h>
#include <cryptopp/sha.h>
#include <network/SessionKey.hpp>

namespace network {

	namespace {
		// The salt every passphrase is stretched over.
		//
		// Fixed, and the header says why at length: both ends have to derive
		// the same key from the same words with nothing exchanged, so there is
		// nowhere for a per-key salt to come from. It names the version because
		// changing the derivation has to change the label that goes with it -
		// two builds disagreeing about how a passphrase becomes a key is a
		// session nobody can join, and the reason would be invisible.
		constexpr char PASSPHRASE_SALT[] = "atomic.network.session.v1";

		// The hexadecimal digit a character is, or 16 for anything else.
		uint8_t HexValue(char character) {
			if (character >= '0' && character <= '9') {
				return static_cast<uint8_t>(character - '0');
			}
			if (character >= 'a' && character <= 'f') {
				return static_cast<uint8_t>(character - 'a' + 10);
			}
			if (character >= 'A' && character <= 'F') {
				return static_cast<uint8_t>(character - 'A' + 10);
			}
			return 16;
		}
	}

	std::optional<SessionKey> SessionKey::FromSecret(std::span<const std::byte> secret) {
		if (secret.size() != BYTES) {
			return std::nullopt;
		}
		SessionKey key;
		for (size_t index = 0; index < BYTES; ++index) {
			key.Secret[index] = static_cast<uint8_t>(secret[index]);
		}
		return key;
	}

	std::optional<SessionKey> SessionKey::FromPassphrase(std::string_view passphrase) {
		if (passphrase.empty()) {
			return std::nullopt;
		}

		SessionKey key;
		CryptoPP::PKCS5_PBKDF2_HMAC<CryptoPP::SHA256> stretch;
		stretch.DeriveKey(
			key.Secret.data(),
			key.Secret.size(),
			0,
			reinterpret_cast<const CryptoPP::byte *>(passphrase.data()),
			passphrase.size(),
			reinterpret_cast<const CryptoPP::byte *>(PASSPHRASE_SALT),
			sizeof(PASSPHRASE_SALT) - 1,
			PASSPHRASE_ROUNDS,
			0.0
		);
		return key;
	}

	std::optional<SessionKey> SessionKey::FromText(std::string_view text) {
		if (text.size() != BYTES * 2) {
			return std::nullopt;
		}

		SessionKey key;
		for (size_t index = 0; index < BYTES; ++index) {
			const uint8_t high = HexValue(text[index * 2]);
			const uint8_t low = HexValue(text[index * 2 + 1]);
			if (high > 15 || low > 15) {
				// Wiped before the refusal: half a key derived from half a
				// valid string is still key-shaped material sitting in memory.
				engine::core::SecureWipe(std::span<uint8_t>(key.Secret));
				return std::nullopt;
			}
			key.Secret[index] = static_cast<uint8_t>((high << 4) | low);
		}
		return key;
	}

	std::optional<SessionKey> SessionKey::Draw() {
		SessionKey key;
		try {
			CryptoPP::OS_GenerateRandomBlock(false, key.Secret.data(), key.Secret.size());
		} catch (const CryptoPP::Exception &) {
			engine::core::SecureWipe(std::span<uint8_t>(key.Secret));
			engine::core::Metrics::Count("network.key.no_entropy", 1.0);
			return std::nullopt;
		}
		return key;
	}

	SessionKey::~SessionKey() {
		engine::core::SecureWipe(std::span<uint8_t>(Secret));
	}

	SessionKey::SessionKey(SessionKey &&other) noexcept : Secret(other.Secret) {
		// The source keeps nothing, for `Cookie`'s reason: a moved-from object
		// whose secret is still in its storage is a copy of that secret nobody
		// believes exists.
		engine::core::SecureWipe(std::span<uint8_t>(other.Secret));
	}

	SessionKey &SessionKey::operator=(SessionKey &&other) noexcept {
		if (this != &other) {
			engine::core::SecureWipe(std::span<uint8_t>(Secret));
			Secret = other.Secret;
			engine::core::SecureWipe(std::span<uint8_t>(other.Secret));
		}
		return *this;
	}

	std::string SessionKey::Text() const {
		static constexpr char DIGITS[] = "0123456789abcdef";
		std::string text;
		text.reserve(BYTES * 2);
		for (const uint8_t value : Secret) {
			text.push_back(DIGITS[value >> 4]);
			text.push_back(DIGITS[value & 0x0Fu]);
		}
		return text;
	}

	std::array<std::byte, SessionKey::TAG_BYTES> SessionKey::Tag(std::span<const std::byte> over) const {
		CryptoPP::HMAC<CryptoPP::SHA256> mac(Secret.data(), Secret.size());
		if (!over.empty()) {
			mac.Update(reinterpret_cast<const CryptoPP::byte *>(over.data()), over.size());
		}

		std::array<std::byte, TAG_BYTES> tag{};
		mac.Final(reinterpret_cast<CryptoPP::byte *>(tag.data()));
		return tag;
	}

	bool SessionKey::Admits(std::span<const std::byte> over, std::span<const std::byte> tag) const {
		if (tag.size() != TAG_BYTES) {
			// A wrong length is a refusal rather than a comparison over
			// whichever bytes happened to be there.
			return false;
		}

		const std::array<std::byte, TAG_BYTES> expected = Tag(over);
		return CryptoPP::VerifyBufsEqual(
			reinterpret_cast<const CryptoPP::byte *>(expected.data()),
			reinterpret_cast<const CryptoPP::byte *>(tag.data()),
			TAG_BYTES
		);
	}
}
