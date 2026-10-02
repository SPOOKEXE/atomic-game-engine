#include "TextureFormatSupport.hpp"

#include <engine/assets/Texture.hpp>
#include <engine/render/LiveImagePublisher.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/render/TextureTable.hpp>

#include <array>
#include <limits>
#include <new>
#include <vector>

namespace engine::render {
	namespace {
		assets::TextureFormat PublishedFormat(assets::TextureFormat format, LiveImageColorSpace colorSpace) {
			if (format == assets::TextureFormat::RGBA8 || format == assets::TextureFormat::RGBA8_LINEAR)
				return colorSpace == LiveImageColorSpace::Display ? assets::TextureFormat::RGBA8
																  : assets::TextureFormat::RGBA8_LINEAR;
			if (format == assets::TextureFormat::RGBA4_UNORM || format == assets::TextureFormat::RGBA4_SRGB)
				return colorSpace == LiveImageColorSpace::Display ? assets::TextureFormat::RGBA4_SRGB
																  : assets::TextureFormat::RGBA4_UNORM;
			return format;
		}

		bool ValidUpload(
			SDL_GPUDevice *device,
			uint32_t width,
			uint32_t height,
			std::span<const std::byte> pixels,
			LiveImageColorSpace colorSpace,
			assets::TextureFormat format
		) {
			const assets::TextureFormat published = PublishedFormat(format, colorSpace);
			const uint32_t bytesPerPixel = assets::BytesPerPixel(published);
			const uint64_t pixelCount = uint64_t(width) * height;
			if (width == 0 || height == 0 || width > LiveImagePublisher::MAXIMUM_SIDE ||
				height > LiveImagePublisher::MAXIMUM_SIDE || bytesPerPixel == 0 ||
				pixelCount > std::numeric_limits<uint64_t>::max() / bytesPerPixel ||
				pixelCount * bytesPerPixel != pixels.size() ||
				pixelCount * bytesPerPixel > LiveImagePublisher::MAXIMUM_IMAGE_BYTES ||
				(colorSpace != LiveImageColorSpace::Display && colorSpace != LiveImageColorSpace::Linear) ||
				!detail::TextureFormatForUpload(published))
				return false;
			const auto support = detail::TextureFormatForUpload(published);
			const uint64_t uploadBytes = pixelCount * support->UploadBytesPerPixel;
			return uploadBytes <= LiveImagePublisher::MAXIMUM_IMAGE_BYTES &&
				   detail::SupportsTextureFormat(device, published);
		}
	}

	uint64_t LiveImagePublisher::Key(core::Name owner, core::Name name) {
		return (uint64_t(owner.Id()) << 32) | name.Id();
	}

	std::optional<LiveImageBinding> LiveImagePublisher::BeginBinding(core::Name owner, core::Name name) {
		if (!owner.IsValid() || !name.IsValid() || NextGeneration == 0) {
			return std::nullopt;
		}

		const uint64_t key = Key(owner, name);
		auto binding = Generations.find(key);
		if (binding == Generations.end() && Generations.size() >= MAXIMUM_BINDINGS) {
			return std::nullopt;
		}

		const uint64_t generation = NextGeneration;
		NextGeneration = generation == std::numeric_limits<uint64_t>::max() ? 0 : generation + 1;
		if (binding == Generations.end()) {
			Generations.emplace(key, generation);
		} else {
			// A new binding invalidates queued work from the previous one. The old
			// texture remains available until AddTexture accepts its replacement.
			binding->second = generation;
		}
		return LiveImageBinding{owner, name, generation};
	}

	LiveImagePublishStatus LiveImagePublisher::Publish(
		Renderer &renderer,
		const LiveImageBinding &binding,
		uint32_t width,
		uint32_t height,
		std::span<const std::byte> pixels,
		LiveImageColorSpace colorSpace,
		assets::TextureFormat format
	) {
		if (!binding.Owner.IsValid() || !binding.Name.IsValid() || binding.Generation == 0) {
			return LiveImagePublishStatus::Invalid;
		}

		const uint64_t key = Key(binding.Owner, binding.Name);
		auto generation = Generations.find(key);
		if (generation == Generations.end() || binding.Generation != generation->second) {
			return LiveImagePublishStatus::Stale;
		}

		const assets::TextureFormat published = PublishedFormat(format, colorSpace);
		if (!ValidUpload(
				renderer.Backend().Device ? static_cast<SDL_GPUDevice *>(renderer.Backend().Device) : nullptr,
				width,
				height,
				pixels,
				colorSpace,
				published
			)) {
			return LiveImagePublishStatus::Invalid;
		}

		assets::TextureData image;
		image.Width = width;
		image.Height = height;
		image.Format = published;
		try {
			image.Pixels.assign(pixels.begin(), pixels.end());
		} catch (const std::bad_alloc &) {
			return LiveImagePublishStatus::UploadFailed;
		}

		return renderer.AddTexture(binding.Name, image, binding.Owner) ? LiveImagePublishStatus::Published
																	   : LiveImagePublishStatus::UploadFailed;
	}

