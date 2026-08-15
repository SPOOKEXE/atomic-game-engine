// The table three reaches share, and the four ways it refuses to grow without
// bound.
//
// The case worth reading first is `Dial`: a host announces the address it bound
// and that address is usually the wildcard, so the port comes from the advert
// and the address comes from the datagram. Getting that backwards produces a
// browser whose rows all point at 0.0.0.0 and a person who cannot join a
// session sitting on the same switch.

#include <engine/net/Transport.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <memory>
#include <network/Beacon.hpp>
#include <network/Directory.hpp>
#include <utility>
#include <vector>

TEST_SUITE_ID("network.directory")
TEST_DEPENDS("network.advert")
TEST_DEPENDS("network.beacon")

using engine::net::Endpoint;
using engine::net::Transport;
using engine::net::TransportSettings;
using network::Access;
using network::Advert;
using network::Beacon;
using network::Directory;
using network::DirectorySettings;
using network::Listing;
using network::Purpose;
using network::Reach;
using network::SessionId;
using network::SessionKey;

namespace {
	std::vector<std::unique_ptr<Transport>> Subnet(size_t ends = 2) {
		TransportSettings settings;
		settings.Broadcast = true;
		return engine::net::MakeLoopbackTransport(ends, settings);
	}

	Advert Sample(std::string_view name = "a session") {
		Advert advert;
		advert.Session = SessionId::Draw();
		advert.Name = std::string(name);
		// The wildcard, which is what a host that bound every interface knows
		// about itself.
		advert.At = Endpoint::FromIPv4({0, 0, 0, 0}, 7777);
		return advert;
	}

	SessionKey Key(std::string_view words) {
		auto key = SessionKey::FromPassphrase(words);
		REQUIRE(key.has_value());
		return std::move(*key);
	}
}

TEST_CASE("an announcement becomes a listing", "[network][directory]") {
	std::vector<std::unique_ptr<Transport>> ends = Subnet();
	const Advert advert = Sample();

	Beacon beacon(*ends[0], advert);
	Directory table;

	beacon.Pump(0.0);
	CHECK(table.Observe(*ends[1], 0.0) == 1);

	REQUIRE(table.Listings().size() == 1);
	const Listing &row = table.Listings()[0];
	CHECK(row.Session.Session == advert.Session);
	CHECK(row.Via == Reach::Lan);
	CHECK(row.Joinable());
	CHECK(table.Find(advert.Session) != nullptr);
	CHECK(table.Counters().Listed == 1);
}

TEST_CASE("a row dials the address the datagram came from", "[network][directory]") {
	std::vector<std::unique_ptr<Transport>> ends = Subnet();
	Beacon beacon(*ends[0], Sample());
	Directory table;

	beacon.Pump(0.0);
	REQUIRE(table.Observe(*ends[1], 0.0) == 1);

	// The host announced 0.0.0.0:7777 - every interface, which is a promise
	// about nothing in particular. What it is dialed on is the address the
	// announcement arrived over, with the port it advertised.
	const Endpoint dial = table.Listings()[0].Dial();
	CHECK(dial.Port == 7777);
	CHECK(dial.Address == ends[0]->Local().Address);
	CHECK(dial != Endpoint::FromIPv4({0, 0, 0, 0}, 7777));

	// A row that was offered with no source falls back to what it advertised,
	// which is what a config file's row is.
	Advert typed = Sample("typed in");
	typed.At = Endpoint::LoopbackIPv4(9000);
	REQUIRE(table.Offer(typed, Reach::Remote, {}, 0.0));
	CHECK(table.Find(typed.Session)->Dial() == Endpoint::LoopbackIPv4(9000));

	// And one with neither is not something to act on. An invalid endpoint
	// rather than the source's ephemeral port, which would look usable.
	Advert nowhere = Sample("nowhere");
	nowhere.At = {};
	REQUIRE(table.Offer(nowhere, Reach::Remote, {}, 0.0));
	CHECK_FALSE(table.Find(nowhere.Session)->Dial().IsValid());
}

