#pragma once
#include <engine/imagegraph/SourceSdf.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace engine::imagegraph::detail::sdf_math {
	struct V2 {
		float x = 0, y = 0;
		V2() = default;
		V2(float v) : x(v), y(v) {}
		V2(float a, float b) : x(a), y(b) {}
		V2 operator-() const {
			return {-x, -y};
		}
		V2 &operator+=(V2 b) {
			x += b.x;
			y += b.y;
			return *this;
		}
		V2 &operator-=(V2 b) {
			x -= b.x;
			y -= b.y;
			return *this;
		}
	};
	struct V3 {
		float x = 0, y = 0, z = 0;
		V3() = default;
		V3(float v) : x(v), y(v), z(v) {}
		V3(float a, float b, float c) : x(a), y(b), z(c) {}
		V3(V2 a, float c) : x(a.x), y(a.y), z(c) {}
		V3(float a, V2 b) : x(a), y(b.x), z(b.y) {}
		V2 xy() const {
			return {x, y};
		}
		V2 xz() const {
			return {x, z};
		}
		V2 yz() const {
			return {y, z};
		}
		V3 yzx() const {
			return {y, z, x};
		}
		V3 zxy() const {
			return {z, x, y};
		}
		V3 operator-() const {
			return {-x, -y, -z};
		}
		V3 &operator-=(V3 b) {
			x -= b.x;
			y -= b.y;
			z -= b.z;
			return *this;
		}
	};
	inline V2 operator+(V2 a, V2 b) {
		return {a.x + b.x, a.y + b.y};
	}
	inline V2 operator-(V2 a, V2 b) {
		return {a.x - b.x, a.y - b.y};
	}
	inline V2 operator*(V2 a, V2 b) {
		return {a.x * b.x, a.y * b.y};
	}
	inline V2 operator/(V2 a, V2 b) {
		return {a.x / b.x, a.y / b.y};
	}
	inline V3 operator+(V3 a, V3 b) {
		return {a.x + b.x, a.y + b.y, a.z + b.z};
	}
	inline V3 operator-(V3 a, V3 b) {
		return {a.x - b.x, a.y - b.y, a.z - b.z};
	}
	inline V3 operator*(V3 a, V3 b) {
		return {a.x * b.x, a.y * b.y, a.z * b.z};
	}
	inline V3 operator/(V3 a, V3 b) {
		return {a.x / b.x, a.y / b.y, a.z / b.z};
	}
	inline float dot(V2 a, V2 b) {
		return a.x * b.x + a.y * b.y;
	}
	inline float dot(V3 a, V3 b) {
		return a.x * b.x + a.y * b.y + a.z * b.z;
	}
	inline float dot2(V2 a) {
		return dot(a, a);
	}
	inline float length(V2 a) {
		return std::sqrt(dot(a, a));
	}
	inline float length(V3 a) {
		return std::sqrt(dot(a, a));
	}
	inline float abs(float a) {
		return std::abs(a);
	}
	inline V2 abs(V2 a) {
		return {abs(a.x), abs(a.y)};
	}
	inline V3 abs(V3 a) {
		return {abs(a.x), abs(a.y), abs(a.z)};
	}
	inline float min(float a, float b) {
		return std::min(a, b);
	}
	inline float max(float a, float b) {
		return std::max(a, b);
	}
	inline V2 max(V2 a, V2 b) {
		return {max(a.x, b.x), max(a.y, b.y)};
	}
	inline V3 max(V3 a, V3 b) {
		return {max(a.x, b.x), max(a.y, b.y), max(a.z, b.z)};
	}
	inline float clamp(float a, float b, float c) {
		return std::clamp(a, b, c);
	}
	inline float sign(float a) {
		return a > 0 ? 1.f : a < 0 ? -1.f : 0.f;
	}
	inline float mod(float a, float b) {
		return a - b * std::floor(a / b);
	}
	inline float mix(float a, float b, float t) {
		return a * (1.f - t) + b * t;
	}
	inline float sqrt(float a) {
		return std::sqrt(a);
	}
	inline float sin(float a) {
		return std::sin(a);
	}
	inline float cos(float a) {
		return std::cos(a);
	}
	inline float atan(float a, float b) {
		return std::atan2(a, b);
	}
	float Primitive(int32_t shape, V3 p, const SourceSdfShape &data);
}
