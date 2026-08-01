#include <engine/core/Random.hpp>

#include <cryptopp/sha.h>

namespace engine::core {

	uint32_t Random::Bits(uint32_t index, uint32_t salt) {
		// Big-endian, written a byte at a time. The value must not depend on
		// the host's byte order — a server on one machine and a client on
		// another have to agree, and memcpy of a uint32_t would make them
		// disagree on a big-endian build without ever failing to compile.
		const CryptoPP::byte input[8] = {
			static_cast<CryptoPP::byte>(index >> 24),
			static_cast<CryptoPP::byte>(index >> 16),
			static_cast<CryptoPP::byte>(index >> 8),
			static_cast<CryptoPP::byte>(index),
			static_cast<CryptoPP::byte>(salt >> 24),
			static_cast<CryptoPP::byte>(salt >> 16),
			static_cast<CryptoPP::byte>(salt >> 8),
			static_cast<CryptoPP::byte>(salt),
		};

		CryptoPP::byte digest[CryptoPP::SHA256::DIGESTSIZE];

		// One block, one compression. CalculateDigest does Update and Final
		// together and restarts the object, so constructing it per call costs
		// eight word stores and nothing else. On any CPU with SHA-NI — which
		// Crypto++ selects at runtime — the compression itself is a handful of
		// instructions.
		CryptoPP::SHA256 hash;
		hash.CalculateDigest(digest, input, sizeof(input));

		return static_cast<uint32_t>(digest[0]) << 24 | static_cast<uint32_t>(digest[1]) << 16 |
			   static_cast<uint32_t>(digest[2]) << 8 | static_cast<uint32_t>(digest[3]);
	}

	float Random::Float(uint32_t index, uint32_t salt) {
		// Top 24 bits, scaled by 2^-24. Every 24-bit integer is exactly
		// representable in a float and the scale is a power of two, so this
		// conversion rounds nowhere and gives the same answer on every
		// implementation. Dividing the full 32 bits by 0xFFFFFFFF, as the
		// mixer this replaced did, both rounds and can return exactly 1.0f.
		return static_cast<float>(Bits(index, salt) >> 8) * 0x1p-24f;
	}

	float Random::Range(uint32_t index, uint32_t salt, float minimum, float maximum) {
		return minimum + Float(index, salt) * (maximum - minimum);
	}
}
