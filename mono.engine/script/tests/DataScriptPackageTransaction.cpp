#include <engine/assets/ContentHash.hpp>
#include <engine/core/Bytes.hpp>
#include <engine/physics/Clock.hpp>
#include <engine/physics/Pipeline.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/UpdateCadence.hpp>
#include <engine/script/Clock.hpp>
#include <engine/script/DataScriptPackageTransaction.hpp>
#include <engine/script/SourceCache.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/DataFactory.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_test_macros.hpp>

#include <span>
#include <stdexcept>
#include <vector>

TEST_SUITE_ID("engine.script.data-script-package-transaction")
TEST_DEPENDS("engine.script.data-script-package")
TEST_DEPENDS("engine.world.data-factory")
TEST_DEPENDS("engine.ecs.scheduler")
TEST_DEPENDS("engine.scene.update-cadence")
TEST_DEPENDS("engine.physics.clock")
TEST_DEPENDS("engine.script.clock")

namespace {
	struct UnserializableResource {
		std::string Value;
	};
	class Runtime final : public engine::script::Runtime {
	  public:
		Runtime(engine::ecs::Store &store, const engine::script::RuntimeLimits &limits)
			: engine::script::Runtime(store, limits) {}
		bool Run(std::string_view, std::string_view) override {
			return true;
		}
		bool RunInstance(engine::ecs::Entity) override {
			return false;
		}
		bool Heartbeat(float) override {
			return true;
		}
		engine::script::Language Which() const override {
			return engine::script::Language::Luau;
		}
	};

	struct SavedCadence {
		engine::ecs::Entity Subject;
		engine::scene::UpdateCadence Policy;
		engine::scene::UpdateCadenceState State;
		engine::physics::PhysicsClock Physics;
		engine::script::ScriptClock Scripts;
	};

	SavedCadence InstallCadence(engine::ecs::Store &store) {
		using namespace engine;
		scene::RegisterSceneComponents();
		physics::RegisterPhysicsComponents();
		script::RegisterScriptComponents();
		SavedCadence saved;
		saved.Subject = store.Create();
		saved.Policy.Identity = core::Name("package.subject");
		saved.Policy.Owner = saved.Subject;
		saved.Policy.Revision = 7;
		saved.Policy.DemotionDelayTicks = 3;
		for (size_t index = 0; index < scene::UPDATE_DOMAIN_COUNT; ++index) {
			saved.Policy.Domains[index] = {4, 6};
			auto &state = saved.State.Domains[index];
			const auto due =
				scene::CadenceDue(saved.Policy, state, static_cast<scene::UpdateDomain>(index), 0, 9);
			REQUIRE(scene::CommitUpdate(state, due, 0, 9, saved.Policy.Revision, true));
		}
		store.Set(saved.Subject, saved.Policy);
		store.Set(saved.Subject, saved.State);
		saved.Physics.Rate = 24.0;
		saved.Physics.Accumulator = 0.01;
		saved.Physics.Paused = true;
		saved.Physics.Steps = 12;
		saved.Physics.DroppedSteps = 2;
		store.SetResource(saved.Physics);
		saved.Scripts = {.Rate = 23.0, .Accumulator = 0.02, .ObservedTick = 0};
		store.SetResource(saved.Scripts);
		return saved;
	}

	void CheckCadence(engine::ecs::Store &store, const SavedCadence &saved) {
		using namespace engine;
		const auto *policy = store.Get<scene::UpdateCadence>(saved.Subject);
		const auto *state = store.Get<scene::UpdateCadenceState>(saved.Subject);
		REQUIRE(policy != nullptr);
		REQUIRE(state != nullptr);
		CHECK(policy->Identity == saved.Policy.Identity);
		CHECK(policy->Owner == saved.Policy.Owner);
		CHECK(policy->Revision == saved.Policy.Revision);
		for (size_t index = 0; index < scene::UPDATE_DOMAIN_COUNT; ++index) {
			CHECK(state->Domains[index].Generation == saved.State.Domains[index].Generation);
			CHECK(state->Domains[index].HoldUntilTick == saved.State.Domains[index].HoldUntilTick);
			CHECK(state->Domains[index].LastCommittedTick == saved.State.Domains[index].LastCommittedTick);
			for (uint64_t tick = 1; tick < 12; ++tick) {
				const auto domain = static_cast<scene::UpdateDomain>(index);
				const auto expected =
					scene::CadenceDue(saved.Policy, saved.State.Domains[index], domain, tick, 9);
				const auto restored = scene::CadenceDue(*policy, state->Domains[index], domain, tick, 9);
				CHECK(restored.Due == expected.Due);
				CHECK(restored.Reason == expected.Reason);
				CHECK(restored.AgeTicks == expected.AgeTicks);
			}
		}
		const auto *physicsClock = store.Resource<physics::PhysicsClock>();
		const auto *scripts = store.Resource<script::ScriptClock>();
		REQUIRE(physicsClock != nullptr);
		REQUIRE(scripts != nullptr);
		CHECK(physicsClock->Rate == saved.Physics.Rate);
		CHECK(physicsClock->Accumulator == saved.Physics.Accumulator);
		CHECK(physicsClock->Paused == saved.Physics.Paused);
		CHECK(physicsClock->Steps == saved.Physics.Steps);
		CHECK(physicsClock->DroppedSteps == saved.Physics.DroppedSteps);
		CHECK(scripts->Rate == saved.Scripts.Rate);
		CHECK(scripts->Accumulator == saved.Scripts.Accumulator);
		CHECK(scripts->ObservedTick == saved.Scripts.ObservedTick);
	}

