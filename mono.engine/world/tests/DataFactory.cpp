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
using engine::world::DataFactoryRenderOnlyRequest;
using engine::world::DataFactorySession;
using engine::world::DataFactoryStatus;
using engine::world::DataFactoryTemporalHistory;
using engine::world::DataFactoryWorldOperation;
using engine::world::DataFactoryWorldRequest;
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

TEST_CASE("data-factory external mutation commits one fresh paused revision", "[world][data-factory]") {
	Universe universe;
	MakeWorld(universe, "data-factory.external-mutation");
	DataFactorySession session(universe);
	session.SetPauseParticipant([](WorldId, DataFactoryPauseScope, bool, std::string &) { return true; });
	const auto paused = session.Pause("data-factory.external-mutation", DataFactoryPauseScope::AllSystems, 0);
	REQUIRE(paused.Status == DataFactoryStatus::Ok);
	const auto committed = session.CommitExternalMutation(
		"data-factory.external-mutation", paused.Clock.Tick, paused.WorldVersion
	);
	REQUIRE(committed.Status == DataFactoryStatus::Ok);
	CHECK(committed.WorldVersion == paused.WorldVersion + 1);
	CHECK(
		session
			.CommitExternalMutation("data-factory.external-mutation", paused.Clock.Tick, paused.WorldVersion)
			.Status == DataFactoryStatus::VersionConflict
	);
}

TEST_CASE(
	"data-factory render-only presentation keeps a paused snapshot unchanged", "[world][data-factory]"
) {
	Universe universe;
	const WorldId world = MakeWorld(universe, "data-factory.render-only");
	BuildCountingWorld(universe, world);
	DataFactorySession session(universe);
	session.SetPauseParticipant([](WorldId, DataFactoryPauseScope, bool, std::string &) { return true; });
	REQUIRE(
		session.Pause("data-factory.render-only", DataFactoryPauseScope::AllSystems, 0).Status ==
		DataFactoryStatus::Ok
	);
	std::string snapshot;
	REQUIRE(session.Snapshot("data-factory.render-only", snapshot).Status == DataFactoryStatus::Ok);
	const auto before = session.Inspect("data-factory.render-only");
	int presented = 0;
	session.SetRenderOnlyPresenter([&](const DataFactoryRenderOnlyRequest &request, std::string &) {
		presented++;
		CHECK(request.InstanceId == "data-factory.render-only");
		CHECK(request.SnapshotId == snapshot);
		CHECK(request.TemporalHistory == DataFactoryTemporalHistory::Preserve);
		CHECK(
			session
				.Step(
					"data-factory.render-only",
					DataFactoryInterval{},
					request.ExpectedTick,
					request.ExpectedWorldVersion
				)
				.Status == DataFactoryStatus::VersionConflict
		);
		return true;
	});

	const DataFactoryRenderOnlyRequest request{
		.InstanceId = "data-factory.render-only",
		.SnapshotId = snapshot,
		.ExpectedWorldEpoch = before.WorldEpoch,
		.ExpectedWorldVersion = before.WorldVersion,
		.ExpectedTick = before.Clock.Tick,
	};
	const auto rendered = session.RenderOnly(request);
	REQUIRE(rendered.Status == DataFactoryStatus::Pending);
	CHECK_FALSE(rendered.Presented);
	CHECK(rendered.OperationId != 0);
	CHECK(rendered.TemporalHistory == DataFactoryTemporalHistory::Preserve);
	CHECK(rendered.WorldEpoch == before.WorldEpoch);
	CHECK(rendered.WorldVersion == before.WorldVersion);
	CHECK(rendered.Clock.Tick == before.Clock.Tick);
	CHECK(rendered.Clock.TimeNanoseconds == before.Clock.TimeNanoseconds);
	CHECK(presented == 1);
	CHECK(
		session.PollRenderOnly("data-factory.render-only", rendered.OperationId).Status ==
		DataFactoryStatus::Pending
	);
	CHECK(
		session.Resume("data-factory.render-only", before.Clock.Tick).Status ==
		DataFactoryStatus::VersionConflict
	);
	CHECK(session.Restore("data-factory.render-only", snapshot).Status == DataFactoryStatus::VersionConflict);
	CHECK(
		session
			.ApplyIntervention(
				"data-factory.render-only", snapshot, {}, before.Clock.Tick, before.WorldVersion
			)
			.Status == DataFactoryStatus::VersionConflict
	);
	CHECK(
		session.ValidateRenderOnlySubmission("data-factory.render-only", rendered.OperationId).Status ==
		DataFactoryStatus::Pending
	);
	const auto completed = session.CompleteRenderOnly({
		.InstanceId = "data-factory.render-only",
		.OperationId = rendered.OperationId,
		.Submitted = true,
		.Detail = {},
	});
	CHECK(completed.Status == DataFactoryStatus::Ok);
	CHECK(completed.Presented);
	CHECK(
		session.PollRenderOnly("data-factory.render-only", rendered.OperationId).Status ==
		DataFactoryStatus::Ok
	);
	CHECK(Count(universe, world) == 0);
	CHECK(universe.StatisticsOf(world).Ticks == 0);
	CHECK(universe.StateOf(world) == WorldState::Suspended);
}

