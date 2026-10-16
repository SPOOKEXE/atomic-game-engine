#include <engine/procedural/Random.hpp>

#include <bit>

namespace engine::procedural {

	namespace {
		uint64_t Mix(uint64_t word) {
			word += 0x9e3779b97f4a7c15ull;
			word = (word ^ (word >> 30)) * 0xbf58476d1ce4e5b9ull;
			word = (word ^ (word >> 27)) * 0x94d049bb133111ebull;
			return word ^ (word >> 31);
		}

		uint64_t HashStream(std::string_view stream) {
			uint64_t hash = 0xcbf29ce484222325ull;
			for (char character : stream) {
				hash ^= static_cast<uint8_t>(character);
				hash *= 0x100000001b3ull;
			}
			return hash;
		}
	}

	uint64_t CounterWord(
		uint64_t rootSeed, std::string_view stream, int64_t globalCellX, int64_t globalCellY, uint64_t counter
	) {
		uint64_t word = Mix(rootSeed ^ HashStream(stream));
		word = Mix(word ^ std::bit_cast<uint64_t>(globalCellX));
		word = Mix(word ^ std::bit_cast<uint64_t>(globalCellY));
		return Mix(word ^ counter);
	}
}