	std::string Manifest(std::string_view source) {
		const std::string hash =
			engine::assets::Hasher::Of(std::as_bytes(std::span(source.data(), source.size()))).ToHex();
		return "{\"format\":\"atomic.data-script.v1\",\"entry\":\"package.luau\",\"source_hash\":\"" + hash +
			   "\",\"assets\":[],\"parameters\":[],\"capabilities\":[],\"budget\":{\"source_bytes\":1024,"
			   "\"asset_bytes\":0,\"assets\":0,\"parameters\":0},\"seed\":0}";
	}

	engine::script::DataScriptResult Run(bool succeed, bool throwAfterSwap = false) {
		engine::world::Universe worlds;
		engine::world::DataFactorySession session(worlds);
		REQUIRE(worlds.Create({.Name = engine::core::Name("package")}).IsValid());
		session.SetPauseParticipant(
			[](engine::world::WorldId, engine::world::DataFactoryPauseScope, bool, std::string &) {
				return true;
			}
		);
		const auto before = session.Inspect("package");
		REQUIRE(
			session.Pause("package", engine::world::DataFactoryPauseScope::AllSystems, before.Clock.Tick)
				.Status == engine::world::DataFactoryStatus::Ok
		);
		const auto paused = session.Inspect("package");
		return engine::script::ExecuteDataScriptPackageTransaction(
			{.Universe = worlds,
			 .Session = session,
			 .MakeRuntime = [](
								engine::ecs::Store &store, const engine::script::RuntimeLimits &limits
							) { return std::make_unique<Runtime>(store, limits); },
			 .RunPackage =
				 [succeed](
					 engine::script::Runtime &,
					 const engine::script::DataScriptPackageContext &,
					 std::string_view,
					 std::string_view
				 ) {
					 return engine::script::DataScriptPackageRunResult{
						 .Terminal = succeed ? engine::script::DataScriptPackageRunResult::State::Completed
											 : engine::script::DataScriptPackageRunResult::State::Failed,
						 .Error = "failed"
					 };
				 },
			 .InstallSystems = [](engine::ecs::Store &, engine::ecs::Scheduler &) {},
			 .AfterSwap =
				 [throwAfterSwap](engine::world::WorldId) {
					 if (throwAfterSwap) throw std::runtime_error("post-swap");
				 },
			 .Admit = [](std::string_view, std::string_view, std::string &) { return true; }},
			{.InstanceId = "package",
			 .Manifest = Manifest("return"),
			 .Source = "return",
			 .Assets = {},
			 .SourceHash = {},
			 .ExpectedTick = paused.Clock.Tick,
			 .ExpectedEpoch = paused.WorldEpoch,
			 .ExpectedVersion = paused.WorldVersion}
		);
	}
}

