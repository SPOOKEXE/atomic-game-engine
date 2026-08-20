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
		constexpr Vector3(float x, float y, float z) : X(x), Y(y), Z(z) {}

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
			return {X + other.X, Y + other.Y, Z + other.Z};
		}
		// Subtracts corresponding components.
		constexpr Vector3 operator-(const Vector3 &other) const {
			return {X - other.X, Y - other.Y, Z - other.Z};
		}
		// Negates every component.
		constexpr Vector3 operator-() const {
			return {-X, -Y, -Z};
		}
		// Multiplies every component by a scalar.
		constexpr Vector3 operator*(float scalar) const {
			return {X * scalar, Y * scalar, Z * scalar};
		}
		// Divides every component by a scalar without checking for zero.
		constexpr Vector3 operator/(float scalar) const {
			return {X / scalar, Y / scalar, Z / scalar};
		}

		// Multiplies corresponding components, matching Roblox.
		//
		// This is not a dot product; use Dot() for that operation.
		constexpr Vector3 operator*(const Vector3 &other) const {
			return {X * other.X, Y * other.Y, Z * other.Z};
		}

		// Divides corresponding components without checking for zero.
		//
		// Unchecked for the reason the scalar overload is: a value type that
		// branched per component would charge every caller for a test only the
		// caller can act on, and an infinity is at least visible in the result.
		constexpr Vector3 operator/(const Vector3 &other) const {
			return {X / other.X, Y / other.Y, Z / other.Z};
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

		// Returns the magnitude of each component, dropping the signs.
		constexpr Vector3 Abs() const {
			return {X < 0.0f ? -X : X, Y < 0.0f ? -Y : Y, Z < 0.0f ? -Z : Z};
		}

		// Returns each component rounded down to a whole number.
		//
		// Down, not toward zero: `-2.6` floors to `-3`. That is what Roblox
		// answers and what Luau's `//` means, and truncating instead would put a
		// one-unit step at the origin that no caller asked for.
		Vector3 Floor() const {
			return {std::floor(X), std::floor(Y), std::floor(Z)};
		}

		// Returns each component rounded up to a whole number.
		Vector3 Ceil() const {
			return {std::ceil(X), std::ceil(Y), std::ceil(Z)};
		}

		// Returns -1, 0 or 1 per component, according to its sign.
		//
		// A zero component answers zero rather than a sign, so a vector already
		// on an axis plane stays on it.
		constexpr Vector3 Sign() const {
			const auto sign = [](float value) { return value > 0.0f ? 1.0f : value < 0.0f ? -1.0f : 0.0f; };
			return {sign(X), sign(Y), sign(Z)};
		}

		// Returns the larger of each pair of corresponding components.
		constexpr Vector3 Max(const Vector3 &other) const {
			return {
				X > other.X ? X : other.X,
				Y > other.Y ? Y : other.Y,
				Z > other.Z ? Z : other.Z,
			};
		}

		// Returns the smaller of each pair of corresponding components.
		constexpr Vector3 Min(const Vector3 &other) const {
			return {
				X < other.X ? X : other.X,
				Y < other.Y ? Y : other.Y,
				Z < other.Z ? Z : other.Z,
			};
		}

		// Returns the unsigned angle to `other` in radians, from zero to pi.
		//
		// **`atan2` of the cross product's length against the dot, not
		// `acos(dot / lengths)`.** The `acos` form loses most of its precision
		// exactly where directions are nearly parallel - which is where a "have I
		// arrived yet" test lives - and rounding can push its argument past 1.0,
		// where it answers NaN rather than the zero angle it was asked for.
		//
		// A zero vector has no direction and answers zero, matching Unit().
		float Angle(const Vector3 &other) const {
			return std::atan2(Cross(other).Magnitude(), Dot(other));
		}

		// Returns the angle to `other` in radians, signed by which way `axis` points.
		//
		// Negative when turning from this vector to `other` goes the opposite way
		// round `axis` by the right hand. `axis` need not be unit length and need
		// not be perpendicular; only the sign of its dot with the cross is read.
		float Angle(const Vector3 &other, const Vector3 &axis) const {
			const float unsigned_angle = Angle(other);
			return axis.Dot(Cross(other)) < 0.0f ? -unsigned_angle : unsigned_angle;
		}

		// Reports whether `other` is within `epsilon` of this vector.
		//
		// **The tolerance is relative to the longer of the two**, so one epsilon
		// works for a normal and for a position a thousand studs out - an absolute
		// one is either useless at that distance or far too loose near the origin.
		// Vectors shorter than one unit are compared against a plain `epsilon`
		// rather than a shrinking one, which is what keeps a near-zero vector from
		// only ever matching itself.
		//
		// @param other   The vector to compare against.
		// @param epsilon The tolerance, in component units at unit scale.
		constexpr bool FuzzyEq(const Vector3 &other, float epsilon = 1.0e-5f) const {
			const float longest =
				MagnitudeSquared() > other.MagnitudeSquared() ? MagnitudeSquared() : other.MagnitudeSquared();
			const float scale = longest > 1.0f ? longest : 1.0f;
			return (*this - other).MagnitudeSquared() <= epsilon * epsilon * scale;
		}
	};

	// Multiplies every vector component by a scalar.
	constexpr Vector3 operator*(float scalar, const Vector3 &vector) {
		return vector * scalar;
	}

	// Defined out of class, and `const` rather than `constexpr`: a constexpr
	// static member of the class's own type cannot be declared inside it, and
	// the declaration and the definition have to agree.
	inline const Vector3 Vector3::Zero{0.0f, 0.0f, 0.0f};
	inline const Vector3 Vector3::One{1.0f, 1.0f, 1.0f};
	inline const Vector3 Vector3::XAxis{1.0f, 0.0f, 0.0f};
	inline const Vector3 Vector3::YAxis{0.0f, 1.0f, 0.0f};
	inline const Vector3 Vector3::ZAxis{0.0f, 0.0f, 1.0f};
}
