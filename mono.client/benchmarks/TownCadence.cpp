#include "CadenceMeasurements.hpp"

#include <engine/core/Paths.hpp>
#include <engine/ecs/Attributes.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/physics/Broadphase.hpp>
#include <engine/physics/Characters.hpp>
#include <engine/physics/Pipeline.hpp>
#include <engine/physics/Query.hpp>
#include <engine/render/Animation.hpp>
#include <engine/render/WorldPresentation.hpp>
#include <engine/scene/Animation.hpp>
#include <engine/scene/Attachments.hpp>
#include <engine/scene/Controls.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Skinning.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/script/UpdateCadenceService.hpp>
#include <engine/scripthost/Runtime.hpp>
#include <engine/testing/Bench.hpp>

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>

TEST_SUITE_ID("client.bench.update-cadence.town")

namespace cadence_town_bench {

	using namespace cadence_bench;
	using namespace engine;
	using script::Language;
	std::string ReadTown(Language language) {
		const auto filename = language == Language::Luau ? "TownCadence.luau" : "TownCadence.js";
		auto source = core::Paths::Assets() / "examples/scripts" / filename;
		if (!std::filesystem::is_regular_file(source))
			source = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() /
					 "mono.engine/examples/assets/scripts" / filename;
		std::ifstream input(source, std::ios::binary);
		Require(input.good(), "cadence could not read actual TownCadence example");
		return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
	}
	struct Citizen {
		Entity Model, Root, Humanoid, Animator, Track, Bone, Target, TargetHumanoid;
		Vector3 InitialPosition;
		uint64_t BeforeDeadline = 0, BeforePerception = 0, BeforeBehaviour = 0;
		float BeforeDistance = 0, BeforeHealth = 0;
		bool Fighting = false;
	};
	struct Town {
		Store Storage{"bench.cadence.town"};
		ecs::Scheduler Physics;
		std::shared_ptr<script::Runtime> Runtime;
		std::vector<Citizen> Citizens;
		BeamSet Beams;
		Entity Target, TargetHumanoid, Player;
		float BeforeHealth = 0;

