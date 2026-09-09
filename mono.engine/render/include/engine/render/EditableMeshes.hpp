#pragma once

// From `scene::EditableMesh`'s raw arrays to `render::MeshTable`, the
// conversion `scene` cannot make itself.
//
// **The identical split `ShaderLibrary` draws**, one door along: a world holds
// words a script wrote and a device turns them into something it can draw. The
// render module already owns both device tables and is the lowest client-tier
// layer allowed to combine scene rows with baked asset layouts.
//
// @tier L12 · client

#include <engine/core/Name.hpp>

#include <cstddef>
#include <unordered_map>
#include <vector>

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

namespace engine::render {
	// Converts the raw arrays into the format `render::MeshTable::Add`
	// takes.
	//
	// Conversion is device-free and covered by host tests. Upload ownership
	// is checked separately with a real renderer and device.
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
	// One instance per renderer tracks revisions separately for each store and
	// owner, so switching presented worlds preserves their upload stamps.
	class EditableMeshUploader {
	  public:
		// Walks every `EditableMesh` and uploads whichever have changed.
		//
		// The owner scopes the generated content names as well as upload tracking.
		// Use distinct owners for worlds whose editable entity handles can collide.
		// Destroying an entity retains its last uploaded resource until owner retirement.
		//
		// @param store    The world being drawn.
		// @param renderer The device to upload to.
		// @param owner The residency namespace for generated content names.
		// @return How many meshes were built and handed to the renderer.
		size_t Refresh(engine::ecs::Store &store, engine::render::Renderer &renderer, core::Name owner = {});

		// Forget device upload stamps when a world or residency owner retires.
		// This does not release resources; Renderer owns their lifetime.
		void ForgetWorld(uint64_t identity);
		void ForgetOwner(core::Name owner);

	  private:
		struct UploadScope {
			uint64_t World = 0;
			core::Name Owner;
			std::unordered_map<uint64_t, uint32_t> Revisions;
		};
		std::vector<UploadScope> Scopes;
	};
}
