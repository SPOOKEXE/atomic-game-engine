#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/core/SecureWipe.hpp>
#include <engine/net/Cipher.hpp>

#include <cryptopp/chachapoly.h>
#include <cryptopp/misc.h>
#include <cstring>

namespace engine::net {

	namespace {
		// The 96-bit nonce for one frame: the fixed part, then the counter.
		//
		// RFC 8439 §2.8 constructs it exactly this way. Big-endian for the
		// counter, which is the RFC's own worked example — the packet header is
		// little-endian and this is not, because this is the AEAD's convention
		// rather than ours and matching a published vector is worth more than
		// matching the frame around it.
		std::array<uint8_t, Cipher::NONCE_BYTES> NonceFor(std::span<const uint8_t> prefix, uint64_t counter) {
			std::array<uint8_t, Cipher::NONCE_BYTES> nonce{};
			std::memcpy(nonce.data(), prefix.data(), Cipher::NONCE_PREFIX_BYTES);
			for (size_t index = 0; index < sizeof(counter); ++index) {
				nonce[Cipher::NONCE_PREFIX_BYTES + index] =
					static_cast<uint8_t>(counter >> (8 * (sizeof(counter) - 1 - index)));
			}
			return nonce;
		}

		// A readable address for a span that may be empty.
		//
		// An empty payload and empty associated data are both ordinary here, and
		// a null pointer with a zero length trips Crypto++'s own assertions in a
		// debug build.
		const CryptoPP::byte *Address(std::span<const std::byte> bytes) {
			static const CryptoPP::byte NOTHING = 0;
			return bytes.empty() ? &NOTHING : reinterpret_cast<const CryptoPP::byte *>(bytes.data());
		}
	}

	Cipher::Sealer::Sealer(std::span<const std::byte> key, std::span<const std::byte> noncePrefix) {
		std::memcpy(Key.data(), key.data(), KEY_BYTES);
		std::memcpy(NoncePrefix.data(), noncePrefix.data(), NONCE_PREFIX_BYTES);
	}

	Cipher::Sealer::~Sealer() {
		core::SecureWipe(Key);
	}

	Cipher::Sealer::Sealer(Sealer &&other) noexcept
		: Key(other.Key), NoncePrefix(other.NoncePrefix), Counter(other.Counter),
		  Frame(std::move(other.Frame)) {
		// The source is wiped and retired rather than merely left alone. Two
		// Sealers holding this key and counting from this counter is the one way
		// a caller could get a nonce to repeat, so there is never a second.
		core::SecureWipe(other.Key);
		other.Counter = EXHAUSTED;
	}

	Cipher::Sealer &Cipher::Sealer::operator=(Sealer &&other) noexcept {
		if (this != &other) {
			core::SecureWipe(Key);
			Key = other.Key;
			NoncePrefix = other.NoncePrefix;
			Counter = other.Counter;
			Frame = std::move(other.Frame);
			core::SecureWipe(other.Key);
			other.Counter = EXHAUSTED;
		}
		return *this;
	}

	std::optional<Cipher::Sealed>
	Cipher::Sealer::Seal(std::span<const std::byte> plaintext, std::span<const std::byte> associatedData) {
		ENGINE_PROFILE("Cipher::Sealer::Seal");

		if (Counter == EXHAUSTED) {
			// Either 2^64 frames, or a moved-from Sealer. Both mean this key has
			// no nonce left that is provably unused, and sealing anyway is the
			// failure this whole type exists to prevent.
			core::Metrics::Count("net.cipher.exhausted", 1.0);
			return std::nullopt;
		}

		const uint64_t counter = Counter;
		const auto nonce = NonceFor(NoncePrefix, counter);

		Frame.resize(plaintext.size() + TAG_BYTES);

		CryptoPP::ChaCha20Poly1305::Encryption cipher;
		cipher.SetKeyWithIV(Key.data(), Key.size(), nonce.data(), nonce.size());
		cipher.EncryptAndAuthenticate(
			reinterpret_cast<CryptoPP::byte *>(Frame.data()),
			reinterpret_cast<CryptoPP::byte *>(Frame.data() + plaintext.size()),
			TAG_BYTES,
			nonce.data(),
			static_cast<int>(nonce.size()),
			Address(associatedData),
			associatedData.size(),
			Address(plaintext),
			plaintext.size()
		);

		++Counter;
		core::Metrics::Count("net.cipher.sealed", 1.0);
		return Sealed{counter, std::span<const std::byte>(Frame)};
	}

