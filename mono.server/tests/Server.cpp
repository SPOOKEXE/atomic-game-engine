#include <engine/core/FrameGraph.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/scene/Attachments.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <server/Server.hpp>
#include <server/Simulation.hpp>
#include <thread>
#include <vector>

TEST_SUITE_ID("server.host")
TEST_DEPENDS("engine.ecs.store")
TEST_DEPENDS("engine.ecs.scheduler")
TEST_DEPENDS("engine.scene.components")
TEST_DEPENDS("engine.scene.attachments")
// The world's settings are what the options are copied into, and the rates that
// arrived there are read straight back out.
TEST_DEPENDS("engine.world.universe")

using Catch::Approx;
using engine::core::CFrame;
using engine::core::FrameGraph;
using engine::core::Metrics;
using engine::core::Vector3;
using engine::ecs::Entity;
using engine::ecs::Phase;
using engine::ecs::Scheduler;
using engine::ecs::Store;
using engine::scene::Attachment;
using engine::scene::Motion;
using engine::scene::Transform;
using engine::scene::WorldBounds;

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
	Hosted hosted{128, 1};

	size_t positions = 0;
	size_t moving = 0;
	hosted.Host.Enter([&](Store &store) {
		positions = store.CountMatching<Transform>();
		moving = store.CountMatching<Transform, Motion>();
	});
	REQUIRE(positions == 128);
	REQUIRE(moving == 128);
}

TEST_CASE("a tick budget is honoured exactly", "[server]") {
	Hosted hosted{32, 25};

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
	Hosted hosted{64, 10};

	const auto positionOf = [&hosted](int nth) {
		engine::core::Vector3 found;
		int seen = 0;
		hosted.Host.Enter([&](Store &store) {
			store.Each<const Transform>([&](Entity, const Transform &transform) {
				if (seen++ == nth) {
					found = transform.Frame.Position;
				}
			});
		});
		return found;
	};

	const auto before = positionOf(0);
	hosted.Host.Run();
	const auto after = positionOf(0);

	REQUIRE_FALSE(before == after);
}

TEST_CASE("nothing escapes the bounds, however long it runs", "[server]") {
	Hosted hosted{256, 400};
	hosted.Host.Run();

	// Reflecting the velocity without clamping the position lets an entity
	// that overshot sit outside the box flipping every tick, which reads as
	// a stuck entity rather than as a bounds bug.
	// The box is a property of the world, so it is read once from the world
	// rather than off each entity.
	bool inside = true;
	hosted.Host.Enter([&inside](Store &store) {
		const float limit = store.Resource<WorldBounds>()->HalfExtent + 0.001f;

		store.Each<const Transform>([&inside, limit](Entity, const Transform &transform) {
			const engine::core::Vector3 &position = transform.Frame.Position;
			inside = inside && std::abs(position.X) <= limit && std::abs(position.Y) <= limit &&
					 std::abs(position.Z) <= limit;
		});
	});

	REQUIRE(inside);
}

TEST_CASE("the world keeps its own tick count", "[server]") {
	Hosted hosted{32, 17};

	const auto summary = hosted.Host.Run();

	// The summary is read out of the world rather than off a counter the loop
	// kept beside it, so the two cannot disagree.
	REQUIRE(summary.Ticks == 17);
	hosted.Host.Enter([](Store &store) {
		REQUIRE(store.Time().Tick == 17);
		REQUIRE(store.Time().Elapsed == Approx(17.0 / 30.0).margin(1e-6));
	});
}