TEST_CASE("package preserves live edits during preparation and teardown", "[script][data-script-package]") {
	using namespace engine;
	bool retain = false;
	bool mutateDuringTeardown = false;
	bool changeRetention = false;
	bool breakSnapshot = false;
	bool changeStructure = false;
	bool candidateFails = false;
	bool candidateThrows = false;
	SECTION("replaced universe, preparation") {}
	SECTION("replaced universe, teardown") {
		mutateDuringTeardown = true;
	}
	SECTION("retained worlds, preparation") {
		retain = true;
	}
	SECTION("retained worlds, teardown") {
		retain = true;
		mutateDuringTeardown = true;
	}
	SECTION("teardown changes retention after an edit") {
		mutateDuringTeardown = true;
		changeRetention = true;
	}
	SECTION("teardown makes the snapshot unavailable") {
		mutateDuringTeardown = true;
		breakSnapshot = true;
	}
	SECTION("preparation changes structure after an edit") {
		retain = true;
		changeStructure = true;
	}
	SECTION("candidate rejects after an edit") {
		candidateFails = true;
	}
	SECTION("candidate throws after an edit") {
		candidateThrows = true;
	}
	world::Universe worlds;
	world::DataFactorySession session(worlds);
	session.SetRetainUnaffectedWorlds(retain);
	const auto live = worlds.Create({.Name = core::Name("package")});
	REQUIRE(live.IsValid());
	session.SetPauseParticipant([](world::WorldId, world::DataFactoryPauseScope, bool, std::string &) {
		return true;
	});
	REQUIRE(
		session.Pause("package", world::DataFactoryPauseScope::AllSystems, 0).Status ==
		world::DataFactoryStatus::Ok
	);
	const auto before = session.Inspect("package");
	ecs::Entity created = ecs::NULL_ENTITY;
	const auto edit = [&] { worlds.Enter(live, [&](ecs::Store &store) { created = store.Create(); }); };
	if (!mutateDuringTeardown)
		session.SetCandidateFinal(
			[&](world::Universe &, core::Name, world::DataFactoryCandidateFinalOperation, std::string &) {
				edit();
				if (changeStructure) (void)worlds.Create({.Name = core::Name("callback-added-world")});
				if (candidateThrows) throw std::runtime_error("candidate failed");
				return !candidateFails;
			}
		);
	int discarded = 0;
	const auto result = script::ExecuteDataScriptPackageTransaction(
		{.Universe = worlds,
		 .Session = session,
		 .DiscardRuntime =
			 [&](world::WorldId) {
				 ++discarded;
				 if (mutateDuringTeardown) edit();
				 if (changeRetention) session.SetRetainUnaffectedWorlds(!retain);
				 if (breakSnapshot)
					 worlds.Enter(live, [](ecs::Store &store) {
						 store.SetResource(UnserializableResource{"not serializable"});
					 });
			 },
		 .MakeRuntime = [](
							ecs::Store &store, const script::RuntimeLimits &limits
						) { return std::make_unique<Runtime>(store, limits); },
		 .RunPackage =
			 [](script::Runtime &,
				const script::DataScriptPackageContext &,
				std::string_view,
				std::string_view) {
				 return script::DataScriptPackageRunResult{
					 .Terminal = script::DataScriptPackageRunResult::State::Completed, .Error = {}
				 };
			 },
		 .InstallSystems = [](ecs::Store &, ecs::Scheduler &) {},
		 .Admit = [](std::string_view, std::string_view, std::string &) { return true; }},
		{.InstanceId = "package",
		 .Manifest = Manifest("return"),
		 .Source = "return",
		 .Assets = {},
		 .SourceHash = {},
		 .ExpectedTick = before.Clock.Tick,
		 .ExpectedEpoch = before.WorldEpoch,
		 .ExpectedVersion = before.WorldVersion}
	);
	CHECK_FALSE(result.Ran);
	CHECK_FALSE(result.Atomic);
	CHECK(
		result.Lifecycle.Status ==
		((candidateFails || candidateThrows || (mutateDuringTeardown && !changeRetention))
			 ? world::DataFactoryStatus::RestoreIncomplete
			 : world::DataFactoryStatus::VersionConflict)
	);
	CHECK(discarded == (mutateDuringTeardown ? 1 : 0));
	CHECK(result.Lifecycle.WorldVersion == before.WorldVersion + 1);
	CHECK(session.Inspect("package").WorldVersion == before.WorldVersion + 1);
	REQUIRE(created != ecs::NULL_ENTITY);
	bool survived = false;
	worlds.Enter(live, [&](ecs::Store &store) { survived = store.Alive(created); });
	CHECK(survived);
}

TEST_CASE("data-script transaction commits only terminal package work", "[script][data-script-package]") {
	CHECK(Run(true).Ran);
	CHECK_FALSE(Run(false).Ran);
}

TEST_CASE(
	"data-script transaction keeps a committed swap after host reconciliation throws",
	"[script][data-script-package]"
) {
	const auto result = Run(true, true);
	CHECK(result.Ran);
	CHECK(result.Atomic);
}

