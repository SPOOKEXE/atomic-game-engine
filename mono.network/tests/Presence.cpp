// The composition, and the property it exists for: no caller has an "is
// discovery enabled" branch.
//
// **This is the one suite here that opens real sockets**, because it is the one
// class whose whole job is opening them. It stays off the well-known port -
// two suites running at once on one machine must not fight over 47600, and a
// developer with the studio open must not fail the build - so what it checks is
// the composition rather than a live subnet. `network.beacon` and
// `network.directory` cover what crosses the wire, over a loopback, with real
// encoding.

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <network/Presence.hpp>
#include <optional>
#include <string>
#include <utility>

TEST_SUITE_ID("network.presence")
TEST_DEPENDS("network.beacon")
TEST_DEPENDS("network.directory")
TEST_DEPENDS("network.rendezvous")

using network::Advert;
using network::Presence;
using network::PresenceFault;
using network::PresenceSettings;
using network::Purpose;
using network::Reach;
using network::ReachState;
using network::SessionId;

namespace {
	// Away from the well-known port, so two of these can run at once and
	// neither disturbs whatever is on 47600.
	constexpr uint16_t QUIET_PORT = 47690;

	Advert Sample() {
		Advert advert;
		advert.Session = SessionId::Draw();
		advert.Name = "a session";
		advert.At = engine::net::Endpoint::FromIPv4({0, 0, 0, 0}, 7777);
		return advert;
	}
}

TEST_CASE("a presence that was asked for nothing opens nothing and still works", "[network][presence]") {
	PresenceSettings settings;
	settings.Announce = false;
	settings.Discover = false;

	const std::unique_ptr<Presence> presence = Presence::Open(settings);
	REQUIRE(presence != nullptr);

	CHECK_FALSE(presence->Announcing());
	CHECK_FALSE(presence->Discovering());
	CHECK_FALSE(presence->Rendezvousing());
	CHECK(presence->Fault() == PresenceFault::None);

	// The directory exists whether or not anything feeds it, which is the
	// property: a program that only ever types an address in walks the same
	// container as one that browses.
	const Advert typed = Sample();
	CHECK(presence->Seen().Offer(typed, Reach::Remote, {}, 0.0));
	CHECK(presence->Seen().Listings().size() == 1);

	// And pumping does no harm with nothing behind it.
	presence->Pump(0.0);
	presence->Browse(0.0);
	presence->Withdraw(0.0);
	CHECK(presence->Reaching() == ReachState::Idle);
	CHECK_FALSE(presence->Reached().IsValid());
	CHECK_FALSE(presence->Reflexive().IsValid());
	CHECK_FALSE(presence->Reach(typed.Session, std::nullopt, 0.0));
}

TEST_CASE("announcing opens an ephemeral socket rather than the well-known one", "[network][presence]") {
	PresenceSettings settings;
	settings.Announce = true;
	settings.DiscoveryPort = QUIET_PORT;
	settings.Protocol = 12;
	settings.Use = Purpose::Studio;

	const std::unique_ptr<Presence> presence = Presence::Open(settings, Sample());
	REQUIRE(presence != nullptr);

	// A machine with no network at all has no socket to open, and that is an
	// ordinary outcome rather than a failure to start.
	if (presence->Fault() == PresenceFault::NoBeaconSocket) {
		CHECK_FALSE(presence->Announcing());
		return;
	}

	CHECK(presence->Announcing());
	CHECK(presence->AnnouncingFrom().IsValid());

	// **Ephemeral, and this is the assertion that matters.** The well-known
	// port belongs to listeners; a host that took it could only ever host one
	// session per machine.
	CHECK(presence->AnnouncingFrom().Port != QUIET_PORT);
	CHECK(presence->AnnouncingFrom().Port != 0);

	// The settings decide the protocol and the purpose, in both directions -
	// so a studio cannot announce one thing and collect another.
	CHECK(presence->Advertised().Protocol == 12);
	CHECK(presence->Advertised().Use == Purpose::Studio);

	presence->Pump(0.0);
	presence->Pump(1.0);
}

