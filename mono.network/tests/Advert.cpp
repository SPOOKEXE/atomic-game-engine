// The one record three reaches share, and the checking that makes it safe to
// read a stranger's datagram into it.
//
// Every refusal here is a refusal the module depends on: an advert arrives on
// an open UDP port from an address anybody can write, and the only thing
// standing between that and a listing is this decoder.

#include <engine/net/Endpoint.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <network/Advert.hpp>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

TEST_SUITE_ID("network.advert")
TEST_DEPENDS("network.sessionkey")
TEST_DEPENDS("engine.core.bytes")

using engine::net::Endpoint;
using network::Access;
using network::Advert;
using network::DecodedAdvert;
using network::Purpose;
using network::SessionId;
using network::SessionKey;

namespace {
	Advert Sample() {
		Advert advert;
		advert.Session = SessionId::Draw();
		advert.Use = Purpose::Game;
		advert.Admits = Access::Public;
		advert.Protocol = 7;
		advert.At = Endpoint::LoopbackIPv4(7777);
		advert.Name = "Declan's game";
		advert.Detail = "Baseplate";
		advert.Peers = 3;
		advert.PeerLimit = 16;
		return advert;
	}

	SessionKey Key(std::string_view words) {
		auto key = SessionKey::FromPassphrase(words);
		REQUIRE(key.has_value());
		return std::move(*key);
	}
}

TEST_CASE("a session id round-trips through its own text", "[network][advert]") {
	const SessionId drawn = SessionId::Draw();
	REQUIRE(drawn.IsValid());

	const std::string text = drawn.Text();
	CHECK(text.size() == SessionId::BYTES * 2);
	CHECK(SessionId::Parse(text) == drawn);

	// Two draws are two sessions. A counter would collide the moment two hosts
	// started at once, which is what a LAN party is.
	CHECK(SessionId::Draw() != drawn);

	// The null id is not a session and says so rather than printing as
	// thirty-two zeroes that look like one.
	CHECK_FALSE(SessionId{}.IsValid());
	CHECK(SessionId{}.Text() == "none");

	CHECK_FALSE(SessionId::Parse("").has_value());
	CHECK_FALSE(SessionId::Parse(std::string(31, 'a')).has_value());
	CHECK_FALSE(SessionId::Parse(std::string(33, 'a')).has_value());
	CHECK_FALSE(SessionId::Parse(std::string(31, 'a') + "z").has_value());
}

TEST_CASE("an advert survives its own encoding", "[network][advert]") {
	const Advert sent = Sample();
	const std::vector<std::byte> datagram = network::Encode(sent, nullptr);

	const std::optional<DecodedAdvert> decoded = network::Decode(datagram, {});
	REQUIRE(decoded.has_value());

	const Advert &got = decoded->Session;
	CHECK(got.Session == sent.Session);
	CHECK(got.Use == sent.Use);
	CHECK(got.Admits == sent.Admits);
	CHECK(got.Protocol == sent.Protocol);
	CHECK(got.At == sent.At);
	CHECK(got.Name == sent.Name);
	CHECK(got.Detail == sent.Detail);
	CHECK(got.Peers == sent.Peers);
	CHECK(got.PeerLimit == sent.PeerLimit);

	// Nothing was signed, so nothing is authenticated. A public session has
	// nothing to prove and a signature on it would be one nobody could check.
	CHECK_FALSE(decoded->Authenticated);
}

