#pragma once

#include "../Mesh2DPayload.hpp"
#include "../SimulationAliases.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace engine::imagegraph::detail {
	inline const Value *VerletScopeValue(const NodeContext &context, std::string_view port) {
		for (const auto &[id, value] : context.InlineOwnerValues)
			if (id == port) return value;
		return nullptr;
	}
	inline int64_t VerletScopeInteger(NodeContext &context, std::string_view port, int64_t fallback) {
		const Value *value = VerletScopeValue(context, port);
		if (!value) return fallback;
		if (const auto *integer = std::get_if<int64_t>(value)) return *integer;
		if (const auto *choice = std::get_if<EnumValue>(value)) return choice->Value;
		if (const auto *scalar = std::get_if<double>(value);
			scalar && std::isfinite(*scalar) && *scalar >= double(std::numeric_limits<int64_t>::min()) &&
			*scalar < double(std::numeric_limits<int64_t>::max()))
			return int64_t(*scalar);
		context.Fail(Status::InvalidValue, "inline simulation control is not an integer", port);
		return fallback;
	}
	inline Vector2 VerletScopeVector(NodeContext &context, std::string_view port, Vector2 fallback) {
		const Value *value = VerletScopeValue(context, port);
		if (!value) return fallback;
		if (const auto *vector = std::get_if<Vector2>(value)) return *vector;
		context.Fail(Status::InvalidValue, "inline simulation control is not a vector", port);
		return fallback;
	}
	inline Vector2 VerletScopeDimension(NodeContext &context) {
		if (context.InlineOwnerId.empty())
			return {double(context.Project.SurfaceWidth), double(context.Project.SurfaceHeight)};
		Vector2 dimension = VerletScopeVector(context, "dimension", {1, 1});
		if (context.InlineOwnerType == "pc.pixel_builder") return dimension;
		const bool linked =
			std::find(
				context.InlineOwnerLinkedValues.begin(), context.InlineOwnerLinkedValues.end(), "dimension"
			) != context.InlineOwnerLinkedValues.end();
		if (!linked) {
			const int64_t unit = VerletScopeInteger(context, "dimension_unit", 1);
			if (unit == 1) {
				dimension.X *= context.Project.SurfaceWidth;
				dimension.Y *= context.Project.SurfaceHeight;
			} else if (unit == 2) {
				bool found = false;
				for (const auto &[port, image] : context.InlineOwnerImages)
					if (port == "dimension" && image) {
						dimension = {double(image->Width), double(image->Height)};
						found = true;
						break;
					}
				if (!found)
					context.Fail(
						Status::UnsupportedExecution,
						"inline mask dimension has no resolved surface",
						"dimension"
					);
			} else if (unit != 0)
				context.Fail(
					Status::InvalidValue, "inline dimension unit is outside its range", "dimension_unit"
				);
		}
		return dimension;
	}
	inline Area VerletMeshArea(NodeContext &context) {
		Area area =
			context.Get<Area>("area", {.CenterX = .5, .CenterY = .5, .HalfWidth = .5, .HalfHeight = .5});
		if (!context.IsLinked("area") && context.Integer("area_unit", 1) == 1) {
			const Vector2 dimension = VerletScopeDimension(context);
			area.CenterX *= dimension.X;
			area.HalfWidth *= dimension.X;
			area.CenterY *= dimension.Y;
			area.HalfHeight *= dimension.Y;
		}
		return area;
	}
	inline bool PublishVerletMesh(NodeContext &context, MeshValue2D mesh) {
		if (!ValidMesh2DPayload(mesh))
			return context.Fail(Status::InvalidValue, "verlet mesh is invalid", "mesh");
		if (!PublishSimulationMeshUpdate(context, mesh)) return false;
		context.SetValue("mesh", std::move(mesh));
		return context.FailureCode == Status::Ok;
	}

	inline std::optional<bool> PreserveVerletForCacheAction(NodeContext &context, const MeshValue2D &mesh) {
		if (!mesh.Data || !mesh.Data->Verlet || context.Request.SimulationCacheCaptures.empty() ||
			!context.Request.SimulationReplay)
			return std::nullopt;
		for (const auto &entry : context.Request.SimulationReplay->Entries)
			if (!entry.Drag && !entry.Cache && entry.NodeId == mesh.Data->OriginNodeId &&
				entry.ProcessorRow == mesh.Data->OriginProcessorRow &&
				entry.State.Tick == context.Request.Tick &&
				entry.State.AuthoringRevision == context.Request.SimulationAuthoringRevision) {
				if (!context.ReserveOutput(Mesh2DStorageBytes<false>(mesh), "mesh")) return false;
				return PublishVerletMesh(context, mesh);
			}
		return std::nullopt;
	}
	inline std::optional<bool> RestoreVerletConstructor(NodeContext &context) {
		const auto *previous = FindSimulationOrigin(context, context.Authored.Id, context.ProcessorRow);
		if (!previous) return std::nullopt;
		if (previous->State.AuthoringRevision != context.Request.SimulationAuthoringRevision)
			return context.Fail(Status::InvalidValue, "verlet constructor revision requires reset", "mesh");
		const uint64_t bytes =
			sizeof(MeshData2D) + RetainedSimulationEntryBytes(*previous) - sizeof(SimulationReplayEntry);
		if (!context.ReserveOutput(bytes, "mesh")) return false;
		MeshValue2D output;
		auto &mesh = output.Data.emplace();
		static_cast<MeshTopology2D &>(mesh) = previous->Topology;
		mesh.Simulation = previous->State.Mesh;
		mesh.Verlet = true;
		mesh.OriginNodeId = previous->NodeId;
		mesh.OriginProcessorRow = previous->ProcessorRow;
		return PublishVerletMesh(context, std::move(output));
	}
	inline void RecomputeVerletMeshBounds(MeshData2D &mesh) {
		Vector2 sum{};
		std::array<double, 4> bounds{
			std::numeric_limits<double>::infinity(),
			std::numeric_limits<double>::infinity(),
			-std::numeric_limits<double>::infinity(),
			-std::numeric_limits<double>::infinity()
		};
		size_t count = 0;
		for (const auto &triangle : mesh.Triangles)
			for (const auto index : triangle) {
				const auto point = mesh.Simulation.Points[index].Position;
				sum.X += point.X;
				sum.Y += point.Y;
				bounds[0] = std::min(bounds[0], point.X);
				bounds[1] = std::min(bounds[1], point.Y);
				bounds[2] = std::max(bounds[2], point.X);
				bounds[3] = std::max(bounds[3], point.Y);
				++count;
			}
		mesh.Center = {};
		if (count) {
			mesh.Center = {sum.X / count, sum.Y / count};
			mesh.Bounds = bounds;
		}
	}
	bool VerletPinMesh(NodeContext &context);
	bool VerletPushMesh(NodeContext &context);
	bool VerletBloat(NodeContext &context);
	bool VerletTear(NodeContext &context);
	bool VerletWind(NodeContext &context);
	bool VerletDragMesh(NodeContext &context);
	bool VerletDiskMesh(NodeContext &context);
	bool VerletPleatMesh(NodeContext &context);
	bool VerletCacheMix(NodeContext &context);
	bool VerletCaptureCache(NodeContext &context);
	bool VerletMeshFromPath(NodeContext &context);
	bool VerletBridgeMesh(NodeContext &context);
	bool VerletRenderStepMesh(NodeContext &context);
	bool VerletRenderMesh(NodeContext &context);
	bool VerletMeshToPath(NodeContext &context);
	bool FlipDomain(NodeContext &context);
	bool FlipUpdate(NodeContext &context);
	bool FlipFill(NodeContext &context);
}
