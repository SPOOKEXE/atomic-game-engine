// Decision 19, exercised: LOD selection targets quad utilization.
//
// The cases that matter are the ones a distance ladder would get wrong. A large
// object and a small one at the same distance cover different areas of the
// screen and want different levels, and area per triangle is the number that
// says so on a phone and on a workstation alike.

#include <engine/core/Name.hpp>
#include <engine/scene/LevelOfDetail.hpp>
#include <engine/scene/MeshCatalogue.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>

TEST_SUITE_ID("engine.scene.levelofdetail")

using engine::core::Name;
using engine::scene::DEFAULT_TARGET_QUAD_AREA;
using engine::scene::LevelMesh;
using engine::scene::LevelOfDetail;
using engine::scene::LevelTriangles;
using engine::scene::LOD_LEVELS;
using engine::scene::LodStrategy;
using engine::scene::MeshCatalogue;
using engine::scene::SelectLevel;

namespace {
	const Name &BaseMesh() {
		static const Name name("lod_test.Statue");
		return name;
	}

	MeshCatalogue CatalogueWith(uint32_t triangles) {
		MeshCatalogue catalogue;
		catalogue.Triangles[BaseMesh().Id()] = triangles;
		return catalogue;
	}

	// A decimated ladder over a ten-thousand-triangle base: 10000, 5000, 2500,
	// 1250 triangles.
	LevelOfDetail DecimatedLadder() {
		LevelOfDetail ladder;
		ladder.Strategy = LodStrategy::Decimated;
		ladder.Levels = LOD_LEVELS;
		return ladder;
	}
}

TEST_CASE("a part that fills the screen stays at level zero", "[scene][lod]") {
	const MeshCatalogue catalogue = CatalogueWith(10000);
	const LevelOfDetail ladder = DecimatedLadder();

	// Two million pixels over ten thousand triangles is two hundred pixels a
	// triangle, which is well past a quad even at the finest level.
	CHECK(SelectLevel(ladder, catalogue, BaseMesh(), 2000000.0f) == 0);
}

TEST_CASE("a part covering a few pixels drops down its ladder", "[scene][lod]") {
	const MeshCatalogue catalogue = CatalogueWith(10000);
	const LevelOfDetail ladder = DecimatedLadder();

	// Ten thousand pixels: one pixel a triangle at level zero, two at level one,
	// four at level two - which is the first that clears the four-pixel quad, so
	// that is where it lands. Level three would clear it too and is not chosen,
	// because the finest acceptable level is the most detail worth shading.
	CHECK(SelectLevel(ladder, catalogue, BaseMesh(), 10000.0f) == 2);

	// Five thousand pixels needs the bottom of the ladder to reach four.
	CHECK(SelectLevel(ladder, catalogue, BaseMesh(), 5000.0f) == LOD_LEVELS - 1);
}

TEST_CASE("area and not distance is what selects", "[scene][lod]") {
	// The case a distance ladder cannot express. Two parts at one distance, one
	// covering a hundred times the area of the other, want different levels -
	// and the *same* ladder has to answer for both.
	const MeshCatalogue catalogue = CatalogueWith(10000);
	const LevelOfDetail ladder = DecimatedLadder();

	const uint8_t tower = SelectLevel(ladder, catalogue, BaseMesh(), 400000.0f);
	const uint8_t cup = SelectLevel(ladder, catalogue, BaseMesh(), 4000.0f);
	CHECK(tower < cup);
}

TEST_CASE("a raised target takes a coarser level sooner", "[scene][lod]") {
	// The dial a game turns when it is trading sharpness for frame time. The
	// component has to be able to express decision 19's target, not only obey
	// its default.
	const MeshCatalogue catalogue = CatalogueWith(10000);

	LevelOfDetail sharp = DecimatedLadder();
	sharp.TargetQuadArea = 1.0f;

	LevelOfDetail cheap = DecimatedLadder();
	cheap.TargetQuadArea = 64.0f;

	CHECK(
		SelectLevel(cheap, catalogue, BaseMesh(), 20000.0f) >
		SelectLevel(sharp, catalogue, BaseMesh(), 20000.0f)
	);
}

