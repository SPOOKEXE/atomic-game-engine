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
