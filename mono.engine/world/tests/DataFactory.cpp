#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/DataFactory.hpp>
#include <engine/world/Postbox.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.world.data-factory")

using Catch::Approx;
using engine::core::Name;
using engine::ecs::Entity;
using engine::ecs::Phase;
using engine::ecs::Scheduler;
using engine::ecs::Store;
using engine::world::DataFactoryInterval;
using engine::world::DataFactoryPauseScope;
using engine::world::DataFactorySession;
using engine::world::DataFactoryStatus;
using engine::world::Delivery;
using engine::world::Postbox;
using engine::world::Universe;
using engine::world::WorldId;
using engine::world::WorldSettings;
using engine::world::WorldState;

namespace {
	struct Tally {
		int Value = 0;
	};

	WorldId MakeWorld(Universe &universe, const char *name) {
		WorldSettings settings;
		settings.Name = Name(name);
		return universe.Create(settings);
	}

	void BuildCountingWorld(Universe &universe, WorldId world, float *seenDelta = nullptr) {
		universe.Enter(world, [seenDelta](Store &store, Scheduler &systems) {
			const Entity entity = store.Create();
			store.Set<Tally>(entity, Tally{});
			systems.Add("data-factory.count", Phase::Simulation, [seenDelta](Store &inner) {
				inner.Each<Tally>([](Entity, Tally &tally) { tally.Value++; });
				if (seenDelta != nullptr) *seenDelta = inner.Time().Delta;
			});
		});
	}

	int Count(Universe &universe, WorldId world) {
		int count = 0;
		universe.Enter(world, [&count](Store &store) {
			store.Each<const Tally>([&count](Entity, const Tally &tally) { count += tally.Value; });
		});
		return count;
	}
}

TEST_CASE("data-factory all-system pause steps one exact tick boundary", "[world][data-factory]") {
	Universe universe;
	const WorldId world = MakeWorld(universe, "data-factory.step");
	DataFactorySession session(universe);
	bool allSystemsPaused = false;
	session.SetPauseParticipant(
		[&](WorldId pausedWorld, DataFactoryPauseScope scope, bool paused, std::string &) {
			if (pausedWorld != world) return false;
			if (scope != DataFactoryPauseScope::AllSystems) return false;
			allSystemsPaused = paused;
			return true;
		}
	);

	const auto paused = session.Pause("data-factory.step", DataFactoryPauseScope::AllSystems, 0);
	REQUIRE(paused.Status == DataFactoryStatus::Ok);
	CHECK(allSystemsPaused);
	CHECK(session.AllSystemsPaused("data-factory.step"));
	CHECK(universe.StateOf(world) == WorldState::Suspended);

	const auto stepped = session.Step("data-factory.step", DataFactoryInterval{}, 0, paused.WorldVersion);
	REQUIRE(stepped.Status == DataFactoryStatus::Ok);
	CHECK(stepped.Clock.Tick == 1);
	CHECK(stepped.Clock.TimeNanoseconds == 16'666'666);
	CHECK(universe.StatisticsOf(world).Ticks == 1);
	CHECK(universe.StateOf(world) == WorldState::Suspended);

	const auto resumed = session.Resume("data-factory.step", 1);
	REQUIRE(resumed.Status == DataFactoryStatus::Ok);
	CHECK_FALSE(allSystemsPaused);
	CHECK_FALSE(session.AllSystemsPaused("data-factory.step"));
	CHECK(universe.StateOf(world) == WorldState::Active);
}

TEST_CASE("data-factory refuses a scope without participating systems", "[world][data-factory]") {
	Universe universe;
	MakeWorld(universe, "data-factory.unsupported");
	DataFactorySession session(universe);

	const auto reply = session.Pause("data-factory.unsupported", DataFactoryPauseScope::AllSystems, 0);
	CHECK(reply.Status == DataFactoryStatus::Unsupported);
}

