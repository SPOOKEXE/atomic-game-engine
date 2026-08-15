// What a world decides about how it is heard, and the one thing that can drift.
//
// `AudioState` is four fields and no behaviour, so most of this suite is about
// the two facts that are stated in two places and would agree until somebody
// edited one: the padding a snapshot writes, and the member names
// `ecs::EnumTable` registers against `Describe`. Both have a precedent in this
// module for going wrong silently - `scene/tests/Input.cpp` carries the six bytes
// that reached a save file uninitialised.

#include <engine/ecs/EnumTable.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Audio.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string>

TEST_SUITE_ID("engine.scene.audio")
TEST_DEPENDS("engine.scene.part")

using engine::core::Name;
using engine::ecs::EnumTable;
using engine::ecs::NULL_ENTITY;
using engine::ecs::Store;
using engine::scene::AudioState;
using engine::scene::Describe;
using engine::scene::ListenerMode;

// The invariant `scene/AGENTS.md` states: a component is serialised as its
// object representation, so a byte the compiler inserted and nobody declared
// reaches a save file uninitialised. A `static_assert` rather than a `CHECK`,
// because the failure is a fact about the type and should stop the build that
// introduced it.
static_assert(
	offsetof(AudioState, Reserved) + sizeof(AudioState::Reserved) == sizeof(AudioState),
	"AudioState::Reserved must reach the end of the object, or the bytes past it are unnamed "
	"padding that Column::Write puts in a save file uninitialised"
);

TEST_CASE("a world nobody has configured is as authored", "[scene][audio]") {
	// **The defaults are the contract**, because `SoundService`'s getters answer
	// from a default-constructed one for a world with no resource - a headless
	// server, and every world before a script says anything. A master volume of
	// anything but 1 here would quietly rescale every scene in the engine.
	const AudioState fresh;
	CHECK(fresh.MasterVolume == 1.0f);
	CHECK(fresh.Mode == ListenerMode::Camera);
	CHECK(fresh.Listener == NULL_ENTITY);
}

TEST_CASE("the listener modes are named in ordinal order", "[scene][audio]") {
	// **The names and the enum are two statements of one order**, and this is
	// the only thing that holds them together: `scene/Part.cpp` registers
	// `Enum.ListenerType` by walking `Describe`, and `SoundService:SetListener`
	// turns a member back into an ordinal and casts it. A name inserted in the
	// middle of one list and not the other makes `ObjectPosition` mean `Camera`
	// with nothing failing.
	CHECK(std::string(Describe(ListenerMode::Camera)) == "Camera");
	CHECK(std::string(Describe(ListenerMode::ObjectPosition)) == "ObjectPosition");

	// Out of range answers the first member rather than reading past the table.
	CHECK(std::string(Describe(ListenerMode::Count)) == "Camera");
}

TEST_CASE("Enum.ListenerType holds exactly what the engine can honour", "[scene][audio]") {
	engine::scene::EnsureClassTree();

	const Name set("ListenerType");
	REQUIRE(EnumTable::MembersOf(set).size() == static_cast<size_t>(ListenerMode::Count));

	for (size_t index = 0; index < static_cast<size_t>(ListenerMode::Count); index++) {
		const Name member = EnumTable::MemberAt(set, index);
		INFO(index);
		CHECK(member.Text() == Describe(static_cast<ListenerMode>(index)));

		size_t back = 0;
		REQUIRE(EnumTable::OrdinalOf(set, member, back));
		CHECK(back == index);
	}

	// **The two Roblox has and this does not**, asserted absent rather than
	// merely not listed. `scene/Audio.hpp` gives the reason - both place the ear
	// *and turn it*, and the mixer is posted a position with no facing - and a
	// member that resolved would be a setting a script could write and nothing
	// would honour.
	size_t ignored = 0;
	CHECK_FALSE(EnumTable::OrdinalOf(set, Name("CFrame"), ignored));
	CHECK_FALSE(EnumTable::OrdinalOf(set, Name("ObjectCFrame"), ignored));
}

TEST_CASE("the settings round-trip as a world resource", "[scene][audio]") {
	// **Registered, because a resource is keyed by a component id too** - one
	// that is never registered is minted by the first `SetResource` under the
	// compiler's spelling of the type, and aborts outright once the table is
	// sealed. That is the failure this case would catch.
	engine::scene::RegisterSceneComponents();
	Store store("audio_test");

	CHECK(store.Resource<AudioState>() == nullptr);

	AudioState wanted;
	wanted.MasterVolume = 0.25f;
	wanted.Mode = ListenerMode::ObjectPosition;
	store.SetResource(wanted);

	const AudioState *read = store.Resource<AudioState>();
	REQUIRE(read != nullptr);
	CHECK(read->MasterVolume == 0.25f);
	CHECK(read->Mode == ListenerMode::ObjectPosition);
}
