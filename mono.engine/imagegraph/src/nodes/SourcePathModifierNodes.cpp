#include "Curve.hpp"
#include "Families.hpp"
#include "Path.hpp"

#include <engine/imagegraph/DataReplay.hpp>
#include <engine/imagegraph/FrameTime.hpp>

#include <algorithm>
namespace engine::imagegraph::detail {
	namespace {
		constexpr uint64_t MODIFIER_WORK_LIMIT = 1u << 24;
		bool ModifierWork(const Path2D &path, uint64_t &work) {
			const uint64_t add = path.Anchors.size() * 33 + path.Weights.size() + 1;
			if (add > MODIFIER_WORK_LIMIT - work) return false;
			work += add;
			if (path.SourceOperation) {
				const auto &op = *path.SourceOperation;
				const uint64_t extra = (op.Shape ? op.Shape->Points.size() : 0) +
									   (op.Mesh ? op.Mesh->Simulation.Edges.size() : 0);
				if (extra > MODIFIER_WORK_LIMIT - work) return false;
				work += extra;
				for (const auto &child : op.Inputs)
					if (!ModifierWork(child, work)) return false;
			}
			return true;
		}
		bool ModifierPublish(NodeContext &c, std::string_view port, Value value) {
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
		bool ModifierRetain(NodeContext &c, std::string_view port) {
			const auto *owner = c.CurrentData ? c.CurrentData : c.Request.DataReplay;
			if (owner) {
				Diagnostic diagnostic;
				if (ValidateDataReplay(*owner, c.ByteBudget, diagnostic) != Status::Ok)
					return c.Fail(diagnostic);
				for (const auto &entry : owner->Entries)
					if (entry.NodeId == c.Authored.Id && entry.ProcessorRow == c.ProcessorRow &&
						!entry.Values.empty()) {
						const auto bytes = ValueClonePayloadBytes(entry.Values.back().Data);
						if (!bytes)
							return c.Fail(
								Status::LimitExceeded, "Retained modifier payload exceeds bounds", port
							);
						if (!c.ReserveOutput(*bytes, port)) return false;
						c.SetValue(port, entry.Values.back().Data);
						return c.FailureCode == Status::Ok;
					}
			}
			if (c.Authored.Type == "pc.path_redistribute")
				return c.Fail(
					Status::UnsupportedExecution,
					"Initial Redistribute constructor output needs a valid path before sampling",
					port
				);
			if (c.Authored.Type == "pc.path_to_curve") {
				if (!c.ReserveOutput(2 * sizeof(std::array<double, 6>), port)) return false;
				Curve initial;
				initial.Header = {0, 1, 0, 0, 1, 0};
				initial.Anchors = {{{0, 0, 0, 0, 1. / 3, 1. / 3}}, {{-1. / 3, -1. / 3, 1, 1, 0, 0}}};
				c.SetValue(port, std::move(initial));
			} else
				c.SetValue(port, UndefinedValue{});
			return c.FailureCode == Status::Ok;
		}
		bool ModifierInput(NodeContext &c, const Path2D *&path, uint64_t samples) {
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
			if (!ModifierWork(*path, work) || work > MODIFIER_WORK_LIMIT /
														 std::max<size_t>(1, c.ProcessorCount) /
														 std::max<uint64_t>(1, samples))
				return c.Fail(
					Status::LimitExceeded, "Path modifier processor batch exceeds bounded work", "path"
				);
			return true;
		}
		bool PathToCurve(NodeContext &c) {
			ENGINE_PROFILE("imagegraph.source.path_to_curve");
			const double resolution = c.Scalar("resolution", 16), kind = c.SourceChoice("type");
			if (!std::isfinite(resolution) || resolution < 1 || resolution >= Limits::MaximumCurveAnchors ||
				std::floor(resolution) != resolution)
				return c.Fail(
					Status::InvalidValue,
					"Path to Curve resolution must be a positive bounded integer",
					"resolution"
				);
			if (kind < 0 || kind > 1 || std::floor(kind) != kind)
				return c.Fail(Status::InvalidValue, "Path to Curve type must be Curve or Step", "type");
			const Path2D *path = nullptr;
			if (!ModifierInput(c, path, uint64_t(resolution) + 1)) return false;
			if (!path) return ModifierRetain(c, "curve");
			PathRuntime runtime;
			if (!runtime.Init(c, *path)) return false;
			const auto outputRange = c.Vec2("output_range", {0, 1}), yRange = c.Vec2("y_range", {0, 1});
			const double shift = c.Scalar("shift"), scale = c.Scalar("scale", 1),
						 width = std::max(1., std::abs(runtime.MaxX - runtime.MinX)),
						 height = std::max(1., std::abs(runtime.MaxY - runtime.MinY));
			const uint64_t bytes = (uint64_t(resolution) + 1) * sizeof(std::array<double, 6>);
			if (!c.ReserveOutput(bytes, "curve")) return false;
			Curve output;
			output.Header = {shift, scale, kind, outputRange.X, outputRange.Y, 0};
			output.Anchors.reserve(size_t(resolution) + 1);
			for (size_t i = 0; i <= size_t(resolution); ++i) {
				const auto point = runtime.PointRatio(double(i) / resolution);
				const double x = (point.X - runtime.MinX) / width,
							 y = yRange.X + (yRange.Y - yRange.X) * (point.Y - runtime.MinY) / height;
				if (!std::isfinite(x) || !std::isfinite(y))
					return c.Fail(Status::InvalidValue, "Path to Curve sampled a nonfinite point", "path");
				output.Anchors.push_back({0, 0, x, y, 0, 0});
			}
			return ModifierPublish(c, "curve", std::move(output));
		}
		bool PathModifier(NodeContext &c, bool redistribute) {
			ENGINE_PROFILE("imagegraph.source.path_modifier");
			const Path2D *path = nullptr;
			if (!ModifierInput(c, path, redistribute ? 34 : 8)) return false;
			if (!path) return ModifierRetain(c, "path");

			const Curve *curve = nullptr;
			if (redistribute) {
				const auto *value = c.Find("curve");
				curve = value ? std::get_if<Curve>(value) : nullptr;
				if (!curve)
					return c.Fail(Status::InvalidValue, "Path Redistribute requires a curve", "curve");
				if (curve->Anchors.size() > Limits::MaximumCurveAnchors)
					return c.Fail(Status::LimitExceeded, "Redistribute curve exceeds bounds", "curve");
				const uint64_t limit = MODIFIER_WORK_LIMIT / std::max<size_t>(1, c.ProcessorCount);
				const uint64_t tableWork = curve->Anchors.size() * 264;
				uint64_t pathWork = 0;
				if (tableWork > limit || !ModifierWork(*path, pathWork) ||
					pathWork > (limit - tableWork) / 34)
					return c.Fail(
						Status::LimitExceeded, "Redistribute processor batch exceeds bounded work", "curve"
					);
			}
			const uint64_t child = SourcePath2DBytes<false>(*path);
			if (child > Limits::MaximumEvaluationBytes - sizeof(SourcePathData2D) - sizeof(Path2D))
				return c.Fail(Status::LimitExceeded, "Path modifier clone exceeds bounds", "path");
			if (!c.ReserveOutput(child + sizeof(SourcePathData2D) + sizeof(Path2D), "path")) return false;
			Path2D output;
			auto &op = output.SourceOperation.emplace();
			op.Kind = redistribute ? SourcePathOperationKind::Redistribute : SourcePathOperationKind::Skew;
			op.Inputs.push_back(*path);
			if (redistribute) {
				op.RedistributeMap.emplace();
				for (size_t i = 0; i <= 32; ++i) {
					const double v = EvalCurveX(*curve, double(i) / 32, 1e-5);
					if (!std::isfinite(v))
						return c.Fail(Status::InvalidValue, "Redistribute curve table is nonfinite", "curve");
					(*op.RedistributeMap)[i] = v;
				}
			} else {
				const double axis = c.SourceChoice("axis");
				if (axis < 0 || axis > 1 || std::floor(axis) != axis)
					return c.Fail(Status::InvalidValue, "Path Skew axis must be X or Y", "axis");
				op.SkewAxis = uint8_t(axis);
				op.SkewStrength = c.Scalar("strength");
				op.SkewCenter = c.Vec2("center", {.5, .5});
			}
			if (!ValidSourcePath2D(output))
				return c.Fail(Status::InvalidValue, "Path modifier controls exceed bounds", "path");
			PathRuntime check;
			if (!check.Init(c, output)) return false;
			const auto p = check.PointDistance(0);
			if (!std::isfinite(p.X) || !std::isfinite(p.Y) || !std::isfinite(p.Weight))
				return c.Fail(Status::InvalidValue, "Path modifier initial point is nonfinite", "path");
			return ModifierPublish(c, "path", std::move(output));
		}
		bool ModifierRedistribute(NodeContext &c) {
			return PathModifier(c, true);
		}
		bool ModifierSkew(NodeContext &c) {
			return PathModifier(c, false);
		}
	}
	std::span<const ExecutorEntry> SourcePathModifierExecutors() {
		static const ExecutorEntry entries[] = {
			{"pc.path_to_curve", PathToCurve, true},
			{"pc.path_redistribute", ModifierRedistribute, true},
			{"pc.path_skew", ModifierSkew, true}
		};
		return entries;
	}
}
