#pragma once

// What two source files of this library share and nothing outside it needs.
//
// **A private header rather than a wider public one.** The content hash is the
// thing a cached result is keyed on, so `Graph::Hash` and the picture key that
// names one of its outputs have to mix bytes exactly the same way — but a
// caller that could reach the mixer could also produce a key the cache would
// then honour, which is a stale picture nobody can explain.

#include <engine/nodegraph/Types.hpp>

#include <cstddef>
#include <cstdint>
#include <string>

namespace engine::nodegraph {

	// FNV-1a over bytes. A hash and not a checksum: it decides whether a cached
	// result may be reused, so what matters is that equal inputs give equal
	// output on every machine — which rules out anything that hashes a pointer
	// or an address.
	//@{
	inline constexpr uint64_t SEED = 1469598103934665603ull;
	inline constexpr uint64_t PRIME = 1099511628211ull;

	inline uint64_t Mix(uint64_t hash, const void *bytes, size_t count) {
		const auto *walk = static_cast<const unsigned char *>(bytes);
		for (size_t index = 0; index < count; index++) {
			hash ^= walk[index];
			hash *= PRIME;
		}
		return hash;
	}

	inline uint64_t MixText(uint64_t hash, const std::string &text) {
		return Mix(hash, text.data(), text.size());
	}

	uint64_t MixValue(uint64_t hash, const Value &value);
	//@}

	// The compressed node's type, registered on first use.
	//
	// **Not at static-initialisation time**, for the demo node set's reason: a
	// registry filled before `main` is one whose order depends on link order.
	// Its ports and knobs are empty because a compressed node derives its own;
	// what the type carries is the width and the accent.
	//
	// Called by `Graph::Compress` and by `Load`, which is why it is here rather
	// than static to either.
	void EnsureCustomType();
}
