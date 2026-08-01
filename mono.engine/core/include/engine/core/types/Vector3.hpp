#pragma once

// A pure three-component value with no allocation, logging, or global state.
//
// The spelling follows Roblox's, because the people this engine is for already
// know it. glm is the arithmetic underneath, never the type in an interface.
//
// @tier L1 · shared

#include <cmath>

namespace engine::core {

	// A three-component floating-point value for positions, directions, and scales.
	//
	// Components have no built-in unit; callers must use one unit consistently.
	// Engine world coordinates are right-handed: +X is right, +Y is up, and -Z
	// is forward.
	struct Vector3 {
		// X component, in the caller's chosen unit.
		float X = 0.0f;
		// Y component, in the caller's chosen unit.
		float Y = 0.0f;
		// Z component, in the caller's chosen unit.
		float Z = 0.0f;

		// Constructs the zero vector.
		constexpr Vector3() = default;
		// Constructs a vector from three components in the same caller-chosen unit.
		//
		// @param x The X component.
		// @param y The Y component.
		// @param z The Z component.
		constexpr Vector3(float x, float y, float z)
			: X(x)
			, Y(y)
			, Z(z) {
		}

		// The vector `(0, 0, 0)`.
		static const Vector3 Zero;
		// The vector `(1, 1, 1)`.
		static const Vector3 One;
		// The unit vector along +X.
		static const Vector3 XAxis;
		// The unit vector along +Y.
		static const Vector3 YAxis;
		// The unit vector along +Z.
		static const Vector3 ZAxis;

		// Adds corresponding components.
		constexpr Vector3 operator+(const Vector3 &other) const {
			return { X + other.X, Y + other.Y, Z + other.Z };
		}
		// Subtracts corresponding components.
		constexpr Vector3 operator-(const Vector3 &other) const {
			return { X - other.X, Y - other.Y, Z - other.Z };
		}
		// Negates every component.
		constexpr Vector3 operator-() const {
			return { -X, -Y, -Z };
		}
		// Multiplies every component by a scalar.
		constexpr Vector3 operator*(float scalar) const {
			return { X * scalar, Y * scalar, Z * scalar };
		}
		// Divides every component by a scalar without checking for zero.
		constexpr Vector3 operator/(float scalar) const {
			return { X / scalar, Y / scalar, Z / scalar };
		}

		// Multiplies corresponding components, matching Roblox.
		//
		// This is not a dot product; use Dot() for that operation.
		constexpr Vector3 operator*(const Vector3 &other) const {
			return { X * other.X, Y * other.Y, Z * other.Z };
		}

		// Reports whether all components are exactly equal.
		constexpr bool operator==(const Vector3 &other) const {
			return X == other.X && Y == other.Y && Z == other.Z;
		}

		// Returns the scalar dot product with another vector.
		constexpr float Dot(const Vector3 &other) const {
			return X * other.X + Y * other.Y + Z * other.Z;
		}

		// Returns the right-handed cross product with another vector.
		constexpr Vector3 Cross(const Vector3 &other) const {
			return {
				Y * other.Z - Z * other.Y,
				Z * other.X - X * other.Z,
				X * other.Y - Y * other.X,
			};
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
		// Returning Zero for a directionless vector avoids introducing NaN values.
		Vector3 Unit() const {
			const float length = Magnitude();
			if (length <= 0.0f) {
				return Zero;
			}
			return *this / length;
		}

		// Interpolates each component linearly toward `target` without clamping `alpha`.
		//
		// `alpha` 0 returns this vector, 1 returns `target`, and values outside that
		// range extrapolate.
		constexpr Vector3 Lerp(const Vector3 &target, float alpha) const {
			return *this + (target - *this) * alpha;
		}
	};

	// Multiplies every vector component by a scalar.
	constexpr Vector3 operator*(float scalar, const Vector3 &vector) {
		return vector * scalar;
	}

	// Defined out of class, and `const` rather than `constexpr`: a constexpr
	// static member of the class's own type cannot be declared inside it, and
	// the declaration and the definition have to agree.
	inline const Vector3 Vector3::Zero { 0.0f, 0.0f, 0.0f };
	inline const Vector3 Vector3::One { 1.0f, 1.0f, 1.0f };
	inline const Vector3 Vector3::XAxis { 1.0f, 0.0f, 0.0f };
	inline const Vector3 Vector3::YAxis { 0.0f, 1.0f, 0.0f };
	inline const Vector3 Vector3::ZAxis { 0.0f, 0.0f, 1.0f };
}
