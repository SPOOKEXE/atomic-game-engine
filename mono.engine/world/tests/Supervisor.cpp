#include <engine/core/Name.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Supervisor.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.world.supervisor")

using engine::core::Name;
using engine::world::HostPlan;
using engine::world::HostState;
using engine::world::Isolation;
using engine::world::PlanHosts;
using engine::world::PlanHostsAcross;
using engine::world::Supervisor;
using engine::world::SupervisorSettings;
using engine::world::WorldSettings;

namespace supervisor_test {
	WorldSettings Shared(const char *name) {
		WorldSettings settings;
		settings.Name = Name(name);
		settings.IsolationLevel = Isolation::Shared;
		return settings;
	}

	WorldSettings Dedicated(const char *name) {
		WorldSettings settings;
		settings.Name = Name(name);
		settings.IsolationLevel = Isolation::Dedicated;
		return settings;
	}

	// A launcher that never spawns anything, so the policy can be exercised
	// without processes. Counts how many times each host was started.
	struct FakeLauncher {
		std::vector<Name> Started;
		size_t Failures = 0;
		bool Refuse = false;

		Supervisor::Launcher Bind() {
			return [this](const HostPlan &plan, engine::parallel::Process &) {
				if (Refuse) {
					Failures++;
					return false;
				}
				Started.push_back(plan.Name);
				return true;
			};
		}

		size_t CountOf(Name host) const {
			return static_cast<size_t>(std::count(Started.begin(), Started.end(), host));
		}
	};
}

using namespace supervisor_test;

// --- grouping -------------------------------------------------------------

TEST_CASE("nothing to place makes no hosts", "[world]") {
	REQUIRE(PlanHosts({}, 8).empty());
}

TEST_CASE("shared worlds are packed together", "[world]") {
	// A process per subarea does not scale to hundreds of them, and soft faults
	// are quarantined per world whatever the grouping - so sharing is the
	// default and the packing is the point.
	const std::vector<WorldSettings> worlds{
		Shared("zone.a"), Shared("zone.b"), Shared("zone.c"), Shared("zone.d"), Shared("zone.e")
	};

	const auto plans = PlanHosts(worlds, 2);

	REQUIRE(plans.size() == 3);
	REQUIRE(plans[0].Worlds.size() == 2);
	REQUIRE(plans[1].Worlds.size() == 2);
	REQUIRE(plans[2].Worlds.size() == 1);

	// Every world is placed exactly once.
	size_t placed = 0;
	for (const HostPlan &plan : plans) {
		placed += plan.Worlds.size();
	}
	REQUIRE(placed == worlds.size());
}

TEST_CASE("a dedicated world gets a host to itself", "[world]") {
	// The only thing isolation buys is protection from a *neighbour's* hard
	// fault, and the only way to buy it is a separate address space.
	const std::vector<WorldSettings> worlds{Shared("zone.a"), Dedicated("overworld"), Shared("zone.b")};

	const auto plans = PlanHosts(worlds, 8);

	REQUIRE(plans.size() == 2);
	REQUIRE(plans[0].Dedicated);
	REQUIRE(plans[0].Worlds.size() == 1);
	REQUIRE(plans[0].Worlds[0] == Name("overworld"));

	REQUIRE_FALSE(plans[1].Dedicated);
	REQUIRE(plans[1].Worlds.size() == 2);
}

TEST_CASE("dedicated worlds never share, however many there are", "[world]") {
	const std::vector<WorldSettings> worlds{Dedicated("a"), Dedicated("b"), Dedicated("c")};

	const auto plans = PlanHosts(worlds, 8);

	REQUIRE(plans.size() == 3);
	for (const HostPlan &plan : plans) {
		REQUIRE(plan.Dedicated);
		REQUIRE(plan.Worlds.size() == 1);
	}
}

