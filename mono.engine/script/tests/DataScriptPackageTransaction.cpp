#include <engine/assets/ContentHash.hpp>
#include <engine/core/Bytes.hpp>
#include <engine/script/DataScriptPackageTransaction.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/DataFactory.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_test_macros.hpp>

#include <span>
#include <stdexcept>

TEST_SUITE_ID("engine.script.data-script-package-transaction")
TEST_DEPENDS("engine.script.data-script-package")
TEST_DEPENDS("engine.world.data-factory")

namespace {
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
	SECTION("refusal") {}
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
		if (accepts) return true;
		detail = "host rejected package candidate";
		return false;
	});
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
		result.Error == (accepts   ? ""
						 : throws  ? "final candidate validation threw"
						 : removes ? "final candidate changed during validation"
								   : "host rejected package candidate")
	);
	CHECK(called == 1);
	CHECK(session.Inspect("package").WorldVersion == before.WorldVersion + (accepts ? 1 : 0));
	uint64_t afterIdentity = 0;
	worlds.Enter(live, [&](ecs::Store &store) { afterIdentity = store.Identity(); });
	CHECK((afterIdentity == liveIdentity) != accepts);
}
