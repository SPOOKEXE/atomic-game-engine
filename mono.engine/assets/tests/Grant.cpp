#include <engine/assets/Grant.hpp>
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

TEST_SUITE_ID("engine.assets.grant")
TEST_DEPENDS("engine.assets.contenthash")
TEST_DEPENDS("engine.core.bytes")
TEST_DEPENDS("engine.core.framegraph")
TEST_DEPENDS("engine.core.metrics")

using engine::assets::ContentHash;
using engine::assets::Grant;
using engine::assets::GrantKey;
using engine::assets::GrantScope;
using engine::assets::Hasher;
using engine::core::FrameGraph;
using engine::core::Metrics;

namespace {
	// Fixed, so a failure reproduces. Not how a real shared secret is made.
	GrantKey Key(uint8_t fill = 1) {
		std::array<std::byte, GrantKey::BYTES> secret{};
		for (size_t index = 0; index < secret.size(); ++index) {
			secret[index] = static_cast<std::byte>(fill + index);
		}
		auto key = GrantKey::FromSecret(secret);
		REQUIRE(key.has_value());
		return std::move(*key);
	}

	ContentHash Bundle(std::string_view text) {
		return Hasher::Of(
			std::span<const std::byte>(reinterpret_cast<const std::byte *>(text.data()), text.size())
		);
	}

	constexpr uint64_t NOW = 1'000'000;
	constexpr uint64_t LATER = NOW + 300;

	GrantScope Scope() {
		GrantScope scope;
		scope.Session = 42;
		scope.Bundles = {Bundle("terrain"), Bundle("characters"), Bundle("audio")};
		scope.ExpiresAtSeconds = LATER;
		scope.ByteBudget = 64 * 1024 * 1024;
		return scope;
	}

	Grant Issue(const GrantKey &key) {
		auto grant = Grant::Issue(Scope(), key);
		REQUIRE(grant.has_value());
		return *grant;
	}
}

TEST_CASE("a grant opens against the key that issued it", "[assets][grant]") {
	const GrantKey key = Key();
	const Grant issued = Issue(key);

	const auto opened = Grant::Open(issued.Encode(), key, NOW);
	REQUIRE(opened.has_value());

	CHECK(opened->Scope().Session == 42);
	CHECK(opened->Scope().ExpiresAtSeconds == LATER);
	CHECK(opened->Scope().ByteBudget == 64 * 1024 * 1024);
	CHECK(opened->Scope().Bundles.size() == 3);
}

TEST_CASE("a grant permits exactly what it names", "[assets][grant]") {
	const GrantKey key = Key();
	const auto opened = Grant::Open(Issue(key).Encode(), key, NOW);
	REQUIRE(opened.has_value());

	CHECK(opened->Permits(Bundle("terrain")));
	CHECK(opened->Permits(Bundle("characters")));
	CHECK(opened->Permits(Bundle("audio")));

	// The origin's whole authorisation decision. Nothing else about the client
	// is knowable from here, and nothing else needs to be.
	CHECK_FALSE(opened->Permits(Bundle("someone else's content")));
	CHECK_FALSE(opened->Permits(ContentHash{}));
}

TEST_CASE("a grant does not open against a different key", "[assets][grant]") {
	const GrantKey mine = Key(1);
	const GrantKey theirs = Key(200);

	CHECK_FALSE(Grant::Open(Issue(mine).Encode(), theirs, NOW).has_value());
}

TEST_CASE("a tampered token does not open", "[assets][grant]") {
	const GrantKey key = Key();
	const auto token = Issue(key).Encode();

	// Every byte, one at a time. A MAC that covered only part of the token
	// would pass here for the bytes it missed, and the field it missed is
	// exactly the one an attacker would edit.
	for (size_t index = 0; index < token.size(); ++index) {
		auto edited = token;
		edited[index] = static_cast<std::byte>(static_cast<uint8_t>(edited[index]) ^ 0x01);
		INFO("byte " << index);
		CHECK_FALSE(Grant::Open(edited, key, NOW).has_value());
	}
}

