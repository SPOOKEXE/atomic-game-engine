#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/VectorField.hpp>

#include <algorithm>
#include <cmath>

namespace engine::scene {

	namespace {
		float AxisWeight(float coordinate, float halfExtent, float falloff) {
			if (!(halfExtent > 0.0f)) {
				return 1.0f;
			}
			const float distance = halfExtent - std::abs(coordinate);
			if (!(distance >= 0.0f)) {
				return 0.0f;
			}
			if (!(falloff > 0.0f)) {
				return 1.0f;
			}
			return std::clamp(distance / falloff, 0.0f, 1.0f);
		}

		float BoundsWeight(
			const core::Vector3 &point, const core::Vector3 &extent, float falloff, bool twoDimensional
		) {
			float weight =
				std::min(AxisWeight(point.X, extent.X, falloff), AxisWeight(point.Z, extent.Z, falloff));
			if (!twoDimensional) {
				weight = std::min(weight, AxisWeight(point.Y, extent.Y, falloff));
			}
			return weight;
		}
	}

	VectorFieldSample ResolveVectorField(const ecs::Store &store, ecs::Entity instance) {
		for (ecs::Entity current = instance; current != ecs::NULL_ENTITY; current = store.ParentOf(current)) {
			const Transform *transform = store.Get<Transform>(current);
			if (const VectorField2D *field = store.Get<VectorField2D>(current)) {
				return {
					transform == nullptr ? core::CFrame{} : transform->Frame,
					{field->Vector.X, 0.0f, field->Vector.Y},
					{field->HalfExtent.X, 0.0f, field->HalfExtent.Y},
					{0.0f, 1.0f, 0.0f},
					current,
					field->Radial,
					field->Tangential,
					field->Falloff,
					field->LocalSpace,
					true,
				};
			}
			if (const VectorField3D *field = store.Get<VectorField3D>(current)) {
				return {
					transform == nullptr ? core::CFrame{} : transform->Frame,
					field->Vector,
					field->HalfExtent,
					field->Axis,
					current,
					field->Radial,
					field->Tangential,
					field->Falloff,
					field->LocalSpace,
					false,
				};
			}
		}
		return {};
	}

	core::Vector3 SampleVectorField(const VectorFieldSample &field, const core::Vector3 &worldPoint) {
		if (field.Source == ecs::NULL_ENTITY) {
			return core::Vector3::Zero;
		}

		const core::Vector3 local =
			field.LocalSpace ? field.Frame.PointToObjectSpace(worldPoint) : worldPoint - field.Frame.Position;
		const float weight = BoundsWeight(local, field.HalfExtent, field.Falloff, field.TwoDimensional);
		if (!(weight > 0.0f)) {
			return core::Vector3::Zero;
		}

		core::Vector3 direction = local;
		if (field.TwoDimensional) {
			direction.Y = 0.0f;
		}
		const core::Vector3 radial = direction.Unit();
		const core::Vector3 axis = field.TwoDimensional ? core::Vector3::YAxis : field.Axis.Unit();
		core::Vector3 force = field.Vector + radial * field.Radial;
		if (field.Tangential != 0.0f && axis.MagnitudeSquared() > 0.0f) {
			force = force + axis.Cross(radial).Unit() * field.Tangential;
		}
		if (field.LocalSpace) {
			force = field.Frame.VectorToWorldSpace(force);
		}
		return force * weight;
	}
}
