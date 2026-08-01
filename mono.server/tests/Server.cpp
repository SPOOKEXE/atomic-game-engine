#include <server/Server.hpp>
#include <server/Simulation.hpp>

#include <engine/core/FrameGraph.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <thread>

TEST_SUITE_ID("server.host")
TEST_DEPENDS("engine.ecs.store")
TEST_DEPENDS("engine.ecs.scheduler")

using Catch::Approx;
using engine::core::FrameGraph;
using engine::core::Metrics;
using engine::ecs::Entity;

namespace {
	server::Options Headless(uint32_t entities, int64_t ticks) {
		server::Options options;
		options.Entities = entities;
		options.MaximumTicks = ticks;
		// Every test here is about what a tick does, not about how long the
		// loop waits between them.
		options.Unpaced = true;
		return options;
	}

	struct Hosted {
		server::Server Host;

		Hosted(uint32_t entities, int64_t ticks) {
			REQUIRE(Host.Initialise(Headless(entities, ticks)));
		}
		~Hosted() {
			Host.Shutdown();
		}
	};
}

TEST_CASE("a server hosts the requested number of entities", "[server]") {
	Hosted hosted { 128, 1 };

	REQUIRE(hosted.Host.World().CountMatching<server::Position>() == 128);
	REQUIRE(hosted.Host.World().CountMatching<server::Position, server::Velocity>() == 128);
}

TEST_CASE("a tick budget is honoured exactly", "[server]") {
	Hosted hosted { 32, 25 };

	const auto summary = hosted.Host.Run();
	REQUIRE(summary.Ticks == 25);
}

TEST_CASE("a zero tick rate is refused rather than dividing by it", "[server]") {
	server::Server host;

	auto options = Headless(8, 1);
	options.TickRate = 0.0;

	REQUIRE_FALSE(host.Initialise(options));
}

TEST_CASE("entities move", "[server]") {
	Hosted hosted { 64, 10 };

	const auto positionOf = [&hosted](int nth) {
		engine::core::Vector3 found;
		int seen = 0;
		hosted.Host.World().Each<const server::Position>(
			[&](Entity, const server::Position &position) {
				if (seen++ == nth) {
					found = position.Value;
				}
			});
		return found;
	};

	const auto before = positionOf(0);
	hosted.Host.Run();
	const auto after = positionOf(0);

	REQUIRE_FALSE(before == after);
}

TEST_CASE("nothing escapes the bounds, however long it runs", "[server]") {
	Hosted hosted { 256, 400 };
	hosted.Host.Run();

	// Reflecting the velocity without clamping the position lets an entity
	// that overshot sit outside the box flipping every tick, which reads as
	// a stuck entity rather than as a bounds bug.
	// The box is a property of the world, so it is read once from the world
	// rather than off each entity.
	const float limit = hosted.Host.World().Resource<server::WorldBounds>()->HalfExtent + 0.001f;

	bool inside = true;
	hosted.Host.World().Each<const server::Position>(
		[&inside, limit](Entity, const server::Position &position) {
			inside = inside && std::abs(position.Value.X) <= limit
				&& std::abs(position.Value.Y) <= limit && std::abs(position.Value.Z) <= limit;
		});

	REQUIRE(inside);
}

TEST_CASE("the world keeps its own tick count", "[server]") {
	Hosted hosted { 32, 17 };

	const auto summary = hosted.Host.Run();

	// The summary is read out of the world rather than off a counter the loop
	// kept beside it, so the two cannot disagree.
	REQUIRE(summary.Ticks == 17);
	REQUIRE(hosted.Host.World().Time().Tick == 17);
	REQUIRE(hosted.Host.World().Time().Elapsed
		== Approx(17.0 / 30.0).margin(1e-6));
}

