#include <engine/core/Profiling.hpp>
#include <engine/core/SecureWipe.hpp>
#include <engine/net/quic/Crypto.hpp>

#include <array>
#include <cryptopp/aes.h>
#include <cryptopp/chacha.h>
#include <cryptopp/chachapoly.h>
#include <cryptopp/gcm.h>
#include <cryptopp/hmac.h>
#include <cryptopp/misc.h>
#include <cryptopp/sha.h>
#include <cstring>
#include <vector>

namespace engine::net::quic {

	namespace {
		// A readable address for a span that may be empty.
		//
		// Empty associated data and an empty payload are both ordinary in QUIC -
		// an ACK-only packet has no plaintext worth naming - and a null pointer
		// with a zero length trips Crypto++'s own assertions in a debug build.
		// The same guard `Cipher.cpp` keeps, for the same reason.
		const CryptoPP::byte *Address(std::span<const std::byte> bytes) {
			static const CryptoPP::byte NOTHING = 0;
			return bytes.empty() ? &NOTHING : reinterpret_cast<const CryptoPP::byte *>(bytes.data());
		}

		CryptoPP::byte *Address(std::span<std::byte> bytes) {
			static CryptoPP::byte NOTHING = 0;
			return bytes.empty() ? &NOTHING : reinterpret_cast<CryptoPP::byte *>(bytes.data());
		}

		// The salt every QUIC version 1 Initial key is extracted under. RFC 9001
		// §5.2, and it is published rather than secret - see `InitialKeys`.
		constexpr std::array<uint8_t, 20> INITIAL_SALT{0x38, 0x76, 0x2c, 0xf7, 0xf5, 0x59, 0x34,
													   0xb3, 0x4d, 0x17, 0x9a, 0xe6, 0xa4, 0xc8,
													   0x0c, 0xad, 0xcc, 0xbb, 0x7f, 0x0a};

		// The key and nonce the Retry integrity tag is computed under. RFC 9001
		// §5.8, version 1. Fixed and public: what the tag proves is that the
		// sender saw the client's first packet, not that it holds a secret.
		//@{
		constexpr std::array<uint8_t, 16> RETRY_KEY{
			0xbe, 0x0c, 0x69, 0x0b, 0x9f, 0x66, 0x57, 0x5a, 0x1d, 0x76, 0x6b, 0x54, 0xe3, 0x68, 0xc8, 0x4e
		};
		constexpr std::array<uint8_t, 12> RETRY_NONCE{
			0x46, 0x15, 0x99, 0xd3, 0x5d, 0x63, 0x2b, 0xf2, 0x23, 0x98, 0x25, 0xbb
		};
		//@}

		std::span<const std::byte> AsBytes(std::span<const uint8_t> raw) {
			return {reinterpret_cast<const std::byte *>(raw.data()), raw.size()};
		}
	}

	size_t KeyBytes(Aead aead) {
		switch (aead) {
		case Aead::Aes128Gcm:
			return 16;
		case Aead::Aes256Gcm:
		case Aead::ChaCha20Poly1305:
			return 32;
		}
		return 0;
	}

	size_t KeyBytes(HeaderCipher cipher) {
		switch (cipher) {
		case HeaderCipher::Aes128:
			return 16;
		case HeaderCipher::Aes256:
		case HeaderCipher::ChaCha20:
			return 32;
		}
		return 0;
	}

	const char *Describe(Aead aead) {
		switch (aead) {
		case Aead::Aes128Gcm:
			return "AEAD_AES_128_GCM";
		case Aead::Aes256Gcm:
			return "AEAD_AES_256_GCM";
		case Aead::ChaCha20Poly1305:
			return "AEAD_CHACHA20_POLY1305";
		}
		return "unknown";
	}

	const char *Describe(HeaderCipher cipher) {
		switch (cipher) {
		case HeaderCipher::Aes128:
			return "AES-128-ECB";
		case HeaderCipher::Aes256:
			return "AES-256-ECB";
		case HeaderCipher::ChaCha20:
			return "ChaCha20";
		}
		return "unknown";
	}

	std::array<std::byte, SECRET_BYTES>
	Extract(std::span<const std::byte> salt, std::span<const std::byte> material) {
		// RFC 5869 §2.2: an absent salt is a string of `HashLen` zeros, and an
		// empty span is how a caller spells absent. TLS 1.3's key schedule
		// depends on this - its first extract passes exactly that.
		std::array<std::byte, SECRET_BYTES> zeros{};
		const std::span<const std::byte> key = salt.empty() ? std::span<const std::byte>(zeros) : salt;

		std::array<std::byte, SECRET_BYTES> result{};
		CryptoPP::HMAC<CryptoPP::SHA256> hmac(
			reinterpret_cast<const CryptoPP::byte *>(key.data()), key.size()
		);
		hmac.Update(Address(material), material.size());
		hmac.Final(reinterpret_cast<CryptoPP::byte *>(result.data()));
		return result;
	}