TEST_CASE(
	"data-script teardown fails without committing a stale candidate", "[script][data-script-package]"
) {
	using namespace engine;
	world::Universe worlds;
	world::DataFactorySession session(worlds);
	const auto live = worlds.Create({.Name = core::Name("package")});
	REQUIRE(live.IsValid());
	session.SetPauseParticipant([](world::WorldId, world::DataFactoryPauseScope, bool, std::string &) {
		return true;
	});
	REQUIRE(
		session.Pause("package", world::DataFactoryPauseScope::AllSystems, 0).Status ==
		world::DataFactoryStatus::Ok
	);
	const auto before = session.Inspect("package");
	bool reenter = false;
	bool activateDirectly = false;
	SECTION("throwing teardown") {}
	SECTION("teardown changes session state") {
		reenter = true;
	}
	SECTION("teardown directly activates the world") {
		activateDirectly = true;
	}
	uint64_t identity = 0;
	worlds.Enter(live, [&](ecs::Store &store) { identity = store.Identity(); });
	script::DataScriptResult result;
	bool escaped = false;
	try {
		result = script::ExecuteDataScriptPackageTransaction(
			{.Universe = worlds,
			 .Session = session,
			 .DiscardRuntime =
				 [&](world::WorldId) {
					 if (reenter) {
						 CHECK(
							 session.Resume("package", before.Clock.Tick).Status ==
							 world::DataFactoryStatus::Ok
						 );
						 return;
					 }
					 if (activateDirectly) {
						 CHECK(worlds.SetState(live, world::WorldState::Active) == world::WorldStatus::Ok);
						 return;
					 }
					 throw std::runtime_error("teardown failed");
				 },
			 .MakeRuntime = [](
								ecs::Store &store, const script::RuntimeLimits &limits
							) { return std::make_unique<Runtime>(store, limits); },
			 .RunPackage =
				 [](script::Runtime &,
					const script::DataScriptPackageContext &,
					std::string_view,
					std::string_view) {
					 return script::DataScriptPackageRunResult{
						 .Terminal = script::DataScriptPackageRunResult::State::Completed
					 };
				 },
			 .InstallSystems = [](ecs::Store &, ecs::Scheduler &) {},
			 .Admit = [](std::string_view, std::string_view, std::string &) { return true; }},
			{.InstanceId = "package",
			 .Manifest = Manifest("return"),
			 .Source = "return",
			 .ExpectedTick = before.Clock.Tick,
			 .ExpectedEpoch = before.WorldEpoch,
			 .ExpectedVersion = before.WorldVersion}
		);
	} catch (...) {
		escaped = true;
	}
	CHECK_FALSE(escaped);
	CHECK_FALSE(result.Ran);
	const auto after = session.Inspect("package");
	CHECK(after.WorldEpoch == before.WorldEpoch);
	CHECK(after.WorldVersion == before.WorldVersion + (reenter ? 1 : 0));
	CHECK(result.Lifecycle.WorldVersion == after.WorldVersion);
	CHECK(
		result.Lifecycle.Status == (reenter || activateDirectly ? world::DataFactoryStatus::VersionConflict
																: world::DataFactoryStatus::RestoreIncomplete)
	);
	uint64_t currentIdentity = 0;
	worlds.Enter(live, [&](ecs::Store &store) { currentIdentity = store.Identity(); });
	CHECK(currentIdentity == identity);
}

TEST_CASE(
	"data-script replacement retains unrelated presentation traffic",
	"[script][data-script-package][presentation]"
) {
	using namespace engine;
	world::Universe worlds;
	world::DataFactorySession session(worlds);
	const auto target = worlds.Create({.Name = core::Name("package")});
	const auto source = worlds.Create({.Name = core::Name("source")});
	const auto destination = worlds.Create({.Name = core::Name("destination")});
	REQUIRE(target.IsValid());
	session.SetPauseParticipant([](world::WorldId, world::DataFactoryPauseScope, bool, std::string &) {
		return true;
	});
	REQUIRE(
		session.Pause("package", world::DataFactoryPauseScope::AllSystems, 0).Status ==
		world::DataFactoryStatus::Ok
	);
	const auto paused = session.Inspect("package");
	REQUIRE(worlds.ConfigurePresentation(81));
	const auto from = worlds.OpenPresentation(source, core::Name("image")).Address;
	const auto to = worlds.OpenPresentation(destination, core::Name("image")).Address;
	const auto oldTarget = worlds.OpenPresentation(target, core::Name("image")).Address;
	REQUIRE(
		worlds.SendPresentation(
			source,
			from,
			to,
			11,
			{},
			world::PresentationPriority::Routine,
			std::span<const std::string_view>{}
		) == world::PresentationStatus::Ok
	);
	REQUIRE(worlds.SendPresentation(target, oldTarget, to, 12, {}) == world::PresentationStatus::Ok);
	const auto run = [&](bool succeed) {
		return script::ExecuteDataScriptPackageTransaction(
			{.Universe = worlds,
			 .Session = session,
			 .MakeRuntime = [](
								ecs::Store &store, const script::RuntimeLimits &limits
							) { return std::make_unique<Runtime>(store, limits); },
			 .RunPackage =
				 [succeed](
					 script::Runtime &,
					 const script::DataScriptPackageContext &,
					 std::string_view,
					 std::string_view
				 ) {
					 return script::DataScriptPackageRunResult{
						 .Terminal = succeed ? script::DataScriptPackageRunResult::State::Completed
											 : script::DataScriptPackageRunResult::State::Failed
					 };
				 },
			 .InstallSystems = [](ecs::Store &, ecs::Scheduler &) {},
			 .Admit = [](std::string_view, std::string_view, std::string &) { return true; }},
			{.InstanceId = "package",
			 .Manifest = Manifest("return"),
			 .Source = "return",
			 .ExpectedTick = paused.Clock.Tick,
			 .ExpectedEpoch = paused.WorldEpoch,
			 .ExpectedVersion = paused.WorldVersion}
		);
	};
	CHECK_FALSE(run(false).Ran);
	CHECK(worlds.LookupPresentation(target, "image") == oldTarget);
	CHECK(worlds.PresentationQueueUsage().Messages == 2);
	REQUIRE(run(true).Ran);
	const auto replaced = worlds.Find(core::Name("package"));
	const auto newTarget = worlds.OpenPresentation(replaced, core::Name("image")).Address;
	CHECK(newTarget != oldTarget);
	CHECK(
		worlds.SendPresentation(replaced, oldTarget, to, 13, {}) == world::PresentationStatus::StaleEndpoint
	);
	const auto messages = worlds.TakePresentation(to);
	REQUIRE(messages.size() == 1);
	CHECK(messages[0].Correlation == 11);
}

