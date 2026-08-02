#pragma once

// Zeroing key material so that it stays zeroed.
//
// Both secrets in this module — a signing seed and a grant key — erase their
// storage on destruction and on the way out of a move, and both want the same
// erase. Private to this module: what needs wiping here is a key, and a wipe
// offered more widely gets reached for by things that merely want a buffer
// cleared.
//
// @tier L8 · shared

#include <cryptopp/misc.h>
#include <cstdint>
#include <span>

namespace engine::assets {

	// Zeroes secret bytes.
	//
	// SecureWipeBuffer rather than a loop or memset: an ordinary store to memory
	// nothing reads again is exactly what a compiler is allowed to delete, and
	// it does.
	//
	// @param bytes The secret to erase, in place.
	inline void SecureWipe(std::span<uint8_t> bytes) {
		CryptoPP::SecureWipeBuffer(bytes.data(), bytes.size());
	}
}
