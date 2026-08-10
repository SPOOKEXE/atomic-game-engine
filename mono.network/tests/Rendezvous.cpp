// Two peers and the third party that introduces them, over a loopback network
// with real encoding and no sockets.
//
// The loopback has no NAT in it, which is exactly why it is the right harness:
// every message of the protocol still crosses, in the order it crosses on the
// internet, and the only thing missing is the router that would have dropped
// the first datagram. What that leaves testable is the whole state machine —
// who says what, in what order, and what each end refuses.
//
// What it cannot test is whether a real router cooperates. Nothing can, from a
// suite; `ReachState::Failed` is what that looks like from here.

#include <engine/net/Transport.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <memory>
#include <network/Directory.hpp>
#include <network/Rendezvous.hpp>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

TEST_SUITE_ID("network.rendezvous")
TEST_DEPENDS("network.advert")
TEST_DEPENDS("network.directory")

using engine::net::Endpoint;
using engine::net::Transport;
using network::Access;
using network::Advert;
using network::Directory;
using network::Purpose;
using network::Reach;
using network::ReachState;
using network::RendezvousClient;
using network::RendezvousPoint;
using network::RendezvousSettings;
using network::SessionId;
using network::SessionKey;

namespace {
	// Three ends: the point, a host and a guest.
	constexpr size_t POINT = 0;
	constexpr size_t HOST = 1;
	constexpr size_t GUEST = 2;

	Advert Sample(Purpose use = Purpose::Game) {
		Advert advert;
		advert.Session = SessionId::Draw();
		advert.Use = use;
		advert.Name = "a session";
		advert.At = Endpoint::FromIPv4({0, 0, 0, 0}, 7777);
		return advert;
	}

	SessionKey Key(std::string_view words) {
		auto key = SessionKey::FromPassphrase(words);
		REQUIRE(key.has_value());
		return std::move(*key);
	}

	// One barrier: everybody pumps once, in the order datagrams would flow.
	void Step(
		RendezvousPoint &point,
		Transport &pointWire,
		RendezvousClient &host,
		RendezvousClient &guest,
		Directory *table,
		double nowSeconds
	) {
		host.Pump(nullptr, nowSeconds);
		guest.Pump(table, nowSeconds);
		point.Serve(pointWire, nowSeconds);
		host.Pump(nullptr, nowSeconds);
		guest.Pump(table, nowSeconds);
	}
}

TEST_CASE("a registration is acknowledged with the address the point saw", "[network][rendezvous]") {
	std::vector<std::unique_ptr<Transport>> ends = engine::net::MakeLoopbackTransport(3);
	RendezvousPoint point;

	RendezvousClient host(*ends[HOST], ends[POINT]->Local());
	host.Register(Sample());

	host.Pump(nullptr, 0.0);
	CHECK(host.Counters().Registrations == 1);
	CHECK_FALSE(host.Enrolled());

	point.Serve(*ends[POINT], 0.0);
	CHECK(point.Holding() == 1);
	CHECK(point.Counters().Registrations == 1);

	host.Pump(nullptr, 0.0);
	CHECK(host.Enrolled());

	// The reflexive address. On a loopback there is no NAT, so it equals the
	// host's own — which is the case a guest can dial without punching at all,
	// and the reason the field is worth reporting.
	CHECK(host.Reflexive() == ends[HOST]->Local());

	// Repeated on the interval, which is what keeps a router's mapping alive.
	host.Pump(nullptr, 1.0);
	CHECK(host.Counters().Registrations == 1);
	host.Pump(nullptr, 10.0);
	CHECK(host.Counters().Registrations == 2);
	point.Serve(*ends[POINT], 10.0);
	CHECK(point.Holding() == 1);
	CHECK(point.Counters().Refreshes == 1);
}