		Town(Language language, bool full, const std::string &source, size_t population = 2)
			: Citizens(population) {
			Require(population == 2 || population == 64, "cadence town population must be 2 or 64");
			scene::RegisterSceneClasses();
			render::RegisterPresentationComponents();
			effects::RegisterEffectClasses();
			physics::PreparePhysicsWorld(Storage);
			Runtime = script::MakeRuntime(Storage, language);
			Require(Runtime != nullptr, "cadence town VM unavailable");
			if (population == 64) {
				ecs::AttributeValue value;
				value.Type = ecs::PropertyType::Int32;
				value.Int32 = 64;
				Require(
					ecs::SetAttribute(Storage, scene::WorkspaceOf(Storage), Name("CadencePopulation"), value),
					"cadence crowded town authoring refused"
				);
			}
			if (!Runtime->Run(source, "TownCadence")) throw std::runtime_error(Runtime->LastError());
			physics::RegisterPhysicsSystems(Physics);
			physics::RegisterCharacterSystems(Physics);
			const auto workspace = scene::WorkspaceOf(Storage);
			Target =
				Storage.FindFirstChild(workspace, population == 2 ? "Practice Target" : "Practice Target 1");
			TargetHumanoid = Storage.FindFirstChild(Target, "Humanoid");
			Require(
				Target != ecs::NULL_ENTITY && TargetHumanoid != ecs::NULL_ENTITY,
				"cadence town target missing"
			);
			auto *health = Storage.GetMutable<scene::Humanoid>(TargetHumanoid);
			health->MaxHealth = 1000000;
			health->Health = 1000000;
			Player = scene::AddPlayer(Storage, "Cadence town owner");
			Require(Player != ecs::NULL_ENTITY, "cadence town player missing");
			for (size_t index = 0; index < Citizens.size(); ++index) {
				auto &citizen = Citizens[index];
				const std::string name = population == 2 ? (index == 0 ? "North Guard" : "South Guard")
														 : "Citizen " + std::to_string(index + 1);
				citizen.Model = Storage.FindFirstChild(workspace, name);
				citizen.Root = Storage.FindFirstChild(citizen.Model, "Body");
				citizen.Humanoid = Storage.FindFirstChild(citizen.Model, "Humanoid");
				citizen.Animator = Storage.FindFirstChild(citizen.Root, "Animator");
				citizen.Track = Storage.FindFirstChild(citizen.Animator, "AnimationTrack");
				citizen.Bone = Storage.FindFirstChild(citizen.Root, "Root");
				citizen.Target =
					population == 2
						? Target
						: Storage.FindFirstChild(workspace, "Practice Target " + std::to_string(index + 1));
				citizen.TargetHumanoid = Storage.FindFirstChild(citizen.Target, "Humanoid");
				Require(
					citizen.Model != ecs::NULL_ENTITY && citizen.Root != ecs::NULL_ENTITY &&
						citizen.Humanoid != ecs::NULL_ENTITY && citizen.Animator != ecs::NULL_ENTITY &&
						citizen.Track != ecs::NULL_ENTITY && citizen.Bone != ecs::NULL_ENTITY &&
						citizen.Target != ecs::NULL_ENTITY && citizen.TargetHumanoid != ecs::NULL_ENTITY,
					"cadence actual town citizen hierarchy changed"
				);
				citizen.InitialPosition = Storage.Get<scene::Transform>(citizen.Root)->Frame.Position;
				const auto *authored = Storage.Get<scene::UpdateCadence>(citizen.Model);
				Require(
					authored != nullptr && authored->Identity.IsValid() && authored->Owner == citizen.Model,
					"cadence actual town owner policy missing"
				);
				constexpr std::array<scene::CadenceInterval, 4> expected{{{8, 12}, {4, 8}, {4, 8}, {4, 8}}};
				for (size_t domain = 0; domain < expected.size(); ++domain)
					Require(
						authored->Domains[domain].IntervalTicks == expected[domain].IntervalTicks &&
							authored->Domains[domain].MaximumStalenessTicks ==
								expected[domain].MaximumStalenessTicks,
						"cadence actual town authored intervals changed"
					);
				if (full) {
					auto policy = *authored;
					for (auto &domain : policy.Domains)
						domain = {1, 1};
					Storage.Set(citizen.Model, policy);
				}
				const size_t beforeBeams = Beams.Beams.size();
				Storage.EachDescendant(citizen.Model, [&](Entity entity) {
					const auto *beam = Storage.Get<effects::Beam>(entity);
					if (beam == nullptr) return;
					Require(
						beam->Enabled && beam->Attachment0 != ecs::NULL_ENTITY &&
							beam->Attachment1 != ecs::NULL_ENTITY && beam->Attachment0 != beam->Attachment1 &&
							Storage.Has<scene::Attachment>(beam->Attachment0) &&
							Storage.Has<scene::Attachment>(beam->Attachment1) &&
							!Storage.Has<scene::UpdateCadence>(entity),
						"cadence actual town beam source or policy changed"
					);
					Beams.Beams.push_back(entity);
					Beams.Ends.push_back(beam->Attachment1);
				});
				Require(
					Beams.Beams.size() == beforeBeams + 1,
					"cadence actual town needs one owned beam per citizen"
				);
				if (population == 64) {
					auto *targetHealth = Storage.GetMutable<scene::Humanoid>(citizen.TargetHumanoid);
					targetHealth->MaxHealth = 1000000;
					targetHealth->Health = 1000000;
				}
			}
			physics::SyncBroadphase(Storage);
		}
		void Author(Window window) {
			bool targetMoved = false;
			if (Citizens.size() == 2) {
				const auto north = Storage.Get<scene::Transform>(Citizens[0].Root)->Frame.Position;
				const auto south = Storage.Get<scene::Transform>(Citizens[1].Root)->Frame.Position;
				Vector3 target{0, 2.5f, -500};
				if (window == Window::SingleCombat) target = north + Vector3{-3, 0, -4};
				if (window == Window::CombatBurst) target = (north + south) * .5f + Vector3{0, 0, -5};
				targetMoved = Storage.Get<scene::Transform>(Target)->Frame.Position != target;
				if (targetMoved) Storage.GetMutable<scene::Transform>(Target)->Frame.Position = target;
			} else {
				for (size_t index = 0; index < Citizens.size(); ++index) {
					const auto &citizen = Citizens[index];
					Vector3 target = citizen.InitialPosition + Vector3{0, 0, -500};
					if (window == Window::CombatBurst || (window == Window::SingleCombat && index == 0))
						target =
							Storage.Get<scene::Transform>(citizen.Root)->Frame.Position + Vector3{0, 0, -5};
					if (Storage.Get<scene::Transform>(citizen.Target)->Frame.Position == target) continue;
					Storage.GetMutable<scene::Transform>(citizen.Target)->Frame.Position = target;
					targetMoved = true;
				}
			}
			// Publish the authored target move before the script's real Raycast.
			// The scheduler still performs its ordinary physics work inside the owner.
			if (targetMoved) physics::SyncBroadphase(Storage);
			for (size_t index = 0; index < Citizens.size(); ++index) {
				auto &citizen = Citizens[index];
				if (window == Window::PlayerOwned && index == 0)
					Storage.Set(citizen.Model, scene::NetworkOwner{Player});
				else if (Storage.Has<scene::NetworkOwner>(citizen.Model))
					Storage.Remove<scene::NetworkOwner>(citizen.Model);
				if (window == Window::Invalidation)
					++Storage.GetMutable<scene::UpdateCadence>(citizen.Model)->Revision;
				citizen.BeforeDeadline = AttackDeadline(Storage, citizen.Model);
				const auto *state = Storage.Get<scene::UpdateCadenceState>(citizen.Model);
				citizen.BeforePerception = state == nullptr ? 0 : state->Domains[0].Generation;
				citizen.BeforeBehaviour = state == nullptr ? 0 : state->Domains[1].Generation;
				citizen.BeforeDistance = (Storage.Get<scene::Transform>(citizen.Target)->Frame.Position -
										  Storage.Get<scene::Transform>(citizen.Root)->Frame.Position)
											 .Magnitude();
				citizen.BeforeHealth = Storage.Get<scene::Humanoid>(citizen.TargetHumanoid)->Health;
				citizen.Fighting = citizen.BeforeDistance <= 12 && citizen.BeforeHealth > 0;
				if (Citizens.size() == 64)
					Require(
						citizen.Fighting ==
							(window == Window::CombatBurst || (window == Window::SingleCombat && index == 0)),
						"cadence crowded town combat scope changed"
					);
			}
			BeforeHealth = Storage.Get<scene::Humanoid>(TargetHumanoid)->Health;
			if (window == Window::Invalidation)
				for (const Entity end : Beams.Ends)
					Storage.GetMutable<scene::Attachment>(end)->Frame.Position.Y += .5f;
		}
		void Step(Work &counts) {
			Storage.ClearChanges();
			Storage.AdvanceTick(DELTA);
			{
				ENGINE_PROFILE_CAT("cadence heartbeat", core::ProfileCategory::Script);
				if (!Runtime->Heartbeat(DELTA)) throw std::runtime_error(Runtime->LastError());
				Storage.FlushSignals();
			}
			{
				ENGINE_PROFILE_CAT("cadence physics", core::ProfileCategory::Physics);
				Physics.RunPhases(Storage, ecs::Phase::Input, ecs::Phase::Physics);
			}
			{
				ENGINE_PROFILE_CAT("cadence animation clock", core::ProfileCategory::Simulation);
				(void)scene::AdvanceAnimationTracks(Storage);
			}
			{
				ENGINE_PROFILE_CAT("cadence pose", core::ProfileCategory::Simulation);
				counts.PoseWrites += render::EvaluateAnimations(Storage);
				counts.BoneResolves += scene::ResolveBones(Storage);
				const auto *history = Storage.Resource<scene::PoseCadenceHistory>();
				Require(history != nullptr && history->Unavailable == 0, "cadence town clip unavailable");
				counts.PoseEvaluated += history->Evaluated;
				counts.PoseSkipped += history->Skipped;
				counts.Promotions += history->Promotions;
			}
			Beams.Build(Storage, counts);
		}
		void Verify(const Town &other, Window window, Work &counts) const {
			const uint64_t tick = Storage.Time().Tick;
			Require(tick == other.Storage.Time().Tick, "cadence VM tick parity failed");
			Require(
				Storage.Get<scene::Humanoid>(TargetHumanoid)->Health ==
					other.Storage.Get<scene::Humanoid>(other.TargetHumanoid)->Health,
				"cadence VM health parity failed"
			);
			const auto *poses = Storage.Resource<scene::PoseCadenceHistory>();
			uint64_t expectedDamage = 0;
			for (size_t index = 0; index < Citizens.size(); ++index) {
				const auto &citizen = Citizens[index];
				const auto &paired = other.Citizens[index];
				const auto *state = Storage.Get<scene::UpdateCadenceState>(citizen.Model);
				const auto *pairedState = other.Storage.Get<scene::UpdateCadenceState>(paired.Model);
				Require(state != nullptr && pairedState != nullptr, "cadence VM admission never committed");
				for (size_t domain = 0; domain < 2; ++domain) {
					const auto &current = state->Domains[domain];
					Require(
						current.Generation == pairedState->Domains[domain].Generation &&
							current.LastCommittedTick == pairedState->Domains[domain].LastCommittedTick,
						"cadence VM admission parity failed"
					);
					const auto age = tick - current.LastCommittedTick;
					counts.MaximumAge = std::max(counts.MaximumAge, age);
					Require(age <= (domain == 0 ? 12 : 8), "cadence town NPC stale beyond bound");
					if (citizen.Fighting || (window == Window::PlayerOwned && index == 0) ||
						window == Window::Invalidation)
						Require(age == 0, "cadence town critical NPC failed immediate refresh");
				}
				const auto perception = state->Domains[0].Generation - citizen.BeforePerception;
				const auto behaviour = state->Domains[1].Generation - citizen.BeforeBehaviour;
				Require(perception <= 1 && behaviour <= 1, "cadence town duplicated an admitted update");
				counts.Perception += perception;
				counts.Behaviour += behaviour;
				// The unchanged script calls Raycast once on a committed observation
				// beyond three metres. Report this provenance beside the VM rows.
				if (citizen.BeforeDistance > 3) counts.Rays += perception;
				const auto *observed = Storage.Get<script::NpcPerception>(citizen.Model);
				const auto *pairedObserved = other.Storage.Get<script::NpcPerception>(paired.Model);
				Require(
					observed->Visible == pairedObserved->Visible &&
						Near(observed->TargetPosition, pairedObserved->TargetPosition),
					"cadence VM perception parity failed"
				);
				if (perception != 0 && observed->Visible) ++counts.RayHits;
				const auto *decision = Storage.Get<script::NpcBehaviour>(citizen.Model);
				const auto *pairedDecision = other.Storage.Get<script::NpcBehaviour>(paired.Model);
				Require(
					Near(decision->MoveDirection, pairedDecision->MoveDirection) &&
						Storage.Get<scene::Humanoid>(citizen.Humanoid)->MoveDirection ==
							decision->MoveDirection,
					"cadence VM behaviour did not steer the real Humanoid"
				);
				const bool attack = citizen.Fighting && tick >= citizen.BeforeDeadline && observed->Visible &&
									state->Domains[0].LastCommittedTick == tick &&
									state->Domains[1].LastCommittedTick == tick;
				if (Citizens.size() == 64) {
					const float health = Storage.Get<scene::Humanoid>(citizen.TargetHumanoid)->Health;
					Require(
						health == other.Storage.Get<scene::Humanoid>(paired.TargetHumanoid)->Health &&
							citizen.BeforeHealth - health == (attack ? 1 : 0),
						"cadence crowded target health differs from VM or deadline oracle"
					);
				}
				const uint64_t expectedDeadline = attack ? tick + 30 : citizen.BeforeDeadline;
				Require(
					AttackDeadline(Storage, citizen.Model) == expectedDeadline &&
						AttackDeadline(other.Storage, paired.Model) == expectedDeadline,
					"cadence town missed an every-tick attack deadline"
				);
				++counts.Deadlines;
				expectedDamage += attack;
				const auto *track = Storage.Get<scene::AnimationTrack>(citizen.Track);
				const auto *pairedTrack = other.Storage.Get<scene::AnimationTrack>(paired.Track);
				Require(
					track->TimePosition == pairedTrack->TimePosition &&
						track->Weight == pairedTrack->Weight &&
						Near(track->TimePosition, static_cast<float>(tick) * DELTA, .002f),
					"cadence VM semantic animation clock paused"
				);
				const auto &pose = poses->Poses.at(citizen.Animator);
				const auto age = tick - pose.LastCommittedTick;
				counts.MaximumAge = std::max(counts.MaximumAge, age);
				Require(age <= 8, "cadence town pose stale beyond bound");
				if (citizen.Fighting || (window == Window::PlayerOwned && index == 0) ||
					window == Window::Invalidation)
					Require(age == 0, "cadence town critical visual pose failed refresh");
				Require(
					Near(
						Storage.Get<scene::Bone>(citizen.Bone)->Transform,
						other.Storage.Get<scene::Bone>(paired.Bone)->Transform
					),
					"cadence VM visual pose parity failed"
				);
				Require(
					Near(
						Storage.Get<scene::Transform>(citizen.Root)->Frame,
						other.Storage.Get<scene::Transform>(paired.Root)->Frame
					),
					"cadence VM real physics movement parity failed"
				);
			}
			if (Citizens.size() == 2)
				Require(
					BeforeHealth - Storage.Get<scene::Humanoid>(TargetHumanoid)->Health ==
						static_cast<float>(expectedDamage),
					"cadence town health differs from attack deadline oracle"
				);
			counts.Damage += expectedDamage;
			// The beam inherits its citizen's combat, ownership and invalidation policy.
			// Compare both VMs' current view and scroll.
			Beams.Verify(Storage, other.Beams, window, counts);
		}
	};

