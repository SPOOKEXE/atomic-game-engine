#pragma once

// An axis-aligned rectangle, stored as two corners.
//
// **Corners rather than a position and a size**, which is the opposite of what
// `AABB` does one dimension up and is worth explaining. `AABB` is the shape a
// containment test wants and it is derived per entity per tick; this is what a
// caller *authors* - a sprite's slice of an atlas, a region of a screen - and an
// author reads two corners off an image far more easily than a corner and an
// extent. `Width` and `Height` are one subtraction away.
//
// Nothing here normalises: a rectangle whose maximum is below its minimum is
// empty, and `Empty()` says so rather than a constructor quietly swapping the
// two. A caller that built one backwards has a bug, and swapping hides it.
//
// @tier L1 · shared

#include <engine/core/types/Vector2.hpp>

#include <algorithm>

namespace engine::core {

	// An axis-aligned rectangle between two corners.
	//
	// @since v0.6
	struct Rect {
		// The lower corner, on both axes.
		Vector2 Min;

		// The upper corner, on both axes.
		Vector2 Max;

		// Constructs the degenerate rectangle at the origin.
		constexpr Rect() = default;

		// Constructs from two corners.
		//
		// @param min The lower corner.
		// @param max The upper corner.
		constexpr Rect(const Vector2 &min, const Vector2 &max) : Min(min), Max(max) {}

		// Constructs from four numbers, in Roblox's argument order.
		//
		// @param minX Lower X.
		// @param minY Lower Y.
		// @param maxX Upper X.
		// @param maxY Upper Y.
		constexpr Rect(float minX, float minY, float maxX, float maxY) : Min(minX, minY), Max(maxX, maxY) {}

		// Reports whether both corners are exactly equal.
		constexpr bool operator==(const Rect &other) const {
			return Min == other.Min && Max == other.Max;
		}

		// The extent on both axes. Negative on an empty rectangle, deliberately:
		// the sign is what tells a caller which way round they built it.
		constexpr Vector2 Size() const {
			return Max - Min;
		}

		// The horizontal extent.
		constexpr float Width() const {
			return Max.X - Min.X;
		}

		// The vertical extent.
		constexpr float Height() const {
			return Max.Y - Min.Y;
		}

		// The point halfway between the corners.
		constexpr Vector2 Center() const {
			return {(Min.X + Max.X) * 0.5f, (Min.Y + Max.Y) * 0.5f};
		}

		// Reports whether the rectangle encloses nothing.
		//
		// True when either axis's maximum is below its minimum. A zero-area
		// rectangle is *not* empty - it is a point or a line, and a caller
		// clipping against one wants that distinction.
		constexpr bool Empty() const {
			return Max.X < Min.X || Max.Y < Min.Y;
		}

		// Reports whether a point lies inside, corners included.
		constexpr bool Contains(const Vector2 &point) const {
			return point.X >= Min.X && point.X <= Max.X && point.Y >= Min.Y && point.Y <= Max.Y;
		}

		// Reports whether two rectangles share any area, edges included.
		constexpr bool Intersects(const Rect &other) const {
			return Min.X <= other.Max.X && Max.X >= other.Min.X && Min.Y <= other.Max.Y &&
				   Max.Y >= other.Min.Y;
		}

		// The overlap of two rectangles, which may be empty.
		constexpr Rect Intersection(const Rect &other) const {
			return {
				Vector2{std::max(Min.X, other.Min.X), std::max(Min.Y, other.Min.Y)},
				Vector2{std::min(Max.X, other.Max.X), std::min(Max.Y, other.Max.Y)}
			};
		}
	};
}
