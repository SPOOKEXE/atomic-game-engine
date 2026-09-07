#include <engine/core/FrameGraph.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Paths.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/game/Play.hpp>
#include <engine/game/PortalSession.hpp>
#include <engine/replication/Connector.hpp>
#include <engine/scene/Attachments.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Controls.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

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

TEST_CASE("a thousand worlds leave main for presentation and use every worker core", "[server]") {
	const server::WorldProcessPlan plan = server::PlanWorldProcesses(1000, 12);
	REQUIRE(plan.Processes == 12);
	REQUIRE(plan.LocalWorlds == 0);
	REQUIRE(plan.RemoteHosts == 11);
}

TEST_CASE("world process placement stays within worlds and cores", "[server]") {
	CHECK(server::PlanWorldProcesses(1, 12).Processes == 1);
	CHECK(server::PlanWorldProcesses(2, 12).Processes == 3);
	CHECK(server::PlanWorldProcesses(1000, 1).Processes == 1);
	CHECK(server::PlanWorldProcesses(1000, 0).Processes == 1);
	CHECK(server::PlanWorldProcesses(1000, 12, 4).Processes == 4);
	CHECK(server::PlanWorldProcesses(1000, 12, 1).LocalWorlds == 1000);
}

TEST_CASE("diagnostic output paths select main and child processes", "[server]") {
	const std::filesystem::path main = "/tmp/rings.folded";
	CHECK(server::ProcessOutputPath(main, 0) == main);
	CHECK(server::ProcessOutputPath(main, 1) == "/tmp/rings.process1.folded");
	CHECK(server::ProcessOutputPath(main, 12) == "/tmp/rings.process12.folded");
}

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

	std::vector<std::byte> Bytes(std::string_view text) {
		const auto *first = reinterpret_cast<const std::byte *>(text.data());
		return {first, first + text.size()};
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

TEST_CASE("mock and live datastores persist into separate server directories", "[server][datastore]") {
	const std::filesystem::path root =
		std::filesystem::temp_directory_path() / "atomic-server-datastore-environments";
	std::error_code ignored;
	std::filesystem::remove_all(root, ignored);

	auto options = Headless(1, 0);
	options.DataStoreRoot = root;
	options.DataStoreEnvironment = engine::world::SharedStoreEnvironment::Mock;
	{
		server::Server host;
		REQUIRE(host.Initialise(options));
		REQUIRE(
			host.Worlds().SetSharedStoreValue(
				engine::world::BusKind::DataStore, engine::core::Name("player:1"), Bytes("mock-value")
			) == engine::world::BusStatus::Ok
		);
		host.Shutdown();
	}
	CHECK(std::filesystem::exists(root / "mock" / "datastore.bin"));
	CHECK_FALSE(std::filesystem::exists(root / "live" / "datastore.bin"));

	{
		server::Server restored;
		REQUIRE(restored.Initialise(options));
		const auto records = restored.Worlds().SharedStoreEntries(engine::world::BusKind::DataStore);
		REQUIRE(records.size() == 1);
		CHECK(records[0].Value == Bytes("mock-value"));
		restored.Shutdown();
	}

	options.DataStoreEnvironment = engine::world::SharedStoreEnvironment::Live;
	{
		server::Server live;
		REQUIRE(live.Initialise(options));
		CHECK(live.Worlds().SharedStoreEntries(engine::world::BusKind::DataStore).empty());
		live.Shutdown();
	}

	std::filesystem::remove_all(root, ignored);
}

TEST_CASE("a malformed datastore blocks startup and survives cleanup", "[server][datastore]") {
	const std::filesystem::path root =
		std::filesystem::temp_directory_path() / "atomic-server-malformed-datastore";
	const std::filesystem::path file = root / "mock" / "datastore.bin";
	std::error_code ignored;
	std::filesystem::remove_all(root, ignored);
	std::filesystem::create_directories(file.parent_path());
	{
		std::ofstream output(file, std::ios::binary);
		output << "malformed";
	}

	auto options = Headless(1, 0);
	options.DataStoreRoot = root;
	options.DataStoreEnvironment = engine::world::SharedStoreEnvironment::Mock;
	server::Server host;
	CHECK_FALSE(host.Initialise(options));
	host.Shutdown();

	std::ifstream input(file, std::ios::binary);
	const std::string contents{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
	CHECK(contents == "malformed");
	std::filesystem::remove_all(root, ignored);
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

TEST_CASE(
	"failed presentation children retry without a rapid launch loop", "[server][presentation-restart]"
) {
	Metrics::Clear();
	server::Server host;
	auto options = Headless(8, 400);
	options.TickRate = 100;
	options.Unpaced = false;
	options.Listening = true;
	options.Transport = engine::net::WireMode::Datagram;
	options.IdentityKey.assign(64, 'a');
	// The server binary refuses the producer's Client arguments and exits.
	options.PresentationProgram =
		engine::core::Paths::Base().parent_path() / "server" / engine::core::Paths::Program("server");
	REQUIRE(host.Initialise(options));
	const auto summary = host.Run();
	host.Shutdown();
	CHECK(summary.Ticks == 400);
	const auto counters = Metrics::Snapshot().Counters;
	const auto total = [&](std::string_view name) {
		const auto found = std::find_if(counters.begin(), counters.end(), [&](const auto &counter) {
			return counter.Name.Text() == name;
		});
		return found == counters.end() ? 0.0 : found->Value;
	};
	CHECK(total("server.presentation.producer-starts") >= 2);
	CHECK(total("server.presentation.producer-starts") <= 3);
	CHECK(total("server.presentation.producer-retries") >= 1);
	CHECK(total("server.presentation.producer-retries") <= 2);
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
	// every entity. Same binary, same machine - which is all the guarantee
	// ever promised.
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
	const bool listening = GENERATE(false, true);
	CAPTURE(listening);
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
		options.Listening = listening;
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

TEST_CASE(
	"player admission reply survives send backpressure", "[server][portal-session][admission-pressure]"
) {
	using namespace engine;
	const bool hasGame = GENERATE(false, true);
	const bool replace = GENERATE(false, true);
	CAPTURE(hasGame, replace);
	auto options = Headless(0, 3);
	options.Listening = true;
	options.Transport = net::WireMode::Datagram;
	const auto path = core::Paths::Base() / "admission-pressure.luau";
	if (hasGame) {
		std::ofstream source(path);
		REQUIRE(source);
		source << "local floor = Instance.new('Part')\n"
			   << "floor.Anchored = true\n"
			   << "floor.Size = Vector3.new(100, 1, 100)\n"
			   << "floor.Parent = workspace\n";
		options.GamePath = path.string();
	}
	server::Server host;
	REQUIRE(host.Initialise(options));
	if (hasGame) {
		std::error_code ignored;
		std::filesystem::remove(path, ignored);
	}
	auto socket = net::MakeUdpTransport(0);
	REQUIRE(socket != nullptr);
	replication::ConnectorSettings settings;
	settings.Advertised = net::WireMode::Datagram;
	double now = core::Clock::Seconds();
	replication::Connector client(
		*socket, net::Endpoint::LoopbackIPv4(host.ListeningOn().Port), now, settings
	);
	Store replica("admission-pressure");
	std::vector<game::PortalSessionMessage> replies;
	client.OnUserMessage([&](std::span<const std::byte> bytes) {
		game::PortalSessionMessage reply;
		if (game::DecodePortalSession(bytes, reply)) replies.push_back(std::move(reply));
	});
	for (int tick = 0; tick < 100 && !client.Admitted(); tick++) {
		now = core::Clock::Seconds();
		client.Poll(replica, now);
		host.Clients()->Poll(now);
		host.Clients()->Advance(now);
		host.Clients()->Flush(now);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	REQUIRE(client.Admitted());

	const std::array<std::byte, 1> filler{std::byte{0}};
	size_t sent = 0;
	while (sent < 128 && host.Clients()->Broadcast(filler, now) != 0)
		sent++;
	REQUIRE(sent > 0);
	REQUIRE(sent < 128);
	game::PortalSessionMessage request;
	request.Attempt = 17;
	REQUIRE(client.SendUser(game::EncodePortalSession(request), now));
	if (replace) {
		request.Attempt = 18;
		REQUIRE(client.SendUser(game::EncodePortalSession(request), now));
	}
	host.Clients()->Poll(now);
	CHECK(replies.empty());

	// Drain the old replies and acknowledge them before the host retries.
	client.Poll(replica, core::Clock::Seconds());
	CHECK(replies.empty());
	REQUIRE(host.Run().Ticks == 3);
	client.Poll(replica, core::Clock::Seconds());
	REQUIRE(replies.size() == 1);
	CHECK(replies.front().Attempt == request.Attempt);
	if (hasGame) {
		CHECK(replies.front().Kind == game::PortalSessionKind::Ready);
		REQUIRE(host.Enter([&](Store &store) {
			CHECK(store.Alive(replies.front().Player));
			size_t players = 0;
			store.EachEntity([&](Entity entity) {
				if (store.IsA(entity, scene::PlayerClass())) players++;
			});
			CHECK(players == 1);
		}));
	} else {
		CHECK(replies.front().Kind == game::PortalSessionKind::Refused);
		CHECK_FALSE(replies.front().Diagnostic.empty());
	}
	host.Shutdown();
}

TEST_CASE(
	"source portal admission holds the rig until client acceptance", "[server][portal-session][source-lease]"
) {
	using namespace engine;
	const int outcome = GENERATE(0, 1, 2, 3, 4, 5);
	bool discardTransferReplies = false;
	size_t droppedTransferReplies = 0;
	const bool refuse = outcome == 1 || outcome == 4;
	const float walkingDirection = outcome == 0 ? GENERATE(-1.f, 1.f) : -1.f;
	CAPTURE(outcome, walkingDirection);
	auto options = Headless(0, 1);
	if (outcome == 0) {
		const auto path = core::Paths::Base() / "portal-source-walk.luau";
		std::ofstream script(path);
		script << "return\n";
		script.close();
		REQUIRE(script.good());
		options.GamePath = path.string();
	}
	options.Listening = true;
	options.Transport = net::WireMode::Datagram;
	server::Server host;
	REQUIRE(host.Initialise(options));
	scene::RegisterSceneClasses();
	script::RegisterPortalTransferComponents();
	host.Worlds().Enter(host.Primary(), [&](Store &store, Scheduler &scheduler) {
		scene::InstallServices(store);
		REQUIRE(script::PortalTransferIncarnation(store) != 0);
		CHECK(scheduler.HasSystem("portal.transfer.pump", Phase::PreSimulation));
		if (outcome == 5)
			scheduler.Add(
				"test.discard-destination-cancellation",
				Phase::PreSimulation,
				[&](Store &source) {
					if (!discardTransferReplies) return;
					if (auto *inbox = source.ResourceMutable<world::Inbox>())
						droppedTransferReplies += std::erase_if(inbox->Arrived, [](const auto &delivery) {
							return delivery.Key.Text() == "engine.portal.transfer" &&
								   !delivery.Reply.Expected();
						});
				},
				ecs::SystemOrder{{"portal.transfer.pump"}, {}}
			);
	});
	world::WorldSettings destinationSettings;
	destinationSettings.Name = core::Name("source-lease-destination");
	const auto destination = host.Worlds().Create(destinationSettings);
	REQUIRE(destination.IsValid());
	host.Worlds().Enter(destination, [&](Store &store, Scheduler &scheduler) {
		scene::InstallServices(store);
		REQUIRE(script::ConfigurePortalTransfers(store, 202));
		script::RegisterTeleportAdmission(scheduler);
		if (outcome == 0) server::PrepareSimulation(store, scheduler, options.PhysicsTickRate);
	});
	const auto endpoint = host.Worlds().OpenPresentation(destination, core::Name("portal-sessions"));
	REQUIRE(endpoint.Status == world::PresentationStatus::Ok);
	std::array<std::byte, assets::SigningKey::SEED_BYTES> seed;
	seed.fill(std::byte{19});
	const auto identity = assets::SigningKey::FromSeed(seed);
	REQUIRE(identity.has_value());
	auto socket = net::MakeUdpTransport(0);
	REQUIRE(socket != nullptr);
	replication::ConnectorSettings connecting;
	connecting.Advertised = net::WireMode::Datagram;
	connecting.ClientIdentity = &*identity;
	double now = core::Clock::Seconds();
	replication::Connector client(
		*socket, net::Endpoint::LoopbackIPv4(host.ListeningOn().Port), now, connecting
	);
	Store replica("source-lease-replica");
	std::vector<game::PortalSessionMessage> replies;
	std::vector<game::PortalSessionMessage> motionReplies;
	client.OnUserMessage([&](std::span<const std::byte> bytes) {
		game::PortalSessionMessage message;
		if (!game::DecodePortalSession(bytes, message)) return;
		if (message.Kind == game::PortalSessionKind::Motion)
			motionReplies.push_back(std::move(message));
		else
			replies.push_back(std::move(message));
	});
	const auto poll = [&] {
		now = core::Clock::Seconds();
		client.Poll(replica, now);
		client.Advance(now);
		host.Clients()->Poll(now);
		host.Clients()->Advance(now);
		host.Clients()->Flush(now);
		client.Poll(replica, now);
	};
	for (int tick = 0; tick < 100 && !client.Admitted(); tick++) {
		poll();
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	REQUIRE(client.Admitted());
	const auto send = [&](const game::PortalSessionMessage &message) {
		const auto bytes = game::EncodePortalSession(message);
		REQUIRE_FALSE(bytes.empty());
		bool sent = false;
		for (int tick = 0; tick < 1000 && !sent; tick++) {
			poll();
			sent = client.SendUser(bytes, now);
			if (!sent) std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
		REQUIRE(sent);
	};
	game::PortalSessionMessage fresh;
	fresh.Attempt = 100;
	send(fresh);
	for (int tick = 0; tick < 1000 && replies.empty(); tick++)
		poll();
	REQUIRE(replies.size() == 1);
	REQUIRE(replies.front().Kind == game::PortalSessionKind::Ready);
	const auto original = replies.front().Player;
	for (int tick = 0; tick < 3; tick++)
		(void)host.Run();
	script::PortalTransferId transfer;
	Entity walkingRoot;
	float startingZ = 0;
	REQUIRE(host.Enter([&](Store &store) {
		REQUIRE(scene::CharacterOf(store, original) != ecs::NULL_ENTITY);
		if (outcome == 0) {
			const auto rig = *store.Get<scene::Character>(scene::CharacterOf(store, original));
			walkingRoot = rig.Root;
			store.GetMutable<scene::Humanoid>(rig.Humanoid)->Health = 23;
			const auto start = store.Get<scene::Transform>(walkingRoot)->Frame.Position;
			startingZ = start.Z;
			scene::PartDesc part;
			part.Frame = core::CFrame({0, -1, 0});
			part.Size = {100, 2, 100};
			REQUIRE(store.SetParent(scene::MakePart(store, part), scene::WorkspaceOf(store)));
			part.Frame = core::CFrame({start.X, 3, start.Z + walkingDirection * 3});
			part.Size = {10, 10, .4f};
			const auto pane = scene::MakePart(store, part);
			REQUIRE(store.SetParent(pane, scene::WorkspaceOf(store)));
			part.Frame.Position.Z -= 30;
			const auto beyond = scene::MakePart(store, part);
			REQUIRE(store.SetParent(beyond, scene::WorkspaceOf(store)));
			const auto portal = store.CreateInstance(ecs::Classes::Find(core::Name("Portal")), "walk");
			REQUIRE(store.SetParent(portal, pane));
			scene::Portal link{beyond};
			link.DestinationWorld = core::Name(endpoint.Address.World);
			store.Set(portal, link);
			return;
		}
		std::string failure;
		REQUIRE(
			script::BeginPortalTransfer(
				store, original, endpoint.Address.World, {core::CFrame({0, 20, 0}), {}, 1}, transfer, failure
			)
		);
	}));
	if (outcome == 0) {
		const auto move = game::EncodeMoveInput({{0, 0, walkingDirection}, false});
		bool crossed = false;
		for (int tick = 0; tick < 180 && !crossed; ++tick) {
			poll();
			(void)client.Submit(client.Applied(), move, now);
			(void)host.Run();
			host.Enter([&](Store &store) {
				const auto receipt = script::PortalTransferOfPlayer(store, original);
				if (!receipt) return;
				transfer = receipt->Id;
				crossed = true;
				CHECK(
					(store.Get<scene::Transform>(walkingRoot)->Frame.Position.Z - startingZ) *
						walkingDirection >
					2
				);
			});
		}
		host.Enter([&](Store &store) {
			const auto rig = *store.Get<scene::Character>(scene::CharacterOf(store, original));
			const auto humanoid = *store.Get<scene::Humanoid>(rig.Humanoid);
			const auto position = store.Get<scene::Transform>(walkingRoot)->Frame.Position;
			CAPTURE(position.X, position.Y, position.Z, humanoid.MoveDirection.Z, humanoid.Enabled);
			REQUIRE(crossed);
		});
	}
	for (int tick = 0; tick < 4; tick++)
		(void)host.Run();
	const auto requests = host.Worlds().TakePresentation(endpoint.Address);
	REQUIRE(requests.size() == 1);
	game::PortalSessionMessage route;
	REQUIRE(game::DecodePortalSession(requests.front().Payload, route));
	REQUIRE(route.Kind == game::PortalSessionKind::LeaseRequest);
	CHECK(route.Identity == identity->Public());
	CHECK(route.Claim.Transfer == transfer);
	CHECK(transfer.SourceIncarnation == requests.front().From.Session);
	CHECK(route.Claim.SourceSession == requests.front().From.Session);
	route.Kind = refuse ? game::PortalSessionKind::Refused : game::PortalSessionKind::LeaseRoute;
	route.Claim.DestinationIncarnation = 202;
	route.Identity = identity->Public();
	route.Port = 9000;
	route.Diagnostic = "test destination refused";
	script::PortalTransferId successor;
	bool restarted = false;
	if (outcome == 4) {
		host.Worlds().Enter(host.Primary(), [&](Store &, Scheduler &scheduler) {
			scheduler.Add("test.retry-refused-portal", Phase::PostSimulation, [&](Store &store) {
				const auto receipt = script::PortalTransferOfPlayer(store, original);
				if (restarted || !receipt || receipt->Stage != script::PortalTransferStage::Refused) return;
				std::string failure;
				REQUIRE(
					script::BeginPortalTransfer(
						store, original, endpoint.Address.World, {}, successor, failure
					)
				);
				restarted = true;
			});
		});
	}
	REQUIRE(
		host.Worlds().SendPresentation(
			destination,
			endpoint.Address,
			requests.front().From,
			route.Attempt,
			game::EncodePortalSession(route)
		) == world::PresentationStatus::Ok
	);
	for (int tick = 0; tick < 1000 && replies.size() < 2; tick++) {
		(void)host.Run();
		poll();
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	REQUIRE(replies.size() == 2);
	INFO(replies.back().Diagnostic);
	CHECK(
		replies.back().Kind == (refuse ? game::PortalSessionKind::Refused : game::PortalSessionKind::Transfer)
	);
	REQUIRE(host.Enter([&](Store &store) {
		CHECK(store.Alive(original));
		const auto receipt = script::PortalTransferOfPlayer(store, original);
		REQUIRE(receipt.has_value());
		CHECK(
			receipt->Stage == (refuse && outcome != 4 ? script::PortalTransferStage::Refused
													  : script::PortalTransferStage::Preparing)
		);
		const auto *rig = store.Get<scene::Character>(scene::CharacterOf(store, original));
		REQUIRE(rig != nullptr);
		const auto *humanoid = store.Get<scene::Humanoid>(rig->Humanoid);
		REQUIRE(humanoid != nullptr);
		CHECK(humanoid->Enabled == (refuse && outcome != 4));
	}));
	host.Worlds().Enter(destination, [&](Store &store) {
		CHECK(script::PortalTransferPlayer(store, transfer) == ecs::NULL_ENTITY);
	});
	if (outcome == 4) {
		REQUIRE(restarted);
		for (int tick = 0; tick < 3; tick++)
			(void)host.Run();
		const auto retry = host.Worlds().TakePresentation(endpoint.Address);
		REQUIRE(retry.size() == 1);
		game::PortalSessionMessage request;
		REQUIRE(game::DecodePortalSession(retry.front().Payload, request));
		CHECK(request.Kind == game::PortalSessionKind::LeaseRequest);
		CHECK(request.Claim.Transfer == successor);
		CHECK(request.Attempt != route.Attempt);
		CHECK(request.Claim.Capability != route.Claim.Capability);
	} else if (outcome >= 2) {
		if (outcome == 2 || outcome == 5) {
			discardTransferReplies = outcome == 5;
			game::PortalSessionMessage cancelled;
			cancelled.Kind = game::PortalSessionKind::Refused;
			cancelled.Attempt = route.Attempt;
			cancelled.Diagnostic = "client destination connection failed";
			send(cancelled);
		} else {
			CHECK(host.Worlds().ClosePresentation(endpoint.Address) == world::PresentationStatus::Ok);
		}
		if (outcome == 5) {
			for (int tick = 0; tick < 40; ++tick) {
				(void)host.Run();
				poll();
			}
			REQUIRE(droppedTransferReplies >= 2);
			CHECK(replies.size() == 2);
			REQUIRE(host.Enter([&](Store &store) {
				const auto receipt = script::PortalTransferOfPlayer(store, original);
				REQUIRE(receipt.has_value());
				CHECK(receipt->Stage == script::PortalTransferStage::Cancelling);
				const auto *rig = store.Get<scene::Character>(scene::CharacterOf(store, original));
				REQUIRE(rig != nullptr);
				CHECK_FALSE(store.Get<scene::Humanoid>(rig->Humanoid)->Enabled);
			}));
			discardTransferReplies = false;
		}
		for (int tick = 0; tick < 1000 && replies.size() == 2; tick++) {
			(void)host.Run();
			poll();
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
		REQUIRE(replies.size() == 3);
		CHECK(replies.back().Kind == game::PortalSessionKind::Refused);
		if (outcome == 2) {
			// A client that missed the terminal reply repeats the same cancellation.
			// The retired departure must still answer without recreating a transfer.
			game::PortalSessionMessage cancelled;
			cancelled.Kind = game::PortalSessionKind::Refused;
			cancelled.Attempt = route.Attempt;
			cancelled.Diagnostic = "client destination connection failed";
			send(cancelled);
			for (int tick = 0; tick < 1000 && replies.size() == 3; ++tick) {
				(void)host.Run();
				poll();
				std::this_thread::sleep_for(std::chrono::milliseconds(2));
			}
			REQUIRE(replies.size() == 4);
			CHECK(replies.back().Kind == game::PortalSessionKind::Refused);
			CHECK(replies.back().Attempt == route.Attempt);
		}
		REQUIRE(host.Enter([&](Store &store) {
			CHECK(store.Alive(original));
			const auto receipt = script::PortalTransferOfPlayer(store, original);
			REQUIRE(receipt.has_value());
			CHECK(receipt->Stage == script::PortalTransferStage::Refused);
			const auto *rig = store.Get<scene::Character>(scene::CharacterOf(store, original));
			REQUIRE(rig != nullptr);
			REQUIRE(store.Get<scene::Humanoid>(rig->Humanoid) != nullptr);
			CHECK(store.Get<scene::Humanoid>(rig->Humanoid)->Enabled);
		}));
		host.Worlds().Enter(destination, [&](Store &store) {
			CHECK(script::PortalTransferPlayer(store, transfer) == ecs::NULL_ENTITY);
		});
	} else if (!refuse) {
		CHECK(replies.back().Claim == route.Claim);
		game::PortalSessionMessage proceed;
		proceed.Kind = game::PortalSessionKind::Proceed;
		proceed.Attempt = route.Attempt;
		proceed.Claim = route.Claim;
		proceed.Claim.Capability[0] ^= std::byte{1};
		send(proceed);
		for (int tick = 0; tick < 1000 && replies.size() == 2; tick++) {
			poll();
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
		REQUIRE(replies.size() == 3);
		CHECK(replies.back().Kind == game::PortalSessionKind::Refused);
		proceed.Claim = route.Claim;
		send(proceed);
		for (int tick = 0; tick < 1000 && replies.size() == 3; tick++) {
			(void)host.Run();
			poll();
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
		REQUIRE(replies.size() == 4);
		CHECK(replies.back().Kind == game::PortalSessionKind::Crossed);
		CHECK(replies.back().Claim == route.Claim);
		REQUIRE(host.Enter([&](Store &store) { CHECK_FALSE(store.Alive(original)); }));
		host.Worlds().Enter(destination, [&](Store &store) {
			const auto arrived = script::PortalTransferPlayer(store, transfer);
			REQUIRE(arrived != ecs::NULL_ENTITY);
			CHECK(store.Alive(arrived));
			CHECK(scene::CharacterOf(store, arrived) != ecs::NULL_ENTITY);
			const auto rig = *store.Get<scene::Character>(scene::CharacterOf(store, arrived));
			CHECK(store.Get<scene::Humanoid>(rig.Humanoid)->Health == 23);
			CHECK(store.Get<scene::Humanoid>(rig.Humanoid)->Enabled);
			CHECK(store.Get<scene::Motion>(rig.Root)->Linear.Z * walkingDirection < 0);
			size_t players = 0;
			store.EachEntity([&](Entity entity) {
				if (store.IsA(entity, scene::PlayerClass())) players++;
			});
			CHECK(players == 1);
		});
		for (int tick = 0; tick < 100 && motionReplies.empty(); ++tick) {
			(void)host.Run();
			poll();
		}
		REQUIRE_FALSE(motionReplies.empty());
		CHECK(motionReplies.front().Claim == route.Claim);
		CHECK(motionReplies.front().Attempt == route.Attempt);
		REQUIRE(motionReplies.front().Motion);
		const auto renewalAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(5100);
		while (std::chrono::steady_clock::now() < renewalAt) {
			poll();
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		for (int tick = 0; tick < 3; ++tick)
			(void)host.Run();
		const auto renewals = host.Worlds().TakePresentation(endpoint.Address);
		REQUIRE(renewals.size() == 1);
		game::PortalSessionMessage renewal;
		REQUIRE(game::DecodePortalSession(renewals.front().Payload, renewal));
		CHECK(renewal.Kind == game::PortalSessionKind::LeaseRequest);
		CHECK(renewal.Claim == route.Claim);
		CHECK(renewal.Attempt == route.Attempt);
		poll();
		CHECK(replies.size() == 4);
	}
	host.Shutdown();
}

TEST_CASE(
	"a destination lease resumes the transferred rig without a fresh player",
	"[server][portal-session][destination-lease]"
) {
	using namespace engine;
	auto options = Headless(0, 64);
	options.Listening = true;
	options.Transport = net::WireMode::Datagram;
	options.IdentityKey.assign(64, 'a');
	server::Server host;
	REQUIRE(host.Initialise(options));
	scene::RegisterSceneClasses();
	script::RegisterPortalTransferComponents();
	host.Worlds().Enter(host.Primary(), [&](Store &store) {
		scene::InstallServices(store);
		REQUIRE(script::PortalTransferIncarnation(store) != 0);
	});
	world::WorldSettings sourceSettings;
	sourceSettings.Name = core::Name("lease-source");
	const auto source = host.Worlds().Create(sourceSettings);
	REQUIRE(source.IsValid());
	ecs::Entity original;
	host.Worlds().Enter(source, [&](Store &store, ecs::Scheduler &scheduler) {
		scene::InstallServices(store);
		REQUIRE(script::ConfigurePortalTransfers(store, 101));
		script::RegisterTeleportAdmission(scheduler);
		original = scene::AddPlayer(store, "traveller", false, 71);
		REQUIRE(original != ecs::NULL_ENTITY);
		REQUIRE(scene::LoadCharacter(store, original) != ecs::NULL_ENTITY);
	});
	for (int tick = 0; tick < 3; tick++)
		host.Worlds().Tick(1.0f / 30);
	script::PortalTransferId transfer;
	host.Worlds().Enter(source, [&](Store &store) {
		std::string failure;
		REQUIRE(
			script::BeginPortalTransfer(
				store,
				original,
				host.Worlds().NameOf(host.Primary()).Text(),
				{core::CFrame({0, 20, 0}), {}, 1},
				transfer,
				failure
			)
		);
	});
	for (int tick = 0; tick < 12; tick++)
		host.Worlds().Tick(1.0f / 30);
	ecs::Entity arrived;
	REQUIRE(host.Enter([&](Store &store) {
		arrived = script::PortalTransferPlayer(store, transfer);
		REQUIRE(arrived != ecs::NULL_ENTITY);
		REQUIRE(scene::CharacterOf(store, arrived) != ecs::NULL_ENTITY);
	}));
	host.Worlds().Enter(source, [&](Store &store) { CHECK_FALSE(store.Alive(original)); });

	std::array<std::byte, assets::SigningKey::SEED_BYTES> seed;
	seed.fill(std::byte{19});
	const auto identity = assets::SigningKey::FromSeed(seed);
	REQUIRE(identity.has_value());
	const auto sender = host.Worlds().OpenPresentation(source, core::Name("portal-sessions"));
	REQUIRE(sender.Status == world::PresentationStatus::Ok);
	const auto destination = host.Worlds().LookupPresentation(host.Primary(), "portal-sessions");
	REQUIRE(destination.Generation != 0);
	game::PortalSessionMessage offer;
	offer.Kind = game::PortalSessionKind::LeaseRequest;
	offer.Attempt = 10;
	offer.Claim.Transfer = transfer;
	offer.Claim.Destination = destination.World;
	offer.Claim.SourceSession = sender.Address.Session;
	offer.Claim.Capability.fill(std::byte{42});
	offer.Identity = identity->Public();
	REQUIRE(
		host.Worlds().SendPresentation(
			source, sender.Address, destination, offer.Attempt, game::EncodePortalSession(offer)
		) == world::PresentationStatus::Ok
	);
	REQUIRE(host.Run().Ticks == 64);
	const auto routes = host.Worlds().TakePresentation(sender.Address);
	REQUIRE(routes.size() == 1);
	game::PortalSessionMessage route;
	REQUIRE(game::DecodePortalSession(routes.front().Payload, route));
	REQUIRE(route.Kind == game::PortalSessionKind::LeaseRoute);
	CHECK(route.Claim.DestinationIncarnation == destination.Session);
	CHECK(route.Port == host.ListeningOn().Port);
	CHECK_FALSE(route.Identity.IsZero());

	auto socket = net::MakeUdpTransport(0);
	REQUIRE(socket != nullptr);
	replication::ConnectorSettings connecting;
	connecting.Advertised = net::WireMode::Datagram;
	connecting.ClientIdentity = &*identity;
	connecting.ServerIdentity = route.Identity;
	double now = core::Clock::Seconds();
	replication::Connector client(*socket, net::Endpoint::LoopbackIPv4(route.Port), now, connecting);
	Store replica("lease-replica");
	std::vector<game::PortalSessionMessage> replies;
	client.OnUserMessage([&](std::span<const std::byte> bytes) {
		game::PortalSessionMessage message;
		if (game::DecodePortalSession(bytes, message)) replies.push_back(std::move(message));
	});
	const auto poll = [&] {
		now = core::Clock::Seconds();
		client.Poll(replica, now);
		host.Clients()->Poll(now);
		host.Clients()->Advance(now);
		host.Clients()->Flush(now);
		client.Poll(replica, now);
	};
	for (int tick = 0; tick < 100 && !client.Admitted(); tick++) {
		poll();
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	REQUIRE(client.Admitted());
	const auto request =
		[&](game::PortalSessionKind kind, game::PortalResume claim, game::PortalSessionKind expected) {
			game::PortalSessionMessage message;
			message.Kind = kind;
			message.Claim = std::move(claim);
			message.Attempt = 20 + replies.size();
			const size_t before = replies.size();
			poll();
			REQUIRE(client.SendUser(game::EncodePortalSession(message), now));
			for (int tick = 0; tick < 100 && replies.size() == before; tick++)
				poll();
			REQUIRE(replies.size() == before + 1);
			CHECK(replies.back().Attempt == message.Attempt);
			CHECK(replies.back().Kind == expected);
			if (expected != game::PortalSessionKind::Refused) CHECK(replies.back().Player == arrived);
		};
	request(game::PortalSessionKind::Commit, route.Claim, game::PortalSessionKind::Refused);
	auto wrong = route.Claim;
	wrong.Capability[0] ^= std::byte{1};
	request(game::PortalSessionKind::Resume, wrong, game::PortalSessionKind::Refused);
	request(game::PortalSessionKind::Resume, route.Claim, game::PortalSessionKind::Ready);
	request(game::PortalSessionKind::Fresh, {}, game::PortalSessionKind::Refused);
	request(game::PortalSessionKind::Commit, route.Claim, game::PortalSessionKind::Committed);
	request(game::PortalSessionKind::Commit, route.Claim, game::PortalSessionKind::Committed);
	REQUIRE(host.Enter([&](Store &store) {
		size_t players = 0;
		store.EachEntity([&](Entity entity) {
			if (store.IsA(entity, scene::PlayerClass())) players++;
		});
		CHECK(players == 1);
		CHECK(store.Alive(arrived));
		CHECK(scene::CharacterOf(store, arrived) != ecs::NULL_ENTITY);
	}));
	host.Shutdown();
}
