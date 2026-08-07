// What a world knows about the textures its emitters name.
//
// **The failure this exists to prevent is silent and looks like nothing.** A
// flipbook whose frame count nobody carried plays some prefix of its animation
// and stops — which is indistinguishable, on screen, from a shorter animation.
// `fox_dance.gif` has forty-eight frames and the scene that used it said
// twenty-four, so half the dance never played and no test, log line or warning
// said a word.
//
// So what is pinned here is the shape that makes the number travel: a texture
// is read once, its facts land in the world, and anything asking gets the same
// answer whether or not there is a graphics device in the process.

#include <engine/ecs/Store.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/TextureCatalogue.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.scene.texturecatalogue")

using engine::core::Name;
using engine::ecs::Store;
using engine::scene::FlipbookFacts;
using engine::scene::FlipbookOf;
using engine::scene::RecordTexture;
using engine::scene::TextureCatalogue;
using engine::scene::TexturesOf;

namespace {
	// **Registered before the store exists**, for `MeshCatalogue`'s reason in
	// full: `TexturesOf` sets a resource, `SetResource` keys on a component id,
	// and an id minted before the explicit registration lands takes the
	// compiler's spelling of the type — which aborts the process the moment
	// `RegisterSceneComponents` gets there, at a call site with nothing to do
	// with this one. It is order-dependent, so a suite that gets it wrong
	// passes most runs.
	Store Fresh(const char *name) {
		engine::scene::RegisterSceneComponents();
		return Store(name);
	}

	FlipbookFacts Fox() {
		return FlipbookFacts{.Side = 8, .Frames = 48, .FrameRate = 24.0f};
	}
}

TEST_CASE("a recorded texture reads back", "[scene][texturecatalogue]") {
	Store store = Fresh("texturecatalogue");
	const Name texture("effects/fox_dance.atex");

	REQUIRE(RecordTexture(store, texture, Fox()));

	const FlipbookFacts facts = FlipbookOf(store, texture);
	CHECK(facts.Side == 8);
	CHECK(facts.Frames == 48);
	CHECK(facts.FrameRate == 24.0f);
	CHECK(facts.IsFlipbook());
}

TEST_CASE("an unknown texture answers zeroes rather than guessing", "[scene][texturecatalogue]") {
	Store store = Fresh("texturecatalogue.unknown");

	// **A still image and an unregistered name give the same answer on
	// purpose.** Neither is something to play, and a consumer that had to tell
	// them apart would be asking a question with no use.
	const FlipbookFacts facts = FlipbookOf(store, Name("nothing/here.atex"));
	CHECK(facts.Side == 0);
	CHECK(facts.Frames == 0);
	CHECK_FALSE(facts.IsFlipbook());

	// An invalid name is the same answer rather than a crash.
	CHECK_FALSE(FlipbookOf(store, Name()).IsFlipbook());
}

TEST_CASE("the reader never creates the resource", "[scene][texturecatalogue]") {
	// **This is what a system's refresh pass calls**, and a read that acquired
	// the resource would be a structural change from inside iteration — on the
	// first frame, on every emitter in the scene.
	Store store = Fresh("texturecatalogue.readonly");

	CHECK(store.Resource<TextureCatalogue>() == nullptr);
	CHECK_FALSE(FlipbookOf(store, Name("effects/fox_dance.atex")).IsFlipbook());
	CHECK(store.Resource<TextureCatalogue>() == nullptr);

	// The mutable accessor is the one that creates it.
	TexturesOf(store);
	CHECK(store.Resource<TextureCatalogue>() != nullptr);
}

TEST_CASE("re-recording a texture replaces what was there", "[scene][texturecatalogue]") {
	// **Last writer wins, because that is what the content path does.** A
	// publisher may replace a texture under a name it already used, and a
	// catalogue that refused the second one would keep answering with the sheet
	// that is no longer drawn.
	Store store = Fresh("texturecatalogue.replace");
	const Name texture("effects/fox_dance.atex");

	REQUIRE(RecordTexture(store, texture, Fox()));
	REQUIRE(RecordTexture(store, texture, FlipbookFacts{.Side = 4, .Frames = 12, .FrameRate = 30.0f}));

	const FlipbookFacts facts = FlipbookOf(store, texture);
	CHECK(facts.Side == 4);
	CHECK(facts.Frames == 12);
	CHECK(facts.FrameRate == 30.0f);
}

TEST_CASE("an invalid name records nothing", "[scene][texturecatalogue]") {
	Store store = Fresh("texturecatalogue.invalid");
	CHECK_FALSE(RecordTexture(store, Name(), Fox()));
	CHECK(TexturesOf(store).Flipbooks.empty());
}

TEST_CASE("the catalogue is derived and does not persist", "[scene][texturecatalogue]") {
	// **Registered with a writer that stores nothing**, the same as the mesh
	// one: the contents came from whatever registered the textures this run, and
	// a save file carrying last run's frame counts would be numbers that agree
	// with nothing on disk. A world reloaded with no content attached honestly
	// knows nothing, which is the answer it also gives before content arrives.
	Store store = Fresh("texturecatalogue.derived");
	REQUIRE(RecordTexture(store, Name("effects/fox_dance.atex"), Fox()));

	TextureCatalogue copy = TexturesOf(store);
	REQUIRE(copy.Flipbooks.size() == 1);

	// What the registered reader does to a fresh instance.
	copy.Flipbooks.clear();
	CHECK(copy.Find(Name("effects/fox_dance.atex")).Side == 0);
}
