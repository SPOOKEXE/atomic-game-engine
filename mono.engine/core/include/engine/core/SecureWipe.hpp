#pragma once

// Zeroing secret bytes so that they stay zeroed.
//
// **An ordinary store to memory nothing reads again is exactly what a compiler
// is allowed to delete, and it does.** A key erased with `memset` at the end of
// a destructor is frequently not erased at all: the write is dead by every rule
// the optimiser has, and the bytes survive in the heap block, in a register
// spill, or in whatever allocates that address next. This is the call that is
// not allowed to be deleted.
//
// **Not a general "clear a buffer".** It is slower than `memset` - that is the
// entire point - and reaching for it to blank an image or reset a vector is
// paying for a guarantee that has nothing to do with the problem. If the bytes
// are not secret, `std::fill` is the right call.
//
// It lives here rather than beside one of its callers because it had three
// implementations: `assets` wiped a signing seed and a grant key, and `net`
// wiped a cipher key and a handshake secret, and each copy carried its own
// spelling of the same reasoning. A module may not include another module's
// private header, so the only place one copy can serve all of them is the one
// every module already links. Three copies of a security primitive is three
// chances for one of them to be quietly weakened.
//
// @tier L1 · shared

#include <cstddef>
#include <cstdint>
#include <span>

namespace engine::core {

	// Zeroes secret bytes, in a way the optimiser may not remove.
	//
	// @param bytes The secret to erase, in place.
	void SecureWipe(std::span<uint8_t> bytes);

	// Zeroes secret bytes, in a way the optimiser may not remove.
	//
	// The `std::byte` spelling, for callers whose buffers are typed that way.
	// Two overloads rather than one and a cast at every call site: a
	// `reinterpret_cast` in front of a security primitive is a place for a
	// wrong length to hide.
	//
	// @param bytes The secret to erase, in place.
	void SecureWipe(std::span<std::byte> bytes);
}
