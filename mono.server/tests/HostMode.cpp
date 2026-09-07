// A supervised host, as an actual second process.
//
// These exercise the product launch path, beyond the engine protocol tests:
// the supervisor
// creating a channel, the child inheriting it across an exec, `--host` finding
// it, and worlds it was granted by name coming up and ticking. Any one of those
// four failing produces a host that looks started and says nothing.
//
// The tests are skipped rather than failed when the server binary is not beside
// the test binary, because a preset that builds tests without the program is a
// legal configuration and a suite that fails on it would be reporting the
// build, not the code.

#include <engine/core/Log.hpp>
#include <engine/core/Name.hpp>
#include <engine/core/Paths.hpp>
#include <engine/game/Game.hpp>
#include <engine/parallel/Process.hpp>
#include <engine/parallel/ProcessChannel.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Supervisor.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <server/Server.hpp>
#include <server/Simulation.hpp>
#include <thread>
#include <vector>

TEST_SUITE_ID("server.hostmode")

using engine::core::Name;
using engine::world::HostPlan;
using engine::world::HostState;
using engine::world::HostStatus;
using engine::world::Supervisor;
using engine::world::SupervisorSettings;

TEST_CASE("saved project child keeps only its granted world local", "[server][saved-host-child][.]") {
	server::Options options;
	options.HostName = "saved-host";
	options.HostWorlds = {"far"};
	options.GamePath = (engine::core::Paths::Base() / "saved-host-project.agame").string();
	options.ControlPort = -1;
	server::Server host;
	REQUIRE(host.Initialise(options));
	CHECK(host.Worlds().NameOf(host.Primary()) == Name("far"));
	CHECK_FALSE(host.Worlds().IsRemote(host.Primary()));
	CHECK(host.Worlds().IsRemote(host.Worlds().Find(Name("near"))));
	bool found = false;
	host.Enter([&](engine::ecs::Store &store) {
		store.Query<const engine::scene::Transform>().Each([&](engine::ecs::Entity,
															   const engine::scene::Transform &transform) {
			found |= transform.Frame.Position.X == 42;
		});
	});
	CHECK(found);
	host.Shutdown();
}

TEST_CASE("supervised Server loads a saved project by granted world name", "[server][saved-host]") {
	using namespace engine;
	scene::RegisterSceneClasses();
	world::Universe authored;
	for (const auto *name : {"near", "far"}) {
		const auto id = authored.Create({.Name = Name(name)});
		authored.Enter(id, [&](ecs::Store &store) {
			const auto workspace = scene::InstallServices(store);
			scene::PartDesc part;
			part.Frame = core::CFrame(core::Vector3{name == std::string_view("far") ? 42.0f : 0.0f, 0, 0});
			REQUIRE(store.SetParent(scene::MakePart(store, part), workspace));
		});
	}
	std::string error;
	REQUIRE(
		game::SaveGame(authored, Name("saved host"), core::Paths::Base() / "saved-host-project.agame", error)
	);
	auto channel = parallel::MakeProcessChannel();
	REQUIRE(channel.Valid());
	parallel::Process child;
	REQUIRE(child.Start(
		core::Paths::Base() / core::Paths::Program("test_server"),
		{"[saved-host-child]"},
		std::move(channel.Remote)
	));
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
	auto status = child.Poll();
	while (status.Alive() && std::chrono::steady_clock::now() < deadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
		status = child.Poll();
	}
	CHECK(status.Reason == parallel::ExitReason::Exited);
	CHECK(status.Code == 0);
}

namespace host_mode_test {
	// The server program, which sits beside the test binary's directory rather
	// than in it: tests stage into `tests/` and programs into their own.
	std::filesystem::path ServerProgram() {
		return engine::core::Paths::Base().parent_path() / "server" / engine::core::Paths::Program("server");
	}

	bool ServerAvailable() {
		return std::filesystem::exists(ServerProgram());
	}

