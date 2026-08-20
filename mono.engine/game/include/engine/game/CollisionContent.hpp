#pragma once

// Turning mesh content into a world's collision geometry.
//
// **The one place that bakes a hull, so a client and a headless server get the
// same shape.** `scene::Collider` names its geometry with a string and
// `scene::CollisionShapes` is where that name resolves; something has to put the
// hull and the triangle soup in, and until v0.17 the only thing that did was the
// client, inline, on the frame a mesh asset arrived. A server therefore had mesh
// colliders it could not resolve, and every part using one silently collided as
// its bound - which is a client and a server disagreeing about where a player is
// standing.
//
// **Here because this is the lowest module that can see both halves.**
// `assets::MeshData` is L8 and `scene::CollisionShapes` is L7, so the conversion
// needs a module above both, and `collision`, at L5, is where the geometry types
// live but cannot see an asset. `game` is L10, is `shared`, and is already the
// module that says how a host's content and a world relate.
//
// **It opens nothing.** A caller hands over bytes it already has: the client
// from `delivery`, a server from the `assets::ChunkStore` it is already serving,
// and either from the built-ins, which are generated rather than fetched. Where
// the bytes came from is the caller's business and the trust boundary stays
// `delivery`'s.
//
// **Two layers of function, because a host with many worlds bakes once.** The
// `Add` pair fill a table; the `Record`/`Merge` pair put a table on a world.
// Quickhull over a model is not free and a `scene::CollisionShapes` is copied
// whole by `SetResource`, so a server with eight worlds fills one table and
// merges it eight times rather than reading its store eight times.
//
// @tier L10 · shared
// @since v0.17

#include <engine/core/Name.hpp>

#include <cstddef>

namespace engine::assets {
	struct MeshData;
	class ChunkStore;
	class Manifest;
}

namespace engine::ecs {
	class Store;
}

namespace engine::scene {
	struct CollisionShapes;
}

namespace engine::game {

	// Bakes one mesh's collision geometry into a table, under a name.
	//
	// **Both a hull and a triangle soup, and the part chooses.** Which of the
	// two a collider wants is authoring - `BasePart.CollisionShape` - and it can
	// change after the mesh has arrived, so baking only the kind that happens to
	// be in use when the bytes land would leave the other unreachable until a
	// reload. A hull is at most `collision::MAXIMUM_HULL_POINTS` points and the
	// soup is the vertices the mesh already holds, so carrying both is cheap
	// next to the mesh itself.
	//
	// A name registered twice replaces, per `scene::CollisionShapes::SetHull`.
	// An invalid name is ignored: nothing could ever ask for it.
	//
	// @param into The table.
	// @param name What a `Collider::Geometry` will say. The mesh's own name.
	// @param mesh The geometry, in the part's object space.
	void AddCollisionShapes(scene::CollisionShapes &into, core::Name name, const assets::MeshData &mesh);

	// Bakes the six built-in meshes into a table.
	//
	// **Every host wants these, because a built-in is there before any content
	// is.** `assets::MakeBuiltin` generates them rather than shipping files, so
	// they never travel the path `AddCollisionShapes` is otherwise called from -
	// which meant a `MeshPart` set to `Cube` with a hull collider resolved to
	// nothing on every host including the client.
	//
	// The six are baked once per process and copied in after that.
	//
	// @param into The table.
	void AddBuiltinCollisionShapes(scene::CollisionShapes &into);

	// Bakes every mesh in a content store into a table.
	//
	// **What lets a headless server host mesh colliders at all.** A server with
	// an attached origin already opens the store and reads the manifest in order
	// to serve it; the bytes are local, and this is the pass that turns the mesh
	// ones into geometry. A server with no store gets the built-ins and nothing
	// else, which is stated rather than worked around.
	//
	// **Every mesh entry, not the ones some world happens to name today.** A
	// script may set `BasePart.CollisionGeometry` to anything in the manifest at
	// any tick, and a server that had baked only what the opening scene used
	// would answer that with a silent fall back to the part's bound. The day a
	// store is large enough for this to hurt, the fix is to bake on first use
	// rather than to bake a subset here.
	//
	// **Only `AssetKind::Mesh` entries**, and the kind is the manifest's own
	// rather than derived from the name: `assets::AssetKind` carries the argument
	// for why deciding it at each reader is two opinions about what `rock.glb`
	// is.
	//
	// **A mesh that will not parse is skipped with a line in the log**, not
	// refused. `assets::Mesh::Read` reads this engine's format, so a store that
	// was published without baking its models holds entries this cannot use -
	// and one unusable entry is one unusable entry. Refusing the whole world
	// over it would take a server down for a model nobody is standing on.
	//
	// @param into     The table.
	// @param chunks   The store holding the bytes.
	// @param manifest Its manifest, already read and verified by the caller.
	// @return How many meshes were baked.
	size_t AddCollisionShapesFrom(
		scene::CollisionShapes &into, const assets::ChunkStore &chunks, const assets::Manifest &manifest
	);

	// Copies one table's rows into another, keeping what the destination had.
	//
	// The other half of baking once: a host that has just baked an arriving mesh
	// into a small table adds it to the session's table with this, and puts it
	// on each open world with the overload below.
	//
	// Names in `from` win, per `scene::CollisionShapes::SetHull`.
	//
	// @param into The table to add to.
	// @param from The table to read.
	void MergeCollisionShapes(scene::CollisionShapes &into, const scene::CollisionShapes &from);

	// Bakes the six built-in meshes into a world.
	//
	// The one call every host makes, whatever else it has: see
	// `AddBuiltinCollisionShapes` for why a built-in needs one at all. Cheap and
	// idempotent, so a world that is rebuilt may have it again.
	//
	// @param store The world.
	void RecordBuiltinCollisionShapes(ecs::Store &store);

	// Puts a prebuilt table on a world, keeping what the world already had.
	//
	// The many-worlds path: bake once with the `Add` functions, merge into each
	// world as it is created. Names in `shapes` win, per `SetHull`.
	//
	// @param store  The world.
	// @param shapes The table.
	void MergeCollisionShapes(ecs::Store &store, const scene::CollisionShapes &shapes);
}
