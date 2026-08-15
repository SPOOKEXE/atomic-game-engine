// The challenge, and the property that makes it worth having: nothing is
// remembered about a peer that has not answered one.
//
// Every deadline here is *stated* rather than waited for - the module rule that
// time is passed in is what lets a suite rotate a secret twice in consecutive
// lines and check that a cookie stopped being answerable.

#include <engine/core/Metrics.hpp>
#include <engine/net/Cookie.hpp>
#include <engine/net/Endpoint.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.net.cookie")
TEST_DEPENDS("engine.core.metrics")

using engine::net::Cookie;
using engine::net::CookieSettings;
using engine::net::Endpoint;

namespace {
	Cookie Begin(double rotateEverySeconds = 10.0) {
		CookieSettings settings;
		settings.RotateEverySeconds = rotateEverySeconds;
		auto issuer = Cookie::Begin(settings);
		REQUIRE(issuer.has_value());
		return std::move(*issuer);
	}

	Endpoint At(uint16_t port) {
		return Endpoint::LoopbackIPv4(port);
	}

	// Whatever a peer had to repeat unchanged. In the protocol this is the key
	// exchange message; here it only has to be some bytes.
	std::vector<std::byte> Evidence(uint8_t fill, size_t bytes = 32) {
		return std::vector<std::byte>(bytes, static_cast<std::byte>(fill));
	}
}

TEST_CASE("a cookie this end issued answers for the peer it was issued to", "[net][cookie]") {
	Cookie issuer = Begin();
	const auto evidence = Evidence(1);

	const auto cookie = issuer.Issue(0.0, At(7777), evidence);
	CHECK(issuer.Answers(0.0, At(7777), evidence, cookie));
}

TEST_CASE("the same question gets the same cookie", "[net][cookie]") {
	// Derived, not drawn. A cookie that differed per call would be one this end
	// could only check by remembering it, which is the whole thing this avoids.
	Cookie issuer = Begin();
	const auto evidence = Evidence(2);

	const auto first = issuer.Issue(1.0, At(7777), evidence);
	const auto second = issuer.Issue(2.0, At(7777), evidence);
	CHECK(first == second);
}

TEST_CASE("a cookie is bound to the address it was issued to", "[net][cookie]") {
	// The whole point. A peer that cannot receive at the address it wrote never
	// sees the cookie, so an answer proves return routability - and an answer
	// replayed from somewhere else proves nothing and is refused.
	Cookie issuer = Begin();
	const auto evidence = Evidence(3);

	const auto cookie = issuer.Issue(0.0, At(7777), evidence);
	CHECK_FALSE(issuer.Answers(0.0, At(7778), evidence, cookie));

	// A different address on the same port, too - the address bytes are in the
	// cookie and not only the port.
	const Endpoint elsewhere = Endpoint::FromIPv4({10, 0, 0, 1}, 7777);
	CHECK_FALSE(issuer.Answers(0.0, elsewhere, evidence, cookie));
}

TEST_CASE("a cookie is bound to what the peer sent with it", "[net][cookie]") {
	// So a relay cannot take a cookie issued for one key exchange message and
	// present it with its own.
	Cookie issuer = Begin();

	const auto cookie = issuer.Issue(0.0, At(7777), Evidence(4));
	CHECK_FALSE(issuer.Answers(0.0, At(7777), Evidence(5), cookie));
}

TEST_CASE("a cookie nobody issued does not answer", "[net][cookie]") {
	Cookie issuer = Begin();
	const auto evidence = Evidence(6);

	SECTION("all zeros") {
		const std::array<std::byte, Cookie::COOKIE_BYTES> zero{};
		CHECK_FALSE(issuer.Answers(0.0, At(7777), evidence, zero));
	}

	SECTION("a guess") {
		std::array<std::byte, Cookie::COOKIE_BYTES> guess{};
		guess.fill(std::byte{0xAB});
		CHECK_FALSE(issuer.Answers(0.0, At(7777), evidence, guess));
	}

	SECTION("one byte of a real one changed") {
		auto cookie = issuer.Issue(0.0, At(7777), evidence);
		cookie[17] = static_cast<std::byte>(static_cast<uint8_t>(cookie[17]) ^ 0x01u);
		CHECK_FALSE(issuer.Answers(0.0, At(7777), evidence, cookie));
	}
}

