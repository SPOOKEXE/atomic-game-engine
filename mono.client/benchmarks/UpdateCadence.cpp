#include "CadenceMeasurements.hpp"

#include <engine/assets/Animation.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/physics/Broadphase.hpp>
#include <engine/physics/Characters.hpp>
#include <engine/physics/Pipeline.hpp>
#include <engine/physics/Query.hpp>
#include <engine/render/Animation.hpp>
#include <engine/render/WorldPresentation.hpp>
#include <engine/scene/Animation.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Controls.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Skinning.hpp>
#include <engine/script/UpdateCadenceService.hpp>
#include <engine/testing/Bench.hpp>

TEST_SUITE_ID("client.bench.update-cadence.native")

namespace cadence_native_bench {

	using namespace cadence_bench;
	using namespace engine;
	struct Actor {
		Entity Model, Root, Humanoid, Animator, Track, Target;
		float InitialTargetHealth = 1000000;
		std::vector<Entity> Bones;
	};
	struct World {
		Store Storage{"bench.cadence.native"};
		ecs::Scheduler Physics;
		std::vector<Actor> Actors;
		BeamSet Beams;
		Entity Player;
		uint64_t SourceRevision = 1;
		size_t BoneCount = 0, RayFan = 1;
		std::vector<Vector3> InitialPositions;
		std::vector<uint64_t> BeforeDeadlines;
		std::vector<float> BeforeHealth;
		std::vector<CFrame> ReferenceHistory;