TEST_CASE("browsing lists public sessions into the directory", "[network][rendezvous]") {
	std::vector<std::unique_ptr<Transport>> ends = engine::net::MakeLoopbackTransport(3);
	RendezvousPoint point;
	Directory table;

	const Advert advert = Sample();
	RendezvousClient host(*ends[HOST], ends[POINT]->Local());
	host.Register(advert);
	RendezvousClient guest(*ends[GUEST], ends[POINT]->Local());

	host.Pump(nullptr, 0.0);
	point.Serve(*ends[POINT], 0.0);

	guest.Browse(Purpose::Game, 0.0);
	point.Serve(*ends[POINT], 0.0);
	guest.Pump(&table, 0.0);

	CHECK(guest.Counters().Listed == 1);
	REQUIRE(table.Listings().size() == 1);
	CHECK(table.Listings()[0].Session.Session == advert.Session);

	// A peer row, and it dials the address the point observed rather than the
	// wildcard the host announced.
	CHECK(table.Listings()[0].Via == Reach::Peer);
	CHECK(table.Listings()[0].Dial() == Endpoint::LoopbackIPv4(7777));
}

TEST_CASE("a private session is absent from every browse reply", "[network][rendezvous]") {
	std::vector<std::unique_ptr<Transport>> ends = engine::net::MakeLoopbackTransport(3);
	RendezvousPoint point;
	Directory table;

	Advert advert = Sample();
	advert.Admits = Access::Private;

	RendezvousClient host(*ends[HOST], ends[POINT]->Local());
	host.Register(advert, Key("the passphrase"));
	RendezvousClient guest(*ends[GUEST], ends[POINT]->Local());

	host.Pump(nullptr, 0.0);
	point.Serve(*ends[POINT], 0.0);
	CHECK(point.Holding() == 1);

	guest.Browse(Purpose::Game, 0.0);
	point.Serve(*ends[POINT], 0.0);
	guest.Pump(&table, 0.0);

	// Held and not listed. A point cannot check a key it does not hold and must
	// not be given one, so possession of the id is what reaches a private
	// session.
	CHECK(table.Listings().empty());
	CHECK(guest.Counters().Listed == 0);
}

TEST_CASE("a browse reply only carries the purpose that was asked for", "[network][rendezvous]") {
	std::vector<std::unique_ptr<Transport>> ends = engine::net::MakeLoopbackTransport(3);
	RendezvousPoint point;
	Directory table;

	RendezvousClient game(*ends[HOST], ends[POINT]->Local());
	game.Register(Sample(Purpose::Game));
	game.Pump(nullptr, 0.0);
	point.Serve(*ends[POINT], 0.0);

	RendezvousClient guest(*ends[GUEST], ends[POINT]->Local());
	guest.Browse(Purpose::Content, 0.0);
	point.Serve(*ends[POINT], 0.0);
	guest.Pump(&table, 0.0);

	CHECK(table.Listings().empty());
	CHECK(point.Counters().Browses == 1);
}

TEST_CASE("two peers punch through and end up holding an address that works", "[network][rendezvous]") {
	std::vector<std::unique_ptr<Transport>> ends = engine::net::MakeLoopbackTransport(3);
	RendezvousPoint point;

	const Advert advert = Sample();
	RendezvousClient host(*ends[HOST], ends[POINT]->Local());
	host.Register(advert);
	RendezvousClient guest(*ends[GUEST], ends[POINT]->Local());

	host.Pump(nullptr, 0.0);
	point.Serve(*ends[POINT], 0.0);
	host.Pump(nullptr, 0.0);
	REQUIRE(host.Enrolled());

	CHECK(guest.State() == ReachState::Idle);
	REQUIRE(guest.Reach(advert.Session, std::nullopt, 0.0));
	CHECK(guest.State() == ReachState::Locating);

	// A fixed number of beats rather than stopping the moment the guest is
	// through: the guest is through one datagram before the host knows it, and
	// a loop that exited on the guest's state would be asserting on a host that
	// has not been given a tick to hear the answer.
	double now = 0.0;
	for (int beat = 0; beat < 8; ++beat) {
		Step(point, *ends[POINT], host, guest, nullptr, now);
		now += 0.25;
	}

	CHECK(guest.State() == ReachState::Reached);
	CHECK(guest.Reached() == ends[HOST]->Local());
	CHECK(point.Counters().Introductions >= 1);
	CHECK(guest.Counters().Punches >= 1);
	CHECK(host.Counters().Punches >= 1);
	CHECK(guest.Counters().Answered >= 1);
	CHECK(host.Counters().Answered >= 1);
}