TEST_CASE(
	"data-factory render-only refuses stale and unsupported requests before the host", "[world][data-factory]"
) {
	Universe universe;
	MakeWorld(universe, "data-factory.render-refusal");
	DataFactorySession session(universe);
	session.SetPauseParticipant([](WorldId, DataFactoryPauseScope, bool, std::string &) { return true; });
	REQUIRE(
		session.Pause("data-factory.render-refusal", DataFactoryPauseScope::AllSystems, 0).Status ==
		DataFactoryStatus::Ok
	);
	std::string snapshot;
	REQUIRE(session.Snapshot("data-factory.render-refusal", snapshot).Status == DataFactoryStatus::Ok);
	const auto current = session.Inspect("data-factory.render-refusal");
	int presented = 0;
	session.SetRenderOnlyPresenter([&](const DataFactoryRenderOnlyRequest &, std::string &) {
		presented++;
		return true;
	});
	DataFactoryRenderOnlyRequest request{
		.InstanceId = "data-factory.render-refusal",
		.SnapshotId = snapshot,
		.ExpectedWorldEpoch = current.WorldEpoch,
		.ExpectedWorldVersion = current.WorldVersion,
		.ExpectedTick = current.Clock.Tick,
	};
	request.TemporalHistory = DataFactoryTemporalHistory::Reset;
	CHECK(session.RenderOnly(request).Status == DataFactoryStatus::Unsupported);
	request.TemporalHistory = DataFactoryTemporalHistory::Disable;
	CHECK(session.RenderOnly(request).Status == DataFactoryStatus::Unsupported);
	request.TemporalHistory = DataFactoryTemporalHistory::Preserve;
	request.ExpectedWorldEpoch++;
	CHECK(session.RenderOnly(request).Status == DataFactoryStatus::StaleSnapshot);
	request.ExpectedWorldEpoch = current.WorldEpoch;
	request.ExpectedWorldVersion++;
	CHECK(session.RenderOnly(request).Status == DataFactoryStatus::VersionConflict);
	request.ExpectedWorldVersion = current.WorldVersion;
	request.SnapshotId = "snapshot-not-retained";
	CHECK(session.RenderOnly(request).Status == DataFactoryStatus::StaleSnapshot);
	CHECK(presented == 0);
}

TEST_CASE(
	"data-factory render-only reports terminal failure after a queued frame cannot submit",
	"[world][data-factory]"
) {
	Universe universe;
	const WorldId world = MakeWorld(universe, "data-factory.render-terminal");
	DataFactorySession session(universe);
	session.SetPauseParticipant([](WorldId, DataFactoryPauseScope, bool, std::string &) { return true; });
	REQUIRE(
		session.Pause("data-factory.render-terminal", DataFactoryPauseScope::AllSystems, 0).Status ==
		DataFactoryStatus::Ok
	);
	std::string snapshot;
	REQUIRE(session.Snapshot("data-factory.render-terminal", snapshot).Status == DataFactoryStatus::Ok);
	const auto current = session.Inspect("data-factory.render-terminal");
	session.SetRenderOnlyPresenter([](const DataFactoryRenderOnlyRequest &, std::string &) { return true; });
	const auto queued = session.RenderOnly({
		.InstanceId = "data-factory.render-terminal",
		.SnapshotId = snapshot,
		.ExpectedWorldEpoch = current.WorldEpoch,
		.ExpectedWorldVersion = current.WorldVersion,
		.ExpectedTick = current.Clock.Tick,
	});
	REQUIRE(queued.Status == DataFactoryStatus::Pending);
	const auto failed = session.CompleteRenderOnly({
		.InstanceId = "data-factory.render-terminal",
		.OperationId = queued.OperationId,
		.Submitted = false,
		.Detail = "renderer lost its target",
	});
	CHECK(failed.Status == DataFactoryStatus::PresentationFailed);
	CHECK_FALSE(failed.Presented);
	CHECK(failed.Detail == "renderer lost its target");
	CHECK(
		session.PollRenderOnly("data-factory.render-terminal", queued.OperationId).Status ==
		DataFactoryStatus::PresentationFailed
	);
	CHECK(session.Resume("data-factory.render-terminal", current.Clock.Tick).Status == DataFactoryStatus::Ok);
	CHECK(universe.StateOf(world) == WorldState::Active);
}

