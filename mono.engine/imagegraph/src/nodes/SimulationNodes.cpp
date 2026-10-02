#include "Families.hpp"
#include "FlipNodes.hpp"
#include "VerletNodes.hpp"

#include <engine/imagegraph/SimulationReplay.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace engine::imagegraph::detail {
	namespace {
		bool Grid(NodeContext &context) {
			if (const auto restored = RestoreVerletConstructor(context)) return *restored;

			const Vector2 subdivision = context.Vec2("subdivision", {4, 4});
			if (!MeshFinite(subdivision))
				return context.Fail(Status::InvalidValue, "grid subdivision must be finite", "subdivision");
			if (subdivision.X != std::trunc(subdivision.X) || subdivision.Y != std::trunc(subdivision.Y))
				return context.Fail(
					Status::UnsupportedExecution,
					"fractional source grid allocation coercion requires a reference capture",
					"subdivision"
				);
			const double columnsValue = std::max(1.0, subdivision.X),
						 rowsValue = std::max(1.0, subdivision.Y);
			if (columnsValue >= Limits::MaximumArrayElements || rowsValue >= Limits::MaximumArrayElements ||
				(columnsValue + 1) * (rowsValue + 1) > Limits::MaximumArrayElements)
				return context.Fail(Status::LimitExceeded, "grid points exceed budget", "subdivision");
			const uint32_t columns = uint32_t(columnsValue), rows = uint32_t(rowsValue);
			const uint64_t pointCount = uint64_t(columns + 1) * (rows + 1);
			const uint64_t edgeCount = uint64_t(rows) * (columns + 1) + uint64_t(rows + 1) * columns;
			const uint64_t triangleCount = uint64_t(rows) * columns * 2;
			const bool quad = context.Boolean("quad");
			if (edgeCount > Limits::MaximumLinks || triangleCount > Limits::MaximumLinks)
				return context.Fail(Status::LimitExceeded, "grid topology exceeds budget", "subdivision");
			const uint64_t bytes = sizeof(MeshData2D) + pointCount * sizeof(VerletPoint) +
								   edgeCount * sizeof(VerletEdge) +
								   triangleCount * sizeof(std::array<uint32_t, 3>) +
								   (quad ? triangleCount / 2 * sizeof(std::array<uint32_t, 2>) : 0);
			if (!context.ReserveOutput(
					bytes + std::max(context.Authored.Id.size(), std::string{}.capacity()), "mesh"
				))
				return false;
			const Area area = VerletMeshArea(context);
			const double flexibility = 1 - context.Scalar("tension", .5);
			const double drag = context.Scalar("drag"), angularDrag = context.Scalar("stiffness");
			if (context.FailureCode != Status::Ok) return false;
			MeshValue2D output;
			auto &mesh = output.Data.emplace();
			mesh.Verlet = true;
			mesh.OriginNodeId = context.Authored.Id;
			mesh.OriginProcessorRow = context.ProcessorRow;
			mesh.Center = {area.CenterX, area.CenterY};
			mesh.Bounds = {
				area.CenterX - area.HalfWidth,
				area.CenterY - area.HalfHeight,
				area.CenterX + area.HalfWidth,
				area.CenterY + area.HalfHeight
			};
			auto &points = mesh.Simulation.Points;
			auto &edges = mesh.Simulation.Edges;
			points.reserve(pointCount);
			edges.reserve(edgeCount);
			mesh.Triangles.reserve(triangleCount);
			if (quad) mesh.Quads.reserve(triangleCount / 2);
			for (uint32_t row = 0; row <= rows; ++row)
				for (uint32_t column = 0; column <= columns; ++column) {
					const Vector2 uv{double(column) / columns, double(row) / rows};
					const Vector2 position{
						mesh.Bounds[0] + (mesh.Bounds[2] - mesh.Bounds[0]) * uv.X,
						mesh.Bounds[1] + (mesh.Bounds[3] - mesh.Bounds[1]) * uv.Y
					};
					VerletPoint point;
					point.Position = point.Previous = point.BeforePrevious = point.Original =
						point.VelocityReference = position;
					point.Drag = drag;
					point.UV = uv;
					point.SourceIndex = uint32_t(points.size());
					points.push_back(point);
				}
			const auto addEdge = [&](uint32_t first, uint32_t second, int64_t previous) {
				const double deltaX = points[second].Position.X - points[first].Position.X;
				const double deltaY = points[second].Position.Y - points[first].Position.Y;
				VerletEdge edge{
					first,
					second,
					std::hypot(deltaX, deltaY),
					flexibility,
					std::atan2(-deltaY, deltaX) * 180 / std::numbers::pi,
					angularDrag
				};
				if (edge.DirectionDegrees < 0) edge.DirectionDegrees += 360;
				edge.PreviousEdge = previous;
				if (previous >= 0) edges[size_t(previous)].NextEdge = int64_t(edges.size());
				edges.push_back(edge);
			};
			for (uint32_t column = 0; column <= columns; ++column) {
				int64_t previous = -1;
				for (uint32_t row = 0; row < rows; ++row) {
					addEdge(row * (columns + 1) + column, (row + 1) * (columns + 1) + column, previous);
					previous = int64_t(edges.size()) - 1;
				}
			}
			for (uint32_t row = 0; row <= rows; ++row) {
				int64_t previous = -1;
				for (uint32_t column = 0; column < columns; ++column) {
					addEdge(row * (columns + 1) + column, row * (columns + 1) + column + 1, previous);
					previous = int64_t(edges.size()) - 1;
				}
			}
			for (uint32_t row = 0; row < rows; ++row)
				for (uint32_t column = 0; column < columns; ++column) {
					const uint32_t first = row * (columns + 1) + column, second = first + 1;
					const uint32_t third = first + columns + 1, fourth = third + 1;
					if (quad)
						mesh.Quads.push_back(
							{uint32_t(mesh.Triangles.size()), uint32_t(mesh.Triangles.size()) + 1}
						);
					mesh.Triangles.push_back({first, second, third});
					mesh.Triangles.push_back({third, second, fourth});
				}
			RecomputeVerletMeshBounds(mesh);
			return PublishVerletMesh(context, std::move(output));
		}
		bool ConvertMesh(NodeContext &context) {
			if (const auto restored = RestoreVerletConstructor(context)) return *restored;
			const Value *input = context.Find("mesh");
			const auto *mesh = input ? std::get_if<MeshValue2D>(input) : nullptr;
			if (!mesh || !mesh->Data) return true;
			if (!ValidMesh2DPayload(*mesh))
				return context.Fail(Status::InvalidValue, "source mesh conversion input is invalid", "mesh");
			if (!mesh->Data->SparseQuads.empty())
				return context.Fail(
					Status::UnsupportedExecution,
					"source sparse quad conversion indexes undefined slots",
					"mesh"
				);
			const double flexibility = 1 - context.Scalar("tension", .5), drag = context.Scalar("drag"),
						 angularDrag = context.Scalar("stiffness");
			const bool remap = context.Boolean("remap");
			Area uv = context.Get<Area>(
				"uv_map", {.CenterX = .5, .CenterY = .5, .HalfWidth = .5, .HalfHeight = .5}
			);
			if (remap && !context.IsLinked("uv_map") && context.Integer("uv_map_unit", 1) == 1) {
				const Vector2 dimension = VerletScopeDimension(context);
				uv.CenterX *= dimension.X;
				uv.HalfWidth *= dimension.X;
				uv.CenterY *= dimension.Y;
				uv.HalfHeight *= dimension.Y;
			}
			if (context.FailureCode != Status::Ok) return false;
			if (remap && (uv.HalfWidth == 0 || uv.HalfHeight == 0))
				return context.Fail(Status::InvalidValue, "source UV map cannot have zero extent", "uv_map");
			const uint64_t names = std::max(context.Authored.Id.size(), std::string{}.capacity());
			if (!context.ReserveOutput(Mesh2DStorageBytes<false>(*mesh) + names, "mesh")) return false;
			MeshValue2D output = *mesh;
			auto &data = *output.Data;
			data.OriginNodeId = context.Authored.Id;
			data.OriginProcessorRow = context.ProcessorRow;
			data.Verlet = true;
			data.Warp = false;
			data.VerletQuads = !data.Quads.empty();
			for (auto &point : data.Simulation.Points) {
				const Vector2 position = point.Position, originalUV = point.UV;
				point = {};
				point.Position = point.Previous = point.Original = position;
				point.Drag = drag;
				point.UV = originalUV;
				if (remap)
					point.UV = {
						(position.X - (uv.CenterX - uv.HalfWidth)) / (uv.HalfWidth * 2),
						(position.Y - (uv.CenterY - uv.HalfHeight)) / (uv.HalfHeight * 2)
					};
			}
			for (auto &edge : data.Simulation.Edges) {
				const auto first = edge.First, second = edge.Second;
				edge = {};
				edge.First = first;
				edge.Second = second;
				const auto &a = data.Simulation.Points[first].Position,
						   &b = data.Simulation.Points[second].Position;
				edge.Distance = std::hypot(b.X - a.X, b.Y - a.Y);
				edge.DirectionDegrees = std::atan2(a.Y - b.Y, b.X - a.X) * 180 / std::numbers::pi;
				if (edge.DirectionDegrees < 0) edge.DirectionDegrees += 360;
				edge.Flexibility = flexibility;
				edge.AngularDrag = angularDrag;
			}
			return PublishVerletMesh(context, std::move(output));
		}
		bool Inline(NodeContext &) {
			return true;
		}
		bool Step(NodeContext &context, bool sourceInline, bool render = false) {
			if (sourceInline &&
				(context.InlineOwnerId.empty() || context.InlineOwnerType != "pc.verlet_sim_inline"))
				return true;
			const Value *input = context.Find("mesh");
			const auto *mesh = input ? std::get_if<MeshValue2D>(input) : nullptr;
			if (!mesh || !mesh->Data) return PublishVerletMesh(context, {});
			if (!ValidMesh2DPayload(*mesh))
				return context.Fail(Status::InvalidValue, "verlet input mesh is invalid", "mesh");
			if (!mesh->Data->Verlet)
				return context.Fail(
					Status::UnsupportedExecution, "source plain Mesh conversion points are unverified", "mesh"
				);
			const SimulationReplayEntry *previous = nullptr;
			if (context.Request.SimulationReplay)
				for (const auto &entry : context.Request.SimulationReplay->Entries)
					if (entry.NodeId == mesh->Data->OriginNodeId &&
						entry.ProcessorRow == mesh->Data->OriginProcessorRow) {
						if (previous)
							return context.Fail(Status::DuplicateId, "simulation replay repeats mesh origin");
						previous = &entry;
					}
			if (previous && !context.Request.SimulationCacheCaptures.empty() &&
				previous->State.Tick == context.Request.Tick &&
				previous->State.AuthoringRevision == context.Request.SimulationAuthoringRevision) {
				if (!context.ReserveOutput(Mesh2DStorageBytes<false>(*mesh), "mesh")) return false;
				return PublishVerletMesh(context, *mesh);
			}
			if (previous) {
				if (previous->State.AuthoringRevision != context.Request.SimulationAuthoringRevision ||
					previous->State.Tick >= Limits::MaximumTick ||
					previous->State.Tick + 1 != context.Request.Tick)
					return context.Fail(
						Status::InvalidValue, "verlet requires contiguous tick and matching revision", "mesh"
					);
			} else if (context.Request.Tick != 0) {
				return context.Fail(
					Status::InvalidValue, "verlet seek requires reset and contiguous replay", "mesh"
				);
			}
			const int64_t substeps =
				sourceInline ? VerletScopeInteger(context, "substep", 8) : context.Integer("substep", 8);
			if (substeps < 0 || uint64_t(substeps) > Limits::MaximumRangeFrames)
				return context.Fail(Status::LimitExceeded, "verlet substep exceeds work budget", "substep");
			VerletStepSettings settings;
			settings.Simple = !sourceInline;
			settings.Substeps = uint32_t(substeps);
			settings.Gravity = sourceInline ? VerletScopeVector(context, "gravity", {0, .5})
											: context.Vec2("gravity", {0, .5});
			settings.Dimension = sourceInline ? VerletScopeDimension(context) : Vector2{1, 1};
			const int64_t wall = sourceInline ? VerletScopeInteger(context, "wall", 0) : 0;
			if (wall < 0 || wall > 15)
				return context.Fail(Status::InvalidValue, "inline wall mask is outside its range", "wall");
			settings.Wall = uint32_t(wall);
			int64_t repeat = sourceInline && !render ? context.Integer("step", 1) : 1;
			if (repeat < 0 || uint64_t(repeat) > Limits::MaximumRangeFrames)
				return context.Fail(Status::LimitExceeded, "inline step count exceeds work budget", "step");
			if (sourceInline && !render && context.Boolean("pre_render") && context.Request.Tick != 0)
				repeat = 0;
			std::vector<VerletCollider> colliders;
			AllocationReservation colliderCharge;
			if (sourceInline && !ResolveVerletColliderControls(context, colliders, colliderCharge))
				return false;
			const uint64_t units = mesh->Data->Simulation.Points.size() +
								   mesh->Data->Simulation.Edges.size() + colliders.size() +
								   (!colliders.empty() ? mesh->Data->Simulation.Points.size() * 2 : 0);
			if (units && uint64_t(repeat) * uint64_t(substeps) > settings.MaximumWork / units)
				return context.Fail(Status::LimitExceeded, "inline step repeats exceed work budget", "step");
			settings.MaximumBytes = context.AvailableBytes();
			if (context.FailureCode != Status::Ok) return false;
			const uint64_t stateBytes = sizeof(SimulationReplayEntry) + context.Authored.Id.size() +
										mesh->Data->Simulation.Points.size() * sizeof(VerletPoint) +
										mesh->Data->Simulation.Edges.size() * sizeof(VerletEdge);
			if (!context.ReserveOutput(Mesh2DStorageBytes<false>(*mesh) + stateBytes * 2, "mesh"))
				return false;
			VerletReplayState state;
			Diagnostic diagnostic;
			VerletReplayState prior;
			const auto reset = ResetVerletReplay(
				mesh->Data->Simulation.Points,
				mesh->Data->Simulation.Edges,
				context.Request.Tick == 0 ? 0 : context.Request.Tick - 1,
				context.Request.SimulationAuthoringRevision,
				settings.MaximumBytes,
				prior,
				diagnostic
			);
			if (reset != Status::Ok) return context.Fail(reset, diagnostic.Message, "mesh");
			for (int64_t iteration = 0; iteration < repeat; ++iteration) {
				const auto stepped = StepVerletReplay(
					prior,
					context.Request.Tick == 0 ? 1 : context.Request.Tick,
					context.Request.SimulationAuthoringRevision,
					settings,
					state,
					diagnostic,
					colliders
				);
				if (stepped != Status::Ok) return context.Fail(stepped, diagnostic.Message, "mesh");
				prior.Mesh = std::move(state.Mesh);
			}
			state.Mesh = std::move(prior.Mesh);
			state.Tick = context.Request.Tick;
			MeshValue2D output = *mesh;
			output.Data->Simulation = std::move(state.Mesh);
			return PublishVerletMesh(context, std::move(output));
		}
		bool Simple(NodeContext &context) {
			return Step(context, false);
		}
		bool InlineStep(NodeContext &context) {
			return Step(context, true);
		}
	}
	bool VerletRenderStepMesh(NodeContext &context) {
		return Step(context, true, true);
	}
	std::span<const ExecutorEntry> SimulationExecutors() {
		static constexpr std::array entries{
			ExecutorEntry{"pc.flip_domain", FlipDomain},
			ExecutorEntry{"pc.flip_update", FlipUpdate},
			ExecutorEntry{"pc.flip_fill", FlipFill},
			ExecutorEntry{"pc.flip_apply_velocity", FlipParticleForce},
			ExecutorEntry{"pc.flip_repel", FlipParticleForce},
			ExecutorEntry{"pc.flip_vortex", FlipParticleForce},
			ExecutorEntry{"pc.flip_to_vfx", FlipToVfx},
			ExecutorEntry{"pc.flip_render", FlipRender},
			ExecutorEntry{"pc.flip_destroy", FlipDestroy},
			ExecutorEntry{"pc.flip_spawner", FlipSpawner},
			ExecutorEntry{"pc.verlet_sim_mesh_grid", Grid},
			ExecutorEntry{"pc.verlet_sim_mesh_disk", VerletDiskMesh},
			ExecutorEntry{"pc.verlet_sim_mesh_pleat", VerletPleatMesh},
			ExecutorEntry{"pc.verlet_sim_mesh_cache_lerp", VerletCacheMix},
			ExecutorEntry{"pc.verlet_sim_mesh_cache", VerletCaptureCache},
			ExecutorEntry{"pc.verlet_sim_path", VerletMeshFromPath},
			ExecutorEntry{"pc.verlet_sim_mesh_bridge", VerletBridgeMesh},
			ExecutorEntry{"pc.verlet_sim_render", VerletRenderMesh},
			ExecutorEntry{"pc.verlet_sim_to_path", VerletMeshToPath},
			ExecutorEntry{"pc.verlet_sim_mesh", ConvertMesh},
			ExecutorEntry{"pc.verlet_sim_mesh_pin", VerletPinMesh},
			ExecutorEntry{"pc.verlet_sim_force", VerletPushMesh},
			ExecutorEntry{"pc.verlet_sim_wind", VerletWind},
			ExecutorEntry{"pc.verlet_sim_mesh_tear", VerletTear},
			ExecutorEntry{"pc.verlet_sim_bloat", VerletBloat},
			ExecutorEntry{"pc.verlet_sim_drag", VerletDragMesh},
			ExecutorEntry{"pc.verlet_sim_collide", VerletCollide},
			ExecutorEntry{"pc.verlet_sim_inline", Inline},
			ExecutorEntry{"pc.verlet_sim_step", InlineStep},
			ExecutorEntry{"image.verlet_simple", Simple}
		};
		return entries;
	}
}
