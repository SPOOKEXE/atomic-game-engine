#pragma once

// Which version of a mesh a frame should draw, and what decides.
//
// **Decision 19 is the whole design, not an influence on it.** "LOD selection
// targets quad utilization. No virtualized geometry" - so the number a level is
// chosen against is *pixels of projected area per triangle*, and never a
// distance. That matters because the two disagree exactly where it costs: a
// hundred-metre tower and a coffee cup at the same distance cover wildly
// different areas of the screen, and a distance ladder either over-tessellates
// the cup or under-tessellates the tower, per camera, per field of view, per
// resolution. Area per triangle is the same number on a phone and on a
// workstation.
//
// A quad is the 2x2 pixel block a rasteriser shades as a unit, so a triangle
// smaller than one still costs four shaded samples. `DEFAULT_TARGET_QUAD_AREA`
// is that threshold expressed in pixels, and it is what the component stores.
//
// **How the levels came to exist is a separate question from which one to
// draw**, and `LodStrategy` is the first while `SelectLevel` is the second.
// `ROADMAP.md`'s bullet names three ways to produce them - four authored meshes,
// auto-decimation, and triangle reduction driven by surface area - and a
// component has to be able to say "none of the above", which is what every part
// in every scene today is.
//
// **Nothing here stores which level was chosen.** That is derived, it changes
// per view, and a mirror looks at the same part from somewhere else in the same
// frame. `KeepLoaded` is the precedent: a rule deciding what reaches a draw call
// lives here as a function so that a test can exercise it, and the renderer
// calls it rather than reimplementing it.
//
// arch-waiver public-header: forward API. `SelectLevel` is decision 19 stated once
// and the draw-list build is what will call it; `docs/FUTURE_COMPONENTS.md`
// says why that pass is not `client::CollectInstances`. Decision 16.
//
// @tier L7 · shared

#include <engine/core/Name.hpp>

#include <cstddef>
#include <cstdint>

namespace engine::scene {

	struct MeshCatalogue;

	// How many levels a part may have, counting the one on `Visual::Mesh`.
	//
	// `ROADMAP.md`'s "4 different meshes version". Four is a ceiling rather than
	// a target: a part with two levels leaves the rest empty.
	inline constexpr size_t LOD_LEVELS = 4;

	// Pixels of projected area a triangle should cover before the next level is
	// taken.
	//
	// **Four, because a quad is 2x2.** A triangle covering less than one quad is
	// shading four samples for less than one pixel of result, which is the
	// utilisation decision 19 names. It is the default rather than the rule: a
	// game trading sharpness for frame time raises it, and one that cannot
	// tolerate popping lowers it.
	inline constexpr float DEFAULT_TARGET_QUAD_AREA = 4.0f;

	// Where a part's coarser levels come from.
	//
	// The numbers are the format: `LevelOfDetail` reaches a save file, so
	// reordering this is a format change. `NormalId` records the same decision.
	//
	// @since v0.19
	enum class LodStrategy : uint8_t {
		// The part is always drawn at `Visual::Mesh`. What every part in every
		// scene is today, and what a part with geometry too simple to reduce
		// should stay.
		None = 0,

		// An author supplied a mesh per level. `LevelOfDetail::Meshes` names
		// them, and level zero is `Visual::Mesh` itself.
		Authored = 1,

		// The bake step produced each level by decimating the one above it to
		// `LevelOfDetail::Ratios`. Names nothing: the publisher derives the level
		// names from the base mesh's.
		Decimated = 2,

		// The bake step produced each level by collapsing edges in order of how
		// little surface area they carry, which is the roadmap's "smart triangle
		// reduction". Uses the same ratios and differs in what it removes: a
		// silhouette edge survives a reduction and does not survive a
		// decimation.
		Reduced = 3,
	};

	// The coarser versions of a part's geometry, on the part.
	//
	// **An optional column and not part of `BasePart`'s set**, which is the
	// opposite call from `SurfaceAppearance` and `Tags` and is right for the
	// opposite reason. Those two are on every part because
	// `client::CollectInstances` is a fixed-signature batched walk that cannot
	// read an optional column at all. Level selection is not that walk: it runs
	// over `<Visual, LevelOfDetail>` and touches only the parts that have levels,
	// and in a world of four thousand plain cubes that is none of them. Putting
	// forty bytes on every cube to save a join on the few hundred that are
	// models is the trade backwards.
	//
	// @since v0.19
	struct LevelOfDetail {
		// The mesh for levels one, two and three.
		//
		// **Three names for four levels, because level zero is `Visual::Mesh`.**
		// Storing it here as well would be the second copy of a fact the part
		// already carries, and repointing `MeshId` would leave it behind - the
		// failure `Visual::Fitted` exists to make impossible one file along. An
		// invalid name ends the ladder, so a part with one coarse level fills one
		// slot.
		core::Name Meshes[LOD_LEVELS - 1];

