// What a part is made of, once a material is content rather than a word.
//
// **The failure this exists to prevent is the one the enum had for four
// versions and nothing reported.** `part.Material = "Wood"` was accepted,
// round-tripped, saved and replicated, and no renderer sampled anything
// different for it - a property that looked like it worked, on the most obvious
// control in the properties panel. So what is pinned here is the whole of what
// replaces it: an instance names an asset, a catalogue says what that asset
// resolves to, and one pass puts the answer where the draw path already looks.

#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Materials.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.scene.materials")

using engine::core::Name;
using engine::ecs::Entity;
using engine::ecs::NULL_ENTITY;
using engine::ecs::Store;
using engine::scene::ColourMapOf;
using engine::scene::MaterialClass;
using engine::scene::MaterialMaps;
using engine::scene::MaterialRef;
using engine::scene::RecordMaterial;
using engine::scene::ResolveMaterials;
using engine::scene::SurfaceAppearance;

namespace {
	// **Registered before the store exists**, for `TextureCatalogue`'s reason in
	// full: `MaterialsOf` sets a resource, `SetResource` keys on a component id,
	// and an id minted before the explicit registration lands takes the
	// compiler's spelling of the type.
	Store Fresh(const char *name) {
		engine::scene::RegisterSceneComponents();
		engine::scene::RegisterSceneClasses();
		return Store(name);
	}

	// A part with a `Material` child naming `asset`, and the child's entity.
	Entity Dress(Store &store, Entity part, const Name &asset) {
		const Entity material = store.CreateInstance(MaterialClass(), "Material");
		REQUIRE(material != NULL_ENTITY);
		REQUIRE(store.SetParent(material, part));

		MaterialRef *ref = store.GetMutable<MaterialRef>(material);
		REQUIRE(ref != nullptr);
		ref->Asset = asset;
		return material;
	}
}

TEST_CASE("the material class is an instance rather than a part", "[scene][materials]") {
	Fresh("materials.class");

	const engine::ecs::ClassId material = MaterialClass();
	REQUIRE(material.IsValid());

	// **Not a `PVInstance`, for `Attachment`'s reason.** A `PVInstance` carries
	// a `Transform` - a world-space placement - and a material has no place of
	// its own. Two of those on one row is two opinions about where a thing is.
	CHECK(engine::ecs::Classes::IsA(material, engine::ecs::Classes::Find(Name("Instance"))));
	CHECK_FALSE(engine::ecs::Classes::IsA(material, engine::ecs::Classes::Find(Name("PVInstance"))));
}

TEST_CASE("a recorded material resolves to its colour map", "[scene][materials]") {
	Store store = Fresh("materials.catalogue");

	const Name material("materials/ambientcg/Bricks075A.amat");
	const Name colour("materials/ambientcg/Bricks075A_Color.atex");

	REQUIRE(RecordMaterial(store, material, MaterialMaps{.Colour = colour}));
	CHECK(ColourMapOf(store, material).Colour == colour);
}

TEST_CASE("every PBR map resolves onto the surface appearance", "[scene][materials]") {
	Store store = Fresh("materials.pbr");

	const Name asset("materials/pbr.amat");
	const MaterialMaps maps{
		.Colour = Name("pbr-colour"),
		.Normal = Name("pbr-normal"),
		.Roughness = Name("pbr-roughness"),
		.Occlusion = Name("pbr-occlusion"),
		.Height = Name("pbr-height"),
		.Metalness = Name("pbr-metalness"),
		.Emissive = Name("pbr-emissive"),
	};
	REQUIRE(RecordMaterial(store, asset, maps));

	const Entity part = store.CreateInstance(engine::ecs::Classes::Find(Name("Part")), "PBR");
	REQUIRE(part != NULL_ENTITY);
	Dress(store, part, asset);
	REQUIRE(ResolveMaterials(store) == 1);

	const SurfaceAppearance *appearance = store.Get<SurfaceAppearance>(part);
	REQUIRE(appearance != nullptr);
	CHECK(appearance->ColourMap == maps.Colour);
	CHECK(appearance->NormalMap == maps.Normal);
	CHECK(appearance->RoughnessMap == maps.Roughness);
	CHECK(appearance->OcclusionMap == maps.Occlusion);
	CHECK(appearance->HeightMap == maps.Height);
	CHECK(appearance->MetalnessMap == maps.Metalness);
	CHECK(appearance->EmissiveMap == maps.Emissive);

	CHECK(MaterialMaps{.Emissive = maps.Emissive}.IsValid());
	CHECK(MaterialMaps{.Metalness = maps.Metalness}.IsValid());
}

