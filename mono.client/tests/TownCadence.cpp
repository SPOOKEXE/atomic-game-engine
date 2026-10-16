#include <engine/core/Bytes.hpp>
#include <engine/core/Paths.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/effects/Registration.hpp>
#include <engine/effects/Ribbon.hpp>
#include <engine/physics/Broadphase.hpp>
#include <engine/physics/Pipeline.hpp>
#include <engine/render/Animation.hpp>
#include <engine/render/WorldPresentation.hpp>
#include <engine/scene/Animation.hpp>
#include <engine/scene/Attachments.hpp>
#include <engine/scene/Components.hpp>
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
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

TEST_SUITE_ID("client.town-cadence")
TEST_DEPENDS("engine.scripthost.updatecadenceservice")
TEST_DEPENDS("engine.render.animation")
TEST_DEPENDS("engine.effects.beam-cadence")

namespace {
	using namespace engine;
	constexpr float TICK_SECONDS = 1.0f / 60.0f;
	constexpr float POSE_TOLERANCE = 0.0001f;

	struct Citizen {
		ecs::Entity Model, Body, Animator, Track, Bone, Target;
		std::vector<ecs::Entity> Beams;
	};

	struct TickSample {
		std::array<std::array<float, 12>, 2> Bones{};
		std::array<float, 2> TrackTimes{};
		std::array<std::array<uint64_t, 4>, 2> Generations{};
		std::array<uint64_t, 4> Counters{};
		effects::RibbonBuffer Ribbons;
	};

	std::string ReadTown(script::Language language) {
		const auto filename = language == script::Language::Luau ? "TownCadence.luau" : "TownCadence.js";
		auto source = std::filesystem::absolute(__FILE__).parent_path().parent_path().parent_path() /
					  "mono.engine/examples/assets/scripts" / filename;
		if (!std::filesystem::is_regular_file(source))
			source = core::Paths::Assets() / "examples/scripts" / filename;
		std::ifstream input(source, std::ios::binary);
		REQUIRE(input.good());
		return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
	}

	bool Near(float left, float right, float tolerance = POSE_TOLERANCE) {
		return std::abs(left - right) <= tolerance;
	}

	bool Near(const core::CFrame &left, const core::CFrame &right) {
		const auto a = left.GetComponents();
		const auto b = right.GetComponents();
		for (size_t index = 0; index < a.size(); ++index)
			if (!Near(a[index], b[index])) return false;
		return true;
	}

	void CompareRibbons(const effects::RibbonBuffer &actual, const effects::RibbonBuffer &expected) {
		CHECK(actual.Runs.size() == expected.Runs.size());
		CHECK(actual.Vertices.size() == expected.Vertices.size());
		if (actual.Runs.size() != expected.Runs.size() || actual.Vertices.size() != expected.Vertices.size())
			return;
		for (size_t index = 0; index < actual.Runs.size(); ++index) {
			const auto &left = actual.Runs[index];
			const auto &right = expected.Runs[index];
			CHECK(left.First == right.First);
			CHECK(left.Count == right.Count);
			CHECK(left.Texture == right.Texture);
			CHECK(Near(left.ZOffset, right.ZOffset));
			CHECK(left.Additive == right.Additive);
			CHECK(left.FaceCamera == right.FaceCamera);
			CHECK(left.RepeatV == right.RepeatV);
		}
		for (size_t index = 0; index < actual.Vertices.size(); ++index) {
			const auto &left = actual.Vertices[index];
			const auto &right = expected.Vertices[index];
			CHECK(Near((left.Position - right.Position).Magnitude(), 0));
			CHECK(Near(left.Coordinate.X, right.Coordinate.X));
			CHECK(Near(left.Coordinate.Y, right.Coordinate.Y));
			CHECK(left.Colour == right.Colour);
		}
	}

