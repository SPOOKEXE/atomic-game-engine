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

	class Sha256 {
	  public:
		static constexpr size_t DIGEST_BYTES = 32;

		Sha256();
		~Sha256();

		Sha256(Sha256 &&) noexcept;
		Sha256 &operator=(Sha256 &&) noexcept;

		void Update(const void *data, size_t bytes);
		void Update(std::string_view text) {
			Update(text.data(), text.size());
		}

		// Lowercase hex. Finalises; do not Update afterwards.
		std::string Hex();

		static std::string Of(std::string_view text);

	  private:
		struct Impl;
		std::unique_ptr<Impl> Self;
	};
}
