#include <engine/core/Paths.hpp>
#include <engine/ecs/Attributes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/physics/Broadphase.hpp>
#include <engine/physics/Characters.hpp>
#include <engine/physics/Pipeline.hpp>
#include <engine/physics/Query.hpp>
#include <engine/scene/Animation.hpp>
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

#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unordered_set>
#include <vector>

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

	constexpr size_t CROWD_POPULATION = 64;
	constexpr float CROWD_DELTA_SECONDS = 1.0f / 60.0f;
	enum class CrowdWindow { Safe, SingleCombat, CombatBurst, PlayerOwned, Invalidation };
	struct CrowdSample {
		std::array<uint64_t, 2> Generations{};
		std::array<uint64_t, 2> CommittedTicks{};
		engine::core::Vector3 Position{};
		engine::core::Vector3 TargetPosition{};
		engine::core::Vector3 MoveDirection{};
		uint64_t AttackDeadline = 0;
		float TargetHealth = 0;
		float AnimationSeconds = 0;
		bool Visible = false;
	};
	bool CrowdNear(engine::core::Vector3 left, engine::core::Vector3 right) {
		return (left - right).Magnitude() < 0.0001f;
	}
	uint64_t CrowdAttackDeadline(const Store &store, Entity actor) {
		engine::ecs::AttributeValue attribute;
		REQUIRE(engine::ecs::GetAttribute(store, actor, engine::core::Name("NextAttackTick"), attribute));
		double deadline = -1;
		switch (attribute.Type) {
		case engine::ecs::PropertyType::Int32:
			deadline = attribute.Int32;
			break;
		case engine::ecs::PropertyType::Int64:
			deadline = static_cast<double>(attribute.Int64);
			break;
		case engine::ecs::PropertyType::Float:
			deadline = attribute.Float;
			break;
		case engine::ecs::PropertyType::Double:
			deadline = attribute.Double;
			break;
		default:
			break;
		}
		REQUIRE(std::isfinite(deadline));
		REQUIRE(deadline >= 0);
		REQUIRE(deadline < 1000000);
		REQUIRE(std::floor(deadline) == deadline);
		return static_cast<uint64_t>(deadline);
	}

	std::vector<CrowdSample> RunCrowdTown(Language language) {
		using namespace engine;
		scene::RegisterSceneClasses();
		Store store("town_cadence_same_world_crowd");
		physics::PreparePhysicsWorld(store);
		const auto runtime = script::MakeRuntime(store, language);
		REQUIRE(runtime != nullptr);
		const Entity workspace = scene::WorkspaceOf(store);
		REQUIRE(workspace != ecs::NULL_ENTITY);
		ecs::AttributeValue population;
		population.Type = ecs::PropertyType::Int32;
		population.Int32 = static_cast<int32_t>(CROWD_POPULATION);
		REQUIRE(ecs::SetAttribute(store, workspace, core::Name("CadencePopulation"), population));
		const bool ran = runtime->Run(ReadTown(language), "TownCadenceCrowd");
		INFO(runtime->LastError());
		REQUIRE(ran);

		struct Citizen {
			Entity Model{}, Root{}, Humanoid{}, Target{}, TargetHumanoid{}, Track{};
			core::Vector3 InitialPosition{};
		};
		std::array<Citizen, CROWD_POPULATION> citizens{};
		std::unordered_set<std::string> identities;
		std::unordered_set<std::string> targetIds;
		size_t models = 0;
		store.EachChild(workspace, [&](Entity entity) {
			if (store.IsA(entity, ecs::Classes::Find(core::Name("Model")))) ++models;
		});
		REQUIRE(models == CROWD_POPULATION);
		for (size_t index = 0; index < citizens.size(); ++index) {
			INFO(index);
			auto &citizen = citizens[index];
			const auto number = std::to_string(index + 1);
			citizen.Model = store.FindFirstChild(workspace, "Citizen " + number);
			REQUIRE(citizen.Model != ecs::NULL_ENTITY);
			citizen.Root = store.FindFirstChild(citizen.Model, "Body");
			citizen.Humanoid = store.FindFirstChild(citizen.Model, "Humanoid");
			citizen.Target = store.FindFirstChild(workspace, "Practice Target " + number);
			citizen.TargetHumanoid = store.FindFirstChild(citizen.Target, "Humanoid");
			const auto animator = store.FindFirstChild(citizen.Root, "Animator");
			citizen.Track = store.FindFirstChild(animator, "AnimationTrack");
			REQUIRE(citizen.Root != ecs::NULL_ENTITY);
			REQUIRE(citizen.Humanoid != ecs::NULL_ENTITY);
			REQUIRE(citizen.Target != ecs::NULL_ENTITY);
			REQUIRE(citizen.TargetHumanoid != ecs::NULL_ENTITY);
			REQUIRE(animator != ecs::NULL_ENTITY);
			REQUIRE(citizen.Track != ecs::NULL_ENTITY);
			REQUIRE(store.FindFirstChild(citizen.Root, "Root") != ecs::NULL_ENTITY);
			const auto *policy = store.Get<scene::UpdateCadence>(citizen.Model);
			REQUIRE(policy != nullptr);
			REQUIRE(policy->Identity.IsValid());
			CHECK(identities.insert(std::string(policy->Identity.Text())).second);
			ecs::AttributeValue targetId;
			REQUIRE(ecs::GetAttribute(store, citizen.Target, core::Name("DataFactoryId"), targetId));
			REQUIRE(targetId.Type == ecs::PropertyType::String);
			CHECK(targetId.String == "town.practice-target." + number);
			CHECK(targetIds.insert(targetId.String).second);
			const auto *root = store.Get<scene::Transform>(citizen.Root);
			REQUIRE(root != nullptr);
			citizen.InitialPosition = root->Frame.Position;
			CHECK(CrowdNear(citizen.InitialPosition, {(static_cast<float>(index) - 31.5f) * 4, 2.5f, 8}));
			const auto *target = store.Get<scene::Transform>(citizen.Target);
			REQUIRE(target != nullptr);
			CHECK(CrowdNear(target->Frame.Position, {citizen.InitialPosition.X, 2.5f, -492}));
		}
		CHECK(identities.size() == CROWD_POPULATION);
		CHECK(targetIds.size() == CROWD_POPULATION);
		const auto player = scene::AddPlayer(store, "Crowd cadence owner");
		REQUIRE(player != ecs::NULL_ENTITY);
		ecs::Scheduler scheduler;
		physics::RegisterPhysicsSystems(scheduler);
		physics::RegisterCharacterSystems(scheduler);

		struct BeforeTick {
			std::array<uint64_t, 2> Generations{};
			core::Vector3 Difference{};
			core::Vector3 TargetPosition{};
			uint64_t Deadline = 0;
			float Health = 0;
			bool Fighting = false;
			bool Visible = false;
		};
		std::array<BeforeTick, CROWD_POPULATION> before{};
		const std::array<std::pair<CrowdWindow, size_t>, 9> windows{{
			{CrowdWindow::Safe, 16},
			{CrowdWindow::SingleCombat, 32},
			{CrowdWindow::Safe, 16},
			{CrowdWindow::CombatBurst, 32},
			{CrowdWindow::Safe, 16},
			{CrowdWindow::PlayerOwned, 4},
			{CrowdWindow::Safe, 16},
			{CrowdWindow::Invalidation, 1},
			{CrowdWindow::Safe, 16},
		}};
		std::vector<CrowdSample> samples;
		size_t totalTicks = 0;
		for (const auto &[window, ticks] : windows) {
			(void)window;
			totalTicks += ticks;
		}
		samples.reserve(totalTicks * CROWD_POPULATION);
		uint64_t skippedPerceptions = 0, skippedBehaviours = 0, singleAttacks = 0, burstAttacks = 0;
		for (const auto &[window, ticks] : windows) {
			for (size_t step = 0; step < ticks; ++step) {
				store.ClearChanges();
				for (size_t index = 0; index < citizens.size(); ++index) {
					const auto &citizen = citizens[index];
					auto &previous = before[index];
					const auto position = store.Get<scene::Transform>(citizen.Root)->Frame.Position;
					const bool near = window == CrowdWindow::CombatBurst ||
									  (window == CrowdWindow::SingleCombat && index == 0);
					const core::Vector3 targetPosition =
						near ? position + core::Vector3{0, 0, -5}
							 : core::Vector3{citizen.InitialPosition.X, 2.5f, -492};
					store.GetMutable<scene::Transform>(citizen.Target)->Frame.Position = targetPosition;
					if (window == CrowdWindow::PlayerOwned && index == 0)
						store.Set(citizen.Model, scene::NetworkOwner{player});
					else if (store.Has<scene::NetworkOwner>(citizen.Model))
						store.Remove<scene::NetworkOwner>(citizen.Model);
					if (window == CrowdWindow::Invalidation)
						++store.GetMutable<scene::UpdateCadence>(citizen.Model)->Revision;
					previous.Difference = targetPosition - position;
					previous.TargetPosition = targetPosition;
					previous.Health = store.Get<scene::Humanoid>(citizen.TargetHumanoid)->Health;
					previous.Deadline = CrowdAttackDeadline(store, citizen.Model);
					previous.Fighting = previous.Difference.Magnitude() <= 12 && previous.Health > 0;
					const auto *state = store.Get<scene::UpdateCadenceState>(citizen.Model);
					for (size_t domain = 0; domain < 2; ++domain)
						previous.Generations[domain] = state ? state->Domains[domain].Generation : 0;
				}
				physics::SyncBroadphase(store);
				for (size_t index = 0; index < citizens.size(); ++index) {
					auto &previous = before[index];
					const auto direction = previous.Difference.Unit();
					const auto position = store.Get<scene::Transform>(citizens[index].Root)->Frame.Position;
					const auto hit = physics::Raycast(
						store, core::Ray{position + direction * 2, direction}, previous.Difference.Magnitude()
					);
					previous.Visible = hit && hit->Owner == citizens[index].Target;
					REQUIRE(previous.Visible);
				}
				// Heartbeat owns one fixed tick; physics consumes the movement it just committed.
				store.AdvanceTick(CROWD_DELTA_SECONDS);
				const bool heartbeat = runtime->Heartbeat(CROWD_DELTA_SECONDS);
				INFO(runtime->LastError());
				REQUIRE(heartbeat);
				store.FlushSignals();
				scheduler.RunPhases(store, ecs::Phase::Input, ecs::Phase::Physics);
				scene::AdvanceAnimationTracks(store);
				const auto tick = store.Time().Tick;
				for (size_t index = 0; index < citizens.size(); ++index) {
					INFO(index);
					INFO(tick);
					const auto &citizen = citizens[index];
					const auto &previous = before[index];
					const auto *policy = store.Get<scene::UpdateCadence>(citizen.Model);
					const auto *state = store.Get<scene::UpdateCadenceState>(citizen.Model);
					const auto *observed = store.Get<script::NpcPerception>(citizen.Model);
					const auto *decision = store.Get<script::NpcBehaviour>(citizen.Model);
					REQUIRE(policy != nullptr);
					REQUIRE(state != nullptr);
					REQUIRE(observed != nullptr);
					REQUIRE(decision != nullptr);
					const bool critical = previous.Fighting || window == CrowdWindow::Invalidation ||
										  (window == CrowdWindow::PlayerOwned && index == 0);
					CrowdSample sample;
					for (size_t domain = 0; domain < 2; ++domain) {
						const auto &current = state->Domains[domain];
						REQUIRE(current.Generation > 0);
						CHECK(current.Generation - previous.Generations[domain] <= 1);
						CHECK(current.LastCommittedTick <= tick);
						CHECK(
							tick - current.LastCommittedTick <= policy->Domains[domain].MaximumStalenessTicks
						);
						if (critical) {
							CHECK(current.LastCommittedTick == tick);
							CHECK(current.PolicyRevision == policy->Revision);
							CHECK(current.SourceRevision == 1);
						}
						sample.Generations[domain] = current.Generation;
						sample.CommittedTicks[domain] = current.LastCommittedTick;
					}
					if (sample.Generations[0] == previous.Generations[0])
						++skippedPerceptions;
					else {
						CHECK(observed->Visible == previous.Visible);
						CHECK(CrowdNear(observed->TargetPosition, previous.TargetPosition));
					}
					if (sample.Generations[1] == previous.Generations[1])
						++skippedBehaviours;
					else {
						const auto expected = observed->Visible && previous.Difference.Magnitude() > 5
												  ? previous.Difference.Unit()
												  : core::Vector3{};
						CHECK(CrowdNear(decision->MoveDirection, expected));
					}
					CHECK(CrowdNear(
						store.Get<scene::Humanoid>(citizen.Humanoid)->MoveDirection, decision->MoveDirection
					));
					if (previous.Fighting) CHECK(script::NpcCombatReady(store, citizen.Model, 1));
					const bool attack = previous.Fighting && tick >= previous.Deadline;
					sample.TargetHealth = store.Get<scene::Humanoid>(citizen.TargetHumanoid)->Health;
					sample.AttackDeadline = CrowdAttackDeadline(store, citizen.Model);
					CHECK(sample.TargetHealth == previous.Health - (attack ? 1.0f : 0.0f));
					CHECK(sample.AttackDeadline == (attack ? tick + 30 : previous.Deadline));
					if (attack && window == CrowdWindow::SingleCombat) ++singleAttacks;
					if (attack && window == CrowdWindow::CombatBurst) ++burstAttacks;
					sample.Position = store.Get<scene::Transform>(citizen.Root)->Frame.Position;
					CHECK(std::isfinite(sample.Position.Y));
					CHECK(sample.Position.Y > 0);
					sample.TargetPosition = observed->TargetPosition;
					sample.MoveDirection = decision->MoveDirection;
					sample.Visible = observed->Visible;
					sample.AnimationSeconds = store.Get<scene::AnimationTrack>(citizen.Track)->TimePosition;
					CHECK(
						std::abs(sample.AnimationSeconds - static_cast<float>(tick) * CROWD_DELTA_SECONDS) <
						0.002f
					);
					samples.push_back(sample);
				}
			}
		}
		CHECK(skippedPerceptions > 0);
		CHECK(skippedBehaviours > 0);
		CHECK(singleAttacks == 2);
		CHECK(burstAttacks >= CROWD_POPULATION);
		for (const auto &citizen : citizens)
			CHECK(
				store.Get<scene::Transform>(citizen.Root)->Frame.Position.Z <
				citizen.InitialPosition.Z - 0.25f
			);
		return samples;
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

TEST_CASE(
	"paired town crowds preserve fresh combat and physics across cadence promotions",
	"[scripting][cadence][crowded]"
) {
	const auto luau = RunCrowdTown(Language::Luau);
	const auto javascript = RunCrowdTown(Language::JavaScript);
	REQUIRE(luau.size() == javascript.size());
	for (size_t index = 0; index < luau.size(); ++index) {
		INFO(index);
		const auto &left = luau[index];
		const auto &right = javascript[index];
		CHECK(left.Generations == right.Generations);
		CHECK(left.CommittedTicks == right.CommittedTicks);
		CHECK(left.Visible == right.Visible);
		CHECK(CrowdNear(left.Position, right.Position));
		CHECK(CrowdNear(left.TargetPosition, right.TargetPosition));
		CHECK(CrowdNear(left.MoveDirection, right.MoveDirection));
		CHECK(left.TargetHealth == right.TargetHealth);
		CHECK(left.AttackDeadline == right.AttackDeadline);
		CHECK(left.AnimationSeconds == right.AnimationSeconds);
	}
}