TEST_CASE(
	"data-script package preserves an unrelated world runtime", "[script][data-script-package][retention]"
) {
	using namespace engine;
	world::Universe worlds;
	world::DataFactorySession session(worlds);
	session.SetRetainUnaffectedWorlds(true);
	const auto target = worlds.Create({.Name = core::Name("package")});
	const auto hole = worlds.Create({.Name = core::Name("hole")});
	const auto other = worlds.Create({.Name = core::Name("other")});
	REQUIRE(target.IsValid());
	REQUIRE(hole.IsValid());
	REQUIRE(other.IsValid());
	REQUIRE(worlds.Destroy(hole) == world::WorldStatus::Ok);
	ecs::Store *original = nullptr;
	uint64_t identity = 0;
	int calls = 0;
	REQUIRE(worlds.Enter(other, [&](ecs::Store &store, ecs::Scheduler &systems) {
		original = &store;
		identity = store.Identity();
		systems.Add("package.retained", ecs::Phase::Simulation, [&](ecs::Store &current) {
			CHECK(&current == original);
			++calls;
		});
	}) == world::WorldStatus::Ok);
	session.SetPauseParticipant([](world::WorldId, world::DataFactoryPauseScope, bool, std::string &) {
		return true;
	});
	REQUIRE(
		session.Pause("package", world::DataFactoryPauseScope::AllSystems, 0).Status ==
		world::DataFactoryStatus::Ok
	);
	const auto before = session.Inspect("package");
	const auto result = script::ExecuteDataScriptPackageTransaction(
		{.Universe = worlds,
		 .Session = session,
		 .MakeRuntime = [](
							ecs::Store &store, const script::RuntimeLimits &limits
						) { return std::make_unique<Runtime>(store, limits); },
		 .RunPackage =
			 [](script::Runtime &,
				const script::DataScriptPackageContext &,
				std::string_view,
				std::string_view) {
				 return script::DataScriptPackageRunResult{
					 .Terminal = script::DataScriptPackageRunResult::State::Completed
				 };
			 },
		 .InstallSystems = [](ecs::Store &, ecs::Scheduler &) {},
		 .Admit = [](std::string_view, std::string_view, std::string &) { return true; }},
		{.InstanceId = "package",
		 .Manifest = Manifest("return"),
		 .Source = "return",
		 .ExpectedTick = before.Clock.Tick,
		 .ExpectedEpoch = before.WorldEpoch,
		 .ExpectedVersion = before.WorldVersion}
	);
	REQUIRE(result.Ran);
	CHECK(worlds.Find(core::Name("other")) == other);
	REQUIRE(worlds.Enter(other, [&](ecs::Store &store, ecs::Scheduler &systems) {
		CHECK(&store == original);
		CHECK(store.Identity() == identity);
		CHECK(systems.SystemCount() == 1);
	}) == world::WorldStatus::Ok);
	worlds.Tick(0.1f);
	CHECK(calls > 0);
}

TEST_CASE(
	"data-script rejects a universe with an unfinished replacement",
	"[script][data-script-package][retention]"
) {
	using namespace engine;
	world::Universe source;
	REQUIRE(source.Create({.Name = core::Name("package")}).IsValid());
	REQUIRE(source.Create({.Name = core::Name("other")}).IsValid());
	world::Universe candidate;
	REQUIRE(candidate.StageReplacement(source, core::Name("package"), 64 * 1024 * 1024));
	world::DataFactorySession session(candidate);
	bool retain = false;
	SECTION("ordinary session") {}
	SECTION("retaining session") {
		retain = true;
	}
	session.SetRetainUnaffectedWorlds(retain);
	session.SetPauseParticipant([](world::WorldId, world::DataFactoryPauseScope, bool, std::string &) {
		return true;
	});
	REQUIRE(
		session.Pause("package", world::DataFactoryPauseScope::AllSystems, 0).Status ==
		world::DataFactoryStatus::Ok
	);
	const auto before = session.Inspect("package");
	const auto target = candidate.Find(core::Name("package"));
	const auto identity = candidate.StoreIdentityOf(target);
	int discarded = 0;
	const auto result = script::ExecuteDataScriptPackageTransaction(
		{.Universe = candidate,
		 .Session = session,
		 .DiscardRuntime = [&](world::WorldId) { ++discarded; },
		 .MakeRuntime = [](
							ecs::Store &store, const script::RuntimeLimits &limits
						) { return std::make_unique<Runtime>(store, limits); },
		 .RunPackage =
			 [](script::Runtime &,
				const script::DataScriptPackageContext &,
				std::string_view,
				std::string_view) {
				 return script::DataScriptPackageRunResult{
					 .Terminal = script::DataScriptPackageRunResult::State::Completed
				 };
			 },
		 .InstallSystems = [](ecs::Store &, ecs::Scheduler &) {},
		 .Admit = [](std::string_view, std::string_view, std::string &) { return true; }},
		{.InstanceId = "package",
		 .Manifest = Manifest("return"),
		 .Source = "return",
		 .ExpectedTick = before.Clock.Tick,
		 .ExpectedEpoch = before.WorldEpoch,
		 .ExpectedVersion = before.WorldVersion}
	);
	CHECK_FALSE(result.Ran);
	CHECK(discarded == 0);
	CHECK(session.Inspect("package").WorldVersion == before.WorldVersion);
	CHECK(candidate.StoreIdentityOf(target) == identity);
}

