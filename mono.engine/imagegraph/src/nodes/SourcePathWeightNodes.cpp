#include "../SourcePathPayload3D.hpp"
#include "../SourcePathWeight.hpp"
#include "Curve.hpp"
#include "Families.hpp"

namespace engine::imagegraph::detail {
	namespace {
		bool WeightAdjust(NodeContext &context) {
			ENGINE_PROFILE("imagegraph.source.path_weight_adjust");
			const auto provenance = context.IsCatalogueDefault("path");
			if (!provenance)
				return context.Fail(
					Status::UnsupportedExecution,
					"Source Weight Adjust requires resolved path provenance",
					"path"
				);
			const auto *inputValue = context.Find("path");
			const auto *path = inputValue ? std::get_if<Path2D>(inputValue) : nullptr;
			const auto *spatial = inputValue ? std::get_if<PathValue3D>(inputValue) : nullptr;
			const auto *curveValue = context.Find("curve");
			const auto *curve = curveValue ? std::get_if<Curve>(curveValue) : nullptr;
			if (!curve || curve->Anchors.size() > Limits::MaximumCurveAnchors)
				return context.Fail(
					Status::InvalidValue, "Source Weight Adjust requires a bounded curve", "curve"
				);
			const double type = context.SourceChoice("adjust_type"),
						 mode = context.SourceChoice("apply_mode"), value = context.Scalar("value"),
						 direction = context.Scalar("direction_shift");
			const auto range = context.Vec2("value_range", {0, 1});
			const bool loop = context.Boolean("loop");
			if (context.FailureCode != Status::Ok) return false;
			if (type < 0 || type > 2 || std::floor(type) != type || mode < 0 || mode > 2 ||
				std::floor(mode) != mode || !std::isfinite(value) || !std::isfinite(direction) ||
				!std::isfinite(range.X) || !std::isfinite(range.Y))
				return context.Fail(
					Status::InvalidValue,
					"Source Weight Adjust controls must be finite and supported",
					"adjust_type"
				);
			const uint64_t precision = context.Timeline ? context.Timeline->Frames : 1;
			if (precision == 0 || precision >= Limits::MaximumArrayElements)
				return context.Fail(
					Status::LimitExceeded, "Weight Adjust curve precision exceeds payload bounds", "curve"
				);
			const uint64_t samples = precision + 1;
			size_t count = size_t(1 + samples);
			if (count > Limits::MaximumArrayElements ||
				(!*provenance &&
				 ((!path || !ValidSourcePath2D(*path, 1, &count)) &&
				  (!spatial || !spatial->Data || !ValidSourcePath3D(*spatial->Data, 1, &count)))))
				return context.Fail(
					Status::LimitExceeded, "Weight Adjust path and curve exceed payload bounds", "path"
				);
			// Each source curve segment can perform eight Newton iterations per table
			// sample.
			const uint64_t work = samples * std::max<size_t>(1, curve->Anchors.size()) * 264;
			const auto runtimeWork =
				*provenance ? std::optional<uint64_t>{0}
							: SourceWeightRuntimeWork(path, spatial ? spatial->Data.operator->() : nullptr);
			if (!runtimeWork || work > (uint64_t{1} << 24) - *runtimeWork)
				return context.Fail(
					Status::LimitExceeded, "Weight Adjust curve sampling exceeds bounded work", "curve"
				);
			uint64_t bytes = sizeof(SourcePathData2D) + samples * sizeof(double);
			if (!*provenance) {
				const auto child =
					spatial ? SourcePath3DBytes<false>(*spatial->Data) : SourcePath2DBytes<false>(*path);
				if (child > Limits::MaximumEvaluationBytes - bytes - sizeof(Path2D))
					return context.Fail(
						Status::LimitExceeded, "Weight Adjust clone exceeds byte bounds", "path"
					);
				bytes += (spatial ? 0 : sizeof(Path2D)) + child;
			}
			if (!context.ReserveOutput(bytes, "path")) return false;
			Path2D output;
			auto &op = output.SourceOperation.emplace();
			op.Kind = SourcePathOperationKind::WeightAdjust;
			op.WeightType = uint8_t(type);
			op.WeightMode = uint8_t(mode);
			op.WeightValue = value;
			op.WeightDirection = direction;
			op.WeightRange = range;
			op.WeightLoop = loop;
			op.WeightCurve.reserve(size_t(samples));
			for (uint64_t i = 0; i < samples; ++i) {
				const double point = EvalCurveX(*curve, double(i) / double(precision), 1e-5);
				if (!std::isfinite(point))
					return context.Fail(
						Status::InvalidValue, "Weight Adjust curve table is nonfinite", "curve"
					);
				op.WeightCurve.push_back(point);
			}
			if (!*provenance) {
				if (spatial)
					op.WeightInput3D = spatial->Data;
				else
					op.Inputs.push_back(*path);
			}
			if (!ValidSourcePath2D(output))
				return context.Fail(Status::InvalidValue, "Weight Adjust output payload is invalid", "path");
			context.SetValue("path", std::move(output));
			return context.FailureCode == Status::Ok;
		}
	} // namespace
	std::span<const ExecutorEntry> SourcePathWeightExecutors() {
		static constexpr std::array ENTRIES{ExecutorEntry{"pc.path_weight_adjust", WeightAdjust, true}};
		return ENTRIES;
	}
} // namespace engine::imagegraph::detail
