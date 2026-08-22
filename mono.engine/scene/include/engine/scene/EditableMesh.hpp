#pragma once

// A mesh a script builds and edits while the engine runs, rather than one
// that arrived from content.
//
// **The class `MeshTable::Add` was written for and that nothing minted.**
// `render::MeshTable` has taken a name and an `assets::MeshData` since v0.9 -
// the content pump has always been able to hand it one built by an importer.
// What did not exist was a second producer: a mesh a *script* builds a
// triangle at a time, in a world with no importer anywhere near it. This is
// that producer's storage.
//
// ## What is here and what is not
//
// Roblox's `EditableMesh` is close to eighty methods: bones, FACS poses,
// batched attribute arrays, per-face-per-vertex normals and UVs distinct from
// the position they sit on. This is the core a mesh needs to exist and be
// correct - one array per attribute, one shared index into all of them, and
// the operations that add, edit and remove a triangle - and it stops there
// deliberately. A vertex is one position, one normal and one UV; two faces
// sharing a corner share all three, which is the ordinary case for a mesh
// built in code and is not Roblox's more general model, where a hard edge or
// a UV seam needs the same position with two different normals. That case
// is not reachable here yet, and is the honest limit of this pass rather
// than an oversight - see `docs/DEFERRED.md` if one is opened for it.
//
// **No vertex removal.** A triangle can be removed - it is three indices,
// and removing a run of three is a `swap-and-pop` that only ever invalidates
// the *other* triangle's id, which `RemoveTriangle` documents. A vertex may
// be referenced by any number of triangles, so removing one would mean
// walking every triangle to fix up or refuse the reference - real work this
// pass does not do. `Clear` is the door for "start over."
//
// ## Where the geometry goes
//
// **Never converted here.** `scene` may not link `assets` - the format
// `render::MeshTable::Add` takes - so this module holds the raw arrays and a
// revision counter, exactly as `scene::ShaderSource` holds GLSL text and a
// revision rather than a compiled module. `client::UpdateEditableMeshes` is
// where the conversion and the upload happen, on the same two-tier split
// `render::ShaderLibrary` already draws between "a world's own words" and
// "a device's own pipeline."
//
// **Named so a part can find it.** `EditableMeshContentName` derives a
// stable `core::Name` from the entity itself, and a script assigns that name
// to `MeshPart.MeshId` exactly as it would assign a published mesh's asset
// name - the render side does not know or care which kind of name resolved.
//
// @tier L7 · shared

#include <engine/core/Name.hpp>
#include <engine/core/types/Color3.hpp>
#include <engine/core/types/Vector2.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Entity.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace engine::ecs {
	class Store;
}

namespace engine::scene {

	// One vertex's own attributes, parallel across every array below by
	// index - vertex `i` is `Positions[i]`, `Normals[i]`, `UVs[i]` and
	// `Colours[i]`.
	//
	// @since v0.18
	struct EditableMesh {
		// Object-space positions. What `AddVertex` appends to.
		std::vector<core::Vector3> Positions;

		// Per-vertex normals, need not be unit length - `render::MeshVertex`
		// carries the same relaxation. Defaults to straight up for a vertex
		// nothing has set one for, which reads as flat-lit rather than as a
		// missing normal.
		std::vector<core::Vector3> Normals;

		// Per-vertex texture coordinates.
		std::vector<core::Vector2> UVs;

		// Per-vertex colour, multiplied into whatever the part's own texture
		// samples - `assets::Submesh::BaseColour` one level down, spread
		// across the vertices instead of held once for the whole run.
		std::vector<core::Color3> Colours;

		// Per-vertex alpha, parallel to `Colours` and kept apart from it for
		// `Visual::Transparency`'s reason: a colour and how much of it shows
		// are two different questions with two different consumers.
		std::vector<float> Alphas;

		// The triangle list. Always a multiple of three; three consecutive
		// entries are one triangle's vertex ids, counter-clockwise seen from
		// outside - `render/AGENTS.md`'s winding rule, unchanged for a mesh
		// built in code.
		std::vector<uint32_t> Indices;

		// Bumped by every call that changes what this describes.
		//
		// **The whole of how `client::UpdateEditableMeshes` knows to
		// re-upload**, matching `ShaderSource::Revision`'s exact reason: a
		// per-frame walk that diffed the arrays themselves would cost the
		// comparison it exists to avoid, so an integer that only moves
		// forward is compared instead.
		uint32_t Revision = 0;
	};

	// The `core::Name` a part's `MeshId` names this mesh by.
	//
	// **Derived from the entity, not authored** - two `EditableMesh`
	// instances never collide and neither survives being confused with the
	// other, because the id folded into the name is the entity's own.
	//
	// @param store    The world.
	// @param instance The `EditableMesh` instance.
	// @return The name, or an invalid one for anything but an `EditableMesh`.
	// @since v0.18
	core::Name EditableMeshContentName(const ecs::Store &store, ecs::Entity instance);

	// Appends a vertex and returns its id.
	//
	// **The id is the index**, so it is stable until `Clear` and never
	// reused by an ordinary edit - `RemoveTriangle` never removes a vertex,
	// so nothing shifts underneath one.
	//
	// @param store    The world.
	// @param instance The `EditableMesh` instance.
	// @param position Object-space position.
	// @param normal   The normal, or straight up when omitted.
	// @param uv       The texture coordinate, or the origin when omitted.
	// @return The vertex id, or nothing for anything but an `EditableMesh`.
	// @since v0.18
	std::optional<uint32_t> AddVertex(
		ecs::Store &store,
		ecs::Entity instance,
		const core::Vector3 &position,
		const core::Vector3 &normal = core::Vector3{0.0f, 1.0f, 0.0f},
		const core::Vector2 &uv = core::Vector2{}
	);

