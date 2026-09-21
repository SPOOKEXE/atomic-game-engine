#include <engine/assets/ContentHash.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Part.hpp>
#include <engine/script/DataScriptExecutor.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/DataFactory.hpp>

#include <catch2/catch_test_macros.hpp>

#include <span>
#include <stdexcept>
#include <string>

TEST_SUITE_ID("engine.script.data-script-package")

namespace {
	using engine::script::DataScriptPackageRunResult;
	using engine::script::DataScriptRequest;
	using engine::script::HostRole;
	using engine::script::Language;
	using engine::script::Runtime;
	using engine::script::RuntimeLimits;
	using engine::world::DataFactoryPauseScope;
	using engine::world::DataFactorySession;
	using engine::world::DataFactoryStatus;
	using engine::world::Universe;
	using engine::world::WorldId;
	using engine::world::WorldSettings;

	class TestRuntime final : public Runtime {
	  public:
		explicit TestRuntime(engine::ecs::Store &store)
			: Runtime(
				  store,
				  RuntimeLimits{
					  .DataCapture = {},
					  .DataLifecycle = {},
					  .MemoryBytes = 64u * 1024u * 1024u,
					  .StepBudget = 200u * 1000u * 1000u,
					  .JobBudget = 100u * 1000u,
					  .Role = HostRole::OfServer(),
					  .Origin = engine::script::ScriptOrigin::Game,
					  .Capabilities = engine::script::ScriptCapabilities::World,
					  .PackageOnly = false,
				  }
			  ) {}

		bool Run(std::string_view, std::string_view) override {
			Ran = true;
			return Succeeds;
		}
		bool RunInstance(engine::ecs::Entity) override {
			return false;
		}
		bool Heartbeat(float) override {
			return true;
		}
		Language Which() const override {
			return Language::Luau;
		}

		bool Succeeds = true;
		bool Ran = false;
	};

	DataScriptPackageRunResult RunToTerminal(
		Runtime &runtime,
		const engine::script::DataScriptPackageContext &,
		std::string_view source,
		std::string_view entry
	) {
		return DataScriptPackageRunResult{
			.Terminal = runtime.Run(source, entry) ? DataScriptPackageRunResult::State::Completed
												   : DataScriptPackageRunResult::State::Failed,
			.Error = {},
		};
	}

	std::string HashOf(std::string_view text) {
		return engine::assets::Hasher::Of(std::as_bytes(std::span(text.data(), text.size()))).ToHex();
	}

	std::string Manifest(std::string_view source, std::string_view assets = "[]") {
		return "{\"format\":\"atomic.data-script.v1\",\"entry\":\"scripts/main.luau\",\"source_hash\":\"" +
			   HashOf(source) + "\",\"assets\":" + std::string(assets) +
			   ",\"parameters\":[{\"name\":\"count\",\"type\":\"integer\",\"value\":4}],\"capabilities\":["
			   "\"world\"],\"budget\":{\"source_bytes\":1024,\"asset_bytes\":1024,\"assets\":4,"
			   "\"parameters\":4},\"seed\":7}";
	}

	struct Fixture {
		Universe Worlds;
		WorldId World;
		DataFactorySession Session;
		TestRuntime *Runtime = nullptr;

		Fixture() : Session(Worlds) {
			engine::scene::EnsureClassTree();
			WorldSettings settings;
			settings.Name = engine::core::Name("package-world");
			World = Worlds.Create(settings);
			Worlds.Enter(World, [this](engine::ecs::Store &store) { Runtime = new TestRuntime(store); });
			Session.SetRehydrate([](Universe &, WorldId, std::string &) { return true; });
			Session.SetPauseParticipant([](WorldId, DataFactoryPauseScope, bool, std::string &) {
				return true;
			});
		}

		~Fixture() {
			delete Runtime;
		}

		DataScriptRequest Request(std::string source) {
			const auto pause = Session.Pause("package-world", DataFactoryPauseScope::AllSystems, 0);
			REQUIRE(pause.Status == DataFactoryStatus::Ok);
			return DataScriptRequest{
				.InstanceId = "package-world",
				.Manifest = Manifest(source),
				.Source = std::move(source),
				.Assets = {},
				.SourceHash = {},
				.Name = "mcp",
				.ExpectedTick = pause.Clock.Tick,
				.ExpectedEpoch = pause.WorldEpoch,
				.ExpectedVersion = pause.WorldVersion,
			};
		}

		engine::core::Name NameOf(engine::ecs::Entity entity) {
			engine::core::Name name;
			Worlds.Enter(Worlds.Find(engine::core::Name("package-world")), [&](engine::ecs::Store &store) {
				name = store.InstanceNameOf(entity);
			});
			return name;
		}