		World(size_t count, size_t bones, size_t keys, size_t beamCount, size_t obstacles, bool full)
			: BoneCount(bones), RayFan(bones > 4 ? 8 : 1) {
			scene::RegisterSceneClasses();
			render::RegisterPresentationComponents();
			script::RegisterUpdateCadenceServiceComponents();
			scene::InstallServices(Storage);
			physics::PreparePhysicsWorld(Storage);
			physics::RegisterPhysicsSystems(Physics);
			physics::RegisterCharacterSystems(Physics);
			Player = scene::AddPlayer(Storage, "Cadence benchmark owner");
			Require(Player != ecs::NULL_ENTITY, "cadence native player creation failed");
			const auto workspace = scene::WorkspaceOf(Storage);
			const Name rigName("bench.cadence.rig"), clipName("bench.cadence.walk");
			assets::AnimationData clip;
			clip.Duration = 128;
			for (size_t joint = 0; joint < bones; ++joint) {
				assets::AnimationChannel channel;
				channel.Joint = static_cast<uint16_t>(joint);
				for (size_t key = 0; key < keys; ++key) {
					const float time = 128 * static_cast<float>(key) / static_cast<float>(keys - 1);
					CFrame frame(Vector3{time * .001f * static_cast<float>(joint + 1), 0, 0});
					const float angle = std::sin(time * .2f + static_cast<float>(joint)) * .05f;
					frame.QuaternionY = std::sin(angle * .5f);
					frame.QuaternionW = std::cos(angle * .5f);
					channel.Keys.push_back({time, frame});
				}
				clip.Channels.push_back(std::move(channel));
			}
			Require(render::RecordAnimation(Storage, clipName, clip), "cadence native clip refused");
			scene::PartDesc floor;
			floor.Frame.Position = {static_cast<float>(count) * 2.5f, -.5f, -20};
			floor.Size = {static_cast<float>(count) * 5 + 8, 1, 128};
			(void)scene::MakePart(Storage, floor);
			for (size_t index = 0; index < count; ++index) {
				Actor actor;
				actor.Model = Storage.CreateInstance(ecs::Classes::Find(Name("Model")), "Citizen");
				Require(Storage.SetParent(actor.Model, workspace), "cadence native model parent refused");
				scene::PartDesc root;
				root.Class = ecs::Classes::Find(Name("MeshPart"));
				root.Frame.Position = {static_cast<float>(index) * 5, 2.5f, 0};
				root.Size = {1, 5, 1};
				root.Simulated = true;
				actor.Root = scene::MakePart(Storage, root);
				Require(Storage.SetParent(actor.Root, actor.Model), "cadence native root parent refused");
				Storage.Set(actor.Root, scene::Skeleton{rigName, static_cast<uint16_t>(bones), {}});
				for (size_t joint = 0; joint < bones; ++joint) {
					const auto entity = Storage.CreateInstance(scene::BoneClass(), "Joint");
					Require(Storage.SetParent(entity, actor.Root), "cadence native bone parent refused");
					scene::Bone bone;
					bone.Joint = static_cast<uint16_t>(joint);
					bone.ParentJoint = joint == 0 ? scene::NO_JOINT : static_cast<uint16_t>(joint - 1);
					bone.Rest.Position.Y = .01f;
					Storage.Set(entity, bone);
					actor.Bones.push_back(entity);
				}
				actor.Animator = Storage.CreateInstance(scene::AnimatorClass(), "Animator");
				Require(
					Storage.SetParent(actor.Animator, actor.Root), "cadence native animator parent refused"
				);
				Storage.Set(actor.Animator, scene::Animator{actor.Root});
				const auto animation = Storage.CreateInstance(ecs::Classes::Find(Name("Animation")), "Walk");
				Storage.Set(animation, scene::AnimationClip{clipName, rigName});
				actor.Track = Storage.CreateInstance(ecs::Classes::Find(Name("AnimationTrack")), "WalkTrack");
				Require(
					Storage.SetParent(actor.Track, actor.Animator), "cadence native track parent refused"
				);
				scene::AnimationTrack track;
				track.Clip = animation;
				track.Playing = true;
				track.Weight = .25f;
				track.FadeTime = .25f;
				Storage.Set(actor.Track, track);
				actor.Humanoid = Storage.CreateInstance(scene::HumanoidClass(), "Humanoid");
				Require(
					Storage.SetParent(actor.Humanoid, actor.Model), "cadence native humanoid parent refused"
				);
				scene::Humanoid humanoid;
				humanoid.RootPart = actor.Root;
				humanoid.WalkSpeed = 1;
				Storage.Set(actor.Humanoid, humanoid);
				Storage.Set(actor.Model, scene::Character{actor.Root, actor.Humanoid, {}});
				scene::PartDesc target;
				target.Frame.Position = {static_cast<float>(index) * 5, 2.5f, -50};
				target.Size = {2, 4, 2};
				actor.Target = scene::MakePart(Storage, target);
				scene::Humanoid targetHealth;
				targetHealth.Health = actor.InitialTargetHealth;
				targetHealth.MaxHealth = actor.InitialTargetHealth;
				Storage.Set(actor.Target, targetHealth);
				Storage.Set(actor.Model, Policy("bench.citizen." + std::to_string(index), full));
				// Configure installs the real service's output/admission rows.
				Require(
					script::ConfigureNpcCadence(
						Storage,
						actor.Model,
						"bench.citizen." + std::to_string(index),
						full ? scene::CadenceInterval{} : scene::CadenceInterval{8, 12},
						full ? scene::CadenceInterval{} : scene::CadenceInterval{4, 8},
						3
					),
					"cadence native configure refused"
				);
				SetAttackDeadline(Storage, actor.Model, 0);
				InitialPositions.push_back(root.Frame.Position);
				Actors.push_back(std::move(actor));
			}
			for (size_t index = 0; index < obstacles; ++index) {
				scene::PartDesc obstacle;
				obstacle.Frame.Position = {
					static_cast<float>(index % count) * 5 + 1.7f,
					2.5f,
					-4 - static_cast<float>(index / count) * 2
				};
				obstacle.Size = {.5f, 3, .5f};
				obstacle.Frame.QuaternionY = .25881904f;
				obstacle.Frame.QuaternionW = .96592583f;
				(void)scene::MakePart(Storage, obstacle);
			}
			std::vector<Entity> owners;
			for (const auto &actor : Actors)
				owners.push_back(actor.Model);
			Beams.Create(Storage, beamCount, full, bones > 4 ? 16 : 2, owners);
			physics::SyncBroadphase(Storage);
			BeforeDeadlines.resize(count);
			BeforeHealth.resize(count);
			ReferenceHistory.resize(HISTORY_TICKS * count * bones);
		}
		void Author(Window window) {
			if (window == Window::Invalidation) ++SourceRevision;
			for (size_t index = 0; index < Actors.size(); ++index) {
				const auto &actor = Actors[index];
				const bool combat =
					window == Window::CombatBurst || (window == Window::SingleCombat && index == 0);
				Require(script::SetNpcCombat(Storage, actor.Model, combat), "cadence native combat refused");
				const auto owner = window == Window::PlayerOwned && index == 0 ? Player : Entity{};
				Storage.GetMutable<scene::Character>(actor.Model)->Owner = owner;
				if (window == Window::Invalidation) {
					++Storage.GetMutable<scene::UpdateCadence>(actor.Model)->Revision;
					Storage.GetMutable<scene::AnimationTrack>(actor.Track)->WeightTarget = .4f;
				}
				if (window == Window::SingleCombat)
					Storage.GetMutable<scene::AnimationTrack>(actor.Track)->WeightTarget = 1;
				BeforeDeadlines[index] = AttackDeadline(Storage, actor.Model);
				BeforeHealth[index] = Storage.Get<scene::Humanoid>(actor.Target)->Health;
			}
			Beams.Author(Storage, window);
		}
		void Step(Work &counts) {
			Storage.ClearChanges();
			Storage.AdvanceTick(DELTA);
			{
				ENGINE_PROFILE_CAT("cadence npc", core::ProfileCategory::Simulation);
				for (auto &actor : Actors) {
					const auto origin = Storage.Get<scene::Transform>(actor.Root)->Frame.Position;
					const auto target = Storage.Get<scene::Transform>(actor.Target)->Frame.Position;
					const auto difference = target - origin;
					const float distance = difference.Magnitude();
					const auto seeing = script::BeginNpcUpdate(
						Storage, 1, actor.Model, UpdateDomain::Perception, SourceRevision
					);
					if (seeing.Due) {
						bool visible = false;
						for (size_t ray = 0; ray < RayFan; ++ray) {
							Vector3 direction = difference / distance;
							if (ray != 0) {
								direction.X += static_cast<float>(ray) * .007f;
								direction = direction.Unit();
							}
							const auto hit = physics::Raycast(
								Storage,
								core::Ray{origin, direction},
								distance + 1,
								spatial::LayerMask::All(),
								actor.Root
							);
							++counts.Rays;
							if (hit) ++counts.RayHits;
							if (ray == 0) visible = hit && hit->Owner == actor.Target;
						}
						Require(
							script::CommitNpcPerception(
								Storage, 1, actor.Model, seeing.Token, SourceRevision, visible, target, true
							),
							"cadence native perception refused"
						);
						++counts.Perception;
					}
					const auto moving = script::BeginNpcUpdate(
						Storage, 1, actor.Model, UpdateDomain::Behaviour, SourceRevision
					);
					if (moving.Due) {
						const bool visible = Storage.Get<script::NpcPerception>(actor.Model)->Visible;
						Require(
							script::CommitNpcBehaviour(
								Storage,
								1,
								actor.Model,
								moving.Token,
								SourceRevision,
								visible ? difference / distance : Vector3{},
								true
							),
							"cadence native behaviour refused"
						);
						++counts.Behaviour;
					}
					++counts.Deadlines;
					if (Storage.Time().Tick >= AttackDeadline(Storage, actor.Model) &&
						script::NpcCombatReady(Storage, actor.Model, SourceRevision)) {
						(void)scene::TakeDamage(Storage, actor.Target, 1);
						SetAttackDeadline(Storage, actor.Model, Storage.Time().Tick + 30);
						++counts.Damage;
					}
				}
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
				Require(history != nullptr && history->Unavailable == 0, "cadence native clip unavailable");
				counts.PoseEvaluated += history->Evaluated;
				counts.PoseSkipped += history->Skipped;
				counts.Promotions += history->Promotions;
			}
			Beams.Build(Storage, counts);
		}
		void Remember() {
			const size_t base = (Storage.Time().Tick % HISTORY_TICKS) * Actors.size() * BoneCount;
			for (size_t actor = 0; actor < Actors.size(); ++actor)
				for (size_t joint = 0; joint < BoneCount; ++joint)
					ReferenceHistory[base + actor * BoneCount + joint] =
						Storage.Get<scene::Bone>(Actors[actor].Bones[joint])->Transform;
		}
		void Verify(const World &reference, Window window, Work &counts) const {
			Require(Storage.Time().Tick == reference.Storage.Time().Tick, "cadence semantic tick diverged");
			const auto tick = Storage.Time().Tick;
			const auto *history = Storage.Resource<scene::PoseCadenceHistory>();
			uint64_t damage = 0;
			for (size_t index = 0; index < Actors.size(); ++index) {
				const auto &actor = Actors[index];
				const auto *track = Storage.Get<scene::AnimationTrack>(actor.Track);
				const auto *fullTrack =
					reference.Storage.Get<scene::AnimationTrack>(reference.Actors[index].Track);
				Require(
					track->TimePosition == fullTrack->TimePosition && track->Weight == fullTrack->Weight,
					"cadence altered semantic animation clock or fade"
				);
				Require(
					Near(track->TimePosition, static_cast<float>(tick) * DELTA, .002f),
					"cadence track stopped ticking"
				);
				const auto &states = Storage.Get<scene::UpdateCadenceState>(actor.Model)->Domains;
				for (const auto domain : {UpdateDomain::Perception, UpdateDomain::Behaviour}) {
					const auto &state = states[static_cast<size_t>(domain)];
					const auto age = tick - state.LastCommittedTick;
					counts.MaximumAge = std::max(counts.MaximumAge, age);
					Require(
						age <= (domain == UpdateDomain::Perception ? 12 : 8),
						"cadence NPC exceeded freshness bound"
					);
					if (Critical(window, index) || window == Window::Invalidation) {
						counts.PromotionLatency = std::max(counts.PromotionLatency, age);
						Require(age == 0, "cadence critical NPC failed immediate admission");
					}
				}
				const auto *perception = Storage.Get<script::NpcPerception>(actor.Model);
				Require(
					perception->Visible && perception->TargetPosition ==
											   Storage.Get<scene::Transform>(actor.Target)->Frame.Position,
					"cadence native ray did not observe its target"
				);
				const auto *behaviour = Storage.Get<script::NpcBehaviour>(actor.Model);
				Require(Near(behaviour->MoveDirection, Vector3{0, 0, -1}), "cadence native steering changed");
				Require(
					Storage.Get<scene::Humanoid>(actor.Humanoid)->MoveDirection == behaviour->MoveDirection,
					"cadence decision did not reach real Humanoid"
				);
				const bool combat =
					window == Window::CombatBurst || (window == Window::SingleCombat && index == 0);
				const bool attack = combat && tick >= BeforeDeadlines[index];
				Require(
					AttackDeadline(Storage, actor.Model) == (attack ? tick + 30 : BeforeDeadlines[index]),
					"cadence skipped or delayed attack deadline"
				);
				Require(
					BeforeHealth[index] - Storage.Get<scene::Humanoid>(actor.Target)->Health ==
						(attack ? 1 : 0),
					"cadence native damage differs from deadline oracle"
				);
				damage += attack;
				const auto &pose = history->Poses.at(actor.Animator);
				const auto age = tick - pose.LastCommittedTick;
				counts.MaximumAge = std::max(counts.MaximumAge, age);
				Require(age <= 8, "cadence pose exceeded freshness bound");
				if (Critical(window, index) || window == Window::Invalidation)
					Require(age == 0, "cadence critical pose did not refresh");
				const size_t base =
					(pose.LastCommittedTick % HISTORY_TICKS) * Actors.size() * BoneCount + index * BoneCount;
				for (size_t joint = 0; joint < BoneCount; ++joint)
					Require(
						Near(
							Storage.Get<scene::Bone>(actor.Bones[joint])->Transform,
							reference.ReferenceHistory[base + joint]
						),
						"cadence pose differs from full evaluation at retained semantic tick"
					);
			}
			Require(counts.Damage == damage, "cadence attack count differs from deadline oracle");
			Beams.Verify(Storage, reference.Beams, window, counts);
		}
	};

