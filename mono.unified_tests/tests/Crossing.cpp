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
#include <engine/core/Name.hpp>
#include <engine/ecs/Attributes.hpp>
#include <engine/net/Packet.hpp>
#include <engine/scene/Components.hpp>
#include <engine/testing/Scenario.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unified/Arrangement.hpp>
#include <unified/Crossing.hpp>
#include <unified/Reports.hpp>
#include <utility>
#include <vector>

TEST_SUITE_ID("unified.crossing")
TEST_DEPENDS("unified.harness")
TEST_DEPENDS("unified.arrangement")
TEST_DEPENDS("engine.testing.scenario")
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

	constexpr std::string_view REPORT_HOOK = "crossing.reports";
	// Imported reports contain scalars, enums, inline optionals and the inline
	// ContentHash digest array. There are no owned strings or dynamic buffers.
	// Adding a nontrivial field requires revisiting this payload byte accounting.
	static_assert(std::is_trivially_copyable_v<Reports>);
	constexpr size_t REPORT_PAYLOAD_BYTES = sizeof(Reports);
	constexpr size_t REPORT_OBSERVATION_BYTES = REPORT_PAYLOAD_BYTES + REPORT_HOOK.size();

	using engine::testing::CheckResult;
	using engine::testing::Observation;
	using engine::testing::ScenarioCheck;
	using engine::testing::ScenarioReport;

	// Only the immutable imported report crosses into check workers. Crossing
	// remains alive until publication joins, retaining its normal job lifecycle.
	std::vector<ScenarioCheck<Reports>> Checks(const Arrangement &arrangement, int ticks) {
		const auto Make = [](std::string name, auto observe) {
			ScenarioCheck<Reports> check;
			check.Name = std::move(name);
			check.SourceSuite = "unified.crossing";
			check.Requirements.Hooks = {{std::string(REPORT_HOOK), 1}};
			check.Observe = [observe](const Observation<Reports> &sample, CheckResult &result) {
				observe(sample.Payload, result);
			};
			return check;
		};
		std::vector<ScenarioCheck<Reports>> checks{
			Make(
				"matrix.world",
				[](const Reports &reports, CheckResult &result) {
					result.Record("server.entities", reports.ServerEntities == 16);
					result.Record("server/client.entities", reports.ClientEntities == reports.ServerEntities);
					result.Record("client/render.entities", reports.Drawn == reports.ClientEntities);
				}
			),
			Make(
				"matrix.agreement",
				[](const Reports &reports, CheckResult &result) {
					const auto found = CrossCheck(reports);
					result.Record("modules.agreement", found.empty(), Spell(found));
				}
			),
			Make(
				"content",
				[](const Reports &reports, CheckResult &result) {
					const bool available = reports.Published && reports.Relay && reports.Link;
					result.Record("cdn/server/client.reports", available);
					if (!available) {
						return;
					}
					result.Record("cdn.assets", reports.Published->Assets > 0);
					result.Record("cdn.chunks", reports.Published->Chunks > 0);
					result.Record("server.served", reports.Relay->Served > 0);
					result.Record("server.sent-bytes", reports.Relay->SentBytes > 0);
					result.Record("server/client.routes", reports.Link->Completed == reports.Relay->Served);
					result.Record("client.discarded", reports.Link->Discarded == 0);
				}
			),
			Make(
				"lossy-content",
				[](const Reports &reports, CheckResult &result) {
					const bool available = reports.ToClient && reports.Link && reports.Relay;
					result.Record("net/server/client.reports", available);
					if (!available) {
						return;
					}
					result.Record("net.dropped", reports.ToClient->Dropped > 0);
					result.Record("client.discarded", reports.Link->Discarded == 0);
					result.Record("client.completed", reports.Link->Completed > 0);
				}
			),
			Make(
				"lossy-world",
				[](const Reports &reports, CheckResult &result) {
					result.Record("server/client.entities", reports.ClientEntities == reports.ServerEntities);
				}
			),
			Make("discovery", [](const Reports &reports, CheckResult &result) {
				const bool available = reports.Beacon && reports.Directory;
				result.Record("network.beacon/directory.reports", available);
				if (!available) {
					return;
				}
				result.Record("beacon.announcements", reports.Beacon->Announcements > 1);
				result.Record("beacon.undelivered", reports.Beacon->Undelivered == 0);
				result.Record("directory.malformed", reports.Directory->Malformed == 0);
				result.Record("directory.listed", reports.Directory->Listed == 1);
				result.Record(
					"network.beacon/directory.announcements",
					reports.Directory->Listed + reports.Directory->Refreshed == reports.Beacon->Announcements
				);
			})
		};
		std::erase_if(checks, [&](const auto &check) {
			if (check.Name == "content") {
				return arrangement != Arrangement{.Serving = Content::Relayed} || ticks != 90;
			}
			if (check.Name == "discovery") {
				return arrangement != Arrangement{.Finding = Discovery::Advertised} || ticks != 90;
			}
			if (check.Name == "lossy-content" || check.Name == "lossy-world") {
				return arrangement !=
						   Arrangement{.Carrying = Transport::Lossy, .Serving = Content::Relayed} ||
					   ticks != 150;
			}
			return ticks != 90;
		});
		return checks;
	}

	ScenarioReport Run(const Arrangement &arrangement, int ticks = 90) {
		const Settings settings = Small();
		const auto &interpolation = settings.Interpolation;
		std::ostringstream configuration;
		configuration << std::hexfloat << settings.Entities << ';' << settings.TickRate << ';'
					  << settings.FramesPerTick << ';' << settings.Workers << ';'
					  << settings.AnnounceEveryTicks << ';' << settings.ContentFiles << ';'
					  << interpolation.TickRate << ';' << interpolation.DelayTicks << ';'
					  << interpolation.ResyncTicks << ';' << interpolation.CorrectionFraction << ';'
					  << interpolation.ExtrapolateSeconds << ';' << interpolation.UnwindFraction;
		std::ostringstream faults;
		faults << "crossing.default-loss-ordinals.v1";
		for (const uint64_t ordinal : settings.Drop) {
			faults << ';' << ordinal;
		}
		engine::testing::ScenarioKey key;
		key.Scene = settings.ScenePath.empty() ? "crossing.default-luau" : settings.ScenePath;
		key.ContentRevision = "crossing.authored-content.v1";
		key.Configuration = configuration.str();
		key.Seed = 0;
		key.TickSchedule = "join(limit=512);step=" + std::to_string(ticks);
		key.FrameSchedule = "frames-per-tick=" + std::to_string(settings.FramesPerTick);
		key.Actions = "none";
		key.Transport = arrangement.Name();
		key.Faults = faults.str();
		key.CaptureConfiguration = "gather-at-final-native-barrier";
		key.ObservationSchema = "unified.imported-reports.v1";
		key.CheckSet = "unified.crossing.checks.v1";

		std::unique_ptr<Crossing> crossing;
		engine::testing::ScenarioDriver<Reports> driver;
		driver.Prepare = [&](auto &) {
			crossing = std::make_unique<Crossing>(settings, arrangement);
			if (!crossing->Join()) {
				throw std::runtime_error("crossing joining snapshot did not complete");
			}
		};
		driver.Run = [&](auto &session) {
			for (int step = 0; step < ticks; step++) {
				session.Checkpoint();
				crossing->Step();
			}
			Reports reports = crossing->Gather();
			const uint64_t tick = reports.Ticks;
			session.Publish({std::string(REPORT_HOOK), tick, std::move(reports), REPORT_PAYLOAD_BYTES});
		};
		driver.Finish = [&] { crossing.reset(); };
		engine::testing::ScenarioOptions options;
		options.MaximumRetainedSamples = 1;
		options.MaximumRetainedBytes = REPORT_OBSERVATION_BYTES;
		options.MaximumObservationBytes = REPORT_OBSERVATION_BYTES;
		// Completed findings alone survive between named cases. No world or
		// observation is retained; the cap covers the full arrangement product.
		static engine::testing::ScenarioCache<Reports> cache(32);
		return cache.Run(std::move(key), Checks(arrangement, ticks), driver, options);
	}

	// Catch only consumes joined value findings on the calling thread.
	void Check(const ScenarioReport &report, std::string_view name) {
		INFO(
			"scenario " << report.Key.Transport << " " << report.Key.TickSchedule
						<< " reused=" << report.Reused << " setups=" << report.Metrics.SetupCount
						<< " publications=" << report.Metrics.PublishedSamples << " bytes="
						<< report.Metrics.PublishedBytes << " dispatches=" << report.Metrics.CheckDispatches
		);
		for (const auto &failure : report.Failures) {
			INFO(failure);
			CHECK(false);
		}
		REQUIRE(report.Failures.empty());
		const auto check = std::find_if(report.Checks.begin(), report.Checks.end(), [&](const auto &result) {
			return result.Name == name;
		});
		REQUIRE(check != report.Checks.end());
		for (const auto &finding : check->Findings) {
			INFO(check->Name << ": " << finding.Property << " " << finding.Message);
			CHECK(finding.Passed);
		}
		CHECK(check->Passed);
	}
}