TEST_CASE("widening the scope does not open", "[assets][grant]") {
	const GrantKey key = Key();
	const Grant issued = Issue(key);

	// The attack the MAC exists to stop: a client editing its own grant to name
	// content it was not given. It has to fail even though the result is a
	// perfectly well-formed token.
	GrantScope wider = issued.Scope();
	wider.Bundles.push_back(Bundle("someone else's content"));
	std::sort(wider.Bundles.begin(), wider.Bundles.end());

	const GrantKey attacker = Key(99);
	const auto forged = Grant::Issue(wider, attacker);
	REQUIRE(forged.has_value());

	CHECK_FALSE(Grant::Open(forged->Encode(), key, NOW).has_value());
}

TEST_CASE("an expired grant does not open", "[assets][grant]") {
	const GrantKey key = Key();
	const auto token = Issue(key).Encode();

	CHECK(Grant::Open(token, key, LATER - 1).has_value());
	CHECK_FALSE(Grant::Open(token, key, LATER).has_value());
	CHECK_FALSE(Grant::Open(token, key, LATER + 10'000).has_value());
}

TEST_CASE("expiry is asked again after opening", "[assets][grant]") {
	const GrantKey key = Key();
	const auto opened = Grant::Open(Issue(key).Encode(), key, NOW);
	REQUIRE(opened.has_value());

	// A long-lived stream outlives the check that admitted it, so the request
	// path asks again rather than trusting the admission.
	CHECK_FALSE(opened->HasExpired(NOW));
	CHECK(opened->HasExpired(LATER));
}

TEST_CASE("a malformed token does not open", "[assets][grant]") {
	const GrantKey key = Key();
	const auto token = Issue(key).Encode();

	CHECK_FALSE(Grant::Open({}, key, NOW).has_value());

	// Every truncation. The reader answers zero past the end rather than
	// throwing, so a short token that is not checked parses as a grant of empty
	// things.
	for (size_t length = 0; length < token.size(); ++length) {
		INFO("truncated to " << length);
		CHECK_FALSE(Grant::Open(std::span<const std::byte>(token).first(length), key, NOW).has_value());
	}

	auto wrongMagic = token;
	wrongMagic[0] = static_cast<std::byte>(0xFF);
	CHECK_FALSE(Grant::Open(wrongMagic, key, NOW).has_value());

	auto wrongVersion = token;
	wrongVersion[4] = static_cast<std::byte>(0x99);
	CHECK_FALSE(Grant::Open(wrongVersion, key, NOW).has_value());
}

TEST_CASE("an invalid scope is not issued", "[assets][grant]") {
	const GrantKey key = Key();

	GrantScope noBundles = Scope();
	noBundles.Bundles.clear();
	// Refused rather than read as "everything". A grant permitting nothing and
	// one permitting all of it must never be the same value.
	CHECK_FALSE(Grant::Issue(noBundles, key).has_value());

	GrantScope noSession = Scope();
	noSession.Session = 0;
	CHECK_FALSE(Grant::Issue(noSession, key).has_value());

	GrantScope noExpiry = Scope();
	noExpiry.ExpiresAtSeconds = 0;
	CHECK_FALSE(Grant::Issue(noExpiry, key).has_value());

	GrantScope noBudget = Scope();
	noBudget.ByteBudget = 0;
	CHECK_FALSE(Grant::Issue(noBudget, key).has_value());

	CHECK(Scope().IsValid());
	CHECK_FALSE(GrantScope{}.IsValid());
}

TEST_CASE("bundle order does not change the token", "[assets][grant]") {
	const GrantKey key = Key();

	GrantScope forward = Scope();
	GrantScope backward = Scope();
	std::reverse(backward.Bundles.begin(), backward.Bundles.end());

	const auto first = Grant::Issue(forward, key);
	const auto second = Grant::Issue(backward, key);
	REQUIRE(first.has_value());
	REQUIRE(second.has_value());

	// One set of bundles, one encoding, one MAC - otherwise two servers
	// permitting the same content issue tokens that cannot be compared or
	// cached against each other.
	CHECK(first->Encode() == second->Encode());
}

TEST_CASE("duplicate bundles collapse", "[assets][grant]") {
	const GrantKey key = Key();

	GrantScope repeated = Scope();
	repeated.Bundles.push_back(Bundle("terrain"));
	repeated.Bundles.push_back(Bundle("terrain"));

	const auto grant = Grant::Issue(repeated, key);
	REQUIRE(grant.has_value());
	CHECK(grant->Scope().Bundles.size() == 3);
	CHECK(grant->Encode() == Issue(key).Encode());
}

TEST_CASE("a token names no player and no path", "[assets][grant]") {
	const GrantKey key = Key();
	const auto token = Issue(key).Encode();

	// The origin learns a session number, some hashes, a time and a budget, and
	// nothing else.
	//
	// Checked structurally rather than by scanning for path-ish bytes. A token
	// is binary, so a stray '/' turns up inside a hash or a MAC by chance about
	// half the time and a scan for one tests the random number generator. The
	// size is the honest statement: header, exactly one hash per bundle, and the
	// MAC. There is no room left for a name.
	constexpr size_t HEADER = 4 + 2 + 8 + 8 + 8 + 4;
	const size_t expected = HEADER + Scope().Bundles.size() * ContentHash::BYTES + Grant::MAC_BYTES;
	CHECK(token.size() == expected);

	// And no name survives into it, which the size alone would not catch if a
	// field were ever added.
	const std::string text(reinterpret_cast<const char *>(token.data()), token.size());
	CHECK(text.find("terrain") == std::string::npos);
	CHECK(text.find("characters") == std::string::npos);
}

TEST_CASE("a wrong-length secret is refused", "[assets][grant]") {
	CHECK_FALSE(GrantKey::FromSecret({}).has_value());

	const std::vector<std::byte> shortSecret(GrantKey::BYTES - 1, std::byte{0x01});
	CHECK_FALSE(GrantKey::FromSecret(shortSecret).has_value());

	const std::vector<std::byte> longSecret(GrantKey::BYTES + 1, std::byte{0x01});
	CHECK_FALSE(GrantKey::FromSecret(longSecret).has_value());
}

TEST_CASE("a moved-from key still issues from its destination", "[assets][grant]") {
	GrantKey source = Key(5);
	GrantKey moved = std::move(source);

	const auto grant = Grant::Issue(Scope(), moved);
	REQUIRE(grant.has_value());
	CHECK(Grant::Open(grant->Encode(), moved, NOW).has_value());
}

TEST_CASE(
	"issuing and opening report themselves to the frame graph and the metrics sink",
	"[assets][grant][framegraph]"
) {
	const GrantKey key = Key();
	const GrantKey other = Key(77);
	const auto token = Issue(key).Encode();

	Metrics::Clear();
	FrameGraph::SetEnabled(true);
	FrameGraph::BeginFrame();
	(void)Grant::Issue(Scope(), key);
	CHECK(Grant::Open(token, key, NOW).has_value());
	CHECK_FALSE(Grant::Open(token, other, NOW).has_value());
	CHECK_FALSE(Grant::Open(token, key, LATER).has_value());
	FrameGraph::EndFrame();
	const std::vector<engine::core::FrameSpan> spans(FrameGraph::Spans().begin(), FrameGraph::Spans().end());
	FrameGraph::SetEnabled(false);

	const auto named = [&spans](std::string_view name) {
		return std::any_of(spans.begin(), spans.end(), [name](const auto &span) {
			return span.Name == name;
		});
	};

	CHECK(named("Grant::Issue"));
	CHECK(named("Grant::Open"));

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

	CHECK(total("assets.grant.issued") == 1.0);
	CHECK(total("assets.grant.opened") == 1.0);

	// Forged and expired are counted apart. An expired grant is an ordinary
	// event - a session that ran long - and a forged one is an alarm. One
	// counter for both would bury the alarm in the noise.
	CHECK(total("assets.grant.forged") == 1.0);
	CHECK(total("assets.grant.expired") == 1.0);
}
