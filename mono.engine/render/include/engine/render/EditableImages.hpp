#pragma once

// From `scene::EditableImage`'s raw pixels to `render::TextureTable`, the
// conversion `scene` cannot make itself.
//
// **`EditableMeshUploader`'s exact shape, one dimension down.** The render
// module already owns both device tables and is the lowest client-tier layer
// allowed to combine scene rows with baked asset layouts.
//
// @tier L12 · client

#include <engine/core/Name.hpp>

#include <cstddef>
#include <unordered_map>

namespace engine::assets {
	struct TextureData;
}

namespace engine::ecs {
	class Store;
}

namespace engine::render {
	class Renderer;
}

namespace engine::scene {
	struct EditableImage;
}

namespace engine::render {
	// Converts the raw pixel buffer into the format `render::TextureTable`
	// takes.
	//
	// **Free and device-free**, `BuildMeshData`'s own reason: the
	// layouts already agree byte for byte, so this is a copy rather than a
	// conversion, and it is the half worth testing without a GPU.
	//
	// @param image The world's own copy.
	// @return The converted texture. Always valid for a genuine
	//         `scene::EditableImage`, whose own doors keep `Pixels.size()`
	//         equal to `Width * Height * 4` at every return.
	// @since v0.18
	engine::assets::TextureData BuildTextureData(const engine::scene::EditableImage &image);

	// Uploads every `scene::EditableImage` whose revision has moved since
	// the last call.
	//
	// @since v0.18
	class EditableImageUploader {
	  public:
		// Walks every `EditableImage` and uploads whichever have changed.
		//
		// **Never removes a texture an instance stopped existing for** -
		// `render::TextureTable` has no eviction, `EditableMeshUploader::
		// Refresh`'s own reason applies unchanged.
		//
		// @param store    The world being drawn.
		// @param renderer The device to upload to.
		// @return How many textures were built and handed to the renderer.
		size_t Refresh(engine::ecs::Store &store, engine::render::Renderer &renderer);

	  private:
		std::unordered_map<uint64_t, uint32_t> Uploaded;
	};
}
