#pragma once

// A length that is part proportion and part absolute - the unit a layout is
// written in.
//
// **Why a pair and not a float.** A UI element sized "half the parent, minus
// eight pixels for the border" cannot be either number alone: the proportion has
// to survive a resize and the offset has to survive a scale change. Roblox
// solved this with `UDim` and the spelling is worth keeping, because the people
// this engine is for already know what `UDim2.new(0.5, -8, 0, 24)` means.
//
// **Resolution is not here.** `Scale` needs a parent length to mean anything,
// and a type that resolved itself would need to know what it is inside - which
// is a fact about a tree, not about a length. `Resolve` takes the parent extent
// as an argument for exactly that reason.
//
// @tier L1 · shared

#include <engine/core/types/Vector2.hpp>

namespace engine::core {

	// One axis of a layout length: a fraction of the parent plus a fixed offset.
	//
	// @since v0.6
	struct UDim {
		// Fraction of the parent's extent along this axis. 1 is the whole of it.
		float Scale = 0.0f;

		// Absolute offset along this axis, in the caller's unit - pixels, for a
		// UI.
		float Offset = 0.0f;

		// Constructs the zero length.
		constexpr UDim() = default;

		// Constructs a length from a proportion and an offset.
		//
		// @param scale  Fraction of the parent's extent.
		// @param offset Absolute offset in the caller's unit.
		constexpr UDim(float scale, float offset) : Scale(scale), Offset(offset) {}

		// Adds both parts. What stacking two lengths means.
		constexpr UDim operator+(const UDim &other) const {
			return {Scale + other.Scale, Offset + other.Offset};
		}

		// Subtracts both parts.
		constexpr UDim operator-(const UDim &other) const {
			return {Scale - other.Scale, Offset - other.Offset};
		}

		// Negates both parts.
		constexpr UDim operator-() const {
			return {-Scale, -Offset};
		}

		// Reports whether both parts are exactly equal.
		constexpr bool operator==(const UDim &other) const {
			return Scale == other.Scale && Offset == other.Offset;
		}

		// Resolves against a parent extent.
		//
		// @param parentExtent The parent's length along this axis.
		// @return The absolute length.
		constexpr float Resolve(float parentExtent) const {
			return Scale * parentExtent + Offset;
		}
	};

	// A two-dimensional layout length: one `UDim` per axis.
	//
	// @since v0.6
	struct UDim2 {
		// The horizontal length.
		UDim X;

		// The vertical length.
		UDim Y;

		// Constructs the zero size.
		constexpr UDim2() = default;

		// Constructs from two axes.
		//
		// @param x The horizontal length.
		// @param y The vertical length.
		constexpr UDim2(const UDim &x, const UDim &y) : X(x), Y(y) {}

		// Constructs from four numbers, in Roblox's argument order.
		//
		// @param xScale  Horizontal fraction of the parent.
		// @param xOffset Horizontal absolute offset.
		// @param yScale  Vertical fraction of the parent.
		// @param yOffset Vertical absolute offset.
		constexpr UDim2(float xScale, float xOffset, float yScale, float yOffset)
			: X(xScale, xOffset), Y(yScale, yOffset) {}

		// Adds both axes.
		constexpr UDim2 operator+(const UDim2 &other) const {
			return {X + other.X, Y + other.Y};
		}

		// Subtracts both axes.
		constexpr UDim2 operator-(const UDim2 &other) const {
			return {X - other.X, Y - other.Y};
		}

		// Negates both axes.
		constexpr UDim2 operator-() const {
			return {-X, -Y};
		}

		// Reports whether both axes are exactly equal.
		constexpr bool operator==(const UDim2 &other) const {
			return X == other.X && Y == other.Y;
		}

		// Resolves against a parent size.
		//
		// @param parentSize The parent's extent on both axes.
		// @return The absolute size.
		constexpr Vector2 Resolve(const Vector2 &parentSize) const {
			return {X.Resolve(parentSize.X), Y.Resolve(parentSize.Y)};
		}

		// Interpolates both axes linearly without clamping `alpha`.
		constexpr UDim2 Lerp(const UDim2 &target, float alpha) const {
			return {
				UDim{
					X.Scale + (target.X.Scale - X.Scale) * alpha,
					X.Offset + (target.X.Offset - X.Offset) * alpha
				},
				UDim{
					Y.Scale + (target.Y.Scale - Y.Scale) * alpha,
					Y.Offset + (target.Y.Offset - Y.Offset) * alpha
				}
			};
		}
	};
}
