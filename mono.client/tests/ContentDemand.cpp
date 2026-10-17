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
#include <engine/scene/Animation.hpp>
#include <engine/scene/Atmosphere.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/ImageGraph.hpp>
#include <engine/scene/LevelOfDetail.hpp>
#include <engine/scene/Materials.hpp>
#include <engine/scene/MeshCatalogue.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
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
	store.Set(
		part,
		engine::scene::SurfaceAppearance{
			.ColourMap = Name("part.atex"),
			.NormalMap = Name("part-normal.atex"),
			.RoughnessMap = Name("part-roughness.atex"),
			.OcclusionMap = Name("part-occlusion.atex"),
			.HeightMap = Name("part-height.atex"),
			.MetalnessMap = Name("part-metalness.atex"),
			.EmissiveMap = Name("part-emissive.atex"),
		}
	);

	const Entity label = store.Create();
	engine::gui::Picture badge;
	badge.Image = Name("label.atex");
	badge.HoverImage = Name("label-hover.atex");
	badge.PressedImage = Name("label-pressed.atex");
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

	const Entity decal = store.Create();
	engine::effects::Decal sign;
	sign.Image = Name("sign.atex");
	store.Set(decal, sign);

	const Entity texture = store.Create();
	engine::effects::Texture tiles;
	tiles.Image = Name("tiles.atex");
	store.Set(texture, tiles);

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

	const Entity animation = store.Create();
	store.Set(animation, engine::scene::AnimationClip{Name("walk.aanim"), {}});

	std::vector<Name> wanted;
	client::CollectWantedContent(store, wanted);

	CHECK(Holds(wanted, "part.atex"));
	CHECK(Holds(wanted, "part-normal.atex"));
	CHECK(Holds(wanted, "part-roughness.atex"));
	CHECK(Holds(wanted, "part-occlusion.atex"));
	CHECK(Holds(wanted, "part-height.atex"));
	CHECK(Holds(wanted, "part-metalness.atex"));
	CHECK(Holds(wanted, "part-emissive.atex"));
	CHECK(Holds(wanted, "label.atex"));
	CHECK(Holds(wanted, "label-hover.atex"));
	CHECK(Holds(wanted, "label-pressed.atex"));
	CHECK(Holds(wanted, "spark.atex"));
	CHECK(Holds(wanted, "bolt.atex"));
	CHECK(Holds(wanted, "swoosh.atex"));
	CHECK(Holds(wanted, "sign.atex"));
	CHECK(Holds(wanted, "tiles.atex"));

	CHECK(Holds(wanted, "model.amesh"));
	CHECK(Holds(wanted, "oak.amat"));
	CHECK(Holds(wanted, "theme.mp3"));
	CHECK(Holds(wanted, "walk.aanim"));
}