TEST_CASE("data-factory intervention rolls back a refusing host executor", "[world][data-factory]") {
	Universe universe;
	const WorldId world = MakeWorld(universe, "data-factory.intervention");
	DataFactorySession session(universe);
	session.SetPauseParticipant([](WorldId, DataFactoryPauseScope, bool, std::string &) { return true; });
	session.SetRehydrate([](Universe &, WorldId, std::string &) { return true; });
	REQUIRE(
		session.Pause("data-factory.intervention", DataFactoryPauseScope::AllSystems, 0).Status ==
		DataFactoryStatus::Ok
	);
	std::string snapshot;
	REQUIRE(session.Snapshot("data-factory.intervention", snapshot).Status == DataFactoryStatus::Ok);
	session.SetInterventionExecutor(
		[](Universe &worlds,
		   WorldId target,
		   std::span<const engine::world::DataFactoryIntervention>,
		   std::string &) -> bool {
			worlds.SetState(target, WorldState::Active);
			throw std::runtime_error("expected value did not match");
		}
	);
	const auto before = session.Inspect("data-factory.intervention");
	const engine::world::DataFactoryIntervention edit{
		.TargetId = "fixture",
		.Path = "attributes.Health",
		.Expected = {},
		.Value = {},
	};
	const auto refused = session.ApplyIntervention(
		"data-factory.intervention", snapshot, std::array{edit}, before.Clock.Tick, before.WorldVersion
	);
	CHECK(refused.Status == DataFactoryStatus::ValidationFailed);
	CHECK(refused.Detail == "intervention executor threw: expected value did not match");
	CHECK(universe.StateOf(world) == WorldState::Suspended);
	CHECK(session.Inspect("data-factory.intervention").WorldVersion == before.WorldVersion);
}

TEST_CASE(
	"data-factory resume restores an all-system pause when physics resume refuses", "[world][data-factory]"
) {
	Universe universe;
	const WorldId world = MakeWorld(universe, "data-factory.resume-rollback");
	DataFactorySession session(universe);
	bool allSystemsPaused = false;
	bool physicsPaused = false;
	bool rejectPhysicsResume = true;
	session.SetPauseParticipant(
		[&](WorldId pausedWorld, DataFactoryPauseScope scope, bool paused, std::string &) {
			if (pausedWorld != world) return false;
			if (scope == DataFactoryPauseScope::AllSystems) {
				allSystemsPaused = paused;
				return true;
			}
			if (!paused && rejectPhysicsResume) {
				rejectPhysicsResume = false;
				return false;
			}
			physicsPaused = paused;
			return true;
		}
	);

	REQUIRE(
		session.Pause("data-factory.resume-rollback", DataFactoryPauseScope::AllSystems, 0).Status ==
		DataFactoryStatus::Ok
	);
	const auto physics = session.Pause("data-factory.resume-rollback", DataFactoryPauseScope::PhysicsOnly, 0);
	REQUIRE(physics.Status == DataFactoryStatus::Ok);

	const auto resumed = session.Resume("data-factory.resume-rollback", 0);
	CHECK(resumed.Status == DataFactoryStatus::RestoreIncomplete);
	CHECK(resumed.WorldVersion == physics.WorldVersion);
	CHECK(universe.StateOf(world) == WorldState::Suspended);
	CHECK(session.AllSystemsPaused("data-factory.resume-rollback"));
	CHECK(allSystemsPaused);
	CHECK(physicsPaused);
	CHECK(
		session.Step("data-factory.resume-rollback", DataFactoryInterval{}, 0, physics.WorldVersion).Status ==
		DataFactoryStatus::Ok
	);
}

TEST_CASE(
	"data-factory records a version when resume rollback cannot restore its participant",
	"[world][data-factory]"
) {
	Universe universe;
	const WorldId world = MakeWorld(universe, "data-factory.resume-rollback-fails");
	DataFactorySession session(universe);
	bool allSystemsPaused = false;
	bool rejectRollback = false;
	session.SetPauseParticipant(
		[&](WorldId pausedWorld, DataFactoryPauseScope scope, bool paused, std::string &) {
			if (pausedWorld != world) return false;
			if (scope == DataFactoryPauseScope::PhysicsOnly) return paused;
			if (paused && rejectRollback) return false;
			allSystemsPaused = paused;
			return true;
		}
	);

	REQUIRE(
		session.Pause("data-factory.resume-rollback-fails", DataFactoryPauseScope::AllSystems, 0).Status ==
		DataFactoryStatus::Ok
	);
	const auto physics =
		session.Pause("data-factory.resume-rollback-fails", DataFactoryPauseScope::PhysicsOnly, 0);
	REQUIRE(physics.Status == DataFactoryStatus::Ok);
	rejectRollback = true;

	const auto resumed = session.Resume("data-factory.resume-rollback-fails", 0);
	CHECK(resumed.Status == DataFactoryStatus::RestoreIncomplete);
	CHECK(resumed.WorldVersion == physics.WorldVersion + 1);
	CHECK(universe.StateOf(world) == WorldState::Suspended);
	CHECK_FALSE(allSystemsPaused);
	CHECK_FALSE(session.AllSystemsPaused("data-factory.resume-rollback-fails"));
	CHECK(
		session.Step("data-factory.resume-rollback-fails", DataFactoryInterval{}, 0, physics.WorldVersion)
			.Status == DataFactoryStatus::VersionConflict
	);
	CHECK(
		session.Step("data-factory.resume-rollback-fails", DataFactoryInterval{}, 0, resumed.WorldVersion)
			.Status == DataFactoryStatus::NotPaused
	);
	CHECK(
		session.RenderSnapshotBarrier("data-factory.resume-rollback-fails", "any-snapshot").Status ==
		DataFactoryStatus::NotPaused
	);
	rejectRollback = false;
	CHECK(
		session.Pause("data-factory.resume-rollback-fails", DataFactoryPauseScope::AllSystems, 0).Status ==
		DataFactoryStatus::Ok
	);
	CHECK(session.AllSystemsPaused("data-factory.resume-rollback-fails"));
}

