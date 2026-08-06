#pragma once

// Every texture the renderer can sample, by name.
//
// `MeshTable`'s twin, and deliberately much simpler: a texture is its own
// device resource, so there is no packing decision to make. What this owns is
// the name-to-handle map and the lifetime.
//
// **One sampler for all of them.** A sampler is filtering and wrapping and
// nothing else, and every texture in this engine wants the same two — linear
// and repeat. A sampler per texture would be a device object per asset for a
// setting nothing authors yet.
//
// **No mipmaps, and that is a stated gap rather than an oversight.** A 2048
// sheet minified onto forty pixels aliases into shimmer, and the fix is a mip
// chain generated at bake time — `bake::ResizeImage` is already the box filter
// that would build one. It is not here because the format has no place to put
// the levels: `assets::Texture` is one image, and adding a chain is a format
// change that should arrive with the sampler work rather than ahead of it.
//
// @tier L12 · client

#include <engine/assets/Texture.hpp>
#include <engine/core/Name.hpp>

#include <cstddef>
#include <cstdint>
#include <unordered_map>

struct SDL_GPUDevice;
struct SDL_GPUSampler;
struct SDL_GPUTexture;

namespace engine::render {

	// The textures a renderer can sample.
	//
	// @client
	// @since v0.9
	class TextureTable {
	  public:
		// How much device memory the table will hold, in bytes.
		//
		// A ceiling on what content can make the renderer allocate, for
		// `MeshTable::MAXIMUM_VERTICES`'s reason. Half a gigabyte is thirty-two
		// 2048-pixel sheets, which is a character-heavy scene and well short of
		// what a modest card has.
		static constexpr size_t MAXIMUM_BYTES = 512u * 1024u * 1024u;

		TextureTable() = default;
		~TextureTable();

		TextureTable(const TextureTable &) = delete;
		TextureTable &operator=(const TextureTable &) = delete;

		// Takes the device and creates the shared sampler.
		//
		// @param device The GPU device. Kept, not owned.
		// @return `false` when the sampler could not be created.
		bool Initialise(SDL_GPUDevice *device);

		// Releases every texture and the sampler.
		void Shutdown();

		// Uploads a texture under a name, replacing one already there.
		//
		// Uploaded immediately rather than deferred to a flush, unlike
		// `MeshTable`: a texture is its own resource, so there is no shared
		// buffer to rebuild and nothing to batch.
		//
		// @param name  The name a `SurfaceAppearance` or a submesh will ask
		//              for.
		// @param image The pixels. An invalid one is refused.
		// @return `false` for an invalid image, a full table or a failed
		//         upload.
		bool Add(const core::Name &name, const assets::TextureData &image);

		// The texture for a name, or null when it is not registered.
		//
		// **Null rather than a fallback, unlike `MeshTable::Resolve`.** The two
		// differ because the consequences differ: a missing mesh has to draw as
		// *something* or the object vanishes, while a missing texture has an
		// honest answer already — the submesh's flat base colour, which is what
		// an untextured model is meant to look like.
		//
		// @param name The name.
		// @return The texture, or null.
		SDL_GPUTexture *Find(const core::Name &name) const;

		// The shared sampler.
		SDL_GPUSampler *Sampler() const {
			return SharedSampler;
		}

		// How many textures are registered.
		size_t Count() const {
			return Textures.size();
		}

		// How many bytes of device memory the table has uploaded.
		size_t Bytes() const {
			return UploadedBytes;
		}

	  private:
		SDL_GPUDevice *Device = nullptr;
		SDL_GPUSampler *SharedSampler = nullptr;
		std::unordered_map<uint32_t, SDL_GPUTexture *> Textures;
		size_t UploadedBytes = 0;
	};
}