TEST_CASE("every arrangement crosses the world and every module agrees", "[unified][matrix]") {
	// **The case this program exists for now.** Every supported arrangement
	// stands the engine up, and the server's world reaches the client, is
	// drawn, and every claim spanning two modules holds. A failure names the
	// arrangement and the two modules, which between them is the whole of
	// where to look.
	for (const Arrangement &arrangement : AllArrangements()) {
		INFO("arrangement " << arrangement.Name());

		const ScenarioReport report = Run(arrangement);
		Check(report, "matrix.world");
		Check(report, "matrix.agreement");
	}
}

TEST_CASE("content published by cdn reaches the client through the server", "[unified][content]") {
	// **The flow that touches four modules and belongs to none of them.**
	// `cdn::Publish` wrote a store, `delivery` fetched routes out of it,
	// `server::ContentRelay` rationed them onto the link and
	// `client::ContentLink` put them back together. Every one of those has a
	// suite; the claim that the bytes the last one holds are the bytes the
	// first one wrote is checkable in exactly one place.
	Check(Run(Arrangement{.Serving = Content::Relayed}), "content");
}

TEST_CASE("a final wire turn delivers content the relay already served", "[unified][content]") {
	// A fixed-duration diagnostic ends after a world tick. The relay's chunks
	// are queued after that tick's wire flush, so they need a bounded transport
	// drain rather than a made-up allowance in the cross-check.
	Crossing crossing(Small(), Arrangement{.Carrying = Transport::Loopback, .Serving = Content::Relayed});
	REQUIRE(crossing.Join());
	Reports before = crossing.Gather();
	for (int turn = 0; turn < 32 && (!before.Relay.has_value() || !before.Link.has_value() ||
									 before.Relay->Served <= before.Link->Completed);
		 turn++) {
		crossing.Step();
		before = crossing.Gather();
	}
	REQUIRE(before.Relay.has_value());
	REQUIRE(before.Link.has_value());
	REQUIRE(before.Relay->Served > before.Link->Completed);
	CHECK_FALSE(CrossCheck(before).empty());

	REQUIRE(crossing.DrainContent());
	const Reports after = crossing.Gather();
	REQUIRE(after.Relay.has_value());
	REQUIRE(after.Link.has_value());
	CHECK(after.Link->Completed == after.Relay->Served);
	CHECK(CrossCheck(after).empty());
}

