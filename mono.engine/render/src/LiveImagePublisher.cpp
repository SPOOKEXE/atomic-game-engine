#include <engine/assets/Texture.hpp>
#include <engine/render/LiveImagePublisher.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/render/TextureTable.hpp>

#include <array>
#include <limits>
#include <new>
#include <vector>

namespace engine::render {

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
		std::span<const std::byte> rgba8,
		LiveImageColorSpace colorSpace
	) {
		if (!binding.Owner.IsValid() || !binding.Name.IsValid() || binding.Generation == 0) {
			return LiveImagePublishStatus::Invalid;
		}

		const uint64_t key = Key(binding.Owner, binding.Name);
		auto generation = Generations.find(key);
		if (generation == Generations.end() || binding.Generation != generation->second) {
			return LiveImagePublishStatus::Stale;
		}

		const uint64_t expectedBytes = uint64_t(width) * height * 4;
		if (width == 0 || height == 0 || width > MAXIMUM_SIDE || height > MAXIMUM_SIDE ||
			expectedBytes > MAXIMUM_IMAGE_BYTES || expectedBytes != rgba8.size() ||
			(colorSpace != LiveImageColorSpace::Display && colorSpace != LiveImageColorSpace::Linear)) {
			return LiveImagePublishStatus::Invalid;
		}

		assets::TextureData image;
		image.Width = width;
		image.Height = height;
		image.Format = colorSpace == LiveImageColorSpace::Linear ? assets::TextureFormat::RGBA8_LINEAR
																 : assets::TextureFormat::RGBA8;
		try {
			image.Pixels.assign(rgba8.begin(), rgba8.end());
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
			const uint64_t bytes = uint64_t(source.Width) * source.Height * 4;
			if (source.Width == 0 || source.Height == 0 || source.Width > MAXIMUM_SIDE ||
				source.Height > MAXIMUM_SIDE || bytes > MAXIMUM_IMAGE_BYTES || bytes != source.Rgba8.size() ||
				(source.ColorSpace != LiveImageColorSpace::Display &&
				 source.ColorSpace != LiveImageColorSpace::Linear))
				return LiveImagePublishStatus::Invalid;
			prepared[index].Width = source.Width;
			prepared[index].Height = source.Height;
			prepared[index].Format = source.ColorSpace == LiveImageColorSpace::Linear
										 ? assets::TextureFormat::RGBA8_LINEAR
										 : assets::TextureFormat::RGBA8;
			try {
				prepared[index].Pixels.assign(source.Rgba8.begin(), source.Rgba8.end());
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
