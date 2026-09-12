#include <engine/assets/Texture.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/render/EditableImages.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/scene/EditableImage.hpp>

#include <algorithm>
#include <cstring>
#include <iterator>

namespace engine::render {

	EditableImagePackingSupport EditableImagePackingSupportOf(const engine::scene::EditableImage &image) {
		if (image.Packing.Format == engine::scene::EditablePackingFormat::Float32)
			return EditableImagePackingSupport::NativeRGBA8;
		constexpr uint8_t imageAttributes =
			static_cast<uint8_t>(engine::scene::EditablePackingAttribute::Colour) |
			static_cast<uint8_t>(engine::scene::EditablePackingAttribute::Alpha);
		if (image.Packing.Attributes == 0 || (image.Packing.Attributes & ~imageAttributes) != 0)
			return EditableImagePackingSupport::UnsupportedAttributes;
		return EditableImagePackingSupport::UnsupportedFormat;
	}

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

	size_t EditableImageUploader::Refresh(
		engine::ecs::Store &store, engine::render::Renderer &renderer, core::Name owner
	) {
		auto foundScope = std::find_if(Scopes.begin(), Scopes.end(), [&](const UploadScope &scope) {
			return scope.World == store.Identity() && scope.Owner == owner;
		});
		if (foundScope == Scopes.end()) {
			Scopes.push_back({store.Identity(), owner, {}});
			foundScope = std::prev(Scopes.end());
		}
		auto &uploadedRevisions = foundScope->Revisions;
		size_t uploaded = 0;

		store.Each<const engine::scene::EditableImage>([&](engine::ecs::Entity entity,
														   const engine::scene::EditableImage &image) {
			const auto found = uploadedRevisions.find(entity.Id);
			if (found != uploadedRevisions.end() && found->second == image.Revision) {
				return;
			}

			const engine::assets::TextureData built = BuildTextureData(image);
			const engine::core::Name name = engine::scene::EditableImageContentName(store, entity);
			if (renderer.AddTexture(name, built, owner)) {
				uploadedRevisions[entity.Id] = image.Revision;
				uploaded++;
			}
		});

		return uploaded;
	}
	void EditableImageUploader::ForgetWorld(uint64_t identity) {
		std::erase_if(Scopes, [identity](const UploadScope &scope) { return scope.World == identity; });
	}

	void EditableImageUploader::ForgetOwner(core::Name owner) {
		std::erase_if(Scopes, [owner](const UploadScope &scope) { return scope.Owner == owner; });
	}

}
