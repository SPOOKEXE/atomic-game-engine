#pragma once

// SHA-256, from Crypto++.
//
// The cache has to be stable across machines so that CI and a laptop can share
// it, which rules out anything whose result depends on the standard library —
// std::hash is explicitly allowed to differ between implementations and between
// runs of the same one.
//
// This was a hand-written FIPS 180-4 implementation until Crypto++ was
// vendored. The argument for writing it out was that a hundred lines beat
// adding a third-party library for one tool, and that was true while the engine
// had no crypto dependency. It has one now, so the trade went the other way:
// the same digest, none of the block-padding edge cases ours had to get right,
// and SHA-NI on any CPU that has it.
//
// Nothing here is security-sensitive. The property wanted is a stable digest
// with no realistic collisions, which is why the switch is invisible to callers
// — the digests are identical, and tests/Sha256.cpp still checks them against
// the NIST vectors precisely so that stays true.
//
// Crypto++ is deliberately absent from this header. It is linked `VENDOR`, so
// it stays private to this module, and the state lives behind Impl rather than
// as a CryptoPP::SHA256 member — see mono.vendor/AGENTS.md on preferring to hide
// a vendor rather than widening it to VENDOR_PUBLIC.

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

namespace testrunner {

	// A SHA-256 digest, accumulated in pieces.
	//
	// Feed it with Update as many times as there are pieces and read it once
	// with Hex. Streaming rather than one call over a buffer because a suite's
	// signature is its source plus every header it includes, and concatenating
	// all of that into one string first would allocate the whole translation
	// unit to hash it.
	//
	// Move-only: the state is a Crypto++ object behind a unique_ptr, and copying
	// a half-finished digest is not something any caller here wants.
	class Sha256 {
	  public:
		// Bytes in a raw digest. Hex returns twice this many characters.
		static constexpr size_t DIGEST_BYTES = 32;

		Sha256();
		~Sha256();

		// Moves the accumulated state. The source is left usable only for
		// assignment and destruction, as usual.
		Sha256(Sha256 &&) noexcept;

		// Moves the accumulated state, discarding whatever this one had.
		Sha256 &operator=(Sha256 &&) noexcept;

		// Adds bytes to the digest.
		//
		// @param data  Start of the block. Not kept — it is hashed and forgotten.
		// @param bytes How many to read.
		void Update(const void *data, size_t bytes);

		// Adds text to the digest, without copying it.
		void Update(std::string_view text) {
			Update(text.data(), text.size());
		}

		// Lowercase hex. Finalises; do not Update afterwards.
		std::string Hex();

		// The digest of one string, for callers with nothing to stream.
		//
		// @param text The whole input.
		// @return Lowercase hex, the same as Update then Hex.
		static std::string Of(std::string_view text);

	  private:
		struct Impl;
		std::unique_ptr<Impl> Self;
	};
}
