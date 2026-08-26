#pragma once

// What a world's terrain is made from, which is a recipe and not a heightfield.
//
// **The storage decision is that the chunks are never stored, and it follows
// from what the roadmap asks for.** `ROADMAP.md` wants a "(procedural,
// node-based) terrain generator", so the authored thing is a graph and a seed.
// The voxels or the heightfield that graph produces are derived from it, and
// this module's standing rule is that a derived fact stored beside its inputs
// goes stale silently - the same argument that keeps a world AABB off `Bounds`
// and a triangle count off `Visual`, at a scale where getting it wrong costs
// gigabytes rather than sixteen bytes.
//
// **It is also the only form that can cross a wire.** `CollisionShapes` makes
// exactly this argument for hulls: a shape derived from content the receiving
// side already has must not be sent, because sending a conclusion instead of its
// input hands an attacker the half they get to choose. A terrain is that with
// four more orders of magnitude on it. Both ends run the same graph over the
// same seed and get the same ground, which is decision 14's strict IEEE
// arithmetic doing the work it exists to do.
//
// **A resource and not a component on a `Terrain` instance**, which is where
// Roblox puts it. Roblox's `Terrain` derives from `BasePart` and is a singleton
// under `Workspace` that `Instance.new` refuses to make a second of, and this
// engine has no way to register a class that cannot be constructed. A resource
// is one per world by construction, which is the property that mattered, and
// `WorldBounds` is the existing type with exactly this shape: authored, one per
// world, and nothing derives it from anything. The script surface is on
// `workspace`, which is where `SurfaceBounces` and `MaxSurfaces` already are.
//
// **Nothing here generates anything, and nothing here may.** `scene` depends on
// `core`, `ecs` and `spatial` and that list is not growing. The generator is a
// `graph` node set and the chunks it produces are the generator's own storage,
// exactly as the broadphase grids are `physics::PhysicsWorld`'s.
//
// arch-waiver public-header: forward API. The generator that reads this recipe is a
// `graph` node set; `docs/FUTURE_COMPONENTS.md` says why the chunks it produces
// are never stored. Decision 16.
//
// @tier L7 · shared

#include <engine/core/Name.hpp>

#include <cstdint>

namespace engine::ecs {
	class Store;
}

namespace engine::scene {

	// The largest chunk edge a world may ask for, in metres.
	//
	// **A ceiling on the recipe rather than on the generator**, because the cost
	// is quadratic in this number and the failure is a single allocation nobody
	// expected: at the default resolution a 4 km chunk is a hundred million
	// samples. A game that wants coarse ground raises `ChunkResolution` down
	// instead, which is the dial that actually trades detail for memory.
	inline constexpr float MAX_CHUNK_EXTENT = 1024.0f;

	// The most samples one chunk edge may carry.
	//
	// 1024 squared is a million samples per chunk, which is the point past which
	// a chunk stops fitting in a cache anywhere and the generator would want to
	// tile internally regardless.
	inline constexpr uint16_t MAX_CHUNK_RESOLUTION = 1024;

	// How a world's ground is generated, one per world.
	//
	// @since v0.19
	struct Terrain {
		// What makes two runs produce the same ground.
		//
		// **Widest first**, so the object representation holds no padding between
		// this and the fields below it, and 64 bits rather than 32 because a seed
		// somebody types is the one number a game hands to its players.
		uint64_t Seed = 0;

		// Which node graph produces the ground.
		//
		// A `core::Name` for rule 4's reason: it crosses a save file and a wire,
		// and `graph::NodeCatalogue` already keys its documents by name. An
		// invalid name is a world with no terrain, which is every world today.
		core::Name Generator;

		// How wide one chunk is, in metres.
		//
		// **Metres and not studs**, matching every other distance in this module -
		// `Gravity` says at length why this engine measures a part sized 2 as two
		// metres rather than copying Roblox's numbers.
		float ChunkExtent = 64.0f;

		// How far above and below the origin the ground may reach, in metres.
		//
		// **Symmetric about zero rather than a floor and a ceiling**, because the
		// number a generator needs is the range it normalises its output into, and
		// two numbers would let an author write a range that does not contain the
		// origin - a world whose sea level is off the bottom of its own terrain.
		float VerticalExtent = 256.0f;

		// How far from a viewer chunks are kept generated, in metres.
		//
		// Authored rather than derived from the camera's far plane, because those
		// are two different questions: a far plane is how far a frame draws and
		// this is how far the ground exists for physics, which has to be the
		// larger of the two or a body walks off the world.
		float ViewDistance = 512.0f;

		// How many samples one chunk edge carries.
		//
		// The resolution of the ground, and the dial that actually trades detail
		// for memory: samples per chunk are the square of this.
		uint16_t ChunkResolution = 64;

		// Whether the world has terrain at all.
		//
		// **A flag rather than an invalid `Generator`**, because switching terrain
		// off for a cutscene and back on must not lose which graph the game was
		// using. `CameraController::Enabled` is the same split for the same
		// reason.
		bool Enabled = false;

		// Explicit padding, for the reason `Components.hpp` opens with.
		uint8_t Reserved[5] = {};
	};

	// The world's terrain recipe, creating a default one if it has none.
	//
	// **`RegisterSceneComponents` must have run first**, as it must before any
	// resource here is set: `SetResource` keys on a component id, and one minted
	// before the explicit registration lands takes the compiler's spelling of the
	// type and aborts once the table is sealed.
	//
	// @param store The world.
	// @return Its terrain recipe.
	Terrain &TerrainOf(ecs::Store &store);

	// The world's terrain recipe, or the defaults when it has never set one.
	//
	// **A free function rather than `store.Resource<Terrain>()` at every call
	// site**, which is `SunOf`'s arrangement and is here for its reason: "no
	// terrain resource" and "terrain switched off" have to be the same answer, so
	// that a world loaded from a file that predates this reads as having no
	// ground rather than as a null a caller forgot to check.
	//
	// @param store The world.
	// @return Its recipe, clamped.
	Terrain TerrainSettings(const ecs::Store &store);

	// Whether a recipe would generate anything.
	//
	// Enabled, with a generator named, a chunk that has an extent and a
	// resolution. Stated once here because a host deciding whether to stand a
	// generator up and an editor deciding whether to draw a terrain gizmo ask the
	// same question, and two statements of it would disagree about the world
	// where somebody switched it on and named nothing.
	//
	// @param terrain The recipe.
	// @return `true` when it describes generatable ground.
	bool GeneratesGround(const Terrain &terrain);
}
