#include "Inertia.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>

namespace engine::physics {

	namespace {
		// One component by index lets the three principal axes use the same
		// calculation without building a matrix.
		float Component(const core::Vector3 &vector, size_t index) {
			return index == 0 ? vector.X : (index == 1 ? vector.Y : vector.Z);
		}
	}

	core::Vector3 InverseInertiaOf(const scene::Collider &collider, float mass) {
		// Collider extents are half-extents. Reading them as full extents makes
		// every body four times harder to turn while looking like a tuning error.
		const float extentX = collider.Extent.X;
		const float extentY = collider.Extent.Y;
		const float extentZ = collider.Extent.Z;

		core::Vector3 inertia;
		switch (collider.Shape) {
		case scene::ShapeKind::Box:
			inertia = core::Vector3{
				mass * (extentY * extentY + extentZ * extentZ) / 3.0f,
				mass * (extentX * extentX + extentZ * extentZ) / 3.0f,
				mass * (extentX * extentX + extentY * extentY) / 3.0f,
			};
			break;

		case scene::ShapeKind::Sphere: {
			const float solid = 0.4f * mass * extentX * extentX;
			inertia = core::Vector3{solid, solid, solid};
			break;
		}

		case scene::ShapeKind::Cylinder: {
			// About the barrel it is a disc; across it, a disc plus a rod.
			const float across = mass * (3.0f * extentX * extentX + 4.0f * extentY * extentY) / 12.0f;
			inertia = core::Vector3{across, 0.5f * mass * extentX * extentX, across};
			break;
		}

		case scene::ShapeKind::Capsule: {
			const float radius = std::max(extentX, 0.0f);
			const float halfSegment = std::max(extentY, 0.0f);
			const float cylinderVolume = std::numbers::pi_v<float> * radius * radius * (2.0f * halfSegment);
			const float sphereVolume = (4.0f / 3.0f) * std::numbers::pi_v<float> * radius * radius * radius;
			const float totalVolume = cylinderVolume + sphereVolume;
			const float cylinderMass = totalVolume > 0.0f ? mass * cylinderVolume / totalVolume : 0.0f;
			const float sphereMass = mass - cylinderMass;
			const float radiusSquared = radius * radius;
			const float axial = 0.5f * cylinderMass * radiusSquared + 0.4f * sphereMass * radiusSquared;
			const float capCentroid = halfSegment + 3.0f * radius / 8.0f;
			const float across =
				cylinderMass * (3.0f * radiusSquared + 4.0f * halfSegment * halfSegment) / 12.0f +
				sphereMass * (83.0f * radiusSquared / 320.0f + capCentroid * capCentroid);
			inertia = core::Vector3{across, axial, across};
			break;
		}

		case scene::ShapeKind::Hull:
		case scene::ShapeKind::Mesh:
			// Hull volume and mass already use the part extent. Its box tensor is
			// the stable approximation until baked hull integrals exist. Dynamic
			// mesh colliders use the same conservative authored extent.
			inertia = core::Vector3{
				mass * (extentY * extentY + extentZ * extentZ) / 3.0f,
				mass * (extentX * extentX + extentZ * extentZ) / 3.0f,
				mass * (extentX * extentX + extentY * extentY) / 3.0f,
			};
			break;
		}

		return core::Vector3{
			inertia.X > 0.0f ? 1.0f / inertia.X : 0.0f,
			inertia.Y > 0.0f ? 1.0f / inertia.Y : 0.0f,
			inertia.Z > 0.0f ? 1.0f / inertia.Z : 0.0f,
		};
	}

	core::Vector3 AngularAcceleration(
		const std::array<core::Vector3, 3> &principalAxes,
		const core::Vector3 &inverseInertia,
		const core::Vector3 &torque
	) {
		core::Vector3 response;
		for (size_t index = 0; index < principalAxes.size(); index++) {
			const core::Vector3 &axis = principalAxes[index];
			response = response + axis * (Component(inverseInertia, index) * axis.Dot(torque));
		}
		return response;
	}
}
