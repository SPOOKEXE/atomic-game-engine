#include "Hex.hpp"

#include <engine/assets/ContentHash.hpp>

#include <algorithm>
#include <blake3.h>
#include <cstring>

namespace engine::assets {

	bool ContentHash::IsZero() const {
		return std::all_of(Digest.begin(), Digest.end(), [](uint8_t byte) { return byte == 0; });
	}

	std::string ContentHash::ToHex() const {
		return ToHexString(Digest);
	}

	std::optional<ContentHash> ContentHash::FromHex(std::string_view text) {
		ContentHash hash;
		if (!FromHexString(text, hash.Digest)) {
			return std::nullopt;
		}
		return hash;
	}

	Hasher::Hasher() {
		// The header sizes State without being able to name this type. Checked
		// here rather than trusted - inside a member, because STATE_BYTES is
		// private - so a BLAKE3 bump that grows the struct stops the build
		// instead of writing past the buffer at run time.
		static_assert(
			sizeof(blake3_hasher) <= STATE_BYTES, "blake3_hasher outgrew Hasher::STATE_BYTES - raise it."
		);
		static_assert(
			alignof(blake3_hasher) <= STATE_ALIGNMENT,
			"blake3_hasher wants more alignment than Hasher::State provides."
		);

		blake3_hasher_init(reinterpret_cast<blake3_hasher *>(State));
	}

	void Hasher::Update(std::span<const std::byte> bytes) {
		if (bytes.empty()) {
			return;
		}
		blake3_hasher_update(reinterpret_cast<blake3_hasher *>(State), bytes.data(), bytes.size());
	}

	ContentHash Hasher::Finish() const {
		ContentHash hash;
		// Finalize does not consume the state, which is what lets a caller take
		// a digest of the stream so far and keep going.
		blake3_hasher_finalize(
			reinterpret_cast<const blake3_hasher *>(State), hash.Digest.data(), ContentHash::BYTES
		);
		return hash;
	}

	void Hasher::Reset() {
		blake3_hasher_init(reinterpret_cast<blake3_hasher *>(State));
	}

	ContentHash Hasher::Of(std::span<const std::byte> bytes) {
		Hasher hasher;
		hasher.Update(bytes);
		return hasher.Finish();
	}
}