TEST_CASE("data-script validates the complete candidate before committing", "[script][data-script-package]") {
	using namespace engine;
	world::Universe worlds;
	world::DataFactorySession session(worlds);
	const auto live = worlds.Create({.Name = core::Name("package")});
	REQUIRE(live.IsValid());
	session.SetPauseParticipant([](world::WorldId, world::DataFactoryPauseScope, bool, std::string &) {
		return true;
	});
	REQUIRE(
		session.Pause("package", world::DataFactoryPauseScope::AllSystems, 0).Status ==
		world::DataFactoryStatus::Ok
	);
	const auto before = session.Inspect("package");
	uint64_t liveIdentity = 0;
	worlds.Enter(live, [&](ecs::Store &store) { liveIdentity = store.Identity(); });
	bool installed = false;
	int called = 0;
	bool throws = false;
	bool removes = false;
	bool accepts = false;
	bool invalidSchedule = false;
	int discarded = 0;
	SECTION("refusal") {}
	SECTION("final validator leaves an invalid system graph") {
		invalidSchedule = true;
	}
	SECTION("exception") {
		throws = true;
	}
	SECTION("candidate world removed") {
		removes = true;
	}
	SECTION("acceptance") {
		accepts = true;
	}
	session.SetCandidateFinal([&](world::Universe &candidate,
								  core::Name affected,
								  world::DataFactoryCandidateFinalOperation operation,
								  std::string &detail) {
		++called;
		CHECK(installed);
		CHECK(affected == core::Name("package"));
		CHECK(operation == world::DataFactoryCandidateFinalOperation::Package);
		CHECK(candidate.Find(affected).IsValid());
		CHECK(&candidate != &worlds);
		if (throws) throw std::runtime_error("rejected");
		if (removes) {
			CHECK(candidate.Destroy(candidate.Find(affected)) == world::WorldStatus::Ok);
			return true;
		}
		if (invalidSchedule) {
			candidate.Enter(candidate.Find(affected), [](ecs::Store &, ecs::Scheduler &systems) {
				systems.Add("duplicate", ecs::Phase::Simulation, [](ecs::Store &) {});
				systems.Add("duplicate", ecs::Phase::Simulation, [](ecs::Store &) {});
			});
			return true;
		}
		if (accepts) return true;
		detail = "host rejected package candidate";
		return false;
	});
	const auto result = script::ExecuteDataScriptPackageTransaction(
		{.Universe = worlds,
		 .Session = session,
		 .DiscardRuntime = [&](world::WorldId) { ++discarded; },
		 .MakeRuntime = [](
							ecs::Store &store, const script::RuntimeLimits &limits
						) { return std::make_unique<Runtime>(store, limits); },
		 .RunPackage =
			 [](script::Runtime &,
				const script::DataScriptPackageContext &,
				std::string_view,
				std::string_view) {
				 return script::DataScriptPackageRunResult{
					 .Terminal = script::DataScriptPackageRunResult::State::Completed, .Error = {}
				 };
			 },
		 .InstallSystems = [&](ecs::Store &, ecs::Scheduler &) { installed = true; },
		 .Admit = [](std::string_view, std::string_view, std::string &) { return true; }},
		{.InstanceId = "package",
		 .Manifest = Manifest("return"),
		 .Source = "return",
		 .Assets = {},
		 .SourceHash = {},
		 .ExpectedTick = before.Clock.Tick,
		 .ExpectedEpoch = before.WorldEpoch,
		 .ExpectedVersion = before.WorldVersion}
	);
	CHECK(result.Ran == accepts);
	CHECK(
		result.Error == (accepts		   ? ""
						 : throws		   ? "final candidate validation threw"
						 : removes		   ? "final candidate changed during validation"
						 : invalidSchedule ? "final candidate system graph is invalid"
										   : "host rejected package candidate")
	);
	CHECK(called == 1);
	CHECK(discarded == (accepts ? 1 : 0));
	CHECK(session.Inspect("package").WorldVersion == before.WorldVersion + (accepts ? 1 : 0));
	uint64_t afterIdentity = 0;
	worlds.Enter(live, [&](ecs::Store &store) { afterIdentity = store.Identity(); });
	CHECK((afterIdentity == liveIdentity) != accepts);
}