	LiveImagePublishStatus
	LiveImagePublisher::PublishBatch(Renderer &renderer, std::span<const LiveImageUpload> images) {
		if (images.empty() || images.size() > 6) return LiveImagePublishStatus::Invalid;
		const core::Name owner = images.front().Binding.Owner;
		std::array<assets::TextureData, 6> prepared;
		std::array<TextureBatchImage, 6> batch{};
		std::array<assets::TextureFormat, 6> formats{};
		for (size_t index = 0; index < images.size(); ++index) {
			const LiveImageUpload &source = images[index];
			const LiveImageBinding &binding = source.Binding;
			if (!owner.IsValid() || binding.Owner != owner || !binding.Name.IsValid() ||
				binding.Generation == 0)
				return LiveImagePublishStatus::Invalid;
			for (size_t earlier = 0; earlier < index; ++earlier)
				if (images[earlier].Binding.Name == binding.Name) return LiveImagePublishStatus::Invalid;
			const auto held = Generations.find(Key(owner, binding.Name));
			if (held == Generations.end() || held->second != binding.Generation)
				return LiveImagePublishStatus::Stale;
			formats[index] = PublishedFormat(source.Format, source.ColorSpace);
			if (!ValidUpload(
					renderer.Backend().Device ? static_cast<SDL_GPUDevice *>(renderer.Backend().Device)
											  : nullptr,
					source.Width,
					source.Height,
					source.Pixels,
					source.ColorSpace,
					formats[index]
				))
				return LiveImagePublishStatus::Invalid;
			prepared[index].Width = source.Width;
			prepared[index].Height = source.Height;
			prepared[index].Format = formats[index];
			try {
				prepared[index].Pixels.assign(source.Pixels.begin(), source.Pixels.end());
			} catch (const std::bad_alloc &) {
				return LiveImagePublishStatus::UploadFailed;
			}
			batch[index] = {binding.Name, &prepared[index]};
		}
		return renderer.AddTextureBatch(std::span(batch).first(images.size()), owner)
				   ? LiveImagePublishStatus::Published
				   : LiveImagePublishStatus::UploadFailed;
	}

	bool LiveImagePublisher::Retire(Renderer &renderer, const LiveImageBinding &binding) {
		if (!binding.Owner.IsValid() || !binding.Name.IsValid() || binding.Generation == 0) {
			return false;
		}

		const auto found = Generations.find(Key(binding.Owner, binding.Name));
		if (found == Generations.end() || found->second != binding.Generation) {
			return false;
		}

		(void)renderer.DropTexture(binding.Name, binding.Owner);
		Generations.erase(found);
		return true;
	}

	bool LiveImagePublisher::ReleaseBinding(const LiveImageBinding &binding) {
		if (!binding.Owner.IsValid() || !binding.Name.IsValid() || binding.Generation == 0) return false;
		const auto found = Generations.find(Key(binding.Owner, binding.Name));
		if (found == Generations.end() || found->second != binding.Generation) return false;
		Generations.erase(found);
		return true;
	}

	size_t LiveImagePublisher::RetireOwner(Renderer &renderer, core::Name owner) {
		if (!owner.IsValid()) {
			return 0;
		}

		size_t retired = 0;
		for (auto binding = Generations.begin(); binding != Generations.end();) {
			if (static_cast<uint32_t>(binding->first >> 32) != owner.Id()) {
				++binding;
				continue;
			}

			const core::Name name = core::Name::FromId(static_cast<uint32_t>(binding->first));
			(void)renderer.DropTexture(name, owner);
			binding = Generations.erase(binding);
			++retired;
		}
		return retired;
	}
}
