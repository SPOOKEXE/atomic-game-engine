#include <engine/assets/Animation.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/render/Animation.hpp>
#include <engine/render/WorldPresentation.hpp>
#include <engine/scene/Animation.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Skinning.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.render.animation")
TEST_DEPENDS("engine.scene.animation")

using engine::core::Name;

TEST_CASE("animation tracks sample clip channels into bone transforms", "[render][animation]") {
	engine::scene::RegisterSceneClasses();
	engine::render::RegisterPresentationComponents();
	engine::ecs::Store store("animation-presentation");

	const engine::ecs::Entity rig = store.CreateInstance(engine::ecs::Classes::Find(Name("MeshPart")), "Rig");
	store.Set(rig, engine::scene::Skeleton{Name("animation.Rig"), 1, {}});
	const engine::ecs::Entity bone = store.CreateInstance(engine::ecs::Classes::Find(Name("Bone")), "Root");
	REQUIRE(store.SetParent(bone, rig));
	store.Set(bone, engine::scene::Bone{});

	const engine::ecs::Entity animator = store.CreateInstance(engine::scene::AnimatorClass(), "Animator");
	REQUIRE(store.SetParent(animator, rig));
	store.Set(animator, engine::scene::Animator{});
	const engine::ecs::Entity clip =
		store.CreateInstance(engine::ecs::Classes::Find(Name("Animation")), "Move");
	store.Set(clip, engine::scene::AnimationClip{Name("animation.Move"), Name("animation.Rig")});
	const engine::ecs::Entity track =
		store.CreateInstance(engine::ecs::Classes::Find(Name("AnimationTrack")), "Track");
	REQUIRE(store.SetParent(track, animator));
	engine::scene::AnimationTrack playing;
	playing.Clip = clip;
	playing.TimePosition = 0.5f;
	playing.Weight = 1.0f;
	playing.Playing = true;
	store.Set(track, playing);

	engine::assets::AnimationData data;
	data.Duration = 1.0f;
	data.Channels = {{0, {{0.0f, {}}, {1.0f, engine::core::CFrame(engine::core::Vector3{4, 0, 0})}}}};
	REQUIRE(engine::render::RecordAnimation(store, Name("animation.Move"), data));

	CHECK(engine::render::EvaluateAnimations(store) == 1);
	const engine::scene::Bone *posed = store.Get<engine::scene::Bone>(bone);
	REQUIRE(posed != nullptr);
	CHECK(posed->Transform.Position.X == Catch::Approx(2.0f));
}
