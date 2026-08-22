// The terrain recipe, which is what is stored, and the ground, which is not.
//
// The clamping cases are the point rather than pedantry: these values arrive
// from a save file and a wire as often as from a property setter, and a reader
// that trusted them would turn somebody else's number into an allocation.

#include <engine/core/Bytes.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Terrain.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.scene.terrain")

using engine::core::ByteReader;
using engine::core::ByteWriter;
using engine::core::Name;
using engine::ecs::Classes;
using engine::ecs::Entity;
using engine::ecs::Store;
using engine::scene::GeneratesGround;
using engine::scene::InstallServices;
using engine::scene::MAX_CHUNK_EXTENT;
using engine::scene::MAX_CHUNK_RESOLUTION;
using engine::scene::RegisterSceneClasses;
using engine::scene::ServiceOf;
using engine::scene::Terrain;
using engine::scene::TerrainOf;
using engine::scene::TerrainSettings;

TEST_CASE("a world with no recipe reads as having no terrain", "[scene][terrain]") {
	// `SunOf`'s rule: "no resource" and "switched off" have to be the same
	// answer, so a world authored before this reads as ground-free rather than
	// as a null a caller forgot to check.
	RegisterSceneClasses();
	Store store("terrain_test.empty");

	const Terrain recipe = TerrainSettings(store);
	CHECK_FALSE(recipe.Enabled);
	CHECK_FALSE(GeneratesGround(recipe));
}

TEST_CASE("asking for the recipe mints one and keeps it", "[scene][terrain]") {
	RegisterSceneClasses();
	Store store("terrain_test.mint");

	TerrainOf(store).Seed = 42;
	TerrainOf(store).Generator = Name("terrain_test.Hills");
	TerrainOf(store).Enabled = true;

	const Terrain recipe = TerrainSettings(store);
	CHECK(recipe.Seed == 42);
	CHECK(recipe.Generator.Text() == "terrain_test.Hills");
	CHECK(GeneratesGround(recipe));
}

TEST_CASE("a recipe with no generator generates nothing", "[scene][terrain]") {
	// The flag and the name are two separate statements, so that switching
	// terrain off for a cutscene does not lose which graph the game was using.
	RegisterSceneClasses();
	Store store("terrain_test.named");

	Terrain &recipe = TerrainOf(store);
	recipe.Enabled = true;
	CHECK_FALSE(GeneratesGround(recipe));

	recipe.Generator = Name("terrain_test.Hills");
	CHECK(GeneratesGround(recipe));

	recipe.Enabled = false;
	CHECK_FALSE(GeneratesGround(recipe));
	CHECK(recipe.Generator.IsValid());
}

TEST_CASE("a chunk asking for more than the ceiling is clamped", "[scene][terrain]") {
	// The cost is quadratic in these two numbers and the failure is a single
	// allocation nobody expected.
	RegisterSceneClasses();
	Store store("terrain_test.clamp");

	Terrain &recipe = TerrainOf(store);
	recipe.ChunkExtent = 100000.0f;
	recipe.ChunkResolution = 60000;
	recipe.VerticalExtent = -10.0f;
	recipe.ViewDistance = -1.0f;

	const Terrain clamped = TerrainSettings(store);
	CHECK(clamped.ChunkExtent == MAX_CHUNK_EXTENT);
	CHECK(clamped.ChunkResolution == MAX_CHUNK_RESOLUTION);
	CHECK(clamped.VerticalExtent == 0.0f);
	CHECK(clamped.ViewDistance == 0.0f);
}

TEST_CASE("a resolution of zero generates nothing", "[scene][terrain]") {
	RegisterSceneClasses();
	Store store("terrain_test.flat");

	Terrain &recipe = TerrainOf(store);
	recipe.Enabled = true;
	recipe.Generator = Name("terrain_test.Hills");
	recipe.ChunkResolution = 0;
	CHECK_FALSE(GeneratesGround(recipe));

	recipe.ChunkResolution = 32;
	recipe.ChunkExtent = 0.0f;
	CHECK_FALSE(GeneratesGround(recipe));
}