	std::array<std::byte, SECRET_BYTES> Digest(std::span<const std::byte> data) {
		std::array<std::byte, SECRET_BYTES> result{};
		CryptoPP::SHA256 sha;
		sha.CalculateDigest(reinterpret_cast<CryptoPP::byte *>(result.data()), Address(data), data.size());
		return result;
	}

	std::array<std::byte, SECRET_BYTES>
	Hmac(std::span<const std::byte> key, std::span<const std::byte> data) {
		std::array<std::byte, SECRET_BYTES> result{};
		CryptoPP::HMAC<CryptoPP::SHA256> hmac(
			reinterpret_cast<const CryptoPP::byte *>(key.data()), key.size()
		);
		hmac.Update(Address(data), data.size());
		hmac.Final(reinterpret_cast<CryptoPP::byte *>(result.data()));
		return result;
	}

	bool SameBytes(std::span<const std::byte> left, std::span<const std::byte> right) {
		if (left.size() != right.size()) {
			return false;
		}
		return CryptoPP::VerifyBufsEqual(Address(left), Address(right), left.size());
	}

	bool ExpandLabel(std::span<const std::byte> secret, std::string_view label, std::span<std::byte> out) {
		return ExpandLabel(secret, label, {}, out);
	}

	bool ExpandLabel(
		std::span<const std::byte> secret,
		std::string_view label,
		std::span<const std::byte> context,
		std::span<std::byte> out
	) {
		// RFC 8446 §7.1. One expansion produces at most 255 hashes' worth, and
		// nothing here asks for more than 32 bytes - the check is here so that a
		// future caller that does gets a refusal rather than a truncation.
		if (out.size() > 255 * SECRET_BYTES) {
			return false;
		}
		// The prefix is added here rather than by the caller. A label somebody
		// spelled `tls13 quic key` themselves derives a different key on one side
		// of a connection and not the other, and the failure is "the handshake
		// does not complete" with nothing pointing at it.
		constexpr std::string_view PREFIX = "tls13 ";
		if (PREFIX.size() + label.size() > 255 || context.size() > 255) {
			return false;
		}

		std::vector<uint8_t> info;
		info.reserve(4 + PREFIX.size() + label.size() + context.size());
		info.push_back(static_cast<uint8_t>(out.size() >> 8));
		info.push_back(static_cast<uint8_t>(out.size() & 0xff));
		info.push_back(static_cast<uint8_t>(PREFIX.size() + label.size()));
		for (const char letter : PREFIX) {
			info.push_back(static_cast<uint8_t>(letter));
		}
		for (const char letter : label) {
			info.push_back(static_cast<uint8_t>(letter));
		}
		info.push_back(static_cast<uint8_t>(context.size()));
		for (const std::byte value : context) {
			info.push_back(static_cast<uint8_t>(value));
		}

		// RFC 5869 §2.3's expansion, written out rather than taken from
		// Crypto++'s `HKDF`, because that class only offers extract and expand
		// together and QUIC needs the two halves apart.
		CryptoPP::HMAC<CryptoPP::SHA256> hmac(
			reinterpret_cast<const CryptoPP::byte *>(secret.data()), secret.size()
		);
		std::array<CryptoPP::byte, SECRET_BYTES> block{};
		size_t written = 0;
		uint8_t counter = 0;
		while (written < out.size()) {
			counter++;
			hmac.Restart();
			if (counter > 1) {
				hmac.Update(block.data(), block.size());
			}
			hmac.Update(info.data(), info.size());
			hmac.Update(&counter, 1);
			hmac.Final(block.data());

			const size_t taking = std::min(block.size(), out.size() - written);
			std::memcpy(out.data() + written, block.data(), taking);
			written += taking;
		}
		core::SecureWipe(block);
		return true;
	}

	PacketKeys DeriveKeys(std::span<const std::byte> secret, Aead aead, HeaderCipher header) {
		PacketKeys keys;
		keys.KeyLength = KeyBytes(aead);
		keys.HeaderKeyLength = KeyBytes(header);
		ExpandLabel(secret, "quic key", {keys.Key.data(), keys.KeyLength});
		ExpandLabel(secret, "quic iv", keys.Iv);
		ExpandLabel(secret, "quic hp", {keys.HeaderKey.data(), keys.HeaderKeyLength});
		return keys;
	}

