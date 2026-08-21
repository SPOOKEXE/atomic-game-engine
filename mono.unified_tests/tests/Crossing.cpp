// Every arrangement of the engine's halves, and the modules agreeing about it.
//
// **These cases are about the seams between modules, not about the modules.**
// Everything below `Authority::Outgoing` is `engine.replication.*`'s to test.
// Chunk layout is `cdn.delivery`'s. A rate limiter is `server.contentrelay`'s.
// An advert's frame is `network.advert`'s. Each of those has a suite and each
// of those suites is better at its own subject than anything here could be.
//
// What is this program's own is the joins: a route `cdn` published arriving at
// the client `server` relayed it to, an announcement `network` encoded being
// the one `network` decoded, and a world that is the same world on both sides
// of whatever is between them. No single module can check any of those,
// because no single module links the other end of it.
//
// Headless, and one worker, so a run of this agrees with the run before it.

#include <engine/core/HeapProfile.hpp>
#include <engine/net/Packet.hpp>
#include <engine/scene/Components.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <unified/Arrangement.hpp>
#include <unified/Crossing.hpp>
#include <unified/Reports.hpp>
#include <vector>

TEST_SUITE_ID("unified.crossing")
TEST_DEPENDS("unified.harness")
TEST_DEPENDS("unified.arrangement")
TEST_DEPENDS("cdn.publisher")
TEST_DEPENDS("client.contentlink")
TEST_DEPENDS("engine.delivery.relay")
TEST_DEPENDS("network.beacon")
TEST_DEPENDS("network.directory")
TEST_DEPENDS("server.contentrelay")

using engine::core::HeapProfile;
using unified::AllArrangements;
using unified::Arrangement;
using unified::Content;
using unified::Contradiction;
using unified::CrossCheck;
using unified::Crossing;
using unified::Discovery;
using unified::Reports;
using unified::Settings;
using unified::Transport;

namespace {
	Settings Small() {
		Settings settings;
		settings.Entities = 16;
		return settings;
	}

	// Every contradiction on one line, so a failure says which claim broke
	// rather than only that one did.
	std::string Spell(const std::vector<Contradiction> &found) {
		std::string text;
		for (const Contradiction &contradiction : found) {
			text += "\n  " + contradiction.Between + ": " + contradiction.Says;
		}
		return text;
	}

	// Runs one arrangement long enough for every axis to have done something.
	//
	// Past the join, past the first content route, and past several
	// announcements.
	Reports Run(const Arrangement &arrangement, int ticks = 90) {
		Crossing crossing(Small(), arrangement);
		REQUIRE(crossing.Join());
		for (int step = 0; step < ticks; step++) {
			crossing.Step();
		}
		return crossing.Gather();
	}
}

TEST_CASE("every arrangement crosses the world and every module agrees", "[unified][matrix]") {
	// **The case this program exists for now.** Twelve ways of standing the
	// engine up, and in each of them the server's world reaches the client, is
	// drawn, and every claim spanning two modules holds. A failure names the
	// arrangement and the two modules, which between them is the whole of
	// where to look.
	for (const Arrangement &arrangement : AllArrangements()) {
		INFO("arrangement " << arrangement.Name());

		const Reports reports = Run(arrangement);

		CHECK(reports.ServerEntities == 16);
		CHECK(reports.ClientEntities == reports.ServerEntities);
		CHECK(reports.Drawn == reports.ClientEntities);

		const std::vector<Contradiction> found = CrossCheck(reports);
		INFO("contradictions" << Spell(found));
		CHECK(found.empty());
	}
}

TEST_CASE("content published by cdn reaches the client through the server", "[unified][content]") {
	// **The flow that touches four modules and belongs to none of them.**
	// `cdn::Publish` wrote a store, `delivery` fetched routes out of it,
	// `server::ContentRelay` rationed them onto the link and
	// `client::ContentLink` put them back together. Every one of those has a
	// suite; the claim that the bytes the last one holds are the bytes the
	// first one wrote is checkable in exactly one place.
	const Reports reports = Run(Arrangement{.Serving = Content::Relayed});

	REQUIRE(reports.Published.has_value());
	REQUIRE(reports.Relay.has_value());
	REQUIRE(reports.Link.has_value());

	CHECK(reports.Published->Assets > 0);
	CHECK(reports.Published->Chunks > 0);

	// Routes actually crossed rather than merely being asked for.
	CHECK(reports.Relay->Served > 0);
	CHECK(reports.Relay->SentBytes > 0);

	// And the two ends counted the same ones.
	CHECK(reports.Link->Completed == reports.Relay->Served);
	CHECK(reports.Link->Discarded == 0);
}