	// Appends a triangle over three existing vertex ids and returns its own
	// id.
	//
	// **The triangle id is not the vertex id's kind of stable** -
	// `RemoveTriangle` explains why the last triangle's id changes when an
	// earlier one is removed.
	//
	// @param store    The world.
	// @param instance The `EditableMesh` instance.
	// @param a        The first vertex id, counter-clockwise seen from
	//        outside.
	// @param b        The second.
	// @param c        The third.
	// @return The triangle id, or nothing when the instance is not an
	//         `EditableMesh` or any id names no vertex.
	// @since v0.18
	std::optional<uint32_t>
	AddTriangle(ecs::Store &store, ecs::Entity instance, uint32_t a, uint32_t b, uint32_t c);

	// Removes one triangle.
	//
	// **`swap-and-pop`, not a shift.** The removed triangle's three indices
	// are replaced by whatever was last in the list, so every triangle id
	// but the one moved keeps meaning what it meant - and the one that moved
	// is the id `TriangleCount() - 1` named a moment before this call. A
	// script removing several by id in descending order sees exactly that
	// and nothing surprising; removing them in ascending order does not,
	// which is the same rule `physics::Publish`'s own row-swap gives for the
	// identical reason.
	//
	// @param store    The world.
	// @param instance The `EditableMesh` instance.
	// @param triangle The triangle id.
	// @return `false` when the instance is not an `EditableMesh` or the id
	//         is out of range.
	// @since v0.18
	bool RemoveTriangle(ecs::Store &store, ecs::Entity instance, uint32_t triangle);

	// Sets one vertex's position.
	//
	// @return `false` when the instance is not an `EditableMesh` or the id
	//         is out of range.
	// @since v0.18
	bool SetVertexPosition(
		ecs::Store &store, ecs::Entity instance, uint32_t vertex, const core::Vector3 &position
	);

	// Sets one vertex's normal.
	//
	// @return `false` when the instance is not an `EditableMesh` or the id
	//         is out of range.
	// @since v0.18
	bool
	SetVertexNormal(ecs::Store &store, ecs::Entity instance, uint32_t vertex, const core::Vector3 &normal);

	// Sets one vertex's texture coordinate.
	//
	// @return `false` when the instance is not an `EditableMesh` or the id
	//         is out of range.
	// @since v0.18
	bool SetVertexUV(ecs::Store &store, ecs::Entity instance, uint32_t vertex, const core::Vector2 &uv);

	// Sets one vertex's colour and alpha.
	//
	// @return `false` when the instance is not an `EditableMesh` or the id
	//         is out of range.
	// @since v0.18
	bool SetVertexColor(
		ecs::Store &store,
		ecs::Entity instance,
		uint32_t vertex,
		const core::Color3 &colour,
		float alpha = 0.0f
	);

	// Empties every array and bumps the revision once.
	//
	// @return `false` for anything but an `EditableMesh`.
	// @since v0.18
	bool ClearEditableMesh(ecs::Store &store, ecs::Entity instance);

	// The `EditableMesh` class id, registering the tree if nobody has yet.
	//
	// @return The class id.
	// @since v0.18
	ecs::ClassId EditableMeshClass();

	// Which revision of each `EditableMesh` has a collision shape baked for it.
	//
	// **A resource and not a field on the host, which the render side's
	// `client::EditableMeshUploader` is.** An uploader belongs to a program
	// because a `render::Renderer` does; a shape table belongs to a *world*,
	// and a server holds many at once. A map on the host keyed by entity id
	// would have two worlds' meshes collide on the same key the first time two
	// of them minted the same id, which they do constantly.
	//
	// Derived state: it is registered with a reader that clears, so a world
	// restored from a snapshot bakes everything again rather than inheriting a
	// claim about shapes it does not have. See `CollisionShapes`, which is
	// cleared on load for the same reason.
	//
	// @since v0.19
	struct EditableMeshCollision {
		// One mesh that has been baked, and at which revision.
		struct Baked {
			// The `EditableMesh` instance, by complete entity id.
			uint64_t Instance = 0;

			// `EditableMesh::Revision` at the time it was baked.
			uint32_t Revision = 0;
		};

		// **A vector and a linear scan, for `CollisionShapes`' own reason** -
		// a world holds a handful of these, and the walk that reads it is
		// already walking every `EditableMesh` in the world.
		std::vector<Baked> Rows;
	};

	// Bakes a collision hull and triangle mesh for every `EditableMesh` whose
	// geometry has changed, and forgets the shapes of meshes that are gone.
	//
	// **The engine gap this closes**: a script that built geometry built
	// something that could be seen and not touched. `client::
	// EditableMeshUploader` hands the mesh to the renderer and registered
	// nothing with `CollisionShapes`, so a `MeshPart` naming a run-time mesh
	// fell back to colliding as its own bound - a box the size of the whole
	// thing. A character standing on a script-built heightfield was standing
	// *inside* that box and could not move in any direction.
	//
	// **A free function on a store rather than a class on a host**, so a
	// dedicated server can call it too. That is the half of the gap that
	// matters most: the server is the machine that decides where anybody is
	// standing, and it has no uploader at all.
	//
	// Revision-tracked, because baking is quickhull plus a triangle soup and a
	// streamed world builds a mesh a frame. A mesh whose revision has not moved
	// costs one integer compare.
	//
	// Call it wherever the geometry is settled and before physics reads it -
	// which for every host in this repository is once a tick.
	//
	// @param store The world.
	// @return How many meshes were baked or forgotten this call.
	// @since v0.19
	size_t RefreshEditableMeshCollision(ecs::Store &store);
}
