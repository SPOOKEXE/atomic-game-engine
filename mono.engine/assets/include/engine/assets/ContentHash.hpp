#pragma once

// What a piece of content is called.
//
// Everything in content delivery is named by the hash of its bytes and never by
// where it sits: a chunk, an asset, a bundle and the manifest are each a
// `ContentHash`. A path that reaches the request layer is a bug, because a path
// can be walked and a hash cannot.
//
// The hash is BLAKE3-256, picked for content addressing because that job is
// adversarial by definition: anyone can run a
// server, so a chunk's address has to be collision-resistant against somebody
// trying rather than merely against accident. A fast non-cryptographic hash
// here is a content-poisoning bug, not a performance decision.
//
// **Changing the algorithm rehashes every byte anyone has ever stored.** There
// is no algorithm tag in this type for that reason: two ways to address content
// is the one place a second mechanism cannot be allowed to accumulate callers.
//
// @tier L8 · shared

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace engine::assets {

	// A 32-byte BLAKE3 digest, and the only name content has.
	//
	// Ordered, so that a list of hashes has one canonical arrangement. Every
	// list in the manifest is sorted by this, because a manifest that differs
	// run to run cannot be diffed and cannot be cached - the same discipline
	// snapshots already follow.
	struct ContentHash {
		// Digest length. Not configurable: it is part of the format.
		static constexpr size_t BYTES = 32;

		// The digest itself. Zero-initialised, which is not a valid hash of
		// anything - see IsZero.
		std::array<uint8_t, BYTES> Digest{};

		// Byte-wise ordering, for the canonical sort every manifest list uses.
		auto operator<=>(const ContentHash &other) const = default;

		// Equality. Not constant-time, and deliberately not: a content address
		// is public, and treating it as a secret would imply the wrong thing
		// about the rest of the format. Secrets are compared in Signature.hpp.
		bool operator==(const ContentHash &other) const = default;

		// Whether this is the all-zero placeholder rather than a real digest.
		//
		// A default-constructed hash is a hash of nothing at all - the empty
		// input has its own non-zero digest - so zero is safe to use as "not
		// set" and this is how that is asked.
		bool IsZero() const;

		// Lowercase hex, 64 characters. What a log line, a cache key on disk
		// and a URL path segment all use.
		std::string ToHex() const;

		// Parses what ToHex wrote.
		//
		// Rejects anything that is not exactly 64 hex characters, including a
		// `0x` prefix and uppercase. One spelling, because two spellings of one
		// address is two cache keys for one piece of content.
		//
		// @param text The 64-character lowercase hex digest.
		// @return The hash, or nothing if the text is not one.
		static std::optional<ContentHash> FromHex(std::string_view text);
	};

	// Hashes a byte stream, in one call or in pieces.
	//
	// Construct, Update as many times as there are pieces, then Finish. The
	// streaming form is the one content delivery actually uses: a chunk arriving
	// over a socket is verified as it lands rather than after it is whole, which
	// is the property content addressing is built on.
	//
	// Holds no allocation. The state is inline, so hashing a million chunks
	// costs a million hashes and no trips to the allocator.
	class Hasher {
	  public:
		// Starts a fresh hash.
		Hasher();

		// Adds bytes to the hash. Any number of calls; only the concatenation
		// matters, so how the caller split its input cannot change the result.
		//
		// @param bytes The next piece of the stream.
		void Update(std::span<const std::byte> bytes);

		// The digest of everything given to Update so far.
		//
		// Does not end the hash: Update may follow, and Finish may be called
		// again for the longer stream. That is what lets one pass over a file
		// produce both a running digest and a final one.
		ContentHash Finish() const;

		// Discards the state and starts again, without releasing anything.
		//
		// For a loop that hashes many chunks - reusing one Hasher keeps the
		// inline state hot instead of reconstructing it per chunk.
		void Reset();

		// The digest of one buffer, for the common case.
		//
		// @param bytes The whole input.
		// @return Its BLAKE3-256 digest.
		static ContentHash Of(std::span<const std::byte> bytes);

	  private:
		// Storage for the vendored hasher, sized here so that `blake3.h` stays
		// out of this header - AGENTS.md refuses a vendor type in a public
		// header, and a `unique_ptr` instead would put an allocation on the path
		// that hashes every chunk of every asset.
		//
		// The real size is checked against this in the source file, so a vendor
		// bump that grows the struct fails to compile rather than overflowing.
		static constexpr size_t STATE_BYTES = 2048;

		// Named rather than written inline in the alignas, so the source file
		// can assert against the number the member actually got. `alignof` on
		// the array type answers 1 - it describes `unsigned char`, not the
		// alignment this alignas imposes - so a check written that way passes or
		// fails for reasons unrelated to the question.
		static constexpr size_t STATE_ALIGNMENT = 16;

		alignas(STATE_ALIGNMENT) unsigned char State[STATE_BYTES]{};
	};
}