TEST_CASE("two servers built the same way stay identical", "[server]") {
	Hosted first{64, 50};
	Hosted second{64, 50};

	first.Host.Run();
	second.Host.Run();

	// The tick uses a fixed delta, so two runs are the same run. Without that
	// there is no replay, and no way to reproduce a report from a log.
	std::vector<engine::core::Vector3> a;
	std::vector<engine::core::Vector3> b;
	first.Host.Enter([&a](Store &store) {
		store.Each<const Transform>([&a](Entity, const Transform &transform) {
			a.push_back(transform.Frame.Position);
		});
	});
	second.Host.Enter([&b](Store &store) {
		store.Each<const Transform>([&b](Entity, const Transform &transform) {
			b.push_back(transform.Frame.Position);
		});
	});

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
	// place - if they do not, the delta has leaked in from the wall clock.
	auto run = [](double rate) {
		server::Server host;
		auto options = Headless(48, 20);
		options.TickRate = rate;
		REQUIRE(host.Initialise(options));
		host.Run();

		std::vector<engine::core::Vector3> positions;
		host.Enter([&positions](Store &store) {
			store.Each<const Transform>([&positions](Entity, const Transform &transform) {
				positions.push_back(transform.Frame.Position);
			});
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
	Hosted hosted{32, 1};

	FrameGraph::SetEnabled(true);
	hosted.Host.Run();
	const auto spans = FrameGraph::Spans();
	FrameGraph::SetEnabled(false);

	const auto named = [&spans](std::string_view name) {
		return std::any_of(spans.begin(), spans.end(), [name](const auto &span) {
			return span.Name == name;
		});
	};

	// The server no longer has a tick span of its own: the universe drives the
	// barrier and whichever branch it took names itself.
	//
	// **One world ticks on the driver's own thread**, so its spans are on the
	// frame's owning thread and are kept. `Universe::Tick` explains why: a
	// `Jobs::ForWorkers` batch owns the process-wide pool, so handing a lone
	// world to a lane makes every parallel loop *inside* it run inline while the
	// rest of the pool waits - which is no concurrency bought at the price of
	// all of it.
	REQUIRE(named("Universe::Tick"));
	REQUIRE(named("worlds (driver)"));
	REQUIRE_FALSE(named("worlds (serial)"));
	REQUIRE_FALSE(named("worlds (pinned workers)"));

	const auto counters = Metrics::Drain();
	REQUIRE(std::any_of(counters.begin(), counters.end(), [](const auto &counter) {
		return counter.Name == engine::core::Name("world.entities");
	}));
}

// --- recording ------------------------------------------------------------

TEST_CASE("a recorded run replays to the same state", "[server]") {
	// The determinism guarantee end to end, through the program's own options
	// rather than through the engine API: record a run, replay it, and compare
	// every entity. Same binary, same machine - which is what `v02v03.md`
	// decision 8 promises and all it promises.
	const std::filesystem::path recording = std::filesystem::temp_directory_path() / "mono-server-replay.rec";

	const auto positionsOf = [](server::Server &host) {
		std::vector<engine::core::Vector3> found;
		host.Enter([&found](Store &store) {
			store.Each<const Transform>([&found](Entity, const Transform &transform) {
				found.push_back(transform.Frame.Position);
			});
		});
		return found;
	};

	std::vector<engine::core::Vector3> live;
	{
		server::Server host;
		auto options = Headless(64, 30);
		options.RecordPath = recording;

		REQUIRE(host.Initialise(options));
		REQUIRE(host.Run().Ticks == 30);
		live = positionsOf(host);
		host.Shutdown(); // writes the recording
	}

	REQUIRE(std::filesystem::exists(recording));

	std::vector<engine::core::Vector3> replayed;
	{
		server::Server host;
		server::Options options;
		options.ReplayPath = recording;

		REQUIRE(host.Initialise(options));
		REQUIRE(host.Run().Ticks == 30);
		replayed = positionsOf(host);
		host.Shutdown();
	}

	REQUIRE(live.size() == replayed.size());
	REQUIRE_FALSE(live.empty());

	size_t drifted = 0;
	for (size_t index = 0; index < live.size(); index++) {
		if (!(live[index] == replayed[index])) {
			drifted++;
		}
	}
	REQUIRE(drifted == 0);

	std::filesystem::remove(recording);
}

TEST_CASE("recording a replay reproduces the recording it replayed", "[server]") {
	// The strongest statement the replay path can make about itself, and the
	// one `just replay-check` runs in CI. Comparing *positions* after a replay
	// says the simulation agreed; comparing the two recordings byte for byte
	// says the snapshot, the frame times and every envelope agreed too - which
	// is what a supervisor restoring a crashed host is relying on.
	const auto directory = std::filesystem::temp_directory_path();
	const std::filesystem::path source = directory / "mono-server-replay-source.rec";
	const std::filesystem::path again = directory / "mono-server-replay-again.rec";
	std::filesystem::remove(source);
	std::filesystem::remove(again);

	{
		server::Server host;
		auto options = Headless(48, 40);
		options.RecordPath = source;
		REQUIRE(host.Initialise(options));
		REQUIRE(host.Run().Ticks == 40);
		host.Shutdown();
	}
	REQUIRE(std::filesystem::exists(source));

	{
		server::Server host;
		server::Options options;
		options.ReplayPath = source;
		options.RecordPath = again;
		REQUIRE(host.Initialise(options));
		REQUIRE(host.Run().Ticks == 40);
		host.Shutdown();
	}

	// The flag combination used to be accepted and ignored, so the file
	// existing is worth asserting separately from its contents.
	REQUIRE(std::filesystem::exists(again));

	const auto read = [](const std::filesystem::path &path) {
		std::ifstream stream(path, std::ios::binary);
		return std::vector<char>(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
	};
	REQUIRE(read(source) == read(again));

	std::filesystem::remove(source);
	std::filesystem::remove(again);
}

TEST_CASE("a replay of something that is not a recording is refused", "[server]") {
	const std::filesystem::path rubbish = std::filesystem::temp_directory_path() / "mono-server-rubbish.rec";

	{
		std::ofstream file(rubbish, std::ios::binary);
		file << "this is not a recording";
	}

	server::Server host;
	server::Options options;
	options.ReplayPath = rubbish;

	REQUIRE_FALSE(host.Initialise(options));
	host.Shutdown();

	std::filesystem::remove(rubbish);
}

TEST_CASE("a replay of a file that does not exist is refused", "[server]") {
	server::Server host;
	server::Options options;
	options.ReplayPath = "/definitely/not/a/recording/anywhere.rec";

	REQUIRE_FALSE(host.Initialise(options));
	host.Shutdown();
}

TEST_CASE("the configured rates reach the world this program creates", "[server]") {
	// **The wiring, not the behaviour.** What a physics rate and a replication
	// rate *do* is `engine.physics.clock` and `engine.world.universe`; what
	// this catches is the flag parsed into a field nothing read, which is the
	// failure `Replication.cpp` opens by naming.
	server::Server host;

	auto options = Headless(8, 1);
	options.PhysicsTickRate = 20.0;
	options.ReplicationTickRate = 10.0;
	REQUIRE(host.Initialise(options));

	const engine::world::WorldId primary = host.Worlds().Worlds().front();
	const engine::world::WorldSettings settings = host.Worlds().SettingsOf(primary);
	CHECK(settings.PhysicsTickRate == Approx(20.0));
	CHECK(settings.ReplicationTickRate == Approx(10.0));

	host.Shutdown();
}

TEST_CASE("a world with no rates configured keeps following its tick", "[server]") {
	Hosted hosted{8, 1};

	const engine::world::WorldId primary = hosted.Host.Worlds().Worlds().front();
	const engine::world::WorldSettings settings = hosted.Host.Worlds().SettingsOf(primary);

	// Zero, which is "step physics on every tick" and "publish on every tick" -
	// what this program did before either rate existed.
	CHECK(settings.PhysicsTickRate == 0.0);
	CHECK(settings.ReplicationTickRate == 0.0);
}

// --- what a prepared world derives ------------------------------------------

TEST_CASE("a prepared world resolves its attachments", "[server]") {
	// **The authority derives the derived half too.** Until v0.19 only
	// `mono.client`'s presentation registered `ResolveAttachments`, so an
	// attachment on a dedicated server kept the identity for the whole run - and
	// the visible half of that was not the cached frame, which nothing here
	// reads, but the *signal*: the pass reports its write, and that report is
	// what makes `Attachment.WorldCFrame` fire `.Changed` for a server script.
	// Reproduced with a scene script before it was fixed: zero signals over
	// twenty ticks of a part moving one stud each.
	server::RegisterPlaceholderComponents();
	engine::scene::RegisterSceneClasses();

	Store store("server_test.attachments");
	Scheduler scheduler;
	server::PrepareSimulation(store, scheduler, 0.0);

	const Entity post = store.CreateInstance(engine::scene::PartClass(), "Post");
	store.GetMutable<Transform>(post)->Frame = CFrame(Vector3(4.0f, 0.0f, 0.0f));

	const Entity point = store.CreateInstance(engine::scene::AttachmentClass(), "Top");
	REQUIRE(store.SetParent(point, post));
	store.GetMutable<Attachment>(point)->Frame = CFrame(Vector3(0.0f, 3.0f, 0.0f));

	std::vector<Entity> heard;
	store.OnChanged<Attachment>([&heard](Store &, Entity moved, const Attachment &) {
		heard.push_back(moved);
	});

	store.ClearChanges();
	store.AdvanceTick(1.0f / 30.0f);
	scheduler.RunPhases(store, Phase::PreSimulation, Phase::PostSimulation);
	REQUIRE(store.FlushSignals() == 1);

	// `PostSimulation`, so the frame stored is the one this tick ended at rather
	// than the one it started from.
	const Attachment *resolved = store.Get<Attachment>(point);
	REQUIRE(resolved != nullptr);
	CHECK(resolved->WorldFrame.Position.X == Approx(4.0f));
	CHECK(resolved->WorldFrame.Position.Y == Approx(3.0f));
	CHECK(heard == std::vector<Entity>{point});
}
