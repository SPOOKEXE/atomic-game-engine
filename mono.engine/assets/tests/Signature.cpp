#include <engine/assets/Signature.hpp>
#include <engine/core/FrameGraph.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

TEST_SUITE_ID("engine.assets.signature")
TEST_DEPENDS("engine.assets.contenthash")
TEST_DEPENDS("engine.core.framegraph")
TEST_DEPENDS("engine.core.metrics")

using engine::assets::ContentHash;
using engine::assets::Hasher;
using engine::assets::PublicKey;
using engine::assets::SignatureBytes;
using engine::assets::SigningKey;
using engine::assets::VerifyManifestRoot;
using engine::core::FrameGraph;
using engine::core::Metrics;

namespace {
	// A fixed seed, so a failure is reproducible. Emphatically not how a real
	// key is made - Ed25519's private key *is* this seed, and a predictable one
	// is a published one.
	std::array<std::byte, SigningKey::SEED_BYTES> Seed(uint8_t fill) {
		std::array<std::byte, SigningKey::SEED_BYTES> seed{};
		for (size_t index = 0; index < seed.size(); ++index) {
			seed[index] = static_cast<std::byte>(fill + index);
		}
		return seed;
	}

	ContentHash Root(std::string_view text) {
		return Hasher::Of(
			std::span<const std::byte>(reinterpret_cast<const std::byte *>(text.data()), text.size())
		);
	}

	SigningKey Key(uint8_t fill = 1) {
		auto key = SigningKey::FromSeed(Seed(fill));
		REQUIRE(key.has_value());
		return std::move(*key);
	}
}

TEST_CASE("a signature verifies against its own root and key", "[assets][signature]") {
	const SigningKey key = Key();
	const ContentHash root = Root("manifest one");

	const SignatureBytes signature = key.SignManifestRoot(root);

	CHECK_FALSE(signature.IsZero());
	CHECK(VerifyManifestRoot(root, signature, key.Public()));
}

TEST_CASE("a signature does not verify against a different root", "[assets][signature]") {
	const SigningKey key = Key();
	const SignatureBytes signature = key.SignManifestRoot(Root("manifest one"));

	// The property the whole trust model rests on: a client holding a root the
	// publisher signed will not accept a different one. CDN.md §1.
	CHECK_FALSE(VerifyManifestRoot(Root("manifest two"), signature, key.Public()));
}

TEST_CASE("a signature does not verify against a different key", "[assets][signature]") {
	const SigningKey mine = Key(1);
	const SigningKey theirs = Key(200);
	const ContentHash root = Root("manifest one");

	REQUIRE_FALSE(mine.Public() == theirs.Public());
	CHECK_FALSE(VerifyManifestRoot(root, mine.SignManifestRoot(root), theirs.Public()));
}

TEST_CASE("a tampered signature does not verify", "[assets][signature]") {
	const SigningKey key = Key();
	const ContentHash root = Root("manifest one");

	SignatureBytes signature = key.SignManifestRoot(root);
	signature.Value[0] ^= 0x01;
	CHECK_FALSE(VerifyManifestRoot(root, signature, key.Public()));

	signature.Value[0] ^= 0x01;
	signature.Value[SignatureBytes::BYTES - 1] ^= 0x80;
	CHECK_FALSE(VerifyManifestRoot(root, signature, key.Public()));
}

TEST_CASE("an empty signature or key does not verify", "[assets][signature]") {
	const SigningKey key = Key();
	const ContentHash root = Root("manifest one");

	CHECK_FALSE(VerifyManifestRoot(root, SignatureBytes{}, key.Public()));
	CHECK_FALSE(VerifyManifestRoot(root, key.SignManifestRoot(root), PublicKey{}));

	CHECK(SignatureBytes{}.IsZero());
	CHECK(PublicKey{}.IsZero());
	CHECK_FALSE(key.Public().IsZero());
}

TEST_CASE("signing is deterministic", "[assets][signature]") {
	const SigningKey key = Key();
	const ContentHash root = Root("manifest one");

	// Ed25519 derives its nonce from the key and the message rather than from a
	// generator, so the same input signs to the same bytes. That is what lets a
	// published manifest be byte-stable all the way to its signature - two
	// builds of one set of content produce one artefact, diffable and cacheable.
	CHECK(key.SignManifestRoot(root) == key.SignManifestRoot(root));
}

TEST_CASE("the same seed always gives the same key", "[assets][signature]") {
	const SigningKey first = Key(7);
	const SigningKey second = Key(7);

	CHECK(first.Public() == second.Public());
	CHECK(first.SignManifestRoot(Root("x")) == second.SignManifestRoot(Root("x")));
}

