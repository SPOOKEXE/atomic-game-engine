#include "DataFactoryRevision.hpp"

#include <engine/core/Bytes.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/DataFactory.hpp>
#include <engine/world/Postbox.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <limits>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <vector>

TEST_SUITE_ID("engine.world.data-factory")

using Catch::Approx;
using engine::core::Name;
using engine::ecs::Entity;
using engine::ecs::Phase;
using engine::ecs::Scheduler;
using engine::ecs::Store;
using engine::world::BusKind;
using engine::world::DataFactoryAction;
using engine::world::DataFactoryActionCommit;
using engine::world::DataFactoryForkRequest;
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
	std::vector<std::byte> Bytes(std::string_view text) {
		const auto *first = reinterpret_cast<const std::byte *>(text.data());
		return {first, first + text.size()};
	}

	struct Tally {
		int Value = 0;
	};
	struct UnserializableResource {
		std::string Value;
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

TEST_CASE("data-factory lifecycle revisions refuse numeric exhaustion", "[world][data-factory]") {
	using engine::world::detail::CanAdvanceDataFactoryRevision;
	constexpr uint64_t maximum = std::numeric_limits<uint64_t>::max();
	CHECK(CanAdvanceDataFactoryRevision(maximum - 1, maximum - 1, true));
	CHECK_FALSE(CanAdvanceDataFactoryRevision(maximum, maximum - 1, true));
	CHECK_FALSE(CanAdvanceDataFactoryRevision(maximum - 1, maximum, true));
	CHECK(CanAdvanceDataFactoryRevision(maximum, maximum - 1, false));
}

TEST_CASE("prepared external mutation is session-bound and revalidated", "[world][data-factory][prepared]") {
	Universe universe;
	DataFactorySession session(universe);
	session.SetPauseParticipant([](WorldId, DataFactoryPauseScope, bool, std::string &) { return true; });
	const DataFactoryWorldRequest create{
		.Operation = DataFactoryWorldOperation::Create,
		.InstanceId = "prepared.factory",
		.TickRate = 60.0,
		.OperationId = "prepared-create",
	};
	const auto created = session.CreateWorld(create);
	REQUIRE(created.Status == DataFactoryStatus::Ok);
	auto token =
		session.PrepareExternalMutation("prepared.factory", created.Clock.Tick, created.WorldVersion);
	REQUIRE(token.Reply().Status == DataFactoryStatus::Ok);
	CHECK(token.Reply().WorldVersion == created.WorldVersion + 1);
	DataFactorySession other(universe);
	CHECK_FALSE(other.CommitPreparedExternalMutation(token));
	CHECK(session.CanCommitPreparedExternalMutation(token));
	CHECK(session.CommitPreparedExternalMutation(token));
	CHECK_FALSE(session.CommitPreparedExternalMutation(token));
	CHECK(session.Inspect("prepared.factory").WorldVersion == created.WorldVersion + 1);
	const auto current = session.Inspect("prepared.factory");
	auto stale =
		session.PrepareExternalMutation("prepared.factory", current.Clock.Tick, current.WorldVersion);
	REQUIRE(stale.Reply().Status == DataFactoryStatus::Ok);
	const auto world = universe.Find(Name("prepared.factory"));
	REQUIRE(universe.SetState(world, WorldState::Active) == engine::world::WorldStatus::Ok);
	CHECK_FALSE(session.CanCommitPreparedExternalMutation(stale));
	CHECK_FALSE(session.CommitPreparedExternalMutation(stale));
	CHECK(session.Inspect("prepared.factory").WorldVersion == current.WorldVersion);
	CHECK(
		session.CommitExternalMutation("prepared.factory", current.Clock.Tick, current.WorldVersion).Status ==
		DataFactoryStatus::VersionConflict
	);
}

TEST_CASE("data-factory adopts a loaded world without replacing its state", "[world][data-factory]") {
	Universe universe;
	const WorldId world = MakeWorld(universe, "authored.scene");
	BuildCountingWorld(universe, world);
	DataFactorySession session(universe);
	CHECK_FALSE(session.OwnsWorld("authored.scene"));
	const auto adopted = session.AdoptWorld("authored.scene");
	REQUIRE(adopted.Status == DataFactoryStatus::Ok);
	CHECK(adopted.WorldVersion == 1);
	CHECK(session.OwnsWorld("authored.scene"));
	CHECK(universe.Find(Name("authored.scene")) == world);
	CHECK(session.AdoptWorld("authored.scene").Status == DataFactoryStatus::VersionConflict);
	session.SetPauseParticipant([](WorldId, DataFactoryPauseScope, bool, std::string &) { return true; });
	const auto paused = session.Pause("authored.scene", DataFactoryPauseScope::AllSystems, 0);
	REQUIRE(paused.Status == DataFactoryStatus::Ok);
	CHECK(paused.WorldVersion == 2);
	CHECK(universe.StateOf(world) == WorldState::Suspended);
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

TEST_CASE("data-factory failed resume rollback is a replay barrier", "[world][data-factory]") {
	Universe universe;
	MakeWorld(universe, "data-factory.resume-replay-barrier");
	DataFactorySession session(universe);
	session.SetRehydrate([](Universe &, WorldId, std::string &) { return true; });
	bool rejectPhysicsResume = false;
	bool rejectAllSystemsRollback = false;
	session.SetPauseParticipant([&](WorldId, DataFactoryPauseScope scope, bool paused, std::string &) {
		if (scope == DataFactoryPauseScope::PhysicsOnly && !paused && rejectPhysicsResume) return false;
		if (scope == DataFactoryPauseScope::AllSystems && paused && rejectAllSystemsRollback) return false;
		return true;
	});
	REQUIRE(
		session.Pause("data-factory.resume-replay-barrier", DataFactoryPauseScope::AllSystems, 0).Status ==
		DataFactoryStatus::Ok
	);
	std::string checkpoint;
	REQUIRE(
		session.Checkpoint("data-factory.resume-replay-barrier", checkpoint).Status == DataFactoryStatus::Ok
	);
	auto revision = session.Inspect("data-factory.resume-replay-barrier");
	REQUIRE(
		session.Step("data-factory.resume-replay-barrier", DataFactoryInterval{}, 0, revision.WorldVersion)
			.Status == DataFactoryStatus::Ok
	);
	REQUIRE(
		session.Pause("data-factory.resume-replay-barrier", DataFactoryPauseScope::PhysicsOnly, 1).Status ==
		DataFactoryStatus::Ok
	);
	rejectPhysicsResume = true;
	rejectAllSystemsRollback = true;
	CHECK(
		session.Resume("data-factory.resume-replay-barrier", 1).Status == DataFactoryStatus::RestoreIncomplete
	);
	rejectPhysicsResume = false;
	rejectAllSystemsRollback = false;
	REQUIRE(
		session.Pause("data-factory.resume-replay-barrier", DataFactoryPauseScope::AllSystems, 1).Status ==
		DataFactoryStatus::Ok
	);
	revision = session.Inspect("data-factory.resume-replay-barrier");
	CHECK(
		session.SeekBackward("data-factory.resume-replay-barrier", 0, 1, revision.WorldVersion).Status ==
		DataFactoryStatus::StaleSnapshot
	);
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
	"data-factory render-only rejects non-pending operation ids without advancing instances",
	"[world][data-factory]"
) {
	Universe firstUniverse;
	Universe secondUniverse;
	DataFactorySession first(firstUniverse);
	DataFactorySession second(secondUniverse);
	const auto prepare = [](Universe &universe,
							DataFactorySession &session,
							const char *instanceId,
							std::string &snapshot) {
		MakeWorld(universe, instanceId);
		session.SetPauseParticipant([](WorldId, DataFactoryPauseScope, bool, std::string &) { return true; });
		REQUIRE(
			session.Pause(instanceId, DataFactoryPauseScope::AllSystems, 0).Status == DataFactoryStatus::Ok
		);
		REQUIRE(session.Snapshot(instanceId, snapshot).Status == DataFactoryStatus::Ok);
		const auto before = session.Inspect(instanceId);
		CHECK(before.Clock.Tick == 0);
		CHECK(before.Clock.TimeNanoseconds == 0);
		CHECK(before.WorldVersion == 1);
		session.SetRenderOnlyPresenter([](const DataFactoryRenderOnlyRequest &, std::string &) {
			return true;
		});
	};
	const auto queue = [](DataFactorySession &session, const char *instanceId, const std::string &snapshot) {
		const auto before = session.Inspect(instanceId);
		return session.RenderOnly({
			.InstanceId = instanceId,
			.SnapshotId = snapshot,
			.ExpectedWorldEpoch = before.WorldEpoch,
			.ExpectedWorldVersion = before.WorldVersion,
			.ExpectedTick = before.Clock.Tick,
		});
	};
	std::string firstSnapshot;
	std::string secondSnapshot;
	prepare(firstUniverse, first, "data-factory.render-operation.first", firstSnapshot);
	prepare(secondUniverse, second, "data-factory.render-operation.second", secondSnapshot);
	const auto consumed = queue(first, "data-factory.render-operation.first", firstSnapshot);
	REQUIRE(consumed.Status == DataFactoryStatus::Pending);
	REQUIRE(
		first
			.CompleteRenderOnly({
				.InstanceId = "data-factory.render-operation.first",
				.OperationId = consumed.OperationId,
				.Submitted = true,
				.Detail = {},
			})
			.Status == DataFactoryStatus::Ok
	);
	const auto firstQueued = queue(first, "data-factory.render-operation.first", firstSnapshot);
	const auto secondQueued = queue(second, "data-factory.render-operation.second", secondSnapshot);
	REQUIRE(firstQueued.Status == DataFactoryStatus::Pending);
	REQUIRE(secondQueued.Status == DataFactoryStatus::Pending);
	REQUIRE(firstQueued.OperationId != secondQueued.OperationId);

	const auto foreignFirst = first.CompleteRenderOnly({
		.InstanceId = "data-factory.render-operation.first",
		.OperationId = secondQueued.OperationId,
		.Submitted = true,
		.Detail = {},
	});
	CHECK(foreignFirst.Status == DataFactoryStatus::ValidationFailed);
	CHECK(
		second
			.CompleteRenderOnly({
				.InstanceId = "data-factory.render-operation.second",
				.OperationId = firstQueued.OperationId,
				.Submitted = true,
				.Detail = {},
			})
			.Status == DataFactoryStatus::ValidationFailed
	);
	CHECK(
		first.PollRenderOnly("data-factory.render-operation.first", firstQueued.OperationId).Status ==
		DataFactoryStatus::Pending
	);
	CHECK(
		second.PollRenderOnly("data-factory.render-operation.second", secondQueued.OperationId).Status ==
		DataFactoryStatus::Pending
	);

	const auto firstCompleted = first.CompleteRenderOnly({
		.InstanceId = "data-factory.render-operation.first",
		.OperationId = firstQueued.OperationId,
		.Submitted = true,
		.Detail = {},
	});
	const auto secondCompleted = second.CompleteRenderOnly({
		.InstanceId = "data-factory.render-operation.second",
		.OperationId = secondQueued.OperationId,
		.Submitted = true,
		.Detail = {},
	});
	CHECK(firstCompleted.Status == DataFactoryStatus::Ok);
	CHECK(secondCompleted.Status == DataFactoryStatus::Ok);
	CHECK(
		first
			.CompleteRenderOnly({
				.InstanceId = "data-factory.render-operation.first",
				.OperationId = firstQueued.OperationId,
				.Submitted = true,
				.Detail = {},
			})
			.Status == DataFactoryStatus::ValidationFailed
	);
	const auto firstAfter = first.Inspect("data-factory.render-operation.first");
	const auto secondAfter = second.Inspect("data-factory.render-operation.second");
	CHECK(firstAfter.Clock.Tick == 0);
	CHECK(firstAfter.Clock.TimeNanoseconds == 0);
	CHECK(firstAfter.WorldVersion == 1);
	CHECK(secondAfter.Clock.Tick == 0);
	CHECK(secondAfter.Clock.TimeNanoseconds == 0);
	CHECK(secondAfter.WorldVersion == 1);
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

TEST_CASE("checkpoint restore preserves a nested committed live edit", "[world][data-factory]") {
	Universe universe;
	const WorldId world = MakeWorld(universe, "data-factory.restore-nested");
	BuildCountingWorld(universe, world);
	DataFactorySession session(universe);
	session.SetPauseParticipant([](WorldId, DataFactoryPauseScope, bool, std::string &) { return true; });
	session.SetRehydrate([](Universe &, WorldId, std::string &) { return true; });
	REQUIRE(
		session.Pause("data-factory.restore-nested", DataFactoryPauseScope::AllSystems, 0).Status ==
		DataFactoryStatus::Ok
	);
	std::string checkpoint;
	const auto saved = session.Checkpoint("data-factory.restore-nested", checkpoint);
	REQUIRE(saved.Status == DataFactoryStatus::Ok);
	DataFactoryStatus nested = DataFactoryStatus::Unsupported;
	bool commit = false;
	int outcome = 0;
	SECTION("committed edit changes the session revision") {
		commit = true;
	}
	SECTION("untracked edit changes the serialized live state") {}
	SECTION("a refused rehydration still invalidates the revision") {
		outcome = 1;
	}
	SECTION("a throwing rehydration still invalidates the revision") {
		outcome = 2;
	}
	bool aborted = false;
	session.SetRehydrator(
		{.Prepare =
			 [&](Universe &, WorldId, std::string &) {
				 universe.Enter(world, [](Store &store) {
					 store.Each<Tally>([](Entity, Tally &tally) { tally.Value = 42; });
				 });
				 if (commit)
					 nested = session
								  .CommitExternalMutation(
									  "data-factory.restore-nested", saved.Clock.Tick, saved.WorldVersion
								  )
								  .Status;
				 if (outcome == 2) throw std::runtime_error("rehydration failure");
				 return outcome != 1;
			 },
		 .Commit = {},
		 .Abort =
			 [&] {
				 aborted = true;
				 throw std::runtime_error("abort failure");
			 }}
	);
	const auto restored = session.Restore("data-factory.restore-nested", checkpoint);
	if (commit) REQUIRE(nested == DataFactoryStatus::Ok);
	CHECK(restored.Status != DataFactoryStatus::Ok);
	CHECK(Count(universe, world) == 42);
	CHECK(session.Inspect("data-factory.restore-nested").WorldVersion == saved.WorldVersion + 1);
	CHECK(aborted);
}

TEST_CASE("checkpoint restore permits the host to reconcile physics pause", "[world][data-factory]") {
	Universe universe;
	const WorldId world = MakeWorld(universe, "data-factory.restore-pause");
	BuildCountingWorld(universe, world);
	DataFactorySession session(universe);
	session.SetRehydrate([](Universe &, WorldId, std::string &) { return true; });
	bool reconciled = false;
	session.SetPauseParticipant([&](WorldId target, DataFactoryPauseScope scope, bool paused, std::string &) {
		if (scope == DataFactoryPauseScope::PhysicsOnly && !paused) {
			universe.Enter(target, [](Store &store) {
				store.Each<Tally>([](Entity, Tally &tally) { tally.Value = 17; });
			});
			reconciled = true;
		}
		return true;
	});
	std::string checkpoint;
	REQUIRE(session.Checkpoint("data-factory.restore-pause", checkpoint).Status == DataFactoryStatus::Ok);
	REQUIRE(
		session.Pause("data-factory.restore-pause", DataFactoryPauseScope::PhysicsOnly, 0).Status ==
		DataFactoryStatus::Ok
	);
	CHECK(session.Restore("data-factory.restore-pause", checkpoint).Status == DataFactoryStatus::Ok);
	CHECK(reconciled);
}

TEST_CASE("checkpoint restore permits an unserializable live world", "[world][data-factory]") {
	Universe universe;
	const WorldId world = MakeWorld(universe, "data-factory.restore-unserializable");
	DataFactorySession session(universe);
	session.SetRehydrate([](Universe &, WorldId, std::string &) { return true; });
	std::string checkpoint;
	REQUIRE(
		session.Checkpoint("data-factory.restore-unserializable", checkpoint).Status == DataFactoryStatus::Ok
	);
	universe.Enter(world, [](Store &store) { store.SetResource(UnserializableResource{"temporary"}); });
	CHECK(session.Restore("data-factory.restore-unserializable", checkpoint).Status == DataFactoryStatus::Ok);
}

TEST_CASE("checkpoint restore detects live edits made by abort", "[world][data-factory]") {
	Universe universe;
	const WorldId world = MakeWorld(universe, "data-factory.restore-abort-edit");
	BuildCountingWorld(universe, world);
	DataFactorySession session(universe);
	session.SetRehydrator(
		{.Prepare = [](Universe &, WorldId, std::string &) { return false; },
		 .Commit = {},
		 .Abort =
			 [&] {
				 universe.Enter(world, [](Store &store) {
					 store.Each<Tally>([](Entity, Tally &tally) { tally.Value = 43; });
				 });
			 }}
	);
	std::string checkpoint;
	const auto saved = session.Checkpoint("data-factory.restore-abort-edit", checkpoint);
	REQUIRE(saved.Status == DataFactoryStatus::Ok);
	CHECK(session.Restore("data-factory.restore-abort-edit", checkpoint).Status != DataFactoryStatus::Ok);
	CHECK(Count(universe, world) == 43);
	CHECK(session.Inspect("data-factory.restore-abort-edit").WorldVersion == saved.WorldVersion + 1);
}

TEST_CASE("checkpoint restore invalidates pause claims if rollback fails", "[world][data-factory]") {
	Universe universe;
	MakeWorld(universe, "data-factory.restore-rollback");
	DataFactorySession session(universe);
	session.SetRehydrate([](Universe &, WorldId, std::string &) { return true; });
	bool restoring = false;
	session.SetPauseParticipant([&](WorldId, DataFactoryPauseScope scope, bool paused, std::string &) {
		if (!restoring) return true;
		if (scope == DataFactoryPauseScope::AllSystems && !paused) return true;
		return false;
	});
	std::string checkpoint;
	REQUIRE(session.Checkpoint("data-factory.restore-rollback", checkpoint).Status == DataFactoryStatus::Ok);
	REQUIRE(
		session.Pause("data-factory.restore-rollback", DataFactoryPauseScope::AllSystems, 0).Status ==
		DataFactoryStatus::Ok
	);
	REQUIRE(
		session.Pause("data-factory.restore-rollback", DataFactoryPauseScope::PhysicsOnly, 0).Status ==
		DataFactoryStatus::Ok
	);
	const auto before = session.Inspect("data-factory.restore-rollback");
	restoring = true;
	CHECK(
		session.Restore("data-factory.restore-rollback", checkpoint).Status ==
		DataFactoryStatus::RestoreIncomplete
	);
	CHECK_FALSE(session.AllSystemsPaused("data-factory.restore-rollback"));
	CHECK(session.Inspect("data-factory.restore-rollback").WorldVersion > before.WorldVersion);
}

TEST_CASE("checkpoint restore preserves the pause of a nested replacement", "[world][data-factory]") {
	Universe universe;
	DataFactorySession session(universe);
	session.SetRehydrate([](Universe &, WorldId, std::string &) { return true; });
	bool restoring = false;
	bool requested = false;
	DataFactoryStatus nestedStatus = DataFactoryStatus::Unsupported;
	DataFactoryWorldRequest create{
		.InstanceId = "data-factory.restore-nested-reset",
		.Seed = 1,
		.TickRate = 60.0,
		.OperationId = "restore-nested-create"
	};
	session.SetPauseParticipant([&](WorldId, DataFactoryPauseScope scope, bool paused, std::string &) {
		if (restoring && !requested && scope == DataFactoryPauseScope::AllSystems && paused) {
			requested = true;
			const auto nestedPause = session.Pause(create.InstanceId, DataFactoryPauseScope::AllSystems, 0);
			if (nestedPause.Status != DataFactoryStatus::Ok) return false;
			auto reset = create;
			reset.OperationId = "restore-nested-reset";
			reset.ExpectedWorldEpoch = nestedPause.WorldEpoch;
			reset.ExpectedWorldVersion = nestedPause.WorldVersion;
			reset.ExpectedTick = nestedPause.Clock.Tick;
			nestedStatus = session.ResetWorld(reset).Status;
		}
		return true;
	});
	const auto created = session.CreateWorld(create);
	REQUIRE(created.Status == DataFactoryStatus::Ok);
	std::string checkpoint;
	REQUIRE(session.Checkpoint(create.InstanceId, checkpoint).Status == DataFactoryStatus::Ok);
	REQUIRE(session.Resume(create.InstanceId, 0).Status == DataFactoryStatus::Ok);
	restoring = true;
	const auto restored = session.Restore(create.InstanceId, checkpoint);
	REQUIRE(nestedStatus == DataFactoryStatus::Ok);
	CHECK(restored.Status != DataFactoryStatus::Ok);
	CHECK(session.AllSystemsPaused(create.InstanceId));
}

TEST_CASE(
	"data-factory forks a retained checkpoint into an isolated rehydrated branch", "[world][data-factory]"
) {
	Universe universe;
	const WorldId parent = MakeWorld(universe, "data-factory.fork.parent");
	BuildCountingWorld(universe, parent);
	REQUIRE(
		universe.SetSharedStoreValue(BusKind::DataStore, Name("data-factory.fork.shared"), Bytes("parent")) ==
		engine::world::BusStatus::Ok
	);
	DataFactorySession session(universe, 2);
	session.SetRehydrate([](Universe &, WorldId, std::string &) { return true; });
	session.SetPauseParticipant([](WorldId, DataFactoryPauseScope, bool, std::string &) { return true; });
	REQUIRE(
		session.Pause("data-factory.fork.parent", DataFactoryPauseScope::AllSystems, 0).Status ==
		DataFactoryStatus::Ok
	);
	const auto paused = session.Inspect("data-factory.fork.parent");
	std::string checkpoint;
	REQUIRE(session.Checkpoint("data-factory.fork.parent", checkpoint).Status == DataFactoryStatus::Ok);

	unsigned prepared = 0;
	unsigned committed = 0;
	unsigned aborted = 0;
	session.SetForkRehydrator({
		.Prepare =
			[&](std::string_view branchId, Universe &branch, WorldId forked, std::string &) {
				CHECK((branchId == "data-factory.fork.blue" || branchId == "data-factory.fork.green"));
				prepared++;
				branch.Enter(forked, [](Store &, Scheduler &systems) {
					systems.Add("data-factory.fork.count", Phase::Simulation, [](Store &store) {
						store.Each<Tally>([](Entity, Tally &tally) { tally.Value++; });
					});
				});
				return true;
			},
		.Commit =
			[&](std::string_view branchId) {
				CHECK((branchId == "data-factory.fork.blue" || branchId == "data-factory.fork.green"));
				committed++;
			},
		.Abort = [&](std::string_view) { aborted++; },
		.Retire = {},
	});

	const auto forked = session.Fork({
		.InstanceId = "data-factory.fork.parent",
		.CheckpointId = checkpoint,
		.BranchId = "data-factory.fork.blue",
		.ExpectedWorldEpoch = paused.WorldEpoch,
		.ExpectedWorldVersion = paused.WorldVersion,
		.ExpectedTick = paused.Clock.Tick,
	});
	REQUIRE(forked.Status == DataFactoryStatus::Ok);
	CHECK(forked.InstanceId == "data-factory.fork.blue");
	CHECK(forked.WorldEpoch == paused.WorldEpoch + 1);
	CHECK(forked.WorldVersion == 1);
	CHECK(session.OwnsFork("data-factory.fork.blue"));
	REQUIRE(session.ForkUniverse("data-factory.fork.blue") != nullptr);
	CHECK(prepared == 1);
	CHECK(committed == 1);
	CHECK(aborted == 0);

	Universe &branch = *session.ForkUniverse("data-factory.fork.blue");
	const WorldId branchWorld = branch.Find(Name("data-factory.fork.parent"));
	REQUIRE(branchWorld.IsValid());
	branch.Enter(branchWorld, [](Store &store) {
		store.Each<Tally>([](Entity, Tally &tally) { tally.Value += 40; });
	});
	REQUIRE(
		branch.SetSharedStoreValue(BusKind::DataStore, Name("data-factory.fork.shared"), Bytes("branch")) ==
		engine::world::BusStatus::Ok
	);
	CHECK(Count(branch, branchWorld) == 40);
	CHECK(Count(universe, parent) == 0);
	std::vector<std::byte> parentValue;
	std::vector<std::byte> branchValue;
	REQUIRE(
		universe.Peek(BusKind::DataStore, Name("data-factory.fork.shared"), &parentValue) ==
		engine::world::BusStatus::Ok
	);
	REQUIRE(
		branch.Peek(BusKind::DataStore, Name("data-factory.fork.shared"), &branchValue) ==
		engine::world::BusStatus::Ok
	);
	CHECK(parentValue == Bytes("parent"));
	CHECK(branchValue == Bytes("branch"));

	const auto branchBeforeStep = session.Inspect("data-factory.fork.blue");
	REQUIRE(branchBeforeStep.Status == DataFactoryStatus::Ok);
	CHECK(branchBeforeStep.WorldEpoch == forked.WorldEpoch);
	CHECK(branchBeforeStep.WorldVersion == forked.WorldVersion);
	const auto stepped = session.Step(
		"data-factory.fork.blue",
		DataFactoryInterval{},
		branchBeforeStep.Clock.Tick,
		branchBeforeStep.WorldVersion
	);
	REQUIRE(stepped.Status == DataFactoryStatus::Ok);
	CHECK(stepped.WorldVersion == branchBeforeStep.WorldVersion + 1);
	CHECK(stepped.Clock.Tick == branchBeforeStep.Clock.Tick + 1);
	const auto retired = session.RetireWorld({
		.Operation = DataFactoryWorldOperation::Retire,
		.InstanceId = "data-factory.fork.blue",
		.ExpectedWorldEpoch = stepped.WorldEpoch,
		.ExpectedWorldVersion = stepped.WorldVersion,
		.ExpectedTick = stepped.Clock.Tick,
		.OperationId = "retire-fork-blue",
	});
	REQUIRE(retired.Status == DataFactoryStatus::Ok);
	CHECK(retired.Tombstone);
	CHECK_FALSE(session.OwnsFork("data-factory.fork.blue"));
	CHECK(session.ForkUniverse("data-factory.fork.blue") == nullptr);
	CHECK(aborted == 0);

	const auto reused = session.Fork({
		.InstanceId = "data-factory.fork.parent",
		.CheckpointId = checkpoint,
		.BranchId = "data-factory.fork.green",
		.ExpectedWorldEpoch = paused.WorldEpoch,
		.ExpectedWorldVersion = paused.WorldVersion,
		.ExpectedTick = paused.Clock.Tick,
	});
	REQUIRE(reused.Status == DataFactoryStatus::Ok);
	CHECK(session.OwnsFork("data-factory.fork.green"));
}

TEST_CASE("data-factory refuses a fork without branch runtime ownership", "[world][data-factory]") {
	Universe universe;
	MakeWorld(universe, "data-factory.fork.refusal");
	DataFactorySession session(universe);
	session.SetRehydrate([](Universe &, WorldId, std::string &) { return true; });
	session.SetPauseParticipant([](WorldId, DataFactoryPauseScope, bool, std::string &) { return true; });
	REQUIRE(
		session.Pause("data-factory.fork.refusal", DataFactoryPauseScope::AllSystems, 0).Status ==
		DataFactoryStatus::Ok
	);
	std::string checkpoint;
	REQUIRE(session.Checkpoint("data-factory.fork.refusal", checkpoint).Status == DataFactoryStatus::Ok);
	const auto parent = session.Inspect("data-factory.fork.refusal");
	const auto refused = session.Fork({
		.InstanceId = "data-factory.fork.refusal",
		.CheckpointId = checkpoint,
		.BranchId = "data-factory.fork.unowned",
		.ExpectedWorldEpoch = parent.WorldEpoch,
		.ExpectedWorldVersion = parent.WorldVersion,
		.ExpectedTick = parent.Clock.Tick,
	});
	CHECK(refused.Status == DataFactoryStatus::Unsupported);
	CHECK_FALSE(session.OwnsFork("data-factory.fork.unowned"));

	unsigned aborted = 0;
	session.SetForkRehydrator({
		.Prepare =
			[](std::string_view, Universe &, WorldId, std::string &detail) {
				detail = "branch host refused installation";
				return false;
			},
		.Commit = {},
		.Abort = [&](std::string_view) { aborted++; },
		.Retire = {},
	});
	const auto rejected = session.Fork({
		.InstanceId = "data-factory.fork.refusal",
		.CheckpointId = checkpoint,
		.BranchId = "data-factory.fork.rejected",
		.ExpectedWorldEpoch = parent.WorldEpoch,
		.ExpectedWorldVersion = parent.WorldVersion,
		.ExpectedTick = parent.Clock.Tick,
	});
	CHECK(rejected.Status == DataFactoryStatus::RestoreIncomplete);
	CHECK(rejected.Detail == "branch host refused installation");
	CHECK(aborted == 1);
	CHECK_FALSE(session.OwnsFork("data-factory.fork.rejected"));

	session.SetForkRehydrator({
		.Prepare = [](std::string_view, Universe &, WorldId, std::string &) { return true; },
		.Commit = [](std::string_view) { throw std::runtime_error("deliberate commit failure"); },
		.Abort = [&](std::string_view) { aborted++; },
		.Retire = {},
	});
	const auto commitFailure = session.Fork({
		.InstanceId = "data-factory.fork.refusal",
		.CheckpointId = checkpoint,
		.BranchId = "data-factory.fork.commit-failure",
		.ExpectedWorldEpoch = parent.WorldEpoch,
		.ExpectedWorldVersion = parent.WorldVersion,
		.ExpectedTick = parent.Clock.Tick,
	});
	CHECK(commitFailure.Status == DataFactoryStatus::RestoreIncomplete);
	CHECK(commitFailure.Detail == "branch commit threw: deliberate commit failure");
	CHECK(aborted == 2);
	CHECK_FALSE(session.OwnsFork("data-factory.fork.commit-failure"));
}

TEST_CASE("data-factory retains a branch when committed retirement cleanup throws", "[world][data-factory]") {
	Universe universe;
	MakeWorld(universe, "data-factory.fork.retire-throw");
	DataFactorySession session(universe);
	session.SetRehydrate([](Universe &, WorldId, std::string &) { return true; });
	session.SetPauseParticipant([](WorldId, DataFactoryPauseScope, bool, std::string &) { return true; });
	REQUIRE(
		session.Pause("data-factory.fork.retire-throw", DataFactoryPauseScope::AllSystems, 0).Status ==
		DataFactoryStatus::Ok
	);
	std::string checkpoint;
	REQUIRE(session.Checkpoint("data-factory.fork.retire-throw", checkpoint).Status == DataFactoryStatus::Ok);
	const auto parent = session.Inspect("data-factory.fork.retire-throw");
	session.SetForkRehydrator({
		.Prepare = [](std::string_view, Universe &, WorldId, std::string &) { return true; },
		.Commit = {},
		.Abort = {},
		.Retire = [](std::string_view) { throw std::runtime_error("deliberate retirement failure"); },
	});
	const auto forked = session.Fork({
		.InstanceId = "data-factory.fork.retire-throw",
		.CheckpointId = checkpoint,
		.BranchId = "data-factory.fork.retire-throw.branch",
		.ExpectedWorldEpoch = parent.WorldEpoch,
		.ExpectedWorldVersion = parent.WorldVersion,
		.ExpectedTick = parent.Clock.Tick,
	});
	REQUIRE(forked.Status == DataFactoryStatus::Ok);
	const auto retired = session.RetireWorld({
		.Operation = DataFactoryWorldOperation::Retire,
		.InstanceId = "data-factory.fork.retire-throw.branch",
		.ExpectedWorldEpoch = forked.WorldEpoch,
		.ExpectedWorldVersion = forked.WorldVersion,
		.ExpectedTick = forked.Clock.Tick,
		.OperationId = "retire-throwing-branch",
	});
	CHECK(retired.Status == DataFactoryStatus::RestoreIncomplete);
	CHECK(session.OwnsFork("data-factory.fork.retire-throw.branch"));
	REQUIRE(session.ForkUniverse("data-factory.fork.retire-throw.branch") != nullptr);
	const auto inspected = session.Inspect("data-factory.fork.retire-throw.branch");
	CHECK(inspected.Status == DataFactoryStatus::Ok);
	CHECK(inspected.WorldVersion == forked.WorldVersion);
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

TEST_CASE("data-factory backward seek replays contiguous fixed steps in scratch", "[world][data-factory]") {
	Universe universe;
	const WorldId world = MakeWorld(universe, "data-factory.seek");
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
		session.Pause("data-factory.seek", DataFactoryPauseScope::AllSystems, 0).Status ==
		DataFactoryStatus::Ok
	);
	std::string checkpoint;
	REQUIRE(session.Checkpoint("data-factory.seek", checkpoint).Status == DataFactoryStatus::Ok);
	for (uint64_t tick = 0; tick < 3; ++tick) {
		const auto before = session.Inspect("data-factory.seek");
		REQUIRE(
			session.Step("data-factory.seek", DataFactoryInterval{}, tick, before.WorldVersion).Status ==
			DataFactoryStatus::Ok
		);
	}
	CHECK(Count(universe, universe.Find(Name("data-factory.seek"))) == 3);
	const auto beforeSeek = session.Inspect("data-factory.seek");
	const auto sought =
		session.SeekBackward("data-factory.seek", 1, beforeSeek.Clock.Tick, beforeSeek.WorldVersion);
	REQUIRE(sought.Status == DataFactoryStatus::Ok);
	CHECK(sought.Clock.Tick == 1);
	CHECK(sought.WorldEpoch == beforeSeek.WorldEpoch + 1);
	CHECK(sought.WorldVersion == beforeSeek.WorldVersion + 1);
	CHECK(Count(universe, universe.Find(Name("data-factory.seek"))) == 1);

	REQUIRE(
		session.Step("data-factory.seek", DataFactoryInterval{}, sought.Clock.Tick, sought.WorldVersion)
			.Status == DataFactoryStatus::Ok
	);
	const auto branched = session.Inspect("data-factory.seek");
	CHECK(branched.Clock.Tick == 2);
	CHECK(Count(universe, universe.Find(Name("data-factory.seek"))) == 2);
	CHECK(
		session.SeekBackward("data-factory.seek", 2, branched.Clock.Tick, branched.WorldVersion).Status ==
		DataFactoryStatus::ValidationFailed
	);
}

TEST_CASE(
	"data-factory rehydration commits staged host state after a nonzero-tick restore", "[world][data-factory]"
) {
	Universe universe;
	const WorldId world = MakeWorld(universe, "data-factory.rehydrate-host");
	BuildCountingWorld(universe, world);
	DataFactorySession session(universe);
	bool hostRuntimeLive = true;
	bool hostRuntimePrepared = false;
	unsigned prepares = 0;
	unsigned commits = 0;
	unsigned aborts = 0;
	session.SetPauseParticipant([](WorldId, DataFactoryPauseScope, bool, std::string &) { return true; });
	session.SetRehydrator({
		.Prepare =
			[&](Universe &candidate, WorldId restored, std::string &) {
				prepares++;
				hostRuntimePrepared = true;
				candidate.Enter(restored, [](Store &, Scheduler &systems) {
					systems.Add("data-factory.count", Phase::Simulation, [](Store &inner) {
						inner.Each<Tally>([](Entity, Tally &tally) { tally.Value++; });
					});
				});
				return true;
			},
		.Commit =
			[&] {
				commits++;
				hostRuntimeLive = hostRuntimePrepared;
				hostRuntimePrepared = false;
			},
		.Abort =
			[&] {
				aborts++;
				hostRuntimePrepared = false;
			},
	});

	REQUIRE(
		session.Pause("data-factory.rehydrate-host", DataFactoryPauseScope::AllSystems, 0).Status ==
		DataFactoryStatus::Ok
	);
	for (uint64_t tick = 0; tick < 2; ++tick) {
		const auto before = session.Inspect("data-factory.rehydrate-host");
		REQUIRE(
			session.Step("data-factory.rehydrate-host", DataFactoryInterval{}, tick, before.WorldVersion)
				.Status == DataFactoryStatus::Ok
		);
	}
	std::string checkpoint;
	REQUIRE(session.Checkpoint("data-factory.rehydrate-host", checkpoint).Status == DataFactoryStatus::Ok);
	const auto before = session.Inspect("data-factory.rehydrate-host");
	REQUIRE(
		session.Step("data-factory.rehydrate-host", DataFactoryInterval{}, 2, before.WorldVersion).Status ==
		DataFactoryStatus::Ok
	);

	const auto restored = session.Restore("data-factory.rehydrate-host", checkpoint);
	REQUIRE(restored.Status == DataFactoryStatus::Ok);
	CHECK(restored.Clock.Tick == 2);
	CHECK(Count(universe, universe.Find(Name("data-factory.rehydrate-host"))) == 2);
	CHECK(prepares == 1);
	CHECK(commits == 1);
	CHECK(aborts == 0);
	CHECK(hostRuntimeLive);
	CHECK_FALSE(hostRuntimePrepared);
}

TEST_CASE(
	"data-factory rehydration aborts staged host state when pause reconciliation refuses",
	"[world][data-factory]"
) {
	Universe universe;
	const WorldId world = MakeWorld(universe, "data-factory.rehydrate-abort");
	BuildCountingWorld(universe, world);
	DataFactorySession session(universe);
	bool refuseResume = false;
	bool liveRuntime = true;
	bool preparedRuntime = false;
	unsigned commits = 0;
	unsigned aborts = 0;
	session.SetPauseParticipant([&](WorldId, DataFactoryPauseScope scope, bool paused, std::string &) {
		return scope != DataFactoryPauseScope::AllSystems || paused || !refuseResume;
	});
	session.SetRehydrator({
		.Prepare =
			[&](Universe &candidate, WorldId restored, std::string &) {
				preparedRuntime = true;
				candidate.Enter(restored, [](Store &, Scheduler &systems) {
					systems.Add("data-factory.count", Phase::Simulation, [](Store &inner) {
						inner.Each<Tally>([](Entity, Tally &tally) { tally.Value++; });
					});
				});
				return true;
			},
		.Commit =
			[&] {
				commits++;
				liveRuntime = preparedRuntime;
				preparedRuntime = false;
			},
		.Abort =
			[&] {
				aborts++;
				preparedRuntime = false;
			},
	});

	REQUIRE(
		session.Pause("data-factory.rehydrate-abort", DataFactoryPauseScope::AllSystems, 0).Status ==
		DataFactoryStatus::Ok
	);
	const auto paused = session.Inspect("data-factory.rehydrate-abort");
	REQUIRE(
		session.Step("data-factory.rehydrate-abort", DataFactoryInterval{}, 0, paused.WorldVersion).Status ==
		DataFactoryStatus::Ok
	);
	REQUIRE(session.Resume("data-factory.rehydrate-abort", 1).Status == DataFactoryStatus::Ok);
	std::string checkpoint;
	REQUIRE(session.Checkpoint("data-factory.rehydrate-abort", checkpoint).Status == DataFactoryStatus::Ok);
	const auto active = session.Inspect("data-factory.rehydrate-abort");
	REQUIRE(
		session.Pause("data-factory.rehydrate-abort", DataFactoryPauseScope::AllSystems, active.Clock.Tick)
			.Status == DataFactoryStatus::Ok
	);
	refuseResume = true;
	const auto beforeRestore = session.Inspect("data-factory.rehydrate-abort");
	const auto restored = session.Restore("data-factory.rehydrate-abort", checkpoint);
	CHECK(restored.Status == DataFactoryStatus::RestoreIncomplete);
	CHECK(session.Inspect("data-factory.rehydrate-abort").WorldVersion == beforeRestore.WorldVersion);
	CHECK(Count(universe, world) == 1);
	CHECK(commits == 0);
	CHECK(aborts == 1);
	CHECK(liveRuntime);
	CHECK_FALSE(preparedRuntime);
}

TEST_CASE("data-factory backward seek refuses replay across an action step", "[world][data-factory]") {
	Universe universe;
	MakeWorld(universe, "data-factory.seek-action");
	DataFactorySession session(universe);
	session.SetPauseParticipant([](WorldId, DataFactoryPauseScope, bool, std::string &) { return true; });
	session.SetRehydrate([](Universe &, WorldId, std::string &) { return true; });
	session.SetActionExecutor([](Universe &,
								 WorldId,
								 std::span<const DataFactoryAction>,
								 DataFactoryActionCommit &commit,
								 std::string &) {
		commit = [] {};
		return true;
	});
	REQUIRE(
		session.Pause("data-factory.seek-action", DataFactoryPauseScope::AllSystems, 0).Status ==
		DataFactoryStatus::Ok
	);
	std::string checkpoint;
	REQUIRE(session.Checkpoint("data-factory.seek-action", checkpoint).Status == DataFactoryStatus::Ok);
	const std::array actions{DataFactoryAction{.Name = "jump"}};
	const auto before = session.Inspect("data-factory.seek-action");
	REQUIRE(
		session.Step("data-factory.seek-action", DataFactoryInterval{}, 0, before.WorldVersion, actions)
			.Status == DataFactoryStatus::Ok
	);
	const auto after = session.Inspect("data-factory.seek-action");
	CHECK(
		session.SeekBackward("data-factory.seek-action", 0, 1, after.WorldVersion).Status ==
		DataFactoryStatus::StaleSnapshot
	);
}

TEST_CASE(
	"data-factory backward seek replays ordered action batches in a scratch world", "[world][data-factory]"
) {
	struct PendingActions {
		std::vector<std::string> Names;
	};
	const auto installActions = [](Universe &host, WorldId target, std::shared_ptr<PendingActions> pending) {
		host.Enter(target, [pending = std::move(pending)](Store &, Scheduler &systems) {
			systems.Add("data-factory.actions", Phase::Simulation, [pending](Store &store) {
				const int actions = static_cast<int>(pending->Names.size());
				pending->Names.clear();
				store.Each<Tally>([actions](Entity, Tally &tally) { tally.Value += actions; });
			});
		});
	};

	Universe universe;
	const WorldId world = MakeWorld(universe, "data-factory.seek-actions");
	BuildCountingWorld(universe, world);
	const auto livePending = std::make_shared<PendingActions>();
	installActions(universe, world, livePending);
	DataFactorySession session(universe);
	std::unordered_map<Universe *, std::shared_ptr<PendingActions>> candidatePending;
	std::vector<std::string> replayed;
	session.SetPauseParticipant([](WorldId, DataFactoryPauseScope, bool, std::string &) { return true; });
	session.SetRehydrator({
		.Prepare =
			[&](Universe &candidate, WorldId target, std::string &) {
				auto pending = std::make_shared<PendingActions>();
				candidatePending.emplace(&candidate, pending);
				candidate.Enter(target, [](Store &, Scheduler &systems) {
					systems.Add("data-factory.count", Phase::Simulation, [](Store &store) {
						store.Each<Tally>([](Entity, Tally &tally) { tally.Value++; });
					});
				});
				installActions(candidate, target, std::move(pending));
				return true;
			},
		.Commit = {},
		.Abort = {},
	});
	session.SetActionExecutor([livePending](
								  Universe &,
								  WorldId,
								  std::span<const DataFactoryAction> actions,
								  DataFactoryActionCommit &commit,
								  std::string &
							  ) {
		std::vector<std::string> names;
		names.reserve(actions.size());
		for (const DataFactoryAction &action : actions)
			names.push_back(action.Name);
		commit = [livePending, names = std::move(names)]() noexcept { livePending->Names = names; };
		return true;
	});
	session.SetReplayActionExecutor([&candidatePending, &replayed](
										Universe &candidate,
										WorldId,
										std::span<const DataFactoryAction> actions,
										DataFactoryActionCommit &commit,
										std::string &detail
									) {
		const auto found = candidatePending.find(&candidate);
		if (found == candidatePending.end()) {
			detail = "candidate action state is unavailable";
			return false;
		}
		std::vector<std::string> names;
		names.reserve(actions.size());
		for (const DataFactoryAction &action : actions) {
			names.push_back(action.Name);
			replayed.push_back(action.Name);
		}
		commit = [pending = found->second, names = std::move(names)]() noexcept { pending->Names = names; };
		return true;
	});

	REQUIRE(
		session.Pause("data-factory.seek-actions", DataFactoryPauseScope::AllSystems, 0).Status ==
		DataFactoryStatus::Ok
	);
	std::string checkpoint;
	REQUIRE(session.Checkpoint("data-factory.seek-actions", checkpoint).Status == DataFactoryStatus::Ok);
	const std::array actions{DataFactoryAction{.Name = "first"}, DataFactoryAction{.Name = "second"}};
	const auto beforeAction = session.Inspect("data-factory.seek-actions");
	REQUIRE(
		session
			.Step("data-factory.seek-actions", DataFactoryInterval{}, 0, beforeAction.WorldVersion, actions)
			.Status == DataFactoryStatus::Ok
	);
	const auto beforeSecond = session.Inspect("data-factory.seek-actions");
	REQUIRE(
		session.Step("data-factory.seek-actions", DataFactoryInterval{}, 1, beforeSecond.WorldVersion)
			.Status == DataFactoryStatus::Ok
	);
	CHECK(Count(universe, world) == 4);

	const auto beforeSeek = session.Inspect("data-factory.seek-actions");
	const auto sought =
		session.SeekBackward("data-factory.seek-actions", 1, beforeSeek.Clock.Tick, beforeSeek.WorldVersion);
	REQUIRE(sought.Status == DataFactoryStatus::Ok);
	CHECK(sought.Clock.Tick == 1);
	CHECK(sought.WorldVersion == beforeSeek.WorldVersion + 1);
	CHECK(Count(universe, universe.Find(Name("data-factory.seek-actions"))) == 3);
	CHECK(replayed == std::vector<std::string>{"first", "second"});
}

TEST_CASE(
	"data-factory action replay refusal leaves the live world and revision unchanged", "[world][data-factory]"
) {
	Universe universe;
	const WorldId world = MakeWorld(universe, "data-factory.seek-action-refusal");
	BuildCountingWorld(universe, world);
	DataFactorySession session(universe);
	bool prepared = false;
	bool aborted = false;
	session.SetPauseParticipant([](WorldId, DataFactoryPauseScope, bool, std::string &) { return true; });
	session.SetRehydrator({
		.Prepare =
			[&prepared](Universe &candidate, WorldId target, std::string &) {
				prepared = true;
				candidate.Enter(target, [](Store &, Scheduler &systems) {
					systems.Add("data-factory.count", Phase::Simulation, [](Store &store) {
						store.Each<Tally>([](Entity, Tally &tally) { tally.Value++; });
					});
				});
				return true;
			},
		.Commit = {},
		.Abort =
			[&] {
				aborted = prepared;
				prepared = false;
			},
	});
	session.SetActionExecutor([](Universe &,
								 WorldId,
								 std::span<const DataFactoryAction>,
								 DataFactoryActionCommit &commit,
								 std::string &) {
		commit = [] {};
		return true;
	});
	session.SetReplayActionExecutor([](Universe &,
									   WorldId,
									   std::span<const DataFactoryAction>,
									   DataFactoryActionCommit &,
									   std::string &detail) {
		detail = "candidate runtime cannot replay this action";
		return false;
	});

	REQUIRE(
		session.Pause("data-factory.seek-action-refusal", DataFactoryPauseScope::AllSystems, 0).Status ==
		DataFactoryStatus::Ok
	);
	std::string checkpoint;
	REQUIRE(
		session.Checkpoint("data-factory.seek-action-refusal", checkpoint).Status == DataFactoryStatus::Ok
	);
	const auto beforeAction = session.Inspect("data-factory.seek-action-refusal");
	REQUIRE(
		session
			.Step(
				"data-factory.seek-action-refusal",
				DataFactoryInterval{},
				0,
				beforeAction.WorldVersion,
				std::array{DataFactoryAction{.Name = "unsafe"}}
			)
			.Status == DataFactoryStatus::Ok
	);
	const auto beforeSecond = session.Inspect("data-factory.seek-action-refusal");
	REQUIRE(
		session.Step("data-factory.seek-action-refusal", DataFactoryInterval{}, 1, beforeSecond.WorldVersion)
			.Status == DataFactoryStatus::Ok
	);
	const auto beforeSeek = session.Inspect("data-factory.seek-action-refusal");
	const auto refused = session.SeekBackward(
		"data-factory.seek-action-refusal", 1, beforeSeek.Clock.Tick, beforeSeek.WorldVersion
	);
	CHECK(refused.Status == DataFactoryStatus::RestoreIncomplete);
	CHECK(refused.Detail == "candidate runtime cannot replay this action");
	CHECK(session.Inspect("data-factory.seek-action-refusal").WorldVersion == beforeSeek.WorldVersion);
	CHECK(session.Inspect("data-factory.seek-action-refusal").Clock.Tick == beforeSeek.Clock.Tick);
	CHECK(Count(universe, world) == 2);
	CHECK(aborted);
	CHECK_FALSE(prepared);
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

TEST_CASE("factory reset preserves live store writes made during preparation", "[world][data-factory]") {
	Universe universe;
	DataFactorySession session(universe);
	session.SetPauseParticipant([](WorldId, DataFactoryPauseScope, bool, std::string &) { return true; });
	DataFactoryWorldRequest request{
		.Operation = DataFactoryWorldOperation::Create,
		.InstanceId = "factory.store-witness",
		.TickRate = 60.0,
		.OperationId = "store-witness-create",
	};
	const auto created = session.CreateWorld(request);
	REQUIRE(created.Status == DataFactoryStatus::Ok);
	const auto original = universe.Find(Name(request.InstanceId));
	Tally *held = nullptr;
	universe.Enter(original, [&](Store &store) {
		const Entity entity = store.Create();
		store.Set(entity, Tally{1});
		held = store.GetUnobserved<Tally>(entity);
	});
	REQUIRE(held != nullptr);
	session.SetCandidateFinal(
		[&](Universe &, Name, engine::world::DataFactoryCandidateFinalOperation, std::string &) {
			held->Value = 2;
			return true;
		}
	);
	request.Operation = DataFactoryWorldOperation::Reset;
	request.OperationId = "store-witness-reset";
	request.ExpectedWorldEpoch = created.WorldEpoch;
	request.ExpectedWorldVersion = created.WorldVersion;
	request.ExpectedTick = created.Clock.Tick;
	CHECK(session.ResetWorld(request).Status == DataFactoryStatus::VersionConflict);
	CHECK(universe.Find(Name(request.InstanceId)) == original);
	CHECK(held->Value == 2);
}

TEST_CASE("factory reset preserves other store writes when replacing all worlds", "[world][data-factory]") {
	Universe universe;
	DataFactorySession session(universe);
	session.SetPauseParticipant([](WorldId, DataFactoryPauseScope, bool, std::string &) { return true; });
	DataFactoryWorldRequest request{
		.Operation = DataFactoryWorldOperation::Create,
		.InstanceId = "factory.store-target",
		.TickRate = 60.0,
		.OperationId = "other-store-create",
	};
	const auto created = session.CreateWorld(request);
	REQUIRE(created.Status == DataFactoryStatus::Ok);
	const auto other = MakeWorld(universe, "factory.other-store");
	Tally *held = nullptr;
	universe.Enter(other, [&](Store &store) {
		const Entity entity = store.Create();
		store.Set(entity, Tally{1});
		held = store.GetUnobserved<Tally>(entity);
	});
	REQUIRE(held != nullptr);
	session.SetCandidateFinal(
		[&](Universe &, Name, engine::world::DataFactoryCandidateFinalOperation, std::string &) {
			held->Value = 2;
			return true;
		}
	);
	request.Operation = DataFactoryWorldOperation::Reset;
	request.OperationId = "other-store-reset";
	request.ExpectedWorldEpoch = created.WorldEpoch;
	request.ExpectedWorldVersion = created.WorldVersion;
	request.ExpectedTick = created.Clock.Tick;
	CHECK(session.ResetWorld(request).Status == DataFactoryStatus::VersionConflict);
	CHECK(universe.Find(Name("factory.other-store")) == other);
	CHECK(held->Value == 2);
}

TEST_CASE("factory create preserves worlds added during candidate preparation", "[world][data-factory]") {
	Universe universe;
	DataFactorySession session(universe);
	session.SetPauseParticipant([](WorldId, DataFactoryPauseScope, bool, std::string &) { return true; });
	WorldId added;
	session.SetCandidateFinal(
		[&](Universe &, Name, engine::world::DataFactoryCandidateFinalOperation, std::string &) {
			added = MakeWorld(universe, "factory.callback-world");
			return true;
		}
	);
	const DataFactoryWorldRequest request{
		.Operation = DataFactoryWorldOperation::Create,
		.InstanceId = "factory.requested-world",
		.TickRate = 60.0,
		.OperationId = "create-with-callback-world",
	};
	CHECK(session.CreateWorld(request).Status == DataFactoryStatus::VersionConflict);
	CHECK(added.IsValid());
	CHECK(universe.Find(Name("factory.callback-world")) == added);
	CHECK_FALSE(universe.Find(Name(request.InstanceId)).IsValid());
}

TEST_CASE("factory reset revalidates a world changed by candidate preparation", "[world][data-factory]") {
	Universe universe;
	DataFactorySession session(universe);
	session.SetPauseParticipant([](WorldId, DataFactoryPauseScope, bool, std::string &) { return true; });
	DataFactoryWorldRequest request{
		.Operation = DataFactoryWorldOperation::Create,
		.InstanceId = "factory.resume-during-reset",
		.TickRate = 60.0,
		.OperationId = "resume-create",
	};
	const auto created = session.CreateWorld(request);
	REQUIRE(created.Status == DataFactoryStatus::Ok);
	const auto original = universe.Find(Name(request.InstanceId));
	session.SetCandidateFinal(
		[&](Universe &, Name, engine::world::DataFactoryCandidateFinalOperation, std::string &) {
			CHECK(session.Resume(request.InstanceId, created.Clock.Tick).Status == DataFactoryStatus::Ok);
			return true;
		}
	);
	request.Operation = DataFactoryWorldOperation::Reset;
	request.OperationId = "resume-reset";
	request.ExpectedWorldEpoch = created.WorldEpoch;
	request.ExpectedWorldVersion = created.WorldVersion;
	request.ExpectedTick = created.Clock.Tick;
	CHECK(session.ResetWorld(request).Status == DataFactoryStatus::VersionConflict);
	CHECK(universe.Find(Name(request.InstanceId)) == original);
	CHECK_FALSE(session.AllSystemsPaused(request.InstanceId));
}

TEST_CASE("factory lifecycle rejects reentrant world operations", "[world][data-factory]") {
	Universe universe;
	DataFactorySession session(universe);
	session.SetPauseParticipant([](WorldId, DataFactoryPauseScope, bool, std::string &) { return true; });
	DataFactoryWorldRequest create{
		.Operation = DataFactoryWorldOperation::Create,
		.InstanceId = "factory.reentrant",
		.TickRate = 60.0,
		.OperationId = "create-reentrant",
	};
	const auto created = session.CreateWorld(create);
	REQUIRE(created.Status == DataFactoryStatus::Ok);
	DataFactoryStatus nested = DataFactoryStatus::Ok;
	int callbacks = 0;
	session.SetWorldLifecycle(
		[&](DataFactoryWorldOperation operation, Universe &, WorldId, bool committed, std::string &) {
			if (operation == DataFactoryWorldOperation::Retire && committed) {
				++callbacks;
				create.OperationId = "nested-create";
				nested = session.CreateWorld(create).Status;
			}
			return true;
		}
	);
	const DataFactoryWorldRequest retire{
		.Operation = DataFactoryWorldOperation::Retire,
		.InstanceId = create.InstanceId,
		.ExpectedWorldEpoch = created.WorldEpoch,
		.ExpectedWorldVersion = created.WorldVersion,
		.ExpectedTick = created.Clock.Tick,
		.OperationId = "retire-reentrant",
	};
	const auto retired = session.RetireWorld(retire);
	REQUIRE(retired.Status == DataFactoryStatus::Ok);
	CHECK(retired.Tombstone);
	CHECK(nested == DataFactoryStatus::VersionConflict);
	CHECK_FALSE(universe.Find(Name("factory.reentrant")).IsValid());
	CHECK(session.RetireWorld(retire).WorldVersion == retired.WorldVersion);
	CHECK(callbacks == 1);
}

TEST_CASE("committed factory lifecycle survives host finalization exceptions", "[world][data-factory]") {
	Universe universe;
	DataFactorySession session(universe);
	session.SetPauseParticipant([](WorldId, DataFactoryPauseScope, bool, std::string &) { return true; });
	int finalizations = 0;
	session.SetWorldLifecycle(
		[&](DataFactoryWorldOperation operation, Universe &, WorldId, bool committed, std::string &) {
			if (committed) {
				++finalizations;
				if (operation != DataFactoryWorldOperation::Retire) {
					CHECK(session.OwnsWorld("factory.finalization"));
					CHECK(session.AllSystemsPaused("factory.finalization"));
					CHECK(
						session.Inspect("factory.finalization").WorldVersion ==
						static_cast<uint64_t>(finalizations)
					);
				} else {
					CHECK_FALSE(session.OwnsWorld("factory.finalization"));
				}
				throw std::runtime_error("host finalization");
			}
			return true;
		}
	);
	DataFactoryWorldRequest request{
		.Operation = DataFactoryWorldOperation::Create,
		.InstanceId = "factory.finalization",
		.TickRate = 60.0,
		.OperationId = "create-finalization",
	};
	const auto created = session.CreateWorld(request);
	REQUIRE(created.Status == DataFactoryStatus::Ok);
	CHECK(created.Detail.find("host finalization threw") != std::string::npos);
	CHECK(session.OwnsWorld(request.InstanceId));
	CHECK(session.AllSystemsPaused(request.InstanceId));
	CHECK(session.CreateWorld(request).WorldVersion == created.WorldVersion);
	CHECK(finalizations == 1);

	request.Operation = DataFactoryWorldOperation::Reset;
	request.OperationId = "reset-finalization";
	request.ExpectedWorldEpoch = created.WorldEpoch;
	request.ExpectedWorldVersion = created.WorldVersion;
	request.ExpectedTick = created.Clock.Tick;
	const auto reset = session.ResetWorld(request);
	REQUIRE(reset.Status == DataFactoryStatus::Ok);
	CHECK(reset.WorldEpoch == created.WorldEpoch + 1);
	CHECK(reset.WorldVersion == created.WorldVersion + 1);
	CHECK(reset.Detail.find("host finalization threw") != std::string::npos);
	CHECK(session.OwnsWorld(request.InstanceId));
	CHECK(session.AllSystemsPaused(request.InstanceId));
	CHECK(session.ResetWorld(request).WorldVersion == reset.WorldVersion);
	CHECK(finalizations == 2);

	request.Operation = DataFactoryWorldOperation::Retire;
	request.OperationId = "retire-finalization";
	request.ExpectedWorldEpoch = reset.WorldEpoch;
	request.ExpectedWorldVersion = reset.WorldVersion;
	request.ExpectedTick = reset.Clock.Tick;
	const auto retired = session.RetireWorld(request);
	REQUIRE(retired.Status == DataFactoryStatus::Ok);
	CHECK(retired.Tombstone);
	CHECK(retired.WorldVersion == reset.WorldVersion + 1);
	CHECK(retired.Detail.find("host finalization threw") != std::string::npos);
	CHECK(session.RetireWorld(request).WorldVersion == retired.WorldVersion);
	CHECK_FALSE(universe.Find(Name("factory.finalization")).IsValid());
	CHECK_FALSE(session.OwnsWorld(request.InstanceId));
	CHECK(finalizations == 3);
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

TEST_CASE(
	"factory reset retains unaffected world identity and scheduler", "[world][data-factory][retention]"
) {
	Universe universe;
	DataFactorySession session(universe);
	session.SetRetainUnaffectedWorlds(true);
	session.SetPauseParticipant([](WorldId, DataFactoryPauseScope, bool, std::string &) { return true; });
	const DataFactoryWorldRequest create{
		.Operation = DataFactoryWorldOperation::Create,
		.InstanceId = "retention.factory",
		.TickRate = 60.0,
		.OperationId = "retention-create",
	};
	const auto created = session.CreateWorld(create);
	REQUIRE(created.Status == DataFactoryStatus::Ok);
	const WorldId target = universe.Find(Name("retention.factory"));
	const WorldId hole = MakeWorld(universe, "retention.hole");
	const WorldId other = MakeWorld(universe, "retention.other");
	REQUIRE(universe.Destroy(hole) == engine::world::WorldStatus::Ok);
	Store *original = nullptr;
	uint64_t identity = 0;
	int calls = 0;
	REQUIRE(universe.Enter(other, [&](Store &store, Scheduler &systems) {
		original = &store;
		identity = store.Identity();
		systems.Add("retention.test", Phase::Simulation, [&](Store &current) {
			CHECK(&current == original);
			++calls;
		});
	}) == engine::world::WorldStatus::Ok);
	Entity addedToOther = engine::ecs::NULL_ENTITY;
	session.SetCandidatePrepare([&](Universe &candidate, std::string &) {
		CHECK(candidate.Find(Name("retention.other")) == other);
		CHECK(candidate.IsRetained(other));
		CHECK(candidate.ProjectedStoreIdentity(other) == identity);
		addedToOther = original->Create();
		return true;
	});
	DataFactoryWorldRequest reset{
		.Operation = DataFactoryWorldOperation::Reset,
		.InstanceId = "retention.factory",
		.TickRate = 60.0,
		.ExpectedWorldEpoch = created.WorldEpoch,
		.ExpectedWorldVersion = created.WorldVersion,
		.ExpectedTick = created.Clock.Tick,
		.OperationId = "retention-reset",
	};
	const auto result = session.ResetWorld(reset);
	REQUIRE(result.Status == DataFactoryStatus::Ok);
	CHECK(universe.Find(Name("retention.factory")) == target);
	CHECK(universe.Find(Name("retention.other")) == other);
	REQUIRE(universe.Enter(other, [&](Store &store, Scheduler &systems) {
		CHECK(&store == original);
		CHECK(store.Identity() == identity);
		CHECK(store.Alive(addedToOther));
		CHECK(systems.SystemCount() == 1);
	}) == engine::world::WorldStatus::Ok);
	universe.Tick(0.1f);
	CHECK(calls > 0);

	// An ignored Busy from a candidate hook must not silently discard a write.
	session.SetCandidatePrepare([&](Universe &candidate, std::string &) {
		CHECK(candidate.Enter(other, [](Store &store) {
			(void)store.Create();
		}) == engine::world::WorldStatus::Busy);
		return true;
	});
	const auto before = session.Inspect("retention.factory");
	reset.ExpectedWorldEpoch = before.WorldEpoch;
	reset.ExpectedWorldVersion = before.WorldVersion;
	reset.ExpectedTick = before.Clock.Tick;
	reset.OperationId = "retention-reject-mutating-hook";
	CHECK(session.ResetWorld(reset).Status == DataFactoryStatus::RestoreIncomplete);
	CHECK(universe.Find(Name("retention.other")) == other);
	CHECK(universe.StoreIdentityOf(other) == identity);
	CHECK(session.Inspect("retention.factory").WorldVersion == before.WorldVersion);

	// A live bus write after staging must survive a refused replacement.
	session.SetCandidatePrepare([&](Universe &, std::string &) {
		CHECK(
			universe.SetSharedStoreValue(BusKind::DataStore, Name("retention.late"), Bytes("live")) ==
			engine::world::BusStatus::Ok
		);
		return true;
	});
	reset.OperationId = "retention-reject-late-bus";
	CHECK(session.ResetWorld(reset).Status == DataFactoryStatus::RestoreIncomplete);
	std::vector<std::byte> value;
	CHECK(universe.Peek(BusKind::DataStore, Name("retention.late"), &value) == engine::world::BusStatus::Ok);
	CHECK(value == Bytes("live"));
	CHECK(session.Inspect("retention.factory").WorldVersion == before.WorldVersion);

	REQUIRE(universe.ConfigurePresentation(42));
	session.SetCandidatePrepare([&](Universe &, std::string &) {
		CHECK(universe.OpenPresentation(other, Name("late")).Status == engine::world::PresentationStatus::Ok);
		return true;
	});
	reset.OperationId = "retention-reject-late-presentation";
	CHECK(session.ResetWorld(reset).Status == DataFactoryStatus::RestoreIncomplete);
	CHECK(universe.LookupPresentation(other, "late").Session == 42);
	CHECK(session.Inspect("retention.factory").WorldVersion == before.WorldVersion);

	// A queued destroy for the old target must not run on its replacement.
	session.SetCandidatePrepare({});
	REQUIRE(universe.OnState([&](WorldId id, Store &, auto) {
		if (id == other) CHECK(universe.Destroy(target) == engine::world::WorldStatus::Ok);
	}));
	REQUIRE(universe.SetState(other, engine::world::WorldState::Suspended) == engine::world::WorldStatus::Ok);
	reset.OperationId = "retention-reject-target-control";
	CHECK(session.ResetWorld(reset).Status == DataFactoryStatus::RestoreIncomplete);
	CHECK(universe.Find(Name("retention.factory")) == target);
	CHECK(session.Inspect("retention.factory").WorldVersion == before.WorldVersion);
}

TEST_CASE("retention plans are bound to their source universe", "[world][data-factory][retention]") {
	Universe source;
	Universe other;
	Universe candidate;
	REQUIRE(candidate.StageReplacement(source, Name("retention.new"), 1024 * 1024));
	Universe nested;
	CHECK_FALSE(nested.StageReplacement(candidate, Name("retention.new"), 1024 * 1024));
	DataFactorySession nestedSession(candidate);
	nestedSession.SetPauseParticipant([](WorldId, DataFactoryPauseScope, bool, std::string &) {
		return true;
	});
	const DataFactoryWorldRequest request{
		.Operation = DataFactoryWorldOperation::Create,
		.InstanceId = "retention.nested",
		.TickRate = 60.0,
		.OperationId = "nested-create",
	};
	CHECK(nestedSession.CreateWorld(request).Status == DataFactoryStatus::RestoreIncomplete);
	const auto nestedWorld = MakeWorld(candidate, "retention.nested");
	REQUIRE(nestedWorld.IsValid());
	const auto paused = nestedSession.Pause("retention.nested", DataFactoryPauseScope::AllSystems, 0);
	REQUIRE(paused.Status == DataFactoryStatus::Ok);
	CHECK(
		nestedSession.PrepareExternalMutation("retention.nested", paused.Clock.Tick, paused.WorldVersion)
			.Reply()
			.Status == DataFactoryStatus::RestoreIncomplete
	);
	CHECK_FALSE(candidate.PreparePresentationReplacement(other, Name("retention.new")));
	CHECK_FALSE(candidate.CanCommitRetainedWorlds(other));
	CHECK_FALSE(candidate.CommitRetainedWorlds(other));
	CHECK_FALSE(other.CanReplaceWith(candidate, true));
	CHECK_FALSE(source.CanReplaceWith(candidate));
	CHECK(source.CanReplaceWith(candidate, true));
	CHECK(candidate.PreparePresentationReplacement(source, Name("retention.new")));
	CHECK(candidate.CommitRetainedWorlds(source));
}

TEST_CASE("factory reset retains unrelated presentation traffic", "[world][data-factory][presentation]") {
	Universe universe;
	DataFactorySession session(universe);
	session.SetPauseParticipant([](WorldId, DataFactoryPauseScope, bool, std::string &) { return true; });
	const DataFactoryWorldRequest create{
		.Operation = DataFactoryWorldOperation::Create,
		.InstanceId = "factory.traffic",
		.TickRate = 60.0,
		.OperationId = "traffic-create",
	};
	const auto created = session.CreateWorld(create);
	REQUIRE(created.Status == DataFactoryStatus::Ok);
	const WorldId source = MakeWorld(universe, "traffic.source");
	const WorldId destination = MakeWorld(universe, "traffic.destination");
	const WorldId target = universe.Find(Name("factory.traffic"));
	REQUIRE(universe.ConfigurePresentation(42));
	const auto from = universe.OpenPresentation(source, Name("image")).Address;
	const auto to = universe.OpenPresentation(destination, Name("image")).Address;
	const auto oldTarget = universe.OpenPresentation(target, Name("image")).Address;
	REQUIRE(
		universe.SendPresentation(
			source,
			from,
			to,
			17,
			Bytes("picture"),
			engine::world::PresentationPriority::Routine,
			std::span<const std::string_view>{}
		) == engine::world::PresentationStatus::Ok
	);
	REQUIRE(
		universe.SendPresentation(target, oldTarget, to, 19, Bytes("old target")) ==
		engine::world::PresentationStatus::Ok
	);
	Delivery delivery;
	delivery.Payload = Bytes("message");
	REQUIRE(universe.Deliver(Name("traffic.destination"), delivery));
	const DataFactoryWorldRequest reset{
		.Operation = DataFactoryWorldOperation::Reset,
		.InstanceId = "factory.traffic",
		.TickRate = 60.0,
		.ExpectedWorldEpoch = created.WorldEpoch,
		.ExpectedWorldVersion = created.WorldVersion,
		.ExpectedTick = created.Clock.Tick,
		.OperationId = "traffic-reset",
	};
	const auto discardedBefore = engine::core::Metrics::Get("world.presentation.discarded.messages");
	const auto enqueuedBefore = engine::core::Metrics::Get("world.presentation.enqueued.messages");
	const auto takenBefore = engine::core::Metrics::Get("world.presentation.taken.messages");
	session.SetCandidatePrepare([&](Universe &candidate, std::string &) {
		const auto audit = candidate.OpenPresentation(destination, Name("audit")).Address;
		if (candidate.SendPresentation(
				source,
				from,
				audit,
				40,
				{},
				engine::world::PresentationPriority::Routine,
				std::span<const std::string_view>{}
			) != engine::world::PresentationStatus::Ok)
			return false;
		return candidate.TakePresentation(audit).size() == 1;
	});
	session.SetCandidateFinal(
		[](Universe &, Name, engine::world::DataFactoryCandidateFinalOperation, std::string &) {
			return false;
		}
	);
	CHECK(session.ResetWorld(reset).Status == DataFactoryStatus::RestoreIncomplete);
	CHECK(universe.LookupPresentation(target, "image") == oldTarget);
	CHECK(universe.PresentationQueueUsage().Messages == 2);
	const auto discardedAfterRefusal = engine::core::Metrics::Get("world.presentation.discarded.messages");
	const auto enqueuedAfterRefusal = engine::core::Metrics::Get("world.presentation.enqueued.messages");
	const auto takenAfterRefusal = engine::core::Metrics::Get("world.presentation.taken.messages");
	CHECK(discardedAfterRefusal.has_value() == discardedBefore.has_value());
	if (discardedBefore && discardedAfterRefusal) {
		CHECK(discardedAfterRefusal->Value == discardedBefore->Value);
		CHECK(discardedAfterRefusal->Samples == discardedBefore->Samples);
	}
	REQUIRE(enqueuedBefore.has_value());
	REQUIRE(enqueuedAfterRefusal.has_value());
	CHECK(enqueuedAfterRefusal->Value == enqueuedBefore->Value);
	CHECK(enqueuedAfterRefusal->Samples == enqueuedBefore->Samples);
	CHECK(takenAfterRefusal.has_value() == takenBefore.has_value());
	if (takenBefore && takenAfterRefusal) {
		CHECK(takenAfterRefusal->Value == takenBefore->Value);
		CHECK(takenAfterRefusal->Samples == takenBefore->Samples);
	}
	session.SetCandidateFinal({});
	REQUIRE(session.ResetWorld(reset).Status == DataFactoryStatus::Ok);
	const auto discardedAfterCommit = engine::core::Metrics::Get("world.presentation.discarded.messages");
	REQUIRE(discardedAfterCommit.has_value());
	CHECK(discardedAfterCommit->Value == (discardedBefore ? discardedBefore->Value : 0.0) + 1.0);
	CHECK(discardedAfterCommit->Samples == (discardedBefore ? discardedBefore->Samples : 0) + 1);
	const auto enqueuedAfterCommit = engine::core::Metrics::Get("world.presentation.enqueued.messages");
	const auto takenAfterCommit = engine::core::Metrics::Get("world.presentation.taken.messages");
	REQUIRE(enqueuedAfterCommit.has_value());
	REQUIRE(takenAfterCommit.has_value());
	CHECK(enqueuedAfterCommit->Value == enqueuedBefore->Value + 1.0);
	CHECK(enqueuedAfterCommit->Samples == enqueuedBefore->Samples + 1);
	CHECK(takenAfterCommit->Value == (takenBefore ? takenBefore->Value : 0.0) + 1.0);
	CHECK(takenAfterCommit->Samples == (takenBefore ? takenBefore->Samples : 0) + 1);
	const WorldId replaced = universe.Find(Name("factory.traffic"));
	const auto newTarget = universe.OpenPresentation(replaced, Name("image")).Address;
	CHECK(newTarget != oldTarget);
	CHECK(
		universe.SendPresentation(replaced, oldTarget, to, 20, {}) ==
		engine::world::PresentationStatus::StaleEndpoint
	);
	const auto messages = universe.TakePresentation(to);
	REQUIRE(messages.size() == 1);
	CHECK(messages[0].Correlation == 17);
	universe.Tick(0.0f);
	const WorldId restored = universe.Find(Name("traffic.destination"));
	REQUIRE(restored.IsValid());
	universe.Enter(restored, [&](Store &store) { CHECK(Postbox(store).Deliveries().size() == 1); });
}

TEST_CASE(
	"factory reset refuses pending presentation that may name its target",
	"[world][data-factory][presentation]"
) {
	Universe universe;
	DataFactorySession session(universe);
	session.SetPauseParticipant([](WorldId, DataFactoryPauseScope, bool, std::string &) { return true; });
	const auto created = session.CreateWorld(
		DataFactoryWorldRequest{
			.Operation = DataFactoryWorldOperation::Create,
			.InstanceId = "factory.references",
			.OperationId = "references-create",
		}
	);
	REQUIRE(created.Status == DataFactoryStatus::Ok);
	const WorldId source = MakeWorld(universe, "references.source");
	const WorldId destination = MakeWorld(universe, "references.destination");
	REQUIRE(universe.ConfigurePresentation(84));
	const auto from = universe.OpenPresentation(source, Name("image")).Address;
	const auto to = universe.OpenPresentation(destination, Name("image")).Address;
	SECTION("unknown references") {
		REQUIRE(universe.SendPresentation(source, from, to, 1, {}) == engine::world::PresentationStatus::Ok);
	}
	SECTION("declared target reference") {
		const std::array<std::string_view, 1> references{"factory.references"};
		REQUIRE(
			universe.SendPresentation(
				source, from, to, 1, {}, engine::world::PresentationPriority::Routine, references
			) == engine::world::PresentationStatus::Ok
		);
	}
	const auto before = session.Inspect("factory.references");
	const auto refused = session.ResetWorld(
		DataFactoryWorldRequest{
			.Operation = DataFactoryWorldOperation::Reset,
			.InstanceId = "factory.references",
			.ExpectedWorldEpoch = before.WorldEpoch,
			.ExpectedWorldVersion = before.WorldVersion,
			.ExpectedTick = before.Clock.Tick,
			.OperationId = "references-reset",
		}
	);
	CHECK(refused.Status == DataFactoryStatus::RestoreIncomplete);
	CHECK(session.Inspect("factory.references").WorldVersion == before.WorldVersion);
	const auto messages = universe.TakePresentation(to);
	REQUIRE(messages.size() == 1);
	CHECK(messages[0].Correlation == 1);
}

TEST_CASE("reset preserves unrelated world script tick rate", "[world][data-factory]") {
	Universe universe;
	DataFactorySession session(universe);
	session.SetPauseParticipant([](WorldId, DataFactoryPauseScope, bool, std::string &) { return true; });
	const auto created = session.CreateWorld(
		DataFactoryWorldRequest{
			.Operation = DataFactoryWorldOperation::Create,
			.InstanceId = "factory.scheduler",
			.OperationId = "scheduler-create",
		}
	);
	REQUIRE(created.Status == DataFactoryStatus::Ok);
	WorldSettings settings;
	settings.Name = Name("unrelated.scheduler");
	settings.ScriptTickRate = 17.0;
	const WorldId unrelated = universe.Create(settings);
	REQUIRE(unrelated.IsValid());
	const auto reset = session.ResetWorld(
		DataFactoryWorldRequest{
			.Operation = DataFactoryWorldOperation::Reset,
			.InstanceId = "factory.scheduler",
			.ExpectedWorldEpoch = created.WorldEpoch,
			.ExpectedWorldVersion = created.WorldVersion,
			.ExpectedTick = created.Clock.Tick,
			.OperationId = "scheduler-reset",
		}
	);
	REQUIRE(reset.Status == DataFactoryStatus::Ok);
	const WorldId restored = universe.Find(Name("unrelated.scheduler"));
	REQUIRE(restored.IsValid());
	CHECK(universe.SettingsOf(restored).ScriptTickRate == 17.0);
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

TEST_CASE("data-factory rolls back failed retirement preparation", "[world][data-factory]") {
	Universe universe;
	DataFactorySession session(universe);
	bool paused = false;
	session.SetPauseParticipant([&](WorldId, DataFactoryPauseScope, bool value, std::string &) {
		paused = value;
		return true;
	});
	const auto created = session.CreateWorld(
		DataFactoryWorldRequest{
			.Operation = DataFactoryWorldOperation::Create,
			.InstanceId = "factory.retire-abort",
			.TickRate = 60.0,
			.OperationId = "retire-abort-create",
		}
	);
	REQUIRE(created.Status == DataFactoryStatus::Ok);
	REQUIRE(paused);
	const auto world = universe.Find(Name("factory.retire-abort"));
	const auto before = session.Inspect("factory.retire-abort");
	bool throws = false;
	bool destroys = false;
	SECTION("preparation refuses") {}
	SECTION("preparation throws") {
		throws = true;
	}
	SECTION("world disappears before destruction") {
		destroys = true;
	}
	int abortCalls = 0;
	session.SetWorldLifecycle([&](DataFactoryWorldOperation,
								  Universe &live,
								  WorldId candidate,
								  bool committed,
								  std::string &detail) {
		CHECK_FALSE(committed);
		CHECK(candidate == world);
		CHECK_FALSE(paused);
		if (throws) throw std::runtime_error("retire setup");
		if (destroys) {
			CHECK(live.Destroy(candidate) == engine::world::WorldStatus::Ok);
			return true;
		}
		detail = "retire refused";
		return false;
	});
	session.SetWorldLifecycleAbort([&](Universe &live, WorldId candidate) {
		++abortCalls;
		CHECK(paused != destroys);
		CHECK(candidate == world);
		CHECK(live.Find(Name("factory.retire-abort")).IsValid() != destroys);
	});
	const auto result = session.RetireWorld(
		DataFactoryWorldRequest{
			.Operation = DataFactoryWorldOperation::Retire,
			.InstanceId = "factory.retire-abort",
			.TickRate = 60.0,
			.ExpectedWorldEpoch = before.WorldEpoch,
			.ExpectedWorldVersion = before.WorldVersion,
			.ExpectedTick = before.Clock.Tick,
			.OperationId = "retire-abort-request",
		}
	);
	CHECK(result.Status == DataFactoryStatus::RestoreIncomplete);
	CHECK(abortCalls == 1);
	CHECK(paused != destroys);
	if (destroys) CHECK(result.Detail == "retirement preparation rollback failed");
	if (!destroys) {
		CHECK(universe.Find(Name("factory.retire-abort")) == world);
		CHECK(session.Inspect("factory.retire-abort").WorldVersion == before.WorldVersion);
	}
}

TEST_CASE("data-factory refuses retirement during a world callback", "[world][data-factory]") {
	Universe universe;
	DataFactorySession session(universe);
	int participantCalls = 0;
	int lifecycleCalls = 0;
	session.SetPauseParticipant([&](WorldId, DataFactoryPauseScope, bool, std::string &) {
		++participantCalls;
		return true;
	});
	const auto created = session.CreateWorld(
		DataFactoryWorldRequest{
			.Operation = DataFactoryWorldOperation::Create,
			.InstanceId = "factory.retire-tick",
			.TickRate = 60.0,
			.OperationId = "retire-tick-create",
		}
	);
	REQUIRE(created.Status == DataFactoryStatus::Ok);
	const WorldId world = universe.Find(Name("factory.retire-tick"));
	const auto before = session.Inspect("factory.retire-tick");
	participantCalls = 0;
	session.SetWorldLifecycle([&](DataFactoryWorldOperation, Universe &, WorldId, bool, std::string &) {
		++lifecycleCalls;
		return true;
	});
	DataFactoryStatus status = DataFactoryStatus::Ok;
	REQUIRE(universe.OnState([&](WorldId, Store &, engine::world::WorldState) {
		status = session
					 .RetireWorld(
						 DataFactoryWorldRequest{
							 .Operation = DataFactoryWorldOperation::Retire,
							 .InstanceId = "factory.retire-tick",
							 .TickRate = 60.0,
							 .ExpectedWorldEpoch = before.WorldEpoch,
							 .ExpectedWorldVersion = before.WorldVersion,
							 .ExpectedTick = before.Clock.Tick,
							 .OperationId = "retire-tick-request",
						 }
					 )
					 .Status;
	}));
	REQUIRE(universe.SetState(world, engine::world::WorldState::Active) == engine::world::WorldStatus::Ok);
	CHECK(status == DataFactoryStatus::RestoreIncomplete);
	CHECK(participantCalls == 0);
	CHECK(lifecycleCalls == 0);
	universe.Tick(0.0f);
	CHECK(universe.Find(Name("factory.retire-tick")) == world);
}

TEST_CASE("data-factory validates final lifecycle candidates before committing", "[world][data-factory]") {
	Universe universe;
	DataFactorySession session(universe);
	int pauseCalls = 0;
	session.SetPauseParticipant([&](WorldId, DataFactoryPauseScope, bool, std::string &) {
		++pauseCalls;
		return true;
	});
	using FinalOperation = engine::world::DataFactoryCandidateFinalOperation;
	int calls = 0;
	session.SetCandidateFinal(
		[&](Universe &candidate, Name affected, FinalOperation operation, std::string &detail) {
			++calls;
			CHECK(affected == Name("factory.final"));
			CHECK(candidate.Find(affected).IsValid());
			CHECK(candidate.SettingsOf(candidate.Find(affected)).TickRate == 30.0);
			CHECK(operation == FinalOperation::Create);
			detail = "host refused final candidate";
			return false;
		}
	);
	DataFactoryWorldRequest create{
		.Operation = DataFactoryWorldOperation::Create,
		.InstanceId = "factory.final",
		.TickRate = 30.0,
		.OperationId = "final-create",
	};
	const auto denied = session.CreateWorld(create);
	CHECK(denied.Status == DataFactoryStatus::RestoreIncomplete);
	CHECK(denied.Detail == "host refused final candidate");
	CHECK(calls == 1);
	CHECK(pauseCalls == 0);
	CHECK(universe.Worlds().empty());

	session.SetCandidateFinal(
		[&](Universe &candidate, Name affected, FinalOperation operation, std::string &) {
			++calls;
			CHECK(operation == FinalOperation::Create);
			CHECK(candidate.Find(affected).IsValid());
			return true;
		}
	);
	const auto created = session.CreateWorld(create);
	REQUIRE(created.Status == DataFactoryStatus::Ok);
	CHECK(calls == 2);
	const auto original = universe.Find(Name("factory.final"));
	REQUIRE(original.IsValid());
	const auto paused = session.Inspect("factory.final");
	const int pausesBefore = pauseCalls;
	session.SetCandidateFinal(
		[&](Universe &candidate, Name affected, FinalOperation operation, std::string &) -> bool {
			++calls;
			CHECK(affected == Name("factory.final"));
			CHECK(operation == FinalOperation::Reset);
			CHECK(candidate.SettingsOf(candidate.Find(affected)).TickRate == 45.0);
			throw std::runtime_error("refused");
		}
	);
	const auto deniedReset = session.ResetWorld(
		DataFactoryWorldRequest{
			.Operation = DataFactoryWorldOperation::Reset,
			.InstanceId = "factory.final",
			.TickRate = 45.0,
			.ExpectedWorldEpoch = paused.WorldEpoch,
			.ExpectedWorldVersion = paused.WorldVersion,
			.ExpectedTick = paused.Clock.Tick,
			.OperationId = "final-reset",
		}
	);
	CHECK(deniedReset.Status == DataFactoryStatus::RestoreIncomplete);
	CHECK(deniedReset.Detail == "final candidate validation threw");
	CHECK(pauseCalls == pausesBefore);
	CHECK(universe.Find(Name("factory.final")) == original);
	CHECK(universe.SettingsOf(original).TickRate == 30.0);
	CHECK(session.Inspect("factory.final").WorldVersion == paused.WorldVersion);
	session.SetCandidateFinal([](Universe &candidate, Name affected, FinalOperation, std::string &) {
		CHECK(candidate.Destroy(candidate.Find(affected)) == engine::world::WorldStatus::Ok);
		return true;
	});
	const auto removedCandidate = session.ResetWorld(
		DataFactoryWorldRequest{
			.Operation = DataFactoryWorldOperation::Reset,
			.InstanceId = "factory.final",
			.TickRate = 45.0,
			.ExpectedWorldEpoch = paused.WorldEpoch,
			.ExpectedWorldVersion = paused.WorldVersion,
			.ExpectedTick = paused.Clock.Tick,
			.OperationId = "final-reset-removed",
		}
	);
	CHECK(removedCandidate.Status == DataFactoryStatus::RestoreIncomplete);
	CHECK(removedCandidate.Detail == "final candidate changed during validation");
	CHECK(universe.Find(Name("factory.final")) == original);
	CHECK(session.Inspect("factory.final").WorldVersion == paused.WorldVersion);
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