TEST_CASE("one session is one row, whichever way it arrived", "[network][directory]") {
	std::vector<std::unique_ptr<Transport>> ends = Subnet();
	const Advert advert = Sample();

	Beacon beacon(*ends[0], advert);
	Directory table;

	beacon.Pump(0.0);
	table.Observe(*ends[1], 0.0);

	// The same session, listed again through a rendezvous point. A browser
	// showing it twice is showing a person a choice that is not one.
	CHECK(table.Offer(advert, Reach::Peer, Endpoint::LoopbackIPv4(4242), 0.0));
	CHECK(table.Listings().size() == 1);

	// And the nearer reach is the one kept: a LAN address survives a router
	// forgetting a mapping and a punched one does not.
	CHECK(table.Find(advert.Session)->Via == Reach::Lan);

	// The other way round, a peer row is upgraded when the LAN hears it.
	Directory second;
	CHECK(second.Offer(advert, Reach::Peer, Endpoint::LoopbackIPv4(4242), 0.0));
	CHECK(second.Find(advert.Session)->Via == Reach::Peer);
	beacon.Pump(1.0);
	second.Observe(*ends[1], 1.0);
	CHECK(second.Find(advert.Session)->Via == Reach::Lan);
	CHECK(second.Listings().size() == 1);
}

TEST_CASE("a session that stops announcing is forgotten", "[network][directory]") {
	std::vector<std::unique_ptr<Transport>> ends = Subnet();
	const Advert advert = Sample();

	DirectorySettings settings;
	settings.ForgetAfterSeconds = 5.0;

	Beacon beacon(*ends[0], advert);
	Directory table(settings);

	beacon.Pump(0.0);
	table.Observe(*ends[1], 0.0);
	REQUIRE(table.Listings().size() == 1);

	// Still there while it is announcing, several intervals in - which is the
	// half that matters, because a browser whose rows flicker is one nobody can
	// click.
	for (double now = 1.0; now <= 20.0; now += 1.0) {
		beacon.Pump(now);
		table.Observe(*ends[1], now);
		CHECK(table.Forget(now) == 0);
		CHECK(table.Listings().size() == 1);
	}

	// The host goes away.
	CHECK(table.Forget(24.0) == 0);
	CHECK(table.Forget(26.0) == 1);
	CHECK(table.Listings().empty());
	CHECK(table.Counters().Forgotten == 1);
}

TEST_CASE("a private session is listed and not joinable without the key", "[network][directory]") {
	std::vector<std::unique_ptr<Transport>> ends = Subnet();

	Advert advert = Sample();
	advert.Admits = Access::Private;

	Beacon beacon(*ends[0], advert, Key("the passphrase"));
	REQUIRE(beacon.Announcing());

	Directory blind;
	beacon.Pump(0.0);
	CHECK(blind.Observe(*ends[1], 0.0) == 1);

	// Listed. "I can see it but cannot join it" and "I cannot see it" are
	// different problems, and the person about to be given the key has to be
	// able to see the session exists.
	REQUIRE(blind.Listings().size() == 1);
	CHECK_FALSE(blind.Listings()[0].Authenticated);
	CHECK_FALSE(blind.Listings()[0].Joinable());
	CHECK(blind.Counters().Locked == 1);

	Directory holding;
	holding.Trust(Key("the passphrase"));
	CHECK(holding.Trusted() == 1);
	beacon.Pump(1.0);
	CHECK(holding.Observe(*ends[1], 1.0) == 1);
	CHECK(holding.Listings()[0].Authenticated);
	CHECK(holding.Listings()[0].Joinable());
	CHECK(holding.Counters().Locked == 0);
}

TEST_CASE("a full session is listed and not joinable", "[network][directory]") {
	Directory table;

	Advert advert = Sample();
	advert.Peers = 8;
	advert.PeerLimit = 8;
	REQUIRE(table.Offer(advert, Reach::Remote, {}, 0.0));

	// Shown rather than hidden: somebody waiting for a slot wants to see the
	// session is there and full.
	CHECK(table.Listings().size() == 1);
	CHECK_FALSE(table.Listings()[0].Joinable());
}