	// Runs the supervisor's barrier until `ready(status)` holds, or the
	// deadline passes.
	//
	// This helper drives autonomous link tests. Product tick coordination uses
	// separate bounded phase waits on the driver thread.
	bool Settle(
		Supervisor &supervisor,
		Name host,
		const std::function<bool(const HostStatus &)> &ready,
		std::chrono::seconds limit = std::chrono::seconds(20)
	) {
		const auto started = std::chrono::steady_clock::now();
		const auto deadline = started + limit;

		while (std::chrono::steady_clock::now() < deadline) {
			const double now =
				std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

			supervisor.Pump(now);
			supervisor.Poll(now);

			if (ready(supervisor.StatusOf(host))) {
				return true;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		return false;
	}

	SupervisorSettings Settings() {
		SupervisorSettings settings;
		settings.Program = ServerProgram();
		settings.Arguments = {"--unpaced", "--entities", "64"};

		// Generous, because these spawn a real process that builds real worlds
		// and the machine may be running the rest of the suite alongside. The
		// deadline being tested elsewhere is the *policy*; this is the number
		// that keeps a slow machine from failing a case a fast one passes.
		settings.HeartbeatSeconds = 30.0;
		settings.RestartLimit = 0;
		return settings;
	}
}

using namespace host_mode_test;

TEST_CASE(
	"repeated product phase acknowledgement does not republish bus traffic", "[server][host-exchange]"
) {
	using namespace engine::world;
	if (!ServerAvailable()) SKIP("the server program was not built into this preset");
	auto settings = Settings();
	settings.Arguments.push_back("--host-tick-exchange");
	settings.Arguments.push_back("--chatter");
	Supervisor supervisor(settings);
	const Name host("host.phase.retry");
	REQUIRE(supervisor.Start({{host, {Name("phase.retry")}}}) == 1);
	REQUIRE(Settle(supervisor, host, [](const HostStatus &status) { return status.Ready; }));
	TickExchangeCommand command;
	command.Frame = 1;
	command.FrameSeconds = 1.0f / 30;
	const auto send = [&] {
		REQUIRE(supervisor.SendTickExchange(host, command));
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (std::chrono::steady_clock::now() < deadline) {
			supervisor.Pump(1);
			if (auto result = supervisor.TakeTickExchange(host)) {
				REQUIRE(result->Success);
				return;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		FAIL("product phase acknowledgement deadline expired");
	};
	for (uint64_t frame = 1; frame <= 2; ++frame) {
		command = {};
		command.Frame = frame;
		command.FrameSeconds = 1.0f / 30;
		send();
		command.FrameSeconds = 0;
		for (auto operation :
			 {TickExchangeOperation::Collect, TickExchangeOperation::Serve, TickExchangeOperation::Apply}) {
			command.Operation = operation;
			send();
		}
		command.Operation = TickExchangeOperation::End;
		command.Round = 1;
		send();
	}
	REQUIRE_FALSE(supervisor.TakeTraffic().empty());
	send();
	CHECK(supervisor.TakeTraffic().empty());
	CHECK(supervisor.StatusOf(host).Tick == 2);
	REQUIRE(supervisor.AskToStop(host));
}

TEST_CASE("product driver advances every spawned host by the same fixed frames", "[server][host-exchange]") {
	if (!ServerAvailable()) SKIP("the server program was not built into this preset");
	server::Options options;
	options.TickRate = 60;
	options.Entities = 4;
	options.Unpaced = true;
	options.MaximumTicks = 8;
	options.Seconds = 10;
	options.HostProgram = ServerProgram();
	options.WorldsPerHost = 1;
	options.RemoteWorlds = {"phase.first", "phase.second"};
	server::Server driver;
	REQUIRE(driver.Initialise(options));
	driver.Run();
	CHECK(driver.Worlds().StatisticsOf(driver.Primary()).Ticks == 8);
	CHECK_FALSE(driver.Hosts()->Statistics().TickExchangeFailed);
	const auto hosts = driver.Hosts()->Hosts().Hosts();
	REQUIRE(hosts.size() == 2);
	for (const auto &host : hosts) {
		CHECK(host.Ready);
		CHECK(host.Tick == 8);
	}
	driver.Shutdown();
}

TEST_CASE("a spawned host comes up, says it is ready, and heartbeats", "[server]") {
	if (!ServerAvailable()) {
		SKIP("the server program was not built into this preset");
	}

	Supervisor supervisor(Settings());

	HostPlan plan;
	plan.Name = Name("host.spawned");
	plan.Worlds = {Name("lobby"), Name("arena")};

	REQUIRE(supervisor.Start({plan}) == 1);
	REQUIRE(supervisor.StatusOf(plan.Name).Linked);

	// Ready is sent once the worlds are built, so it is the answer to "did the
	// child find its channel and understand what it was granted", which is the
	// whole join this case exists for.
	REQUIRE(Settle(supervisor, plan.Name, [](const HostStatus &status) { return status.Ready; }));

	// And then it ticks. A host that came up and stopped simulating would
	// answer the heartbeat forever with a tick count that never moved.
	REQUIRE(Settle(supervisor, plan.Name, [](const HostStatus &status) { return status.Tick > 0; }));

	const HostStatus status = supervisor.StatusOf(plan.Name);
	REQUIRE(status.State == HostState::Running);
	REQUIRE(status.Restarts == 0);
	REQUIRE(status.ProcessId != 0);

	supervisor.StopAll();
}

TEST_CASE("a listening host reports its own replication port", "[server]") {
	if (!ServerAvailable()) {
		SKIP("the server program was not built into this preset");
	}

	SupervisorSettings settings = Settings();
	settings.Arguments.emplace_back("--listen");
	settings.Arguments.emplace_back("0");
	Supervisor supervisor(settings);

	HostPlan plan;
	plan.Name = Name("host.listening");
	plan.Worlds = {Name("lobby")};

	REQUIRE(supervisor.Start({plan}) == 1);
	REQUIRE(Settle(supervisor, plan.Name, [](const HostStatus &status) { return status.Ready; }));

	const HostStatus status = supervisor.StatusOf(plan.Name);
	CHECK(status.Worlds == plan.Worlds);
	CHECK(status.Port != 0);

	supervisor.StopAll();
}

TEST_CASE("a host stops when its driver asks over the link", "[server]") {
	if (!ServerAvailable()) {
		SKIP("the server program was not built into this preset");
	}

	Supervisor supervisor(Settings());

	HostPlan plan;
	plan.Name = Name("host.asked");
	plan.Worlds = {Name("lobby")};

	REQUIRE(supervisor.Start({plan}) == 1);
	REQUIRE(Settle(supervisor, plan.Name, [](const HostStatus &status) { return status.Ready; }));

	// Asked rather than signalled, so the host finishes the tick it is in.
	REQUIRE(supervisor.AskToStop(plan.Name));

	// The link closing is how the driver learns it went. With `RestartLimit` at
	// zero it is held down rather than respawned, which is the state to check
	// for: a restart here would mean the driver could not tell "I asked it to
	// go" from "it died".
	REQUIRE(Settle(supervisor, plan.Name, [](const HostStatus &status) {
		return status.State == HostState::Failed;
	}));

	supervisor.StopAll();
}

TEST_CASE("a host whose driver goes away exits rather than orphaning itself", "[server]") {
	if (!ServerAvailable()) {
		SKIP("the server program was not built into this preset");
	}

	// The failure this prevents is a host that keeps ticking worlds nobody can
	// reach, holding their memory and answering to nothing. It is also the case
	// that only works if the child did not inherit a copy of the driver's own
	// end of the channel.
	{
		Supervisor supervisor(Settings());

		HostPlan plan;
		plan.Name = Name("host.orphan");
		plan.Worlds = {Name("lobby")};

		REQUIRE(supervisor.Start({plan}) == 1);
		REQUIRE(Settle(supervisor, plan.Name, [](const HostStatus &status) { return status.Ready; }));

		// The supervisor's destructor kills what it started, which is the
		// belt-and-braces path. What is being checked is the other one, so the
		// child is checked for having gone on its own below.
	}

	// Nothing to assert on directly once the supervisor is gone - the process
	// is reaped by its destructor. The case earns its place by not hanging: a
	// host that failed to notice would still be running, and `StopAll` in the
	// destructor would have had to kill it.
	SUCCEED();
}

TEST_CASE("a host granted no worlds refuses to start", "[server]") {
	if (!ServerAvailable()) {
		SKIP("the server program was not built into this preset");
	}

	// A supervisor that granted nothing has a bug in its plan, and an idle
	// process that answers heartbeats is how that bug survives to production.
	Supervisor supervisor(Settings());

	HostPlan plan;
	plan.Name = Name("host.empty");

	REQUIRE(supervisor.Start({plan}) == 1);

	// It exits, so the link closes and the child reaps. With no restarts
	// allowed it is held down.
	REQUIRE(Settle(supervisor, plan.Name, [](const HostStatus &status) {
		return status.State == HostState::Failed;
	}));

	REQUIRE_FALSE(supervisor.StatusOf(plan.Name).Ready);
	supervisor.StopAll();
}

// --- the driver's side, end to end -------------------------------------------
//
// Everything above proves a host starts, answers and stops. What is left is the
// reason it exists: a world in another process behaving like one in this one.
// These cases run a real driver with a real spawned host and require bus
// traffic to cross in both directions through the driver's own buses.

TEST_CASE("a world in a host and a world here share one bus", "[server]") {
	if (!ServerAvailable()) {
		SKIP("the server program was not built into this preset");
	}

	server::Options options;
	options.TickRate = 60.0;
	options.Entities = 32;
	options.Chatter = true;
	options.HostProgram = ServerProgram();
	options.RemoteWorlds = {"remote.one", "remote.two"};

	// Bounded by wall time rather than ticks, and paced. A host is a *process*:
	// it has to be spawned, linked, and get its worlds built before it can say
	// anything, and an unpaced driver runs four thousand ticks in the
	// millisecond before any of that has happened.
	options.Seconds = 6.0;

	server::Server driver;
	REQUIRE(driver.Initialise(options));

	// Both remote worlds are in the directory before their host has answered
	// anything, so a subscription made in the first barrier has somewhere to go.
	REQUIRE(driver.Worlds().Find(Name("remote.one")).IsValid());
	REQUIRE(driver.Worlds().IsRemote(driver.Worlds().Find(Name("remote.one"))));

	driver.Run();

	// The driver's own world heard from a world in another process. Nothing
	// short of the whole chain produces this: the host spawned, inherited its
	// channel, built the world it was granted, posted a publish, the driver
	// accepted it against the host that sent it, routed it through the one
	// MessagingService, and put it in a local inbox.
	uint64_t heard = 0;
	Name from;
	REQUIRE(driver.Enter([&heard, &from](engine::ecs::Store &store) {
		if (const server::Heard *record = store.Resource<server::Heard>(); record != nullptr) {
			heard = record->Count;
			from = record->From;
		}
	}));

	driver.Shutdown();

	REQUIRE(heard > 0);
	REQUIRE(from.IsValid());
	REQUIRE((from == Name("remote.one") || from == Name("remote.two")));
}

TEST_CASE("a host's traffic is refused if it names a world the host does not hold", "[server]") {
	if (!ServerAvailable()) {
		SKIP("the server program was not built into this preset");
	}

	// The honest version of this check runs against a spawned host that is
	// behaving, so what is asserted is the *absence* of refusals - a driver
	// counting refusals against its own hosts would mean its directory and
	// theirs disagree, which is a bug rather than a load figure.
	server::Options options;
	options.TickRate = 60.0;
	options.Entities = 16;
	options.Chatter = true;
	options.HostProgram = ServerProgram();
	options.RemoteWorlds = {"honest.one"};
	options.Seconds = 4.0;

	server::Server driver;
	REQUIRE(driver.Initialise(options));
	driver.Run();

	const auto &stats = driver.Hosts()->Statistics();
	const size_t refused = stats.TrafficRefused;
	const size_t dropped = stats.DeliveriesDropped;

	driver.Shutdown();

	REQUIRE(refused == 0);
	REQUIRE(dropped == 0);
}

TEST_CASE("a driver and its hosts share one machine's worth of workers", "[server]") {
	if (!ServerAvailable()) {
		SKIP("the server program was not built into this preset");
	}

	// Every process calling `Jobs::Start(0)` is the bug this prevents: a driver
	// and seven hosts on a twenty-four core machine would run a hundred and
	// ninety threads over twenty-four cores. The driver is the only process
	// that knows how many there will be, so it works it out and passes it down.
	server::Options options;
	options.RemoteWorlds = {"a", "b", "c"};
	options.WorldsPerHost = 1;
	options.HostProgram = ServerProgram();
	options.MaximumTicks = 1;
	options.Unpaced = true;
	options.Entities = 1;

	server::Server driver;
	REQUIRE(driver.Initialise(options));

	// One host per world, because each was planned alone.
	REQUIRE(driver.Hosts()->Hosts().Count() == 3);

	const unsigned alone = engine::parallel::WorkersPerHost(1);
	const unsigned shared = engine::parallel::WorkersPerHost(4);
	REQUIRE(shared <= alone);

	driver.Run();
	driver.Shutdown();
}

TEST_CASE("listening remote worlds receive one process and port each", "[server]") {
	if (!ServerAvailable()) {
		SKIP("the server program was not built into this preset");
	}

	server::Options options;
	options.RemoteWorlds = {"network.one", "network.two"};
	options.WorldsPerHost = 8;
	options.HostProgram = ServerProgram();
	options.Listening = true;
	options.ListenPort = 0;
	options.Entities = 1;

	server::Server driver;
	REQUIRE(driver.Initialise(options));
	Supervisor &supervisor = driver.Hosts()->Hosts();
	REQUIRE(supervisor.Count() == 2);

	const Name first("host.shared.0");
	const Name second("host.shared.1");
	const auto listening = [](const HostStatus &status) { return status.Ready && status.Port != 0; };
	REQUIRE(Settle(supervisor, first, listening));
	REQUIRE(Settle(supervisor, second, listening));

	const HostStatus firstStatus = supervisor.StatusOf(first);
	const HostStatus secondStatus = supervisor.StatusOf(second);
	CHECK(firstStatus.Worlds == std::vector<Name>{Name("network.one")});
	CHECK(secondStatus.Worlds == std::vector<Name>{Name("network.two")});
	CHECK(firstStatus.Port != secondStatus.Port);

	driver.Shutdown();
}

TEST_CASE(
	"a server host advertises and answers its portal session endpoint",
	"[server][portal-session][lease-discovery]"
) {
	using namespace engine;
	if (!ServerAvailable()) SKIP("the server program is not built into this preset");
	auto settings = Settings();
	settings.Arguments.insert(settings.Arguments.end(), {"--listen", "0"});
	Supervisor supervisor(settings);
	HostPlan plan;
	plan.Name = Name("lease-host");
	plan.Worlds = {Name("lease-destination")};
	REQUIRE(supervisor.Start({plan}) == 1);
	world::PresentationDirectory published;
	REQUIRE(Settle(supervisor, plan.Name, [&](const HostStatus &status) {
		for (const auto &entry : supervisor.TakePresentationDirectories()) {
			if (entry.Host == plan.Name && !entry.Directory.Endpoints.empty()) published = entry.Directory;
		}
		return status.Ready && !published.Endpoints.empty();
	}));
	REQUIRE(published.Endpoints.size() == 1);
	const auto endpoint = published.Endpoints.front();
	CHECK(endpoint.World == "lease-destination");
	CHECK(endpoint.Channel == "portal-sessions");
	CHECK(endpoint.Session != 0);
	CHECK(endpoint.Generation != 0);
	CHECK(supervisor.WantsPresentationRoutes(plan.Name));

	world::Universe universe;
	REQUIRE(universe.ConfigurePresentation(123));
	world::WorldSettings nearSettings;
	nearSettings.Name = Name("lease-source");
	const auto near = universe.Create(nearSettings);
	world::WorldSettings farSettings;
	farSettings.Name = Name("lease-destination");
	const auto far = universe.CreateRemote(farSettings, plan.Name);
	REQUIRE(far.IsValid());
	REQUIRE(universe.ApplyPresentationDirectory(plan.Name, published) == world::PresentationStatus::Ok);
	const auto sender = universe.OpenPresentation(near, Name("portal-sessions"));
	REQUIRE(sender.Status == world::PresentationStatus::Ok);
	REQUIRE(supervisor.PublishPresentationRoutes(plan.Name, universe.PresentationRoutesFor(plan.Name)));
	game::PortalSessionMessage request;
	request.Kind = game::PortalSessionKind::LeaseRequest;
	request.Attempt = 17;
	request.Claim.Transfer = {"lease-source", 101, 1};
	request.Claim.Destination = "lease-destination";
	request.Claim.SourceSession = sender.Address.Session;
	request.Claim.Capability.fill(std::byte{42});
	request.Identity.Value.fill(19);
	REQUIRE(
		universe.SendPresentation(
			near, sender.Address, endpoint, request.Attempt, game::EncodePortalSession(request)
		) == world::PresentationStatus::Ok
	);
	const auto outgoing = universe.TakePresentationOutbound();
	REQUIRE(outgoing.size() == 1);
	REQUIRE(supervisor.SendPresentation(plan.Name, outgoing.front().Message));
	game::PortalSessionMessage response;
	bool received = false;
	REQUIRE(Settle(supervisor, plan.Name, [&](const HostStatus &) {
		for (const auto &entry : supervisor.TakePresentationTraffic()) {
			REQUIRE(entry.Host == plan.Name);
			REQUIRE(universe.IngestPresentation(plan.Name, entry.Message) == world::PresentationStatus::Ok);
			for (const auto &reply : universe.TakePresentation(sender.Address)) {
				REQUIRE(reply.Correlation == request.Attempt);
				REQUIRE(game::DecodePortalSession(reply.Payload, response));
				received = true;
			}
		}
		return received;
	}));
	CHECK(response.Kind == game::PortalSessionKind::LeaseRoute);
	CHECK(response.Attempt == request.Attempt);
	CHECK(response.Claim.DestinationIncarnation == endpoint.Session);
	CHECK(response.Port == supervisor.StatusOf(plan.Name).Port);
	CHECK_FALSE(response.Identity.IsZero());
	REQUIRE(supervisor.AskToStop(plan.Name));
	REQUIRE(Settle(supervisor, plan.Name, [](const HostStatus &status) {
		return status.State == HostState::Failed;
	}));
}