TEST_CASE("content survives a link that loses datagrams", "[unified][content]") {
	// **The arrangement that is more than the sum of its axes.** A route is
	// several chunks on a reliable, ordered channel; losing one means the
	// pieces behind it arrive with a hole in front of them, and `ContentLink`
	// assembles contiguously on purpose. So this is the case where content and
	// loss interact, and neither `server.contentrelay` nor `client.contentlink`
	// can build it - one has no client and the other has no wire.
	const Reports reports = Run(Arrangement{.Carrying = Transport::Lossy, .Serving = Content::Relayed}, 150);

	REQUIRE(reports.ToClient.has_value());
	REQUIRE(reports.Link.has_value());
	REQUIRE(reports.Relay.has_value());

	// A lossy case that lost nothing passed for the wrong reason.
	CHECK(reports.ToClient->Dropped > 0);

	// Nothing was thrown away, because the channel is reliable and ordered and
	// the retransmission fills the hole before the next piece is offered.
	CHECK(reports.Link->Discarded == 0);
	CHECK(reports.Link->Completed > 0);
	CHECK(reports.ClientEntities == reports.ServerEntities);
}

TEST_CASE("an announcement leaves the beacon and lands in the directory", "[unified][discovery]") {
	// `network.beacon` proves an advert encodes and `network.directory` proves
	// one decodes. Neither runs the pair, and the pair is where a version bump
	// on one side and not the other shows up.
	const Reports reports = Run(Arrangement{.Finding = Discovery::Advertised});

	REQUIRE(reports.Beacon.has_value());
	REQUIRE(reports.Directory.has_value());

	CHECK(reports.Beacon->Announcements > 1);
	CHECK(reports.Beacon->Undelivered == 0);
	CHECK(reports.Directory->Malformed == 0);
	CHECK(reports.Directory->Listed == 1);
	CHECK(reports.Directory->Listed + reports.Directory->Refreshed == reports.Beacon->Announcements);
}

TEST_CASE("a wire refuses nothing this world produces", "[unified][matrix]") {
	// **The four-times bug, checked where it can actually happen.** The
	// direct arrangement can only say a message is over the limit; a real link
	// refuses it, and a refusal handed back through `Authority::Unsent` is the
	// difference between a hole that is repaired and one that is permanent.
	Settings settings;
	settings.Entities = 512;

	Crossing crossing(settings, Arrangement{.Carrying = Transport::Loopback});
	REQUIRE(crossing.Join());
	for (int step = 0; step < 60; step++) {
		crossing.Step();
	}

	const Reports reports = crossing.Gather();
	CHECK(reports.LargestMessage > 0);
	CHECK(reports.LargestMessage <= engine::net::Packet::MAXIMUM_MESSAGE_BYTES);
	CHECK(reports.Authority.Oversized == 0);
	CHECK(CrossCheck(reports).empty());
}

TEST_CASE("a crossing gives back what it took", "[unified][heap]") {
	// **The long-term runaway check, at unit-test speed.** A soak run is the
	// honest way to find a leak and it costs minutes; this costs milliseconds
	// and catches the thing a soak would, because a crossing that grows the
	// process every time it is built and destroyed is a leak whether it is run
	// twice or ten thousand times.
	//
	// **The first one is the warm-up and is not measured.** Building a world
	// fills caches that are meant to stay filled - component registrations, the
	// job system's threads, a log sink's buffer - and counting those as a leak
	// would make this fail on a correct engine. The second one has nothing left
	// to fill, so what it does not give back is what it kept.
	if (!HeapProfile::IsCompiledIn()) {
		SUCCEED("built without MONO_HEAP_PROFILE, so there is nothing to measure");
		return;
	}

	const auto Cycle = [] {
		Crossing crossing(Small(), Arrangement{.Serving = Content::Relayed});
		REQUIRE(crossing.Join());
		for (int step = 0; step < 30; step++) {
			crossing.Step();
		}
	};

	Cycle();
	const int64_t warm = HeapProfile::Totals().LiveBytes;
	Cycle();
	const int64_t after = HeapProfile::Totals().LiveBytes;

	// A quarter of a megabyte of slack, which is a few caches settling rather
	// than a per-run retention: one crossing holds megabytes while it lives.
	const int64_t kept = after - warm;
	INFO("kept " << kept << " bytes across one build and teardown");
	CHECK(kept < 256 * 1024);
}

TEST_CASE("the heap profiler sees this program's own stages", "[unified][heap]") {
	// **Instrumentation that is not wired reports a clean run.** The tags below
	// are the stages a leak would be attributed to, and a tag that stopped
	// being pushed would make every future soak of this program blame
	// `untagged` - which is the one answer that says nothing.
	if (!HeapProfile::IsCompiledIn()) {
		SUCCEED("built without MONO_HEAP_PROFILE, so there are no tags to find");
		return;
	}

	{
		Crossing crossing(
			Small(), Arrangement{.Serving = Content::Relayed, .Finding = Discovery::Advertised}
		);
		REQUIRE(crossing.Join());
		crossing.Step();
	}

	std::vector<std::string> paths;
	for (uint32_t index = 0; index < HeapProfile::NodeCount(); index++) {
		paths.push_back(HeapProfile::Path(index));
	}

	const auto Names = [&paths](std::string_view tag) {
		for (const std::string &path : paths) {
			if (path.find(tag) != std::string::npos) {
				return true;
			}
		}
		return false;
	};

	CHECK(Names("unified.step"));
	CHECK(Names("unified.server.publish"));
	CHECK(Names("unified.client.draw"));
	CHECK(Names("unified.content"));
	CHECK(Names("unified.discovery"));
}
