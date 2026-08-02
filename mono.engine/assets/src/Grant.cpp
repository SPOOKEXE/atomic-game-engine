
#include <engine/assets/Grant.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/core/SecureWipe.hpp>

#include <algorithm>
#include <cryptopp/hmac.h>
#include <cryptopp/misc.h>
#include <cryptopp/sha.h>
#include <cstring>
#include <utility>

namespace engine::assets {

	namespace {
		// A grant naming more bundles than this is refused. A session needs tens;
		// the 32-bit count could ask for four billion, and the gap between those
		// two numbers is the whole attack.
		constexpr uint32_t MAXIMUM_BUNDLES = 65'536;

		// The bytes the MAC is taken over: everything in the scope, in one
		// arrangement, with the magic and version inside rather than beside.
		//
		// Inside, because a MAC that does not cover the version lets a v2 token
		// be presented as a v1 one and read by the wrong parser. The same
		// reasoning puts the manifest's format version inside its signing
		// context.
		void WriteScope(core::ByteWriter &writer, const GrantScope &scope) {
			writer.WriteUInt32(Grant::MAGIC);
			writer.WriteUInt16(Grant::VERSION);
			writer.WriteUInt64(scope.Session);
			writer.WriteUInt64(scope.ExpiresAtSeconds);
			writer.WriteUInt64(scope.ByteBudget);
			writer.WriteUInt32(static_cast<uint32_t>(scope.Bundles.size()));
			for (const ContentHash &bundle : scope.Bundles) {
				writer.WriteRaw(bundle.Digest.data(), ContentHash::BYTES);
			}
		}

		std::array<uint8_t, Grant::MAC_BYTES>
		ComputeMac(const GrantScope &scope, std::span<const uint8_t> secret) {
			core::ByteWriter writer;
			WriteScope(writer, scope);
			const auto bytes = writer.Bytes();

			std::array<uint8_t, Grant::MAC_BYTES> mac{};
			CryptoPP::HMAC<CryptoPP::SHA256> hmac(secret.data(), secret.size());
			hmac.Update(reinterpret_cast<const CryptoPP::byte *>(bytes.data()), bytes.size());
			hmac.Final(mac.data());
			return mac;
		}
	}

	bool GrantScope::IsValid() const {
		// An empty bundle list is refused rather than read as "everything". A
		// grant that permits nothing and a grant that permits all of it must
		// never be one value — that mistake is only ever discovered by somebody
		// receiving content they should not have.
		return Session != 0 && !Bundles.empty() && Bundles.size() <= MAXIMUM_BUNDLES &&
			   ExpiresAtSeconds != 0 && ByteBudget != 0;
	}

	std::optional<GrantKey> GrantKey::FromSecret(std::span<const std::byte> secret) {
		if (secret.size() != BYTES) {
			return std::nullopt;
		}

		GrantKey key;
		std::memcpy(key.Secret.data(), secret.data(), BYTES);
		return key;
	}

	GrantKey::~GrantKey() {
		core::SecureWipe(Secret);
	}

	GrantKey::GrantKey(GrantKey &&other) noexcept : Secret(other.Secret) {
		core::SecureWipe(other.Secret);
	}

	GrantKey &GrantKey::operator=(GrantKey &&other) noexcept {
		if (this != &other) {
			core::SecureWipe(Secret);
			Secret = other.Secret;
			core::SecureWipe(other.Secret);
		}
		return *this;
	}

	std::optional<Grant> Grant::Issue(GrantScope scope, const GrantKey &key) {
		ENGINE_PROFILE("Grant::Issue");

		// Sorted and deduplicated before anything else, so one set of bundles
		// has one encoding and therefore one MAC. Without this, two servers
		// permitting the same content would issue tokens that do not compare
		// equal and could not be cached against each other.
		std::sort(scope.Bundles.begin(), scope.Bundles.end());
		scope.Bundles.erase(std::unique(scope.Bundles.begin(), scope.Bundles.end()), scope.Bundles.end());

		if (!scope.IsValid()) {
			return std::nullopt;
		}

		Grant grant;
		grant.Permitted = std::move(scope);
		grant.Mac = ComputeMac(grant.Permitted, key.Secret);

		core::Metrics::Count("assets.grant.issued", 1.0);
		return grant;
	}

