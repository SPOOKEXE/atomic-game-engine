#include <engine/core/Paths.hpp>
#include <engine/ecs/Attributes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/physics/Pipeline.hpp>
#include <engine/scene/Controls.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/UpdateCadence.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/script/UpdateCadenceService.hpp>
#include <engine/scripthost/Runtime.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

TEST_SUITE_ID("engine.scripthost.updatecadenceservice")
TEST_DEPENDS("engine.script.updatecadenceservice")

using engine::ecs::Entity;
using engine::ecs::Store;
using engine::script::Language;

namespace {
	std::string ReadTown(Language language) {
		const auto filename = language == Language::Luau ? "TownCadence.luau" : "TownCadence.js";
		std::filesystem::path source = engine::core::Paths::Assets() / "examples/scripts" / filename;
		if (!std::filesystem::is_regular_file(source))
			source = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() /
					 "examples/assets/scripts" / filename;
		std::ifstream input(source, std::ios::binary);
		REQUIRE(input.good());
		return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
	}

	struct TownResult {
		uint64_t PerceptionGeneration = 0;
		uint64_t BehaviourGeneration = 0;
		bool Visible = false;
		engine::core::Vector3 MoveDirection;
		float TargetHealth = 0;
	};

	TownResult RunTown(Language language) {
		engine::scene::EnsureClassTree();
		engine::scene::RegisterSceneComponents();
		Store store("town_cadence_parity");
		engine::physics::PreparePhysicsWorld(store);
		const auto runtime = engine::script::MakeRuntime(store, language);
		REQUIRE(runtime != nullptr);
		const std::string source = ReadTown(language);
		const bool ran = runtime->Run(source, "TownCadence");
		INFO(runtime->LastError());
		REQUIRE(ran);

		engine::ecs::Scheduler scheduler;
		engine::physics::RegisterPhysicsSystems(scheduler);
		for (int tick = 0; tick < 20; ++tick) {
			scheduler.Tick(store, 1.0f / 60.0f);
			const bool heartbeat = runtime->Heartbeat(store.Time().Delta);
			INFO(runtime->LastError());
			REQUIRE(heartbeat);
			store.FlushSignals();
		}

		const Entity workspace = engine::scene::WorkspaceOf(store);
		REQUIRE(workspace != engine::ecs::NULL_ENTITY);
		const Entity guard = store.FindFirstChild(workspace, "North Guard");
		const Entity target = store.FindFirstChild(workspace, "Practice Target");
		REQUIRE(guard != engine::ecs::NULL_ENTITY);
		REQUIRE(target != engine::ecs::NULL_ENTITY);
		const auto *state = store.Get<engine::scene::UpdateCadenceState>(guard);
		const auto *perception = store.Get<engine::script::NpcPerception>(guard);
		const auto *behaviour = store.Get<engine::script::NpcBehaviour>(guard);
		const Entity targetHumanoid = store.FindFirstChild(target, "Humanoid");
		REQUIRE(state != nullptr);
		REQUIRE(perception != nullptr);
		REQUIRE(behaviour != nullptr);
		REQUIRE(targetHumanoid != engine::ecs::NULL_ENTITY);
		const auto *health = store.Get<engine::scene::Humanoid>(targetHumanoid);
		REQUIRE(health != nullptr);
		TownResult result;
		result.PerceptionGeneration =
			state->Domains[static_cast<size_t>(engine::scene::UpdateDomain::Perception)].Generation;
		result.BehaviourGeneration =
			state->Domains[static_cast<size_t>(engine::scene::UpdateDomain::Behaviour)].Generation;
		result.Visible = perception->Visible;
		result.MoveDirection = behaviour->MoveDirection;
		result.TargetHealth = health->Health;
		return result;
	}
}