TEST_CASE("a material nobody recorded resolves to nothing", "[scene][materials]") {
	Store store = Fresh("materials.unknown");

	// **An unknown material and an untextured one give the same answer on
	// purpose.** Neither is something to sample, and the renderer draws its own
	// default either way - a consumer that had to tell them apart would be
	// asking a question with no use.
	CHECK_FALSE(ColourMapOf(store, Name("materials/nothing.amat")).IsValid());

	REQUIRE(RecordMaterial(store, Name("materials/blank.amat"), MaterialMaps{.Colour = Name()}));
	CHECK_FALSE(ColourMapOf(store, Name("materials/blank.amat")).IsValid());
}

TEST_CASE("resolving writes the texture onto the parent part", "[scene][materials]") {
	Store store = Fresh("materials.resolve");

	const Name asset("materials/oak.amat");
	const Name colour("materials/oak_Color.atex");
	REQUIRE(RecordMaterial(store, asset, MaterialMaps{.Colour = colour}));

	const Entity part = store.CreateInstance(engine::ecs::Classes::Find(Name("Part")), "Crate");
	REQUIRE(part != NULL_ENTITY);
	Dress(store, part, asset);

	CHECK(ResolveMaterials(store) == 1);

	// **`SurfaceAppearance::ColourMap` and not a field of its own**, because
	// that is what the draw-list pass already reads - `engine::render::CollectInstances`
	// is a batched parallel loop over a fixed signature and cannot follow a
	// child. `scene/Materials.hpp` carries the argument.
	const SurfaceAppearance *appearance = store.Get<SurfaceAppearance>(part);
	REQUIRE(appearance != nullptr);
	CHECK(appearance->ColourMap == colour);
}

TEST_CASE("a material set back to none clears the part", "[scene][materials]") {
	Store store = Fresh("materials.none");

	const Name asset("materials/oak.amat");
	REQUIRE(RecordMaterial(store, asset, MaterialMaps{.Colour = Name("materials/oak_Color.atex")}));

	const Entity part = store.CreateInstance(engine::ecs::Classes::Find(Name("Part")), "Crate");
	const Entity material = Dress(store, part, asset);

	REQUIRE(ResolveMaterials(store) == 1);
	REQUIRE(store.Get<SurfaceAppearance>(part)->ColourMap.IsValid());

	// **The case that makes `None` mean something.** The pass writes even when
	// it resolves to nothing, so setting a material back to none goes back to
	// the engine's default rather than leaving whatever it last pointed at - a
	// pass that skipped the empty answer would make "None" mean "keep the
	// previous one".
	store.GetMutable<MaterialRef>(material)->Asset = Name();
	CHECK(ResolveMaterials(store) == 1);
	CHECK_FALSE(store.Get<SurfaceAppearance>(part)->ColourMap.IsValid());
}

TEST_CASE("a part with no material instance is left alone", "[scene][materials]") {
	Store store = Fresh("materials.untouched");

	const Entity part = store.CreateInstance(engine::ecs::Classes::Find(Name("Part")), "Painted");
	REQUIRE(part != NULL_ENTITY);

	// Authoring `BasePart.ColorMap` directly still works and still means what it
	// did. The pass visits `MaterialRef` rows, so a part without one is never
	// touched - which is what keeps the direct path and the material path from
	// fighting over the same field every tick.
	const Name authored("textures/hand_painted.atex");
	store.GetMutable<SurfaceAppearance>(part)->ColourMap = authored;

	CHECK(ResolveMaterials(store) == 0);
	CHECK(store.Get<SurfaceAppearance>(part)->ColourMap == authored);
}

