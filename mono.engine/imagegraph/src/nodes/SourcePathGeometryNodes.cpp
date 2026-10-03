#include "Families.hpp"
#include "Path.hpp"

#include <engine/imagegraph/DataReplay.hpp>
#include <engine/imagegraph/FrameTime.hpp>

#include <algorithm>
namespace engine::imagegraph::detail {
	namespace {
		constexpr uint64_t GEOMETRY_WORK_LIMIT = 1u << 24;
		bool GeometryWork(const Path2D &path, uint64_t &work) {
			const uint64_t add = path.Anchors.size() * 33 + path.Weights.size() + 1;
			if (add > GEOMETRY_WORK_LIMIT - work) return false;
			work += add;
			if (path.SourceOperation) {
				const auto &op = *path.SourceOperation;
				const uint64_t extra = (op.Shape ? op.Shape->Points.size() : 0) +
									   (op.Mesh ? op.Mesh->Simulation.Edges.size() : 0);
				if (extra > GEOMETRY_WORK_LIMIT - work) return false;
				work += extra;
				for (const auto &child : op.Inputs)
					if (!GeometryWork(child, work)) return false;
			}
			return true;
		}
		bool GeometryPublish(NodeContext &c, std::string_view port, Value value) {
			const auto bytes = ValueClonePayloadBytes(value);
			if (!bytes || *bytes > Limits::MaximumEvaluationBytes / 2)
				return c.Fail(Status::LimitExceeded, "Path modifier payload exceeds bounds", port);
			if (!c.ReserveOutput(
					*bytes * 2 + sizeof(DataReplayEntry) + sizeof(DataReplayValueFrame) +
						std::max(c.Authored.Id.size(), std::string{}.capacity()),
					port
				))
				return false;
			DataReplayEntry state;
			state.NodeId = c.Authored.Id;
			state.ProcessorRow = c.ProcessorRow;
			state.Tick = c.Request.Tick;
			state.Subframe = c.Request.Subframe;
			state.NegativeFrame = c.Request.NegativeFrame;
			state.Initialized = true;
			state.PreviousFrame = double(FrameTimeToReal({state.Tick, state.Subframe, state.NegativeFrame}));
			state.Values.push_back({state.Tick, value});
			c.SetValue(port, std::move(value));
			if (c.FailureCode != Status::Ok) return false;
			c.DataUpdates.push_back(std::move(state));
			return true;
		}
		bool GeometryRetain(NodeContext &c) {
			const auto *owner = c.CurrentData ? c.CurrentData : c.Request.DataReplay;
			if (owner) {
				Diagnostic d;
				if (ValidateDataReplay(*owner, c.ByteBudget, d) != Status::Ok) return c.Fail(d);
				for (const auto &entry : owner->Entries)
					if (entry.NodeId == c.Authored.Id && entry.ProcessorRow == c.ProcessorRow &&
						!entry.Values.empty()) {
						const auto bytes = ValueClonePayloadBytes(entry.Values.back().Data);
						if (!bytes)
							return c.Fail(
								Status::LimitExceeded, "Retained area mapping exceeds bounds", "path"
							);
						if (!c.ReserveOutput(*bytes, "path")) return false;
						c.SetValue("path", entry.Values.back().Data);
						return c.FailureCode == Status::Ok;
					}
			}
			if (!c.ReserveOutput(sizeof(SourcePathData2D), "path")) return false;
			Path2D initial;
			initial.SourceOperation.emplace().Kind = SourcePathOperationKind::AreaMap;
			return GeometryPublish(c, "path", std::move(initial));
		}
		bool GeometryInput(NodeContext &c, const Path2D *&path, uint64_t samples) {
			const auto *value = c.Find("path");
			path = value ? std::get_if<Path2D>(value) : nullptr;
			const auto provenance = c.IsCatalogueDefault("path");
			if (!provenance)
				return c.Fail(
					Status::UnsupportedExecution, "Path modifier requires source default provenance", "path"
				);
			if (*provenance) {
				path = nullptr;
				return true;
			}
			if (!path)
				return c.Fail(Status::UnsupportedExecution, "Path modifier requires a planar path", "path");
			if (!ValidSourcePath2D(*path))
				return c.Fail(Status::InvalidValue, "Path modifier input tree is invalid", "path");
			uint64_t work = 0;
			if (!GeometryWork(*path, work) || work > GEOMETRY_WORK_LIMIT /
														 std::max<size_t>(1, c.ProcessorCount) /
														 std::max<uint64_t>(1, samples))
				return c.Fail(
					Status::LimitExceeded, "Path modifier processor batch exceeds bounded work", "path"
				);
			return true;
		}
		bool GeometryAreaOrigin(NodeContext &c) {
			const auto *link = c.InputProducer("area");
			constexpr std::string_view suffix = ".bypass";
			// Ordinary output junctions terminate source provenance with their own empty display data.
			if (!link || !link->FromPort.ends_with(suffix)) return true;
			if (!c.EvaluationDocument)
				return c.Fail(
					Status::UnsupportedExecution, "Linked Area bypass origin is unavailable", "area"
				);
			const auto producer = std::find_if(
				c.EvaluationDocument->Nodes.begin(),
				c.EvaluationDocument->Nodes.end(),
				[&](const Node &node) { return node.Id == link->FromNode; }
			);
			const auto *entry =
				producer == c.EvaluationDocument->Nodes.end() ? nullptr : FindCatalogueEntry(producer->Type);
			const auto port =
				std::string_view(link->FromPort).substr(0, link->FromPort.size() - suffix.size());
			const auto *input = entry ? FindCatalogueInput(*entry, port) : nullptr;
			if (!input)
				return c.Fail(
					Status::UnsupportedExecution, "Linked Area bypass getter is unavailable", "area"
				);
			if (input->SourceKind != "Area") return true;
			for (const auto &control : entry->Inputs)
				if (control.SourceKind == "ValueUnit" && control.Id.starts_with(port) &&
					control.Id.substr(port.size()) == "_unit")
					return c.Fail(
						Status::UnsupportedExecution,
						"Linked Area bypass requires the originating surface-context getter observation",
						"area"
					);
			return true;
		}
		bool GeometryVector4(NodeContext &c, std::string_view port, Vector4 &output) {
			const auto *value = c.Find(port);
			const auto *v = value ? std::get_if<Vector4>(value) : nullptr;
			if (!v)
				return c.Fail(
					Status::InvalidValue, "Path area mapping requires a four-component bound", port
				);
			output = *v;
			return true;
		}
		bool GeometryPath(NodeContext &c, bool mapping) {
			ENGINE_PROFILE("imagegraph.source.path_geometry");
			const Path2D *input = nullptr;
			if (!GeometryInput(c, input, 8)) return false;
			if (mapping && !input) return GeometryRetain(c);
			uint64_t bytes = sizeof(SourcePathData2D);
			if (input) {
				const auto child = SourcePath2DBytes<false>(*input) + sizeof(Path2D);
				if (child > Limits::MaximumEvaluationBytes - bytes)
					return c.Fail(Status::LimitExceeded, "Path geometry clone exceeds bounds", "path");
				bytes += child;
			}
			if (!c.ReserveOutput(bytes, "path")) return false;
			Path2D output;
			auto &op = output.SourceOperation.emplace();
			op.Kind = mapping ? SourcePathOperationKind::AreaMap : SourcePathOperationKind::Transform;
			if (input) op.Inputs.push_back(*input);
			if (!mapping) {
				op.TransformPosition = c.Vec2("position");
				op.TransformScale = c.Vec2("scale", {1, 1});
				op.TransformAnchor = c.Vec2("anchor");
				op.TransformRotation = c.Scalar("rotation");
			} else {
				const auto from = c.SourceChoice("map_from"), to = c.SourceChoice("map_to");
				if (from < 0 || from > 2 || std::floor(from) != from)
					return c.Fail(
						Status::InvalidValue,
						"Map From must be Path Boundary, Fix Dimension or BBOX",
						"map_from"
					);
				if (to < 0 || to > 2 || std::floor(to) != to)
					return c.Fail(
						Status::InvalidValue, "Map To must be Area, Fix Dimension or BBOX", "map_to"
					);
				if (from == 0) {
					PathRuntime path;
					if (!path.Init(c, *input)) return false;
					op.MapFrom = {path.MinX, path.MinY, path.MaxX - path.MinX, path.MaxY - path.MinY};
				} else if (from == 1) {
					const auto dimension = c.Vec2(
						"dimension_from", {double(c.Project.SurfaceWidth), double(c.Project.SurfaceHeight)}
					);
					op.MapFrom = {0, 0, dimension.X, dimension.Y};
				} else {
					Vector4 box;
					if (!GeometryVector4(c, "bbox_from", box)) return false;
					op.MapFrom = {box.X, box.Y, box.Z - box.X, box.W - box.Y};
				}
				if (to == 0) {
					if (!GeometryAreaOrigin(c)) return false;
					Area area{
						c.Project.SurfaceWidth / 2.,
						c.Project.SurfaceHeight / 2.,
						c.Project.SurfaceWidth / 2.,
						c.Project.SurfaceHeight / 2.,
						0,
						0
					};
					if (const auto *value = c.Find("area")) {
						const auto *a = std::get_if<Area>(value);
						if (!a) return c.Fail(Status::InvalidValue, "Path mapping requires an area", "area");
						area = *a;
					}
					// This source input never calls setUnitSimple. Its authored Area fields are consumed raw.
					op.MapArea = {area.CenterX, area.CenterY, area.HalfWidth, area.HalfHeight};
				} else if (to == 1) {
					const auto d = c.Vec2(
						"dimension_to", {double(c.Project.SurfaceWidth), double(c.Project.SurfaceHeight)}
					);
					op.MapArea = {d.X / 2, d.Y / 2, d.X / 2, d.Y / 2};
				} else {
					Vector4 box;
					if (!GeometryVector4(c, "bbox_to", box)) return false;
					op.MapArea = {
						(box.X + box.Z) / 2, (box.Y + box.W) / 2, (box.Z - box.X) / 2, (box.W - box.Y) / 2
					};
				}
				if (op.MapFrom.Z == 0 || op.MapFrom.W == 0)
					return c.Fail(
						Status::UnsupportedExecution,
						"Zero source mapping extent produces source nonfinite coordinates",
						"path"
					);
			}
			if (!ValidSourcePath2D(output))
				return c.Fail(
					Status::InvalidValue, "Path geometry controls are nonfinite or exceed bounds", "path"
				);
			PathRuntime check;
			if (!check.Init(c, output)) return false;
			const auto p = check.PointRatio(0);
			if (!std::isfinite(p.X) || !std::isfinite(p.Y) || !std::isfinite(p.Weight) ||
				!std::isfinite(check.MinX) || !std::isfinite(check.MinY) || !std::isfinite(check.MaxX) ||
				!std::isfinite(check.MaxY))
				return c.Fail(
					Status::InvalidValue, "Path geometry produces nonfinite samples or bounds", "path"
				);
			if (mapping) return GeometryPublish(c, "path", std::move(output));
			c.SetValue("path", std::move(output));
			return c.FailureCode == Status::Ok;
		}
		bool GeometryTransform(NodeContext &c) {
			return GeometryPath(c, false);
		}
		bool GeometryAreaMap(NodeContext &c) {
			return GeometryPath(c, true);
		}
	}
	std::span<const ExecutorEntry> SourcePathGeometryExecutors() {
		static const ExecutorEntry entries[] = {
			{"pc.path_transform", GeometryTransform, true}, {"pc.path_map_area", GeometryAreaMap, true}
		};
		return entries;
	}
}
