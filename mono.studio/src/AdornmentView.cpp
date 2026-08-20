// Drawing what a `SelectionBox` says.
//
// **The first consumer `render::AdornmentGeometry` has ever had.** That class
// resolved every adornment in a world against its adornee, was tested, and was
// called by nothing - so a `SelectionBox` drew nothing whatever its properties
// said, and `docs/DEFERRED.md` D00129 held `LineThickness`, `SurfaceColor3` and
// `SurfaceTransparency` back behind "a triangle path for adornments" when what
// was missing was any path at all.
//
// **An overlay rather than a render pass**, which is `Editor::DrawColliderOutlines`'
// arrangement and its argument in full: the studio already projects world points
// into a panel and draws into an `ImDrawList`, so an adornment is a list of
// segments and no pipeline, no shader and no target. It also gets two things a
// pass would have had to build - a line width that is a real number rather than
// a device setting with no portable guarantee, and a filled convex polygon -
// from imgui for nothing.
//
// **What it does not do is depth.** `AdornmentLine::AlwaysOnTop` is honoured
// only in its `true` sense, because an overlay is drawn after the world and has
// no depth buffer to test against. That is the right way round for the default:
// an adornment a wall hides is one that does not say what is selected. A
// `false` one draws over the wall anyway, and the header says so rather than
// pretending otherwise.

#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Store.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <imgui.h>
#include <studio/Editor.hpp>
#include <studio/Viewports.hpp>

namespace studio {

	namespace {
		using engine::core::Vector3;

		// The most segments and faces one viewport will draw.
		//
		// **A bound rather than a hope**, for `DrawColliderOutlines`' reason: an
		// overlay list is a vertex buffer and a world where somebody selected
		// ten thousand parts would fill it. Past the cap the rest are dropped,
		// which is a partial outline rather than a dropped frame.
		//@{
		constexpr size_t ADORNMENT_SEGMENTS = 20000;
		constexpr size_t ADORNMENT_FACES = 4000;
		//@}

		// The thinnest and thickest a line is allowed to come out, in pixels.
		//
		// A thickness in studs projects to nothing at a distance and to a slab
		// up close, and imgui rasterises both literally. The floor is what keeps
		// a far-away selection visible at all; the ceiling is what stops a box
		// you are standing inside filling the viewport.
		//@{
		constexpr float THINNEST_PIXELS = 1.0f;
		constexpr float THICKEST_PIXELS = 12.0f;
		//@}

		// Packs a colour and a transparency the way imgui wants them.
		ImU32 Packed(const engine::core::Color3 &colour, float transparency) {
			const float alpha = std::clamp(1.0f - transparency, 0.0f, 1.0f);
			return ImGui::GetColorU32(ImVec4(colour.R, colour.G, colour.B, alpha));
		}

		// How many pixels wide a world-space thickness is where this segment is.
		//
		// **Measured rather than derived from the field of view**, because the
		// projection carries a matrix and an eye and not a lens. The offset used
		// is perpendicular to both the segment and the view direction - which is
		// the screen-space perpendicular, and therefore the direction the width
		// actually occupies - so this is the width imgui should draw rather than
		// an approximation of it.
		float PixelsFor(const PanelProjection &panel, const Vector3 &from, const Vector3 &to, float studs) {
			if (!(studs > 0.0f)) {
				return THINNEST_PIXELS;
			}

			const Vector3 middle = (from + to) * 0.5f;
			const Vector3 along = to - from;
			const Vector3 toEye = panel.Eye - middle;

			Vector3 sideways = along.Cross(toEye);
			const float length = sideways.Magnitude();
			if (!(length > 0.0f)) {
				// The segment points straight at the camera, so it is a dot and
				// its width is whatever the floor is.
				return THINNEST_PIXELS;
			}
			sideways = sideways * (studs / length);

			glm::vec2 here{};
			glm::vec2 beside{};
			if (!panel.WorldToPanel(middle, here) || !panel.WorldToPanel(middle + sideways, beside)) {
				return THINNEST_PIXELS;
			}

			const float wide = std::hypot(beside.x - here.x, beside.y - here.y);
			return std::clamp(wide, THINNEST_PIXELS, THICKEST_PIXELS);
		}
	}

	void Editor::DrawAdornments(size_t viewport, const PanelProjection &panel) {
		OverlaySlot &slot = Overlays[viewport];
		ImDrawList *list = slot.List;
		if (list == nullptr || Universe == nullptr || !panel.IsValid()) {
			return;
		}

		const WorldId world = ViewportWorld(viewport);

		Universe->Enter(world, [&](engine::ecs::Store &store) {
			Adornments.Build(store);

			// **The faces first and the lines over them**, so an outline stays
			// visible against its own fill. An adornment that filled over its
			// own edges would be a box whose outline disappears exactly when
			// somebody turns the surface on.
			size_t faces = 0;
			for (const engine::render::AdornmentFace &face : Adornments.Faces()) {
				if (faces >= ADORNMENT_FACES) {
					break;
				}
				faces++;

				// **All four corners or none.** A face with one corner behind
				// the eye cannot be clipped the way a segment can - the result
				// is a polygon rather than a shorter one - so a partly-visible
				// face is dropped rather than drawn wrong. The edges are still
				// drawn, because `ProjectSegment` does clip.
				ImVec2 points[4];
				bool whole = true;
				for (size_t corner = 0; corner < 4 && whole; corner++) {
					glm::vec2 at{};
					whole = panel.WorldToPanel(face.Corners[corner], at);
					points[corner] = ImVec2(at.x, at.y);
				}
				if (!whole) {
					continue;
				}

				list->AddConvexPolyFilled(points, 4, Packed(face.Colour, face.Transparency));
			}

			size_t drawn = 0;
			for (const engine::render::AdornmentLine &line : Adornments.Lines()) {
				if (drawn >= ADORNMENT_SEGMENTS) {
					break;
				}
				drawn++;

				glm::vec2 from{};
				glm::vec2 to{};
				if (!panel.ProjectSegment(line.From, line.To, from, to)) {
					continue;
				}

				list->AddLine(
					ImVec2(from.x, from.y),
					ImVec2(to.x, to.y),
					Packed(line.Colour, line.Transparency),
					PixelsFor(panel, line.From, line.To, line.Thickness)
				);
			}
		});
	}
}