TEST_CASE("package refuses late scheduler edits without losing callbacks", "[script][data-script-package]") {
	using namespace engine;
	bool retain = false;
	bool replace = false;
	bool assign = false;
	SECTION("whole universe add") {}
	SECTION("retained add") {
		retain = true;
	}
	SECTION("retained replace") {
		retain = true;
		replace = true;
	}
	SECTION("retained assignment") {
		retain = true;
		assign = true;
	}
	world::Universe worlds;
	world::DataFactorySession session(worlds);
	session.SetRetainUnaffectedWorlds(retain);
	const auto live = worlds.Create({.Name = core::Name("package")});
	int calls = 0;
	SavedCadence cadence;
	worlds.Enter(live, [&](ecs::Store &store) { cadence = InstallCadence(store); });
	worlds.Enter(live, [](ecs::Store &, ecs::Scheduler &systems) {
		systems.Add("original", ecs::Phase::Simulation, [](ecs::Store &) {});
	});
	session.SetPauseParticipant([](world::WorldId, world::DataFactoryPauseScope, bool, std::string &) {
		return true;
	});
	REQUIRE(
		session.Pause("package", world::DataFactoryPauseScope::AllSystems, 0).Status ==
		world::DataFactoryStatus::Ok
	);
	const auto before = session.Inspect("package");
	const auto ran = script::ExecuteDataScriptPackageTransaction(
		{.Universe = worlds,
		 .Session = session,
		 .DiscardRuntime =
			 [&](world::WorldId id) {
				 worlds.Enter(id, [&](ecs::Store &, ecs::Scheduler &systems) {
					 auto callback = [&](ecs::Store &) { ++calls; };
					 if (replace)
						 REQUIRE(systems.Replace("original", 2, callback));
					 else if (assign) {
						 ecs::Scheduler changed;
						 changed.Add("late", ecs::Phase::Simulation, callback);
						 systems = std::move(changed);
					 } else
						 systems.Add("late", ecs::Phase::Simulation, callback);
				 });
			 },
		 .MakeRuntime = [](
							ecs::Store &store, const script::RuntimeLimits &limits
						) { return std::make_unique<Runtime>(store, limits); },
		 .RunPackage =
			 [](script::Runtime &,
				const script::DataScriptPackageContext &,
				std::string_view,
				std::string_view) {
				 return script::DataScriptPackageRunResult{
					 .Terminal = script::DataScriptPackageRunResult::State::Completed
				 };
			 },
		 .InstallSystems = [](ecs::Store &, ecs::Scheduler &) {},
		 .Admit = [](std::string_view, std::string_view, std::string &) { return true; }},
		{.InstanceId = "package",
		 .Manifest = Manifest("return"),
		 .Source = "return",
		 .ExpectedTick = before.Clock.Tick,
		 .ExpectedEpoch = before.WorldEpoch,
		 .ExpectedVersion = before.WorldVersion}
	);
	CHECK_FALSE(ran.Ran);
	CHECK_FALSE(ran.Atomic);
	CHECK(ran.Lifecycle.Status == world::DataFactoryStatus::RestoreIncomplete);
	CHECK(session.Inspect("package").WorldVersion == before.WorldVersion + 1);
	REQUIRE(worlds.Enter(live, [&](ecs::Store &store, ecs::Scheduler &systems) {
		CheckCadence(store, cadence);
		systems.RunPhases(store, ecs::Phase::Simulation, ecs::Phase::Simulation);
	}) == world::WorldStatus::Ok);
	CHECK(calls == 1);
}

