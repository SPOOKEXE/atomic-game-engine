#include "SourcePathWeight.hpp"

#include "SourcePathPayload3D.hpp"
#include "nodes/Path3D.hpp"
namespace engine::imagegraph::detail {
	bool ValidSourceWeightInput3D(const PathData3D &data, size_t depth, size_t *count) {
		return ValidSourcePath3D(data, depth, count);
	}
	uint64_t SourceWeightInput3DBytes(const PathData3D &data, bool retained, size_t depth) {
		return retained ? SourcePath3DBytes<true>(data, depth) : SourcePath3DBytes<false>(data, depth);
	}
	std::optional<uint64_t> SourceWeightRuntimeWork(const Path2D *planar, const PathData3D *spatial) {
		constexpr uint64_t limit = uint64_t{1} << 24;
		const auto visit =
			[&](auto &&self, const Path2D *p, const PathData3D *s, size_t depth) -> std::optional<uint64_t> {
			if (depth > Limits::MaximumArrayDepth) return {};
			uint64_t total = 1;
			auto add = [&](uint64_t n) {
				if (n > limit - total) return false;
				total += n;
				return true;
			};
			auto child = [&](const Path2D *p, const PathData3D *s, uint64_t repeats) {
				auto n = self(self, p, s, depth + 1);
				return n && *n <= (limit - total) / repeats && add(*n * repeats);
			};
			if (p) {
				if (p->Anchors.size() > Limits::MaximumPathAnchors ||
					p->Weights.size() > Limits::MaximumPathWeights)
					return {};
				if (!p->SourceOperation) {
					if (!add(p->Anchors.size() * 128 + p->Weights.size() + 101)) return {};
					return total;
				}
				const auto &op = *p->SourceOperation;
				if (op.Inputs.size() > Limits::MaximumArrayElements ||
					op.CachedLengths.size() > Limits::MaximumArrayElements ||
					op.WeightCurve.size() > Limits::MaximumArrayElements ||
					op.BlendAccumulated.size() > Limits::MaximumArrayElements)
					return {};
				if (!add(
						op.CachedLengths.size() + op.WeightCurve.size() + op.Reversed.size() +
						op.BlendLengths.size() + op.BlendAccumulated.size()
					))
					return {};
				for (const auto &row : op.BlendAccumulated)
					if (!add(row.size())) return {};
				if (op.Shape && !add(op.Shape->Points.size() * 4 + size_t(op.Shape->Loop))) return {};
				if (op.Mesh && !add(op.Mesh->Simulation.Edges.size() * 16)) return {};
				uint64_t repeats = 1;
				if (op.Spiral) {
					const auto &q = *op.Spiral;
					if (q.Cache.size() > Limits::MaximumArrayElements ||
						!add(
							q.AmplitudeCurve.size() + q.DirectionCurve.size() +
							q.Cache.size() * q.Cache.size() + 4096 + 256
						))
						return {};
					repeats = q.Direction == 0 ? 6 : 4;
				}
				if (op.Baked) {
					if (!add(4096 + op.Baked->Lines.size())) return {};
					for (const auto &line : op.Baked->Lines)
						if (!add(line.size() * 8)) return {};
				}
				if (op.Sequential) {
					const auto &q = *op.Sequential;
					if (q.SmoothSteps < 0 || q.SmoothSteps > int64_t(Limits::MaximumArrayElements) ||
						!add(
							q.Accumulated.size() + q.FlattenLengths.size() + q.FlattenOwners.size() +
							q.Cache.size() * 4096 + 4096
						))
						return {};
					if (op.Kind == SourcePathOperationKind::Smoothen)
						repeats = 1 + 2 * uint64_t(q.SmoothSteps);
				}
				if (op.Kind == SourcePathOperationKind::Shift) repeats = 3;
				if (op.Kind == SourcePathOperationKind::WeightAdjust && op.WeightType == 2) repeats = 3;
				for (const auto &v : op.Inputs)
					if (!child(&v, nullptr, repeats)) return {};
				if (op.WeightInput3D && !child(nullptr, &*op.WeightInput3D, repeats)) return {};
			} else if (s && s->SourcePresent) {
				if (s->Anchors.size() > Limits::MaximumPathAnchors ||
					s->Resolution > Limits::MaximumArrayElements ||
					s->Transforms.size() > Limits::MaximumArrayDepth)
					return {};
				if (!add(s->Transforms.size() * 64)) return {};
				if (s->Source2D && !child(&*s->Source2D, nullptr, 1)) return {};
				if (s->SourceOperation) {
					if (s->SourceOperation->Inputs.size() > Limits::MaximumArrayElements) return {};
					for (const auto &v : s->SourceOperation->Inputs)
						if (!v.Data || !child(nullptr, &*v.Data, 1)) return {};
				} else if (!s->Source2D &&
						   !add(
							   s->SourcePolyline ? s->Anchors.size() * 16
												 : s->Anchors.size() * (uint64_t(s->Resolution) + 1) * 64
						   ))
					return {};
			}
			return total;
		};
		return visit(visit, planar, spatial, 0);
	}