TEST_CASE("zero means the default target", "[scene][lod]") {
	// So a ladder an author never touched still selects, rather than never
	// advancing past level zero.
	const MeshCatalogue catalogue = CatalogueWith(10000);

	LevelOfDetail implicit = DecimatedLadder();
	LevelOfDetail explicitly = DecimatedLadder();
	explicitly.TargetQuadArea = DEFAULT_TARGET_QUAD_AREA;

	CHECK(
		SelectLevel(implicit, catalogue, BaseMesh(), 30000.0f) ==
		SelectLevel(explicitly, catalogue, BaseMesh(), 30000.0f)
	);
}

TEST_CASE("a strategy of none never leaves level zero", "[scene][lod]") {
	// What every part in every scene is today.
	const MeshCatalogue catalogue = CatalogueWith(10000);

	LevelOfDetail off;
	off.Levels = LOD_LEVELS;
	CHECK(SelectLevel(off, catalogue, BaseMesh(), 1.0f) == 0);
}

TEST_CASE("a base mesh the world has not been told about stays at level zero", "[scene][lod]") {
	// `MeshCatalogue`'s zero means "not known here", not "empty". Choosing a
	// coarse level from a count of zero would drop every mesh part to its lowest
	// detail for the whole time content was arriving.
	const MeshCatalogue empty;
	CHECK(SelectLevel(DecimatedLadder(), empty, BaseMesh(), 4.0f) == 0);
}

TEST_CASE("an area that is not a number stays at level zero", "[scene][lod]") {
	// Written so a NaN selects level zero. The obvious way round makes a NaN
	// select the coarsest level, which is a part that quietly turns into a blob
	// on whichever machine produced the NaN. `IsDead`'s rule.
	const MeshCatalogue catalogue = CatalogueWith(10000);
	CHECK(SelectLevel(DecimatedLadder(), catalogue, BaseMesh(), std::numeric_limits<float>::quiet_NaN()) == 0);
	CHECK(SelectLevel(DecimatedLadder(), catalogue, BaseMesh(), -5.0f) == 0);
}

TEST_CASE("an authored level's triangles come from the catalogue", "[scene][lod]") {
	// The difference an author sees between the two generated strategies and the
	// authored one: a published coarse mesh has a real count, and the ratio is
	// only the fallback for before it has arrived.
	const Name coarse("lod_test.StatueLow");

	MeshCatalogue catalogue = CatalogueWith(10000);

	LevelOfDetail ladder;
	ladder.Strategy = LodStrategy::Authored;
	ladder.Levels = 2;
	ladder.Meshes[0] = coarse;
	ladder.Ratios[0] = 0.5f;

	// Before the coarse mesh lands, the ratio answers.
	CHECK(LevelTriangles(ladder, catalogue, BaseMesh(), 1) == 5000);

	// Once it lands, the mesh answers, and it need not agree with the ratio.
	catalogue.Triangles[coarse.Id()] = 900;
	CHECK(LevelTriangles(ladder, catalogue, BaseMesh(), 1) == 900);
	CHECK(LevelMesh(ladder, BaseMesh(), 1) == coarse);
}

TEST_CASE("level zero is the base mesh and is not stored twice", "[scene][lod]") {
	// Three names for four levels. Storing the base here as well would be the
	// second copy of a fact the part already carries, and repointing `MeshId`
	// would leave it behind.
	LevelOfDetail ladder;
	ladder.Strategy = LodStrategy::Authored;
	ladder.Levels = LOD_LEVELS;
	CHECK(LevelMesh(ladder, BaseMesh(), 0) == BaseMesh());

	// A level naming nothing falls back to the base rather than to an invalid
	// name, which would draw the default cube.
	CHECK(LevelMesh(ladder, BaseMesh(), 2) == BaseMesh());
}

TEST_CASE("an authored level count out of range is clamped rather than trusted", "[scene][lod]") {
	// `Levels` arrives from a file somebody else wrote, and every loop over the
	// ladder indexes `Meshes` with it.
	const MeshCatalogue catalogue = CatalogueWith(10000);

	LevelOfDetail broken = DecimatedLadder();
	broken.Levels = 200;
	CHECK(SelectLevel(broken, catalogue, BaseMesh(), 10000.0f) < LOD_LEVELS);

	broken.Levels = 0;
	CHECK(SelectLevel(broken, catalogue, BaseMesh(), 10000.0f) == 0);
}