TEST_CASE(
	"data-factory inspection is read-only and zero checkpoint retention refuses snapshots",
	"[world][data-factory]"
) {
	Universe universe;
	MakeWorld(universe, "data-factory.inspect");
	DataFactorySession session(universe, 0);

	const auto inspected = session.Inspect("data-factory.inspect");
	REQUIRE(inspected.Status == DataFactoryStatus::Ok);
	CHECK(inspected.WorldEpoch == 1);
	CHECK(inspected.WorldVersion == 0);
	CHECK(inspected.Clock.Tick == 0);

	std::string snapshot;
	const auto refused = session.Snapshot("data-factory.inspect", snapshot);
	CHECK(refused.Status == DataFactoryStatus::ResourceLimit);
	CHECK(snapshot.empty());
}

TEST_CASE(
	"data-factory checkpoint restores through scratch and creates a fresh epoch", "[world][data-factory]"
) {
	Universe universe;
	const WorldId world = MakeWorld(universe, "data-factory.restore");
	DataFactorySession session(universe);
	bool rehydrated = false;
	session.SetRehydrate([&](Universe &, WorldId, std::string &) {
		rehydrated = true;
		return true;
	});

	std::string checkpoint;
	const auto saved = session.Checkpoint("data-factory.restore", checkpoint);
	REQUIRE(saved.Status == DataFactoryStatus::Ok);
	REQUIRE(session.HasCheckpoint(checkpoint));

	universe.SetState(world, WorldState::Suspended);
	const auto restored = session.Restore("data-factory.restore", checkpoint);
	REQUIRE(restored.Status == DataFactoryStatus::Ok);
	CHECK(rehydrated);
	CHECK(restored.WorldEpoch == saved.WorldEpoch + 1);
	CHECK(restored.InstanceId == "data-factory.restore");
	CHECK(universe.StateOf(universe.Find(Name("data-factory.restore"))) == WorldState::Active);
	CHECK(session.HasCheckpoint(checkpoint));
	const auto restoredAgain = session.Restore("data-factory.restore", checkpoint);
	CHECK(restoredAgain.Status == DataFactoryStatus::Ok);
	CHECK(restoredAgain.WorldEpoch == restored.WorldEpoch + 1);
}