	struct SourcePathWeightRuntime3D::Storage {
		NodeContext &Context;
		PathRuntime3D Runtime;
		Storage(NodeContext &context, const PathData3D &data) : Context(context), Runtime(data, &context) {}
	};
	SourcePathWeightRuntime3D::SourcePathWeightRuntime3D(NodeContext &context, const PathData3D &data) {
		auto charge = context.ReserveWorkspace(sizeof(Storage), "path");
		if (!charge) return;
		Charge = std::move(*charge);
		Data = std::make_unique<Storage>(context, data);
		if (!Data->Runtime.Valid())
			context.Fail(Status::InvalidValue, "Weight Adjust spatial path runtime is invalid", "path");
	}
	SourcePathWeightRuntime3D::~SourcePathWeightRuntime3D() = default;
	SourcePathWeightRuntime3D::SourcePathWeightRuntime3D(SourcePathWeightRuntime3D &&) noexcept = default;
	SourcePathWeightRuntime3D &
	SourcePathWeightRuntime3D::operator=(SourcePathWeightRuntime3D &&other) noexcept {
		if (this == &other) return *this;
		Data.reset();
		Charge = std::move(other.Charge);
		Data = std::move(other.Data);
		return *this;
	}
	bool SourcePathWeightRuntime3D::Valid() const {
		return Data && Data->Runtime.Valid();
	}
	std::array<double, 3> SourcePathWeightRuntime3D::Ratio(double ratio, size_t line) const {
		const auto point = Data->Runtime.Ratio(ratio, line);
		return {point.Position.X, point.Position.Y, point.Weight};
	}
	SourcePathPointBuffer
	SourcePathWeightRuntime3D::RatioInto(double ratio, size_t line, SourcePathPointBuffer &out) const {
		return Data->Runtime.RatioInto(ratio, line, out);
	}
	SourcePathPointBuffer
	SourcePathWeightRuntime3D::DistanceInto(double distance, size_t line, SourcePathPointBuffer &out) const {
		return Data->Runtime.DistanceInto(distance, line, out);
	}
	size_t SourcePathWeightRuntime3D::OriginalChildCount() const {
		return Data->Runtime.OriginalChildCount();
	}
	size_t SourcePathWeightRuntime3D::OriginalChildLineCount(size_t index) const {
		const auto *child = Data->Runtime.OriginalChild(index);
		return child ? child->LineCount() : 0;
	}
	double SourcePathWeightRuntime3D::OriginalChildLength(size_t index) const {
		const auto *child = Data->Runtime.OriginalChild(index);
		return child ? child->Length() : 0;
	}
	size_t SourcePathWeightRuntime3D::OriginalChildSegmentCount(size_t index) const {
		const auto *child = Data->Runtime.OriginalChild(index);
		return child ? child->SourceSegmentCount() : 0;
	}
	SourcePathPointBuffer SourcePathWeightRuntime3D::OriginalChildDistanceInto(
		size_t index, double distance, size_t line, SourcePathPointBuffer &out
	) const {
		const auto *child = Data->Runtime.OriginalChild(index);
		return child ? child->DistanceInto(distance, line, out) : out;
	}
	size_t SourcePathWeightRuntime3D::LineCount() const {
		return Data->Runtime.LineCount();
	}
	double SourcePathWeightRuntime3D::Length(size_t line) const {
		return Data->Runtime.Length(line);
	}
	size_t SourcePathWeightRuntime3D::SegmentCount(size_t line) const {
		return Data->Runtime.SourceSegmentCount(line);
	}
	size_t SourcePathWeightRuntime3D::AccumulatedCount(size_t line) const {
		return Data->Runtime.SourceAccumulatedCount(line);
	}
	double SourcePathWeightRuntime3D::AccumulatedAt(size_t index, size_t line) const {
		const double value = Data->Runtime.SourceAccumulatedAt(index, line);
		if (!std::isfinite(value))
			Data->Context.Fail(
				Status::UnsupportedExecution,
				"Source spatial shape cached accumulated lengths are "
				"absent from this saved path",
				"path"
			);
		return value;
	}
	std::optional<Vector4> SourcePathWeightRuntime3D::Boundary(size_t line) const {
		return Data->Runtime.SourceBoundary(line);
	}
} // namespace engine::imagegraph::detail
