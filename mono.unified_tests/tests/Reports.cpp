// The contradictions, checked against reports built to contain them.
//
// **A checker that has never seen a failure is a checker nobody has tested.**
// `tests/Crossing.cpp` runs real arrangements and asserts they are clean, which
// proves the checker does not fire on a correct engine and proves nothing at
// all about whether it fires on a broken one. These cases are the other half:
// a report with a known disagreement in it, and the claim that `CrossCheck`
// names it.
//
// Hand-built rather than produced by breaking something, because the point is
// the arithmetic. A `Reports` is a plain aggregate of other modules' plain
// aggregates, so a case can state exactly one disagreement and nothing else.

#include <engine/net/Packet.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <unified/Arrangement.hpp>
#include <unified/Reports.hpp>
#include <vector>

TEST_SUITE_ID("unified.reports")
TEST_DEPENDS("unified.arrangement")

using unified::Arrangement;
using unified::Content;
using unified::Contradiction;
using unified::CrossCheck;
using unified::Discovery;
using unified::Reports;
using unified::Transport;

namespace {
	// A report every claim holds for, so a case changes one thing and sees one
	// contradiction rather than reading a list.
	Reports Agreeing() {
		Reports reports;
		reports.Ticks = 100;
		reports.Produced = 200;
		reports.Handed = 200;
		reports.LargestMessage = 900;
		reports.Replica.Snapshots = 1;
		reports.Replica.Deltas = 99;
		reports.ServerEntities = 16;
		reports.ClientEntities = 16;
		reports.Drawn = 16;
		return reports;
	}

	// Whether any contradiction is between these two modules.
	bool Names(const std::vector<Contradiction> &found, std::string_view between) {
		for (const Contradiction &contradiction : found) {
			if (contradiction.Between == between) {
				return true;
			}
		}
		return false;
	}
}

TEST_CASE("a report every module agrees with produces nothing", "[unified][reports]") {
	// The floor. Without this, every case below passes by the checker firing
	// on everything.
	CHECK(CrossCheck(Agreeing()).empty());
}

TEST_CASE("a message that fell out of the send loop is named", "[unified][reports]") {
	// **A partition, and the one number no module keeps.** The authority knows
	// what it produced and the link knows what it took; that they add up is a
	// claim about the loop between them, which is this program's own code and
	// therefore this program's own check.
	Reports reports = Agreeing();
	reports.Handed = 199;

	const std::vector<Contradiction> found = CrossCheck(reports);
	REQUIRE_FALSE(found.empty());
	CHECK(Names(found, "replication/unified"));

	// A refusal accounts for it, because a refusal is handed back.
	reports.Refused = 1;
	CHECK(CrossCheck(reports).empty());
}

TEST_CASE("a message too large for a datagram is caught with no datagram in sight", "[unified][reports]") {
	// **The four-times bug.** The direct arrangement has no framing, so a
	// message over the limit crosses happily and is refused by the first real
	// link it meets. Checked in every arrangement rather than only the ones
	// with a wire, because the one without a wire is where it is produced.
	Reports reports = Agreeing();
	reports.LargestMessage = engine::net::Packet::MAXIMUM_MESSAGE_BYTES + 1;

	const std::vector<Contradiction> found = CrossCheck(reports);
	REQUIRE_FALSE(found.empty());
	CHECK(Names(found, "replication/net"));
}

TEST_CASE("a route the client asked for and the server never saw is named", "[unified][reports]") {
	// The claim `mono.server` cannot make because it does not link
	// `mono.client`, and `mono.client` cannot make for the same reason back.
	Reports reports = Agreeing();
	reports.Ran = Arrangement{.Serving = Content::Relayed};
	reports.Relay.emplace();
	reports.Link.emplace();

	reports.Link->Requests = 8;
	reports.Link->Completed = 7;
	reports.Relay->Requests = 7;
	reports.Relay->Served = 7;

	// One ask reached neither branch: not accepted, not rate-dropped. Under a
	// direct arrangement there is nothing for it to be in flight in.
	const std::vector<Contradiction> found = CrossCheck(reports);
	REQUIRE_FALSE(found.empty());
	CHECK(Names(found, "client/server"));

	// A rate limiter refusing it accounts for it. **This is the fix that made
	// the claim stronger**: the first version of this check compared accepted
	// requests against asked ones and called a working bucket a lost message.
	reports.Relay->Dropped = 1;
	reports.Link->Completed = 7;
	CHECK(CrossCheck(reports).empty());
}