TEST_CASE("content survives a link that loses datagrams", "[unified][content]") {
	// **The arrangement that is more than the sum of its axes.** A route is
	// several chunks on a reliable, ordered channel; losing one means the
	// pieces behind it arrive with a hole in front of them, and `ContentLink`
	// assembles contiguously on purpose. So this is the case where content and
	// loss interact, and neither `server.contentrelay` nor `client.contentlink`
	// can build it - one has no client and the other has no wire.
	const ScenarioReport report =
		Run(Arrangement{.Carrying = Transport::Lossy, .Serving = Content::Relayed}, 150);
	Check(report, "lossy-content");
	Check(report, "lossy-world");
}

TEST_CASE("an announcement leaves the beacon and lands in the directory", "[unified][discovery]") {
	// `network.beacon` proves an advert encodes and `network.directory` proves
	// one decodes. Neither runs the pair, and the pair is where a version bump
	// on one side and not the other shows up.
	Check(Run(Arrangement{.Finding = Discovery::Advertised}), "discovery");
}

TEST_CASE("a wire stages oversized resources and retries congestion refusals", "[unified][matrix]") {
	// A real link enforces the message limit. A refused snapshot chunk must be
	// returned through `Authority::Unsent` so a busy link cannot leave a hole.
	Settings settings;
	settings.Entities = 512;

	Crossing crossing(settings, Arrangement{.Carrying = Transport::Loopback});
	REQUIRE(crossing.Join());
	const engine::ecs::Entity workspace = crossing.ServerWorld().FindFirstRoot("Workspace");
	const engine::ecs::Entity field = crossing.ServerWorld().FindFirstChild(workspace, "ServerDefault");
	const engine::ecs::Entity cube = crossing.ServerWorld().FindFirstChild(field, "Cube1");
	REQUIRE(cube != engine::ecs::NULL_ENTITY);
	const engine::core::Name velocity("Velocity");
	engine::ecs::AttributeValue original;
	REQUIRE(engine::ecs::GetAttribute(crossing.ClientWorld(), cube, velocity, original));
	REQUIRE(original.Type == engine::ecs::PropertyType::Vector3);
	REQUIRE_FALSE(original.Vector3 == engine::core::Vector3{});
	engine::ecs::AttributeValue stopped;
	stopped.Type = engine::ecs::PropertyType::Vector3;
	stopped.Vector3 = {};
	REQUIRE(engine::ecs::SetAttribute(crossing.ServerWorld(), cube, velocity, stopped));
	for (int step = 0; step < 60; step++) {
		crossing.Step();
	}
	engine::ecs::AttributeValue received;
	REQUIRE(engine::ecs::GetAttribute(crossing.ClientWorld(), cube, velocity, received));
	CHECK(received.Type == engine::ecs::PropertyType::Vector3);
	CHECK(received.Vector3 == stopped.Vector3);

	const Reports reports = crossing.Gather();
	CHECK(reports.LargestMessage > 0);
	CHECK(reports.LargestMessage <= engine::net::Packet::MAXIMUM_MESSAGE_BYTES);
	// Velocity attributes form a large resource. It must take the authority's
	// chunked overlay path while every emitted datagram remains within the limit.
	CHECK(reports.Authority.Oversized > 0);
	REQUIRE(reports.ServerLink.has_value());
	// The path may temporarily withhold allowance. Refusals from that limit
	// are retried, while configured-budget refusals would hide an unsendable
	// resource or a packet rate this scene cannot sustain.
	CHECK(reports.ServerLink->SendsOverBudget == 0);
	CHECK(reports.Refused <= reports.ServerLink->SendsOverAllowance);
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