TEST_CASE("planning is deterministic", "[world]") {
	// A supervisor rebuilding after a restart has to produce the same grouping,
	// or a restored host would hold different worlds than the one it replaced.
	const std::vector<WorldSettings> worlds{
		Shared("a"), Dedicated("b"), Shared("c"), Shared("d"), Dedicated("e"), Shared("f")
	};

	const auto first = PlanHosts(worlds, 2);
	const auto second = PlanHosts(worlds, 2);

	REQUIRE(first.size() == second.size());
	for (size_t index = 0; index < first.size(); index++) {
		REQUIRE(first[index].Name == second[index].Name);
		REQUIRE(first[index].Worlds == second[index].Worlds);
	}
}

TEST_CASE("a per-host count of zero is treated as one", "[world]") {
	const auto plans = PlanHosts({Shared("a"), Shared("b")}, 0);
	REQUIRE(plans.size() == 2);
}

TEST_CASE("a world with no name is not placed", "[world]") {
	// Everything crossing a boundary addresses a world by name, so an unnamed
	// one cannot be given to a host that would have to be told about it.
	const std::vector<WorldSettings> worlds{Shared("named"), WorldSettings{}};
	const auto plans = PlanHosts(worlds, 8);

	REQUIRE(plans.size() == 1);
	REQUIRE(plans[0].Worlds.size() == 1);
}

TEST_CASE("shared worlds are balanced across an exact host count", "[world]") {
	const auto plans = PlanHostsAcross(
		{Shared("a"), Shared("b"), Shared("c"), Shared("d"), Shared("e"), Shared("f"), Shared("g")}, 3
	);

	REQUIRE(plans.size() == 3);
	REQUIRE(plans[0].Worlds.size() == 3);
	REQUIRE(plans[1].Worlds.size() == 2);
	REQUIRE(plans[2].Worlds.size() == 2);
}

TEST_CASE("exact host planning never creates an empty host", "[world]") {
	const auto plans = PlanHostsAcross({Shared("a"), Shared("b")}, 8);
	REQUIRE(plans.size() == 2);
	REQUIRE(plans[0].Worlds.size() == 1);
	REQUIRE(plans[1].Worlds.size() == 1);
}

// --- starting -------------------------------------------------------------

TEST_CASE("a supervisor starts one host per plan", "[world]") {
	FakeLauncher launcher;
	Supervisor supervisor;
	supervisor.SetLauncher(launcher.Bind());

	const auto plans = PlanHosts({Shared("a"), Shared("b"), Dedicated("c")}, 8);
	REQUIRE(supervisor.Start(plans) == plans.size());

	REQUIRE(supervisor.Count() == plans.size());
	for (const auto &status : supervisor.Hosts()) {
		REQUIRE(status.State == HostState::Running);
		REQUIRE(status.Restarts == 0);
	}
}

TEST_CASE("a supervisor assigns distinct physical-core slots", "[world]") {
	SupervisorSettings settings;
	settings.PinToPhysicalCores = true;
	settings.FirstPhysicalCore = 1;

	FakeLauncher launcher;
	Supervisor supervisor(settings);
	supervisor.SetLauncher(launcher.Bind());
	supervisor.Start(PlanHosts({Shared("a"), Shared("b")}, 1));

	const auto hosts = supervisor.Hosts();
	REQUIRE(hosts.size() == 2);
	REQUIRE(hosts[0].PhysicalCore == 1);
	REQUIRE(hosts[1].PhysicalCore == 2);
}

TEST_CASE("a host that will not start is marked failed rather than pretended", "[world]") {
	FakeLauncher launcher;
	launcher.Refuse = true;

	Supervisor supervisor;
	supervisor.SetLauncher(launcher.Bind());

	REQUIRE(supervisor.Start(PlanHosts({Shared("a")}, 8)) == 0);
	REQUIRE(supervisor.Hosts()[0].State == HostState::Failed);
}