	template <Language LANGUAGE, bool COMPLEX, bool FULL, bool CROWDED = false>
	LatestSummary &RunBaseline(size_t calls) {
		constexpr size_t worlds = COMPLEX ? 32 : 1, population = CROWDED ? 64 : 2;
		constexpr Language paired = LANGUAGE == Language::Luau ? Language::JavaScript : Language::Luau;
		const auto source = ReadTown(LANGUAGE), pairedSource = ReadTown(paired);
		std::vector<std::unique_ptr<Town>> main, reference;
		for (size_t index = 0; index < worlds; ++index) {
			main.push_back(std::make_unique<Town>(LANGUAGE, FULL, source, population));
			reference.push_back(std::make_unique<Town>(paired, FULL, pairedSource, population));
		}
		const auto author = [&](Window window) {
			for (size_t index = 0; index < worlds; ++index) {
				main[index]->Author(window);
				reference[index]->Author(window);
			}
		};
		const auto step = [&](Work &counts) {
			for (auto &town : main)
				town->Step(counts);
		};
		const auto verify = [&](Window window, Work &counts) {
			for (size_t index = 0; index < worlds; ++index) {
				Work ignored;
				reference[index]->Step(ignored);
				main[index]->Verify(*reference[index], window, counts);
			}
		};
		for (size_t tick = 0; tick < WARM_TICKS; ++tick) {
			author(Window::Steady);
			Work counts;
			step(counts);
			verify(Window::Steady, counts);
		}
		const size_t ticks = TickCount();
		VmMemoryWindow vmMemory;
		for (const auto &town : main)
			vmMemory.Begin(*town->Runtime);
		Measurements measured(ticks);
		Work total;
		for (size_t tick = 0; tick < ticks; ++tick) {
			const auto window = WindowAt(tick, ticks);
			author(window);
			measured.Measure(tick, window, step);
			verify(window, measured.Ticks[tick].Counts);
			total.Add(measured.Ticks[tick].Counts);
		}
		vmMemory.End();
		Require(
			total.Rays > 0 && total.RayHits > 0 && total.Damage > 0 && total.PoseWrites > 0,
			"cadence town did no real ray, combat or animation work"
		);
		Require(
			FULL || (total.Perception < worlds * population * ticks && total.PoseSkipped > 0 &&
					 total.BeamSkipped > 0),
			"cadence town authored policy skipped no consumer work"
		);
		uint64_t retained = 0;
		for (const auto &town : main) {
			retained += town->Beams.Payload(town->Storage);
			retained += town->Storage.Resource<scene::PoseCadenceHistory>()->Poses.size() *
						sizeof(scene::DomainCadenceState);
			for (const auto &citizen : town->Citizens)
				Require(
					(town->Storage.Get<scene::Transform>(citizen.Root)->Frame.Position -
					 citizen.InitialPosition)
							.Magnitude() > 1,
					"cadence town Humanoid never physically moved"
				);
		}
		const std::string row = std::string(LANGUAGE == Language::Luau ? "town-luau-" : "town-js-") +
								(CROWDED   ? "crowded-"
								 : COMPLEX ? "complex-"
										   : "simple-") +
								(FULL ? "full" : "authored");
		auto &summary = BeginSummary(row, calls);

		summary.Append(
			"# cadence-clock row=%s final_tick=%llu track_time_seconds=%.9f next_attack_tick=%llu "
			"target_health=%.1f reference=opposite_VM_same_policy_outside_owner\n",
			row.c_str(),
			static_cast<unsigned long long>(main.front()->Storage.Time().Tick),
			main.front()
				->Storage.Get<scene::AnimationTrack>(main.front()->Citizens.front().Track)
				->TimePosition,
			static_cast<unsigned long long>(
				AttackDeadline(main.front()->Storage, main.front()->Citizens.front().Model)
			),
			main.front()->Storage.Get<scene::Humanoid>(main.front()->TargetHumanoid)->Health
		);

		summary.Append(
			"# cadence-town script=actual_authored_TownCadence paired_language_parity=1 "
			"ray_attempts_provenance=committed_perception_beyond_three_metres "
			"ray_hits_provenance=visible_target_commits vm_source_profiler=off\n"
		);
		if constexpr (CROWDED)
			summary.Append(
				"# cadence-crowd actors_per_world=64 runtime_count=1 shared_broadphase=1 "
				"individual_targets=64 target_health_scope=first_target "
				"deadline_oracle=each_individual_target\n"
			);
		measured.Summarize(
			summary,
			row.c_str(),
			calls,
			worlds * population,
			worlds,
			1,
			3,
			worlds * population,
			worlds * (CROWDED ? 68 : 5),
			FULL,
			retained
		);
		vmMemory.Summarize(summary, row.c_str());
		engine::testing::Consume(total.Rays);
		return summary;
	}