	std::optional<Cipher::Opener>
	Cipher::Opener::FromKey(std::span<const std::byte> key, std::span<const std::byte> noncePrefix) {
		if (key.size() != KEY_BYTES || noncePrefix.size() != NONCE_PREFIX_BYTES) {
			return std::nullopt;
		}
		return Opener(key, noncePrefix);
	}

	Cipher::Opener::Opener(std::span<const std::byte> key, std::span<const std::byte> noncePrefix) {
		std::memcpy(Key.data(), key.data(), KEY_BYTES);
		std::memcpy(NoncePrefix.data(), noncePrefix.data(), NONCE_PREFIX_BYTES);
	}

	Cipher::Opener::~Opener() {
		core::SecureWipe(Key);
	}

	Cipher::Opener::Opener(Opener &&other) noexcept
		: Key(other.Key), NoncePrefix(other.NoncePrefix), Plain(std::move(other.Plain)) {
		core::SecureWipe(other.Key);
	}

	Cipher::Opener &Cipher::Opener::operator=(Opener &&other) noexcept {
		if (this != &other) {
			core::SecureWipe(Key);
			Key = other.Key;
			NoncePrefix = other.NoncePrefix;
			Plain = std::move(other.Plain);
			core::SecureWipe(other.Key);
		}
		return *this;
	}

	std::optional<std::span<const std::byte>> Cipher::Opener::Open(
		uint64_t counter, std::span<const std::byte> sealed, std::span<const std::byte> associatedData
	) {
		ENGINE_PROFILE("Cipher::Opener::Open");

		if (sealed.size() < TAG_BYTES) {
			// A frame too short to hold a tag. Refused before anything is
			// measured against it, because the subtraction below would wrap.
			core::Metrics::Count("net.cipher.refused", 1.0);
			return std::nullopt;
		}

		const size_t plainBytes = sealed.size() - TAG_BYTES;
		const auto nonce = NonceFor(NoncePrefix, counter);

		Plain.resize(plainBytes);

		// Decrypted here and copied nowhere until the tag has verified. Crypto++
		// leaves the output buffer written whether or not the frame was genuine,
		// so this buffer holds attacker-chosen bytes for the length of the call
		// and the caller is never given a view of it in that state.
		CryptoPP::byte empty = 0;
		CryptoPP::byte *plain = plainBytes == 0 ? &empty : reinterpret_cast<CryptoPP::byte *>(Plain.data());

		CryptoPP::ChaCha20Poly1305::Decryption cipher;
		cipher.SetKeyWithIV(Key.data(), Key.size(), nonce.data(), nonce.size());
		const bool verified = cipher.DecryptAndVerify(
			plain,
			Address(sealed) + plainBytes,
			TAG_BYTES,
			nonce.data(),
			static_cast<int>(nonce.size()),
			Address(associatedData),
			associatedData.size(),
			Address(sealed),
			plainBytes
		);

		if (!verified) {
			// One answer for a forged tag, a rewritten header, a wrong counter
			// and a wrong key. Which check failed is information about the key.
			//
			// The buffer is dropped rather than returned. It is not wiped: it
			// holds a would-be plaintext, which is the attacker's own input run
			// through a keystream, and not key material.
			Plain.clear();
			core::Metrics::Count("net.cipher.refused", 1.0);
			return std::nullopt;
		}

		core::Metrics::Count("net.cipher.opened", 1.0);
		return std::span<const std::byte>(Plain);
	}
}