TEST_CASE("a private punch needs the key at both ends", "[network][rendezvous]") {
	std::vector<std::unique_ptr<Transport>> ends = engine::net::MakeLoopbackTransport(3);
	RendezvousPoint point;

	Advert advert = Sample();
	advert.Admits = Access::Private;

	RendezvousClient host(*ends[HOST], ends[POINT]->Local());
	host.Register(advert, Key("the passphrase"));
	RendezvousClient wrong(*ends[GUEST], ends[POINT]->Local());

	host.Pump(nullptr, 0.0);
	point.Serve(*ends[POINT], 0.0);
	host.Pump(nullptr, 0.0);

	// The id is enough to be introduced — the point holds no key and cannot
	// decide otherwise. The key is what the poke has to carry.
	REQUIRE(wrong.Reach(advert.Session, Key("not the passphrase"), 0.0));

	double now = 0.0;
	for (int beat = 0; beat < 8; ++beat) {
		Step(point, *ends[POINT], host, wrong, nullptr, now);
		now += 0.25;
	}

	CHECK(wrong.State() != ReachState::Reached);
	CHECK(host.Counters().Refused >= 1);

	// The same session, reached with the right key.
	std::vector<std::unique_ptr<Transport>> second = engine::net::MakeLoopbackTransport(3);
	RendezvousPoint point2;
	RendezvousClient host2(*second[HOST], second[POINT]->Local());
	host2.Register(advert, Key("the passphrase"));
	RendezvousClient right(*second[GUEST], second[POINT]->Local());

	host2.Pump(nullptr, 0.0);
	point2.Serve(*second[POINT], 0.0);
	host2.Pump(nullptr, 0.0);
	REQUIRE(right.Reach(advert.Session, Key("the passphrase"), 0.0));

	now = 0.0;
	for (int beat = 0; beat < 8; ++beat) {
		Step(point2, *second[POINT], host2, right, nullptr, now);
		now += 0.25;
	}
	CHECK(right.State() == ReachState::Reached);
	CHECK(host2.Counters().Refused == 0);
}

TEST_CASE("a session the point does not hold fails at once", "[network][rendezvous]") {
	std::vector<std::unique_ptr<Transport>> ends = engine::net::MakeLoopbackTransport(3);
	RendezvousPoint point;

	RendezvousClient guest(*ends[GUEST], ends[POINT]->Local());
	REQUIRE(guest.Reach(SessionId::Draw(), std::nullopt, 0.0));

	guest.Pump(nullptr, 0.0);
	point.Serve(*ends[POINT], 0.0);
	guest.Pump(nullptr, 0.0);

	// Told rather than left to time out. Somebody who mistyped an id waits
	// eight seconds for nothing otherwise.
	CHECK(guest.State() == ReachState::Failed);
	CHECK(guest.Counters().Unknown == 1);
	CHECK(point.Counters().Unknown == 1);

	// And the null id is not an attempt at all.
	CHECK_FALSE(guest.Reach({}, std::nullopt, 0.0));
}

TEST_CASE("an attempt nothing answers gives up rather than spinning", "[network][rendezvous]") {
	std::vector<std::unique_ptr<Transport>> ends = engine::net::MakeLoopbackTransport(3);

	RendezvousSettings settings;
	settings.GiveUpAfterSeconds = 2.0;
	// The point end exists and nothing serves it, which is what a rendezvous
	// point that is down looks like from here.
	RendezvousClient guest(*ends[GUEST], ends[POINT]->Local(), settings);
	REQUIRE(guest.Reach(SessionId::Draw(), std::nullopt, 0.0));

	for (double now = 0.0; now < 1.9; now += 0.25) {
		guest.Pump(nullptr, now);
		CHECK(guest.State() == ReachState::Locating);
	}
	guest.Pump(nullptr, 2.0);
	CHECK(guest.State() == ReachState::Failed);
	CHECK_FALSE(guest.Reached().IsValid());

	// Failed stays failed. A second try is a second call, not a state that
	// revives itself under a caller who has stopped checking.
	guest.Pump(nullptr, 3.0);
	CHECK(guest.State() == ReachState::Failed);
}