	template <Language LANGUAGE, bool COMPLEX, bool FULL, bool CROWDED = false>
	void RunPromotions(LatestSummary &summary, size_t calls) {
		constexpr size_t worlds = COMPLEX ? 32 : 1, population = CROWDED ? 64 : 2;
		constexpr Language paired = LANGUAGE == Language::Luau ? Language::JavaScript : Language::Luau;
		const auto source = ReadTown(LANGUAGE), pairedSource = ReadTown(paired);
		Measurements measured(PROMOTION_TICKS);
		std::array<size_t, static_cast<size_t>(Window::Count)> entries{};
		uint64_t retained = 0;
		for (size_t batch = 0; batch < PROMOTION_BATCHES; ++batch) {
			std::vector<std::unique_ptr<Town>> main, reference;
			main.reserve(worlds);
			reference.reserve(worlds);
			for (size_t index = 0; index < worlds; ++index) {
				main.push_back(std::make_unique<Town>(LANGUAGE, FULL, source, population));
				reference.push_back(std::make_unique<Town>(paired, FULL, pairedSource, population));
			}
			const auto author = [&](Window window) {
				for (size_t index = 0; index < worlds; ++index) {
					main[index]->Author(window);
					reference[index]->Author(window);
				}
			};
			const auto step = [&](Work &counts) {
				for (auto &town : main)
					town->Step(counts);
			};
			const auto verify = [&](Window window, Work &counts) {
				for (size_t index = 0; index < worlds; ++index) {
					Work ignored;
					reference[index]->Step(ignored);
					main[index]->Verify(*reference[index], window, counts);
				}
			};
			for (size_t tick = 0; tick < WARM_TICKS; ++tick) {
				author(Window::Steady);
				Work counts;
				step(counts);
				verify(Window::Steady, counts);
			}
			for (size_t tick = 0; tick < PROMOTION_BATCH_TICKS; ++tick) {
				const auto window = PromotionWindow(tick);
				if (PromotionEntry(tick))
					for (size_t index = 0; index < worlds; ++index) {
						VerifyPromotionSettled(main[index]->Storage);
						VerifyPromotionSettled(reference[index]->Storage);
					}
				author(window);
				if (PromotionEntry(tick))
					for (const auto &town : main)
						for (size_t index = 0; index < population; ++index)
							Require(
								town->Citizens[index].Fighting ==
									(window == Window::CombatBurst ||
									 (window == Window::SingleCombat && index == 0)),
								"cadence town entry did not produce its authored combat scope"
							);
				const size_t sample = batch * PROMOTION_BATCH_TICKS + tick;
				measured.Measure(sample, window, step);
				auto &counts = measured.Ticks[sample].Counts;
				verify(window, counts);
				if (window == Window::Demotion)
					for (size_t index = 0; index < worlds; ++index) {
						VerifyPromotionDemotion(main[index]->Storage);
						VerifyPromotionDemotion(reference[index]->Storage);
					}
				if (PromotionEntry(tick)) {
					++entries[static_cast<size_t>(window)];
					const size_t refreshed =
						worlds *
						(window == Window::CombatBurst || window == Window::Invalidation ? population : 1);
					Require(
						counts.Perception >= refreshed && counts.Behaviour >= refreshed &&
							counts.PoseEvaluated >= refreshed && counts.BeamRebuilt >= refreshed,
						"cadence town entry did not refresh its actual consumers"
					);
				}
			}
			uint64_t batchRetained = 0;
			for (const auto &town : main) {
				batchRetained += town->Beams.Payload(town->Storage);
				batchRetained += town->Storage.Resource<scene::PoseCadenceHistory>()->Poses.size() *
								 sizeof(scene::DomainCadenceState);
			}
			retained = std::max(retained, batchRetained);
		}
		for (const auto window :
			 {Window::SingleCombat, Window::CombatBurst, Window::PlayerOwned, Window::Invalidation})
			Require(entries[static_cast<size_t>(window)] == 128, "cadence town entry sample count changed");
		const std::string row = std::string(summary.Row.data()) + ".promotion";
		summary.Append(
			"# cadence-promotion row=%s batches=%zu rounds_per_batch=%zu settle_ticks_per_entry=%zu "
			"actual_entries_per_trigger=128 measured_entry_ticks=512 measured_settle_ticks=8192 "
			"maximum_fixture_lifetime_ticks=664 fixture_setup_warm_reference_excluded=1 "
			"retained_payload_basis=maximum_single_batch_final "
			"reference=opposite_VM_same_policy_outside_owner\n",
			row.c_str(),
			PROMOTION_BATCHES,
			PROMOTION_ROUNDS,
			PROMOTION_SETTLE_TICKS
		);
		measured.Summarize(
			summary,
			row.c_str(),
			calls,
			worlds * population,
			worlds,
			1,
			3,
			worlds * population,
			worlds * (CROWDED ? 68 : 5),
			FULL,
			retained,
			true,
			WARM_TICKS * PROMOTION_BATCHES
		);
		summary.Append(
			"# cadence-vm-memory row=%s vm_count_per_batch=%zu basis=unavailable "
			"window=promotion_cohort reason=sequential_VM_lifetimes_not_aggregated\n",
			row.c_str(),
			worlds
		);
	}

