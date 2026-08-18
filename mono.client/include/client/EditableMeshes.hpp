#pragma once

// From `scene::EditableMesh`'s raw arrays to `render::MeshTable`, the
// conversion `scene` cannot make itself.
//
// **The identical split `render::ShaderLibrary` draws**, one door along: a
// world holds words a script wrote and a device turns them into something it
// can draw, and the two halves meet in `client` because that is the one tier
// that may link both `scene` and `assets`. `scene::EditableMesh`'s own header
// carries the full argument for why the conversion is not made there.
//
// @tier L12 · client

#include <engine/core/Name.hpp>

#include <cstddef>
#include <unordered_map>

namespace engine::assets {
	struct MeshData;
}

namespace engine::ecs {
	class Store;
}

namespace engine::render {
	class Renderer;
}

namespace engine::scene {
	struct EditableMesh;
}

namespace client {
	// Converts the raw arrays into the format `render::MeshTable::Add`
	// takes.
	//
	// **Free and device-free, for `render::ShaderLibrary`'s own reason** -
	// "no device anywhere in this file" is what lets a route be tested
	// without a GPU, and this is the half of the conversion that has no
	// business needing one. `EditableMeshUploader::Refresh` is the other
	// half, and it is not tested the same way: nothing in this codebase
	// unit-tests a call into `render::Renderer` itself, because there is
	// nothing to assert against without a device.
	//
	// @param mesh The world's own copy.
	// @return The converted geometry. `IsValid()` is false for a mesh with
	//         vertices and no triangle yet, which is the ordinary state
	//         right after `Instance.new("EditableMesh")`.
	// @since v0.18
	engine::assets::MeshData BuildMeshData(const engine::scene::EditableMesh &mesh);
	// Uploads every `scene::EditableMesh` whose revision has moved since the
	// last call.
	//
	// **One instance per client, matching `ShaderLibrary`'s reason.** The
	// ledger it keeps - which revision was last uploaded, per entity - is
	// what turns a per-frame walk into an integer compare for the steady
	// case, exactly as `ShaderSource::Revision` does for a compiled shader.
	class EditableMeshUploader {
	  public:
		// Walks every `EditableMesh` and uploads whichever have changed.
		//
		// **Never removes a mesh an instance stopped existing for.**
		// `render::MeshTable`'s own header says why: eviction is not
		// supported there at all, so a part naming a destroyed
		// `EditableMesh`'s content id keeps drawing whatever was last
		// uploaded under that name, harmlessly, for the life of the process
		// - the same fate an ordinary published mesh has if the part that
		// named it is the only thing that goes away.
		//
		// @param store    The world being drawn.
		// @param renderer The device to upload to.
		// @return How many meshes were built and handed to the renderer.
		size_t Refresh(engine::ecs::Store &store, engine::render::Renderer &renderer);

	  private:
		// Keyed by `ecs::Entity::Id`, matching every other entity-keyed
		// ledger in this codebase - the generation is part of the key, so an
		// index the allocator reuses after a destroy never reads as already
		// uploaded.
		std::unordered_map<uint64_t, uint32_t> Uploaded;
	};
}