TEST_CASE("a cookie of the wrong length is refused", "[net][cookie]") {
	Cookie issuer = Begin();
	const auto evidence = Evidence(7);
	const auto cookie = issuer.Issue(0.0, At(7777), evidence);

	CHECK_FALSE(issuer.Answers(0.0, At(7777), evidence, {}));
	CHECK_FALSE(issuer.Answers(0.0, At(7777), evidence, std::span<const std::byte>(cookie).first(31)));

	std::vector<std::byte> longer(cookie.begin(), cookie.end());
	longer.push_back(std::byte{0});
	CHECK_FALSE(issuer.Answers(0.0, At(7777), evidence, longer));
}

TEST_CASE("two issuers do not answer each other's cookies", "[net][cookie]") {
	// Each draws its own secret. A shared or derived-from-nothing secret would
	// make a cookie from one server work on another, which is a forgery with a
	// second server as the oracle.
	Cookie first = Begin();
	Cookie second = Begin();
	const auto evidence = Evidence(8);

	const auto cookie = first.Issue(0.0, At(7777), evidence);
	CHECK_FALSE(second.Answers(0.0, At(7777), evidence, cookie));
}

TEST_CASE("a cookie survives one rotation and not two", "[net][cookie]") {
	// Stated, not waited for. The bound is what stops a cookie captured off the
	// wire being useful for ever, and "between one and two periods" is the
	// property rather than an implementation detail - the previous secret is
	// kept so a peer answering across a rotation is not refused for the
	// server's timing.
	Cookie issuer = Begin(10.0);
	const auto evidence = Evidence(9);

	const auto cookie = issuer.Issue(0.0, At(7777), evidence);
	CHECK(issuer.Answers(5.0, At(7777), evidence, cookie));

	// One period on: the secret it was issued under is now the previous one.
	CHECK(issuer.Answers(11.0, At(7777), evidence, cookie));

	// Two more: both secrets have moved past it.
	CHECK_FALSE(issuer.Answers(32.0, At(7777), evidence, cookie));
}

TEST_CASE("a long quiet spell does not keep an old cookie alive", "[net][cookie]") {
	// The rotation is driven by the calls, so a server that heard nothing for an
	// hour has not rotated. Shifting current into previous once at that point
	// would keep an hour-old cookie answerable - the bound has to be two
	// periods of *time*, not two calls.
	Cookie issuer = Begin(10.0);
	const auto evidence = Evidence(10);

	const auto cookie = issuer.Issue(0.0, At(7777), evidence);
	CHECK_FALSE(issuer.Answers(3600.0, At(7777), evidence, cookie));
}

TEST_CASE("nothing is remembered per peer", "[net][cookie]") {
	// **The property D00006 asks for, as far as this class can state it.** A
	// thousand peers are challenged and the first one's cookie still answers,
	// which no bounded table would manage - there is no table. What the number
	// of outstanding challenges costs this object is nothing at all.
	Cookie issuer = Begin();
	const auto evidence = Evidence(11);

	const auto first = issuer.Issue(0.0, At(1), evidence);
	for (uint16_t port = 2; port < 1002; port++) {
		issuer.Issue(0.0, At(port), evidence);
	}

	CHECK(issuer.Answers(0.0, At(1), evidence, first));
}

TEST_CASE("a moved-from issuer answers nothing", "[net][cookie]") {
	Cookie issuer = Begin();
	const auto evidence = Evidence(12);
	const auto cookie = issuer.Issue(0.0, At(7777), evidence);

	Cookie moved = std::move(issuer);
	CHECK(moved.Answers(0.0, At(7777), evidence, cookie));

	// The source kept neither secret, so it cannot answer for the cookie it
	// issued a moment ago.
	CHECK_FALSE(issuer.Answers(0.0, At(7777), evidence, cookie));
}

TEST_CASE("challenges are counted", "[net][cookie][metrics]") {
	using engine::core::Metrics;

	Cookie issuer = Begin();
	const auto evidence = Evidence(13);
	const auto cookie = issuer.Issue(0.0, At(7777), evidence);

	Metrics::Clear();
	CHECK(issuer.Answers(0.0, At(7777), evidence, cookie));
	CHECK_FALSE(issuer.Answers(0.0, At(7778), evidence, cookie));
	issuer.Issue(0.0, At(7779), evidence);

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

	// Issued climbing while answered stays flat is somebody probing the port,
	// and the honest reading is that each of those cost one HMAC rather than a
	// slot. That distinction is the whole reason these are separate counters.
	CHECK(total("net.cookie.issued") == 1.0);
	CHECK(total("net.cookie.answered") == 1.0);
	CHECK(total("net.cookie.refused") == 1.0);
}
