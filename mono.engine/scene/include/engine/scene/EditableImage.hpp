#pragma once

// A texture a script draws into while the engine runs, rather than one that
// arrived from content.
//
// **`EditableMesh`'s exact shape, one dimension down.** `render::TextureTable`
// has taken a name and an `assets::TextureData` since v0.8; what did not exist
// was a producer with no importer anywhere near it. This is that producer's
// storage - a row-major RGBA8 buffer and a revision counter, converted and
// uploaded by `client::UpdateEditableImages` for `scene::EditableMesh`'s own
// reason: `scene` may not link `assets`, the format the render tier takes.
//
// ## What is here and what is not
//
// Roblox's `EditableImage` reads and writes raw pixels through a Luau
// `buffer`, which this engine's script surface has no marshalling for yet -
// `ScriptCall` carries a fixed set of argument and return kinds, and `buffer`
// is not one of them. So the door onto the pixels is the drawing primitives
// rather than the buffer itself: `DrawRectangle`, `DrawLine` and `DrawCircle`
// cover what a script can put on the image, and `docs/DEFERRED.md` is where a
// `WritePixelsBuffer` would be opened once that marshalling exists.
//
// **Fixed size, chosen at creation.** Roblox's own `EditableImage` is the
// same - `AssetService:CreateEditableImage({Size = ...})` sets it once - and
// `Resize` here is the escape hatch for a script that decides differently
// later; it clears the image, because there is no resampling rule that is
// obviously right for content nothing published.
//
// @tier L7 · shared

#include <engine/core/Name.hpp>
#include <engine/core/types/Color3.hpp>
#include <engine/core/types/Vector2.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Entity.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace engine::ecs {
	class Store;
}

namespace engine::scene {

	// The default an `EditableImage` is created at - Roblox's own default is
	// 512x512; this halves it, because the transport is `Store::Save`'s
	// ordinary snapshot path rather than a streamed asset and a raw RGBA8
	// buffer costs `Width * Height * 4` there uncompressed.
	inline constexpr uint32_t DEFAULT_EDITABLE_IMAGE_SIZE = 256;

	// The largest an `EditableImage` may be. Sixteen million pixels is a
	// 4096x4096 sheet at four bytes each - 64 MB - which is already past what
	// a script drawing UI or a decal should need; the ceiling exists so a
	// mistyped `Resize` cannot allocate an unbounded buffer.
	inline constexpr uint32_t MAXIMUM_EDITABLE_IMAGE_PIXELS = 16u * 1024u * 1024u;

	// Row-major RGBA8, top row first - `assets::TextureData::Pixels`'s own
	// layout, so `client::BuildTextureData` is a copy and not a conversion.
	//
	// @since v0.18
	struct EditableImage {
		uint32_t Width = DEFAULT_EDITABLE_IMAGE_SIZE;
		uint32_t Height = DEFAULT_EDITABLE_IMAGE_SIZE;

		// `Width * Height * 4` bytes, R-G-B-A per pixel.
		//
		// **Defaulted to match `Width` and `Height` above rather than left
		// empty.** The three must never disagree - every drawing primitive
		// indexes this buffer by `Width` and `Height` with no bounds check
		// beyond them, so a default that left this empty while they read
		// 256 would be a `CreateInstance` that hands out a row one
		// `DrawRectangle` call away from writing off the end of an empty
		// vector.
		std::vector<uint8_t> Pixels = std::vector<uint8_t>(
			static_cast<size_t>(DEFAULT_EDITABLE_IMAGE_SIZE) * DEFAULT_EDITABLE_IMAGE_SIZE * 4, 0
		);

		// Bumped by every call that changes a pixel - `EditableMesh::
		// Revision`'s exact reason and exact contract.
		uint32_t Revision = 0;
	};

	// The `core::Name` a texture reference names this image by - `SurfaceAppearance::
	// ColourMap` and `gui::Picture::Image` alike, since both resolve a name
	// against the same `render::TextureTable`.
	//
	// @param store    The world.
	// @param instance The `EditableImage` instance.
	// @return The name, or an invalid one for anything but an `EditableImage`.
	// @since v0.18
	core::Name EditableImageContentName(const ecs::Store &store, ecs::Entity instance);

	// Allocates a new pixel buffer at this size, clearing it to fully
	// transparent black.
	//
	// **Clears rather than resamples.** There is no rule for what a resize of
	// content nothing published should preserve, so this states the honest
	// one: whatever was drawn is gone, and a script that wants to keep it
	// draws again.
	//
	// @param store    The world.
	// @param instance The `EditableImage` instance.
	// @param width    The new width in pixels. Clamped to at least one.
	// @param height   The new height in pixels. Clamped to at least one.
	// @return `false` for anything but an `EditableImage`, or a size past
	//         `MAXIMUM_EDITABLE_IMAGE_PIXELS`.
	// @since v0.18
	bool ResizeEditableImage(ecs::Store &store, ecs::Entity instance, uint32_t width, uint32_t height);

	// Fills an axis-aligned rectangle, clipped to the image.
	//
	// **Over, not replace.** `transparency` blends this colour over what is
	// already there - `alpha = 1 - transparency` - so drawing a translucent
	// rectangle twice deepens it rather than repeating the same result,
	// which is Roblox's own blend and the one a script layering shapes
	// expects.
	//
	// @param store        The world.
	// @param instance     The `EditableImage` instance.
	// @param position     The top-left corner, in pixels.
	// @param size         The extent, in pixels.
	// @param colour       The fill colour.
	// @param transparency 0 is opaque, 1 draws nothing.
	// @return `false` for anything but an `EditableImage`.
	// @since v0.18
	bool DrawRectangle(
		ecs::Store &store,
		ecs::Entity instance,
		const core::Vector2 &position,
		const core::Vector2 &size,
		const core::Color3 &colour,
		float transparency = 0.0f
	);

	// Draws a one-pixel-wide line between two points with Bresenham's
	// algorithm, clipped to the image.
	//
	// @return `false` for anything but an `EditableImage`.
	// @since v0.18
	bool DrawLine(
		ecs::Store &store,
		ecs::Entity instance,
		const core::Vector2 &from,
		const core::Vector2 &to,
		const core::Color3 &colour,
		float transparency = 0.0f
	);

	// Draws a filled circle with the midpoint algorithm, clipped to the
	// image.
	//
	// @return `false` for anything but an `EditableImage`.
	// @since v0.18
	bool DrawCircle(
		ecs::Store &store,
		ecs::Entity instance,
		const core::Vector2 &centre,
		float radius,
		const core::Color3 &colour,
		float transparency = 0.0f
	);

	// The `EditableImage` class id, registering the tree if nobody has yet.
	//
	// @return The class id.
	// @since v0.18
	ecs::ClassId EditableImageClass();
}
