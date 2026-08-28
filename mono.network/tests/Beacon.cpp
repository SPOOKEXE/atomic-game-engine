// The host's half of LAN discovery, over a loopback network with real
// encoding.
//
// **No socket and no sleeping.** The loopback honours a broadcast, so what runs
// here is the same `Encode`, the same datagram and the same `Decode` a real
// subnet carries - the send-rate-is-ours argument, applied to discovery.
// Every interval is stated rather than waited for, which is what the module's
// "time is passed in" rule buys.

#include <engine/net/Transport.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <memory>
#include <network/Beacon.hpp>
#include <optional>
#include <utility>
#include <vector>

TEST_SUITE_ID("network.beacon")
TEST_DEPENDS("network.advert")
TEST_DEPENDS("engine.net.transport")

using engine::net::Transport;
using engine::net::TransportSettings;
using engine::net::TransportStatus;
using network::Access;
using network::Advert;
using network::Beacon;
using network::BeaconSettings;
using network::DecodedAdvert;
using network::SessionId;
using network::SessionKey;

namespace {
	std::vector<std::unique_ptr<Transport>> Subnet(size_t ends = 2) {
		TransportSettings settings;
		settings.Broadcast = true;
		return engine::net::MakeLoopbackTransport(ends, settings);
	}

	Advert Sample() {
		Advert advert;
		advert.Session = SessionId::Draw();
		advert.Name = "a session";
		advert.At = engine::net::Endpoint::LoopbackIPv4(7777);
		return advert;
	}

	// Takes the next announcement off an end, if one is waiting.
	std::optional<DecodedAdvert> Hear(Transport &end, std::span<const SessionKey> keys = {}) {
		std::vector<std::byte> datagram;
		if (end.Receive(datagram).Status != TransportStatus::Ok) {
			return std::nullopt;
		}
		return network::Decode(datagram, keys);
	}
}

TEST_CASE("a beacon announces on its interval and not between", "[network][beacon]") {
	std::vector<std::unique_ptr<Transport>> ends = Subnet();

	BeaconSettings settings;
	settings.AnnounceEverySeconds = 1.0;

	const Advert advert = Sample();
	Beacon beacon(*ends[0], advert, std::nullopt, settings);
	REQUIRE(beacon.Announcing());

	// The first pump announces. A host that waited a full interval to say it
	// exists is a host nobody can find for that interval.
	beacon.Pump(0.0);
	CHECK(beacon.Counters().Announcements == 1);

	const std::optional<DecodedAdvert> heard = Hear(*ends[1]);
	REQUIRE(heard.has_value());
	CHECK(heard->Session.Session == advert.Session);
	CHECK(heard->Session.Name == "a session");

	// Nothing between.
	beacon.Pump(0.5);
	beacon.Pump(0.99);
	CHECK(beacon.Counters().Announcements == 1);
	CHECK_FALSE(Hear(*ends[1]).has_value());

	beacon.Pump(1.0);
	CHECK(beacon.Counters().Announcements == 2);
	CHECK(Hear(*ends[1]).has_value());
}

TEST_CASE("a host never hears itself", "[network][beacon]") {
	std::vector<std::unique_ptr<Transport>> ends = Subnet(3);
	Beacon beacon(*ends[0], Sample());

	beacon.Pump(0.0);

	// Both of the others, and not the sender: a beacon that heard its own
	// announcement would list the session it is already hosting.
	CHECK(Hear(*ends[1]).has_value());
	CHECK(Hear(*ends[2]).has_value());
	CHECK_FALSE(Hear(*ends[0]).has_value());
}

TEST_CASE("a private session with no key announces nothing", "[network][beacon]") {
	std::vector<std::unique_ptr<Transport>> ends = Subnet();

	Advert advert = Sample();
	advert.Admits = Access::Private;

	Beacon beacon(*ends[0], advert);
	CHECK_FALSE(beacon.Announcing());

	beacon.Pump(0.0);
	beacon.Pump(2.0);
	CHECK(beacon.Counters().Announcements == 0);
	CHECK(beacon.Counters().Refused == 2);
	CHECK_FALSE(Hear(*ends[1]).has_value());

	// Refused and still rescheduled. A beacon that only rescheduled on success
	// would re-check a misconfigured advert every tick.
	CHECK(beacon.Counters().Refused == 2);
}

TEST_CASE("a private session with a key announces something a holder can verify", "[network][beacon]") {
	std::vector<std::unique_ptr<Transport>> ends = Subnet();

	Advert advert = Sample();
	advert.Admits = Access::Private;

	auto hostKey = SessionKey::FromPassphrase("let me in");
	REQUIRE(hostKey.has_value());
	Beacon beacon(*ends[0], advert, std::move(*hostKey));
	REQUIRE(beacon.Announcing());
	beacon.Pump(0.0);

	std::vector<SessionKey> holding;
	auto guestKey = SessionKey::FromPassphrase("let me in");
	REQUIRE(guestKey.has_value());
	holding.push_back(std::move(*guestKey));

	const std::optional<DecodedAdvert> heard = Hear(*ends[1], holding);
	REQUIRE(heard.has_value());
	CHECK(heard->Authenticated);
}

TEST_CASE("a malformed advert is refused rather than sent", "[network][beacon]") {
	std::vector<std::unique_ptr<Transport>> ends = Subnet();

	Advert nameless = Sample();
	nameless.Session = {};

	Beacon beacon(*ends[0], nameless);
	CHECK_FALSE(beacon.Announcing());
	beacon.Pump(0.0);
	CHECK(beacon.Counters().Refused == 1);
	CHECK(beacon.Counters().Announcements == 0);
}

TEST_CASE("what is announced can change without disturbing the schedule", "[network][beacon]") {
	std::vector<std::unique_ptr<Transport>> ends = Subnet();

	Advert advert = Sample();
	advert.Peers = 1;

	Beacon beacon(*ends[0], advert);
	beacon.Pump(0.0);
	REQUIRE(Hear(*ends[1])->Session.Peers == 1);

	// A player joins. The next announcement carries the new number; the
	// schedule does not move, so a host whose count changes every tick does not
	// broadcast every tick.
	advert.Peers = 2;
	beacon.SetAdvert(advert);
	beacon.Pump(0.5);
	CHECK_FALSE(Hear(*ends[1]).has_value());

	beacon.Pump(1.0);
	CHECK(Hear(*ends[1])->Session.Peers == 2);
}

TEST_CASE("a transport that will not broadcast is counted, not crashed into", "[network][beacon]") {
	// The default settings, which is what a caller who forgot the flag gets.
	// The send is refused by the transport exactly as an operating system
	// refuses it, and the beacon says so through a counter rather than looking
	// healthy.
	std::vector<std::unique_ptr<Transport>> ends = engine::net::MakeLoopbackTransport(2);

	Beacon beacon(*ends[0], Sample());
	CHECK(beacon.Announcing());

	beacon.Pump(0.0);
	CHECK(beacon.Counters().Announcements == 0);
	CHECK(beacon.Counters().Undelivered == 1);
	CHECK(beacon.Counters().Refused == 0);
}
