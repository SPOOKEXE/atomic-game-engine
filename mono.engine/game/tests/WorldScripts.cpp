#include <engine/core/Name.hpp>
#include <engine/ecs/Attributes.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/game/Game.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/script/Clock.hpp>
#include <engine/script/Instances.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/script/SourceCache.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <limits>
#include <memory>
#include <string>
#include <string_view>

TEST_SUITE_ID("engine.game.worldscripts")
TEST_DEPENDS("engine.scripthost.sourcecache")
TEST_DEPENDS("engine.script.clock")

namespace world_scripts_test {
	using namespace engine;
	constexpr float WORLD_DELTA_SECONDS = 1.0f / 60.0f;

	std::string_view StartupSource(script::Language language) {
		return language == script::Language::Luau ? R"(
workspace:SetAttribute('TopRuns', (workspace:GetAttribute('TopRuns') or 0) + 1)
workspace:SetAttribute('StartedLanguage', 'Luau')
workspace.DescendantRemoving:Connect(function()
    workspace:SetAttribute('Removing', (workspace:GetAttribute('Removing') or 0) + 1)
end)
game:GetService('RunService').Heartbeat:Connect(function(delta)
    workspace:SetAttribute('Beats', (workspace:GetAttribute('Beats') or 0) + 1)
    workspace:SetAttribute('Elapsed', (workspace:GetAttribute('Elapsed') or 0) + delta)
    workspace:SetAttribute('LastDelta', delta)
end)
)"
												  : R"(
workspace.SetAttribute('TopRuns', (workspace.GetAttribute('TopRuns') || 0) + 1);
workspace.SetAttribute('StartedLanguage', 'JavaScript');
workspace.DescendantRemoving.Connect(function() {
    workspace.SetAttribute('Removing', (workspace.GetAttribute('Removing') || 0) + 1);
});
game.GetService('RunService').Heartbeat.Connect(function(delta) {
    workspace.SetAttribute('Beats', (workspace.GetAttribute('Beats') || 0) + 1);
    workspace.SetAttribute('Elapsed', (workspace.GetAttribute('Elapsed') || 0) + delta);
    workspace.SetAttribute('LastDelta', delta);
});
)";
	}
	std::string_view Extension(script::Language language) {
		return language == script::Language::Luau ? ".luau" : ".js";
	}
	script::Language OtherLanguage(script::Language language) {
		return language == script::Language::Luau ? script::Language::JavaScript : script::Language::Luau;
	}
	struct Fixture {
		ecs::Store Store{"world_script_startup"};
		ecs::Scheduler Scheduler{};
		ecs::Entity Workspace{};
		script::RuntimeLimits Limits{};
		std::string Error{};

		Fixture() {
			scene::RegisterSceneClasses();
			script::RegisterScriptComponents();
			scene::InstallServices(Store);
			Workspace = scene::WorkspaceOf(Store);
			REQUIRE(Workspace != ecs::NULL_ENTITY);
			Store.SetResource(script::SourceCache{});
			Limits.Role = script::HostRole::OfServer();
		}
		ecs::Entity AddScript(
			script::Language language, std::string_view name, std::string_view source, bool local = false
		) {
			const std::string path =
				"cached/world-scripts/" + std::string(name) + std::string(Extension(language));
			Store.ResourceMutable<script::SourceCache>()->Set(core::Name(path), std::string(source));
			const auto instance = script::MakeScript(Store, path, name, local);
			REQUIRE(instance != ecs::NULL_ENTITY);
			Store.SetParent(instance, Workspace);
			return instance;
		}
		std::shared_ptr<script::Runtime> Start(double rate = 0.0) {
			return game::StartWorldScripts(Store, Scheduler, Limits, Error, nullptr, rate);
		}
		bool Attribute(std::string_view name, ecs::AttributeValue &out) const {
			return ecs::GetAttribute(Store, Workspace, core::Name(name), out);
		}
		double Number(std::string_view name) const {
			ecs::AttributeValue value;
			REQUIRE(Attribute(name, value));
			switch (value.Type) {
			case ecs::PropertyType::Int32:
				return value.Int32;
			case ecs::PropertyType::Int64:
				return static_cast<double>(value.Int64);
			case ecs::PropertyType::Float:
				return value.Float;
			case ecs::PropertyType::Double:
				return value.Double;
			default:
				FAIL("script counter is not numeric");
				return 0;
			}
		}
		void Tick(size_t count) {
			for (size_t index = 0; index < count; ++index)
				Scheduler.Tick(Store, WORLD_DELTA_SECONDS);
		}
	};
}