TEST_CASE(
	"shared cadence calls reject stale results and refresh before combat in both VMs", "[scripting][cadence]"
) {
	for (const Language language : {Language::Luau, Language::JavaScript}) {
		engine::scene::EnsureClassTree();
		engine::scene::RegisterSceneComponents();
		Store store("cadence_service_methods");
		const auto runtime = engine::script::MakeRuntime(store, language);
		REQUIRE(runtime != nullptr);
		const char *source = language == Language::Luau ? R"(
			local cadence = game:GetService('UpdateCadenceService')
			local guard = Instance.new('Humanoid')
			guard.Name = 'Guard'
			guard.Parent = workspace
			assert(cadence:Configure(guard, 'town.guard.one', 8, 12, 4, 8, 3))
			local old = cadence:BeginUpdate(guard, 'perception', 1)
			assert(old.due and old.token > 0)
			assert(cadence:SetCombat(guard, true))
			assert(not cadence:CommitPerception(guard, old.token, 1, true, Vector3.new(1, 0, 0), true))
			assert(not cadence:CombatReady(guard, 1))
			local perception = cadence:BeginUpdate(guard, 'perception', 1)
			assert(perception.due)
			assert(cadence:CommitPerception(guard, perception.token, 1, true, Vector3.new(1, 0, 0), true))
			local behaviour = cadence:BeginUpdate(guard, 'behaviour', 1)
			assert(behaviour.due)
			assert(cadence:CommitBehaviour(guard, behaviour.token, 1, Vector3.new(1, 0, 0), true))
			assert(cadence:CombatReady(guard, 1))
			assert(cadence:Perception(guard).visible)
			assert(cadence:Behaviour(guard).X == 1)
			guard:SetAttribute('CadenceParity', 'ok')
		)"
														: R"(
			const cadence = game.GetService('UpdateCadenceService');
			const guard = Instance.new('Humanoid');
			guard.Name = 'Guard';
			guard.Parent = workspace;
			if (!cadence.Configure(guard, 'town.guard.one', 8, 12, 4, 8, 3)) throw new Error('configure');
			const old = cadence.BeginUpdate(guard, 'perception', 1);
			if (!old.due || old.token <= 0) throw new Error('admission');
			if (!cadence.SetCombat(guard, true)) throw new Error('combat');
			if (cadence.CommitPerception(guard, old.token, 1, true, Vector3.new(1, 0, 0), true)) throw new Error('stale');
			if (cadence.CombatReady(guard, 1)) throw new Error('premature combat');
			const perception = cadence.BeginUpdate(guard, 'perception', 1);
			if (!perception.due || !cadence.CommitPerception(guard, perception.token, 1, true,
			    Vector3.new(1, 0, 0), true)) throw new Error('perception');
			const behaviour = cadence.BeginUpdate(guard, 'behaviour', 1);
			if (!behaviour.due || !cadence.CommitBehaviour(guard, behaviour.token, 1,
			    Vector3.new(1, 0, 0), true)) throw new Error('behaviour');
			if (!cadence.CombatReady(guard, 1) || !cadence.Perception(guard).visible ||
			    cadence.Behaviour(guard).X !== 1) throw new Error('not ready');
			guard.SetAttribute('CadenceParity', 'ok');
		)";
		const bool ran = runtime->Run(source);
		INFO(runtime->LastError());
		REQUIRE(ran);
		const Entity workspace = engine::scene::WorkspaceOf(store);
		const Entity guard = store.FindFirstChild(workspace, "Guard");
		REQUIRE(guard != engine::ecs::NULL_ENTITY);
		engine::ecs::AttributeValue value;
		REQUIRE(engine::ecs::GetAttribute(store, guard, engine::core::Name("CadenceParity"), value));
		CHECK(value.String == "ok");
	}
}

TEST_CASE(
	"paired town scripts produce matching NPC cadence output in headless worlds", "[scripting][cadence]"
) {
	const TownResult luau = RunTown(Language::Luau);
	const TownResult javascript = RunTown(Language::JavaScript);
	CHECK(luau.PerceptionGeneration > 0);
	CHECK(luau.BehaviourGeneration > 0);
	CHECK(luau.PerceptionGeneration == javascript.PerceptionGeneration);
	CHECK(luau.BehaviourGeneration == javascript.BehaviourGeneration);
	CHECK(luau.Visible == javascript.Visible);
	CHECK(luau.MoveDirection == javascript.MoveDirection);
	CHECK(luau.TargetHealth == javascript.TargetHealth);
}
