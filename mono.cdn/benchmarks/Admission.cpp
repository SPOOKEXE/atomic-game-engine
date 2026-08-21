// The one decision the origin makes per request, at the rate requests arrive.
//
// **This is the only thing between a stranger and the content**, and it runs on
// every single fetch. An origin serving a launch is answering thousands of
// requests a second across as many connections, and every one of them presents
// a token that has to be verified before anything else happens - so this
// figure multiplies by the request rate and by nothing else. A gate that costs
// ten microseconds caps an origin at a hundred thousand requests a second on a
// core it would rather spend sending bytes.
//
// **The refusals are measured beside the acceptance, and that is the point of
// the file.** Three checks run in a fixed order - the MAC, then the expiry,
// then the scope - and a refusal exits at whichever one failed. That ordering
// is a security property: nothing in a token means anything until it has
// verified, so acting on an unverified field even to reject it is how a parser
// becomes the attack surface the MAC was meant to remove. It is also a
// performance property, and the two agree here rather than trading off. A
// forged token is refused at the first check without the scope ever being
// walked, which is what stops a flood of garbage costing more per request than
// real traffic does.
//
// **So the row to watch is the forged one.** If it is ever slower than the
// accepted row, something has started doing work before verifying, and an
// attacker who cannot obtain a grant can still make the origin do the expensive
// part. That is a denial of service with no credential required, and this is
// the only place it would show as a number rather than as an outage.
//
// A refusal says nothing to the caller about which check failed, because a
// reason returned to a client is an oracle. The rows here are separate for the
// operator's benefit, which is the same split the counters make.

#include <engine/assets/ContentHash.hpp>
#include <engine/assets/Grant.hpp>
#include <engine/testing/Bench.hpp>

#include <array>
#include <cdn/Gate.hpp>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

TEST_SUITE_ID("cdn.bench.admission")

using cdn::Gate;
using engine::assets::ContentHash;
using engine::assets::Grant;
using engine::assets::GrantKey;
using engine::assets::GrantScope;
using engine::testing::Consume;

namespace admission_bench {
	// Requests per sample. An origin under load answers this many in well under
	// a second, so it is a realistic burst rather than a synthetic one.
	constexpr size_t REQUESTS = 100'000;

	// The clock the server and the origin share. Absolute, because a relative
	// window would have to be resolved against a clock somewhere and the two
	// ends would resolve it against different ones.
	constexpr uint64_t NOW = 1'000'000;
	constexpr uint64_t EXPIRY = NOW + 300;

	// A distinct bundle root, derived rather than hashed - what the gate does
	// with a root is compare it, which costs the same whatever produced it.
	ContentHash BundleOf(uint64_t index) {
		ContentHash root;
		for (size_t byte = 0; byte < root.Digest.size(); byte++) {
			root.Digest[byte] = static_cast<uint8_t>((index >> (byte % 8 * 8)) ^ (byte * 17));
		}
		return root;
	}

	// The secret the server and the origin both hold. One function, so the two
	// keys below cannot drift apart into a suite where nothing ever verifies.
	std::array<std::byte, GrantKey::BYTES> Secret() {
		std::array<std::byte, GrantKey::BYTES> secret{};
		for (size_t index = 0; index < secret.size(); index++) {
			secret[index] = static_cast<std::byte>(index * 7 + 1);
		}
		return secret;
	}

	// The server's copy, used to issue.
	const GrantKey &Shared() {
		static const GrantKey key = std::move(*GrantKey::FromSecret(Secret()));
		return key;
	}

	// A token permitting `bundles` bundles, expiring at `expiry`.
	//
	// The bundle count is a parameter because the scope check walks the list,
	// and a session entitled to two bundles and one entitled to a thousand are
	// the same request with very different tails.
	std::vector<std::byte> TokenFor(size_t bundles, uint64_t expiry) {
		GrantScope scope;
		scope.Session = 7;
		scope.ExpiresAtSeconds = expiry;
		scope.ByteBudget = 1024ull * 1024 * 1024;
		scope.Bundles.reserve(bundles);
		for (uint64_t index = 0; index < bundles; index++) {
			scope.Bundles.push_back(BundleOf(index));
		}
		return Grant::Issue(scope, Shared())->Encode();
	}

	// The origin's copy, which is a second key from the same secret because a
	// `GrantKey` is move-only and the gate takes ownership of one.
	const Gate &Origin() {
		static const Gate gate(std::move(*GrantKey::FromSecret(Secret())));
		return gate;
	}

	// Runs one admission decision `REQUESTS` times and reports how many were
	// admitted, so neither branch can be folded away.
	size_t Ask(std::span<const std::byte> token, const ContentHash &bundle, uint64_t nowSeconds) {
		const Gate &gate = Origin();
		size_t admitted = 0;
		for (size_t request = 0; request < REQUESTS; request++) {
			admitted += gate.Admits(token, bundle, nowSeconds) ? 1 : 0;
		}
		return admitted;
	}
}

