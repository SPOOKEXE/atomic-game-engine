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
#include <vector>

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
	// TextureTable currently samples only native RGBA8/R8 content. Keeping this
	// result explicit prevents an authored compact image policy from silently
	// changing channel interpretation while the renderer still uploads RGBA8.
	enum class EditableImagePackingSupport : uint8_t {
		NativeRGBA8,
		UnsupportedFormat,
		UnsupportedAttributes,
	};

	EditableImagePackingSupport EditableImagePackingSupportOf(const engine::scene::EditableImage &image);

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
		// The owner scopes the generated content names as well as upload tracking.
		// Use distinct owners for worlds whose editable entity handles can collide.
		// Destroying an entity retains its last uploaded resource until owner retirement.
		//
		// @param store    The world being drawn.
		// @param renderer The device to upload to.
		// @param owner The residency namespace for generated content names.
		// @return How many textures were built and handed to the renderer.
		size_t Refresh(engine::ecs::Store &store, engine::render::Renderer &renderer, core::Name owner = {});

		// Forget device upload stamps when a world or residency owner retires.
		// This does not release resources; Renderer owns their lifetime.
		void ForgetWorld(uint64_t identity);
		void ForgetOwner(core::Name owner);

	  private:
		struct UploadScope {
			uint64_t World = 0;
			core::Name Owner;
			struct Revision {
				uint32_t Image = 0;
				uint32_t Packing = 0;

				bool operator==(const Revision &) const = default;
			};
			std::unordered_map<uint64_t, Revision> Revisions;
		};
		std::vector<UploadScope> Scopes;
	};
}
