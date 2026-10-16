#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Animation.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <server/Simulation.hpp>

TEST_SUITE_ID("server.animation")
TEST_DEPENDS("engine.scene.animation")
TEST_DEPENDS("engine.ecs.scheduler")

TEST_CASE(
	"dedicated server preparation advances semantic animation on its fixed tick", "[server][animation]"
) {
	using namespace engine;
	server::RegisterServerComponents();
	scene::RegisterSceneClasses();
	ecs::Store store("server.animation");
	ecs::Scheduler scheduler;
	server::PrepareSimulation(store, scheduler, 0);
	scene::RegisterAnimationClockSystem(scheduler);
	scene::RegisterAnimationClockSystem(scheduler);
	REQUIRE(scheduler.Validate().Status == ecs::SystemScheduleStatus::Ready);
	const auto track = store.CreateInstance(ecs::Classes::Find(core::Name("AnimationTrack")), "Playing");
	scene::AnimationTrack playing;
	playing.TimePosition = 2;
	playing.Speed = 2;
	playing.Playing = true;
	playing.Weight = 0;
	playing.WeightTarget = 1;
	playing.FadeTime = 0.5f;
	store.Set(track, playing);
	for (int tick = 0; tick < 4; ++tick) {
		store.AdvanceTick(0.125f);
		scheduler.RunPhases(store, ecs::Phase::Input, ecs::Phase::Replication);
	}
	const auto *advanced = store.Get<scene::AnimationTrack>(track);
	REQUIRE(advanced != nullptr);
	CHECK(advanced->TimePosition == Catch::Approx(3));
	CHECK(advanced->Weight == Catch::Approx(1));
	CHECK(store.Time().Tick == 4);
	CHECK(store.Time().FrameDelta == 0);
}