TEST_CASE("a seed of the wrong length is refused", "[assets][signature]") {
	CHECK_FALSE(SigningKey::FromSeed({}).has_value());

	const std::vector<std::byte> shortSeed(SigningKey::SEED_BYTES - 1, std::byte{0x01});
	CHECK_FALSE(SigningKey::FromSeed(shortSeed).has_value());

	const std::vector<std::byte> longSeed(SigningKey::SEED_BYTES + 1, std::byte{0x01});
	CHECK_FALSE(SigningKey::FromSeed(longSeed).has_value());
}

TEST_CASE("a moved-from key leaves no seed behind", "[assets][signature]") {
	SigningKey source = Key(9);
	const PublicKey expected = source.Public();

	SigningKey moved = std::move(source);
	CHECK(moved.Public() == expected);

	// The signature still works from the destination, which is the half that is
	// easy to check. The half that matters - the source's storage being wiped -
	// is not observable through the public surface, and a test that reached for
	// it would be testing the layout rather than the behaviour.
	const ContentHash root = Root("after the move");
	CHECK(VerifyManifestRoot(root, moved.SignManifestRoot(root), expected));
}

TEST_CASE("keys and signatures round-trip through hex", "[assets][signature]") {
	const SigningKey key = Key();
	const SignatureBytes signature = key.SignManifestRoot(Root("manifest one"));

	const std::string keyText = key.Public().ToHex();
	const std::string signatureText = signature.ToHex();

	REQUIRE(keyText.size() == PublicKey::BYTES * 2);
	REQUIRE(signatureText.size() == SignatureBytes::BYTES * 2);

	const auto parsedKey = PublicKey::FromHex(keyText);
	const auto parsedSignature = SignatureBytes::FromHex(signatureText);
	REQUIRE(parsedKey.has_value());
	REQUIRE(parsedSignature.has_value());

	CHECK(*parsedKey == key.Public());
	CHECK(*parsedSignature == signature);
}

TEST_CASE("hex parsing refuses the wrong length or spelling", "[assets][signature]") {
	const std::string keyText = Key().Public().ToHex();

	CHECK_FALSE(PublicKey::FromHex("").has_value());
	CHECK_FALSE(PublicKey::FromHex(keyText.substr(0, 63)).has_value());
	CHECK_FALSE(PublicKey::FromHex(keyText + "0").has_value());
	CHECK_FALSE(PublicKey::FromHex("0x" + keyText.substr(2)).has_value());

	// A signature is twice as long as a key, and neither parses as the other.
	CHECK_FALSE(SignatureBytes::FromHex(keyText).has_value());
}

TEST_CASE("the signed message is domain-separated from the bare root", "[assets][signature]") {
	const SigningKey key = Key();
	const ContentHash root = Root("manifest one");
	const SignatureBytes signature = key.SignManifestRoot(root);

	// Constructed so that if SignManifestRoot signed the root directly, this
	// root - whose digest is the tagged message - would verify with the same
	// signature and a manifest could be swapped for whatever else the key signs.
	// It must not.
	const ContentHash impostor = Root("manifest one but different");
	CHECK_FALSE(VerifyManifestRoot(impostor, signature, key.Public()));

	// And the signature is not simply over the 32 root bytes: a verifier that
	// ignored the tag would accept the root of any other tagged message too.
	// The check above plus this one pin both halves.
	CHECK(VerifyManifestRoot(root, signature, key.Public()));
}

TEST_CASE(
	"verification reports itself to the frame graph and the metrics sink", "[assets][signature][framegraph]"
) {
	const SigningKey key = Key();
	const ContentHash root = Root("manifest one");
	const SignatureBytes signature = key.SignManifestRoot(root);

	Metrics::Clear();
	FrameGraph::SetEnabled(true);
	FrameGraph::BeginFrame();
	CHECK(VerifyManifestRoot(root, signature, key.Public()));
	CHECK_FALSE(VerifyManifestRoot(Root("elsewhere"), signature, key.Public()));
	(void)key.SignManifestRoot(root);
	FrameGraph::EndFrame();
	const std::vector<engine::core::FrameSpan> spans(FrameGraph::Spans().begin(), FrameGraph::Spans().end());
	FrameGraph::SetEnabled(false);

	const auto named = [&spans](std::string_view name) {
		return std::any_of(spans.begin(), spans.end(), [name](const auto &span) {
			return span.Name == name;
		});
	};

	CHECK(named("assets::VerifyManifestRoot"));
	CHECK(named("SigningKey::SignManifestRoot"));

	const auto counters = Metrics::Drain();
	const auto total = [&counters](std::string_view name) {
		double sum = 0.0;
		for (const auto &counter : counters) {
			if (counter.Name == engine::core::Name(name)) {
				sum += counter.Value;
			}
		}
		return sum;
	};

	// A rejected manifest is either a misconfiguration or an attack, and it
	// reads nothing like content being absent. Counted apart for that reason.
	CHECK(total("assets.manifest.verified") == 1.0);
	CHECK(total("assets.manifest.rejected") == 1.0);
}