		// What fraction of the base mesh's triangles each level keeps.
		//
		// **Read for every strategy, not only the generated ones**, because it is
		// what `SelectLevel` needs and the catalogue may not have been told about
		// a coarse mesh yet - a client selecting before its content pump has run
		// would otherwise fall back to level zero on exactly the frames that most
		// need a coarse one. An authored ladder that never records its ratios
		// still works; it simply cannot be chosen between until the meshes land.
		float Ratios[LOD_LEVELS - 1] = {0.5f, 0.25f, 0.125f};

		// Pixels of projected area a triangle should cover before the next level
		// is taken.
		//
		// Zero means `DEFAULT_TARGET_QUAD_AREA`, so a component an author never
		// touched still selects rather than never advancing past level zero.
		float TargetQuadArea = 0.0f;

		// Where the coarser levels came from.
		LodStrategy Strategy = LodStrategy::None;

		// How many levels this ladder actually has, one to `LOD_LEVELS`.
		//
		// **Stored rather than counted from the first invalid name**, because
		// `Decimated` and `Reduced` name nothing at all and would otherwise
		// always count one.
		uint8_t Levels = 1;

		// Explicit padding, for the reason `Components.hpp` opens with. This
		// component is registered with a hand-written pair because it holds
		// names, so these bytes do not reach a file - they are here so that
		// adding a field is a decision about the layout rather than a silent
		// change to it, which is `InputState::Reserved`'s rule.
		uint8_t Reserved[2] = {};
	};

	// Which level a part should be drawn at.
	//
	// **The single statement of decision 19**, and a free function for
	// `KeepLoaded`'s reason: a renderer is the one module a test cannot exercise,
	// so the rule that decides what reaches a draw call is the last place that
	// should live.
	//
	// The comparison is pixels of projected area per triangle. A level is
	// acceptable when its triangle count would still leave each triangle covering
	// at least `TargetQuadArea` pixels, and the **finest** acceptable level is
	// chosen: a coarser level always has more pixels per triangle, so asking for
	// the coarsest acceptable one would answer "the coarsest" for everything and
	// the target would do nothing. What decision 19 wants is the most detail that
	// is still worth shading. A part filling the screen therefore stays at level
	// zero, and one covering a few pixels falls to the bottom of its ladder.
	//
	// **A part too far away for any level to clear the target takes the coarsest
	// one**, which is the honest answer rather than the finest.
	//
	// **A base mesh the catalogue has never heard of selects level zero.** Zero
	// triangles is `MeshCatalogue`'s "this world has not been told", and choosing
	// a coarse level from a count of zero would drop every mesh part to its
	// lowest detail for the whole time content was arriving.
	//
	// @param lod           The ladder.
	// @param catalogue     Where triangle counts per mesh name come from.
	// @param base          `Visual::Mesh`, which is level zero.
	// @param projectedArea How many pixels of the frame the part covers.
	// @return The level index, from zero to `lod.Levels - 1`.
	uint8_t SelectLevel(
		const LevelOfDetail &lod, const MeshCatalogue &catalogue, const core::Name &base, float projectedArea
	);

	// The mesh a level names.
	//
	// Level zero is the base mesh; the rest come from `Meshes`. A generated
	// ladder names nothing above zero, so this answers the base for those and the
	// publisher's naming convention decides the rest when `bake` grows one.
	//
	// @param lod   The ladder.
	// @param base  `Visual::Mesh`.
	// @param level The level index.
	// @return The mesh to draw, or `base` when the level names none.
	core::Name LevelMesh(const LevelOfDetail &lod, const core::Name &base, uint8_t level);

	// How many triangles a level has.
	//
	// **An authored level's count comes from the catalogue and a generated one's
	// from its ratio**, which is the one place those two strategies differ to a
	// caller. Both answer zero when the base mesh is unknown, which is what makes
	// `SelectLevel` refuse to advance.
	//
	// @param lod       The ladder.
	// @param catalogue Where triangle counts per mesh name come from.
	// @param base      `Visual::Mesh`.
	// @param level     The level index.
	// @return The triangle count, or zero when it cannot be known.
	uint32_t LevelTriangles(
		const LevelOfDetail &lod, const MeshCatalogue &catalogue, const core::Name &base, uint8_t level
	);
}
