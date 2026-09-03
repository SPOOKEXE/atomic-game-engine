#include <engine/ecs/Store.hpp>
#include <engine/gui/Adornments.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/render/AdornmentGeometry.hpp>
#include <engine/scene/Components.hpp>

#include <algorithm>
#include <array>
#include <cmath>

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
		constexpr std::array<std::array<int, 4>, 6> BOX_FACES{{
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

		// Matches the pose CollectInstances publishes for the same frame. Reading
		// Transform directly puts an adornment one simulation step ahead of the
		// object whenever presentation interpolation is active.
		CFrame
		PresentedFrame(const Store &store, Entity adornee, const scene::Transform &transform, float alpha) {
			const scene::PreviousTransform *previous = store.Get<scene::PreviousTransform>(adornee);
			return previous == nullptr ? transform.Frame : previous->Frame.NLerp(transform.Frame, alpha);
		}
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

		for (const auto &face : BOX_FACES) {
			AdornmentFace filled = *fill;
			for (size_t corner = 0; corner < 4; corner++) {
				filled.Corners[corner] = corners[static_cast<size_t>(face[corner])];
			}
			Fills.push_back(filled);
		}
	}

	void AdornmentGeometry::AddCircle(
		const CFrame &frame, uint32_t axis, float radius, float angleDegrees, const AdornmentLine &style
	) {
		constexpr size_t SEGMENTS = 32;
		constexpr float PI = 3.14159265358979323846f;
		const float safeRadius = std::max(radius, 0.0f);
		const float radians = std::clamp(angleDegrees, 0.0f, 360.0f) * PI / 180.0f;
		if (safeRadius <= 0.0f || radians <= 0.0f) {
			return;
		}

		const size_t segments =
			std::max<size_t>(1, static_cast<size_t>(std::ceil(SEGMENTS * radians / (2.0f * PI))));
		auto point = [&](float angle) {
			const float a = std::cos(angle) * safeRadius;
			const float b = std::sin(angle) * safeRadius;
			const Vector3 local = axis == 0	  ? Vector3{0.0f, a, b}
								  : axis == 1 ? Vector3{a, 0.0f, b}
											  : Vector3{a, b, 0.0f};
			return frame.PointToWorldSpace(local);
		};

		for (size_t segment = 0; segment < segments; segment++) {
			AdornmentLine line = style;
			line.From = point(radians * static_cast<float>(segment) / static_cast<float>(segments));
			line.To = point(radians * static_cast<float>(segment + 1) / static_cast<float>(segments));
			Segments.push_back(line);
		}
	}

	void AdornmentGeometry::AddSphere(const CFrame &frame, float radius, const AdornmentLine &style) {
		for (uint32_t axis = 0; axis < 3; axis++) {
			AddCircle(frame, axis, radius, 360.0f, style);
		}
	}

	void AdornmentGeometry::AddCylinder(
		const CFrame &frame,
		float radius,
		float innerRadius,
		float height,
		float angleDegrees,
		const AdornmentLine &style
	) {
		const float halfHeight = std::max(height, 0.0f) * 0.5f;
		const CFrame lower = frame * CFrame(Vector3{0.0f, 0.0f, -halfHeight});
		const CFrame upper = frame * CFrame(Vector3{0.0f, 0.0f, halfHeight});
		AddCircle(lower, 2, radius, angleDegrees, style);
		AddCircle(upper, 2, radius, angleDegrees, style);
		if (innerRadius > 0.0f) {
			AddCircle(lower, 2, innerRadius, angleDegrees, style);
			AddCircle(upper, 2, innerRadius, angleDegrees, style);
		}

		constexpr float PI = 3.14159265358979323846f;
		const float radians = std::clamp(angleDegrees, 0.0f, 360.0f) * PI / 180.0f;
		const bool fullCircle = angleDegrees >= 360.0f;
		const size_t sideCount = fullCircle ? 4 : 5;
		for (size_t side = 0; side < sideCount; side++) {
			const float angle = radians * static_cast<float>(side) / 4.0f;
			AdornmentLine line = style;
			const Vector3 radial{
				std::cos(angle) * std::max(radius, 0.0f), std::sin(angle) * std::max(radius, 0.0f), 0.0f
			};
			line.From = lower.PointToWorldSpace(radial);
			line.To = upper.PointToWorldSpace(radial);
			Segments.push_back(line);
		}
	}

	void AdornmentGeometry::AddCone(
		const CFrame &frame, float radius, float height, bool hollow, const AdornmentLine &style
	) {
		const float safeHeight = std::max(height, 0.0f);
		AddCircle(frame, 2, radius, 360.0f, style);
		const Vector3 tip = frame.PointToWorldSpace(Vector3{0.0f, 0.0f, safeHeight});
		constexpr float PI = 3.14159265358979323846f;
		for (size_t side = 0; side < 8; side++) {
			const float angle = 2.0f * PI * static_cast<float>(side) / 8.0f;
			AdornmentLine line = style;
			line.From = frame.PointToWorldSpace(
				Vector3{
					std::cos(angle) * std::max(radius, 0.0f), std::sin(angle) * std::max(radius, 0.0f), 0.0f
				}
			);
			line.To = tip;
			Segments.push_back(line);
		}
		if (!hollow && radius > 0.0f) {
			AdornmentLine diameter = style;
			diameter.From = frame.PointToWorldSpace(Vector3{-radius, 0.0f, 0.0f});
			diameter.To = frame.PointToWorldSpace(Vector3{radius, 0.0f, 0.0f});
			Segments.push_back(diameter);
		}
	}

	void AdornmentGeometry::Build(Store &store) {
		Segments.clear();
		Fills.clear();
		const float alpha = store.Time().Alpha;

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
			const CFrame frame = PresentedFrame(store, adornee, *transform, alpha);

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

			const scene::Bounds *bounds = store.Get<scene::Bounds>(adornee);

			// A handle carries its own local frame. SizeRelativeOffset is measured
			// against the adornee's half extent, so one reaches its surface.
			if (const gui::HandleShape *handle = store.Get<gui::HandleShape>(adornment)) {
				const Vector3 relative =
					bounds == nullptr ? Vector3::Zero : handle->SizeRelativeOffset * bounds->HalfExtent;
				const CFrame placed = frame * CFrame(relative) * handle->Offset;
				if (const auto *box = store.Get<gui::BoxHandleShape>(adornment)) {
					AddBox(placed, box->Size * 0.5f, style, nullptr);
				} else if (const auto *sphere = store.Get<gui::SphereHandleShape>(adornment)) {
					AddSphere(placed, sphere->Radius, style);
				} else if (const auto *cylinder = store.Get<gui::CylinderHandleShape>(adornment)) {
					AddCylinder(
						placed,
						cylinder->Radius,
						cylinder->InnerRadius,
						cylinder->Height,
						cylinder->Angle,
						style
					);
				} else if (const auto *line = store.Get<gui::LineHandleShape>(adornment)) {
					AdornmentLine segment = style;
					segment.Thickness = std::max(line->Thickness, 0.0f);
					segment.From = placed.Position;
					segment.To = placed.PointToWorldSpace(Vector3{0.0f, 0.0f, std::max(line->Length, 0.0f)});
					Segments.push_back(segment);
				} else if (const auto *cone = store.Get<gui::ConeHandleShape>(adornment)) {
					AddCone(placed, cone->Radius, cone->Height, cone->Hollow, style);
				}
				return;
			}

			if (bounds == nullptr) {
				return;
			}

			if (const auto *handles = store.Get<gui::HandlesShape>(adornment)) {
				const Vector3 directions[6] = {
					{1.0f, 0.0f, 0.0f},
					{0.0f, 1.0f, 0.0f},
					{0.0f, 0.0f, 1.0f},
					{-1.0f, 0.0f, 0.0f},
					{0.0f, -1.0f, 0.0f},
					{0.0f, 0.0f, -1.0f},
				};
				for (uint32_t face = 0; face < 6; face++) {
					if ((handles->Faces & (1u << face)) == 0) {
						continue;
					}
					const Vector3 edge = directions[face] * bounds->HalfExtent;
					AdornmentLine line = style;
					line.From = frame.PointToWorldSpace(edge);
					line.To = frame.PointToWorldSpace(edge + directions[face]);
					Segments.push_back(line);
				}
				return;
			}

			if (const auto *arcs = store.Get<gui::ArcHandlesShape>(adornment)) {
				const float radius =
					std::max({bounds->HalfExtent.X, bounds->HalfExtent.Y, bounds->HalfExtent.Z}) * 1.15f;
				for (uint32_t axis = 0; axis < 3; axis++) {
					if ((arcs->Axes & (1u << axis)) != 0) {
						AddCircle(frame, axis, radius, 360.0f, style);
					}
				}
				return;
			}

			if (store.ClassOf(adornment) == gui::GuiClass("SelectionSphere")) {
				const float radius =
					std::max({bounds->HalfExtent.X, bounds->HalfExtent.Y, bounds->HalfExtent.Z}) * 1.002f;
				AddSphere(frame, radius, style);
				return;
			}

			// **A hair larger than the part**, so the outline is not coplanar
			// with the surface it surrounds. Z-fighting on a selection box makes
			// it flicker along every edge, which reads as a driver fault.
			constexpr float SWELL = 1.002f;
			AddBox(frame, bounds->HalfExtent * SWELL, style, fills ? &fill : nullptr);
		});
	}
}
