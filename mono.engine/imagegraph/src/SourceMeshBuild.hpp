#pragma once
#include "NodeExecutors.hpp"
namespace engine::imagegraph::detail {
	struct SourceBuildMaterial {
		const MaterialValue3D *Material = nullptr;
		const Image *Surface = nullptr;
		uint64_t Bytes() const {
			return Material ? MaterialStorageBytes<true>(*Material)
							: (Surface ? sizeof(MaterialData3D) + MeshVectorBytes<true>(Surface->Pixels) : 0);
		}
		MaterialValue3D Clone() const {
			if (Material) return *Material;
			MaterialValue3D result;
			if (Surface) result.Data.emplace().Surface = *Surface;
			return result;
		}
	};
	inline bool
	ReadSourceBuildMaterial(NodeContext &context, std::string_view id, SourceBuildMaterial &result) {
		if (const Value *value = context.Find(id)) {
			result.Material = std::get_if<MaterialValue3D>(value);
			if (!result.Material || !ValidMaterialPayload(*result.Material))
				return context.Fail(Status::TypeMismatch, "mesh material requires a valid descriptor", id);
			if (!result.Material->Get().Surface && context.Input(id)) {
				result.Surface = context.Input(id);
				result.Material = nullptr;
				if (!ValidMaterialSurface(*result.Surface))
					return context.Fail(Status::InvalidValue, "mesh material surface is invalid", id);
			}
		} else {
			result.Surface = context.Input(id);
			if (result.Surface && !ValidMaterialSurface(*result.Surface))
				return context.Fail(Status::InvalidValue, "mesh material surface is invalid", id);
		}
		return true;
	}
	inline bool ReadSourceBuildTransform(NodeContext &context, MeshTransform3D &transform) {
		for (auto [id, target] :
			 {std::pair<std::string_view, Vector3 *>{"position", &transform.Position},
			  {"anchor", &transform.Anchor},
			  {"scale", &transform.Scale}}) {
			if (const Value *value = context.Find(id)) {
				if (const auto *vector = std::get_if<Vector3>(value))
					*target = *vector;
				else if (const auto *scalar = std::get_if<double>(value))
					*target = {*scalar, *scalar, *scalar};
				else if (const auto *integer = std::get_if<int64_t>(value))
					*target = {double(*integer), double(*integer), double(*integer)};
				else
					return context.Fail(Status::TypeMismatch, "mesh transform requires Vector3", id);
			}
			if (!MeshFinite(*target))
				return context.Fail(Status::InvalidValue, "mesh transform must be finite", id);
		}
		if (const Value *value = context.Find("rotation")) {
			const auto *rotation = std::get_if<Quaternion>(value);
			if (!rotation)
				return context.Fail(Status::TypeMismatch, "mesh rotation requires Quaternion", "rotation");
			transform.Rotation = *rotation;
		}
		return MeshFinite(transform.Rotation) ||
			   context.Fail(Status::InvalidValue, "mesh rotation must be finite", "rotation");
	}
	inline Vector3 SourceBuildNormal(Vector3 a, Vector3 b) {
		Vector3 normal{a.Y * b.Z - a.Z * b.Y, a.Z * b.X - a.X * b.Z, a.X * b.Y - a.Y * b.X};
		const double length = std::hypot(normal.X, normal.Y, normal.Z);
		if (length != 0) normal = {normal.X / length, normal.Y / length, normal.Z / length};
		return normal;
	}
}
