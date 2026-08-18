#pragma once

// A compiled interface, as triangles.
//
// **The half of the interface pass that has logic in it, and the half a device
// is not needed for.** `gui::DrawList` says what to draw in canvas pixels;
// `Renderer`'s interface hook needs vertices, indices and the ranges between
// scissor changes. Everything between those two is arithmetic - quad
// generation, corner rounding, nine-slice, text layout against a `GlyphAtlas` -
// and arithmetic is testable headlessly, which is why it is a type of its own
// rather than a loop inside a Vulkan recording function.
//
// **The second backend is a new consumer of `gui::DrawList`, not a second
// compile.** That is what makes the client's interface a renderer rather than a
// feature: `ui::PaintGui` and this build from the same list, so neither can
// drift from the other about where an element is. What they do differ on is
// only what they hand the bytes to - an `ImDrawList` there, a vertex buffer
// here.
//
// @tier L12 · client

#include <engine/core/types/Rect.hpp>
#include <engine/core/types/Vector2.hpp>
#include <engine/gui/DrawList.hpp>
#include <engine/render/Flipbook.hpp>
#include <engine/render/GlyphAtlas.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace engine::render {

	// One interface vertex.
	//
	// **Packed colour rather than four floats**, because a full-screen interface
	// is tens of thousands of vertices and the colour is the one attribute that
	// loses nothing to eight bits - it is multiplied into a sampled texel or
	// used flat, and neither wants more precision than a display has.
	//
	// @since v0.8
	struct InterfaceVertex {
		// Position in canvas pixels. The pass turns these into clip space with
		// one multiply, so nothing here has to know the target's size.
		//@{
		float X = 0.0f;
		float Y = 0.0f;
		//@}

		// Where to sample. For an untextured quad this is the atlas's white
		// texel, so a filled rectangle and a glyph go through one pipeline.
		//@{
		float U = 0.0f;
		float V = 0.0f;
		//@}

		// RGBA, non-premultiplied, in that byte order.
		//@{
		uint8_t R = 255;
		uint8_t G = 255;
		uint8_t B = 255;
		uint8_t A = 255;
		//@}
	};

	// A run of indices sharing one texture and one scissor.
	//
	// **Split by scissor as well as by texture**, because a clip is a pipeline
	// state rather than a vertex attribute: two elements clipped differently
	// cannot be one draw however alike their pixels are. That is the whole
	// reason this type exists instead of one index buffer.
	//
	// @since v0.8
	struct InterfaceBatch {
		// Where this run starts in the index buffer, and how long it is.
		//@{
		uint32_t FirstIndex = 0;
		uint32_t IndexCount = 0;
		//@}

		// The scissor, in canvas pixels.
		core::Rect Clip;

		// Which content name to sample, or an invalid name for the atlas.
		//
		// **A `core::Name` rather than a texture handle**, because resolving one
		// is the caller's - `gui::DrawCommand::Image` is a content name for
		// exactly that reason, and this type is no better placed to resolve it
		// than the list it came from. A backend maps the name to whatever it can
		// sample and draws the batch.
		core::Name Image;

		// The element providing a dynamic viewport texture, or null for every
		// other batch.
		ecs::Entity Viewport;

		// The canvas this batch belongs to. A backend uses this to keep screen
		// pixels out of a world-space collector and vice versa.
		ecs::Entity Collector;

		// Which fragment shader draws this run, or invalid for the pass's
		// own - from `gui::DrawCommand::Shader`.
		//
		// **Split for the identical reason a texture change is**: a pipeline
		// is bound per batch, not per quad, so two quads wanting different
		// fragment shaders cannot be one draw call whatever else they share.
		//
		// @since v0.18
		core::Name Shader;
	};

	// The source-pixel extent of an image after selecting its animation cell.
	//
	// A stretch needs no dimensions, but fit, crop, tile and nine-slice all do.
	// The texture handle stays in `InterfacePass`; this arithmetic layer needs
	// only the size and remains device-free.
	struct InterfaceImageInfo {
		core::Vector2 Size;
		FlipbookCell Cell;
		core::Vector2 UVMax{1.0f, 1.0f};
	};

	// Vertices, indices and the batches between them.
	//
	// Long-lived: built once per frame into buffers that keep their capacity, so
	// a steady interface stops allocating after the frame that reached its high
	// -water mark. That is `gui::Compiled`'s arrangement and `core::ByteWriter`'s
	// before it.
	//
	// @since v0.8
	class InterfaceMesh {
	  public:
		// Rebuilds from a compiled list.
		//
		// **The atlas is what makes text possible and is optional anyway.** A
		// mesh built without one draws every rectangle and image and no glyph,
		// which is what a client whose fonts failed to stage should show - the
		// interface is still usable and the missing text is visible as missing.
		//
		// @param list  The compiled list, in paint order.
		// @param atlas The glyphs, or an unbuilt atlas for no text.
		void Build(
			const gui::DrawList &list,
			const GlyphAtlas &atlas,
			const std::function<InterfaceImageInfo(const core::Name &)> &images = {},
			const std::function<InterfaceImageInfo(ecs::Entity)> &viewports = {}
		);

		// The vertices, valid until the next `Build`.
		const std::vector<InterfaceVertex> &Vertices() const {
			return VertexData;
		}

		// The indices, valid until the next `Build`.
		const std::vector<uint16_t> &Indices() const {
			return IndexData;
		}

		// The batches, in submission order.
		const std::vector<InterfaceBatch> &Batches() const {
			return BatchData;
		}

		// Where in the atlas a solid quad samples.
		//
		// **One texel of pure white, baked into every atlas.** Without it a
		// filled rectangle would need a second pipeline with no texture bound,
		// and two pipelines is two places for the blend state to be set
		// differently. See `GlyphAtlas::WhiteTexel`.
		static core::Vector2 WhiteUV(const GlyphAtlas &atlas);

	  private:
		// One element's rotation, resolved once per command.
		//
		// **Sine and cosine computed once rather than per corner**, and the
		// pivot is the *element's* centre rather than each quad's - a glyph
		// inside a rotated label turns with the label, and a per-quad pivot
		// would spin every letter on the spot while leaving the run in a
		// straight line.
		struct Rotation {
			core::Vector2 Pivot;
			float Sine = 0.0f;
			float Cosine = 1.0f;
		};

		// The rotation one command asks for, resolved once.
		static Rotation TurnOf(const gui::DrawCommand &command);

		void Push(const core::Rect &bounds, const core::Rect &uv, uint32_t colour, const Rotation &turn);
		void PushRounded(
			const core::Rect &bounds,
			const core::Rect &uv,
			float radius,
			uint32_t colour,
			const Rotation &turn
		);
		void PushRoundedOutline(
			const core::Rect &bounds,
			float radius,
			float thickness,
			const core::Vector2 &uv,
			uint32_t colour,
			const Rotation &turn
		);

		std::vector<InterfaceVertex> VertexData;
		std::vector<uint16_t> IndexData;
		std::vector<InterfaceBatch> BatchData;
	};
}