	template <bool COMPLEX, bool FULL> void Run() {
		constexpr size_t actors = COMPLEX ? 128 : 16, bones = COMPLEX ? 64 : 4;
		constexpr size_t keys = COMPLEX ? 32 : 8, beams = COMPLEX ? 256 : 16,
						 obstacles = COMPLEX ? 2048 : 128;
		const size_t ticks = TickCount();
		World world(actors, bones, keys, beams, obstacles, FULL);
		World reference(actors, bones, keys, beams, obstacles, true);
		for (size_t tick = 0; tick < WARM_TICKS; ++tick) {
			world.Author(Window::Steady);
			reference.Author(Window::Steady);
			Work mainWork, referenceWork;
			world.Step(mainWork);
			reference.Step(referenceWork);
			reference.Remember();
			world.Verify(reference, Window::Steady, mainWork);
		}
		Measurements measured(ticks);
		Work total;
		for (size_t tick = 0; tick < ticks; ++tick) {
			const auto window = WindowAt(tick, ticks);
			world.Author(window);
			reference.Author(window);
			measured.Measure(tick, window, [&](Work &counts) { world.Step(counts); });
			Work referenceWork;
			reference.Step(referenceWork);
			reference.Remember();
			world.Verify(reference, window, measured.Ticks[tick].Counts);
			total.Add(measured.Ticks[tick].Counts);
		}
		Require(
			total.Rays > 0 && total.RayHits > 0 && total.Damage > 0 && total.PoseWrites > 0 &&
				total.BeamRebuilt > 0,
			"cadence native workload did no actual consumer work"
		);
		Require(
			FULL || (total.PoseSkipped > 0 && total.BeamSkipped > 0 && total.Perception < actors * ticks),
			"cadence native authored workload did not skip work"
		);
		for (size_t index = 0; index < actors; ++index)
			Require(
				world.Storage.Get<scene::Transform>(world.Actors[index].Root)->Frame.Position.Z <
					world.InitialPositions[index].Z - 1,
				"cadence native Humanoid never physically moved"
			);
		static size_t calls = 0;
		const char *row = COMPLEX ? (FULL ? "native-complex-full" : "native-complex-authored")
								  : (FULL ? "native-simple-full" : "native-simple-authored");
		auto &summary = BeginSummary(row, ++calls);

		summary.Append(
			"# cadence-clock fixture=native full=%d final_tick=%llu track_time_seconds=%.9f "
			"next_attack_tick=%llu target_health=%.1f source_revision=%llu rays_per_perception=%zu "
			"reference=full_fidelity_history_outside_owner\n",
			FULL,
			static_cast<unsigned long long>(world.Storage.Time().Tick),
			world.Storage.Get<scene::AnimationTrack>(world.Actors.front().Track)->TimePosition,
			static_cast<unsigned long long>(AttackDeadline(world.Storage, world.Actors.front().Model)),
			world.Storage.Get<scene::Humanoid>(world.Actors.front().Target)->Health,
			static_cast<unsigned long long>(world.SourceRevision),
			world.RayFan
		);

		measured.Summarize(
			summary,
			row,
			calls,
			actors,
			1,
			bones,
			keys,
			beams,
			obstacles,
			FULL,
			world.Beams.Payload(world.Storage) +
				world.Storage.Resource<scene::PoseCadenceHistory>()->Poses.size() *
					sizeof(scene::DomainCadenceState)
		);
		VmMemoryWindow{}.Summarize(summary, row);
		engine::testing::Consume(total.Rays);
	}
}

BENCH("Native simple full fidelity, whole harness", 1) {
	cadence_native_bench::Run<false, true>();
}
BENCH("Native simple authored cadence, whole harness", 1) {
	cadence_native_bench::Run<false, false>();
}
BENCH("Native complex full fidelity, whole harness", 1) {
	cadence_native_bench::Run<true, true>();
}
BENCH("Native complex authored cadence, whole harness", 1) {
	cadence_native_bench::Run<true, false>();
}
