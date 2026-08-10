// Team create's session layer: what an editor announces, what it holds, and
// what it refuses.
//
// The wire is `network`'s and is covered there over a loopback. What belongs
// here is the editor's own shape — that watching announces nothing, that
// hosting produces an invitation somebody can pass on, and that leaving takes
// it all back down.

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <studio/TeamCreate.hpp>
#include <vector>

TEST_SUITE_ID("studio.teamcreate")
TEST_DEPENDS("network.presence")

using studio::TeamCreate;
using studio::TeamCreateSettings;

TEST_CASE("a fresh editor holds no socket and sees nobody", "[studio][teamcreate]") {
	TeamCreate team;

	// The property that keeps this off every editor's start-up path: nothing
	// is opened until somebody asks.
	CHECK_FALSE(team.Watching());
	CHECK_FALSE(team.Hosting());
	CHECK(team.Peers().empty());
	CHECK(team.Invitation().empty());
	CHECK(team.Fault() == network::PresenceFault::None);

	// And pumping one that was never started is a null check.
	team.Pump(0.0);
	team.Leave(1.0);
	CHECK_FALSE(team.Watching());
}

TEST_CASE("watching announces nothing about this editor", "[studio][teamcreate]") {
	TeamCreate team;
	const std::vector<std::string> noKeys;
	team.Watch({}, noKeys);

	CHECK(team.Watching());

	// **The assertion that matters.** Somebody who opened the panel to look has
	// not decided to invite anybody, and an editor that announced itself the
	// moment the window opened would be publishing a project name they had not
	// chosen to publish.
	CHECK_FALSE(team.Hosting());
	CHECK_FALSE(team.Session().Session.IsValid());
	CHECK(team.Invitation().empty());
}

TEST_CASE("hosting produces a session and an invitation to pass on", "[studio][teamcreate]") {
	TeamCreate team;

	TeamCreateSettings offering;
	offering.Name = "the shared project";
	offering.Project = "Baseplate";
	offering.Secret = "let us both edit";
	offering.Port = 7777;

	std::string trouble;
	REQUIRE(team.Host(offering, trouble));
	CHECK(team.Watching());

	CHECK(team.Session().Use == network::Purpose::Studio);
	CHECK(team.Session().Admits == network::Access::Private);
	CHECK(team.Session().Name == "the shared project");
	CHECK(team.Session().Detail == "Baseplate");
	CHECK(team.Session().At.Port == 7777);
	CHECK(team.Session().Session.IsValid());

	// The key is the invitation, and it has to be copyable out of the panel or
	// a private session is a session of one.
	CHECK(team.Invitation().size() == network::SessionKey::BYTES * 2);

	// The same passphrase derives the same key on the machine at the other end,
	// which is the whole point of the passphrase path.
	auto theirs = network::SessionKey::FromPassphrase("let us both edit");
	REQUIRE(theirs.has_value());
	CHECK(theirs->Text() == team.Invitation());
}

TEST_CASE("a public session says so and carries no invitation", "[studio][teamcreate]") {
	TeamCreate team;

	TeamCreateSettings offering;
	offering.Name = "anybody";
	offering.Port = 7777;

	std::string trouble;
	REQUIRE(team.Host(offering, trouble));
	CHECK(team.Session().Admits == network::Access::Public);
	CHECK(team.Invitation().empty());
}

TEST_CASE("leaving takes the session back down", "[studio][teamcreate]") {
	TeamCreate team;

	TeamCreateSettings offering;
	offering.Name = "briefly";
	offering.Secret = "a passphrase";
	offering.Port = 7777;

	std::string trouble;
	REQUIRE(team.Host(offering, trouble));
	const bool wasHosting = team.Hosting();

	team.Leave(1.0);
	CHECK_FALSE(team.Watching());
	CHECK_FALSE(team.Hosting());
	CHECK(team.Peers().empty());

	// The invitation goes with it. A key left in a panel after the session it
	// opened is gone is a key somebody sends to a session that is not there.
	CHECK(team.Invitation().empty());
	CHECK_FALSE(team.Session().Session.IsValid());

	// Hosting again is a new session, not a revived one — `ConnectionId`'s rule
	// and `SessionId`'s.
	std::string second;
	REQUIRE(team.Host(offering, second));
	if (wasHosting) {
		CHECK(team.Hosting());
	}
	CHECK(team.Session().Session.IsValid());
}

TEST_CASE("the collaborator count only moves what it changes", "[studio][teamcreate]") {
	TeamCreate team;

	TeamCreateSettings offering;
	offering.Name = "counting";
	offering.Port = 7777;
	offering.PeerLimit = 4;

	std::string trouble;
	REQUIRE(team.Host(offering, trouble));
	CHECK(team.Session().PeerLimit == 4);

	if (!team.Hosting()) {
		// No subnet on this machine. The count has nothing to be announced on
		// and the rest of the suite has already covered what it would say.
		return;
	}

	CHECK(team.Session().Peers == 1);
	team.SetCollaborators(3);
	CHECK(team.Session().Peers == 3);
	CHECK_FALSE(team.Session().IsFull());

	team.SetCollaborators(4);
	CHECK(team.Session().IsFull());
}