	std::optional<std::vector<TickSample>> RunTown(script::Language language) {
		INFO((language == script::Language::Luau ? "Luau" : "JavaScript"));
		scene::RegisterSceneClasses();
		render::RegisterPresentationComponents();
		effects::RegisterEffectClasses();
		ecs::Store subject("town.authored-cadence");
		physics::PreparePhysicsWorld(subject);
		const auto runtime = script::MakeRuntime(subject, language);
		REQUIRE(runtime != nullptr);
		const bool ran = runtime->Run(ReadTown(language), "TownCadence");
		INFO(runtime->LastError());
		REQUIRE(ran);

		const auto workspace = scene::WorkspaceOf(subject);
		const auto target = subject.FindFirstChild(workspace, "Practice Target");
		REQUIRE(target != ecs::NULL_ENTITY);
		std::array<Citizen, 2> citizens{};
		std::unordered_set<std::string> identities;
		const std::array<std::string, 2> names{"North Guard", "South Guard"};
		const std::array<scene::CadenceInterval, 4> expected{{{8, 12}, {4, 8}, {4, 8}, {4, 8}}};
		for (size_t index = 0; index < citizens.size(); ++index) {
			auto &citizen = citizens[index];
			INFO(names[index]);
			citizen.Model = subject.FindFirstChild(workspace, names[index]);
			REQUIRE(citizen.Model != ecs::NULL_ENTITY);
			citizen.Body = subject.FindFirstChild(citizen.Model, "Body");
			REQUIRE(citizen.Body != ecs::NULL_ENTITY);
			citizen.Animator = subject.FindFirstChild(citizen.Body, "Animator");
			citizen.Track = subject.FindFirstChild(citizen.Animator, "AnimationTrack");
			citizen.Bone = subject.FindFirstChild(citizen.Body, "Root");
			citizen.Target = target;
			REQUIRE(citizen.Animator != ecs::NULL_ENTITY);
			REQUIRE(citizen.Track != ecs::NULL_ENTITY);
			REQUIRE(citizen.Bone != ecs::NULL_ENTITY);

			const auto *policy = subject.Get<scene::UpdateCadence>(citizen.Model);
			REQUIRE(policy != nullptr);
			CHECK(policy->Identity.IsValid());
			CHECK(policy->Owner == citizen.Model);
			CHECK(policy->Identity.Text() == "town.citizen." + names[index]);
			CHECK(identities.insert(std::string(policy->Identity.Text())).second);
			for (size_t domain = 0; domain < expected.size(); ++domain) {
				CHECK(policy->Domains[domain].IntervalTicks == expected[domain].IntervalTicks);
				CHECK(
					policy->Domains[domain].MaximumStalenessTicks == expected[domain].MaximumStalenessTicks
				);
			}
			subject.EachDescendant(citizen.Model, [&](ecs::Entity entity) {
				const auto *beam = subject.Get<effects::Beam>(entity);
				if (beam == nullptr) return;
				citizen.Beams.push_back(entity);
				CHECK(beam->Enabled);
				CHECK(beam->Attachment0 != ecs::NULL_ENTITY);
				CHECK(beam->Attachment1 != ecs::NULL_ENTITY);
				CHECK(beam->Attachment0 != beam->Attachment1);
				CHECK(subject.Alive(beam->Attachment0));
				CHECK(subject.Alive(beam->Attachment1));
				CHECK(subject.Has<scene::Attachment>(beam->Attachment0));
				CHECK(subject.Has<scene::Attachment>(beam->Attachment1));
				CHECK(std::abs(beam->TextureSpeed) > 0.0f);
			});
			CHECK(citizen.Beams.size() == 1);
		}
		if (citizens[0].Beams.size() != 1 || citizens[1].Beams.size() != 1) return std::nullopt;

		core::ByteWriter snapshot;
		REQUIRE(subject.Save(snapshot));
		ecs::Store reference("town.full-cadence-reference");
		core::ByteReader reader(snapshot.Bytes());
		REQUIRE(reference.Load(reader));
		physics::PreparePhysicsWorld(reference);
		// Beam attachment handles are intentionally omitted from snapshots, so restore authored endpoints
		// only in this reference copy before using the real ribbon builder.
		subject.Each<effects::Beam>([&](ecs::Entity entity, const effects::Beam &beam) {
			reference.Set(entity, beam);
		});
		for (const auto &citizen : citizens) {
			auto policy = *reference.Get<scene::UpdateCadence>(citizen.Model);
			policy.Domains[static_cast<size_t>(scene::UpdateDomain::VisualPose)] = {1, 1};
			policy.Domains[static_cast<size_t>(scene::UpdateDomain::CosmeticEffects)] = {1, 1};
			reference.Set(citizen.Model, policy);
		}
		// Keep root frames fixed so this fixture isolates presentation cadence from physics integration.
		subject.GetMutable<scene::Transform>(target)->Frame.Position = {0, 2.5f, -500};
		reference.GetMutable<scene::Transform>(target)->Frame.Position = {0, 2.5f, -500};
		physics::SyncBroadphase(subject);
		physics::SyncBroadphase(reference);

		std::vector<TickSample> samples;
		samples.reserve(25);
		effects::RibbonBuffer actualRibbons, fullRibbons;
		uint64_t totalPoseSkips = 0, totalBeamSkips = 0;
		for (uint64_t step = 1; step <= 24; ++step) {
			const auto tickWork = [&](ecs::Store &store, script::Runtime *scriptRuntime) {
				store.AdvanceTick(TICK_SECONDS);
				if (scriptRuntime != nullptr) {
					const bool heartbeat = scriptRuntime->Heartbeat(TICK_SECONDS);
					INFO(scriptRuntime->LastError());
					REQUIRE(heartbeat);
					store.FlushSignals();
				}
				(void)scene::AdvanceAnimationTracks(store);
			};
			std::array<std::array<float, 12>, 2> previousBones{};
			std::array<uint64_t, 2> previousPoseGenerations{};
			std::array<uint64_t, 2> previousBeamGenerations{};
			for (size_t index = 0; index < citizens.size(); ++index) {
				previousBones[index] =
					subject.Get<scene::Bone>(citizens[index].Bone)->Transform.GetComponents();
				const auto *history = subject.Resource<scene::PoseCadenceHistory>();
				if (history != nullptr && history->Poses.contains(citizens[index].Animator))
					previousPoseGenerations[index] = history->Poses.at(citizens[index].Animator).Generation;
				const auto *beams = subject.Resource<effects::BeamCadenceCache>();
				if (beams != nullptr && beams->Beams.contains(citizens[index].Beams.front()))
					previousBeamGenerations[index] =
						beams->Beams.at(citizens[index].Beams.front()).State.Generation;
			}
			tickWork(subject, runtime.get());
			REQUIRE(render::EvaluateAnimations(subject) <= citizens.size());
			tickWork(reference, nullptr);
			REQUIRE(render::EvaluateAnimations(reference) == citizens.size());

			const auto *poseHistory = subject.Resource<scene::PoseCadenceHistory>();
			REQUIRE(poseHistory != nullptr);
			CHECK(poseHistory->MaximumAgeTicks <= 8);
			totalPoseSkips += poseHistory->Skipped;
			for (size_t index = 0; index < citizens.size(); ++index) {
				const auto &citizen = citizens[index];
				REQUIRE(poseHistory->Poses.contains(citizen.Animator));
				const auto &pose = poseHistory->Poses.at(citizen.Animator);
				CHECK(step - pose.LastCommittedTick <= 8);
				const auto *fullBone = reference.Get<scene::Bone>(citizen.Bone);
				const auto *actualBone = subject.Get<scene::Bone>(citizen.Bone);
				REQUIRE(fullBone != nullptr);
				REQUIRE(actualBone != nullptr);
				const bool poseAdvanced = pose.Generation > previousPoseGenerations[index];
				CHECK(poseAdvanced == (pose.LastCommittedTick == step));
				if (poseAdvanced) {
					CHECK(Near(actualBone->Transform, fullBone->Transform));
				} else {
					const auto current = actualBone->Transform.GetComponents();
					for (size_t component = 0; component < current.size(); ++component)
						CHECK(Near(current[component], previousBones[index][component]));
				}
				const auto *track = subject.Get<scene::AnimationTrack>(citizen.Track);
				REQUIRE(track != nullptr);
				CHECK(Near(track->TimePosition, static_cast<float>(step) * TICK_SECONDS, 0.002f));
			}

			const core::Vector3 eye{static_cast<float>(step) * 0.05f, 40, -5};
			const float elapsed = static_cast<float>(step) * TICK_SECONDS;
			CHECK(effects::BuildRibbons(subject, eye, elapsed, actualRibbons) == citizens.size());
			CHECK(effects::BuildRibbons(reference, eye, elapsed, fullRibbons) == citizens.size());
			CompareRibbons(actualRibbons, fullRibbons);
			const auto *beamHistory = subject.Resource<effects::BeamCadenceCache>();
			REQUIRE(beamHistory != nullptr);
			CHECK(beamHistory->Rebuilt + beamHistory->Skipped == citizens.size());
			totalBeamSkips += beamHistory->Skipped;
			TickSample sample;
			sample.Ribbons = actualRibbons;
			sample.Counters = {
				poseHistory->Evaluated, poseHistory->Skipped, beamHistory->Rebuilt, beamHistory->Skipped
			};
			for (size_t index = 0; index < citizens.size(); ++index) {
				const auto &citizen = citizens[index];
				REQUIRE(beamHistory->Beams.contains(citizen.Beams.front()));
				const auto *policy = subject.Get<scene::UpdateCadence>(citizen.Model);
				const auto *state = subject.Get<scene::UpdateCadenceState>(citizen.Model);
				REQUIRE(policy != nullptr);
				REQUIRE(state != nullptr);
				CHECK(policy->Owner == citizen.Model);
				for (size_t domain = 0; domain < expected.size(); ++domain) {
					CHECK(policy->Domains[domain].IntervalTicks == expected[domain].IntervalTicks);
					CHECK(
						policy->Domains[domain].MaximumStalenessTicks ==
						expected[domain].MaximumStalenessTicks
					);
				}
				const auto *track = subject.Get<scene::AnimationTrack>(citizen.Track);
				sample.TrackTimes[index] = track->TimePosition;
				sample.Bones[index] = subject.Get<scene::Bone>(citizen.Bone)->Transform.GetComponents();
				sample.Generations[index][0] = state->Domains[0].Generation;
				sample.Generations[index][1] = state->Domains[1].Generation;
				sample.Generations[index][2] = poseHistory->Poses.at(citizen.Animator).Generation;
				const auto &beamState = beamHistory->Beams.at(citizen.Beams.front()).State;
				sample.Generations[index][3] = beamState.Generation;
				CHECK(step - beamState.LastCommittedTick <= 8);
				CHECK(
					(beamState.Generation > previousBeamGenerations[index]) ==
					(beamState.LastCommittedTick == step)
				);
			}
			samples.push_back(std::move(sample));
		}
		CHECK(totalPoseSkips > 0);
		CHECK(totalBeamSkips > 0);

		const auto north = citizens[0].Model;
		const auto northBodyPosition = subject.Get<scene::Transform>(citizens[0].Body)->Frame.Position;
		subject.GetMutable<scene::Transform>(target)->Frame.Position =
			northBodyPosition + core::Vector3{0, 0, -5};
		reference.GetMutable<scene::Transform>(target)->Frame.Position =
			northBodyPosition + core::Vector3{0, 0, -5};
		physics::SyncBroadphase(subject);
		physics::SyncBroadphase(reference);
		const uint64_t combatTick = subject.Time().Tick + 1;
		const auto *previousCombatPoses = subject.Resource<scene::PoseCadenceHistory>();
		REQUIRE(previousCombatPoses != nullptr);
		REQUIRE(previousCombatPoses->Poses.contains(citizens[0].Animator));
		const auto northPoseGeneration = previousCombatPoses->Poses.at(citizens[0].Animator).Generation;
		subject.AdvanceTick(TICK_SECONDS);
		reference.AdvanceTick(TICK_SECONDS);
		const bool heartbeat = runtime->Heartbeat(TICK_SECONDS);
		INFO(runtime->LastError());
		REQUIRE(heartbeat);
		subject.FlushSignals();
		CHECK(script::NpcCombatReady(subject, north, 1));
		(void)scene::AdvanceAnimationTracks(subject);
		(void)scene::AdvanceAnimationTracks(reference);
		CHECK(render::EvaluateAnimations(subject) <= citizens.size());
		REQUIRE(render::EvaluateAnimations(reference) == citizens.size());
		const auto *combatState = subject.Get<scene::UpdateCadenceState>(north);
		REQUIRE(combatState != nullptr);
		CHECK(combatState->Domains[0].LastCommittedTick == combatTick);
		CHECK(combatState->Domains[1].LastCommittedTick == combatTick);
		const auto *combatPoses = subject.Resource<scene::PoseCadenceHistory>();
		REQUIRE(combatPoses != nullptr);
		REQUIRE(combatPoses->Poses.contains(citizens[0].Animator));
		CHECK(combatPoses->Poses.at(citizens[0].Animator).LastCommittedTick == combatTick);
		CHECK(combatPoses->Poses.at(citizens[0].Animator).Generation > northPoseGeneration);
		CHECK(Near(
			subject.Get<scene::Bone>(citizens[0].Bone)->Transform,
			reference.Get<scene::Bone>(citizens[0].Bone)->Transform
		));
		const core::Vector3 combatEye{2, 40, -5};
		effects::BuildRibbons(
			subject, combatEye, static_cast<float>(combatTick) * TICK_SECONDS, actualRibbons
		);
		effects::BuildRibbons(
			reference, combatEye, static_cast<float>(combatTick) * TICK_SECONDS, fullRibbons
		);
		CompareRibbons(actualRibbons, fullRibbons);
		const auto *combatBeams = subject.Resource<effects::BeamCadenceCache>();
		REQUIRE(combatBeams != nullptr);
		REQUIRE(combatBeams->Beams.contains(citizens[0].Beams.front()));
		const auto &northBeam = combatBeams->Beams.at(citizens[0].Beams.front()).State;
		CHECK(northBeam.LastCommittedTick == combatTick);
		TickSample combatSample;
		combatSample.Ribbons = actualRibbons;
		combatSample.Counters = {
			combatPoses->Evaluated, combatPoses->Skipped, combatBeams->Rebuilt, combatBeams->Skipped
		};
		for (size_t index = 0; index < citizens.size(); ++index) {
			const auto &citizen = citizens[index];
			const auto *state = subject.Get<scene::UpdateCadenceState>(citizen.Model);
			REQUIRE(state != nullptr);
			REQUIRE(combatPoses->Poses.contains(citizen.Animator));
			REQUIRE(combatBeams->Beams.contains(citizen.Beams.front()));
			combatSample.TrackTimes[index] = subject.Get<scene::AnimationTrack>(citizen.Track)->TimePosition;
			combatSample.Bones[index] = subject.Get<scene::Bone>(citizen.Bone)->Transform.GetComponents();
			combatSample.Generations[index] = {
				state->Domains[0].Generation,
				state->Domains[1].Generation,
				combatPoses->Poses.at(citizen.Animator).Generation,
				combatBeams->Beams.at(citizen.Beams.front()).State.Generation,
			};
		}
		samples.push_back(std::move(combatSample));

		return samples;
	}

	void CompareSequences(const std::vector<TickSample> &left, const std::vector<TickSample> &right) {
		REQUIRE(left.size() == right.size());
		for (size_t tick = 0; tick < left.size(); ++tick) {
			for (size_t citizen = 0; citizen < 2; ++citizen) {
				CHECK(Near(left[tick].TrackTimes[citizen], right[tick].TrackTimes[citizen]));
				CHECK(left[tick].Generations[citizen] == right[tick].Generations[citizen]);
				for (size_t component = 0; component < 12; ++component)
					CHECK(Near(left[tick].Bones[citizen][component], right[tick].Bones[citizen][component]));
			}
			CHECK(left[tick].Counters == right[tick].Counters);
			CompareRibbons(left[tick].Ribbons, right[tick].Ribbons);
		}
	}
}

TEST_CASE("paired town scripts exercise authored pose and beam cadence", "[town-authored-cadence]") {
	const auto luau = RunTown(engine::script::Language::Luau);
	const auto javascript = RunTown(engine::script::Language::JavaScript);
	if (luau.has_value() && javascript.has_value()) CompareSequences(*luau, *javascript);
}
