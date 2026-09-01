// Where a rig's pose is driven from, and the one refusal this module makes.
//
// The resolution cases are the point: an animator finds its rig through the
// tree or through a handle, and a rig finds its animator without either side
// keeping a back-pointer that could go stale.

#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Animation.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Skinning.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.scene.animation")

using engine::core::Name;
using engine::ecs::Classes;
using engine::ecs::Entity;
using engine::ecs::NULL_ENTITY;
using engine::ecs::Store;
using engine::scene::AnimationClip;
using engine::scene::AnimationPriority;
using engine::scene::AnimationTrack;
using engine::scene::Animator;
using engine::scene::AnimatorClass;
using engine::scene::AnimatorFor;
using engine::scene::ClipFitsRig;
using engine::scene::RegisterSceneClasses;
using engine::scene::RigFor;
using engine::scene::Skeleton;

namespace {
	Entity MakeRig(Store &store, std::string_view name, const char *label) {
		const Entity rig = store.CreateInstance(Classes::Find(Name("MeshPart")), label);
		store.Set(rig, Skeleton{Name(name), 4, {}});
		return rig;
	}
}

TEST_CASE("an animator finds the rig it is parented into", "[scene][animation]") {
	// Roblox's arrangement: an `Animator` under a `Humanoid` under the model,
	// with the skinned mesh a sibling. Nothing names anything.
	RegisterSceneClasses();
	Store store("animation_test.tree");

	const Entity model = store.CreateInstance(Classes::Find(Name("Model")), "Character");
	const Entity rig = MakeRig(store, "animation_test.Fox", "Body");
	REQUIRE(store.SetParent(rig, model));

	const Entity animator = store.CreateInstance(AnimatorClass(), "Animator");
	REQUIRE(store.SetParent(animator, rig));
	store.Set(animator, Animator{});

	CHECK(RigFor(store, animator) == rig);
	CHECK(AnimatorFor(store, rig) == animator);
}

TEST_CASE("an animator beside the rig is still its animator", "[scene][animation]") {
	// The parent's subtree, which is where a `Humanoid`'s animator actually sits.
	RegisterSceneClasses();
	Store store("animation_test.sibling");

	const Entity model = store.CreateInstance(Classes::Find(Name("Model")), "Character");
	const Entity rig = MakeRig(store, "animation_test.Fox", "Body");
	REQUIRE(store.SetParent(rig, model));

	const Entity animator = store.CreateInstance(AnimatorClass(), "Animator");
	REQUIRE(store.SetParent(animator, model));

	Animator driver;
	driver.Rig = rig;
	store.Set(animator, driver);

	CHECK(RigFor(store, animator) == rig);
	CHECK(AnimatorFor(store, rig) == animator);
}

TEST_CASE("a named rig beats the one the tree would find", "[scene][animation]") {
	// Naming one is the explicit case and the walk is the convenience, so the
	// handle has to win even when the animator sits inside a different rig.
	RegisterSceneClasses();
	Store store("animation_test.named");

	const Entity near = MakeRig(store, "animation_test.Near", "Near");
	const Entity far = MakeRig(store, "animation_test.Far", "Far");

	const Entity animator = store.CreateInstance(AnimatorClass(), "Animator");
	REQUIRE(store.SetParent(animator, near));

	Animator driver;
	driver.Rig = far;
	store.Set(animator, driver);

	CHECK(RigFor(store, animator) == far);
	CHECK(AnimatorFor(store, near) == NULL_ENTITY);
}

TEST_CASE("a handle naming something that is not a rig falls back to the tree", "[scene][animation]") {
	// A rig destroyed under a script leaves a stale handle behind, and the
	// useful answer is the one the tree still says rather than nothing at all.
	RegisterSceneClasses();
	Store store("animation_test.stale");

	const Entity rig = MakeRig(store, "animation_test.Fox", "Body");
	const Entity animator = store.CreateInstance(AnimatorClass(), "Animator");
	REQUIRE(store.SetParent(animator, rig));

	Animator driver;
	driver.Rig = store.CreateInstance(Classes::Find(Name("Part")), "NotARig");
	store.Set(animator, driver);

	CHECK(RigFor(store, animator) == rig);
}

