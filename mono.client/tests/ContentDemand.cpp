// Which content a world asks for.
//
// **Two failures, and neither said anything.** The client asked for every
// texture in the catalogue, `render::TextureTable` spent its 512 MB ceiling in
// manifest order, and the remaining 1,463 uploads were refused - so a part
// naming a texture drew untextured and the only trace was a warning per refusal
// in a log nobody reads. And it asked for every mesh and material by kind, which
// - because the unit that travels is a *bundle* - asks for essentially every
// bundle in the store, decompressed synchronously on the calling thread. On this
// repository's own store that is 6.9 GB on the frame the editor opens, which is
// what "the studio freezes when I open it" was.
//
// So what is pinned here is the list: every place a texture can be named. A row
// missing from it is a whole class of asset that never loads while everything
// else does, which reads as that asset being broken rather than as a name
// nobody asked for.

#include <engine/ecs/Store.hpp>
#include <engine/effects/Particles.hpp>
#include <engine/effects/Registration.hpp>
#include <engine/effects/Ribbon.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Materials.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <client/ContentDemand.hpp>
#include <vector>

TEST_SUITE_ID("client.contentdemand")

using engine::core::Name;
using engine::ecs::Entity;
using engine::ecs::Store;

namespace {
	Store Fresh(const char *name) {
		engine::scene::RegisterSceneComponents();
		engine::gui::RegisterGuiComponents();
		engine::effects::RegisterEffectComponents();
		return Store(name);
	}

	bool Holds(const std::vector<Name> &names, const char *text) {
		const Name wanted(text);
		return std::find(names.begin(), names.end(), wanted) != names.end();
	}
}

TEST_CASE("every place content can be named is collected", "[client][contentdemand]") {
	Store store = Fresh("contentdemand.all");

	const Entity part = store.Create();
	store.Set(part, engine::scene::SurfaceAppearance{.ColourMap = Name("part.atex")});

	const Entity label = store.Create();
	engine::gui::Picture badge;
	badge.Image = Name("label.atex");
	store.Set(label, badge);

	const Entity emitter = store.Create();
	engine::effects::ParticleEmitter spark;
	spark.Texture = Name("spark.atex");
	store.Set(emitter, spark);

	const Entity beam = store.Create();
	engine::effects::Beam bolt;
	bolt.Texture = Name("bolt.atex");
	store.Set(beam, bolt);

	const Entity trail = store.Create();
	engine::effects::Trail swoosh;
	swoosh.Texture = Name("swoosh.atex");
	store.Set(trail, swoosh);

	// **The kinds that used to be fetched by kind**, which is what the freeze
	// was: a mesh and a material are named by a world exactly as a texture is,
	// and asking for all of them instead pulled the whole store.
	const Entity meshPart = store.Create();
	engine::scene::Visual visual;
	visual.Mesh = Name("model.amesh");
	store.Set(meshPart, visual);

	const Entity material = store.Create();
	store.Set(material, engine::scene::MaterialRef{.Asset = Name("oak.amat")});

	const Entity speaker = store.Create();
	engine::scene::Sound sound;
	sound.SoundId = Name("theme.mp3");
	store.Set(speaker, sound);

	std::vector<Name> wanted;
	client::CollectWantedContent(store, wanted);

	CHECK(Holds(wanted, "part.atex"));
	CHECK(Holds(wanted, "label.atex"));
	CHECK(Holds(wanted, "spark.atex"));
	CHECK(Holds(wanted, "bolt.atex"));
	CHECK(Holds(wanted, "swoosh.atex"));

	CHECK(Holds(wanted, "model.amesh"));
	CHECK(Holds(wanted, "oak.amat"));
	CHECK(Holds(wanted, "theme.mp3"));
}

TEST_CASE("a world names nothing it does not use", "[client][contentdemand]") {
	// **The property the whole change rests on.** A store of two thousand assets
	// and a scene that uses three must ask for three - anything else is the
	// by-kind fetch wearing a different hat, and the unit that travels is a
	// bundle.
	Store store = Fresh("contentdemand.bounded");

	for (int index = 0; index < 50; index++) {
		const Entity part = store.Create();
		store.Set(part, engine::scene::SurfaceAppearance{});
		store.Set(part, engine::scene::Visual{});
	}

	const Entity used = store.Create();
	engine::scene::Visual visual;
	visual.Mesh = Name("the_one.amesh");
	store.Set(used, visual);

	std::vector<Name> wanted;
	client::CollectWantedContent(store, wanted);

	// One name from fifty-one entities: the invalid ones are not asked for.
	CHECK(wanted.size() == 1);
	CHECK(Holds(wanted, "the_one.amesh"));
}

TEST_CASE("a world naming nothing asks for nothing", "[client][contentdemand]") {
	// The case that makes demand loading worth anything: a scene with no
	// content fetches no content, whatever the store holds.
	Store store = Fresh("contentdemand.empty");

	const Entity part = store.Create();
	store.Set(part, engine::scene::SurfaceAppearance{});

	std::vector<Name> wanted;
	client::CollectWantedContent(store, wanted);
	CHECK(wanted.empty());
}

TEST_CASE("a material's texture is collected once it has resolved", "[client][contentdemand]") {
	// **The indirection that let materials stay out of the demand path.** A
	// material reaches a part through `ResolveMaterials`, which writes its
	// texture into that part's `SurfaceAppearance::ColourMap` - a field this
	// already reads. So nothing here knows what a material is, and fetching a
	// material's sheet on arrival is what had to be *removed*: every material in
	// a catalogue arrives whether anything uses it or not, so doing it there was
	// requesting every texture by kind again, one indirection later.
	Store store = Fresh("contentdemand.material");
	engine::scene::RegisterSceneClasses();

	REQUIRE(
		engine::scene::RecordMaterial(
			store, Name("oak.amat"), engine::scene::MaterialMaps{.Colour = Name("oak_Color.atex")}
		)
	);

	const Entity part = store.CreateInstance(engine::ecs::Classes::Find(Name("Part")), "Crate");
	REQUIRE(part != engine::ecs::NULL_ENTITY);

	const Entity material = store.CreateInstance(engine::scene::MaterialClass(), "Material");
	REQUIRE(store.SetParent(material, part));
	store.GetMutable<engine::scene::MaterialRef>(material)->Asset = Name("oak.amat");

	std::vector<Name> before;
	client::CollectWantedContent(store, before);
	CHECK_FALSE(Holds(before, "oak_Color.atex"));

	REQUIRE(engine::scene::ResolveMaterials(store) == 1);

	std::vector<Name> after;
	client::CollectWantedContent(store, after);
	CHECK(Holds(after, "oak_Color.atex"));
}