TEST_CASE("data-factory restores paused checkpoints with rebuilt systems", "[world][data-factory]") {
	Universe universe;
	const WorldId world = MakeWorld(universe, "data-factory.roundtrip");
	BuildCountingWorld(universe, world);
	DataFactorySession session(universe);
	session.SetPauseParticipant([](WorldId, DataFactoryPauseScope, bool, std::string &) { return true; });
	session.SetRehydrate([](Universe &candidate, WorldId restored, std::string &) {
		candidate.Enter(restored, [](Store &, Scheduler &systems) {
			systems.Add("data-factory.count", Phase::Simulation, [](Store &inner) {
				inner.Each<Tally>([](Entity, Tally &tally) { tally.Value++; });
			});
		});
		return true;
	});

	REQUIRE(
		session.Pause("data-factory.roundtrip", DataFactoryPauseScope::AllSystems, 0).Status ==
		DataFactoryStatus::Ok
	);
	REQUIRE(
		session.Step("data-factory.roundtrip", DataFactoryInterval{}, 0, 1).Status == DataFactoryStatus::Ok
	);
	CHECK(Count(universe, world) == 1);
	std::string checkpoint;
	REQUIRE(session.Checkpoint("data-factory.roundtrip", checkpoint).Status == DataFactoryStatus::Ok);
	REQUIRE(
		session.Step("data-factory.roundtrip", DataFactoryInterval{}, 1, 2).Status == DataFactoryStatus::Ok
	);
	CHECK(Count(universe, world) == 2);
	session.SetRehydrate([](Universe &, WorldId, std::string &detail) {
		detail = "deliberate rehydrate failure";
		return false;
	});
	CHECK(
		session.Restore("data-factory.roundtrip", checkpoint).Status == DataFactoryStatus::RestoreIncomplete
	);
	CHECK(Count(universe, world) == 2);
	session.SetRehydrate([](Universe &candidate, WorldId restored, std::string &) {
		candidate.Enter(restored, [](Store &, Scheduler &systems) {
			systems.Add("data-factory.count", Phase::Simulation, [](Store &inner) {
				inner.Each<Tally>([](Entity, Tally &tally) { tally.Value++; });
			});
		});
		return true;
	});

	const auto restored = session.Restore("data-factory.roundtrip", checkpoint);
	REQUIRE(restored.Status == DataFactoryStatus::Ok);
	REQUIRE(
		session.Step("data-factory.roundtrip", DataFactoryInterval{}, 1, restored.WorldVersion).Status ==
		DataFactoryStatus::Ok
	);
	CHECK(Count(universe, universe.Find(Name("data-factory.roundtrip"))) == 2);
	REQUIRE(session.Resume("data-factory.roundtrip", 2).Status == DataFactoryStatus::Ok);
}

TEST_CASE(
	"data-factory manual steps use active delta and do not tick other worlds", "[world][data-factory]"
) {
	Universe universe;
	WorldSettings settings;
	settings.Name = Name("data-factory.idle");
	settings.TickRate = 60.0;
	settings.IdleTickRate = 2.0;
	const WorldId paused = universe.Create(settings);
	const WorldId other = MakeWorld(universe, "data-factory.other");
	float delta = 0.0f;
	BuildCountingWorld(universe, paused, &delta);
	BuildCountingWorld(universe, other);
	universe.SetState(paused, WorldState::Idle);
	universe.Tick(0.1f);
	const int otherBefore = Count(universe, other);

	DataFactorySession session(universe);
	session.SetPauseParticipant([](WorldId, DataFactoryPauseScope, bool, std::string &) { return true; });
	REQUIRE(
		session.Pause("data-factory.idle", DataFactoryPauseScope::AllSystems, 0).Status ==
		DataFactoryStatus::Ok
	);
	REQUIRE(session.Step("data-factory.idle", DataFactoryInterval{}, 0, 1).Status == DataFactoryStatus::Ok);
	CHECK(delta == Approx(1.0f / 60.0f));
	CHECK(Count(universe, paused) == 1);
	CHECK(Count(universe, other) == otherBefore);
}

TEST_CASE("data-factory manual steps route each mailbox delivery once", "[world][data-factory]") {
	Universe universe;
	const WorldId world = MakeWorld(universe, "data-factory.mailbox");
	int deliveries = 0;
	universe.Enter(world, [&deliveries](Store &, Scheduler &systems) {
		systems.Add("data-factory.mailbox", Phase::PreSimulation, [&deliveries](Store &store) {
			deliveries += static_cast<int>(Postbox(store).Deliveries().size());
		});
	});

	DataFactorySession session(universe);
	session.SetPauseParticipant([](WorldId, DataFactoryPauseScope, bool, std::string &) { return true; });
	REQUIRE(
		session.Pause("data-factory.mailbox", DataFactoryPauseScope::AllSystems, 0).Status ==
		DataFactoryStatus::Ok
	);
	Delivery delivery;
	delivery.Payload = {std::byte{0x2a}};
	REQUIRE(universe.Deliver(Name("data-factory.mailbox"), delivery));
	CHECK(universe.StepPaused(WorldId{}) == engine::world::WorldStatus::NoSuchWorld);
	universe.Enter(world, [](Store &store) { CHECK(Postbox(store).Deliveries().empty()); });
	CHECK(universe.Statistics().Deliveries == 0);

	REQUIRE(
		session.Step("data-factory.mailbox", DataFactoryInterval{}, 0, 1).Status == DataFactoryStatus::Ok
	);
	CHECK(deliveries == 1);
	CHECK(universe.Statistics().Deliveries == 1);
	REQUIRE(
		session.Step("data-factory.mailbox", DataFactoryInterval{}, 1, 2).Status == DataFactoryStatus::Ok
	);
	CHECK(deliveries == 1);
	CHECK(universe.Statistics().Deliveries == 0);
}