TEST_CASE("restored world script startup preserves cadence", "[game][world-scripts][clock]") {
	using namespace world_scripts_test;
	for (const auto language : {script::Language::Luau, script::Language::JavaScript}) {
		Fixture fixture;
		fixture.AddScript(language, "Runner", StartupSource(language));
		fixture.Store.SetResource(
			script::ScriptClock{.Rate = 30.0, .Accumulator = 0.0125, .ObservedTick = 7}
		);
		auto runtime = game::StartWorldScripts(
			fixture.Store,
			fixture.Scheduler,
			fixture.Limits,
			fixture.Error,
			nullptr,
			30.0,
			game::ScriptClockStart::Preserve
		);
		REQUIRE(runtime != nullptr);
		CHECK(fixture.Error.empty());
		CHECK(fixture.Number("TopRuns") == 1);
		const auto *clock = fixture.Store.Resource<script::ScriptClock>();
		REQUIRE(clock != nullptr);
		CHECK(clock->Rate == 30.0);
		CHECK(clock->Accumulator == 0.0125);
		CHECK(clock->ObservedTick == 7);
	}
}

TEST_CASE("script startup initializes an absent restored clock", "[game][world-scripts][clock]") {
	using namespace world_scripts_test;
	Fixture fixture;
	REQUIRE(fixture.Store.Resource<script::ScriptClock>() == nullptr);
	auto runtime = game::StartWorldScripts(
		fixture.Store,
		fixture.Scheduler,
		fixture.Limits,
		fixture.Error,
		nullptr,
		30.0,
		game::ScriptClockStart::Preserve
	);
	REQUIRE(runtime != nullptr);
	const auto *clock = fixture.Store.Resource<script::ScriptClock>();
	REQUIRE(clock != nullptr);
	CHECK(clock->Rate == 30.0);
	CHECK(clock->Accumulator == 0.0);
}

TEST_CASE("ordinary script startup resets an existing clock", "[game][world-scripts][clock]") {
	using namespace world_scripts_test;
	Fixture fixture;
	fixture.Store.SetResource(script::ScriptClock{.Rate = 30.0, .Accumulator = 0.0125, .ObservedTick = 7});
	auto runtime = fixture.Start(60.0);
	REQUIRE(runtime != nullptr);
	const auto *clock = fixture.Store.Resource<script::ScriptClock>();
	REQUIRE(clock != nullptr);
	CHECK(clock->Rate == 60.0);
	CHECK(clock->Accumulator == 0.0);
	CHECK(clock->ObservedTick == std::numeric_limits<uint64_t>::max());
}

TEST_CASE(
	"world startup selects the authored VM and beats at the shared fixed script rate",
	"[game][scripts][language]"
) {
	using namespace world_scripts_test;
	for (const auto language : {script::Language::Luau, script::Language::JavaScript}) {
		for (const double rate : {0.0, 30.0, 120.0}) {
			INFO(static_cast<int>(language));
			INFO(rate);
			Fixture fixture;
			fixture.AddScript(language, "Runner", StartupSource(language));
			auto runtime = fixture.Start(rate);
			INFO(fixture.Error);
			REQUIRE(runtime != nullptr);
			REQUIRE(fixture.Error.empty());
			CHECK(runtime->Which() == language);
			CHECK(fixture.Scheduler.SystemCount() == 1);
			CHECK(fixture.Number("TopRuns") == 1);
			ecs::AttributeValue selected;
			REQUIRE(fixture.Attribute("StartedLanguage", selected));
			CHECK(selected.String == (language == script::Language::Luau ? "Luau" : "JavaScript"));
			REQUIRE(runtime->Costs().size() == 1);
			CHECK(runtime->Costs().front().Completed);
			// The scheduler owns the running VM after the caller releases its copy.
			const std::weak_ptr<script::Runtime> lifetime = runtime;
			runtime.reset();
			CHECK_FALSE(lifetime.expired());
			fixture.Tick(60);
			const double expectedBeats = rate == 0 ? 60 : rate;
			CHECK(fixture.Store.Time().Tick == 60);
			CHECK(fixture.Number("Beats") == expectedBeats);
			CHECK(fixture.Number("Elapsed") == Catch::Approx(1.0).margin(0.00001));
			CHECK(fixture.Number("LastDelta") == Catch::Approx(1.0 / expectedBeats));
			CHECK(fixture.Number("TopRuns") == 1);
			const auto observed = fixture.Store.CreateInstance(scene::PartClass(), "BeforeRuntimeRemoval");
			REQUIRE(observed != ecs::NULL_ENTITY);
			fixture.Store.SetParent(observed, fixture.Workspace);
			fixture.Store.DestroyInstance(observed);
			CHECK(fixture.Number("Removing") == 1);
			fixture.Scheduler = ecs::Scheduler{};
			CHECK(lifetime.expired());
			const auto transient = fixture.Store.CreateInstance(scene::PartClass(), "AfterRuntime");
			REQUIRE(transient != ecs::NULL_ENTITY);
			fixture.Store.SetParent(transient, fixture.Workspace);
			fixture.Store.DestroyInstance(transient);
			CHECK(fixture.Number("Removing") == 1);
		}
	}
}

