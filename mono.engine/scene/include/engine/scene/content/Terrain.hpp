#pragma once

// The authored recipe attached to a world's generated Terrain instance.
//
// The recipe is stored and serialised on the singleton child of Workspace.
// Generated samples, meshes, and collision remain derived from the recipe and
// are not stored here. Keeping those outputs out of the save and wire payload
// lets each world generate them from the same authored inputs.
//
// `InstallServices` creates the non-creatable Terrain instance, migrates older
// world resources into its component, and protects it from deletion or reparenting.
// Scripts and tools reach it through `Workspace.Terrain`.
//
// Generation belongs to the graph and runtime layers. This module owns only
// the authored settings and never stores generated chunks.

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

	// Authored generation settings stored on the world's Terrain instance.
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

	// The recipe on this world's generated Terrain instance, creating the world
	// fixtures first when necessary.
	//
	// `RegisterSceneClasses` must have run first so the generated class and its
	// registered component are available.
	//
	// @param store The world.
	// @return The mutable recipe component on the singleton Terrain instance.
	Terrain &TerrainOf(ecs::Store &store);

	// The world's recipe, or the defaults when no Terrain instance or legacy
	// resource has supplied one.
	//
	// A read never creates fixtures or migrates the legacy resource. This keeps
	// property inspection safe before world installation has completed.
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
