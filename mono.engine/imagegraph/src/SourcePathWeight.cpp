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
		uint64_t total = 0;
		const auto add = [&](uint64_t n) {
			if (n > (uint64_t{1} << 24) - total) return false;
			total += n;
			return true;
		};
		const auto visit = [&](auto &&self, const Path2D *p, const PathData3D *s, size_t depth) -> bool {
			if (depth > Limits::MaximumArrayDepth || !add(1)) return false;
			if (p) {
				if (!p->SourceOperation) return add(p->Anchors.size() * 33 + p->Weights.size() + 101);
				const auto &op = *p->SourceOperation;
				if (op.Shape && !add(op.Shape->Points.size() + size_t(op.Shape->Loop))) return false;
				for (const auto &child : op.Inputs)
					if (!self(self, &child, nullptr, depth + 1)) return false;
				if (op.WeightInput3D && !self(self, nullptr, op.WeightInput3D.operator->(), depth + 1))
					return false;
			} else if (s && s->SourcePresent) {
				if (s->Source2D && !self(self, &*s->Source2D, nullptr, depth + 1)) return false;
				if (s->SourceOperation) {
					for (const auto &child : s->SourceOperation->Inputs) {
						if (!child.Data || !self(self, nullptr, child.Data.operator->(), depth + 1))
							return false;
					}
				} else if (!s->Source2D &&
						   !add(
							   s->SourcePolyline ? s->Anchors.size()
												 : s->Anchors.size() * (uint64_t(s->Resolution) + 1)
						   ))
					return false;
			}
			return true;
		};
		return visit(visit, planar, spatial, 0) ? std::optional<uint64_t>{total} : std::nullopt;
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
