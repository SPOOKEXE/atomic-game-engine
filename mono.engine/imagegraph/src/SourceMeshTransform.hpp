#pragma once

#include "MeshPayload.hpp"

namespace engine::imagegraph::detail {
	inline Vector3 SourceRotate(Quaternion rotation, Vector3 point) {
		const double norm = std::sqrt(
			rotation.X * rotation.X + rotation.Y * rotation.Y + rotation.Z * rotation.Z +
			rotation.W * rotation.W
		);
		if (norm == 0) return {NAN, NAN, NAN};
		const double x = rotation.X / norm, y = rotation.Y / norm, z = rotation.Z / norm,
					 w = rotation.W / norm;
		return {
			(1 - 2 * (y * y + z * z)) * point.X + 2 * (x * y - w * z) * point.Y +
				2 * (x * z + w * y) * point.Z,
			2 * (x * y + w * z) * point.X + (1 - 2 * (x * x + z * z)) * point.Y +
				2 * (y * z - w * x) * point.Z,
			2 * (x * z - w * y) * point.X + 2 * (y * z + w * x) * point.Y +
				(1 - 2 * (x * x + y * y)) * point.Z
		};
	}
	inline Vector3 SourceMeshPoint(const MeshTransform3D &transform, Vector3 point, bool direction = false) {
		if (!direction)
			point = {
				point.X - transform.Anchor.X, point.Y - transform.Anchor.Y, point.Z - transform.Anchor.Z
			};
		point = {point.X * transform.Scale.X, point.Y * transform.Scale.Y, point.Z * transform.Scale.Z};
		point = SourceRotate(transform.Rotation, point);
		if (!direction)
			point = {
				point.X + transform.Position.X, point.Y + transform.Position.Y, point.Z + transform.Position.Z
			};
		return point;
	}
}