TEST_CASE("a rig with nothing driving it answers nothing", "[scene][animation]") {
	RegisterSceneClasses();
	Store store("animation_test.none");

	const Entity rig = MakeRig(store, "animation_test.Fox", "Body");
	CHECK(AnimatorFor(store, rig) == NULL_ENTITY);

	const Entity part = store.CreateInstance(Classes::Find(Name("Part")), "Plain");
	CHECK(AnimatorFor(store, part) == NULL_ENTITY);
}

TEST_CASE("a clip authored for one rig refuses another", "[scene][animation]") {
	// The one refusal, stated once so that the handler and an editor's preview
	// cannot disagree about the permissive case.
	Skeleton fox;
	fox.Rig = Name("animation_test.Fox");

	Skeleton dragon;
	dragon.Rig = Name("animation_test.Dragon");

	AnimationClip walk;
	walk.Asset = Name("animation_test.Walk");
	walk.Rig = Name("animation_test.Fox");

	CHECK(ClipFitsRig(walk, fox));
	CHECK_FALSE(ClipFitsRig(walk, dragon));

	// A clip that names no rig plays anywhere, which is what an author gets
	// before anybody has said otherwise.
	AnimationClip anywhere;
	anywhere.Asset = Name("animation_test.Nod");
	CHECK(ClipFitsRig(anywhere, fox));
	CHECK(ClipFitsRig(anywhere, dragon));
}

TEST_CASE("a track is a row and survives a snapshot", "[scene][animation]") {
	// The whole reason a track is an instance rather than a userdata: playback
	// state saves and replicates with no machinery of its own.
	RegisterSceneClasses();
	Store source("animation_test.track.source");

	const Entity track = source.CreateInstance(Classes::Find(Name("AnimationTrack")), "Walk");

	AnimationTrack playing;
	playing.TimePosition = 1.25f;
	playing.Speed = 2.0f;
	playing.Weight = 0.5f;
	playing.Priority = AnimationPriority::Movement;
	playing.Looped = true;
	playing.Playing = true;
	source.Set(track, playing);

	engine::core::ByteWriter writer;
	REQUIRE(source.Save(writer));

	Store restored("animation_test.track.restored");
	engine::core::ByteReader reader(writer.Bytes());
	REQUIRE(restored.Load(reader));

	const AnimationTrack *back = restored.Get<AnimationTrack>(track);
	REQUIRE(back != nullptr);
	CHECK(back->TimePosition == 1.25f);
	CHECK(back->Speed == 2.0f);
	CHECK(back->Priority == AnimationPriority::Movement);
	CHECK(back->Looped);
	CHECK(back->Playing);
}

TEST_CASE("a clip's two names cross as text", "[scene][animation]") {
	// `AnimationClip` holds two `core::Name`s, so it is registered with a
	// hand-written pair. Interning a fresh string between the halves shifts every
	// id assigned afterwards, which is what makes this case prove anything.
	RegisterSceneClasses();
	Store source("animation_test.clip.source");

	const Entity clip = source.CreateInstance(Classes::Find(Name("Animation")), "Walk");
	source.Set(clip, AnimationClip{Name("animation_test.WalkAsset"), Name("animation_test.Fox")});

	engine::core::ByteWriter writer;
	REQUIRE(source.Save(writer));

	const Name shifter("animation_test.ShiftsTheIdSpace");
	CHECK(shifter.IsValid());

	Store restored("animation_test.clip.restored");
	engine::core::ByteReader reader(writer.Bytes());
	REQUIRE(restored.Load(reader));

	const AnimationClip *back = restored.Get<AnimationClip>(clip);
	REQUIRE(back != nullptr);
	CHECK(back->Asset.Text() == "animation_test.WalkAsset");
	CHECK(back->Rig.Text() == "animation_test.Fox");
}

TEST_CASE("playing tracks advance and fade on fixed world time", "[scene][animation]") {
	RegisterSceneClasses();
	Store store("animation_test.advance");
	store.AdvanceTick(0.1f);
	const Entity track = store.CreateInstance(Classes::Find(Name("AnimationTrack")), "Walk");
	AnimationTrack state;
	state.Playing = true;
	state.Speed = 2.0f;
	state.Weight = 0.0f;
	state.WeightTarget = 1.0f;
	state.FadeTime = 0.5f;
	store.Set(track, state);

	CHECK(engine::scene::AdvanceAnimationTracks(store) == 1);
	const AnimationTrack *advanced = store.Get<AnimationTrack>(track);
	REQUIRE(advanced != nullptr);
	CHECK(advanced->TimePosition == 0.2f);
	CHECK(advanced->Weight == 0.2f);
}
