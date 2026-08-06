#pragma once

// Manifest roots use one Ed25519 signature; hashes bind descendants.
// @tier L8 · shared

#include <engine/assets/ContentHash.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace engine::assets {

	// An Ed25519 public key.
	struct PublicKey {
		// Key length. Part of the format.
		static constexpr size_t BYTES = 32;

		// The key itself.
		std::array<uint8_t, BYTES> Value{};

		// Whether this is the all-zero placeholder rather than a key.
		bool IsZero() const;

		// Lowercase hex, 64 characters.
		std::string ToHex() const;

		// Parses 64 lowercase hex characters.
		//
		// @param text The 64-character lowercase hex key.
		// @return The key, or nothing.
		static std::optional<PublicKey> FromHex(std::string_view text);

		// Whether two keys are the same. Ordinary comparison: a public key is
		// public, and treating it as a secret would imply the wrong thing.
		bool operator==(const PublicKey &other) const = default;
	};

	// An Ed25519 signature over a domain-separated message.
	struct SignatureBytes {
		// Signature length. Part of the format.
		static constexpr size_t BYTES = 64;

		// r || s, as Ed25519 defines it.
		std::array<uint8_t, BYTES> Value{};

		// Whether this is the all-zero placeholder rather than a signature.
		bool IsZero() const;

		// Lowercase hex, 128 characters.
		std::string ToHex() const;

		// Parses what ToHex wrote. Refuses anything but 128 lowercase hex
		// characters.
		//
		// @param text The 128-character lowercase hex signature.
		// @return The signature, or nothing.
		static std::optional<SignatureBytes> FromHex(std::string_view text);

		// Whether two signatures are the same.
		//
		// Constant-time comparison.
		bool operator==(const SignatureBytes &other) const;
	};

	// A signing key. Server and CLI use only; storage is zeroed on destruction.
	class SigningKey {
	  public:
		// Seed length. Ed25519's private key *is* its 32-byte seed.
		static constexpr size_t SEED_BYTES = 32;

		// Builds a key from exactly 32 secret bytes.
		//
		// @param seed Exactly SEED_BYTES of secret, high-entropy material.
		// @return The key, or nothing if the seed is the wrong length.
		static std::optional<SigningKey> FromSeed(std::span<const std::byte> seed);

		// Zeroes the seed.
		~SigningKey();

		SigningKey(const SigningKey &) = delete;
		SigningKey &operator=(const SigningKey &) = delete;

		// Moves the key, leaving the source zeroed.
		SigningKey(SigningKey &&other) noexcept;

		// Moves the key, zeroing both the destination's old seed and the
		// source's.
		SigningKey &operator=(SigningKey &&other) noexcept;

		// The public half, which is what a client is given.
		const PublicKey &Public() const {
			return Verifier;
		}

		// Signs a domain-separated manifest-root message.
		//
		// @param root The manifest root to commit to.
		// @return The signature.
		SignatureBytes SignManifestRoot(const ContentHash &root) const;

		// Signs a domain-separated session transcript.
		//
		// @param transcript The bytes both sides agree the exchange was.
		// @return The signature.
		// @since v0.9
		SignatureBytes SignSessionTranscript(std::span<const std::byte> transcript) const;

	  private:
		SigningKey() = default;

		std::array<uint8_t, SEED_BYTES> Seed{};
		PublicKey Verifier;
	};

	// Verifies a domain-separated manifest-root signature.
	//
	// @param root The manifest root being checked.
	// @param signature The signature to check.
	// @param key The publisher's public key.
	// @return True only if the signature is valid for exactly this root.
	bool VerifyManifestRoot(const ContentHash &root, const SignatureBytes &signature, const PublicKey &key);

	// Whether `signature` really is `key`'s signature over `transcript`.
	//
	// The call a client makes to learn that the server it agreed keys with is
	// the server it meant to. Takes no signing key and needs none.
	//
	// @param transcript The transcript being checked.
	// @param signature  The signature to check.
	// @param key        The server's pinned public key.
	// @return True only if the signature is valid for exactly this transcript.
	// @since v0.9
	bool VerifySessionTranscript(
		std::span<const std::byte> transcript, const SignatureBytes &signature, const PublicKey &key
	);
}