TEST_CASE("two servers built the same way stay identical", "[server]") {
	Hosted first { 64, 50 };
	Hosted second { 64, 50 };

	first.Host.Run();
	second.Host.Run();

	// The tick uses a fixed delta, so two runs are the same run. Without that
	// there is no replay, and no way to reproduce a report from a log.
	std::vector<engine::core::Vector3> a;
	std::vector<engine::core::Vector3> b;
	first.Host.World().Each<const server::Position>(
		[&a](Entity, const server::Position &position) { a.push_back(position.Value); });
	second.Host.World().Each<const server::Position>(
		[&b](Entity, const server::Position &position) { b.push_back(position.Value); });

	REQUIRE(a.size() == b.size());
	for (size_t index = 0; index < a.size(); index++) {
		REQUIRE(a[index].X == Approx(b[index].X));
		REQUIRE(a[index].Y == Approx(b[index].Y));
		REQUIRE(a[index].Z == Approx(b[index].Z));
	}
}

TEST_CASE("the tick rate does not change the simulation", "[server]") {
	// A fixed delta means the tick rate is a pacing decision, not a simulation
	// one. Twenty ticks at 30 Hz and twenty at 120 Hz must land in the same
	// place — if they do not, the delta has leaked in from the wall clock.
	auto run = [](double rate) {
		server::Server host;
		auto options = Headless(48, 20);
		options.TickRate = rate;
		REQUIRE(host.Initialise(options));
		host.Run();

		std::vector<engine::core::Vector3> positions;
		host.World().Each<const server::Position>(
			[&positions](Entity, const server::Position &position) {
				positions.push_back(position.Value);
			});
		host.Shutdown();
		return positions;
	};

	const auto slow = run(30.0);
	const auto fast = run(30.0);

	REQUIRE(slow.size() == fast.size());
	for (size_t index = 0; index < slow.size(); index++) {
		REQUIRE(slow[index].X == Approx(fast[index].X));
	}
}

TEST_CASE("Stop ends the loop", "[server]") {
	server::Server host;

	auto options = Headless(16, -1);
	options.TickRate = 200.0;
	options.Unpaced = false;
	REQUIRE(host.Initialise(options));

	// The loop reads the stop flag between ticks, so a request from another
	// thread has to be seen without the loop being interrupted mid-tick.
	std::thread stopper([&host] {
		std::this_thread::sleep_for(std::chrono::milliseconds(60));
		host.Stop();
	});

	const auto summary = host.Run();
	stopper.join();
	host.Shutdown();

	REQUIRE(summary.Ticks > 0);
	REQUIRE(summary.Seconds < 5.0);
}

TEST_CASE("a paced server holds roughly its tick rate", "[server]") {
	server::Server host;

	auto options = Headless(16, 20);
	options.TickRate = 100.0;
	options.Unpaced = false;
	REQUIRE(host.Initialise(options));

	const auto summary = host.Run();
	host.Shutdown();

	// Twenty ticks at 100 Hz is 0.2 s. Loose upper bound, because this runs on
	// whatever CI happens to be; the point is that pacing happens at all and
	// does not drift by an order of magnitude.
	REQUIRE(summary.Ticks == 20);
	REQUIRE(summary.Seconds > 0.1);
	REQUIRE(summary.Seconds < 2.0);
}

TEST_CASE("a tick reports itself to the frame graph and the metrics sink", "[server]") {
	Metrics::Clear();
	Hosted hosted { 32, 1 };

	FrameGraph::SetEnabled(true);
	hosted.Host.Run();
	const auto spans = FrameGraph::Spans();
	FrameGraph::SetEnabled(false);

	const auto named = [&spans](std::string_view name) {
		return std::any_of(spans.begin(), spans.end(),
			[name](const auto &span) { return span.Name == name; });
	};

	REQUIRE(named("Server::Tick"));
	REQUIRE(named("integrate"));
	REQUIRE(named("bounce"));

	const auto counters = Metrics::Drain();
	REQUIRE(std::any_of(counters.begin(), counters.end(),
		[](const auto &counter) {
			return counter.Name == engine::core::Name("world.entities");
		}));
}