	std::vector<std::byte> Grant::Encode() const {
		core::ByteWriter writer;
		WriteScope(writer, Permitted);
		writer.WriteRaw(Mac.data(), MAC_BYTES);

		const auto bytes = writer.Bytes();
		return std::vector<std::byte>(bytes.begin(), bytes.end());
	}

	std::optional<Grant>
	Grant::Open(std::span<const std::byte> token, const GrantKey &key, uint64_t nowSeconds) {
		ENGINE_PROFILE("Grant::Open");

		const auto refuse = [](const char *counter) -> std::optional<Grant> {
			core::Metrics::Count(counter, 1.0);
			return std::nullopt;
		};

		core::ByteReader reader(token);

		if (reader.ReadUInt32() != MAGIC || reader.ReadUInt16() != VERSION) {
			return refuse("assets.grant.malformed");
		}

		GrantScope scope;
		scope.Session = reader.ReadUInt64();
		scope.ExpiresAtSeconds = reader.ReadUInt64();
		scope.ByteBudget = reader.ReadUInt64();

		const uint32_t bundleCount = reader.ReadUInt32();
		if (bundleCount == 0 || bundleCount > MAXIMUM_BUNDLES) {
			return refuse("assets.grant.malformed");
		}
		scope.Bundles.reserve(bundleCount);

		for (uint32_t index = 0; index < bundleCount; ++index) {
			ContentHash bundle;
			if (!reader.ReadRaw(bundle.Digest.data(), ContentHash::BYTES)) {
				return refuse("assets.grant.malformed");
			}
			// Sorted order is part of the encoding, so a token that is not
			// sorted is not this format. Accepting one would give two byte
			// sequences for one grant.
			if (index > 0 && !(scope.Bundles.back() < bundle)) {
				return refuse("assets.grant.malformed");
			}
			scope.Bundles.push_back(bundle);
		}

		std::array<uint8_t, MAC_BYTES> presented{};
		if (!reader.ReadRaw(presented.data(), MAC_BYTES) || reader.Failed()) {
			return refuse("assets.grant.malformed");
		}

		// **The MAC first, and in constant time.** Nothing above this line has
		// been acted on — the fields were read into a local and no decision was
		// taken from them. Rejecting on an unverified expiry would leak, by
		// timing and by which counter moved, what an attacker's forged token
		// contained.
		const auto expected = ComputeMac(scope, key.Secret);
		if (!CryptoPP::VerifyBufsEqual(expected.data(), presented.data(), MAC_BYTES)) {
			return refuse("assets.grant.forged");
		}

		// Only now is the scope trustworthy enough to judge.
		if (!scope.IsValid()) {
			return refuse("assets.grant.malformed");
		}

		Grant grant;
		grant.Permitted = std::move(scope);
		grant.Mac = presented;

		if (grant.HasExpired(nowSeconds)) {
			// Counted apart from a forgery. An expired grant is an ordinary
			// event — a session that ran long — and a forged one is not. A
			// single counter for both would bury the alarm in the noise.
			return refuse("assets.grant.expired");
		}

		core::Metrics::Count("assets.grant.opened", 1.0);
		return grant;
	}

	bool Grant::Permits(const ContentHash &bundleRoot) const {
		return std::binary_search(Permitted.Bundles.begin(), Permitted.Bundles.end(), bundleRoot);
	}

	bool Grant::HasExpired(uint64_t nowSeconds) const {
		return nowSeconds >= Permitted.ExpiresAtSeconds;
	}
}
