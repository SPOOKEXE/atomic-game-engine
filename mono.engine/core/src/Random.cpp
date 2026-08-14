#include <engine/core/Random.hpp>

namespace engine::core {

	namespace {
		// SplitMix64's finaliser, whole and unmodified.
		//
		// From Steele, Lea and Flood, *Fast Splittable Pseudorandom Number
		// Generators* (OOPSLA 2014), where it is `mix64variant13`; the spelling
		// below is Vigna's public-domain `splitmix64.c` and it is what Java's
		// `SplittableRandom` draws through. Three constants and four shifts, all
		// of them published - which is the same property SHA-256 was here for
		// and the whole reason a hand-rolled mixer was not acceptable.
		//
		// The golden-gamma addition is part of the algorithm, not decoration.
		// Without it the finaliser maps zero to zero, and `Bits(0, 0)` - the
		// most-called pair in the engine - would be zero rather than a number.
		uint32_t SplitMix64(uint64_t state) {
			uint64_t z = state + 0x9E3779B97F4A7C15ull;
			z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
			z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
			z = z ^ (z >> 31);
			return static_cast<uint32_t>(z >> 32);
		}
	}

	uint32_t Random::Bits(uint32_t index, uint32_t salt) {
		// **Arithmetic all the way down, so byte order cannot reach the result.**
		// The SHA-256 this replaced had to assemble its input a byte at a time
		// and say why in a comment, because a `memcpy` of two `uint32_t` would
		// have disagreed on a big-endian build without ever failing to compile.
		// A shift and an or have one answer on every machine.
		//
		// The pair is packed rather than mixed together first: the finaliser is
		// a bijection on 64 bits, so distinct `(index, salt)` pairs cannot
		// collide before the truncation to 32.
		return SplitMix64(static_cast<uint64_t>(salt) << 32 | static_cast<uint64_t>(index));
	}

	float Random::Float(uint32_t index, uint32_t salt) {
		// Top 24 bits, scaled by 2^-24. Every 24-bit integer is exactly
		// representable in a float and the scale is a power of two, so this
		// conversion rounds nowhere and gives the same answer on every
		// implementation. Dividing the full 32 bits by 0xFFFFFFFF, as the
		// copy-pasted mixer this type replaced did, both rounds and can return
		// exactly 1.0f.
		return static_cast<float>(Bits(index, salt) >> 8) * 0x1p-24f;
	}

	float Random::Range(uint32_t index, uint32_t salt, float minimum, float maximum) {
		return minimum + Float(index, salt) * (maximum - minimum);
	}
}