TEST_CASE("a route only one end thinks finished is named", "[unified][reports]") {
	Reports reports = Agreeing();
	reports.Ran = Arrangement{.Serving = Content::Relayed};
	reports.Relay.emplace();
	reports.Link.emplace();

	reports.Link->Requests = 8;
	reports.Relay->Requests = 8;
	reports.Relay->Served = 8;
	reports.Link->Completed = 7;

	CHECK(Names(CrossCheck(reports), "client/server"));

	// **Not a contradiction over a lossy link**, where a route genuinely can be
	// in flight or gone. Asserting equality there would be asserting that a
	// link built to lose things does not.
	reports.Ran.Carrying = Transport::Lossy;
	reports.ToClient.emplace();
	reports.ToClient->Arrived = 400;
	reports.ToClient->Dropped = 3;
	CHECK(CrossCheck(reports).empty());
}

TEST_CASE("a lossy link that lost nothing is named", "[unified][reports]") {
	// **A case that meant to lose something and lost nothing passes for the
	// wrong reason**, which is what `net::LossStatistics` asks a caller to
	// assert against by name. Here it is asserted once, for every arrangement
	// that claims to be lossy.
	Reports reports = Agreeing();
	reports.Ran = Arrangement{.Carrying = Transport::Lossy};
	reports.ToClient.emplace();
	reports.ToClient->Arrived = 400;

	CHECK(Names(CrossCheck(reports), "net/unified"));

	reports.ToClient->Dropped = 1;
	CHECK(CrossCheck(reports).empty());
}

TEST_CASE("an announcement nothing heard is named", "[unified][reports]") {
	// The subnet is a lossless loopback, so a shortfall is the encoder and the
	// decoder disagreeing rather than a datagram going missing - which is the
	// whole reason to run the pair rather than each side's own suite.
	Reports reports = Agreeing();
	reports.Ran = Arrangement{.Finding = Discovery::Advertised};
	reports.Beacon.emplace();
	reports.Directory.emplace();

	reports.Beacon->Announcements = 10;
	reports.Directory->Listed = 1;
	reports.Directory->Refreshed = 8;

	CHECK(Names(CrossCheck(reports), "network/network"));

	reports.Directory->Refreshed = 9;
	CHECK(CrossCheck(reports).empty());
}

TEST_CASE("a world that did not arrive and a world that did not draw are told apart", "[unified][reports]") {
	// **Two symptoms with one sentence and two fixes**, which is the split the
	// per-tick table's columns exist for. Rows that did not arrive is a
	// replication problem; rows that arrived and were not drawn is a scene one,
	// and the components that go missing are the ones sent once in the joining
	// snapshot and never again.
	Reports reports = Agreeing();
	reports.ClientEntities = 12;
	CHECK(Names(CrossCheck(reports), "client/server"));

	reports = Agreeing();
	reports.Drawn = 12;
	CHECK(Names(CrossCheck(reports), "client/scene"));
}

TEST_CASE("the formatted report names every module, present or not", "[unified][reports]") {
	// **An absent module is named as absent rather than omitted.** A relay that
	// served nothing and a run with no relay in it are different facts, and a
	// report that printed neither line would make them the same fact.
	const std::string text = unified::Format(Agreeing());

	CHECK(text.find("engine/replication") != std::string::npos);
	CHECK(text.find("engine/net") != std::string::npos);
	CHECK(text.find("engine/core") != std::string::npos);
	CHECK(text.find("cdn") != std::string::npos);
	CHECK(text.find("server") != std::string::npos);
	CHECK(text.find("client") != std::string::npos);
	CHECK(text.find("network") != std::string::npos);

	CHECK(text.find("absent - this arrangement carries no content") != std::string::npos);
	CHECK(text.find("absent - this arrangement announces nothing") != std::string::npos);
	CHECK(text.find("every module's report agrees with every other") != std::string::npos);
}