TEST_CASE("announcements for another build or another purpose are dropped", "[network][directory]") {
	std::vector<std::unique_ptr<Transport>> ends = Subnet();

	DirectorySettings settings;
	settings.Protocol = 4;
	settings.Use = Purpose::Game;
	Directory table(settings);

	Advert older = Sample("an older build");
	older.Protocol = 3;
	Beacon oldBeacon(*ends[0], older);
	oldBeacon.Pump(0.0);
	CHECK(table.Observe(*ends[1], 0.0) == 0);
	CHECK(table.Counters().WrongProtocol == 1);

	Advert origin = Sample("a content origin");
	origin.Protocol = 4;
	origin.Use = Purpose::Content;
	Beacon originBeacon(*ends[0], origin);
	originBeacon.Pump(1.0);
	CHECK(table.Observe(*ends[1], 1.0) == 0);
	CHECK(table.Counters().WrongPurpose == 1);

	Advert ours = Sample("ours");
	ours.Protocol = 4;
	Beacon oursBeacon(*ends[0], ours);
	oursBeacon.Pump(2.0);
	CHECK(table.Observe(*ends[1], 2.0) == 1);
	CHECK(table.Listings().size() == 1);
	CHECK(table.Counters().Heard == 3);
}

TEST_CASE("the table is bounded and a flood cannot evict what is being looked at", "[network][directory]") {
	DirectorySettings settings;
	settings.MaximumListings = 4;
	Directory table(settings);

	std::vector<SessionId> first;
	for (size_t index = 0; index < 4; ++index) {
		const Advert advert = Sample();
		first.push_back(advert.Session);
		CHECK(table.Offer(advert, Reach::Remote, {}, 0.0));
	}
	CHECK(table.Listings().size() == 4);

	// Past the cap the new one is dropped rather than the oldest evicted. A
	// table that evicted would let a flood push out every session somebody is
	// looking at, which is the outcome the cap exists to prevent.
	for (size_t index = 0; index < 100; ++index) {
		CHECK_FALSE(table.Offer(Sample(), Reach::Remote, {}, 0.0));
	}
	CHECK(table.Listings().size() == 4);
	CHECK(table.Counters().Overflowed == 100);
	for (const SessionId &session : first) {
		CHECK(table.Find(session) != nullptr);
	}

	// A session already listed still refreshes when the table is full, which is
	// the other half of that: the sessions somebody is watching must not go
	// stale because somebody else started flooding.
	Advert refresh = Sample();
	refresh.Session = first[0];
	CHECK(table.Offer(refresh, Reach::Remote, {}, 9.0));
	CHECK(table.Find(first[0])->LastSeenSeconds == 9.0);
}

TEST_CASE("rubbish on the discovery port is counted and ignored", "[network][directory]") {
	std::vector<std::unique_ptr<Transport>> ends = Subnet();
	Directory table;

	const std::vector<std::byte> rubbish(64, std::byte{0xAB});
	REQUIRE(ends[0]->Send(ends[1]->Local(), rubbish) == engine::net::TransportStatus::Ok);

	CHECK(table.Observe(*ends[1], 0.0) == 0);
	CHECK(table.Counters().Heard == 1);
	CHECK(table.Counters().Malformed == 1);
	CHECK(table.Listings().empty());
}

TEST_CASE("a row can be dropped by hand and comes back if it is still there", "[network][directory]") {
	std::vector<std::unique_ptr<Transport>> ends = Subnet();
	const Advert advert = Sample();
	Beacon beacon(*ends[0], advert);
	Directory table;

	beacon.Pump(0.0);
	table.Observe(*ends[1], 0.0);
	REQUIRE(table.Listings().size() == 1);

	// What a caller does after failing to connect: the row goes, and the next
	// announcement brings it back if the host is really there.
	CHECK(table.Drop(advert.Session));
	CHECK(table.Listings().empty());
	CHECK_FALSE(table.Drop(advert.Session));

	beacon.Pump(1.0);
	table.Observe(*ends[1], 1.0);
	CHECK(table.Listings().size() == 1);

	table.Clear();
	CHECK(table.Listings().empty());
}
