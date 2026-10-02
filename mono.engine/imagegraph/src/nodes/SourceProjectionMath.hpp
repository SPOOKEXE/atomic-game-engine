#pragma once
#include <array>
#include <cmath>
#include <numbers>
namespace engine::imagegraph::detail {
	using ProjectionVector = std::array<float, 3>;
	using ProjectionMatrix = std::array<ProjectionVector, 3>;
	inline ProjectionVector ProjectionApply(const ProjectionMatrix &m, const ProjectionVector &v) {
		ProjectionVector out{};
		for (size_t r = 0; r < 3; r++)
			for (size_t c = 0; c < 3; c++)
				out[r] += m[r][c] * v[c];
		return out;
	}
	inline ProjectionMatrix ProjectionMultiply(const ProjectionMatrix &a, const ProjectionMatrix &b) {
		ProjectionMatrix out{};
		for (size_t r = 0; r < 3; r++)
			for (size_t c = 0; c < 3; c++)
				for (size_t k = 0; k < 3; k++)
					out[r][c] += a[r][k] * b[k][c];
		return out;
	}
	// Source matrices are GLSL column constructors, stored here by row.
	inline bool ProjectionInverseRotation(const ProjectionVector &angle, ProjectionMatrix &inverse) {
		const float radians = float(std::numbers::pi) / 180;
		const float cx = std::cos(angle[0] * radians), sx = std::sin(angle[0] * radians),
					cy = std::cos(angle[1] * radians), sy = std::sin(angle[1] * radians),
					cz = std::cos(angle[2] * radians), sz = std::sin(angle[2] * radians);
		const ProjectionMatrix x{{{1, 0, 0}, {0, cx, sx}, {0, -sx, cx}}},
			y{{{cy, 0, -sy}, {0, 1, 0}, {sy, 0, cy}}}, z{{{cz, sz, 0}, {-sz, cz, 0}, {0, 0, 1}}};
		const auto m = ProjectionMultiply(ProjectionMultiply(x, y), z);
		const float a00 = m[0][0], a01 = m[1][0], a02 = m[2][0], a10 = m[0][1], a11 = m[1][1], a12 = m[2][1],
					a20 = m[0][2], a21 = m[1][2], a22 = m[2][2];
		const float b01 = a22 * a11 - a12 * a21, b11 = -a22 * a10 + a12 * a20, b21 = a21 * a10 - a11 * a20;
		const float determinant = a00 * b01 + a01 * b11 + a02 * b21;
		if (!std::isfinite(determinant) || determinant == 0) return false;
		inverse = {
			{{b01, b11, b21},
			 {-a22 * a01 + a02 * a21, a22 * a00 - a02 * a20, -a21 * a00 + a01 * a20},
			 {a12 * a01 - a02 * a11, -a12 * a00 + a02 * a10, a11 * a00 - a01 * a10}}
		};
		for (auto &row : inverse)
			for (float &value : row) {
				value /= determinant;
				if (!std::isfinite(value)) return false;
			}
		return true;
	}
	inline bool ProjectionFinite(const ProjectionVector &v) {
		for (float c : v)
			if (!std::isfinite(c)) return false;
		return true;
	}
	inline bool ProjectionRay(
		const ProjectionMatrix &inverse,
		const ProjectionVector &position,
		float u,
		float v,
		float aspect,
		int projection,
		float fov,
		float distance,
		float scale,
		ProjectionVector &eye,
		ProjectionVector &direction
	) {
		const float x = u - .5f, y = (v - .5f) / aspect;
		if (projection == 0) {
			const float dz = 1 / std::tan(fov * float(std::numbers::pi) / 180 / 2);
			direction = {x * 2, y * 2, -dz};
			eye = {0, 0, std::sqrt(3.f) * distance};
		} else {
			direction = {0, 0, -1};
			eye = {x * scale, y * scale, std::sqrt(3.f)};
		}
		direction = ProjectionApply(inverse, direction);
		const float len = std::sqrt(
			direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2]
		);
		if (!std::isfinite(len) || len == 0) return false;
		for (float &c : direction)
			c /= len;
		eye = ProjectionApply(inverse, eye);
		for (size_t c = 0; c < 3; c++)
			eye[c] -= position[c];
		return ProjectionFinite(eye) && ProjectionFinite(direction);
	}
}
