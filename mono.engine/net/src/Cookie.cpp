#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/core/SecureWipe.hpp>
#include <engine/net/Cookie.hpp>

#include <array>
#include <cryptopp/hmac.h>
#include <cryptopp/misc.h>
#include <cryptopp/osrng.h>
#include <cryptopp/sha.h>

namespace engine::net {

	namespace {
		// The address, in a form that means one thing.
		//
		// Sixteen address bytes, the port big-endian, then the family - every
		// byte of an `Endpoint` that distinguishes one peer from another, in a
		// fixed order. A layout that skipped the family would let a v4 address
		// and the v6 address whose first four bytes match it share a cookie.
		constexpr size_t ADDRESS_BYTES = 16 + 2 + 1;

		std::array<CryptoPP::byte, ADDRESS_BYTES> Flatten(const Endpoint &peer) {
			std::array<CryptoPP::byte, ADDRESS_BYTES> flat{};
			for (size_t index = 0; index < peer.Address.size(); index++) {
				flat[index] = static_cast<CryptoPP::byte>(peer.Address[index]);
			}
			flat[16] = static_cast<CryptoPP::byte>(peer.Port >> 8);
			flat[17] = static_cast<CryptoPP::byte>(peer.Port & 0xFFu);
			flat[18] = static_cast<CryptoPP::byte>(peer.Family);
			return flat;
		}

		// A fresh secret, or nothing when the operating system refused.
		bool DrawSecret(std::span<uint8_t> into) {
			try {
				CryptoPP::OS_GenerateRandomBlock(false, into.data(), into.size());
			} catch (const CryptoPP::Exception &) {
				core::SecureWipe(into);
				core::Metrics::Count("net.cookie.no_entropy", 1.0);
				return false;
			}
			return true;
		}
	}

	std::optional<Cookie> Cookie::Begin(const CookieSettings &settings) {
		Cookie issuer;
		if (!DrawSecret(issuer.Current) || !DrawSecret(issuer.Previous)) {
			// No entropy is a refusal to admit anybody. A predictable secret
			// here is a cookie every stranger can compute, and the server would
			// go on reporting healthy admissions while the challenge protected
			// nothing at all.
			issuer.Forget();
			return std::nullopt;
		}

		issuer.RotatePeriod = settings.RotateEverySeconds > 0.0 ? settings.RotateEverySeconds
																: CookieSettings{}.RotateEverySeconds;
		return issuer;
	}

	Cookie::~Cookie() {
		Forget();
	}

	Cookie::Cookie(Cookie &&other) noexcept
		: Current(other.Current), Previous(other.Previous), RotatePeriod(other.RotatePeriod),
		  RotateAt(other.RotateAt), Timed(other.Timed) {
		// The source keeps neither secret, for the reason `Handshake` does the
		// same: a moved-from object whose secret is still in its storage is a
		// copy of that secret nobody believes exists.
		other.Forget();
	}

	Cookie &Cookie::operator=(Cookie &&other) noexcept {
		if (this != &other) {
			Forget();
			Current = other.Current;
			Previous = other.Previous;
			RotatePeriod = other.RotatePeriod;
			RotateAt = other.RotateAt;
			Timed = other.Timed;
			other.Forget();
		}
		return *this;
	}

	void Cookie::Rotate(double nowSeconds) {
		if (!Timed) {
			Timed = true;
			RotateAt = nowSeconds + RotatePeriod;
			return;
		}

		if (nowSeconds < RotateAt) {
			return;
		}

		std::array<uint8_t, SECRET_BYTES> fresh{};
		if (!DrawSecret(fresh)) {
			// The one case where standing still is safer than acting. Zeroing
			// the secret because the operating system would not hand over
			// entropy turns every cookie into a forgeable one; keeping the
			// current secret for another period does not.
			core::SecureWipe(fresh);
			RotateAt = nowSeconds + RotatePeriod;
			return;
		}

		// **Two periods late means both secrets are stale, not one.** Shifting
		// current into previous after a long quiet spell would keep a cookie
		// issued an hour ago valid, which is exactly the replay the rotation
		// exists to close. The bound has to be two periods however long nothing
		// happened.
		if (nowSeconds >= RotateAt + RotatePeriod) {
			Previous = fresh;
		} else {
			Previous = Current;
		}

		Current = fresh;
		core::SecureWipe(fresh);
		RotateAt = nowSeconds + RotatePeriod;
	}

	std::array<std::byte, Cookie::COOKIE_BYTES> Cookie::Derive(
		const std::array<uint8_t, SECRET_BYTES> &secret,
		const Endpoint &peer,
		std::span<const std::byte> evidence
	) const {
		const std::array<CryptoPP::byte, ADDRESS_BYTES> address = Flatten(peer);

		CryptoPP::HMAC<CryptoPP::SHA256> mac(secret.data(), secret.size());
		mac.Update(address.data(), address.size());
		if (!evidence.empty()) {
			mac.Update(reinterpret_cast<const CryptoPP::byte *>(evidence.data()), evidence.size());
		}

		std::array<std::byte, COOKIE_BYTES> cookie{};
		static_assert(
			COOKIE_BYTES == CryptoPP::SHA256::DIGESTSIZE, "A cookie is the whole digest, never a prefix."
		);
		mac.Final(reinterpret_cast<CryptoPP::byte *>(cookie.data()));
		return cookie;
	}

	std::array<std::byte, Cookie::COOKIE_BYTES>
	Cookie::Issue(double nowSeconds, const Endpoint &peer, std::span<const std::byte> evidence) {
		ENGINE_PROFILE_CAT("Cookie::Issue", core::ProfileCategory::Network);

		Rotate(nowSeconds);
		core::Metrics::Count("net.cookie.issued", 1.0);
		return Derive(Current, peer, evidence);
	}

	bool Cookie::Answers(
		double nowSeconds,
		const Endpoint &peer,
		std::span<const std::byte> evidence,
		std::span<const std::byte> cookie
	) {
		ENGINE_PROFILE_CAT("Cookie::Answers", core::ProfileCategory::Network);

		if (cookie.size() != COOKIE_BYTES) {
			core::Metrics::Count("net.cookie.refused", 1.0);
			return false;
		}

		Rotate(nowSeconds);

		const auto *offered = reinterpret_cast<const CryptoPP::byte *>(cookie.data());
		const auto same = [offered](const std::array<std::byte, COOKIE_BYTES> &expected) {
			return static_cast<unsigned>(CryptoPP::VerifyBufsEqual(
				offered, reinterpret_cast<const CryptoPP::byte *>(expected.data()), COOKIE_BYTES
			));
		};

		// Both are compared, and neither short-circuits. `VerifyBufsEqual` is
		// constant time, and `|` rather than `||` keeps the pair that way too -
		// an early exit on the first match would leak which secret answered,
		// which is a bit of information about when the cookie was issued.
		const unsigned matched =
			same(Derive(Current, peer, evidence)) | same(Derive(Previous, peer, evidence));

		core::Metrics::Count(matched != 0 ? "net.cookie.answered" : "net.cookie.refused", 1.0);
		return matched != 0;
	}

	void Cookie::Forget() {
		core::SecureWipe(Current);
		core::SecureWipe(Previous);
	}
}