TEST_CASE(
	"scrollbar and LOD image references join demand and change its revision", "[client][contentdemand]"
) {
	Store store = Fresh("contentdemand.additional.images");
	const auto scroll = store.Create();
	engine::gui::Scrolling scrolling;
	scrolling.TopImage = Name("imagegraph://scroll.aimagegraph#top");
	scrolling.MidImage = Name("imagegraph://scroll.aimagegraph#middle");
	scrolling.BottomImage = Name("imagegraph://scroll.aimagegraph#bottom");
	store.Set(scroll, scrolling);
	const auto automatic = store.Create();
	engine::scene::LODAuto generated;
	generated.Billboard = Name("imagegraph://lod.aimagegraph#automatic");
	store.Set(automatic, generated);
	const auto custom = store.Create();
	engine::scene::LODCustom authored;
	authored.Billboard = Name("imagegraph://lod.aimagegraph#custom");
	store.Set(custom, authored);
	std::vector<Name> wanted;
	client::CollectWantedContent(store, wanted);
	CHECK(wanted.size() == 5);
	CHECK(Holds(wanted, "imagegraph://scroll.aimagegraph#top"));
	CHECK(Holds(wanted, "imagegraph://scroll.aimagegraph#middle"));
	CHECK(Holds(wanted, "imagegraph://scroll.aimagegraph#bottom"));
	CHECK(Holds(wanted, "imagegraph://lod.aimagegraph#automatic"));
	CHECK(Holds(wanted, "imagegraph://lod.aimagegraph#custom"));
	const auto before = client::WantedContentRevision(store);
	store.GetMutable<engine::gui::Scrolling>(scroll)->MidImage = Name("replacement.atex");
	const auto changed = client::WantedContentRevision(store);
	CHECK(changed != before);
	store.GetMutable<engine::scene::LODCustom>(custom)->Billboard = Name("replacement-lod.atex");
	CHECK(client::WantedContentRevision(store) != changed);
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

TEST_CASE("unchanged demand revisions ignore simulation and unrelated writes", "[client][contentdemand]") {
	Store store = Fresh("contentdemand.revision.steady");
	const Entity emitter = store.Create();
	engine::effects::ParticleEmitter particles;
	particles.Texture = Name("shared.atex");
	store.Set(emitter, particles);

	const uint64_t before = client::WantedContentRevision(store);
	store.AdvanceTick(1.0 / 60.0);
	const Entity unrelated = store.Create();
	store.Set(unrelated, engine::scene::Transform{});

	CHECK(client::WantedContentRevision(store) == before);
}

TEST_CASE("content property changes advance the precise demand revision", "[client][contentdemand]") {
	Store store = Fresh("contentdemand.revision.changed");
	const Entity emitter = store.Create();
	store.Set(emitter, engine::effects::ParticleEmitter{});
	const uint64_t before = client::WantedContentRevision(store);

	store.GetMutable<engine::effects::ParticleEmitter>(emitter)->Texture = Name("changed.atex");

	CHECK(client::WantedContentRevision(store) != before);
}

TEST_CASE("ImageButton state image changes advance content demand", "[client][contentdemand]") {
	Store store = Fresh("contentdemand.revision.button");
	const Entity button = store.Create();
	store.Set(button, engine::gui::Picture{});
	const uint64_t before = client::WantedContentRevision(store);
	store.GetMutable<engine::gui::Picture>(button)->HoverImage = Name("hover.atex");
	const uint64_t hovered = client::WantedContentRevision(store);
	CHECK(hovered != before);
	store.GetMutable<engine::gui::Picture>(button)->PressedImage = Name("pressed.atex");
	CHECK(client::WantedContentRevision(store) != hovered);
}

TEST_CASE("collecting demand does not dirty the references it reads", "[client][contentdemand]") {
	Store store = Fresh("contentdemand.revision.readonly");
	const Entity part = store.Create();
	engine::scene::Visual visual;
	visual.Mesh = Name("readonly.amesh");
	store.Set(part, visual);
	const uint64_t before = client::WantedContentRevision(store);

	std::vector<Name> wanted;
	client::CollectWantedContent(store, wanted);

	CHECK(client::WantedContentRevision(store) == before);
	REQUIRE(wanted.size() == 1);
}

TEST_CASE("removing a content-bearing row advances demand revision", "[client][contentdemand]") {
	Store store = Fresh("contentdemand.revision.removed");
	const Entity emitter = store.Create();
	store.Set(emitter, engine::effects::ParticleEmitter{});
	const uint64_t before = client::WantedContentRevision(store);

	store.Remove<engine::effects::ParticleEmitter>(emitter);

	CHECK(client::WantedContentRevision(store) != before);
}

TEST_CASE("demand revisions cover destruction and equal-count replacement", "[client][contentdemand]") {
	Store store = Fresh("contentdemand.revision.membership");
	const Entity first = store.Create();
	engine::effects::ParticleEmitter settings;
	settings.Texture = Name("first.atex");
	store.Set(first, settings);
	const uint64_t initial = client::WantedContentRevision(store);
	const uint64_t writes = store.ComponentChangeVersion<engine::effects::ParticleEmitter>();
	store.Destroy(first);
	store.ClearChanges();
	CHECK(store.ComponentChangeVersion<engine::effects::ParticleEmitter>() == writes);
	CHECK(client::WantedContentRevision(store) != initial);
	std::vector<Name> wanted;
	client::CollectWantedContent(store, wanted);
	CHECK(wanted.empty());

	const Entity second = store.Create();
	store.Set(second, settings);
	const uint64_t restored = client::WantedContentRevision(store);
	const auto count = store.CountMatching<engine::effects::ParticleEmitter>();
	const Entity replacement = store.Create();
	settings.Texture = Name("replacement.atex");
	store.Remove<engine::effects::ParticleEmitter>(second);
	store.Set(replacement, settings);
	store.ClearChanges();
	REQUIRE(store.CountMatching<engine::effects::ParticleEmitter>() == count);
	CHECK(client::WantedContentRevision(store) != restored);
	client::CollectWantedContent(store, wanted);
	REQUIRE(wanted.size() == 1);
	CHECK(Holds(wanted, "replacement.atex"));
	CHECK_FALSE(Holds(wanted, "first.atex"));
	const uint64_t settled = client::WantedContentRevision(store);
	store.ClearChanges();
	CHECK(client::WantedContentRevision(store) == settled);
}

TEST_CASE("many emitters share one demanded texture name", "[client][contentdemand]") {
	Store store = Fresh("contentdemand.shared.texture");
	for (size_t index = 0; index < 4096; index++) {
		const Entity emitter = store.Create();
		engine::effects::ParticleEmitter particles;
		particles.Texture = Name("shared.atex");
		store.Set(emitter, particles);
	}

	std::vector<Name> wanted;
	client::CollectWantedContent(store, wanted);

	REQUIRE(wanted.size() == 1);
	CHECK(wanted.front() == Name("shared.atex"));
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

TEST_CASE("a selected skybox asks only for faces it names", "[client][contentdemand][skybox]") {
	Store store = Fresh("contentdemand.skybox");
	engine::scene::RegisterSceneClasses();
	engine::scene::InstallServices(store);
	const Entity lighting = store.FindFirstRoot("Lighting");
	REQUIRE(lighting != engine::ecs::NULL_ENTITY);

	const Entity selected =
		store.CreateInstance(engine::ecs::Classes::Find(Name("SkyboxTextures")), "Selected");
	REQUIRE(store.SetParent(selected, lighting));
	auto *faces = store.GetMutable<engine::scene::SkyboxTextures>(selected);
	REQUIRE(faces != nullptr);
	faces->Front = Name("sky/front.atex");
	faces->Up = Name("sky/up.atex");

	const Entity ignored =
		store.CreateInstance(engine::ecs::Classes::Find(Name("SkyboxTextures")), "Ignored");
	REQUIRE(store.SetParent(ignored, lighting));
	store.GetMutable<engine::scene::SkyboxTextures>(ignored)->Front = Name("sky/ignored.atex");

	std::vector<Name> wanted;
	client::CollectWantedContent(store, wanted);
	CHECK(wanted.size() == 2);
	CHECK(Holds(wanted, "sky/front.atex"));
	CHECK(Holds(wanted, "sky/up.atex"));
	CHECK_FALSE(Holds(wanted, "sky/ignored.atex"));
}

TEST_CASE(
	"cooked material shader names join content demand without fetching builtins", "[client][contentdemand]"
) {
	Store store = Fresh("contentdemand.cooked-shaders");
	const auto material = store.Create();
	store.Set(material, engine::scene::MaterialRef{.Asset = {}, .Shader = Name("material.ashader")});
	const auto builtin = store.Create();
	store.Set(builtin, engine::scene::MaterialRef{.Asset = {}, .Shader = Name("unlit")});
	std::vector<Name> wanted;
	client::CollectWantedContent(store, wanted);
	REQUIRE(wanted.size() == 1);
	CHECK(Holds(wanted, "material.ashader"));
	CHECK_FALSE(Holds(wanted, "unlit"));
	const auto revision = client::WantedContentRevision(store);
	store.GetMutable<engine::scene::MaterialRef>(material)->Shader = Name("replacement.ashader");
	CHECK(client::WantedContentRevision(store) != revision);
	wanted.clear();
	client::CollectWantedContent(store, wanted);
	CHECK(Holds(wanted, "replacement.ashader"));
	CHECK_FALSE(Holds(wanted, "material.ashader"));
}

TEST_CASE("mesh submesh image references survive reconstruction of demand", "[client][contentdemand]") {
	Store store = Fresh("contentdemand.mesh.images");
	const auto entity = store.Create();
	engine::scene::Visual visual;
	visual.Mesh = Name("mesh.amesh");
	store.Set(entity, visual);
	const Name sheet("imagegraph://material.aimagegraph#image");
	REQUIRE(engine::scene::RecordMesh(store, visual.Mesh, 2, std::span(&sheet, 1)));
	std::vector<Name> wanted;
	client::CollectWantedContent(store, wanted);
	CHECK(Holds(wanted, "imagegraph://material.aimagegraph#image"));
	wanted.clear();
	client::CollectWantedContent(store, wanted);
	CHECK(Holds(wanted, "imagegraph://material.aimagegraph#image"));
}

TEST_CASE(
	"image graph instance source assets and wire input replacements invalidate demand",
	"[client][contentdemand]"
) {
	Store store = Fresh("contentdemand.graph.instance");
	const auto entity = store.Create();
	engine::scene::ImageGraph graph;
	graph.InstanceKey = Name("instance");
	graph.Graph = Name("live.aimagegraph");
	store.Set(entity, graph);
	std::vector<Name> wanted;
	client::CollectWantedContent(store, wanted);
	CHECK(Holds(wanted, "live.aimagegraph"));
	const auto before = client::WantedContentRevision(store);
	graph.Inputs.push_back(
		{.Name = Name("opacity"),
		 .Kind = engine::scene::ImageGraphInputKind::Number,
		 .Number = 0.25,
		 .Boolean = false,
		 .Colour = {},
		 .String = {}}
	);
	store.Set(entity, graph);
	CHECK(graph.Revision == 0);
	CHECK(client::WantedContentRevision(store) != before);
}
