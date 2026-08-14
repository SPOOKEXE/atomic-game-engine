#pragma once

// The secret that makes a session private, and the only thing in this module
// that is secret at all.
//
// One key, held by the host and by everyone the host gave it to. It does two
// jobs and no others:
//
// - **It authenticates an announcement.** A private session's advert carries a
//   MAC under this key, so a browser that holds the key can tell the session it
//   was invited to from somebody else's session wearing the same name.
// - **It gates a rendezvous registration.** A point will not hand out the
//   address of a private session to a client that cannot prove it holds the
//   key, which is what stops a public rendezvous from being a directory of
//   everybody's private games.
//
// **It is not a transport key and there is nothing here that encrypts.**
// `engine::net::Handshake` already derives per-session ciphers from an
// ephemeral exchange, and forward secrecy is the whole reason it is ephemeral -
// a configured key that also encrypted traffic would make every past session
// readable from one file on one machine. So this authenticates and stops.
//
// **A MAC rather than a signature**, for `assets::Grant`'s reason: both ends of
// a private session are one trust domain, HMAC-SHA256 verification is cheap,
// and an asymmetric operation per announcement would buy nothing. The
// asymmetric key is where the trust boundary actually is - `assets::Signature`
// over a manifest, `replication::ConnectorSettings::ServerIdentity` over a
// server.
//
// @tier shared

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace network {

	// A pre-shared secret, and the tags it makes.
	//
	// Move-only and self-wiping, like every other holder of key material in
	// this repository: two copies of a secret is one more place for it to be
	// read out of than there needs to be.
	//
	// @since v0.13
	class SessionKey {
	  public:
		// Key length. 32 bytes, matching HMAC-SHA256's block-derived strength.
		static constexpr size_t BYTES = 32;

		// Tag length. The full HMAC-SHA256 output, untruncated - the thing
		// being tagged is a whole announcement, and 16 saved bytes off a
		// datagram that already carries two strings is not worth the argument
		// about how much strength truncation costs.
		static constexpr size_t TAG_BYTES = 32;

		// How many rounds `FromPassphrase` stretches over.
		//
		// **Chosen against what an attacker gets, not against what it costs
		// us.** A key derived here is derived once at start-up, so 200000
		// rounds is a fifth of a second nobody notices; the same figure turns
		// each guess against a stolen announcement into a fifth of a second
		// too, which is the only number that matters. It is written down here
		// because changing it changes every key ever derived from a
		// passphrase - see FromPassphrase.
		static constexpr uint32_t PASSPHRASE_ROUNDS = 200000;

		// Builds a key from shared secret material.
		//
		// Where the secret comes from is the deployment's problem.
		// `core::Random` is a deterministic simulation generator and must not
		// be used for one.
		//
		// @param secret Exactly BYTES of shared, high-entropy material.
		// @return The key, or nothing if the secret is the wrong length.
		static std::optional<SessionKey> FromSecret(std::span<const std::byte> secret);

		// Stretches a passphrase into a key.
		//
		// **This is the call a person uses**, and it exists because the
		// alternative is that nobody uses any of it: "tell your friends the
		// passphrase" is a thing people do, and "tell your friends these 64 hex
		// characters" is a thing they do once.
		//
		// PBKDF2-HMAC-SHA256 over a fixed salt. A fixed salt is the wrong
		// choice for a password database and the right one here: both ends have
		// to derive the *same* key from the same words with nothing else
		// exchanged, so there is nowhere for a per-key salt to come from. What
		// that costs is precomputation - a table of common passphrases works
		// against every session ever - and what it buys is a session two people
		// can join by agreeing on a sentence. Pick a sentence.
		//
		// **The salt and the round count are part of the key.** Changing either
		// silently changes every key derived from every passphrase, and the
		// symptom is a session nobody can join any more with the words that
		// worked yesterday. If either has to change, the version in the advert
		// changes with it.
		//
		// @param passphrase The words, taken as their exact bytes. Not trimmed
		//        and not case-folded: doing either would make two passphrases
		//        that differ collide, and a person who typed a trailing space
		//        should be told the key is wrong rather than quietly given
		//        somebody else's.
		// @return The key, or nothing for an empty passphrase.
		static std::optional<SessionKey> FromPassphrase(std::string_view passphrase);

		// Reads what Text wrote.
		//
		// @param text Exactly 64 hexadecimal characters, either case.
		// @return The key, or nothing when the text is not one.
		static std::optional<SessionKey> FromText(std::string_view text);

		// Draws a fresh key from the operating system's entropy.
		//
		// @return The key, or nothing if the operating system refused - which
		//         is a refusal to host a private session, never a fallback to a
		//         weaker source.
		static std::optional<SessionKey> Draw();

		// Zeroes the secret.
		~SessionKey();

		SessionKey(const SessionKey &) = delete;
		SessionKey &operator=(const SessionKey &) = delete;

		// Moves the key, leaving the source zeroed.
		SessionKey(SessionKey &&other) noexcept;

		// Moves the key, zeroing both this key's old secret and the source's.
		SessionKey &operator=(SessionKey &&other) noexcept;

		// The key as 64 lowercase hexadecimal characters.
		//
		// **A deliberate exception to "key material is never printed", and the
		// reason is that this key's whole job is to be shared.** A host draws
		// one and has to be able to give it to somebody; a key that could only
		// live inside the process that made it would mean every private session
		// is a session of one. `assets::GrantKey` has no such method because
		// nothing is ever supposed to carry one out of a deployment.
		//
		// @return The text. Copy it, hand it over, and treat it as the secret
		//         it is.
		std::string Text() const;

		// Tags a block of bytes.
		//
		// @param over Whatever the tag has to commit to.
		// @return The tag.
		std::array<std::byte, TAG_BYTES> Tag(std::span<const std::byte> over) const;

		// Whether a tag is one this key made over these bytes.
		//
		// Compared in constant time. A comparison that stopped at the first
		// wrong byte would let a tag be recovered a byte at a time by timing
		// the refusals, which is a forgery with more steps.
		//
		// @param over The bytes the tag should commit to.
		// @param tag  The tag presented, of any length - a wrong length is a
		//        refusal rather than a read past the end.
		// @return Whether the tag verifies.
		bool Admits(std::span<const std::byte> over, std::span<const std::byte> tag) const;

	  private:
		SessionKey() = default;

		std::array<uint8_t, BYTES> Secret{};
	};
}
