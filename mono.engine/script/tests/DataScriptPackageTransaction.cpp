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
TEST_DEPENDS("engine.world.datafactory")

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