TEST_CASE("two processes on one machine can both listen", "[network][presence]") {
	PresenceSettings settings;
	settings.Discover = true;
	settings.DiscoveryPort = QUIET_PORT;

	const std::unique_ptr<Presence> first = Presence::Open(settings);
	const std::unique_ptr<Presence> second = Presence::Open(settings);
	REQUIRE(first != nullptr);
	REQUIRE(second != nullptr);

	// The reason `TransportSettings::ReuseAddress` exists. Without it the
	// second one binds nothing and silently discovers nothing - a client and
	// the studio open at once is ordinary, and so is two clients under test.
	if (first->Fault() == PresenceFault::NoDiscoverySocket) {
		// A platform that will not share the port at all. Said out loud rather
		// than asserted away.
		WARN("this machine refused a second listener on the discovery port");
		return;
	}
	CHECK(first->Discovering());
	CHECK(second->Discovering());
	CHECK(second->Fault() == PresenceFault::None);
}

TEST_CASE("a rendezvous address that is not one is a fault, not a crash", "[network][presence]") {
	PresenceSettings settings;
	settings.RendezvousAddress = "rendezvous.example.com:47601";

	const std::unique_ptr<Presence> presence = Presence::Open(settings);
	REQUIRE(presence != nullptr);

	// A host name is refused for `Endpoint::Parse`'s reason: resolving one is a
	// blocking call to a network service, and nothing at this layer may block.
	// An operator with a name resolves it themselves.
	CHECK(presence->Fault() == PresenceFault::BadRendezvousAddress);
	CHECK_FALSE(presence->Rendezvousing());

	PresenceSettings numeric;
	numeric.RendezvousAddress = "127.0.0.1:47601";
	const std::unique_ptr<Presence> reachable = Presence::Open(numeric);
	REQUIRE(reachable != nullptr);
	if (reachable->Fault() == PresenceFault::NoBeaconSocket) {
		return;
	}
	CHECK(reachable->Fault() == PresenceFault::None);
	CHECK(reachable->Rendezvousing());
}

TEST_CASE("what is announced can be replaced after opening", "[network][presence]") {
	PresenceSettings settings;
	settings.Announce = true;
	settings.DiscoveryPort = QUIET_PORT;

	const std::unique_ptr<Presence> presence = Presence::Open(settings, Sample());
	REQUIRE(presence != nullptr);
	if (presence->Fault() == PresenceFault::NoBeaconSocket) {
		return;
	}

	Advert busier = presence->Advertised();
	busier.Peers = 4;
	busier.Detail = "Baseplate";
	presence->SetAdvert(busier);

	CHECK(presence->Advertised().Peers == 4);
	CHECK(presence->Advertised().Detail == "Baseplate");
	CHECK(presence->Advertised().Session == busier.Session);
}

TEST_CASE("a rendezvous registration does not depend on announcing", "[network][presence]") {
	// **The regression this case exists for.** A dedicated server on the
	// internet registers with a point and announces to nobody, and tying the
	// two together made that exact case register nothing while still logging
	// that it had. Checked against a real socket standing in for the point,
	// because the bug was in what did or did not leave the process.
	engine::net::TransportSettings quiet;
	std::unique_ptr<engine::net::Transport> point = engine::net::MakeUdpTransport(0, quiet);
	if (point == nullptr) {
		WARN("no socket available to stand in for a rendezvous point");
		return;
	}

	PresenceSettings settings;
	settings.Announce = false;
	settings.Discover = false;
	settings.RendezvousAddress = engine::net::Endpoint::LoopbackIPv4(point->Local().Port).Text();

	const std::unique_ptr<Presence> presence = Presence::Open(settings, Sample());
	REQUIRE(presence != nullptr);
	if (!presence->Rendezvousing()) {
		WARN("no socket available to register from");
		return;
	}
	CHECK_FALSE(presence->Announcing());

	presence->Pump(0.0);

	// The registration is due on the first pump, because a host that waited a
	// full interval to say it exists is a host nobody can find for that
	// interval.
	std::vector<std::byte> datagram;
	const engine::net::Transport::Inbound inbound = point->Receive(datagram);
	REQUIRE(inbound.Status == engine::net::TransportStatus::Ok);
	CHECK(network::IsRendezvousMessage(datagram));
}
