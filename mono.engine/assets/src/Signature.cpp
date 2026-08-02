#include <engine/assets/Signature.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>

#include <algorithm>
#include <cryptopp/donna.h>
#include <cryptopp/misc.h>
#include <cstring>
#include <string_view>

namespace engine::assets {

	namespace {
		constexpr char HEX_DIGITS[] = "0123456789abcdef";

		constexpr int HexValue(char character) {
			if (character >= '0' && character <= '9') {
				return character - '0';
			}
			if (character >= 'a' && character <= 'f') {
				return character - 'a' + 10;
			}
			return -1;
		}

		template <size_t Length> std::string ToHexString(const std::array<uint8_t, Length> &value) {
			std::string text(Length * 2, '\0');
			for (size_t index = 0; index < Length; ++index) {
				text[index * 2] = HEX_DIGITS[value[index] >> 4];
				text[index * 2 + 1] = HEX_DIGITS[value[index] & 0x0F];
			}
			return text;
		}

		template <size_t Length>
		bool FromHexString(std::string_view text, std::array<uint8_t, Length> &value) {
			if (text.size() != Length * 2) {
				return false;
			}
			for (size_t index = 0; index < Length; ++index) {
				const int high = HexValue(text[index * 2]);
				const int low = HexValue(text[index * 2 + 1]);
				if (high < 0 || low < 0) {
					return false;
				}
				value[index] = static_cast<uint8_t>((high << 4) | low);
			}
			return true;
		}

		// What is actually signed.
		//
		// Not the bare root. A key that signs manifest roots today may sign a
		// grant, a game file or a hotpatch tomorrow, and a signature over 32
		// opaque bytes is replayable between any two of those if nothing in the
		// signed material says which it was. The tag and the version string say
		// it, and they cost one hash.
		//
		// The version is in the tag rather than beside it so that a v2 manifest
		// format cannot be attacked with a v1 signature, and vice versa.
		constexpr std::byte SIGNING_TAG{0x03};
		constexpr std::string_view SIGNING_CONTEXT = "atomic-manifest-v1";

		ContentHash SigningMessage(const ContentHash &root) {
			Hasher hasher;
			hasher.Update(std::span<const std::byte>(&SIGNING_TAG, 1));
			hasher.Update(
				std::span<const std::byte>(
					reinterpret_cast<const std::byte *>(SIGNING_CONTEXT.data()), SIGNING_CONTEXT.size()
				)
			);
			hasher.Update(
				std::span<const std::byte>(
					reinterpret_cast<const std::byte *>(root.Digest.data()), ContentHash::BYTES
				)
			);
			return hasher.Finish();
		}

		void Wipe(std::span<uint8_t> bytes) {
			// SecureWipeBuffer rather than a loop or memset: an ordinary store
			// to memory nothing reads again is exactly what a compiler is
			// allowed to delete, and it does.
			CryptoPP::SecureWipeBuffer(bytes.data(), bytes.size());
		}
	}

	bool PublicKey::IsZero() const {
		return std::all_of(Value.begin(), Value.end(), [](uint8_t byte) { return byte == 0; });
	}

	std::string PublicKey::ToHex() const {
		return ToHexString(Value);
	}

	std::optional<PublicKey> PublicKey::FromHex(std::string_view text) {
		PublicKey key;
		if (!FromHexString(text, key.Value)) {
			return std::nullopt;
		}
		return key;
	}

	bool SignatureBytes::IsZero() const {
		return std::all_of(Value.begin(), Value.end(), [](uint8_t byte) { return byte == 0; });
	}

	std::string SignatureBytes::ToHex() const {
		return ToHexString(Value);
	}

	std::optional<SignatureBytes> SignatureBytes::FromHex(std::string_view text) {
		SignatureBytes signature;
		if (!FromHexString(text, signature.Value)) {
			return std::nullopt;
		}
		return signature;
	}

	bool SignatureBytes::operator==(const SignatureBytes &other) const {
		return CryptoPP::VerifyBufsEqual(Value.data(), other.Value.data(), BYTES);
	}

	std::optional<SigningKey> SigningKey::FromSeed(std::span<const std::byte> seed) {
		if (seed.size() != SEED_BYTES) {
			return std::nullopt;
		}

		SigningKey key;
		std::memcpy(key.Seed.data(), seed.data(), SEED_BYTES);
		CryptoPP::Donna::ed25519_publickey(key.Verifier.Value.data(), key.Seed.data());
		return key;
	}

	SigningKey::~SigningKey() {
		Wipe(Seed);
	}

	SigningKey::SigningKey(SigningKey &&other) noexcept : Seed(other.Seed), Verifier(other.Verifier) {
		// The source is wiped rather than merely left alone. A moved-from key
		// whose seed is still in its storage is a copy of the secret nobody
		// believes exists.
		Wipe(other.Seed);
	}

	SigningKey &SigningKey::operator=(SigningKey &&other) noexcept {
		if (this != &other) {
			Wipe(Seed);
			Seed = other.Seed;
			Verifier = other.Verifier;
			Wipe(other.Seed);
		}
		return *this;
	}

	SignatureBytes SigningKey::SignManifestRoot(const ContentHash &root) const {
		ENGINE_PROFILE("SigningKey::SignManifestRoot");

		const ContentHash message = SigningMessage(root);

		SignatureBytes signature;
		CryptoPP::Donna::ed25519_sign(
			message.Digest.data(),
			ContentHash::BYTES,
			Seed.data(),
			Verifier.Value.data(),
			signature.Value.data()
		);
		return signature;
	}

	bool VerifyManifestRoot(const ContentHash &root, const SignatureBytes &signature, const PublicKey &key) {
		ENGINE_PROFILE("assets::VerifyManifestRoot");

		const ContentHash message = SigningMessage(root);

		// Donna answers 0 for a good signature, which is the C convention and
		// the opposite of what the name reads as. Inverted once, here.
		const bool passed =
			CryptoPP::Donna::ed25519_sign_open(
				message.Digest.data(), ContentHash::BYTES, key.Value.data(), signature.Value.data()
			) == 0;

		// The counter that matters most in this module. A client rejecting a
		// manifest means the publisher's key does not match the content it was
		// handed, and that is either a misconfiguration or an attack — never
		// something to let pass quietly.
		core::Metrics::Count(passed ? "assets.manifest.verified" : "assets.manifest.rejected", 1.0);
		return passed;
	}
}