TEST_CASE("withdrawing takes a session off the point, and only its own", "[network][rendezvous]") {
	std::vector<std::unique_ptr<Transport>> ends = engine::net::MakeLoopbackTransport(3);
	RendezvousPoint point;

	const Advert advert = Sample();
	RendezvousClient host(*ends[HOST], ends[POINT]->Local());
	host.Register(advert);
	host.Pump(nullptr, 0.0);
	point.Serve(*ends[POINT], 0.0);
	REQUIRE(point.Holding() == 1);

	// Somebody else who learned the id cannot take it off. Otherwise a denial
	// of service costs one datagram.
	RendezvousClient stranger(*ends[GUEST], ends[POINT]->Local());
	stranger.Register(advert);
	stranger.Withdraw(0.0);
	point.Serve(*ends[POINT], 0.0);
	CHECK(point.Holding() == 1);

	host.Withdraw(1.0);
	point.Serve(*ends[POINT], 1.0);
	CHECK(point.Holding() == 0);
	CHECK(point.Counters().Withdrawals == 1);
}

TEST_CASE("a point forgets a host that stopped registering", "[network][rendezvous]") {
	std::vector<std::unique_ptr<Transport>> ends = engine::net::MakeLoopbackTransport(3);

	network::PointSettings settings;
	settings.ForgetAfterSeconds = 30.0;
	RendezvousPoint point(settings);

	RendezvousClient host(*ends[HOST], ends[POINT]->Local());
	host.Register(Sample());
	host.Pump(nullptr, 0.0);
	point.Serve(*ends[POINT], 0.0);
	REQUIRE(point.Holding() == 1);

	CHECK(point.Forget(29.0) == 0);
	CHECK(point.Forget(31.0) == 1);
	CHECK(point.Counters().Forgotten == 1);
}

TEST_CASE("a point holds a bounded number of sessions", "[network][rendezvous]") {
	std::vector<std::unique_ptr<Transport>> ends = engine::net::MakeLoopbackTransport(3);

	network::PointSettings settings;
	settings.MaximumSessions = 2;
	RendezvousPoint point(settings);

	// Three registrations from one address, which is what a flood looks like
	// before it has bothered to vary anything.
	for (int index = 0; index < 3; ++index) {
		RendezvousClient host(*ends[HOST], ends[POINT]->Local());
		host.Register(Sample());
		host.Pump(nullptr, 0.0);
		point.Serve(*ends[POINT], 0.0);
	}
	CHECK(point.Holding() == 2);
	CHECK(point.Counters().Full == 1);
}

TEST_CASE("rubbish and a client's own replies are counted rather than answered", "[network][rendezvous]") {
	std::vector<std::unique_ptr<Transport>> ends = engine::net::MakeLoopbackTransport(3);
	RendezvousPoint point;

	const std::vector<std::byte> rubbish(48, std::byte{0x5A});
	REQUIRE(ends[HOST]->Send(ends[POINT]->Local(), rubbish) == engine::net::TransportStatus::Ok);
	CHECK(point.Serve(*ends[POINT], 0.0) == 0);
	CHECK(point.Counters().Malformed == 1);

	// Nothing came back. A point that replied to a datagram it did not
	// understand would be a reflector.
	std::vector<std::byte> nothing;
	CHECK(ends[HOST]->Receive(nothing).Status == engine::net::TransportStatus::Empty);

	CHECK_FALSE(network::IsRendezvousMessage(rubbish));
	CHECK_FALSE(network::IsRendezvousMessage({}));
}

TEST_CASE("a shared socket routes by magic", "[network][rendezvous]") {
	// A program that wants the punch to serve its own traffic hands the client
	// the transport that traffic uses, drains it itself, and offers each
	// datagram here. This is that seam.
	std::vector<std::unique_ptr<Transport>> ends = engine::net::MakeLoopbackTransport(3);
	RendezvousPoint point;

	RendezvousClient host(*ends[HOST], ends[POINT]->Local());
	host.Register(Sample());
	host.Deliver({}, {}, nullptr, 0.0);
	point.Serve(*ends[POINT], 0.0);

	std::vector<std::byte> datagram;
	const Transport::Inbound inbound = ends[HOST]->Receive(datagram);
	REQUIRE(inbound.Status == engine::net::TransportStatus::Ok);
	CHECK(network::IsRendezvousMessage(datagram));
	CHECK(host.Deliver(datagram, inbound.From, nullptr, 0.0));
	CHECK(host.Enrolled());

	// Something that is not ours passes straight through, which is what makes
	// sharing the socket possible at all.
	const std::vector<std::byte> foreign(32, std::byte{0x11});
	CHECK_FALSE(host.Deliver(foreign, inbound.From, nullptr, 0.0));
}
