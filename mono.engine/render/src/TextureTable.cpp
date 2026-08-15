#include <engine/assets/Builtin.hpp>
#include <engine/core/Log.hpp>
#include <engine/render/DefaultTexture.hpp>
#include <engine/render/MissingTexture.hpp>
#include <engine/render/TextureTable.hpp>

#include <SDL3/SDL_gpu.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace engine::render {

	TextureTable::~TextureTable() {
		Shutdown();
	}

	bool TextureTable::Initialise(SDL_GPUDevice *device) {
		Device = device;
		if (Device == nullptr) {
			return false;
		}

		SDL_GPUSamplerCreateInfo sampler{};
		sampler.min_filter = SDL_GPU_FILTER_LINEAR;
		sampler.mag_filter = SDL_GPU_FILTER_LINEAR;
		sampler.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;

		// Imported model coordinates may exceed one; repeat avoids edge smearing.
		sampler.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
		sampler.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
		sampler.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;

		// **`max_lod` is the field that turns the chain on**, and leaving it at
		// its zero-initialised value is the whole of how a mip chain gets built,
		// uploaded and never sampled: `mipmap_mode` has said LINEAR since v0.8,
		// and a clamp of zero pins every fetch to level zero regardless. The
		// bound is past `assets::MipLevelCount`'s largest answer, which is what
		// "as many levels as the texture has" is spelled as here - SDL takes a
		// LOD clamp rather than a level count, so a per-texture number would have
		// to be a per-texture sampler.
		sampler.min_lod = 0.0f;
		sampler.max_lod = 32.0f;

		SharedSampler = SDL_CreateGPUSampler(Device, &sampler);
		if (SharedSampler == nullptr) {
			ENGINE_ERROR("texture table: sampler: {}", SDL_GetError());
			return false;
		}

		// **Uploaded here rather than lazily**, so the first frame that draws an
		// untextured part costs a lookup and not a create-and-copy - and so a
		// device that cannot make a 64-pixel texture fails at start-up rather
		// than at the moment somebody selects a part.
		size_t defaultBytes = 0;
		DefaultHandle = Upload(DefaultTexture(), "default", defaultBytes);
		if (DefaultHandle == nullptr) {
			return false;
		}

		// **And the marker beside it, for the same reason twice over.** It is
		// wanted at exactly the moment content is failing to arrive, which is
		// the worst moment to discover the device cannot make a texture.
		size_t missingBytes = 0;
		MissingHandle = Upload(MissingTexture(), "missing", missingBytes);
		if (MissingHandle == nullptr) {
			return false;
		}

		// **The named built-ins, so that a sheet an author can *choose* is here
		// before any content is.** `MeshTable::Initialise` registers the six
		// built-in shapes for the same reason and in the same place: a name that
		// resolves without a store, a publish or a fetch is the only thing an
		// editor can offer on a machine that has none of the three.
		//
		// Registered as ordinary entries rather than as another special handle -
		// `engine.Checker` is content that happens to be generated, and a part
		// naming it takes the path every other texture takes.
		for (uint8_t index = 0; index < assets::BUILTIN_TEXTURE_COUNT; index++) {
			const auto builtin = static_cast<assets::BuiltinTexture>(index);
			if (!Add(core::Name(assets::BuiltinName(builtin)), assets::MakeBuiltin(builtin))) {
				ENGINE_ERROR("texture table: built-in {} was refused", assets::BuiltinName(builtin));
				return false;
			}
		}
		return true;
	}

	void TextureTable::Shutdown() {
		if (Device != nullptr) {
			for (const auto &[name, entry] : Textures) {
				SDL_ReleaseGPUTexture(Device, entry.Texture);
			}
			if (DefaultHandle != nullptr) {
				SDL_ReleaseGPUTexture(Device, DefaultHandle);
			}
			if (MissingHandle != nullptr) {
				SDL_ReleaseGPUTexture(Device, MissingHandle);
			}
			if (SharedSampler != nullptr) {
				SDL_ReleaseGPUSampler(Device, SharedSampler);
			}
		}

		Textures.clear();
		Awaiting.clear();
		DefaultHandle = nullptr;
		MissingHandle = nullptr;
		SharedSampler = nullptr;
		UploadedBytes = 0;
		Device = nullptr;
	}

	SDL_GPUTexture *
	TextureTable::Upload(const assets::TextureData &image, std::string_view label, size_t &bytes) {
		const uint32_t levels = image.LevelCount();

		// **Every level staged back to back in one buffer**, so a chain costs one
		// transfer allocation and one copy pass rather than fifteen of each. The
		// offset walks it, which is the only reason `SDL_GPUTextureTransferInfo`
		// carries one. Packed rather than padded to D3D12's 512-byte placement
		// alignment: SDL's backend inserts a staging copy for a level that lands
		// unaligned, and paying that on the two or three smallest levels is
		// cheaper than the padding on every level of every texture.
		size_t uploadBytes = 0;
		for (uint32_t level = 0; level < levels; level++) {
			uploadBytes += static_cast<size_t>(assets::MipExtent(image.Width, level)) *
						   assets::MipExtent(image.Height, level) * 4;
		}

		// Expand R8 assets so every texture uses the pipeline's RGBA format.
		std::vector<std::byte> staged(uploadBytes);
		size_t written = 0;
		for (uint32_t level = 0; level < levels; level++) {
			const std::vector<std::byte> &source = level == 0 ? image.Pixels : image.Mips[level - 1];
			if (image.Format == assets::TextureFormat::R8) {
				for (size_t index = 0; index < source.size(); index++) {
					staged[written + index * 4] = source[index];
					staged[written + index * 4 + 1] = source[index];
					staged[written + index * 4 + 2] = source[index];
					staged[written + index * 4 + 3] = std::byte{255};
				}
				written += source.size() * 4;
				continue;
			}
			std::memcpy(staged.data() + written, source.data(), source.size());
			written += source.size();
		}

		SDL_GPUTextureCreateInfo info{};
		info.type = SDL_GPU_TEXTURETYPE_2D;
		info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
		info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
		info.width = image.Width;
		info.height = image.Height;
		info.layer_count_or_depth = 1;
		info.num_levels = levels;
		info.sample_count = SDL_GPU_SAMPLECOUNT_1;

		SDL_GPUTexture *texture = SDL_CreateGPUTexture(Device, &info);
		if (texture == nullptr) {
			ENGINE_ERROR("texture table: {}: {}", label, SDL_GetError());
			return nullptr;
		}

		SDL_GPUTransferBufferCreateInfo transferInfo{};
		transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
		transferInfo.size = static_cast<uint32_t>(uploadBytes);

		SDL_GPUTransferBuffer *transfer = SDL_CreateGPUTransferBuffer(Device, &transferInfo);
		if (transfer == nullptr) {
			ENGINE_ERROR("texture table: transfer buffer: {}", SDL_GetError());
			SDL_ReleaseGPUTexture(Device, texture);
			return nullptr;
		}

		void *mapped = SDL_MapGPUTransferBuffer(Device, transfer, false);
		std::memcpy(mapped, staged.data(), uploadBytes);
		SDL_UnmapGPUTransferBuffer(Device, transfer);

		SDL_GPUCommandBuffer *command = SDL_AcquireGPUCommandBuffer(Device);
		SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(command);

		size_t offset = 0;
		for (uint32_t level = 0; level < levels; level++) {
			const uint32_t width = assets::MipExtent(image.Width, level);
			const uint32_t height = assets::MipExtent(image.Height, level);

			SDL_GPUTextureTransferInfo source{};
			source.transfer_buffer = transfer;
			source.offset = static_cast<uint32_t>(offset);
			source.pixels_per_row = width;
			source.rows_per_layer = height;

			SDL_GPUTextureRegion region{};
			region.texture = texture;
			region.mip_level = level;
			region.w = width;
			region.h = height;
			region.d = 1;

			SDL_UploadToGPUTexture(copy, &source, &region, false);
			offset += static_cast<size_t>(width) * height * 4;
		}

		SDL_EndGPUCopyPass(copy);
		SDL_SubmitGPUCommandBuffer(command);
		SDL_ReleaseGPUTransferBuffer(Device, transfer);

		bytes = uploadBytes;
		return texture;
	}

	TextureTable::Entry
	TextureTable::Describe(SDL_GPUTexture *texture, size_t bytes, const assets::TextureData &image) {
		// **One description, because there are two call sites.** `Add` replaces
		// an entry or inserts one, and two aggregate initialisers is two places
		// to forget a field - which is exactly how the sheet layout would go
		// missing on a *replaced* texture only, so an animation would play until
		// a publisher re-published it and then stop.
		return Entry{
			.Texture = texture,
			.Bytes = bytes,
			.Width = image.Width,
			.Height = image.Height,
			.FlipbookSide = image.FlipbookSide,
			.FlipbookFrames = image.FlipbookFrames,
			.FlipbookFrameRate = image.FlipbookFrameRate,
		};
	}

	FlipbookCell TextureTable::CellOf(const core::Name &name, double seconds) const {
		if (!name.IsValid()) {
			return {};
		}
		const auto found = Textures.find(name.Id());
		if (found == Textures.end()) {
			return {};
		}
		return FlipbookCellAt(
			found->second.FlipbookSide, found->second.FlipbookFrames, found->second.FlipbookFrameRate, seconds
		);
	}

	bool TextureTable::Add(const core::Name &name, const assets::TextureData &image) {
		if (Device == nullptr || !name.IsValid() || !image.IsValid()) {
			return false;
		}

		// The chain counts against the ceiling too - it is a third of a texture's
		// device memory, and a pre-check that ignored it would let a full table
		// take an upload it then could not afford.
		size_t bytes = image.Pixels.size();
		for (const std::vector<std::byte> &level : image.Mips) {
			bytes += level.size();
		}
		if (UploadedBytes + bytes > MAXIMUM_BYTES) {
			ENGINE_WARN("texture table: full, refusing {}", name.Text());
			return false;
		}

		size_t uploadBytes = 0;
		SDL_GPUTexture *texture = Upload(image, name.Text(), uploadBytes);
		if (texture == nullptr) {
			return false;
		}

		// Release the old texture only after the replacement upload succeeds.
		const auto existing = Textures.find(name.Id());
		if (existing != Textures.end()) {
			SDL_ReleaseGPUTexture(Device, existing->second.Texture);

			// **The old size comes off before the new one goes on**, which it
			// did not before: the total only ever grew, so a session that
			// replaced textures drifted up until the ceiling refused an upload
			// that would have fit.
			UploadedBytes -= std::min(UploadedBytes, existing->second.Bytes);
			existing->second = Describe(texture, uploadBytes, image);
		} else {
			Textures.emplace(name.Id(), Describe(texture, uploadBytes, image));
		}

		UploadedBytes += uploadBytes;

		// Arrived, so nothing is coming for it any more. Done here rather than
		// left to the caller because there is no path where a registered
		// texture is still in flight, and a rule the type enforces is one no
		// host can forget.
		Awaiting.erase(name.Id());
		return true;
	}

	SDL_GPUTexture *TextureTable::Find(const core::Name &name) const {
		if (!name.IsValid()) {
			return nullptr;
		}
		const auto found = Textures.find(name.Id());
		return found == Textures.end() ? nullptr : found->second.Texture;
	}

	bool TextureTable::SizeOf(const core::Name &name, uint32_t &width, uint32_t &height) const {
		if (!name.IsValid()) {
			return false;
		}
		const auto found = Textures.find(name.Id());
		if (found == Textures.end()) {
			return false;
		}
		width = found->second.Width;
		height = found->second.Height;
		return true;
	}

	bool TextureTable::Adopt(
		const core::Name &name, SDL_GPUTexture *texture, uint32_t width, uint32_t height, size_t bytes
	) {
		if (Device == nullptr || !name.IsValid() || texture == nullptr) {
			return false;
		}

		// **Refused before the swap, so a full table leaves the caller its
		// texture.** The alternative - release the old entry and then discover
		// there is no room - would drop a working picture to make space for one
		// that is not going in.
		if (UploadedBytes + bytes > MAXIMUM_BYTES) {
			ENGINE_WARN("texture table: full, refusing {}", name.Text());
			return false;
		}

		Entry entry;
		entry.Texture = texture;
		entry.Bytes = bytes;
		entry.Width = width;
		entry.Height = height;

		// No flipbook fields: a rendered picture is one frame by construction,
		// and claiming a grid would make `FlipbookCell` walk cells that are not
		// there.

		const auto existing = Textures.find(name.Id());
		if (existing != Textures.end()) {
			SDL_ReleaseGPUTexture(Device, existing->second.Texture);
			UploadedBytes -= std::min(UploadedBytes, existing->second.Bytes);
			existing->second = entry;
		} else {
			Textures.emplace(name.Id(), entry);
		}

		UploadedBytes += bytes;

		// Arrived, so nothing is coming for it any more. Done here rather than
		// left to the caller because there is no path where a registered
		// texture is still in flight, and a rule the type enforces is one no
		// host can forget.
		Awaiting.erase(name.Id());
		return true;
	}

	void TextureTable::Expect(const core::Name &name) {
		if (!name.IsValid()) {
			return;
		}

		// **Not conditioned on the table already holding it.** Content is
		// republished, and a name asked for again while the old texture is
		// still registered is still in flight - refusing the mark here would
		// make the second fetch invisible for no gain.
		Awaiting.insert(name.Id());
	}

	void TextureTable::StopExpecting(const core::Name &name) {
		if (name.IsValid()) {
			Awaiting.erase(name.Id());
		}
	}

	bool TextureTable::Expecting(const core::Name &name) const {
		return name.IsValid() && Awaiting.find(name.Id()) != Awaiting.end();
	}

	bool TextureTable::Drop(const core::Name &name) {
		if (Device == nullptr || !name.IsValid()) {
			return false;
		}

		const auto found = Textures.find(name.Id());
		if (found == Textures.end()) {
			return false;
		}

		SDL_ReleaseGPUTexture(Device, found->second.Texture);
		UploadedBytes -= std::min(UploadedBytes, found->second.Bytes);
		Textures.erase(found);
		return true;
	}
}
