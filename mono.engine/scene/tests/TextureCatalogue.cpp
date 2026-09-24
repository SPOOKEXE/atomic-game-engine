// What a world knows about the textures its emitters name.
//
// **The failure this exists to prevent is silent and looks like nothing.** A
// flipbook whose frame count nobody carried plays some prefix of its animation
// and stops - which is indistinguishable, on screen, from a shorter animation.
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

#include <catch2/catch_approx.hpp>
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
	// compiler's spelling of the type - which aborts the process the moment
	// `RegisterSceneComponents` gets there, at a call site with nothing to do
	// with this one. It is order-dependent, so a suite that gets it wrong
	// passes most runs.
	Store Fresh(const char *name) {
		engine::scene::RegisterSceneComponents();
		return Store(name);
	}

	FlipbookFacts Fox() {
		return FlipbookFacts{
			.Side = 8, .Frames = 48, .FrameRate = 24.0f, .FrameDurations = {}, .CumulativeEnds = {}
		};
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

TEST_CASE("a 16 by 16 texture keeps all 256 frame facts", "[scene][texturecatalogue]") {
	Store store = Fresh("texturecatalogue.large");
	const Name texture("effects/large.atex");
	REQUIRE(RecordTexture(
		store,
		texture,
		FlipbookFacts{
			.Side = 16, .Frames = 256, .FrameRate = 30.0f, .FrameDurations = {}, .CumulativeEnds = {}
		}
	));
	const FlipbookFacts facts = FlipbookOf(store, texture);
	CHECK(facts.Side == 16);
	CHECK(facts.Frames == 256);
	CHECK(facts.FrameRate == 30.0f);
}

TEST_CASE("variable frame durations replace prior texture timing", "[scene][texturecatalogue]") {
	Store store = Fresh("texturecatalogue.variable");
	const Name texture("effects/variable.atex");
	REQUIRE(RecordTexture(
		store,
		texture,
		FlipbookFacts{.Side = 2, .Frames = 3, .FrameDurations = {0.04f, 0.10f, 0.06f}, .CumulativeEnds = {}}
	));
	const uint64_t recorded = TexturesOf(store).Revision;
	CHECK(FlipbookOf(store, texture).FrameDurations == std::vector<float>{0.04f, 0.10f, 0.06f});
	CHECK(FlipbookOf(store, texture).CumulativeEnds == std::vector<float>{0.04f, 0.14f, 0.20f});
	REQUIRE(RecordTexture(
		store,
		texture,
		FlipbookFacts{.Side = 2, .Frames = 3, .FrameRate = 12.0f, .FrameDurations = {}, .CumulativeEnds = {}}
	));
	CHECK(TexturesOf(store).Revision > recorded);
	CHECK(FlipbookOf(store, texture).FrameDurations.empty());
	CHECK(FlipbookOf(store, texture).CumulativeEnds.empty());
}

TEST_CASE(
	"a 257-frame sequence records exact ordered timing without an atlas side", "[scene][texturecatalogue]"
) {
	Store store = Fresh("texturecatalogue.sequence");
	const Name texture("effects/long.aseq");
	FlipbookFacts authored;
	authored.Frames = 257;
	for (uint32_t frame = 0; frame < 257; ++frame)
		authored.FrameDurations.push_back((frame & 1) == 0 ? 0.04f : 0.11f);
	REQUIRE(RecordTexture(store, texture, authored));
	const FlipbookFacts recorded = FlipbookOf(store, texture);
	CHECK(recorded.IsFlipbook());
	CHECK(recorded.Side == 0);
	CHECK(recorded.Frames == 257);
	REQUIRE(recorded.CumulativeEnds.size() == 257);
	CHECK(recorded.CumulativeEnds[0] == Catch::Approx(0.04f));
	CHECK(recorded.CumulativeEnds[1] == Catch::Approx(0.15f));
	CHECK(recorded.TotalDuration == Catch::Approx(recorded.CumulativeEnds.back()));
	const uint64_t revision = TexturesOf(store).Revision;
	authored.FrameDurations.pop_back();
	CHECK_FALSE(RecordTexture(store, texture, authored));
	CHECK(TexturesOf(store).Revision == revision);
	CHECK(FlipbookOf(store, texture).Frames == 257);
}

TEST_CASE("invalid variable timing leaves the recorded texture unchanged", "[scene][texturecatalogue]") {
	Store store = Fresh("texturecatalogue.invalid");
	const Name texture("effects/variable.atex");
	REQUIRE(RecordTexture(
		store,
		texture,
		FlipbookFacts{.Side = 2, .Frames = 3, .FrameDurations = {0.04f, 0.10f, 0.06f}, .CumulativeEnds = {}}
	));
	const uint64_t revision = TexturesOf(store).Revision;
	CHECK_FALSE(RecordTexture(
		store,
		texture,
		FlipbookFacts{.Side = 2, .Frames = 3, .FrameDurations = {0.04f, 0.0f, 0.06f}, .CumulativeEnds = {}}
	));
	CHECK(TexturesOf(store).Revision == revision);
	CHECK(FlipbookOf(store, texture).FrameDurations[1] == 0.10f);
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
	// the resource would be a structural change from inside iteration - on the
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
	REQUIRE(RecordTexture(
		store,
		texture,
		FlipbookFacts{.Side = 4, .Frames = 12, .FrameRate = 30.0f, .FrameDurations = {}, .CumulativeEnds = {}}
	));

	const FlipbookFacts facts = FlipbookOf(store, texture);
	CHECK(facts.Side == 4);
	CHECK(facts.Frames == 12);
	CHECK(facts.FrameRate == 30.0f);
	CHECK(TexturesOf(store).Revision == 2);
}

TEST_CASE("an invalid name records nothing", "[scene][texturecatalogue]") {
	Store store = Fresh("texturecatalogue.invalid");
	CHECK_FALSE(RecordTexture(store, Name(), Fox()));
	CHECK(TexturesOf(store).Flipbooks.empty());
	CHECK(TexturesOf(store).Revision == 0);
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
	copy.Revision = 0;
	CHECK(copy.Find(Name("effects/fox_dance.atex")).Side == 0);
}
