#include <engine/core/Name.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.scene.sound")
TEST_DEPENDS("engine.scene.part")

using Catch::Approx;
using engine::core::Name;
using engine::ecs::Classes;
using engine::ecs::ClassId;
using engine::ecs::Entity;
using engine::ecs::NULL_ENTITY;
using engine::ecs::Store;
using engine::scene::MakePart;
using engine::scene::PartDesc;
using engine::scene::RegisterSceneClasses;
using engine::scene::Sound;
using engine::scene::SoundClass;
using engine::scene::Transform;

namespace {
	template <class T> T Read(const Store &store, Entity instance, const char *property) {
		T value{};
		REQUIRE(store.GetProperty(instance, Name(property), &value, sizeof(T)));
		return value;
	}

	template <class T> bool Write(Store &store, Entity instance, const char *property, const T &value) {
		return store.SetProperty(instance, Name(property), &value, sizeof(T));
	}

	Entity NewSound(Store &store) {
		const Entity sound = store.CreateInstance(SoundClass());
		REQUIRE(sound != NULL_ENTITY);
		return sound;
	}
}

TEST_CASE("a sound group preserves imported sound hierarchy", "[scene][sound]") {
	RegisterSceneClasses();
	Store store("sound.group");

	const ClassId groupClass = Classes::Find(Name("SoundGroup"));
	REQUIRE(groupClass.IsValid());
	const Entity group = store.CreateInstance(groupClass, "Interface");
	REQUIRE(group != NULL_ENTITY);

	const Entity sound = NewSound(store);
	REQUIRE(store.SetParent(sound, group));
	CHECK(store.ParentOf(sound) == group);
}

TEST_CASE("a sound is an instance and not a PVInstance", "[scene][sound]") {
	RegisterSceneClasses();

	const ClassId sound = SoundClass();
	REQUIRE(sound.IsValid());

	CHECK(Classes::IsA(sound, Classes::Find(Name("Instance"))));

	// **The omission is the design.** A sound has no place of its own: where
	// it is heard from is its parent's. A `Transform` here would be a second
	// opinion about where a thing is, and it would make "attach a sound to a
	// thing" a field to keep in step with a parent that already says it.
	CHECK_FALSE(Classes::IsA(sound, Classes::Find(Name("PVInstance"))));
}

TEST_CASE("a fresh sound carries the component and nothing to play", "[scene][sound]") {
	Store store("sound_test.new");
	const Entity sound = NewSound(store);

	CHECK(store.Has<Sound>(sound));
	CHECK_FALSE(store.Has<Transform>(sound));

	const Sound *made = store.Get<Sound>(sound);
	REQUIRE(made != nullptr);

	// Nothing to play, and not playing. A class whose instances made a noise
	// the moment they existed would be one nobody could create quietly.
	CHECK_FALSE(made->SoundId.IsValid());
	CHECK_FALSE(made->Playing);
	CHECK_FALSE(made->Looped);
}

TEST_CASE("SoundId names a published asset", "[scene][sound]") {
	Store store("sound_test.id");
	const Entity sound = NewSound(store);

	// The manifest's spelling, extension included, exactly as `MeshId` carries
	// a mesh - the lookup is a string compare and a spelling is the one place
	// the two ends could diverge.
	const Name track("audio/lilium-lainu.mp3");
	REQUIRE(Write(store, sound, "SoundId", track));
	CHECK(Read<Name>(store, sound, "SoundId") == track);
	CHECK(store.Get<Sound>(sound)->SoundId == track);
}

TEST_CASE("Playing is a property, because a method would be on every part", "[scene][sound]") {
	Store store("sound_test.playing");
	const Entity sound = NewSound(store);

	REQUIRE(Write(store, sound, "Playing", true));
	CHECK(store.Get<Sound>(sound)->Playing);

	REQUIRE(Write(store, sound, "Looped", true));
	CHECK(store.Get<Sound>(sound)->Looped);

	REQUIRE(Write(store, sound, "Playing", false));
	CHECK_FALSE(store.Get<Sound>(sound)->Playing);
}

TEST_CASE("Volume clamps at ten rather than at one", "[scene][sound]") {
	Store store("sound_test.volume");
	const Entity sound = NewSound(store);

	// Over full scale is legal and is clamped once at the device - `Sample.hpp`
	// keeps floats precisely so the graph does not attenuate defensively at
	// every stage. Refusing above 1 would make a quietly authored sound
	// impossible to bring up.
	REQUIRE(Write(store, sound, "Volume", 4.0f));
	CHECK(Read<float>(store, sound, "Volume") == Approx(4.0f));

	// Clamped rather than refused, which is the right answer for a continuous
	// quantity: a fade driven off a curve overshoots by a hair at both ends.
	REQUIRE(Write(store, sound, "Volume", 25.0f));
	CHECK(Read<float>(store, sound, "Volume") == Approx(10.0f));

	REQUIRE(Write(store, sound, "Volume", -3.0f));
	CHECK(Read<float>(store, sound, "Volume") == Approx(0.0f));
}

TEST_CASE("the roll-off distances are the sound's own", "[scene][sound]") {
	Store store("sound_test.rolloff");
	const Entity sound = NewSound(store);

	REQUIRE(Write(store, sound, "RollOffMinDistance", 5.0f));
	REQUIRE(Write(store, sound, "RollOffMaxDistance", 90.0f));

	CHECK(Read<float>(store, sound, "RollOffMinDistance") == Approx(5.0f));
	CHECK(Read<float>(store, sound, "RollOffMaxDistance") == Approx(90.0f));
}

TEST_CASE("a sound is heard from whatever it is parented to", "[scene][sound]") {
	// The whole of the positional rule, and it is a hierarchy fact rather than
	// a field: a sound inside a part is that part's, and one under a service
	// is heard everywhere. Nothing here plays anything - the client walks these
	// rows - but the shape a walker reads has to be this one.
	Store store("sound_test.parent");

	const Entity part = MakePart(store, PartDesc{});
	REQUIRE(part != NULL_ENTITY);

	const Entity attached = NewSound(store);
	REQUIRE(store.SetParent(attached, part));

	CHECK(store.ParentOf(attached) == part);
	CHECK(store.Has<Transform>(store.ParentOf(attached)));

	// And one with no parent that has a place in the world is the global case,
	// which is what a script parenting to `Workspace` produces.
	const Entity global = NewSound(store);
	CHECK_FALSE(store.Has<Transform>(store.ParentOf(global)));
}
