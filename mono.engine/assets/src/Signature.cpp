#include "Hex.hpp"

#include <engine/assets/Signature.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/core/SecureWipe.hpp>

#include <algorithm>
#include <cryptopp/donna.h>
#include <cryptopp/misc.h>
#include <cstring>
#include <string_view>

namespace engine::assets {

	namespace {
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
		core::SecureWipe(Seed);
	}

	SigningKey::SigningKey(SigningKey &&other) noexcept : Seed(other.Seed), Verifier(other.Verifier) {
		// The source is wiped rather than merely left alone. A moved-from key
		// whose seed is still in its storage is a copy of the secret nobody
		// believes exists.
		core::SecureWipe(other.Seed);
	}

	SigningKey &SigningKey::operator=(SigningKey &&other) noexcept {
		if (this != &other) {
			core::SecureWipe(Seed);
			Seed = other.Seed;
			Verifier = other.Verifier;
			core::SecureWipe(other.Seed);
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
