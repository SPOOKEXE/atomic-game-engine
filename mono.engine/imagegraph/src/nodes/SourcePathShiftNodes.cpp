#include "../SourcePathShiftMemo.hpp"
#include "Families.hpp"
#include "Path.hpp"
namespace engine::imagegraph::detail {
	namespace {
		bool ShiftInputBytes(const Value &value, uint64_t &maximum) {
			if (const auto *path = std::get_if<Path2D>(&value)) {
				if (!ValidSourcePath2D(*path)) return false;
				maximum = std::max(maximum, SourcePath2DBytes<false>(*path));
				return true;
			}
			if (const auto *array = std::get_if<ArrayValue>(&value)) {
				if (!array->Items.empty()) return false;
				const auto leaf = [&](const ElementValue &item) {
					const auto *path = std::get_if<Path2D>(&item);
					if (!path || !ValidSourcePath2D(*path)) return false;
					maximum = std::max(maximum, SourcePath2DBytes<false>(*path));
					return true;
				};
				for (const auto &item : array->Elements)
					if (!leaf(item)) return false;
				for (const auto &row : array->Nested)
					for (const auto &item : row)
						if (!leaf(item)) return false;
				return true;
			}
			return false;
		}
		bool SourcePathShift(NodeContext &context) {
			ENGINE_PROFILE("imagegraph.source.path_shift");
			const auto provenance = context.IsCatalogueDefault("path");
			if (!provenance)
				return context.Fail(
					Status::UnsupportedExecution,
					"Source Shift requires resolved path default provenance",
					"path"
				);
			const auto *value = context.Find("path");
			const auto *input = value ? std::get_if<Path2D>(value) : nullptr;
			if (!*provenance && (!input || !ValidSourcePath2D(*input)))
				return context.Fail(
					Status::InvalidValue, "Source Shift requires a bounded planar path", "path"
				);
			const double distance = context.Scalar("distance");
			const Vector2 range = context.Vec2("range", {0, 1});
			const bool loop = context.Boolean("loop");
			if (context.FailureCode != Status::Ok) return false;
			if (!std::isfinite(distance) || !std::isfinite(range.X) || !std::isfinite(range.Y))
				return context.Fail(Status::InvalidValue, "Source Shift controls must be finite", "distance");
			uint64_t maximum = *provenance ? 0 : SourcePath2DBytes<false>(*input);
			if (context.ProcessorRow == 0) {
				for (const auto &[port, original] : context.ProcessorOriginalValues)
					if (port == "path" && original && !ShiftInputBytes(*original, maximum))
						return context.Fail(
							Status::UnsupportedExecution,
							"Source Shift batch requires planar path rows",
							"path"
						);
				const uint64_t rows = std::max<size_t>(1, context.ProcessorCount);
				if (maximum > Limits::MaximumEvaluationBytes - sizeof(SourcePathData2D) - sizeof(Path2D))
					return context.Fail(
						Status::LimitExceeded, "Source Shift batch clone exceeds byte bounds", "path"
					);
				const uint64_t each = maximum + sizeof(SourcePathData2D) + sizeof(Path2D);
				if (rows > Limits::MaximumEvaluationBytes / each)
					return context.Fail(
						Status::LimitExceeded, "Source Shift batch clone exceeds byte bounds", "path"
					);
				auto admission = context.ReserveWorkspace(each * rows, "path");
				if (!admission) return false;
			}
			const uint64_t bytes = sizeof(SourcePathData2D) +
								   (*provenance ? 0 : sizeof(Path2D) + SourcePath2DBytes<false>(*input));
			if (!context.ReserveOutput(bytes, "path")) return false;
			Path2D output;
			auto &op = output.SourceOperation.emplace();
			op.Kind = SourcePathOperationKind::Shift;
			op.ShiftDistance = distance;
			op.ShiftRange = range;
			op.ShiftLoop = loop;
			if (!*provenance) op.Inputs.push_back(*input);
			if (!ValidSourcePath2D(output))
				return context.Fail(
					Status::LimitExceeded, "Source Shift tree exceeds depth or payload limits", "path"
				);
			if (!StampSourcePathShiftOutput(context, output)) return false;
			context.SetValue("path", std::move(output));
			return context.FailureCode == Status::Ok;
		}
	}
	std::span<const ExecutorEntry> SourcePathShiftExecutors() {
		static constexpr std::array ENTRIES{ExecutorEntry{"pc.path_shift", SourcePathShift, true}};
		return ENTRIES;
	}
}
