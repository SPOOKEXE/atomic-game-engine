#include <engine/core/SecureWipe.hpp>

#include <cryptopp/misc.h>

namespace engine::core {

	// `SecureWipeBuffer` rather than a loop or `memset`. Crypto++ writes it
	// through a volatile pointer, which is the standard-conforming way to say
	// "this store is observable" — the compiler is not permitted to prove it
	// dead and remove it, which it otherwise would and does.
	//
	// Crypto++ is already linked here for `Random`'s SHA-256, as `VENDOR` and
	// not `VENDOR_PUBLIC`, so no vendor type reaches the header above and no
	// module pays for it in compile time.
	void SecureWipe(std::span<uint8_t> bytes) {
		if (bytes.empty()) {
			return;
		}
		CryptoPP::SecureWipeBuffer(bytes.data(), bytes.size());
	}

	void SecureWipe(std::span<std::byte> bytes) {
		if (bytes.empty()) {
			return;
		}
		CryptoPP::SecureWipeBuffer(reinterpret_cast<uint8_t *>(bytes.data()), bytes.size());
	}
}