TEST_CASE("a host knows which worlds it was given", "[world]") {
	FakeLauncher launcher;
	Supervisor supervisor;
	supervisor.SetLauncher(launcher.Bind());

	supervisor.Start(PlanHosts({Shared("a"), Shared("b")}, 8));

	const auto status = supervisor.Hosts()[0];
	REQUIRE(status.Worlds.size() == 2);
	REQUIRE(status.Worlds[0] == Name("a"));
	REQUIRE(status.Worlds[1] == Name("b"));
}

// --- heartbeats -----------------------------------------------------------

TEST_CASE("a host that keeps beating is left alone", "[world]") {
	SupervisorSettings settings;
	settings.HeartbeatSeconds = 5.0;

	FakeLauncher launcher;
	Supervisor supervisor(settings);
	supervisor.SetLauncher(launcher.Bind());

	const auto plans = PlanHosts({Shared("a")}, 8);
	supervisor.Start(plans);
	const Name host = plans[0].Name;

	for (double now = 0.0; now < 60.0; now += 1.0) {
		supervisor.Heartbeat(host, now);
		REQUIRE(supervisor.Poll(now) == 0);
	}

	REQUIRE(supervisor.StatusOf(host).State == HostState::Running);
	REQUIRE(supervisor.StatusOf(host).Restarts == 0);
	REQUIRE(launcher.CountOf(host) == 1);
}

TEST_CASE("a host that goes silent is restarted", "[world]") {
	// Overdue rather than confirmed dead: a host that stopped answering may be
	// wedged rather than gone, and restarting is what makes the two one case.
	SupervisorSettings settings;
	settings.HeartbeatSeconds = 5.0;

	FakeLauncher launcher;
	Supervisor supervisor(settings);
	supervisor.SetLauncher(launcher.Bind());

	const auto plans = PlanHosts({Shared("a")}, 8);
	supervisor.Start(plans);
	const Name host = plans[0].Name;

	supervisor.Heartbeat(host, 0.0);
	REQUIRE(supervisor.Poll(1.0) == 0);
	REQUIRE(supervisor.Poll(4.9) == 0);

	// Past the deadline.
	REQUIRE(supervisor.Poll(5.1) == 1);
	REQUIRE(supervisor.StatusOf(host).State == HostState::Running);
	REQUIRE(supervisor.StatusOf(host).Restarts == 1);
	REQUIRE(launcher.CountOf(host) == 2);
}

TEST_CASE("a host that has never beaten is not presumed dead", "[world]") {
	// Start-up takes time. A deadline measured from a heartbeat that has not
	// happened yet would kill every host before it finished loading.
	SupervisorSettings settings;
	settings.HeartbeatSeconds = 1.0;

	FakeLauncher launcher;
	Supervisor supervisor(settings);
	supervisor.SetLauncher(launcher.Bind());

	const auto plans = PlanHosts({Shared("a")}, 8);
	supervisor.Start(plans);

	REQUIRE(supervisor.Poll(1000.0) == 0);
	REQUIRE(supervisor.StatusOf(plans[0].Name).Restarts == 0);
}

TEST_CASE("a late heartbeat is believed over the deadline", "[world]") {
	// The deadline is a guess about a host that has stopped. A host that
	// answers has not stopped, whatever the guess said a moment ago.
	SupervisorSettings settings;
	settings.HeartbeatSeconds = 5.0;

	FakeLauncher launcher;
	Supervisor supervisor(settings);
	supervisor.SetLauncher(launcher.Bind());

	const auto plans = PlanHosts({Shared("a")}, 8);
	supervisor.Start(plans);
	const Name host = plans[0].Name;

	supervisor.Heartbeat(host, 0.0);
	supervisor.Heartbeat(host, 4.0);
	REQUIRE(supervisor.Poll(6.0) == 0);
	REQUIRE(supervisor.StatusOf(host).State == HostState::Running);
}

TEST_CASE("a heartbeat from a host nobody started is ignored", "[world]") {
	Supervisor supervisor;
	supervisor.Heartbeat(Name("host.stranger"), 1.0);
	REQUIRE(supervisor.Count() == 0);
	REQUIRE(supervisor.StatusOf(Name("host.stranger")).State == HostState::Idle);
}

