#include <engine/imagegraph/SourceInstance3D.hpp>

#include <array>
#include <cmath>
namespace engine::imagegraph {
	namespace {
		using Triple = std::array<float, 3>;
		Triple Euler(const MeshInstance3D &instance, Vector3 point) {
			const auto &fields = instance.Fields;
			const float cx = std::cos(fields[4]), cy = std::cos(fields[5]), cz = std::cos(fields[6]),
						sx = std::sin(fields[4]), sy = std::sin(fields[5]), sz = std::sin(fields[6]);
			const float x = float(point.X), y = float(point.Y), z = float(point.Z);
			return {
				cy * cz * x + cy * sz * y - sy * z,
				(-cx * sz + sx * sy * cz) * x + (cx * cz + sx * sy * sz) * y + sx * cy * z,
				(sx * sz + cx * sy * cz) * x + (-sx * cz + cx * sy * sz) * y + cx * cy * z
			};
		}
	}
	Vector3 SourceInstanceNormal3D(const MeshInstance3D &instance, Vector3 normal) {
		const auto p = Euler(instance, normal);
		return {p[0], p[1], p[2]};
	}
	Vector3 SourceInstancePosition3D(const MeshInstance3D &instance, Vector3 position) {
		auto p = Euler(instance, position);
		const auto &f = instance.Fields;
		float nx = f[12], ny = f[13], nz = f[14];
		const float n = std::sqrt(nx * nx + ny * ny + nz * nz);
		if (n > 0) {
			nx /= n;
			ny /= n;
			nz /= n;
			float xx = -ny, xy = nx, xz = 0;
			const float length = std::sqrt(xx * xx + xy * xy + xz * xz);
			if (length >= .0001f) {
				xx /= length;
				xy /= length;
				const float yx = ny * xz - nz * xy, yy = nz * xx - nx * xz, yz = nx * xy - ny * xx;
				p = {
					xx * p[0] + xy * p[1] + xz * p[2],
					yx * p[0] + yy * p[1] + yz * p[2],
					nx * p[0] + ny * p[1] + nz * p[2]
				};
			}
		}
		return {double(p[0] * f[8] + f[0]), double(p[1] * f[9] + f[1]), double(p[2] * f[10] + f[2])};
	}
}