TEST_CASE(
	"data-factory retains render-only terminals across a later request and resume", "[world][data-factory]"
) {
	Universe universe;
	MakeWorld(universe, "data-factory.render-history");
	DataFactorySession session(universe);
	session.SetPauseParticipant([](WorldId, DataFactoryPauseScope, bool, std::string &) { return true; });
	REQUIRE(
		session.Pause("data-factory.render-history", DataFactoryPauseScope::AllSystems, 0).Status ==
		DataFactoryStatus::Ok
	);
	std::string snapshot;
	REQUIRE(session.Snapshot("data-factory.render-history", snapshot).Status == DataFactoryStatus::Ok);
	const auto current = session.Inspect("data-factory.render-history");
	session.SetRenderOnlyPresenter([](const DataFactoryRenderOnlyRequest &, std::string &) { return true; });
	const auto request = [&] {
		return DataFactoryRenderOnlyRequest{
			.InstanceId = "data-factory.render-history",
			.SnapshotId = snapshot,
			.ExpectedWorldEpoch = current.WorldEpoch,
			.ExpectedWorldVersion = current.WorldVersion,
			.ExpectedTick = current.Clock.Tick,
		};
	};
	const auto first = session.RenderOnly(request());
	REQUIRE(first.Status == DataFactoryStatus::Pending);
	REQUIRE(
		session
			.CompleteRenderOnly({
				.InstanceId = "data-factory.render-history",
				.OperationId = first.OperationId,
				.Submitted = true,
				.Detail = {},
			})
			.Status == DataFactoryStatus::Ok
	);
	const auto second = session.RenderOnly(request());
	REQUIRE(second.Status == DataFactoryStatus::Pending);
	CHECK(
		session.PollRenderOnly("data-factory.render-history", first.OperationId).Status ==
		DataFactoryStatus::Ok
	);
	REQUIRE(
		session
			.CompleteRenderOnly({
				.InstanceId = "data-factory.render-history",
				.OperationId = second.OperationId,
				.Submitted = true,
				.Detail = {},
			})
			.Status == DataFactoryStatus::Ok
	);
	REQUIRE(
		session.Resume("data-factory.render-history", current.Clock.Tick).Status == DataFactoryStatus::Ok
	);
	CHECK(
		session.PollRenderOnly("data-factory.render-history", first.OperationId).Status ==
		DataFactoryStatus::Ok
	);
	CHECK(
		session.PollRenderOnly("data-factory.render-history", second.OperationId).Status ==
		DataFactoryStatus::Ok
	);
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

TEST_CASE("data-factory owns create reset retire lifecycle state", "[world][data-factory]") {
	Universe universe;
	DataFactorySession session(universe);
	session.SetPauseParticipant([](WorldId, DataFactoryPauseScope, bool, std::string &) { return true; });
	const DataFactoryWorldRequest create{
		.Operation = DataFactoryWorldOperation::Create,
		.InstanceId = "factory.lifecycle",
		.Seed = 42,
		.TickRate = 60.0,
		.OperationId = "create-1",
	};
	const auto created = session.CreateWorld(create);
	REQUIRE(created.Status == DataFactoryStatus::Ok);
	CHECK(created.InstanceId == "factory.lifecycle");
	CHECK(created.Clock.Tick == 0);
	CHECK(universe.Find(Name("factory.lifecycle")).IsValid());
	CHECK(session.CreateWorld(create).Status == DataFactoryStatus::Ok);
	DataFactoryWorldRequest second = create;
	second.InstanceId = "factory.lifecycle.second";
	second.OperationId = "create-second";
	CHECK(session.CreateWorld(second).Status == DataFactoryStatus::ResourceLimit);
	const auto conflict = DataFactoryWorldRequest{
		.Operation = DataFactoryWorldOperation::Create,
		.InstanceId = "factory.lifecycle",
		.Seed = 43,
		.TickRate = 60.0,
		.OperationId = "create-1",
	};
	CHECK(session.CreateWorld(conflict).Status == DataFactoryStatus::OperationIdConflict);
	const DataFactoryWorldRequest staleReset{
		.Operation = DataFactoryWorldOperation::Reset,
		.InstanceId = "factory.lifecycle",
		.Seed = 43,
		.TickRate = 30.0,
		.ExpectedWorldEpoch = created.WorldEpoch,
		.ExpectedWorldVersion = created.WorldVersion,
		.ExpectedTick = created.Clock.Tick + 1,
		.OperationId = "reset-stale",
	};
	CHECK(session.ResetWorld(staleReset).Status == DataFactoryStatus::VersionConflict);
	CHECK(session.AllSystemsPaused("factory.lifecycle"));

	bool externalPaused = false;
	session.SetPauseParticipant(
		[&externalPaused](WorldId, DataFactoryPauseScope, bool paused, std::string &) {
			externalPaused = paused;
			return true;
		}
	);
	REQUIRE(
		session.Pause("factory.lifecycle", DataFactoryPauseScope::AllSystems, created.Clock.Tick).Status ==
		DataFactoryStatus::Ok
	);
	CHECK_FALSE(externalPaused);
	const auto paused = session.Inspect("factory.lifecycle");
	std::string snapshotId;
	REQUIRE(session.Snapshot("factory.lifecycle", snapshotId).Status == DataFactoryStatus::Ok);
	REQUIRE(session.HasCheckpoint(snapshotId));
	DataFactoryWorldRequest reset{
		.Operation = DataFactoryWorldOperation::Reset,
		.InstanceId = "factory.lifecycle",
		.Seed = 43,
		.TickRate = 30.0,
		.ExpectedWorldEpoch = paused.WorldEpoch,
		.ExpectedWorldVersion = paused.WorldVersion,
		.ExpectedTick = paused.Clock.Tick,
		.OperationId = "reset-1",
	};
	const auto resetReply = session.ResetWorld(reset);
	REQUIRE(resetReply.Status == DataFactoryStatus::Ok);
	CHECK(resetReply.Clock.Tick == 0);
	CHECK(resetReply.WorldEpoch == paused.WorldEpoch + 1);
	CHECK(resetReply.WorldVersion == paused.WorldVersion + 1);
	CHECK(universe.SettingsOf(universe.Find(Name("factory.lifecycle"))).TickRate == 30.0);
	CHECK_FALSE(session.HasCheckpoint(snapshotId));
	CHECK(externalPaused);
	CHECK(session.ResetWorld(reset).Status == DataFactoryStatus::Ok);
	const auto replayedCreate = session.CreateWorld(create);
	CHECK(replayedCreate.Status == DataFactoryStatus::Ok);
	CHECK(replayedCreate.WorldEpoch == created.WorldEpoch);
	CHECK(replayedCreate.WorldVersion == created.WorldVersion);
	CHECK(universe.Find(Name("factory.lifecycle")).IsValid());

	const auto afterReset = session.Inspect("factory.lifecycle");
	REQUIRE(
		session.Pause("factory.lifecycle", DataFactoryPauseScope::AllSystems, afterReset.Clock.Tick).Status ==
		DataFactoryStatus::Ok
	);
	const auto pausedReset = session.Inspect("factory.lifecycle");
	REQUIRE(
		session
			.Step(
				"factory.lifecycle",
				DataFactoryInterval{.NumeratorNanoseconds = 1'000'000'000, .Denominator = 30},
				pausedReset.Clock.Tick,
				pausedReset.WorldVersion
			)
			.Status == DataFactoryStatus::Ok
	);
	const auto finalState = session.Inspect("factory.lifecycle");
	session.SetPauseParticipant([](WorldId, DataFactoryPauseScope, bool paused, std::string &detail) {
		if (!paused) {
			detail = "audio device refused resume";
			return false;
		}
		return true;
	});
	DataFactoryWorldRequest retire{
		.Operation = DataFactoryWorldOperation::Retire,
		.InstanceId = "factory.lifecycle",
		.ExpectedWorldEpoch = finalState.WorldEpoch,
		.ExpectedWorldVersion = finalState.WorldVersion,
		.ExpectedTick = finalState.Clock.Tick,
		.OperationId = "retire-1",
	};
	CHECK(session.RetireWorld(retire).Status == DataFactoryStatus::RestoreIncomplete);
	CHECK(universe.Find(Name("factory.lifecycle")).IsValid());
	session.SetPauseParticipant(
		[&externalPaused](WorldId, DataFactoryPauseScope, bool paused, std::string &) {
			externalPaused = paused;
			return true;
		}
	);
	const auto retired = session.RetireWorld(retire);
	CHECK(retired.Status == DataFactoryStatus::Ok);
	CHECK(retired.Tombstone);
	CHECK(retired.InstanceId == "factory.lifecycle");
	CHECK(retired.Clock.Tick == finalState.Clock.Tick);
	CHECK(retired.Clock.Tick == 1);
	CHECK(retired.WorldEpoch == finalState.WorldEpoch);
	CHECK(retired.WorldVersion == finalState.WorldVersion + 1);
	CHECK_FALSE(universe.Find(Name("factory.lifecycle")).IsValid());
	CHECK_FALSE(externalPaused);
	CHECK(session.RetireWorld(retire).Status == DataFactoryStatus::Ok);
	const auto retiredCreateReplay = session.CreateWorld(create);
	CHECK(retiredCreateReplay.Status == DataFactoryStatus::Ok);
	CHECK(retiredCreateReplay.WorldEpoch == created.WorldEpoch);
	CHECK_FALSE(universe.Find(Name("factory.lifecycle")).IsValid());
}

TEST_CASE("data-factory prepares a scratch universe before lifecycle replacement", "[world][data-factory]") {
	Universe universe;
	DataFactorySession session(universe);
	session.SetPauseParticipant([](WorldId, DataFactoryPauseScope, bool, std::string &) { return true; });
	const DataFactoryWorldRequest create{
		.Operation = DataFactoryWorldOperation::Create,
		.InstanceId = "factory.transaction",
		.Seed = 7,
		.TickRate = 60.0,
		.OperationId = "transaction-create",
	};
	REQUIRE(session.CreateWorld(create).Status == DataFactoryStatus::Ok);
	const WorldId original = universe.Find(Name("factory.transaction"));
	REQUIRE(original.IsValid());

	bool externallyPaused = false;
	session.SetPauseParticipant(
		[&externallyPaused](WorldId, DataFactoryPauseScope, bool paused, std::string &) {
			externallyPaused = paused;
			return true;
		}
	);
	REQUIRE(
		session.Pause("factory.transaction", DataFactoryPauseScope::AllSystems, 0).Status ==
		DataFactoryStatus::Ok
	);
	const auto paused = session.Inspect("factory.transaction");

	bool stagedReset = false;
	bool committed = false;
	session.SetWorldLifecycle([&](DataFactoryWorldOperation operation,
								  Universe &candidate,
								  WorldId world,
								  bool isCommitted,
								  std::string &detail) {
		if (isCommitted) {
			committed = true;
			return false;
		}
		if (operation != DataFactoryWorldOperation::Reset) return true;
		stagedReset = &candidate != &universe && candidate.SettingsOf(world).TickRate == 30.0;
		detail = "product setup rejected candidate";
		return false;
	});
	const DataFactoryWorldRequest rejectedReset{
		.Operation = DataFactoryWorldOperation::Reset,
		.InstanceId = "factory.transaction",
		.Seed = 8,
		.TickRate = 30.0,
		.ExpectedWorldEpoch = paused.WorldEpoch,
		.ExpectedWorldVersion = paused.WorldVersion,
		.ExpectedTick = paused.Clock.Tick,
		.OperationId = "transaction-reset-rejected",
	};
	const auto rejected = session.ResetWorld(rejectedReset);
	CHECK(rejected.Status == DataFactoryStatus::RestoreIncomplete);
	CHECK(stagedReset);
	CHECK_FALSE(committed);
	CHECK(universe.Find(Name("factory.transaction")) == original);
	CHECK(universe.SettingsOf(original).TickRate == 60.0);
	const auto afterRejected = session.Inspect("factory.transaction");
	CHECK(afterRejected.WorldEpoch == paused.WorldEpoch);
	CHECK(afterRejected.WorldVersion == paused.WorldVersion);
	CHECK(afterRejected.Clock.Tick == paused.Clock.Tick);
	CHECK(session.AllSystemsPaused("factory.transaction"));
	CHECK_FALSE(externallyPaused);

	session.SetWorldLifecycle(
		[&committed](DataFactoryWorldOperation, Universe &, WorldId, bool isCommitted, std::string &) {
			if (isCommitted) committed = true;
			return true;
		}
	);
	const DataFactoryWorldRequest acceptedReset{
		.Operation = DataFactoryWorldOperation::Reset,
		.InstanceId = "factory.transaction",
		.Seed = 8,
		.TickRate = 30.0,
		.ExpectedWorldEpoch = afterRejected.WorldEpoch,
		.ExpectedWorldVersion = afterRejected.WorldVersion,
		.ExpectedTick = afterRejected.Clock.Tick,
		.OperationId = "transaction-reset-accepted",
	};
	CHECK(session.ResetWorld(acceptedReset).Status == DataFactoryStatus::Ok);
	CHECK(committed);
	CHECK(universe.SettingsOf(universe.Find(Name("factory.transaction"))).TickRate == 30.0);
}

TEST_CASE("data-factory refuses create when candidate preparation fails", "[world][data-factory]") {
	Universe universe;
	DataFactorySession session(universe);
	session.SetPauseParticipant([](WorldId, DataFactoryPauseScope, bool, std::string &) { return true; });
	bool staged = false;
	session.SetWorldLifecycle([&](DataFactoryWorldOperation operation,
								  Universe &candidate,
								  WorldId world,
								  bool committed,
								  std::string &detail) {
		if (committed || operation != DataFactoryWorldOperation::Create) return true;
		staged = &candidate != &universe && world.IsValid();
		detail = "product setup rejected candidate";
		return false;
	});
	const auto reply = session.CreateWorld(
		DataFactoryWorldRequest{
			.Operation = DataFactoryWorldOperation::Create,
			.InstanceId = "factory.create-rejected",
			.Seed = 9,
			.TickRate = 60.0,
			.OperationId = "transaction-create-rejected",
		}
	);
	CHECK(reply.Status == DataFactoryStatus::RestoreIncomplete);
	CHECK(staged);
	CHECK_FALSE(universe.Find(Name("factory.create-rejected")).IsValid());
	CHECK(universe.Worlds().empty());
}

TEST_CASE(
	"data-factory retains lifecycle retries until its bounded ledger is full", "[world][data-factory]"
) {
	Universe universe;
	DataFactorySession session(universe);
	session.SetPauseParticipant([](WorldId, DataFactoryPauseScope, bool, std::string &) { return true; });
	const DataFactoryWorldRequest create{
		.Operation = DataFactoryWorldOperation::Create,
		.InstanceId = "factory.ledger",
		.TickRate = 60.0,
		.OperationId = "ledger-create",
	};
	REQUIRE(session.CreateWorld(create).Status == DataFactoryStatus::Ok);
	session.SetPauseParticipant([](WorldId, DataFactoryPauseScope, bool, std::string &) { return true; });
	for (size_t index = 0; index < 255; ++index) {
		const auto before = session.Inspect("factory.ledger");
		REQUIRE(
			session.Pause("factory.ledger", DataFactoryPauseScope::AllSystems, before.Clock.Tick).Status ==
			DataFactoryStatus::Ok
		);
		CHECK(
			session
				.ResetWorld(
					DataFactoryWorldRequest{
						.Operation = DataFactoryWorldOperation::Reset,
						.InstanceId = "factory.ledger",
						.TickRate = 60.0,
						.ExpectedWorldEpoch = before.WorldEpoch,
						.ExpectedWorldVersion = before.WorldVersion,
						.ExpectedTick = before.Clock.Tick,
						.OperationId = "ledger-reset-" + std::to_string(index),
					}
				)
				.Status == DataFactoryStatus::Ok
		);
	}
	const auto full = session.Inspect("factory.ledger");
	REQUIRE(
		session.Pause("factory.ledger", DataFactoryPauseScope::AllSystems, full.Clock.Tick).Status ==
		DataFactoryStatus::Ok
	);
	CHECK(
		session
			.ResetWorld(
				DataFactoryWorldRequest{
					.Operation = DataFactoryWorldOperation::Reset,
					.InstanceId = "factory.ledger",
					.TickRate = 60.0,
					.ExpectedWorldEpoch = full.WorldEpoch,
					.ExpectedWorldVersion = full.WorldVersion,
					.ExpectedTick = full.Clock.Tick,
					.OperationId = "ledger-overflow",
				}
			)
			.Status == DataFactoryStatus::ResourceLimit
	);
	CHECK(session.Inspect("factory.ledger").WorldVersion == full.WorldVersion);
}