// --- crash loops ----------------------------------------------------------

TEST_CASE("a host that keeps dying is held down", "[world]") {
	// A crash loop that looks alive on every dashboard is worse than one that
	// stops, because nobody goes looking for it.
	SupervisorSettings settings;
	settings.HeartbeatSeconds = 1.0;
	settings.RestartLimit = 3;

	FakeLauncher launcher;
	Supervisor supervisor(settings);
	supervisor.SetLauncher(launcher.Bind());

	const auto plans = PlanHosts({Shared("a")}, 8);
	supervisor.Start(plans);
	const Name host = plans[0].Name;

	double now = 0.0;
	for (int attempt = 0; attempt < 10; attempt++) {
		supervisor.Heartbeat(host, now);
		now += 2.0; // past the deadline every time
		supervisor.Poll(now);
	}

	REQUIRE(supervisor.StatusOf(host).State == HostState::Failed);
	REQUIRE(supervisor.StatusOf(host).Restarts == 3);

	// And it stays down rather than being retried forever.
	const size_t launches = launcher.CountOf(host);
	supervisor.Poll(now + 100.0);
	REQUIRE(launcher.CountOf(host) == launches);
}

TEST_CASE("a host that fails to relaunch is failed rather than lost", "[world]") {
	SupervisorSettings settings;
	settings.HeartbeatSeconds = 1.0;

	FakeLauncher launcher;
	Supervisor supervisor(settings);
	supervisor.SetLauncher(launcher.Bind());

	const auto plans = PlanHosts({Shared("a")}, 8);
	supervisor.Start(plans);
	const Name host = plans[0].Name;

	supervisor.Heartbeat(host, 0.0);
	launcher.Refuse = true;
	supervisor.Poll(5.0);

	REQUIRE(supervisor.StatusOf(host).State == HostState::Failed);
	REQUIRE(launcher.Failures == 1);
}

TEST_CASE("one host failing does not disturb the others", "[world]") {
	SupervisorSettings settings;
	settings.HeartbeatSeconds = 1.0;

	FakeLauncher launcher;
	Supervisor supervisor(settings);
	supervisor.SetLauncher(launcher.Bind());

	const auto plans = PlanHosts({Dedicated("a"), Dedicated("b"), Dedicated("c")}, 8);
	supervisor.Start(plans);

	// Only the middle one goes silent; the others keep beating.
	double now = 0.0;
	for (const auto &plan : plans) {
		supervisor.Heartbeat(plan.Name, now);
	}

	for (int step = 0; step < 5; step++) {
		now += 2.0;
		supervisor.Heartbeat(plans[0].Name, now);
		supervisor.Heartbeat(plans[2].Name, now);
		supervisor.Poll(now);
	}

	REQUIRE(supervisor.StatusOf(plans[0].Name).Restarts == 0);
	REQUIRE(supervisor.StatusOf(plans[2].Name).Restarts == 0);
	REQUIRE(supervisor.StatusOf(plans[1].Name).Restarts > 0);
}

// --- stopping -------------------------------------------------------------

TEST_CASE("stopping everything leaves every host idle", "[world]") {
	FakeLauncher launcher;
	Supervisor supervisor;
	supervisor.SetLauncher(launcher.Bind());

	supervisor.Start(PlanHosts({Shared("a"), Shared("b")}, 1));
	supervisor.StopAll();

	for (const auto &status : supervisor.Hosts()) {
		REQUIRE(status.State == HostState::Idle);
	}

	// And a poll afterwards does not resurrect them.
	REQUIRE(supervisor.Poll(100.0) == 0);
}

TEST_CASE("a supervisor with no program spawns nothing and reports success", "[world]") {
	// The in-process case. Reporting failure would make a universe that hosts
	// its worlds itself look broken.
	Supervisor supervisor;
	REQUIRE(supervisor.Start(PlanHosts({Shared("a")}, 8)) == 1);
	REQUIRE(supervisor.Hosts()[0].State == HostState::Running);
}