	std::array<std::byte, SECRET_BYTES> NextSecret(std::span<const std::byte> secret) {
		std::array<std::byte, SECRET_BYTES> next{};
		ExpandLabel(secret, "quic ku", next);
		return next;
	}

	InitialKeys DeriveInitialKeys(std::span<const std::byte> destination) {
		const auto initial = Extract(AsBytes(std::span<const uint8_t>(INITIAL_SALT)), destination);

		std::array<std::byte, SECRET_BYTES> client{};
		std::array<std::byte, SECRET_BYTES> server{};
		ExpandLabel(initial, "client in", client);
		ExpandLabel(initial, "server in", server);

		InitialKeys keys;
		keys.Client = DeriveKeys(client, Aead::Aes128Gcm, HeaderCipher::Aes128);
		keys.Server = DeriveKeys(server, Aead::Aes128Gcm, HeaderCipher::Aes128);
		core::SecureWipe(client);
		core::SecureWipe(server);
		return keys;
	}

	std::array<std::byte, NONCE_BYTES> NonceFor(std::span<const std::byte> iv, uint64_t packetNumber) {
		std::array<std::byte, NONCE_BYTES> nonce{};
		const size_t length = std::min(iv.size(), nonce.size());
		std::memcpy(nonce.data(), iv.data(), length);

		// RFC 9001 §5.3: the packet number is left-padded to the IV's length and
		// exclusive-ORed in, so the last eight bytes are the ones that move.
		for (size_t index = 0; index < sizeof(packetNumber) && index < length; index++) {
			const auto shift = static_cast<unsigned>(8 * index);
			nonce[length - 1 - index] ^= static_cast<std::byte>((packetNumber >> shift) & 0xff);
		}
		return nonce;
	}

	bool Seal(
		Aead aead,
		std::span<const std::byte> key,
		std::span<const std::byte> nonce,
		std::span<const std::byte> associatedData,
		std::span<const std::byte> plaintext,
		std::span<std::byte> out
	) {
		ENGINE_PROFILE_CAT("quic::Seal", core::ProfileCategory::Network);

		if (key.size() != KeyBytes(aead) || nonce.size() != NONCE_BYTES) {
			return false;
		}
		if (out.size() != plaintext.size() + TAG_BYTES) {
			return false;
		}

		auto *cipher = Address(out);
		auto *tag = cipher + plaintext.size();
		const auto run = [&](CryptoPP::AuthenticatedSymmetricCipher &engine) {
			engine.SetKeyWithIV(
				reinterpret_cast<const CryptoPP::byte *>(key.data()),
				key.size(),
				reinterpret_cast<const CryptoPP::byte *>(nonce.data()),
				nonce.size()
			);
			engine.EncryptAndAuthenticate(
				cipher,
				tag,
				TAG_BYTES,
				reinterpret_cast<const CryptoPP::byte *>(nonce.data()),
				static_cast<int>(nonce.size()),
				Address(associatedData),
				associatedData.size(),
				Address(plaintext),
				plaintext.size()
			);
		};

		switch (aead) {
		case Aead::Aes128Gcm:
		case Aead::Aes256Gcm: {
			CryptoPP::GCM<CryptoPP::AES>::Encryption engine;
			run(engine);
			return true;
		}
		case Aead::ChaCha20Poly1305: {
			CryptoPP::ChaCha20Poly1305::Encryption engine;
			run(engine);
			return true;
		}
		}
		return false;
	}