TEST_CASE("a shader-only material preserves maps authored on its part", "[scene][materials]") {
	Store store = Fresh("materials.shader_only");

	const Entity part = store.CreateInstance(engine::ecs::Classes::Find(Name("Part")), "Terrain");
	REQUIRE(part != NULL_ENTITY);
	SurfaceAppearance *appearance = store.GetMutable<SurfaceAppearance>(part);
	REQUIRE(appearance != nullptr);
	appearance->ColourMap = Name("textures/procedural_lut.atex");
	appearance->NormalMap = Name("textures/procedural_normal.atex");

	const Entity material = Dress(store, part, Name{});
	MaterialRef *reference = store.GetMutable<MaterialRef>(material);
	REQUIRE(reference != nullptr);
	reference->Shader = Name("PlanetSurface");

	REQUIRE(ResolveMaterials(store) == 1);
	CHECK(appearance->ColourMap == Name("textures/procedural_lut.atex"));
	CHECK(appearance->NormalMap == Name("textures/procedural_normal.atex"));
	CHECK(appearance->Shader == Name("PlanetSurface"));
}

TEST_CASE("a material parented to nothing resolves onto nothing", "[scene][materials]") {
	Store store = Fresh("materials.orphan");

	const Name asset("materials/oak.amat");
	REQUIRE(RecordMaterial(store, asset, MaterialMaps{.Colour = Name("materials/oak_Color.atex")}));

	// A `Material` at the root of a world is legal - an author drags one in
	// before deciding where it goes - and resolves onto nothing rather than
	// raising. Counted as unresolved, because nothing was written.
	const Entity material = store.CreateInstance(MaterialClass(), "Loose");
	REQUIRE(material != NULL_ENTITY);
	store.GetMutable<MaterialRef>(material)->Asset = asset;

	CHECK(ResolveMaterials(store) == 0);
}

TEST_CASE("deleting a material clears the part it dressed", "[scene][materials]") {
	Store store = Fresh("materials.deleted");

	const Name asset("materials/oak.amat");
	REQUIRE(RecordMaterial(store, asset, MaterialMaps{.Colour = Name("materials/oak_Color.atex")}));

	const Entity part = store.CreateInstance(engine::ecs::Classes::Find(Name("Part")), "Crate");
	const Entity material = Dress(store, part, asset);

	REQUIRE(ResolveMaterials(store) == 1);
	REQUIRE(store.Get<SurfaceAppearance>(part)->ColourMap.IsValid());

	// **D00032, and the pass that fixes it never visits the part directly.**
	// Nothing walks a part that has no material - the clear comes from the
	// difference between what this pass wrote and what the last one did, so it
	// costs the number of materials rather than the number of parts.
	store.DestroyInstance(material);

	CHECK(ResolveMaterials(store) == 0);
	CHECK_FALSE(store.Get<SurfaceAppearance>(part)->ColourMap.IsValid());
}

TEST_CASE("a material moved to another part clears the first", "[scene][materials]") {
	Store store = Fresh("materials.reparented");

	const Name asset("materials/oak.amat");
	const Name colour("materials/oak_Color.atex");
	REQUIRE(RecordMaterial(store, asset, MaterialMaps{.Colour = colour}));

	const engine::ecs::ClassId partClass = engine::ecs::Classes::Find(Name("Part"));
	const Entity first = store.CreateInstance(partClass, "First");
	const Entity second = store.CreateInstance(partClass, "Second");
	const Entity material = Dress(store, first, asset);

	REQUIRE(ResolveMaterials(store) == 1);
	REQUIRE(store.Get<SurfaceAppearance>(first)->ColourMap == colour);

	// **This is the case a destruction hook would not have caught**, and it is
	// why the fix is a difference between passes rather than a hook on the row
	// leaving. Nothing was destroyed: the row is alive and simply names a
	// different parent, and the part it left keeps its texture for ever without
	// this.
	REQUIRE(store.SetParent(material, second));

	CHECK(ResolveMaterials(store) == 1);
	CHECK_FALSE(store.Get<SurfaceAppearance>(first)->ColourMap.IsValid());
	CHECK(store.Get<SurfaceAppearance>(second)->ColourMap == colour);
}

TEST_CASE("a world with no materials acquires no catalogue", "[scene][materials]") {
	Store store = Fresh("materials.noresource");

	const Entity part = store.CreateInstance(engine::ecs::Classes::Find(Name("Part")), "Plain");
	REQUIRE(part != NULL_ENTITY);

	// **The pass leaves an untouched world untouched.** `ColourMapOf` is the
	// non-creating reader precisely so this holds, and recording the resolved
	// set would undo it for every world in a universe if it were written
	// unconditionally rather than only when there is something to remember.
	CHECK(ResolveMaterials(store) == 0);
	CHECK_FALSE(store.HasResource<engine::scene::MaterialCatalogue>());
}
