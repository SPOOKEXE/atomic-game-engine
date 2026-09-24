#pragma once

#include <engine/core/Name.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>

namespace engine::render {

	class Renderer;

	// Copyable identity for one live image binding. The publisher assigns a new
	// generation on every bind, so late work from an old binding is refused.
	struct LiveImageBinding {
		core::Name Owner;
		core::Name Name;
		uint64_t Generation = 0;
	};

	enum class LiveImagePublishStatus : uint8_t {
		Published,
		Stale,
		Invalid,
		UploadFailed,
	};

	enum class LiveImageColorSpace : uint8_t { Display, Linear };

	struct LiveImageUpload {
		LiveImageBinding Binding;
		uint32_t Width = 0;
		uint32_t Height = 0;
		std::span<const std::byte> Rgba8;
		LiveImageColorSpace ColorSpace = LiveImageColorSpace::Display;
	};

	// Copies bounded RGBA8 images into renderer-owned, owner-scoped textures.
	// Calls must run on the renderer's owning thread.
	class LiveImagePublisher {
	  public:
		static constexpr uint32_t MAXIMUM_SIDE = 8192;
		static constexpr size_t MAXIMUM_IMAGE_BYTES = 64u * 1024u * 1024u;
		// Bounds concurrent names. Retired names release their state immediately.
		static constexpr size_t MAXIMUM_BINDINGS = 4096;

		// Starts or rebinds one owner/name pair. A rebind invalidates older work
		// while leaving the last uploaded texture available until replacement succeeds.
		std::optional<LiveImageBinding> BeginBinding(core::Name owner, core::Name name);

		// Copies the pixels before upload. A failed or invalid replacement leaves
		// the last uploaded image available under the same owner and name.
		LiveImagePublishStatus Publish(
			Renderer &renderer,
			const LiveImageBinding &binding,
			uint32_t width,
			uint32_t height,
			std::span<const std::byte> rgba8,
			LiveImageColorSpace colorSpace = LiveImageColorSpace::Display
		);

		// Commits up to six current bindings as one renderer texture generation.
		// Any invalid, stale or failed face leaves every previous face intact.
		LiveImagePublishStatus PublishBatch(Renderer &renderer, std::span<const LiveImageUpload> images);

		// Retires one current binding and drops its texture once. A stale generation
		// cannot retire its replacement.
		bool Retire(Renderer &renderer, const LiveImageBinding &binding);

		// Retires every tracked binding for an owner and returns the number of
		// binding records removed.
		size_t RetireOwner(Renderer &renderer, core::Name owner);

		size_t ActiveBindingCount() const {
			return Generations.size();
		}

	  private:
		static uint64_t Key(core::Name owner, core::Name name);

		// Renderer::TextureTable remains the source of truth for resident images.
		// Only active owner/name bindings live here. Unique generations make removed
		// records stale without keeping per-world tombstones.
		std::unordered_map<uint64_t, uint64_t> Generations;
		uint64_t NextGeneration = 1;
	};
}
