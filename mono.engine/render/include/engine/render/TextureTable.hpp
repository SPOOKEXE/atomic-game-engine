#pragma once

// Renderer textures are named device resources with one shared sampler.
// Mipmaps are not represented by the current asset format.
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
		// Bounds device memory reachable from content.
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
		// Uploads immediately because textures are independent resources.
		//
		// @param name  The name a `SurfaceAppearance` or a submesh will ask
		//              for.
		// @param image The pixels. An invalid one is refused.
		// @return `false` for an invalid image, a full table or a failed
		//         upload.
		bool Add(const core::Name &name, const assets::TextureData &image);

		// The texture for a name, or null when it is not registered.
		//
		// Missing textures return null; the material supplies the base colour.
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
