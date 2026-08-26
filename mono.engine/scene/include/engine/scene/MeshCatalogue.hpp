#pragma once

// How big each mesh a world knows about is, by name.
//
// **This exists because a part does not know what it is made of.** `Visual::
// Mesh` is a name - rule 4, so it survives a save file and a wire - and the
// geometry behind that name lives wherever the bytes were read: the renderer's
// `MeshTable` on a client, and nowhere at all on a headless server. A script
// asking `MeshPart.TrianglesCount` is asking a question about the *mesh*, and
// this is the one place in the shared tier that can answer it.
//
// **A count per mesh, never per part.** A thousand parts naming one mesh share
// one entry, and `scene/AGENTS.md` refuses the alternative in as many words: a
// derived fact cached on a row goes stale silently the first time the thing it
// was derived from changes. Here there is one copy, written where the mesh was
// read, and a part holds only the name it already held.
//
// **A triangle count is not device data**, which is what makes it legal here at
// all - apply this module's own test: a server with no graphics stack could
// produce this number and mean it, because it is `Indices.size() / 3` of a file
// on disk. What it could not produce is a GPU buffer offset, and that is why
// `render::MeshRange` stays where it is.
//
// **Nothing here reads a mesh.** `scene` depends on `core`, `ecs` and `spatial`
// and that list is not growing - see AGENTS.md - so this takes an integer from
// whoever *did* read the mesh rather than taking `assets::MeshData` and pulling
// the number out itself. The client's content pump is that caller today.
//
// @tier L7 · shared

#include <engine/core/Name.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace engine::ecs {
	class Store;
}

namespace engine::scene {

	// What a world knows about the meshes named on its parts.
	//
	// **Derived rather than authored, so it is not saved.** Its registration
	// writes nothing and reads back empty, exactly as `engine::render::DrawList`'s
	// does: the contents come from whatever registered the meshes this run, and
	// a save file carrying last run's counts would be a number that agrees with
	// nothing on disk. A world reloaded with no content attached honestly knows
	// nothing, which is the same answer it gives before content arrives.
	//
	// @since v0.9
	struct MeshCatalogue {
		// Triangles per mesh, keyed by `core::Name::Id`.
		//
		// The id rather than the `Name`, matching `render::MeshTable::Entries`:
		// a `Name` is already an integer in this process and hashing the
		// integer skips the registry lock that comparing text would take.
		std::unordered_map<uint32_t, uint32_t> Triangles;

		// How many triangles a mesh has, or zero when this world has not been
		// told.
		//
		// **Zero means "not known here", not "empty".** A mesh with no
		// triangles is not a thing a publisher produces - `assets::Mesh::Read`
		// refuses one - so the two cases cannot be confused, and the honest
		// answer on a headless server is the same as the honest answer before
		// the content pump has run.
		//
		// @param mesh The mesh's name.
		// @return The count, or zero.
		uint32_t Find(const core::Name &mesh) const;

		// The sheets a mesh's own submeshes name, keyed the same way.
		//
		// **What a script cannot otherwise find out.** A `MeshPart` naming no
		// `TextureID` wears whatever each submesh recorded at bake time, and
		// those names live *inside* the mesh file - so an author who wants to
		// swap a model's sheet has no way to learn what it is wearing, and no
		// name to put back. Recorded here at intake, where the file is open and
		// the names are readable, exactly as the triangle count is.
		//
		// **In submesh order and duplicates kept.** A character with twenty
		// submeshes sharing four sheets is four names repeated, and collapsing
		// them would lose which run wears which - a fact the day something wants
		// per-submesh overrides.
		//
		// @since v0.13
		std::unordered_map<uint32_t, std::vector<core::Name>> Textures;

		// The sheets a mesh names, or an empty span.
		//
		// **Empty means "not known here", like `Find`'s zero.** A mesh whose
		// submeshes name nothing is a real thing - every built-in is one - so
		// this cannot distinguish that from a mesh nothing has recorded, and
		// says so rather than implying otherwise.
		//
		// @param mesh The mesh's name.
		// @return Its sheets, valid until the catalogue is next written.
		// @since v0.13
		std::span<const core::Name> Sheets(const core::Name &mesh) const;
	};

	// The world's catalogue, creating an empty one if it has none.
	//
	// **`RegisterSceneComponents` must have run first**, as it must before any
	// resource here is set. `SetResource` keys on a component id, and one
	// minted before the explicit registration lands takes the compiler's
	// spelling of the type - which aborts the whole process when the
	// registration finally arrives, at a call site with nothing to do with this
	// one. It is order-dependent, so it passes most runs.
	//
	// @param store The world.
	// @return The catalogue.
	MeshCatalogue &MeshesOf(ecs::Store &store);

	// Records how many triangles a mesh has.
	//
	// **Last writer wins, and re-registering is legal**, because that is what
	// the content path does: a publisher may replace a mesh under a name it
	// already used, and a catalogue that refused the second one would keep
	// answering with the geometry that is no longer drawn.
	//
	// @param store     The world.
	// @param mesh      The mesh's name.
	// @param triangles How many triangles it has.
	// @param sheets    The textures its submeshes name, in submesh order.
	//        Replaces whatever was recorded, because a republished mesh may
	//        name different ones and a merge would leave a sheet listed that
	//        the geometry no longer wears.
	// @return `false` for an invalid name.
	bool RecordMesh(
		ecs::Store &store, const core::Name &mesh, uint32_t triangles, std::span<const core::Name> sheets = {}
	);

	// The sheets a mesh's submeshes name, in this world.
	//
	// The `const` reader, like `TrianglesOf`: it never creates the resource, so
	// a property getter and a script binding can both use it.
	//
	// @param store The world.
	// @param mesh  The mesh's name.
	// @param out   Filled with the sheets. Cleared first.
	// @return How many there are.
	// @since v0.13
	size_t SheetsOf(const ecs::Store &store, const core::Name &mesh, std::vector<core::Name> &out);

	// How many triangles a mesh has in this world, or zero.
	//
	// The `const` reader, so a property getter can use it: it never creates the
	// resource, and a world that has none answers zero rather than acquiring an
	// empty one from inside a read.
	//
	// @param store The world.
	// @param mesh  The mesh's name.
	// @return The count, or zero.
	uint32_t TrianglesOf(const ecs::Store &store, const core::Name &mesh);
}