	template <Language LANGUAGE, bool COMPLEX, bool FULL, bool CROWDED = false> void Run() {
		static_assert(!COMPLEX || !CROWDED);
		const bool promotions = PromotionEnabled();
		static size_t calls = 0;
		auto &summary = RunBaseline<LANGUAGE, COMPLEX, FULL, CROWDED>(++calls);
		if (promotions) RunPromotions<LANGUAGE, COMPLEX, FULL, CROWDED>(summary, calls);
	}
}

BENCH("Luau town simple full fidelity, whole harness", 1) {
	cadence_town_bench::Run<engine::script::Language::Luau, false, true>();
}
BENCH("Luau town simple authored cadence, whole harness", 1) {
	cadence_town_bench::Run<engine::script::Language::Luau, false, false>();
}
BENCH("Luau town complex full fidelity, whole harness", 1) {
	cadence_town_bench::Run<engine::script::Language::Luau, true, true>();
}
BENCH("Luau town complex authored cadence, whole harness", 1) {
	cadence_town_bench::Run<engine::script::Language::Luau, true, false>();
}
BENCH("JS town simple full fidelity, whole harness", 1) {
	cadence_town_bench::Run<engine::script::Language::JavaScript, false, true>();
}
BENCH("JS town simple authored cadence, whole harness", 1) {
	cadence_town_bench::Run<engine::script::Language::JavaScript, false, false>();
}
BENCH("JS town complex full fidelity, whole harness", 1) {
	cadence_town_bench::Run<engine::script::Language::JavaScript, true, true>();
}
BENCH("JS town complex authored cadence, whole harness", 1) {
	cadence_town_bench::Run<engine::script::Language::JavaScript, true, false>();
}
BENCH("Luau crowded town full fidelity, whole harness", 1) {
	cadence_town_bench::Run<engine::script::Language::Luau, false, true, true>();
}
BENCH("Luau crowded town authored cadence, whole harness", 1) {
	cadence_town_bench::Run<engine::script::Language::Luau, false, false, true>();
}
BENCH("JS crowded town full fidelity, whole harness", 1) {
	cadence_town_bench::Run<engine::script::Language::JavaScript, false, true, true>();
}
BENCH("JS crowded town authored cadence, whole harness", 1) {
	cadence_town_bench::Run<engine::script::Language::JavaScript, false, false, true>();
}
