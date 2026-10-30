#include <engine/assets/Animation.hpp>
#include <engine/core/Bytes.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/render/Animation.hpp>
#include <engine/render/WorldPresentation.hpp>
#include <engine/scene/Animation.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Skinning.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <client/Replicated.hpp>
#include <client/Scene.hpp>

TEST_SUITE_ID("client.animation")
TEST_DEPENDS("engine.scene.animation")
TEST_DEPENDS("engine.scene.skinning")
TEST_DEPENDS("engine.render.animation")
TEST_DEPENDS("engine.ecs.scheduler")

namespace {
	struct AnimationWorkers {
		AnimationWorkers() {
			engine::parallel::Jobs::Start(1);
		}
		~AnimationWorkers() {
			engine::parallel::Jobs::Stop();
		}
	};
}

TEST_CASE(
	"replica host samples received animation before publishing the skin palette",
	"[client][animation][replica]"
) {
	using namespace engine;
	AnimationWorkers workers;
	client::RegisterClientComponents();
	scene::RegisterSceneClasses();
	ecs::Store authority("animation.authority");
	scene::PartDesc description;
	description.Class = ecs::Classes::Find(core::Name("SkinnedMeshPart"));
	description.Frame = core::CFrame(core::Vector3{10, 1, 0});
	const auto rig = scene::MakePart(authority, description);
	REQUIRE(rig != ecs::NULL_ENTITY);
	authority.Set(rig, scene::Skeleton{core::Name("host.rig"), 1, {}});
	const auto bone = authority.CreateInstance(scene::BoneClass(), "Root");
	REQUIRE(authority.SetParent(bone, rig));
	scene::Bone joint;
	joint.Rest = core::CFrame(core::Vector3{0, 1, 0});
	authority.Set(bone, joint);
	const auto animator = authority.CreateInstance(scene::AnimatorClass(), "Animator");
	REQUIRE(authority.SetParent(animator, rig));
	const auto clip = authority.CreateInstance(ecs::Classes::Find(core::Name("Animation")), "Move");
	const auto buffer = authority.CreateInstance(ecs::Classes::Find(core::Name("AnimationBuffer")), "Clip");
	assets::AnimationData data;
	data.Duration = 1;
	data.Channels = {{0, {{0, {}}, {1, core::CFrame(core::Vector3{4, 0, 0})}}}};
	core::ByteWriter encoded;
	REQUIRE(assets::Animation::Write(encoded, data));
	REQUIRE(scene::SetAnimationBuffer(authority, buffer, encoded.Bytes()));
	authority.Set(clip, scene::AnimationClip{{}, core::Name("host.rig"), buffer});
	const auto track = authority.CreateInstance(ecs::Classes::Find(core::Name("AnimationTrack")), "Track");
	REQUIRE(authority.SetParent(track, animator));
	scene::AnimationTrack playing;
	playing.Clip = clip;
	playing.TimePosition = 0.5f;
	playing.Weight = 1;
	playing.WeightTarget = 1;
	playing.FadeTime = 0;
	playing.Playing = true;
	authority.Set(track, playing);
	core::ByteWriter snapshot;
	REQUIRE(authority.Save(snapshot));
	ecs::Store replica("animation.replica");
	core::ByteReader reader(snapshot.Bytes());
	REQUIRE(replica.Load(reader));
	replica.SetAdoptOnly(true);
	ecs::Scheduler scheduler;
	REQUIRE(client::BuildReplicatedWorld(replica, scheduler) != nullptr);
	REQUIRE(scheduler.HasSystem("evaluate-animations", ecs::Phase::PreRender));
	REQUIRE(scheduler.HasSystem("resolve-bones", ecs::Phase::PreRender));
	REQUIRE(scheduler.Validate().Status == ecs::SystemScheduleStatus::Ready);
	client::RecordReplicatedTick(replica, 1);
	for (int tick = 0; tick < 4; ++tick) {
		replica.AdvanceTick(0.125f);
		scheduler.RunPhases(replica, ecs::Phase::Input, ecs::Phase::Replication);
		replica.SetFrame(1.0f / 240.0f, 0);
		scheduler.RunPhases(replica, ecs::Phase::PreRender, ecs::Phase::PreRender);
	}
	CHECK(replica.Get<scene::AnimationTrack>(track)->TimePosition == 0.5f);
	CHECK(replica.Get<scene::AnimationTrack>(track)->Weight == 1);
	CHECK(replica.Get<scene::Bone>(bone)->Transform.Position.X == Catch::Approx(2));
	CHECK(replica.Get<scene::Bone>(bone)->WorldFrame.Position.X == Catch::Approx(12));
	CHECK(replica.Get<scene::Bone>(bone)->WorldFrame.Position.Y == Catch::Approx(2));
	const auto *draw = replica.Resource<render::DrawList>();
	REQUIRE(draw != nullptr);
	REQUIRE(draw->JointFrames.size() == 1);
	CHECK(draw->JointFrames[0].Position.X == Catch::Approx(2));
	CHECK(draw->JointFrames[0].Position.Y == Catch::Approx(1));

	render::RegisterAnimationPresentationSystems(scheduler);
	render::RegisterAnimationPresentationSystems(scheduler);
	REQUIRE(scheduler.Validate().Status == ecs::SystemScheduleStatus::Ready);
	scheduler.RunPhases(replica, ecs::Phase::PreRender, ecs::Phase::PreRender);
	CHECK(replica.Get<scene::AnimationTrack>(track)->TimePosition == 0.5f);
	CHECK(replica.Get<scene::Bone>(bone)->Transform.Position.X == Catch::Approx(2));
}
