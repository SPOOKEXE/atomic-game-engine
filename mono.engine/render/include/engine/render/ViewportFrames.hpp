#pragma once

// Rendering the miniature scenes owned by `ViewportFrame` elements.
//
// A viewport is not an image asset. Its texture is a scene target rewritten
// from the frame's own camera before the outer world is drawn. This object owns
// the small amount of frame-to-slot bookkeeping needed by both the client and
// Studio, so the two hosts cannot disagree about which texture belongs to an
// element.
//
// @tier L12 · client

#include <engine/ecs/Entity.hpp>
#include <engine/render/InterfacePass.hpp>
#include <engine/render/Overlay.hpp>

#include <cstddef>
#include <vector>

namespace engine::ecs {
	class Store;
}

namespace engine::gui {
	struct DrawList;
}

namespace engine::render {

	class Renderer;

	// Every `ViewportFrame` in one interface, rendered into scene slots.
	//
	// **A pass of its own rather than part of the interface pass**, because each
	// of these is a whole scene render with its own camera: they have to happen
	// before the interface that shows them is recorded, and the interface only
	// needs the texture that came out.
	class ViewportFrames {
	  public:
		// Renders every visible viewport command into consecutive scene slots.
		// Returns the number whose camera and target were valid.
		size_t Render(Renderer &renderer, ecs::Store &store, const gui::DrawList &list, size_t firstSlot);

		// Resolves the texture produced by the most recent `Render`.
		InterfaceImage Resolve(ecs::Entity instance) const;

	  private:
		struct Entry {
			ecs::Entity Instance;
			void *Texture = nullptr;
			core::Vector2 UVMax{1.0f, 1.0f};
			uint32_t Width = 0;
			uint32_t Height = 0;
		};

		std::vector<Entry> Entries;
		OverlayImage EmptyOverlay;
	};
}
