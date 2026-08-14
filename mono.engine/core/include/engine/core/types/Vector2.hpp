#pragma once

// A pure two-component value, spelled the way Roblox spells it.
//
// **Not a truncated `Vector3`.** The two answer different questions - a screen
// position, a texture coordinate and a UI offset are all genuinely flat - and a
// three-component value carrying a zero Z is a third of a cache line spent
// saying nothing. Everything here is the two-dimensional case written out
// rather than `Vector3` with a component ignored.
//
// @tier L1 · shared

#include <cmath>

namespace engine::core {

	// A two-component floating-point value for screen positions, texture
	// coordinates and flat offsets.
	//
	// Components have no built-in unit; callers must use one unit consistently.
	//
	// @since v0.6
	struct Vector2 {
		// X component, in the caller's chosen unit.
		float X = 0.0f;
		// Y component, in the caller's chosen unit.
		float Y = 0.0f;

		// Constructs the zero vector.
		constexpr Vector2() = default;

		// Constructs a vector from two components in the same caller-chosen unit.
		//
		// @param x The X component.
		// @param y The Y component.
		constexpr Vector2(float x, float y) : X(x), Y(y) {}

		// The vector `(0, 0)`.
		static const Vector2 Zero;
		// The vector `(1, 1)`.
		static const Vector2 One;
		// The unit vector along +X.
		static const Vector2 XAxis;
		// The unit vector along +Y.
		static const Vector2 YAxis;

		// Adds corresponding components.
		constexpr Vector2 operator+(const Vector2 &other) const {
			return {X + other.X, Y + other.Y};
		}
		// Subtracts corresponding components.
		constexpr Vector2 operator-(const Vector2 &other) const {
			return {X - other.X, Y - other.Y};
		}
		// Negates both components.
		constexpr Vector2 operator-() const {
			return {-X, -Y};
		}
		// Multiplies both components by a scalar.
		constexpr Vector2 operator*(float scalar) const {
			return {X * scalar, Y * scalar};
		}
		// Divides both components by a scalar without checking for zero.
		constexpr Vector2 operator/(float scalar) const {
			return {X / scalar, Y / scalar};
		}

		// Multiplies corresponding components, matching Roblox.
		//
		// This is not a dot product; use Dot() for that operation.
		constexpr Vector2 operator*(const Vector2 &other) const {
			return {X * other.X, Y * other.Y};
		}

		// Reports whether both components are exactly equal.
		constexpr bool operator==(const Vector2 &other) const {
			return X == other.X && Y == other.Y;
		}

		// Returns the scalar dot product with another vector.
		constexpr float Dot(const Vector2 &other) const {
			return X * other.X + Y * other.Y;
		}

		// Returns the two-dimensional cross product - the signed area of the
		// parallelogram the two vectors span.
		//
		// A scalar rather than a vector, because the only direction a flat cross
		// product can point is out of the plane. The sign is the winding, which
		// is what a caller asking this actually wants.
		constexpr float Cross(const Vector2 &other) const {
			return X * other.Y - Y * other.X;
		}

		// Returns the squared length in squared component units.
		constexpr float MagnitudeSquared() const {
			return Dot(*this);
		}

		// Returns the Euclidean length in component units.
		float Magnitude() const {
			return std::sqrt(MagnitudeSquared());
		}

		// Returns the same direction with length one, or Zero for a zero vector.
		//
		// Returning Zero for a directionless vector avoids introducing NaN
		// values, exactly as `Vector3::Unit` does.
		Vector2 Unit() const {
			const float length = Magnitude();
			if (length <= 0.0f) {
				return Zero;
			}
			return *this / length;
		}

		// Interpolates each component linearly toward `target` without clamping
		// `alpha`.
		//
		// `alpha` 0 returns this vector, 1 returns `target`, and values outside
		// that range extrapolate.
		constexpr Vector2 Lerp(const Vector2 &target, float alpha) const {
			return *this + (target - *this) * alpha;
		}
	};

	// Multiplies both vector components by a scalar.
	constexpr Vector2 operator*(float scalar, const Vector2 &vector) {
		return vector * scalar;
	}

	// Defined out of class, and `const` rather than `constexpr`, for the reason
	// `Vector3` gives: a constexpr static member of the class's own type cannot
	// be declared inside it.
	inline const Vector2 Vector2::Zero{0.0f, 0.0f};
	inline const Vector2 Vector2::One{1.0f, 1.0f};
	inline const Vector2 Vector2::XAxis{1.0f, 0.0f};
	inline const Vector2 Vector2::YAxis{0.0f, 1.0f};
}
