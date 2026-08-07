#pragma once

// Renderer textures are named device resources with one shared sampler.
// Mipmaps are not represented by the current asset format.
//
// @tier L12 · client

#include <engine/assets/Texture.hpp>
#include <engine/core/Name.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>
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

		// Takes the device, creates the shared sampler and uploads the default.
		//
		// @param device The GPU device. Kept, not owned.
		// @return `false` when the sampler or the default could not be created.
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
		// **Stays honest about absence**, which is what makes it usable for the
		// two callers that need to tell "not registered" from "registered as
		// something": a thumbnail that has not been built and a particle run
		// whose sheet has not streamed both want null. A caller that wants a
		// picture instead of an answer asks `Default()` for one.
		//
		// @param name The name.
		// @return The texture, or null.
		SDL_GPUTexture *Find(const core::Name &name) const;

		// What to sample when a drawable names no texture, or names one that is
		// not here.
		//
		// **A real texture and not the one white texel.** `render/AGENTS.md` and
		// `DefaultTexture.hpp` carry the argument: a fallback binding exists so a
		// sampler is not reading uninitialised memory, and a *default material*
		// is what an author sees on a part they have not textured. Conflating the
		// two is why every untextured part in the engine was flat white.
		//
		// **Held apart from the map rather than under a reserved name**, so no
		// `Add` can replace it and no `Drop` can release it. A name would have to
		// be one content could never spell, and a rule like that is only as good
		// as the next person who reads it.
		//
		// @return The default texture. Null only before `Initialise`.
		// @since v0.10
		SDL_GPUTexture *Default() const {
			return DefaultHandle;
		}

		// How big a registered texture is, in source pixels.
		//
		// @param name   The name.
		// @param width  Set to the width, or left alone when the name is absent.
		// @param height Set to the height, likewise.
		// @return `false` for a name this table does not hold.
		// @since v0.10
		bool SizeOf(const core::Name &name, uint32_t &width, uint32_t &height) const;

		// The shared sampler.
		SDL_GPUSampler *Sampler() const {
			return SharedSampler;
		}

		// How many textures are registered.
		size_t Count() const {
			return Textures.size();
		}

		// Forgets a texture and releases it.
		//
		// **Because a thumbnail cache has to have a ceiling.** Everything else
		// here is content that lives as long as the session; a preview is built
		// for a row somebody scrolled past, and a table that only ever grew
		// would hold a store's worth of images in video memory by the time they
		// had browsed it.
		//
		// @param name The name to drop.
		// @return `false` for a name this table does not hold.
		// @since v0.10
		bool Drop(const core::Name &name);

		// How many bytes of device memory the table has uploaded.
		size_t Bytes() const {
			return UploadedBytes;
		}

	  private:
		// One registered texture and what it cost.
		//
		// **The size is held per texture rather than only summed**, which it was
		// not before `Drop` existed — and the sum was wrong because of it:
		// replacing a texture under a name added the new size and never
		// subtracted the old, so a session that re-registered content drifted
		// upward until `MAXIMUM_BYTES` refused an upload that would have fit.
		// Nothing noticed, because nothing replaced a texture often.
		struct Entry {
			SDL_GPUTexture *Texture = nullptr;
			size_t Bytes = 0;

			// **What was uploaded, because a caller cannot ask the device.** A
			// nine-sliced or tiled `ImageLabel` is laid out in *source* pixels —
			// `gui::DrawCommand`'s slice insets are in them — so a painter
			// resolving a name to a handle needs the dimensions with it or it
			// draws every slice at the wrong scale.
			uint32_t Width = 0;
			uint32_t Height = 0;
		};

		// Creates one device texture and fills it, widening `R8` on the way.
		//
		// **Shared by `Add` and by the default's upload**, because two copies of
		// a create-transfer-copy-submit sequence is two chances to get the row
		// pitch wrong and only one of them under test.
		//
		// @param image  The pixels. Assumed valid; callers check.
		// @param label  What to name in a log line if it fails.
		// @param bytes  Set to what the upload cost in device memory.
		// @return The texture, or null.
		SDL_GPUTexture *Upload(const assets::TextureData &image, std::string_view label, size_t &bytes);

		SDL_GPUDevice *Device = nullptr;
		SDL_GPUSampler *SharedSampler = nullptr;

		// The default, outside `Textures` on purpose — see `Default()`. Its
		// bytes are not counted against `MAXIMUM_BYTES`: it is sixteen kilobytes
		// the engine always holds, and a ceiling that content can spend should
		// not shrink by a constant nobody can see.
		SDL_GPUTexture *DefaultHandle = nullptr;

		std::unordered_map<uint32_t, Entry> Textures;
		size_t UploadedBytes = 0;
	};
}