using namespace admission_bench;

// --- the accepted path --------------------------------------------------------

BENCH("Admits · 100k accepted requests, 2 bundles in scope", REQUESTS) {
	static const std::vector<std::byte> token = TokenFor(2, EXPIRY);
	Consume(Ask(token, BundleOf(1), NOW));
}

BENCH("Admits · 100k accepted requests, 64 bundles in scope", REQUESTS) {
	// A session that has loaded a lot of the game. The difference from the row
	// above is the scope walk and nothing else, because the MAC covers the same
	// token whatever is in it.
	static const std::vector<std::byte> token = TokenFor(64, EXPIRY);
	Consume(Ask(token, BundleOf(63), NOW));
}

BENCH("Admits · 100k accepted requests, 1024 bundles in scope", REQUESTS) {
	// The tail, and what these three rows together say: **the cost of an
	// admission tracks the size of the token, not the work of deciding.** A
	// bundle is thirty-two bytes of root and the MAC covers every one of them,
	// so a thousand-bundle scope is a thirty-kilobyte token and verifying it
	// costs what hashing thirty kilobytes costs. The scope lookup does not
	// appear above that at any of these sizes.
	//
	// Which makes this a budget on the *server* rather than a thing to optimise
	// here: a session granted everything it might ever want pays for that on
	// every request it makes. Narrower grants renewed more often are cheaper
	// per request, and this is the row that prices the trade.
	static const std::vector<std::byte> token = TokenFor(1024, EXPIRY);
	Consume(Ask(token, BundleOf(1023), NOW));
}

// --- the refusals -------------------------------------------------------------

BENCH("Admits · 100k forged tokens", REQUESTS) {
	// **The row that matters most and the one nobody would think to write.**
	// A flood of garbage is the cheapest attack there is, so a forged token
	// must never cost *more* than a real one. It costs the same, to within
	// noise, against the sixty-four-bundle accepted row - which is the right
	// answer rather than a disappointing one: a forgery is only detectable by
	// verifying, and verifying is the whole cost. What would be wrong is this
	// row coming out slower, which is what it would do if anything read a field
	// before the MAC had cleared it.
	static const std::vector<std::byte> forged = [] {
		std::vector<std::byte> token = TokenFor(64, EXPIRY);
		token.back() = static_cast<std::byte>(std::to_integer<uint8_t>(token.back()) ^ 0xFF);
		return token;
	}();
	Consume(Ask(forged, BundleOf(1), NOW));
}

BENCH("Admits · 100k tokens that are not tokens at all", REQUESTS) {
	// A wrong blob fails at its first four bytes, which is what the magic is
	// for. This is the shape a scanner sends and the cheapest refusal there is.
	static const std::vector<std::byte> rubbish(256, std::byte{0xA5});
	Consume(Ask(rubbish, BundleOf(1), NOW));
}

BENCH("Admits · 100k expired tokens", REQUESTS) {
	// Past the MAC, refused at the second check. The one refusal that is a
	// legitimate client rather than an attacker, and therefore the one that
	// arrives in bursts when a lot of sessions renew at once.
	static const std::vector<std::byte> token = TokenFor(64, NOW - 1);
	Consume(Ask(token, BundleOf(1), NOW));
}

BENCH("Admits · 100k requests for a bundle out of scope", REQUESTS) {
	// Past the MAC and past the expiry, refused at the third. The full cost of
	// a decision, and therefore the ceiling every other row here sits under.
	static const std::vector<std::byte> token = TokenFor(64, EXPIRY);
	Consume(Ask(token, BundleOf(9999), NOW));
}

// --- issuing ------------------------------------------------------------------
//
// The server's half rather than the origin's, and it runs once per session
// rather than once per request - but it is the same key and the same MAC, so
// the two figures belong beside each other.

BENCH("Grant::Issue · 10k grants over 64 bundles", 10'000) {
	for (size_t issue = 0; issue < 10'000; issue++) {
		GrantScope scope;
		scope.Session = issue;
		scope.ExpiresAtSeconds = EXPIRY;
		scope.ByteBudget = 1024ull * 1024;
		scope.Bundles.reserve(64);
		for (uint64_t index = 0; index < 64; index++) {
			scope.Bundles.push_back(BundleOf(index));
		}
		Consume(Grant::Issue(scope, Shared()).has_value());
	}
}

BENCH("Grant::Encode · 10k tokens over 64 bundles", 10'000) {
	static const Grant grant = [] {
		GrantScope scope;
		scope.Session = 7;
		scope.ExpiresAtSeconds = EXPIRY;
		scope.ByteBudget = 1024ull * 1024;
		scope.Bundles.reserve(64);
		for (uint64_t index = 0; index < 64; index++) {
			scope.Bundles.push_back(BundleOf(index));
		}
		return std::move(*Grant::Issue(scope, Shared()));
	}();
	for (size_t call = 0; call < 10'000; call++) {
		Consume(grant.Encode().size());
	}
}
