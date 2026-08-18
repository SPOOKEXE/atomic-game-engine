#include <client/EditableImages.hpp>

#include <engine/assets/Texture.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/scene/EditableImage.hpp>

#include <cstring>

namespace client {

	engine::assets::TextureData BuildTextureData(const engine::scene::EditableImage &image) {
		engine::assets::TextureData built;
		built.Width = image.Width;
		built.Height = image.Height;
		built.Format = engine::assets::TextureFormat::RGBA8;

		// **A copy, not a conversion** - `scene::EditableImage::Pixels` is
		// already row-major RGBA8 top row first, `assets::TextureData::
		// Pixels`'s own layout. `std::byte` and `uint8_t` are both one byte
		// with the same alignment, so this is the same bytes read through
		// the type the render tier expects them as.
		built.Pixels.resize(image.Pixels.size());
		std::memcpy(built.Pixels.data(), image.Pixels.data(), image.Pixels.size());

		return built;
	}

	size_t EditableImageUploader::Refresh(engine::ecs::Store &store, engine::render::Renderer &renderer) {
		size_t uploaded = 0;

		store.Each<const engine::scene::EditableImage>(
			[&](engine::ecs::Entity entity, const engine::scene::EditableImage &image) {
				const auto found = Uploaded.find(entity.Id);
				if (found != Uploaded.end() && found->second == image.Revision) {
					return;
				}

				const engine::assets::TextureData built = BuildTextureData(image);
				const engine::core::Name name = engine::scene::EditableImageContentName(store, entity);
				if (renderer.AddTexture(name, built)) {
					Uploaded[entity.Id] = image.Revision;
					uploaded++;
				}
			}
		);

		return uploaded;
	}
}
