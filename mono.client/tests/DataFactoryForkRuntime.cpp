// The client owns branch VMs separately from its rendered world's runtime list.
// A retained no-script checkpoint may step in isolation and the BranchId becomes
// reusable only after the branch retirement callback releases that ownership.

#include <engine/scene/Part.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/DataFactory.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_test_macros.hpp>

#include <client/DataFactoryRehydrator.hpp>
#include <memory>
#include <string>

TEST_SUITE_ID("client.data-factory-fork-runtime")
TEST_DEPENDS("engine.world.data-factory")

namespace {
	using engine::core::Name;
	using engine::world::DataFactoryForkRequest;
	using engine::world::DataFactoryPauseScope;
	using engine::world::DataFactorySession;
	using engine::world::DataFactoryStatus;
	using engine::world::Universe;
	using engine::world::WorldId;
	using engine::world::WorldSettings;
}

TEST_CASE("client retains a compatible fork runtime by branch id", "[client][data-factory]") {
	engine::scene::EnsureClassTree();
	Universe worlds;
	WorldSettings settings;
	settings.Name = Name("fork-parent");
	const WorldId parent = worlds.Create(settings);
	REQUIRE(parent.IsValid());
	DataFactorySession session(worlds);
	session.SetRehydrate([](Universe &, WorldId, std::string &) { return true; });
	session.SetPauseParticipant([](WorldId, DataFactoryPauseScope, bool, std::string &) { return true; });

	client::DataFactoryRuntimeList runtimes;
	client::DataFactoryRehydrator rehydrator(worlds, runtimes, 0, 640, 360, nullptr, nullptr);
	session.SetForkRehydrator({
		.Prepare = [&rehydrator](
					   std::string_view branchId, Universe &branch, WorldId world, std::string &detail
				   ) { return rehydrator.PrepareFork(branchId, branch, world, detail); },
		.Commit = [&rehydrator](std::string_view branchId) { rehydrator.CommitFork(branchId); },
		.Abort = [&rehydrator](std::string_view branchId) noexcept { rehydrator.AbortFork(branchId); },
		.Retire = [&rehydrator](std::string_view branchId) noexcept { rehydrator.RetireFork(branchId); },
	});

	REQUIRE(
		session.Pause("fork-parent", DataFactoryPauseScope::AllSystems, 0).Status == DataFactoryStatus::Ok
	);
	const auto paused = session.Inspect("fork-parent");
	std::string checkpoint;
	REQUIRE(session.Checkpoint("fork-parent", checkpoint).Status == DataFactoryStatus::Ok);
	const DataFactoryForkRequest request{
		.InstanceId = "fork-parent",
		.CheckpointId = checkpoint,
		.BranchId = "fork-blue",
		.ExpectedWorldEpoch = paused.WorldEpoch,
		.ExpectedWorldVersion = paused.WorldVersion,
		.ExpectedTick = paused.Clock.Tick,
	};
	const auto forked = session.Fork(request);
	REQUIRE(forked.Status == DataFactoryStatus::Ok);
	const auto stepped =
		session.Step("fork-blue", forked.Clock.Interval, forked.Clock.Tick, forked.WorldVersion);
	CHECK(stepped.Status == DataFactoryStatus::Ok);
	const auto retired = session.RetireWorld({
		.Operation = engine::world::DataFactoryWorldOperation::Retire,
		.InstanceId = "fork-blue",
		.ExpectedWorldEpoch = stepped.WorldEpoch,
		.ExpectedWorldVersion = stepped.WorldVersion,
		.ExpectedTick = stepped.Clock.Tick,
		.OperationId = "retire-fork-blue",
	});
	REQUIRE(retired.Status == DataFactoryStatus::Ok);
	CHECK_FALSE(session.OwnsFork("fork-blue"));
	const auto reused = session.Fork(request);
	REQUIRE(reused.Status == DataFactoryStatus::Ok);
	CHECK(reused.WorldEpoch != forked.WorldEpoch);
	CHECK(
		session.Step("fork-blue", forked.Clock.Interval, stepped.Clock.Tick, stepped.WorldVersion).Status ==
		DataFactoryStatus::VersionConflict
	);
}