TEST_CASE("the recipe crosses a snapshot and the generator crosses as text", "[scene][terrain]") {
	// The whole design in one case: what is stored is the recipe, and the name
	// in it is a string somebody chose rather than an id this process assigned.
	RegisterSceneClasses();
	Store source("terrain_test.save.source");

	Terrain &recipe = TerrainOf(source);
	recipe.Seed = 0x0123456789ABCDEFull;
	recipe.Generator = Name("terrain_test.Archipelago");
	recipe.ChunkExtent = 96.0f;
	recipe.VerticalExtent = 300.0f;
	recipe.ViewDistance = 700.0f;
	recipe.ChunkResolution = 128;
	recipe.Enabled = true;

	ByteWriter writer;
	REQUIRE(source.Save(writer));

	// Shifts every id assigned afterwards, which is what makes the name half of
	// this case prove anything.
	const Name shifter("terrain_test.ShiftsTheIdSpace");
	CHECK(shifter.IsValid());

	Store restored("terrain_test.save.restored");
	ByteReader reader(writer.Bytes());
	REQUIRE(restored.Load(reader));

	const Terrain back = TerrainSettings(restored);
	CHECK(back.Seed == 0x0123456789ABCDEFull);
	CHECK(back.Generator.Text() == "terrain_test.Archipelago");
	CHECK(back.ChunkExtent == 96.0f);
	CHECK(back.VerticalExtent == 300.0f);
	CHECK(back.ViewDistance == 700.0f);
	CHECK(back.ChunkResolution == 128);
	CHECK(back.Enabled);
}

TEST_CASE("the recipe is authored through workspace and nowhere else", "[scene][terrain]") {
	// `SurfaceBounces`' arrangement: the resource is the only storage and the
	// property is the only way in, so there is nothing for a second copy to
	// drift from. The cases that matter are the ones a plain member projection
	// could not express - a getter that never acquires the resource, and a
	// refusal at the setter.
	RegisterSceneClasses();
	Store store("terrain_test.properties");
	InstallServices(store);

	const Entity workspace = ServiceOf(store, Classes::Find(Name("Workspace")));
	REQUIRE(workspace != engine::ecs::NULL_ENTITY);

	// Reading before anything has written must not mint the resource, which is
	// `TrianglesOf`'s split against `MeshesOf`: a getter that made a structural
	// write would make one on every properties-panel refresh.
	bool enabled = true;
	REQUIRE(store.GetProperty(workspace, Name("TerrainEnabled"), &enabled, sizeof(enabled)));
	CHECK_FALSE(enabled);
	CHECK_FALSE(store.HasResource<Terrain>());

	const Name generator("terrain_test.Dunes");
	REQUIRE(store.SetProperty(workspace, Name("TerrainGenerator"), &generator, sizeof(generator)));

	const int64_t seed = -7;
	REQUIRE(store.SetProperty(workspace, Name("TerrainSeed"), &seed, sizeof(seed)));

	enabled = true;
	REQUIRE(store.SetProperty(workspace, Name("TerrainEnabled"), &enabled, sizeof(enabled)));

	const float chunk = 48.0f;
	REQUIRE(store.SetProperty(workspace, Name("TerrainChunkSize"), &chunk, sizeof(chunk)));

	const float view = 900.0f;
	REQUIRE(store.SetProperty(workspace, Name("TerrainViewDistance"), &view, sizeof(view)));

	const Terrain recipe = TerrainSettings(store);
	CHECK(recipe.Generator == generator);
	CHECK(recipe.ChunkExtent == 48.0f);
	CHECK(recipe.ViewDistance == 900.0f);
	CHECK(GeneratesGround(recipe));

	// **A seed is a bit pattern rather than a quantity**, so a negative number a
	// script wrote has to come back as the same negative number rather than as
	// something clamped on the way through an unsigned field.
	int64_t back = 0;
	REQUIRE(store.GetProperty(workspace, Name("TerrainSeed"), &back, sizeof(back)));
	CHECK(back == -7);

	// A negative distance is refused rather than clamped: too large is a world
	// asking for more than a machine will allocate, and below zero is a world
	// asking for something the word does not mean.
	const float backwards = -1.0f;
	CHECK_FALSE(store.SetProperty(workspace, Name("TerrainChunkSize"), &backwards, sizeof(backwards)));
	CHECK_FALSE(store.SetProperty(workspace, Name("TerrainViewDistance"), &backwards, sizeof(backwards)));
	CHECK(TerrainSettings(store).ChunkExtent == 48.0f);

	// Larger than the ceiling is clamped rather than refused, because the
	// ceiling is a limit on what the generator will allocate rather than a
	// statement about what the number means.
	const float huge = MAX_CHUNK_EXTENT * 4.0f;
	REQUIRE(store.SetProperty(workspace, Name("TerrainChunkSize"), &huge, sizeof(huge)));
	CHECK(TerrainSettings(store).ChunkExtent == MAX_CHUNK_EXTENT);
}
