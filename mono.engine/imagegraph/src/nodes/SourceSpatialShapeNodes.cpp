#include "Families.hpp"
#include "Path3D.hpp"

#include <engine/imagegraph/DataReplay.hpp>
#include <engine/imagegraph/FrameTime.hpp>

#include <numbers>
namespace engine::imagegraph::detail {
	namespace {
		template <class V> bool SpatialShapeNumber(const V &v, double &out) {
			return std::visit(
				[&](const auto &leaf) {
					using T = std::decay_t<decltype(leaf)>;
					if constexpr (std::is_same_v<T, double> || std::is_same_v<T, int64_t> ||
								  std::is_same_v<T, bool>)
						out = double(leaf);
					else if constexpr (std::is_same_v<T, EnumValue>)
						out = double(leaf.Value);
					else
						return false;
					return std::isfinite(out);
				},
				v
			);
		}
		bool SpatialShapeVector(NodeContext &c, std::string_view port, Vector3 &v) {
			const Value *value = c.Find(port);
			if (!value) return true;
			if (std::holds_alternative<Path2D>(*value) || std::holds_alternative<PathValue3D>(*value))
				return c.Fail(
					Status::UnsupportedExecution,
					"Shape Path 3D coordinate arithmetic cannot use the source path object",
					port
				);
			if (const auto *p = std::get_if<Vector3>(value))
				v = *p;
			else if (const auto *p = std::get_if<Vector2>(value))
				v = {p->X, p->Y, 0};
			else if (const auto *p = std::get_if<Vector4>(value))
				v = {p->X, p->Y, p->Z};
			else if (const auto *p = std::get_if<Quaternion>(value))
				v = {p->X, p->Y, p->Z};
			else if (const auto *a = std::get_if<ArrayValue>(value)) {
				if (!ValidRuntimeValue(*value) || !a->Nested.empty())
					return c.Fail(
						Status::UnsupportedExecution,
						"Shape Path 3D receives whole coordinate tuples, not processor rows",
						port
					);
				v = {};
				double *axes[] = {&v.X, &v.Y, &v.Z};
				for (size_t i = 0;
					 i < std::min<size_t>(3, a->Items.empty() ? a->Elements.size() : a->Items.size());
					 ++i) {
					const auto *leaf =
						a->Items.empty() ? &a->Elements[i] : std::get_if<ElementValue>(&a->Items[i].Data);
					if (!leaf || !SpatialShapeNumber(*leaf, *axes[i]))
						return c.Fail(
							Status::TypeMismatch, "Shape Path 3D coordinates must be numeric", port
						);
				}
			} else {
				double n = 0;
				if (!SpatialShapeNumber(*value, n))
					return c.Fail(Status::TypeMismatch, "Shape Path 3D coordinates must be numeric", port);
				v = {n, n, n};
			}
			return MeshFinite(v) ||
				   c.Fail(Status::InvalidValue, "Shape Path 3D coordinates must be finite", port);
		}
		bool SpatialShapeScalar(NodeContext &c, std::string_view port, double fallback, double &out) {
			out = fallback;
			const Value *value = c.Find(port);
			if (!value) return true;
			return SpatialShapeNumber(*value, out) ||
				   c.Fail(
					   Status::UnsupportedExecution,
					   "Shape Path 3D requires a finite whole scalar control",
					   port
				   );
		}
		// Node_Path_Shape_3D is a plain Node: source getInputs does not schedule array rows.
		bool SpatialShape(NodeContext &c) {
			ENGINE_PROFILE("imagegraph.path3d.shape");
			Vector3 pos{}, half{.5, .5, .5};
			double rotation = 0;
			if (!SpatialShapeVector(c, "position", pos) || !SpatialShapeVector(c, "half_size", half) ||
				!SpatialShapeScalar(c, "rotation", 0, rotation))
				return false;
			const double selected = c.SourceChoice("shape"), axis = c.SourceChoice("up_axis", 2);
			if (c.FailureCode != Status::Ok) return false;
			if (!std::isfinite(selected) || std::trunc(selected) != selected || selected < 0 ||
				selected > 8 || selected == 3 || selected == 5)
				return c.Fail(
					Status::UnsupportedExecution,
					"Shape Path 3D separator selection has no source shape object",
					"shape"
				);
			if (axis != 0 && axis != 1 && axis != 2)
				return c.Fail(Status::InvalidValue, "Shape Path 3D Up Axis is invalid", "up_axis");
			const int shape = int(selected);
			const bool loop = shape <= 4;
			double sides = 6, resolution = 64, revolution = 4, pitch = .2, inner = .5;
			if ((shape == 2 || shape == 4) && !SpatialShapeScalar(c, "sides", 6, sides)) return false;
			if ((shape == 1 || shape >= 6) && !SpatialShapeScalar(c, "resolution", 64, resolution))
				return false;
			if (shape >= 6 && !SpatialShapeScalar(c, "revolution", 4, revolution)) return false;
			if ((shape == 6 || shape == 8) && !SpatialShapeScalar(c, "pitch", .2, pitch)) return false;
			if (shape == 4 && !SpatialShapeScalar(c, "inner_radius", .5, inner)) return false;
			const bool reverse = shape == 8 && c.Boolean("reverse");
			if (c.FailureCode != Status::Ok) return false;
			sides = std::max(3., sides);
			const double steps = shape == 0	  ? 4
								 : shape == 1 ? resolution
								 : shape == 2 ? sides
								 : shape == 4 ? sides * 2
											  : resolution * revolution;
			if (!std::isfinite(steps) || steps < 0 ||
				steps > double(Limits::MaximumPathAnchors - (loop ? 1 : 0)))
				return c.Fail(
					Status::LimitExceeded,
					"Shape Path 3D generated point count exceeds native bounds",
					"path_data"
				);
			if (std::trunc(sides) != sides && (shape == 2 || shape == 4))
				return c.Fail(
					Status::InvalidValue, "Shape Path 3D Sides must pass the integer getter", "sides"
				);
			if (std::trunc(resolution) != resolution && (shape == 1 || shape >= 6))
				return c.Fail(
					Status::InvalidValue,
					"Shape Path 3D Resolution must pass the integer getter",
					"resolution"
				);
			const size_t count = size_t(std::ceil(steps)), total = count + (loop && count ? 1 : 0);
			if ((shape == 1 || shape >= 6) && resolution <= 0)
				return c.Fail(
					Status::UnsupportedExecution,
					"Shape Path 3D resolution arithmetic is undefined",
					"resolution"
				);
			if (reverse && revolution == 0)
				return c.Fail(
					Status::UnsupportedExecution,
					"Shape Path 3D reverse divides by zero revolution",
					"reverse"
				);
			if (reverse) {
				pitch = 1 / std::abs(revolution);
				rotation -= 360 / resolution * steps;
			}
			if (!std::isfinite(pitch) || !std::isfinite(rotation))
				return c.Fail(
					Status::UnsupportedExecution, "Shape Path 3D reverse arithmetic is undefined", "reverse"
				);
			const double radians = rotation * std::numbers::pi / 180, cosine = std::cos(radians),
						 sine = std::sin(radians);
			if (!std::isfinite(radians))
				return c.Fail(
					Status::UnsupportedExecution, "Shape Path 3D angular arithmetic is undefined", "rotation"
				);
			if (!c.ReserveOutput(
					2 * (sizeof(PathData3D) + total * sizeof(PathAnchor3D)) + std::string{}.capacity() +
						sizeof(DataReplayEntry) + sizeof(DataReplayValueFrame) +
						std::max(c.Authored.Id.size(), std::string{}.capacity()),
					"path_data"
				))
				return false;
			PathValue3D value;
			auto &data = value.Data.emplace();
			data.SourcePolyline = true;
			data.Loop = loop;
			data.Resolution = 1;
			data.Anchors.reserve(total);
			if (data.Anchors.capacity() > total &&
				!c.ReserveOutput(2 * (data.Anchors.capacity() - total) * sizeof(PathAnchor3D), "path_data"))
				return false;

			if (!count) {
				const auto *owner = c.CurrentData ? c.CurrentData : c.Request.DataReplay;
				if (owner) {
					if (owner->Entries.size() > Limits::MaximumArrayElements)
						return c.Fail(
							Status::LimitExceeded, "Shape Path 3D replay rows exceed bounds", "path_data"
						);
					auto validation = c.ReserveWorkspace(owner->Entries.size() * sizeof(size_t), "path_data");
					if (!validation) return false;
					Diagnostic diagnostic;
					if (ValidateDataReplay(*owner, c.ByteBudget, diagnostic) != Status::Ok)
						return c.Fail(diagnostic, "path_data");
					for (const auto &entry : owner->Entries) {
						if (entry.NodeId != c.Authored.Id || entry.ProcessorRow != c.ProcessorRow ||
							entry.Values.empty())
							continue;
						const auto *prior = std::get_if<PathValue3D>(&entry.Values.back().Data);
						if (!prior || !prior->Data || !prior->Data->SourcePolyline)
							return c.Fail(
								Status::InvalidValue,
								"Shape Path 3D replay does not contain its source path class",
								"path_data"
							);
						if (prior->Data->SourceEmptyCache)
							data.SourceEmptyCache = prior->Data->SourceEmptyCache;
						else if (!prior->Data->Anchors.empty()) {
							PathRuntime3D previous(*prior->Data);
							data.SourceEmptyCache = SourcePolylineEmptyCache3D{
								previous.Length(), uint32_t(prior->Data->Anchors.size())
							};
						}
						break;
					}
				}
			}

			for (size_t i = 0; i < count; ++i) {
				Vector3 p = pos;
				if (shape == 0) {
					p.X += (i == 0 || i == 3) ? -half.X : half.X;
					p.Y += i < 2 ? -half.Y : half.Y;
				} else {
					const double divisor = shape == 2 || shape == 4 ? sides : resolution;
					const double turn = shape == 4 ? double(i) / 2 : double(i);
					const double angle = 2 * std::numbers::pi * turn / divisor;
					double radius = shape == 4 && i % 2 ? inner : 1;
					if (shape == 7) {
						const double t = double(i) / steps, r = t * 2 - 1;
						radius = std::sqrt(1 - r * r);
						p.Z = (pos.Z - half.Z) + (2 * half.Z) * t;
					} else if (shape == 8)
						radius = double(i) / resolution * pitch;
					else if (shape == 6)
						p.Z = pos.Z + double(i) / resolution * pitch;
					p.X = pos.X + half.X * radius * std::cos(angle);
					p.Y = pos.Y - half.Y * radius * std::sin(angle);
				}
				const double x = p.X - pos.X, y = p.Y - pos.Y;
				// Source point_rotate(-rotation) has exact shortcuts for zero and 180 degrees.
				if (rotation == -180) {
					p.X = pos.X + (pos.X - p.X);
					p.Y = pos.Y + (pos.Y - p.Y);
				} else if (rotation != 0) {
					p.X = pos.X + x * cosine - y * sine;
					p.Y = pos.Y + x * sine + y * cosine;
				}
				if (axis == 0)
					std::swap(p.X, p.Z);
				else if (axis == 1)
					std::swap(p.Y, p.Z);
				if (!MeshFinite(p))
					return c.Fail(
						Status::UnsupportedExecution,
						"Shape Path 3D geometry arithmetic is undefined",
						"path_data"
					);
				PathAnchor3D anchor;
				anchor.Controls[0] = p.X;
				anchor.Controls[1] = p.Y;
				anchor.Controls[2] = p.Z;
				data.Anchors.push_back(anchor);
			}
			if (loop && count) data.Anchors.push_back(data.Anchors.front());
			PathRuntime3D runtime(data);
			if (!runtime.Valid())
				return c.Fail(
					Status::UnsupportedExecution, "Shape Path 3D Euclidean length overflowed", "path_data"
				);
			DataReplayEntry state;
			state.NodeId = c.Authored.Id;
			if (state.NodeId.capacity() > std::max(c.Authored.Id.size(), std::string{}.capacity()) &&
				!c.ReserveOutput(
					state.NodeId.capacity() - std::max(c.Authored.Id.size(), std::string{}.capacity()),
					"path_data"
				))
				return false;
			state.ProcessorRow = c.ProcessorRow;
			state.Tick = c.Request.Tick;
			state.Subframe = c.Request.Subframe;
			state.NegativeFrame = c.Request.NegativeFrame;
			state.PreviousFrame = double(FrameTimeToReal({state.Tick, state.Subframe, state.NegativeFrame}));
			state.Initialized = true;
			state.Values.reserve(1);
			if (state.Values.capacity() > 1 &&
				!c.ReserveOutput((state.Values.capacity() - 1) * sizeof(DataReplayValueFrame), "path_data"))
				return false;
			state.Values.push_back({state.Tick, Value{value}});
			c.DataUpdates.reserve(1);
			if (c.DataUpdates.capacity() > 1 &&
				!c.ReserveOutput((c.DataUpdates.capacity() - 1) * sizeof(DataReplayEntry), "path_data"))
				return false;
			c.SetValue("path_data", std::move(value));
			if (c.FailureCode != Status::Ok) return false;
			c.DataUpdates.push_back(std::move(state));
			return true;
		}
	}
	std::span<const ExecutorEntry> SourceSpatialShapeExecutors() {
		static constexpr ExecutorEntry entries[] = {{"pc.path_shape_3_d", SpatialShape}};
		return entries;
	}
}
