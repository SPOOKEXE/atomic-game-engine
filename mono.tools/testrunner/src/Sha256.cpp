#include <cryptopp/filters.h>
#include <cryptopp/hex.h>
#include <cryptopp/sha.h>
#include <testrunner/Sha256.hpp>

namespace testrunner {

	// The whole of the implementation, kept out of the header so that Crypto++
	// stays private to this module. Sha256 is what the runner uses; that it is
	// Crypto++ underneath is not part of the interface.
	struct Sha256::Impl {
		CryptoPP::SHA256 Hash;

		// Crypto++'s Final() restarts the object, so a second call would digest
		// the empty message rather than repeat the answer. Hex() is documented
		// as idempotent and tests/Sha256.cpp checks it, so the result is kept
		// here and Final() is reached exactly once. Empty means "not yet
		// finalised" — a hex digest is always sixty-four characters.
		std::string Digest;
	};

	static_assert(
		CryptoPP::SHA256::DIGESTSIZE == Sha256::DIGEST_BYTES,
		"SHA-256 is a 32-byte digest; DIGEST_BYTES disagrees with Crypto++"
	);

	Sha256::Sha256() : Self(std::make_unique<Impl>()) {}

	// Out of line, all four of them: Impl is incomplete in the header, so the
	// compiler cannot generate these where the class is declared.
	Sha256::~Sha256() = default;
	Sha256::Sha256(Sha256 &&) noexcept = default;
	Sha256 &Sha256::operator=(Sha256 &&) noexcept = default;

	void Sha256::Update(const void *data, size_t bytes) {
		// CryptoPP::byte rather than std::byte or uint8_t. Crypto++ moved its
		// own byte into the namespace when C++17 introduced std::byte, and this
		// is the type its interface actually takes.
		Self->Hash.Update(static_cast<const CryptoPP::byte *>(data), bytes);
	}

	std::string Sha256::Hex() {
		if (Self->Digest.empty()) {
			CryptoPP::byte digest[CryptoPP::SHA256::DIGESTSIZE];
			Self->Hash.Final(digest);

			// false is `uppercase`. The default is uppercase hex, and every
			// cache key ever written by this tool is lowercase — getting this
			// wrong would invalidate every entry rather than fail a build.
			CryptoPP::ArraySource(
				digest,
				sizeof(digest),
				true,
				new CryptoPP::HexEncoder(new CryptoPP::StringSink(Self->Digest), false)
			);
		}
		return Self->Digest;
	}

	std::string Sha256::Of(std::string_view text) {
		Sha256 hash;
		hash.Update(text);
		return hash.Hex();
	}
}
