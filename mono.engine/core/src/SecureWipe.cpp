#include <engine/core/SecureWipe.hpp>

namespace engine::core {

	namespace {
		// **Through a `volatile` pointer, which is the standard's own way of
		// saying "this store is observable".** The compiler is not permitted to
		// prove such a write dead and remove it, and every other spelling it
		// may: that is the entire subject of the header above.
		//
		// This was `CryptoPP::SecureWipeBuffer`, which is the same loop. It is
		// written out here because `Engine::core` stopped linking Crypto++ when
		// `Random` did — one primitive of four lines is not a reason for the
		// bottom layer of the engine to depend on a cryptography library, and
		// keeping the dependency for it would have been the whole cost of the
		// change with none of the point.
		void WipeBytes(uint8_t *data, size_t count) {
			volatile uint8_t *cursor = data;
			while (count-- > 0) {
				*cursor++ = 0;
			}
		}
	}

	void SecureWipe(std::span<uint8_t> bytes) {
		if (bytes.empty()) {
			return;
		}
		WipeBytes(bytes.data(), bytes.size());
	}

	void SecureWipe(std::span<std::byte> bytes) {
		if (bytes.empty()) {
			return;
		}
		WipeBytes(reinterpret_cast<uint8_t *>(bytes.data()), bytes.size());
	}
}