		engine::ecs::Entity AddEntity(std::string_view name) {
			engine::ecs::Entity entity = engine::ecs::NULL_ENTITY;
			Worlds.Enter(Worlds.Find(engine::core::Name("package-world")), [&](engine::ecs::Store &store) {
				entity = store.CreateInstance(engine::scene::PartClass(), name);
			});
			REQUIRE(entity != engine::ecs::NULL_ENTITY);
			return entity;
		}
	};
}

TEST_CASE("data-script package parses bounded typed inputs", "[script][data-script-package]") {
	const std::string source = "return 4";
	const auto parsed = engine::script::ParseDataScriptPackage(Manifest(source));
	REQUIRE(parsed);
	CHECK(parsed.Package->Entry == "scripts/main.luau");
	CHECK(parsed.Package->Parameters.size() == 1);
	CHECK(parsed.Package->Parameters.front().Value.Integer == 4);
	CHECK(
		engine::script::DataScriptSeedStream(7, "terrain") ==
		engine::script::DataScriptSeedStream(7, "terrain")
	);
	CHECK(
		engine::script::DataScriptSeedStream(7, "terrain") != engine::script::DataScriptSeedStream(7, "props")
	);
}

TEST_CASE("runtime discard proof is sticky after retained host work", "[script][data-script-package]") {
	engine::ecs::Store store("discard-proof");
	TestRuntime runtime(store);
	CHECK(runtime.CanDiscardForWorldSwap());
	runtime.DeliverSettingsMenuAction(engine::core::Name("open-settings"));
	CHECK_FALSE(runtime.CanDiscardForWorldSwap());
}

TEST_CASE(
	"data-script package refuses traversal, untyped values, and unknown fields",
	"[script][data-script-package]"
) {
	const std::string source = "return 4";
	std::string badPath = Manifest(source);
	badPath.replace(
		badPath.find("scripts/main.luau"), std::string("scripts/main.luau").size(), "../main.luau"
	);
	CHECK_FALSE(engine::script::ParseDataScriptPackage(badPath));

	std::string untyped = Manifest(source);
	untyped.replace(untyped.find("\"integer\""), std::string("\"integer\"").size(), "\"boolean\"");
	CHECK_FALSE(engine::script::ParseDataScriptPackage(untyped));

	std::string unknown = Manifest(source);
	unknown.insert(unknown.size() - 1, ",\"future\":true");
	CHECK_FALSE(engine::script::ParseDataScriptPackage(unknown));
}

TEST_CASE("data-script package decodes JSON unicode escapes strictly", "[script][data-script-package]") {
	const std::string source = "return 4";
	std::string escaped = Manifest(source);
	escaped.replace(
		escaped.find("scripts/main.luau"), std::string("scripts/main.luau").size(), "scripts/m\\u0061in.luau"
	);
	const auto parsed = engine::script::ParseDataScriptPackage(escaped);
	REQUIRE(parsed);
	CHECK(parsed.Package->Entry == "scripts/main.luau");

	std::string unpaired = Manifest(source);
	unpaired.replace(
		unpaired.find("scripts/main.luau"), std::string("scripts/main.luau").size(), "scripts/\\uD800.luau"
	);
	CHECK_FALSE(engine::script::ParseDataScriptPackage(unpaired));

	std::string nulName = Manifest(source);
	nulName.replace(nulName.find("count"), 5, "count\\u0000tail");
	CHECK_FALSE(engine::script::ParseDataScriptPackage(nulName));
}

TEST_CASE("data-script execution checkpoints before invoking its runtime", "[script][data-script-package]") {
	Fixture fixture;
	DataScriptRequest request = fixture.Request("return 4");
	const auto result = engine::script::ExecuteDataScript(
		fixture.Worlds,
		fixture.Session,
		request,
		[&fixture](WorldId, engine::script::ScriptCapabilities) { return fixture.Runtime; },
		RunToTerminal
	);
	CHECK(result.Ran);
	CHECK(result.Atomic);
	CHECK(result.Error.empty());
	CHECK(fixture.Runtime->Ran);
	CHECK(result.Lifecycle.WorldVersion == request.ExpectedVersion + 1);
}