TEST_CASE(
	"mixed world startup refuses both creation orders before any script or heartbeat runs",
	"[game][scripts][language]"
) {
	using namespace world_scripts_test;
	for (const auto first : {script::Language::Luau, script::Language::JavaScript}) {
		INFO(static_cast<int>(first));
		Fixture fixture;
		size_t sentinelTicks = 0;
		fixture.Scheduler.Add("existing-system", ecs::Phase::Simulation, [&](ecs::Store &) {
			++sentinelTicks;
		});
		fixture.AddScript(first, "First", StartupSource(first));
		fixture.AddScript(OtherLanguage(first), "Second", StartupSource(OtherLanguage(first)));
		const auto registered = fixture.Scheduler.SystemCount();
		const auto runtime = fixture.Start();
		CHECK(runtime == nullptr);
		CHECK(fixture.Error.find("mixed") != std::string::npos);
		CHECK(fixture.Error.find("First") != std::string::npos);
		CHECK(fixture.Error.find("Second") != std::string::npos);
		CHECK(fixture.Scheduler.SystemCount() == registered);
		ecs::AttributeValue value;
		CHECK_FALSE(fixture.Attribute("TopRuns", value));
		fixture.Tick(2);
		CHECK(sentinelTicks == 2);
		CHECK_FALSE(fixture.Attribute("TopRuns", value));
		CHECK_FALSE(fixture.Attribute("Beats", value));
	}
}

TEST_CASE(
	"inactive foreign sources and empty scripts do not select the world VM", "[game][scripts][language]"
) {
	using namespace world_scripts_test;
	enum class Inactive { Disabled, ClientOnly, Empty };
	for (const auto language : {script::Language::Luau, script::Language::JavaScript}) {
		for (const auto inactive : {Inactive::Disabled, Inactive::ClientOnly, Inactive::Empty}) {
			INFO(static_cast<int>(language));
			INFO(static_cast<int>(inactive));
			Fixture fixture;
			ecs::Entity ignored{};
			if (inactive == Inactive::Empty) {
				ignored = fixture.Store.CreateInstance(script::ScriptClass(), "NoSourceYet");
				REQUIRE(ignored != ecs::NULL_ENTITY);
				fixture.Store.SetParent(ignored, fixture.Workspace);
				REQUIRE_FALSE(script::ActiveSourceOf(fixture.Store, ignored).IsValid());
			} else {
				const auto foreign = OtherLanguage(language);
				ignored = fixture.AddScript(
					foreign, "InactiveForeign", StartupSource(foreign), inactive == Inactive::ClientOnly
				);
				if (inactive == Inactive::Disabled) fixture.Store.Set(ignored, script::Disabled{});
			}
			fixture.AddScript(language, "Active", StartupSource(language));
			const auto runtime = fixture.Start();
			INFO(fixture.Error);
			REQUIRE(runtime != nullptr);
			REQUIRE(fixture.Error.empty());
			CHECK(runtime->Which() == language);
			CHECK(fixture.Number("TopRuns") == 1);
			fixture.Tick(2);
			CHECK(fixture.Number("TopRuns") == 1);
			CHECK(fixture.Number("Beats") == 2);
			if (inactive != Inactive::Disabled) CHECK_FALSE(fixture.Store.Has<script::Disabled>(ignored));
		}
	}
}

TEST_CASE(
	"late foreign scripts fail by language while healthy scripts still start once",
	"[game][scripts][language]"
) {
	using namespace world_scripts_test;
	for (const auto language : {script::Language::Luau, script::Language::JavaScript}) {
		INFO(static_cast<int>(language));
		Fixture fixture;
		fixture.AddScript(language, "Initial", StartupSource(language));
		const auto runtime = fixture.Start();
		INFO(fixture.Error);
		REQUIRE(runtime != nullptr);
		REQUIRE(fixture.Error.empty());
		// This text is valid in the current VM. Only the authored language can refuse it.
		const auto foreignText = language == script::Language::Luau
									 ? "workspace:SetAttribute('ForeignRan', true)"
									 : "workspace.SetAttribute('ForeignRan', true);";
		const auto foreign = fixture.AddScript(OtherLanguage(language), "ForeignLate", foreignText);
		const auto healthyText =
			language == script::Language::Luau
				? "workspace:SetAttribute('HealthyRuns', (workspace:GetAttribute('HealthyRuns') or 0) + 1)"
				: "workspace.SetAttribute('HealthyRuns', (workspace.GetAttribute('HealthyRuns') || 0) + 1);";
		const auto healthy = fixture.AddScript(language, "HealthyLate", healthyText);
		const std::array wanted{foreign, healthy};
		CHECK(runtime->RunNewScripts(wanted) == 1);
		CHECK(runtime->LastError().find("language") != std::string::npos);
		CHECK(runtime->LastError().find("ForeignLate") != std::string::npos);
		CHECK(fixture.Store.Has<script::Disabled>(foreign));
		CHECK_FALSE(fixture.Store.Has<script::Disabled>(healthy));
		ecs::AttributeValue value;
		CHECK_FALSE(fixture.Attribute("ForeignRan", value));
		CHECK(fixture.Number("HealthyRuns") == 1);
		CHECK(runtime->RunNewScripts(wanted) == 0);
		CHECK(fixture.Number("HealthyRuns") == 1);
		fixture.Tick(2);
		CHECK(fixture.Number("Beats") == 2);
		CHECK_FALSE(fixture.Attribute("ForeignRan", value));
	}
}
