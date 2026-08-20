#include <engine/ecs/Store.hpp>
#include <engine/gui/Adornments.hpp>
#include <engine/gui/Components.hpp>
#include <engine/render/AdornmentGeometry.hpp>
#include <engine/scene/Components.hpp>

#include <algorithm>
#include <array>

namespace engine::render {

	namespace {
		using core::CFrame;
		using core::Vector3;
		using ecs::Entity;
		using ecs::Store;

		// The twelve edges of a box, as pairs of corner indices.
		//
		// A corner is a sign per axis, so index `n` is `(n&1, n&2, n&4)` mapped
		// to -1 or +1 - which is why the table below is indices rather than
		// vectors: the corners are derived and the *edges* are the fact.
		// The six faces of a box, as four corner indices wound around each.
		//
		// **Wound so consecutive pairs are edges**, which is what
		// `AdornmentFace` promises and what lets a drawer treat the four points
		// as a convex polygon without sorting them. Getting a winding wrong here
		// draws a bow tie rather than a face, which is unmistakable and is
		// exactly what the case in `AdornmentGeometry.cpp`'s suite checks.
		constexpr std::array<std::array<int, 4>, 6> FACES{{
			{0, 1, 3, 2},
			{4, 6, 7, 5},
			{0, 4, 5, 1},
			{2, 3, 7, 6},
			{0, 2, 6, 4},
			{1, 5, 7, 3},
		}};

		constexpr std::array<std::pair<int, int>, 12> EDGES{{
			{0, 1},
			{2, 3},
			{4, 5},
			{6, 7},
			{0, 2},
			{1, 3},
			{4, 6},
			{5, 7},
			{0, 4},
			{1, 5},
			{2, 6},
			{3, 7},
		}};
	}

	void AdornmentGeometry::AddBox(
		const CFrame &frame, const Vector3 &halfExtent, const AdornmentLine &style, const AdornmentFace *fill
	) {
		std::array<Vector3, 8> corners{};
		for (int index = 0; index < 8; index++) {
			const Vector3 local{
				(index & 1) != 0 ? halfExtent.X : -halfExtent.X,
				(index & 2) != 0 ? halfExtent.Y : -halfExtent.Y,
				(index & 4) != 0 ? halfExtent.Z : -halfExtent.Z,
			};

			// **Through the adornee's own frame**, so a rotated part gets a
			// rotated box. Building the corners in world space from a position
			// and an extent would draw an axis-aligned box around a tilted
			// object, which reads as the selection being wrong rather than the
			// renderer.
			corners[static_cast<size_t>(index)] = frame.PointToWorldSpace(local);
		}

		for (const auto &[from, to] : EDGES) {
			AdornmentLine line = style;
			line.From = corners[static_cast<size_t>(from)];
			line.To = corners[static_cast<size_t>(to)];
			Segments.push_back(line);
		}

		// **The faces come after the edges and only when asked for.** A drawer
		// walks the faces first and the lines over them, which is what keeps an
		// outline visible against its own fill; emitting them in this order
		// costs nothing and makes the two lists independently drawable.
		if (fill == nullptr) {
			return;
		}

		for (const auto &face : FACES) {
			AdornmentFace filled = *fill;
			for (size_t corner = 0; corner < 4; corner++) {
				filled.Corners[corner] = corners[static_cast<size_t>(face[corner])];
			}
			Fills.push_back(filled);
		}
	}

	void AdornmentGeometry::Build(Store &store) {
		Segments.clear();
		Fills.clear();

		gui::EachAdornment(store, [&](Entity adornment, Entity adornee) {
			const gui::Adornment *state = store.Get<gui::Adornment>(adornment);
			if (state == nullptr) {
				return;
			}

			// **No transform, no geometry.** A `Folder` can legally be an
			// `Adornee`, and outlining it at the origin would draw a box around
			// a place nothing is - which looks like a bug in the selection
			// rather than an adornee that has no position.
			const scene::Transform *transform = store.Get<scene::Transform>(adornee);
			if (transform == nullptr) {
				return;
			}

			AdornmentLine style;
			style.Colour = state->Color;
			style.Transparency = state->Transparency;
			style.AlwaysOnTop = state->AlwaysOnTop;

			// **`SelectionBox` carries the outline's width and the surface, and
			// a handle carries neither.** `gui::SelectionOutline` is on the two
			// selection classes only, so a `BoxHandleAdornment` keeps the
			// default thickness and fills nothing - which is what a grab target
			// is.
			AdornmentFace fill;
			bool fills = false;
			if (const gui::SelectionOutline *outline = store.Get<gui::SelectionOutline>(adornment)) {
				style.Thickness = std::max(outline->LineThickness, 0.0f);
				fill.Colour = outline->SurfaceColor;
				fill.Transparency = outline->SurfaceTransparency;
				fill.AlwaysOnTop = state->AlwaysOnTop;

				// Fully transparent is the default and means no face at all
				// rather than a face nobody can see: a drawer given six
				// invisible quads still projects and rasterises them.
				fills = outline->SurfaceTransparency < 1.0f;
			}

			// A handle carries its own offset and size; everything else is drawn
			// against the adornee's own extent.
			if (const gui::HandleShape *handle = store.Get<gui::HandleShape>(adornment)) {
				// **Composed rather than added**, so a handle offset from a
				// rotated part follows the part's rotation - which is what makes
				// a move gizmo's arms point along the object's axes rather than
				// the world's.
				const CFrame placed = transform->Frame * handle->Offset;
				AddBox(placed, handle->Size * 0.5f, style, fills ? &fill : nullptr);
				return;
			}

			const scene::Bounds *bounds = store.Get<scene::Bounds>(adornee);
			if (bounds == nullptr) {
				return;
			}

			// **A hair larger than the part**, so the outline is not coplanar
			// with the surface it surrounds. Z-fighting on a selection box makes
			// it flicker along every edge, which reads as a driver fault.
			constexpr float SWELL = 1.002f;
			AddBox(transform->Frame, bounds->HalfExtent * SWELL, style, fills ? &fill : nullptr);
		});
	}
}