TEST_CASE("a v6 endpoint and an unbound one both survive", "[network][advert]") {
	Advert advert = Sample();
	advert.At = Endpoint::FromIPv6({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, 7777);

	std::optional<DecodedAdvert> decoded = network::Decode(network::Encode(advert, nullptr), {});
	REQUIRE(decoded.has_value());
	CHECK(decoded->Session.At == advert.At);

	// The wildcard case, which is what a host that bound 0.0.0.0 announces. It
	// has to survive, because `Listing::Dial` is what fixes it up and it needs
	// the port.
	advert.At = Endpoint::FromIPv4({0, 0, 0, 0}, 7777);
	decoded = network::Decode(network::Encode(advert, nullptr), {});
	REQUIRE(decoded.has_value());
	CHECK(decoded->Session.At == advert.At);
	CHECK(decoded->Session.At.Port == 7777);
}

TEST_CASE("a tagged advert authenticates only under the key that made it", "[network][advert]") {
	Advert advert = Sample();
	advert.Admits = Access::Private;

	const SessionKey key = Key("the passphrase");
	const std::vector<std::byte> datagram = network::Encode(advert, &key);

	std::vector<SessionKey> holding;
	holding.push_back(Key("the passphrase"));

	std::optional<DecodedAdvert> decoded = network::Decode(datagram, holding);
	REQUIRE(decoded.has_value());
	CHECK(decoded->Authenticated);
	CHECK(decoded->Session.Admits == Access::Private);

	// A browser holding the wrong key still sees the session — it has to, or
	// the person about to be given the key never learns it exists — and does
	// not believe it.
	std::vector<SessionKey> wrong;
	wrong.push_back(Key("not the passphrase"));
	decoded = network::Decode(datagram, wrong);
	REQUIRE(decoded.has_value());
	CHECK_FALSE(decoded->Authenticated);

	// And one holding no key at all.
	decoded = network::Decode(datagram, {});
	REQUIRE(decoded.has_value());
	CHECK_FALSE(decoded->Authenticated);

	// Several keys are tried, because somebody in two private sessions is in
	// two private sessions.
	std::vector<SessionKey> several;
	several.push_back(Key("some other session"));
	several.push_back(Key("the passphrase"));
	decoded = network::Decode(datagram, several);
	REQUIRE(decoded.has_value());
	CHECK(decoded->Authenticated);
}

TEST_CASE("a tag commits to every field of the advert", "[network][advert]") {
	Advert advert = Sample();
	advert.Admits = Access::Private;

	const SessionKey key = Key("the passphrase");
	std::vector<std::byte> datagram = network::Encode(advert, &key);

	std::vector<SessionKey> holding;
	holding.push_back(Key("the passphrase"));

	// Flip a byte in the middle of the body — the player count, as it happens.
	// Whatever it lands on, the tag stops verifying, which is the property that
	// makes an announcement worth trusting at all.
	for (size_t index = 8; index + SessionKey::TAG_BYTES < datagram.size(); index += 7) {
		std::vector<std::byte> altered = datagram;
		altered[index] = static_cast<std::byte>(static_cast<uint8_t>(altered[index]) ^ 0x01u);

		const std::optional<DecodedAdvert> decoded = network::Decode(altered, holding);
		// Either the frame no longer parses, or it parses and does not
		// authenticate. Both are refusals; neither is a listing somebody trusts.
		if (decoded.has_value()) {
			CHECK_FALSE(decoded->Authenticated);
		}
	}
}

TEST_CASE("hostile bytes are refused whole", "[network][advert]") {
	const std::vector<std::byte> good = network::Encode(Sample(), nullptr);

	// Empty, and everything shorter than the fixed fields.
	CHECK_FALSE(network::Decode({}, {}).has_value());
	for (size_t length = 1; length < good.size(); length += 5) {
		CHECK_FALSE(network::Decode(std::span(good).first(length), {}).has_value());
	}

	// A wrong magic, which is what every other datagram on the discovery port
	// is — including this module's own rendezvous messages.
	std::vector<std::byte> foreign = good;
	foreign[0] = static_cast<std::byte>(0x00);
	CHECK_FALSE(network::Decode(foreign, {}).has_value());

	// An unknown version. Refused rather than guessed at: a reader that
	// guesses mis-parses hostile bytes.
	std::vector<std::byte> future = good;
	future[4] = static_cast<std::byte>(0x99);
	CHECK_FALSE(network::Decode(future, {}).has_value());

	// A flag this build does not know. The fields after it may be laid out
	// differently, and reading them anyway is the guess the version check
	// exists to prevent.
	std::vector<std::byte> flagged = good;
	flagged[6] = static_cast<std::byte>(0x80);
	CHECK_FALSE(network::Decode(flagged, {}).has_value());

	// Trailing rubbish. Two datagrams that decode identically must not carry
	// different bytes, or a tag commits to less than it appears to.
	std::vector<std::byte> padded = good;
	padded.push_back(std::byte{0});
	CHECK_FALSE(network::Decode(padded, {}).has_value());

	// A purpose and an access outside their lists. The advert's `Use` byte sits
	// straight after the session id.
	std::vector<std::byte> unknownUse = good;
	unknownUse[7 + SessionId::BYTES] = static_cast<std::byte>(0x7F);
	CHECK_FALSE(network::Decode(unknownUse, {}).has_value());

	std::vector<std::byte> unknownAccess = good;
	unknownAccess[8 + SessionId::BYTES] = static_cast<std::byte>(0x7F);
	CHECK_FALSE(network::Decode(unknownAccess, {}).has_value());
}

TEST_CASE("an advert with no session is not one", "[network][advert]") {
	Advert nameless = Sample();
	nameless.Session = {};
	CHECK_FALSE(nameless.IsValid());

	// And it does not decode either, so a host that could not draw an id
	// announces nothing rather than announcing as everybody.
	CHECK_FALSE(network::Decode(network::Encode(nameless, nullptr), {}).has_value());
}

TEST_CASE("text is capped on the way out and refused on the way in", "[network][advert]") {
	Advert shouty = Sample();
	shouty.Name = std::string(Advert::MAXIMUM_TEXT_BYTES + 40, 'x');
	shouty.Detail = std::string(Advert::MAXIMUM_TEXT_BYTES + 40, 'y');

	// Not valid to hold, because a caller that built one that long has made a
	// mistake worth telling them about.
	CHECK_FALSE(shouty.IsValid());

	// Encoded anyway, cut at the cap: a name is cosmetic and dropping a whole
	// announcement over one is worse than a shortened name.
	const std::optional<DecodedAdvert> decoded = network::Decode(network::Encode(shouty, nullptr), {});
	REQUIRE(decoded.has_value());
	CHECK(decoded->Session.Name.size() == Advert::MAXIMUM_TEXT_BYTES);
	CHECK(decoded->Session.Detail.size() == Advert::MAXIMUM_TEXT_BYTES);
	CHECK(decoded->Session.IsValid());
}

TEST_CASE("a stated limit is what full means", "[network][advert]") {
	Advert advert = Sample();
	advert.Peers = 16;
	advert.PeerLimit = 16;
	CHECK(advert.IsFull());

	advert.Peers = 15;
	CHECK_FALSE(advert.IsFull());

	// No limit stated is not a limit of zero. A host that does not publish one
	// is not a host that is permanently full.
	advert.PeerLimit = 0;
	advert.Peers = 900;
	CHECK_FALSE(advert.IsFull());
}