	bool Open(
		Aead aead,
		std::span<const std::byte> key,
		std::span<const std::byte> nonce,
		std::span<const std::byte> associatedData,
		std::span<const std::byte> ciphertext,
		std::span<std::byte> out
	) {
		ENGINE_PROFILE_CAT("quic::Open", core::ProfileCategory::Network);

		if (key.size() != KeyBytes(aead) || nonce.size() != NONCE_BYTES) {
			return false;
		}
		// A frame shorter than the tag is the one refusal allowed to be cheap:
		// there is nothing to compare in constant time against, and the length it
		// leaks is the length the attacker chose. `Cipher.hpp` measures the same
		// distinction.
		if (ciphertext.size() < TAG_BYTES || out.size() != ciphertext.size() - TAG_BYTES) {
			return false;
		}

		const size_t bodyLength = ciphertext.size() - TAG_BYTES;
		const auto *body = Address(ciphertext);
		const auto *tag = body + bodyLength;
		const auto run = [&](CryptoPP::AuthenticatedSymmetricCipher &engine) {
			engine.SetKeyWithIV(
				reinterpret_cast<const CryptoPP::byte *>(key.data()),
				key.size(),
				reinterpret_cast<const CryptoPP::byte *>(nonce.data()),
				nonce.size()
			);
			return engine.DecryptAndVerify(
				Address(out),
				tag,
				TAG_BYTES,
				reinterpret_cast<const CryptoPP::byte *>(nonce.data()),
				static_cast<int>(nonce.size()),
				Address(associatedData),
				associatedData.size(),
				body,
				bodyLength
			);
		};

		switch (aead) {
		case Aead::Aes128Gcm:
		case Aead::Aes256Gcm: {
			CryptoPP::GCM<CryptoPP::AES>::Decryption engine;
			return run(engine);
		}
		case Aead::ChaCha20Poly1305: {
			CryptoPP::ChaCha20Poly1305::Decryption engine;
			return run(engine);
		}
		}
		return false;
	}

	bool Mask(
		HeaderCipher cipher,
		std::span<const std::byte> key,
		std::span<const std::byte> sample,
		std::span<std::byte> out
	) {
		if (key.size() != KeyBytes(cipher) || sample.size() < SAMPLE_BYTES || out.size() != MASK_BYTES) {
			return false;
		}

		switch (cipher) {
		case HeaderCipher::Aes128:
		case HeaderCipher::Aes256: {
			// One ECB block over the sample, RFC 9001 §5.4.3. ECB is a keystream
			// generator here rather than an encryption mode - there is one block,
			// it is never chained, and the first five bytes of it are the mask.
			CryptoPP::AES::Encryption engine(
				reinterpret_cast<const CryptoPP::byte *>(key.data()), key.size()
			);
			std::array<CryptoPP::byte, 16> block{};
			engine.ProcessBlock(reinterpret_cast<const CryptoPP::byte *>(sample.data()), block.data());
			std::memcpy(out.data(), block.data(), MASK_BYTES);
			core::SecureWipe(block);
			return true;
		}
		case HeaderCipher::ChaCha20: {
			// RFC 9001 §5.4.4: the first four bytes of the sample are the block
			// counter, little-endian, and the remaining twelve are the nonce. The
			// mask is the keystream, so five zero bytes are encrypted.
			uint32_t counter = 0;
			for (size_t index = 0; index < 4; index++) {
				counter |= static_cast<uint32_t>(sample[index]) << (8 * index);
			}

			// `ChaChaTLS` rather than `ChaCha`: the original cipher takes a
			// 64-bit nonce with a 64-bit counter, and RFC 8439's IETF variant -
			// which is what RFC 9001 §5.4.4 names - splits it 96 and 32. The two
			// are different functions of the same key, so the wrong one produces
			// a mask that is wrong in a way nothing reports.
			CryptoPP::ChaChaTLS::Encryption engine;
			engine.SetKeyWithIV(
				reinterpret_cast<const CryptoPP::byte *>(key.data()),
				key.size(),
				reinterpret_cast<const CryptoPP::byte *>(sample.data()) + 4,
				12
			);
			engine.Seek(static_cast<CryptoPP::lword>(counter) * 64);

			std::array<CryptoPP::byte, MASK_BYTES> zeros{};
			engine.ProcessData(reinterpret_cast<CryptoPP::byte *>(out.data()), zeros.data(), MASK_BYTES);
			return true;
		}
		}
		return false;
	}

	bool RetryTag(
		std::span<const std::byte> original, std::span<const std::byte> retry, std::span<std::byte> out
	) {
		if (out.size() != TAG_BYTES || original.size() > 255) {
			return false;
		}

		// The Retry pseudo-packet, RFC 9001 §5.8: the original Destination
		// Connection ID with a length byte in front, then the Retry packet up to
		// but not including the tag. All of it is associated data - there is no
		// plaintext, because a Retry carries nothing to hide.
		std::vector<std::byte> pseudo;
		pseudo.reserve(1 + original.size() + retry.size());
		pseudo.push_back(static_cast<std::byte>(original.size()));
		pseudo.insert(pseudo.end(), original.begin(), original.end());
		pseudo.insert(pseudo.end(), retry.begin(), retry.end());

		return Seal(
			Aead::Aes128Gcm,
			AsBytes(std::span<const uint8_t>(RETRY_KEY)),
			AsBytes(std::span<const uint8_t>(RETRY_NONCE)),
			pseudo,
			{},
			out
		);
	}
}
