#include "SourceShape3DProjection.hpp"

#include "../MeshPayload.hpp"
#include "../NodeExecutors.hpp"

#include <algorithm>
#include <cmath>
#include <new>
#include <numbers>
#include <stdexcept>
#include <utility>

namespace engine::imagegraph::detail {
	namespace source_shape3d_projection {
		bool Profile(SourceShape3DFramebufferProfile profile) {
			return profile == SourceShape3DFramebufferProfile::NativeTopOrigin ||
				   profile == SourceShape3DFramebufferProfile::HTML5FramebufferRows;
		}
		bool Camera(const SourceShape3DRecipe &r) {
			return r.Width && r.Height && r.Width <= Limits::MaximumDimension &&
				   r.Height <= Limits::MaximumDimension && r.CameraEye == Vector3{0, 1, 0} &&
				   r.CameraTarget == Vector3{} && r.CameraUp == Vector3{0, 0, -1} &&
				   r.OrthographicWidth == 1 && r.OrthographicHeight == 1 && r.Near == 0 && r.Far == 2;
		}
		bool World(const SourceShape3DRecipe &recipe, Vector3 &v, bool normal) {
			for (auto step = recipe.WorldStack.rbegin(); step != recipe.WorldStack.rend(); ++step) {
				const auto a = step->Value;
				if (!MeshFinite(a)) return false;
				switch (step->Kind) {
				case SourceShape3DTransformKind::Translate:
					if (!normal) v = {v.X + a.X, v.Y + a.Y, v.Z + a.Z};
					break;
				case SourceShape3DTransformKind::Scale:
					v = {v.X * a.X, v.Y * a.Y, v.Z * a.Z};
					break;
				case SourceShape3DTransformKind::RotateX: {
					const double angle = -a.X * std::numbers::pi / 180, c = std::cos(angle),
								 s = std::sin(angle);
					v = {v.X, c * v.Y - s * v.Z, s * v.Y + c * v.Z};
					break;
				}
				case SourceShape3DTransformKind::RotateY: {
					const double angle = -a.Y * std::numbers::pi / 180, c = std::cos(angle),
								 s = std::sin(angle);
					v = {c * v.X + s * v.Z, v.Y, -s * v.X + c * v.Z};
					break;
				}
				case SourceShape3DTransformKind::RotateZ: {
					const double angle = -a.Z * std::numbers::pi / 180, c = std::cos(angle),
								 s = std::sin(angle);
					v = {c * v.X - s * v.Y, s * v.X + c * v.Y, v.Z};
					break;
				}
				default:
					return false;
				}
				if (!MeshFinite(v)) return false;
			}
			return true;
		}
	}
	bool ProjectSourceShape3DVertex(
		const SourceShape3DRecipe &recipe,
		const MeshVertex3D &vertex,
		SourceShape3DFramebufferProfile profile,
		SourceShape3DRasterVertex &result
	) {
		using namespace source_shape3d_projection;
		if (!Profile(profile) || !Camera(recipe) || !MeshFinite(vertex.Position) ||
			!MeshFinite(vertex.Normal) || !std::isfinite(vertex.UV.X) || !std::isfinite(vertex.UV.Y))
			return false;
		Vector3 position = vertex.Position, normal = vertex.Normal;
		if (!World(recipe, position, false) || !World(recipe, normal, true)) return false;
		// Source multiplies normals by the world linear matrix, not its inverse transpose.
		normal = {-normal.X, -normal.Z, -normal.Y};
		const double length = std::hypot(normal.X, normal.Y, normal.Z);
		if (!std::isfinite(length)) return false;
		if (length != 0) normal = {normal.X / length, normal.Y / length, normal.Z / length};
		const double clipX = -2 * position.X, clipY = -2 * position.Z, clipZ = (1 - position.Y) / 2;
		SourceShape3DRasterVertex prepared;
		prepared.Screen = {
			(clipX + 1) * recipe.Width / 2,
			(profile == SourceShape3DFramebufferProfile::NativeTopOrigin ? 1 - clipY : 1 + clipY) *
				recipe.Height / 2
		};
		prepared.ClipDepth = clipZ;
		prepared.TestDepth =
			profile == SourceShape3DFramebufferProfile::NativeTopOrigin ? clipZ : .5 * clipZ + .5;
		prepared.ViewNormal = normal;
		prepared.UV = vertex.UV;
		prepared.Tint = vertex.Tint;
		if (!std::isfinite(prepared.Screen.X) || !std::isfinite(prepared.Screen.Y) ||
			!std::isfinite(prepared.ClipDepth) || !std::isfinite(prepared.TestDepth))
			return false;
		result = prepared;
		return true;
	}
	bool BuildSourceShape3DProjection(
		NodeContext &context,
		const SourceShape3DRecipe &recipe,
		const MeshValue3D &geometry,
		SourceShape3DFramebufferProfile profile,
		SourceShape3DProjectionResult &result
	) try {
		ENGINE_PROFILE("imagegraph.shape3d.projection");
		if (!source_shape3d_projection::Profile(profile) || !source_shape3d_projection::Camera(recipe))
			return context.Fail(
				Status::InvalidValue,
				"Shape 3D projection requires its fixed source camera and an explicit profile"
			);
		if (!geometry.Data || !geometry.Data->CpuVerticesPresent || geometry.Data->Instanced)
			return context.Fail(
				Status::InvalidValue, "Shape 3D projection requires generated local CPU triangles"
			);
		if (geometry.Data->Parts.empty() || geometry.Data->Parts.size() > Limits::MaximumArrayElements ||
			geometry.Data->LocalTransforms.size() != 1 ||
			geometry.Data->LocalTransforms.front() != MeshTransform3D{})
			return context.Fail(
				Status::InvalidValue, "Shape 3D projection requires generated identity metadata"
			);
		uint64_t count = 0;
		for (const auto &part : geometry.Data->Parts) {
			if (part.LocalMatrix || part.Vertices.empty() || part.Vertices.size() % 3)
				return context.Fail(
					Status::InvalidValue, "Shape 3D projection requires complete local triangles"
				);
			count += part.Vertices.size() / 3;
			if (count > Limits::MaximumArrayElements)
				return context.Fail(
					Status::LimitExceeded, "Shape 3D projected triangle count exceeds bounds"
				);
		}
		// 192 units per triangle cover two seven-step transforms per vertex, trig and validation.
		const uint64_t rows = std::max<size_t>(context.ProcessorCount, 1);
		if (rows > 64000000 || count * 192 > 64000000 / rows)
			return context.Fail(
				Status::LimitExceeded, "Shape 3D projection exceeds whole processor work bounds"
			);
		const uint64_t bytes = count * sizeof(SourceShape3DRasterTriangle);
		if (bytes > Limits::MaximumArrayBytes)
			return context.Fail(Status::LimitExceeded, "Shape 3D projected triangles exceed byte bounds");
		auto charge = context.ReserveWorkspace(bytes);
		if (!charge) return false;
		SourceShape3DProjectionResult prepared;
		prepared.Charge = std::move(*charge);
		prepared.Triangles.reserve(static_cast<size_t>(count));
		for (size_t partIndex = 0; partIndex < geometry.Data->Parts.size(); ++partIndex) {
			const auto &part = geometry.Data->Parts[partIndex];
			for (size_t index = 0; index < part.Vertices.size(); index += 3) {
				SourceShape3DRasterTriangle triangle;
				triangle.Submesh = partIndex;
				for (size_t corner = 0; corner < 3; ++corner)
					if (!ProjectSourceShape3DVertex(
							recipe, part.Vertices[index + corner], profile, triangle.Vertices[corner]
						))
						return context.Fail(Status::InvalidValue, "Shape 3D projected vertex is invalid");
				prepared.Triangles.push_back(triangle);
			}
		}
		result = std::move(prepared);
		return true;
	} catch (const std::bad_alloc &) {
		return context.Fail(Status::LimitExceeded, "Shape 3D projection allocation failed");
	} catch (const std::length_error &) {
		return context.Fail(Status::LimitExceeded, "Shape 3D projection allocation exceeds container bounds");
	}
}