TEST_CASE(
	"package preserves accepted scheduler callbacks state and dependency order",
	"[script][data-script-package]"
) {
	using namespace engine;
	world::Universe worlds;
	world::DataFactorySession session(worlds);
	bool retain = false;
	bool bound = false;
	bool retireBound = false;
	int discarded = 0;
	bool initialRun = true;
	SECTION("whole universe portable override") {}
	SECTION("retained worlds portable override") {
		retain = true;
	}
	SECTION("retained worlds bound callback") {
		retain = true;
		bound = true;
	}
	SECTION("required dependency on retired callback refuses before teardown") {
		retain = true;
		bound = true;
		retireBound = true;
	}
	session.SetRetainUnaffectedWorlds(retain);
	const auto live = worlds.Create({.Name = core::Name("package"), .ReplicationTickRate = 15.0});
	std::vector<int> calls;
	uint64_t originalStore = 0;
	SavedCadence cadence;
	worlds.Enter(live, [&](ecs::Store &store, ecs::Scheduler &systems) {
		originalStore = store.Identity();
		cadence = InstallCadence(store);
		systems.Add(
			"custom",
			ecs::Phase::Simulation,
			[&](ecs::Store &current) {
				CHECK((initialRun || current.Identity() != originalStore));
				calls.push_back(2);
			},
			ecs::SystemOrder({}, {"host"})
		);
		systems.Add(
			"host",
			ecs::Phase::Simulation,
			[&, count = 0](ecs::Store &) mutable { calls.push_back(-++count); },
			{},
			bound ? ecs::SystemLifetime::WorldBound : ecs::SystemLifetime::Portable
		);
	});
	worlds.Tick(1.0f / 60.0f);
	worlds.Enter(live, [&](ecs::Store &store, ecs::Scheduler &systems) {
		systems.RunPhases(store, ecs::Phase::Simulation, ecs::Phase::Simulation);
	});
	calls.clear();
	initialRun = false;
	core::ByteWriter baseline;
	REQUIRE(worlds.Save(baseline));
	core::ByteReader baselineReader(baseline.Bytes());
	world::Universe cadenceReference;
	REQUIRE(cadenceReference.Load(baselineReader));
	const auto referenceWorld = cadenceReference.Find(core::Name("package"));
	session.SetPauseParticipant([](world::WorldId, world::DataFactoryPauseScope, bool, std::string &) {
		return true;
	});
	REQUIRE(
		session
			.Pause("package", world::DataFactoryPauseScope::AllSystems, session.Inspect("package").Clock.Tick)
			.Status == world::DataFactoryStatus::Ok
	);
	const auto before = session.Inspect("package");
	const auto ran = script::ExecuteDataScriptPackageTransaction(
		{.Universe = worlds,
		 .Session = session,
		 .DiscardRuntime = [&](world::WorldId) { ++discarded; },
		 .MakeRuntime = [](
							ecs::Store &store, const script::RuntimeLimits &limits
						) { return std::make_unique<Runtime>(store, limits); },
		 .RunPackage =
			 [](script::Runtime &,
				const script::DataScriptPackageContext &,
				std::string_view,
				std::string_view) {
				 return script::DataScriptPackageRunResult{
					 .Terminal = script::DataScriptPackageRunResult::State::Completed
				 };
			 },
		 .InstallSystems =
			 [&](ecs::Store &, ecs::Scheduler &systems) {
				 if (retireBound) return;
				 systems.Add(
					 "host",
					 ecs::Phase::Simulation,
					 [&](ecs::Store &) { calls.push_back(1); },
					 {},
					 ecs::SystemLifetime::WorldBound
				 );
			 },
		 .Admit = [](std::string_view, std::string_view, std::string &) { return true; }},
		{.InstanceId = "package",
		 .Manifest = Manifest("return"),
		 .Source = "return",
		 .ExpectedTick = before.Clock.Tick,
		 .ExpectedEpoch = before.WorldEpoch,
		 .ExpectedVersion = before.WorldVersion}
	);
	if (retireBound) {
		CHECK_FALSE(ran.Ran);
		CHECK_FALSE(ran.Atomic);
		CHECK(discarded == 0);
		worlds.Enter(live, [&](ecs::Store &store) {
			CHECK(store.Identity() == originalStore);
			CheckCadence(store, cadence);
		});
		return;
	}
	REQUIRE(ran.Ran);
	CHECK(discarded == 1);
	REQUIRE(worlds.Enter(worlds.Find(core::Name("package")), [&](ecs::Store &store, ecs::Scheduler &systems) {
		CheckCadence(store, cadence);
		CHECK(systems.SystemCount() == 2);
		CHECK(systems.Validate().Status == ecs::SystemScheduleStatus::Ready);
		systems.RunPhases(store, ecs::Phase::Simulation, ecs::Phase::Simulation);
	}) == world::WorldStatus::Ok);
	CHECK(calls == std::vector<int>{bound ? 1 : -3, 2});
	REQUIRE(
		session.Resume("package", session.Inspect("package").Clock.Tick).Status ==
		world::DataFactoryStatus::Ok
	);
	// A partial publication interval must survive along with the Store resources.
	const auto replacement = worlds.Find(core::Name("package"));
	for (int tick = 0; tick < 8; ++tick) {
		worlds.Tick(1.0f / 60.0f);
		cadenceReference.Tick(1.0f / 60.0f);
		CHECK(worlds.StatisticsOf(replacement).Ticks == cadenceReference.StatisticsOf(referenceWorld).Ticks);
		CHECK(
			worlds.StatisticsOf(replacement).ReplicationTicks ==
			cadenceReference.StatisticsOf(referenceWorld).ReplicationTicks
		);
	}
}
