#pragma once

// The one signature in content delivery, and where it sits.
//
// A manifest root is signed with Ed25519. **Nothing below it is signed.**
// Everything under the root — bundles, assets, chunks — is already bound to it
// by the hash tree, so a client that trusts the root and verifies the chain has
// verified the content, and did so with hashes rather than with asymmetric
// crypto. CDN.md §2.
//
// Signing per asset or per request would put an asymmetric operation on the hot
// path and make an origin's throughput a function of a crypto primitive, buying
// nothing. It is one signature per published manifest, verified once per
// session by each client.
//
// **The origin holds no signing key.** Publishing is the studio's and the CLI's
// job; the origin serves bytes it cannot forge, which is what makes it safe to
// deploy on hardware nobody here owns. DATATYPES_LIBRARIES.md puts Ed25519
// signing at `server` and CLI tier and verification at `shared` for that reason
// — a split this module states and cannot enforce, because both halves are one
// `shared` library. `assets/AGENTS.md` records it as the convention it is.
//
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

	// An Ed25519 public key. Safe to publish, and meant to be.
	struct PublicKey {
		// Key length. Part of the format.
		static constexpr size_t BYTES = 32;

		// The key itself.
		std::array<uint8_t, BYTES> Value{};

		// Whether this is the all-zero placeholder rather than a key.
		bool IsZero() const;

		// Lowercase hex, 64 characters — how a key is written into a
		// configuration file or a session descriptor.
		std::string ToHex() const;

		// Parses what ToHex wrote. Refuses anything but 64 lowercase hex
		// characters, for the reason ContentHash::FromHex does.
		//
		// @param text The 64-character lowercase hex key.
		// @return The key, or nothing.
		static std::optional<PublicKey> FromHex(std::string_view text);

		// Whether two keys are the same. Ordinary comparison: a public key is
		// public, and treating it as a secret would imply the wrong thing.
		bool operator==(const PublicKey &other) const = default;
	};

	// An Ed25519 signature over a manifest root.
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
		// Constant-time. A signature is not a secret, but comparing one with an
		// early-out is the habit that eventually gets applied to something that
		// is — and the cost here is 64 bytes of XOR.
		bool operator==(const SignatureBytes &other) const;
	};

	// A signing key. **Server and CLI only, by convention.**
	//
	// Zeroes its own storage on destruction, so a key does not outlive the
	// object in a freed page somebody later reads. That is the floor rather than
	// the ceiling: this type does nothing about a key sitting in a swap file or
	// a core dump, and a deployment that cares has to.
	class SigningKey {
	  public:
		// Seed length. Ed25519's private key *is* its 32-byte seed.
		static constexpr size_t SEED_BYTES = 32;

		// Builds a key from a 32-byte seed.
		//
		// The seed is the whole secret. Where it comes from is the caller's
		// problem and a serious one — `core::Random` is not a cryptographic
		// generator and must not be used for this.
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

		// Signs a manifest root.
		//
		// What is signed is not the bare root but a domain-separated message
		// built from it — see VerifyManifestRoot for why that matters.
		//
		// @param root The manifest root to commit to.
		// @return The signature.
		SignatureBytes SignManifestRoot(const ContentHash &root) const;

		// Signs a session transcript, so a peer can prove which server it is.
		//
		// **The second purpose this key has, and it is the one `net::Handshake`
		// has been asking for since v0.3.** An X25519 agreement is safe against
		// a listener and not against a relay: whoever carries the two messages
		// can substitute its own key and hold a session with each side. A
		// signature over the transcript closes that, because a relay cannot
		// produce one over a transcript containing *its* key that verifies
		// under the server's.
		//
		// The same key a publisher signs manifests with, deliberately: a server
		// and the content it serves are one identity as far as a player is
		// concerned, and two keys would be two things to distribute and two
		// chances to pin the wrong one. **Domain-separated with its own tag**,
		// so a signature over a manifest root can never be replayed as one over
		// a transcript — the reason `VerifyManifestRoot` gives, now that there
		// is a second purpose to be confused with.
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

	// Whether `signature` really is `key`'s signature over `root`.
	//
	// The one call a client makes, and the root of everything it trusts
	// afterwards. Takes no signing key and needs none.
	//
	// **The signed message is domain-separated**, not the bare root. A key that
	// signs manifest roots may one day sign something else, and a signature over
	// 32 opaque bytes is replayable between the two if nothing says which is
	// which. The tag says which.
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