TEST_CASE(
	"data-script execution refuses before runtime work without rollback support",
	"[script][data-script-package]"
) {
	Fixture fixture;
	fixture.Session.SetRehydrate({});
	DataScriptRequest request = fixture.Request("return 4");
	const auto result = engine::script::ExecuteDataScript(
		fixture.Worlds,
		fixture.Session,
		request,
		[&fixture](WorldId, engine::script::ScriptCapabilities) { return fixture.Runtime; },
		RunToTerminal
	);
	CHECK_FALSE(result.Ran);
	CHECK_FALSE(result.Atomic);
	CHECK_FALSE(fixture.Runtime->Ran);
	CHECK(result.Lifecycle.Status == DataFactoryStatus::Unsupported);
}

TEST_CASE(
	"data-script execution restores a failed runtime through its checkpoint", "[script][data-script-package]"
) {
	Fixture fixture;
	DataScriptRequest request = fixture.Request("return 4");
	engine::ecs::Entity mutation = engine::ecs::NULL_ENTITY;
	const auto result = engine::script::ExecuteDataScript(
		fixture.Worlds,
		fixture.Session,
		request,
		[&fixture](WorldId, engine::script::ScriptCapabilities) { return fixture.Runtime; },
		[&fixture, &mutation](
			Runtime &, const engine::script::DataScriptPackageContext &, std::string_view, std::string_view
		) {
			mutation = fixture.AddEntity("rollback-created");
			return DataScriptPackageRunResult{
				.Terminal = DataScriptPackageRunResult::State::Failed, .Error = "deliberate runtime failure"
			};
		}
	);
	CHECK_FALSE(result.Ran);
	CHECK(result.Atomic);
	CHECK(result.Lifecycle.Status == DataFactoryStatus::Ok);
	CHECK(result.Lifecycle.WorldEpoch > request.ExpectedEpoch);
	CHECK(fixture.NameOf(mutation).Text() != "rollback-created");
}

TEST_CASE("data-script execution rolls back a throwing executor", "[script][data-script-package]") {
	Fixture fixture;
	DataScriptRequest request = fixture.Request("return 4");
	engine::ecs::Entity mutation = engine::ecs::NULL_ENTITY;
	const auto result = engine::script::ExecuteDataScript(
		fixture.Worlds,
		fixture.Session,
		request,
		[&fixture](WorldId, engine::script::ScriptCapabilities) { return fixture.Runtime; },
		[&fixture, &mutation](
			Runtime &, const engine::script::DataScriptPackageContext &, std::string_view, std::string_view
		) {
			mutation = fixture.AddEntity("throwing-created");
			throw std::runtime_error("deliberate executor failure");
			return DataScriptPackageRunResult{};
		}
	);
	CHECK_FALSE(result.Ran);
	CHECK(result.Atomic);
	CHECK(result.Lifecycle.Status == DataFactoryStatus::Ok);
	CHECK(result.Error.find("deliberate executor failure") != std::string::npos);
	CHECK(fixture.NameOf(mutation).Text() != "throwing-created");
}

TEST_CASE("data-script execution reports failed rollback as non-atomic", "[script][data-script-package]") {
	Fixture fixture;
	fixture.Session.SetRehydrate([](Universe &, WorldId, std::string &detail) {
		detail = "deliberate restore failure";
		return false;
	});
	DataScriptRequest request = fixture.Request("return 4");
	engine::ecs::Entity mutation = engine::ecs::NULL_ENTITY;
	const auto result = engine::script::ExecuteDataScript(
		fixture.Worlds,
		fixture.Session,
		request,
		[&fixture](WorldId, engine::script::ScriptCapabilities) { return fixture.Runtime; },
		[&fixture, &mutation](
			Runtime &, const engine::script::DataScriptPackageContext &, std::string_view, std::string_view
		) {
			mutation = fixture.AddEntity("restore-incomplete-created");
			return DataScriptPackageRunResult{
				.Terminal = DataScriptPackageRunResult::State::Failed, .Error = "fail"
			};
		}
	);
	CHECK_FALSE(result.Ran);
	CHECK_FALSE(result.Atomic);
	CHECK(result.Lifecycle.Status == DataFactoryStatus::RestoreIncomplete);
	CHECK(fixture.NameOf(mutation).Text() == "restore-incomplete-created");
}

TEST_CASE("data-script execution refuses a stale lifecycle revision", "[script][data-script-package]") {
	Fixture fixture;
	DataScriptRequest request = fixture.Request("return 4");
	request.ExpectedVersion++;
	const auto result = engine::script::ExecuteDataScript(
		fixture.Worlds, fixture.Session, request, [&fixture](WorldId, engine::script::ScriptCapabilities) {
			return fixture.Runtime;
		}
	);
	CHECK_FALSE(result.Ran);
	CHECK_FALSE(result.Atomic);
	CHECK(result.Error == "expected lifecycle revision does not match");
}
