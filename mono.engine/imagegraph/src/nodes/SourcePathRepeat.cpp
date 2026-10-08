#include "SourcePathRepeat.hpp"

#include "../SourceMirrorPathProjection.hpp"
#include "Path.hpp"

#include <array>
#include <cmath>
#include <numbers>

namespace engine::imagegraph::detail {
	namespace {
		constexpr uint64_t REPEAT_WORK_LIMIT = uint64_t{1} << 24;

		std::optional<size_t> RepeatLineCount(const Path2D &path);
		std::optional<size_t> RepeatLineCount(const PathData3D &path) {
			if (!path.SourceOperation)
				return path.Source2D ? RepeatLineCount(*path.Source2D) : std::optional<size_t>{1};
			const auto &op = *path.SourceOperation;
			if (op.Kind != SourcePathOperationKind::Combine)
				return op.Inputs.empty() ? std::optional<size_t>{1} : RepeatLineCount(*op.Inputs[0].Data);
			size_t total = 0;
			for (const auto &child : op.Inputs) {
				const auto count = RepeatLineCount(*child.Data);
				if (!count || *count > Limits::MaximumArrayElements - total) return {};
				total += *count;
			}
			return total;
		}
		std::optional<size_t> RepeatLineCount(const Path2D &path) {
			if (!path.SourceOperation) return 1;
			const auto &op = *path.SourceOperation;
			if (op.Baked) return op.Baked->Lines.size();
			if (op.Sequential && op.Kind != SourcePathOperationKind::Smoothen) return 1;
			if (op.WeightInput3D) return RepeatLineCount(*op.WeightInput3D);
			if (op.Kind == SourcePathOperationKind::VerletMesh) return 1;
			if (op.Kind == SourcePathOperationKind::Blend)
				return op.BlendInputsValid[0] ? RepeatLineCount(op.Inputs[0]) : std::optional<size_t>{1};
			if (op.Kind != SourcePathOperationKind::Combine && op.Kind != SourcePathOperationKind::Join &&
				op.Kind != SourcePathOperationKind::Repeat)
				return op.Inputs.empty() ? std::optional<size_t>{1} : RepeatLineCount(op.Inputs[0]);
			size_t total = 0;
			for (const auto &child : op.Inputs) {
				const auto count = RepeatLineCount(child);
				if (!count || *count > Limits::MaximumArrayElements - total) return {};
				total += *count;
			}
			return total;
		}

		bool RepeatVector(
			NodeContext &context, std::string_view port, Vector2 fallback, bool units, Vector2 &output
		) {
			const auto *surface = context.Input(port);
			const auto *value = context.Find(port);
			if (!surface && value && !std::holds_alternative<Vector2>(*value) &&
				!std::holds_alternative<double>(*value) && !std::holds_alternative<int64_t>(*value))
				return context.Fail(
					Status::UnsupportedExecution,
					"Repeat Path vector getter requires a numeric pair, scalar, surface or sampled planar "
					"path",
					port
				);
			output = surface ? Vector2{double(surface->Width), double(surface->Height)}
							 : context.Vec2(port, fallback);
			if (units && !surface && !SourceRepeatPathSampled(context, port)) {
				const std::string unitPort = std::string(port) + "_unit";
				const double unit = context.SourceChoice(unitPort, 1);
				if (context.FailureCode != Status::Ok) return false;
				if (unit != std::trunc(unit))
					return context.Fail(
						Status::UnsupportedExecution,
						"Repeat Path fractional unit choice is undefined",
						unitPort
					);
				if (unit < 0 || unit > 1)
					return context.Fail(
						Status::InvalidValue, "Repeat Path unit must be Pixel or Reference", unitPort
					);
				if (unit == 1) {
					output.X *= context.Project.SurfaceWidth;
					output.Y *= context.Project.SurfaceHeight;
				}
			}
			return (std::isfinite(output.X) && std::isfinite(output.Y)) ||
				   context.Fail(Status::InvalidValue, "Repeat Path vector must be finite", port);
		}

		struct RepeatCopy {
			Vector2 Position, Scale;
			double Rotation = 0;
		};

		RepeatCopy CopyControls(
			size_t index,
			size_t amount,
			bool circular,
			bool rotateAlong,
			Vector2 position,
			Vector2 shiftPosition,
			Vector2 center,
			Vector2 radius,
			double rotation,
			double shiftRotation,
			Vector2 scale,
			Vector2 shiftScale
		) {
			const double angle = circular ? (360. / amount) * index : 0;
			const double radians = angle * std::numbers::pi / 180;
			return {
				{position.X + (circular ? center.X + radius.X * std::cos(radians) : 0) +
					 shiftPosition.X * index,
				 position.Y + (circular ? center.Y - radius.Y * std::sin(radians) : 0) +
					 shiftPosition.Y * index},
				{scale.X * std::pow(shiftScale.X, double(index)),
				 scale.Y * std::pow(shiftScale.Y, double(index))},
				rotation + (circular && rotateAlong ? angle : 0) + shiftRotation * index
			};
		}
	}

	bool SourcePathRepeat(NodeContext &context) {
		ENGINE_PROFILE("imagegraph.source.path_repeat");
		const auto provenance = context.IsCatalogueDefault("path");
		if (!provenance)
			return context.Fail(
				Status::UnsupportedExecution, "Repeat Path requires resolved path default provenance", "path"
			);
		const auto *value = context.Find("path");
		const auto *input = *provenance ? nullptr : (value ? std::get_if<Path2D>(value) : nullptr);
		if (!*provenance && !input)
			return context.Fail(
				Status::UnsupportedExecution, "Repeat Path requires a planar source path", "path"
			);

		const double rawAmount = context.Scalar("amount", 4);
		if (!std::isfinite(rawAmount))
			return context.Fail(Status::InvalidValue, "Repeat Path amount must be finite", "amount");
		if (rawAmount < 0)
			return context.Fail(
				Status::UnsupportedExecution, "Repeat Path negative source array size is undefined", "amount"
			);
		if (rawAmount != std::trunc(rawAmount))
			return context.Fail(
				Status::UnsupportedExecution,
				"Repeat Path fractional source array size is undefined",
				"amount"
			);
		if (rawAmount > Limits::MaximumArrayElements)
			return context.Fail(Status::LimitExceeded, "Repeat Path amount exceeds bounds", "amount");
		const size_t amount = input ? size_t(rawAmount) : 0;
		const double pattern = context.SourceChoice("pattern");
		if (context.FailureCode != Status::Ok) return false;
		if (pattern != std::trunc(pattern))
			return context.Fail(
				Status::UnsupportedExecution, "Repeat Path fractional pattern branch is undefined", "pattern"
			);
		if (pattern < 0 || pattern > 1)
			return context.Fail(
				Status::InvalidValue, "Repeat Path pattern must be Linear or Circular", "pattern"
			);
		Vector2 position, shiftPosition, center, radius, anchor, scale, shiftScale;
		if (!RepeatVector(context, "position", {}, true, position) ||
			!RepeatVector(context, "shift_position", {}, true, shiftPosition) ||
			!RepeatVector(context, "center", {.5, .5}, true, center) ||
			!RepeatVector(context, "radius", {.5, .5}, true, radius) ||
			!RepeatVector(context, "anchor", {}, true, anchor) ||
			!RepeatVector(context, "scale", {1, 1}, false, scale) ||
			!RepeatVector(context, "shift_scale", {1, 1}, false, shiftScale))
			return false;
		const double rotation = context.Scalar("rotation"), shiftRotation = context.Scalar("shift_rotation");
		if (!std::isfinite(rotation) || !std::isfinite(shiftRotation))
			return context.Fail(Status::InvalidValue, "Repeat Path rotation must be finite", "rotation");
		const bool rotateAlong = context.Boolean("rotate_along", true);
		if (context.FailureCode != Status::Ok) return false;

		size_t inputCount = 0;
		if (input && !ValidSourcePath2D(*input, amount ? 3 : 0, &inputCount))
			return context.Fail(
				Status::LimitExceeded, "Repeat Path input depth or payload exceeds bounds", "path"
			);
		if (amount && amount > (Limits::MaximumArrayElements - 1) / (inputCount + 2))
			return context.Fail(
				Status::LimitExceeded, "Repeat Path cloned operation count exceeds bounds", "amount"
			);
		const auto lines = input ? RepeatLineCount(*input) : std::optional<size_t>{0};
		if (!lines || (amount && *lines > Limits::MaximumArrayElements / amount))
			return context.Fail(Status::LimitExceeded, "Repeat Path line count exceeds bounds", "amount");
		const auto work = input ? SourceWeightRuntimeWork(input, nullptr) : std::optional<uint64_t>{0};
		const uint64_t copies = std::max<size_t>(amount, 1),
					   rows = std::max<size_t>(context.ProcessorCount, 1);
		if (!work || *work + 16 > REPEAT_WORK_LIMIT / rows / copies / 8 / std::max<size_t>(*lines, 1))
			return context.Fail(
				Status::LimitExceeded, "Repeat Path processor batch exceeds bounded work", "amount"
			);
		const uint64_t childBytes = input ? SourcePath2DBytes<false>(*input) : 0;
		const uint64_t overhead = 2 * sizeof(SourcePathData2D) + 3 * sizeof(Path2D);
		if (childBytes > Limits::MaximumEvaluationBytes - overhead ||
			amount > (Limits::MaximumEvaluationBytes - sizeof(SourcePathData2D)) / (childBytes + overhead))
			return context.Fail(
				Status::LimitExceeded, "Repeat Path cloned payload exceeds byte bounds", "amount"
			);
		const uint64_t bytes = sizeof(SourcePathData2D) + amount * (childBytes + overhead);
		if (bytes > Limits::MaximumEvaluationBytes / rows)
			return context.Fail(
				Status::LimitExceeded, "Repeat Path processor payload exceeds byte bounds", "amount"
			);
		if (!context.ReserveOutput(bytes, "path")) return false;

		for (size_t index = 0; index < amount; ++index) {
			const auto copy = CopyControls(
				index,
				amount,
				pattern == 1,
				rotateAlong,
				position,
				shiftPosition,
				center,
				radius,
				rotation,
				shiftRotation,
				scale,
				shiftScale
			);
			if (!std::isfinite(copy.Position.X) || !std::isfinite(copy.Position.Y) ||
				!std::isfinite(copy.Scale.X) || !std::isfinite(copy.Scale.Y) ||
				!std::isfinite(copy.Rotation) || !std::isfinite(copy.Rotation * std::numbers::pi / 180))
				return context.Fail(
					Status::InvalidValue, "Repeat Path derived copy controls are nonfinite", "amount"
				);
		}

		Path2D output;
		auto &op = output.SourceOperation.emplace();
		op.Kind = SourcePathOperationKind::Repeat;
		op.Inputs.reserve(amount);
		for (size_t index = 0; index < amount; ++index) {
			const auto copy = CopyControls(
				index,
				amount,
				pattern == 1,
				rotateAlong,
				position,
				shiftPosition,
				center,
				radius,
				rotation,
				shiftRotation,
				scale,
				shiftScale
			);
			// Repeat rotates before scaling. Two ordinary transforms keep that order durable.
			Path2D rotated;
			auto &rotate = rotated.SourceOperation.emplace();
			rotate.Kind = SourcePathOperationKind::Transform;
			rotate.TransformAnchor = anchor;
			rotate.TransformRotation = copy.Rotation;
			rotate.Inputs.push_back(*input);
			Path2D transformed;
			auto &transform = transformed.SourceOperation.emplace();
			transform.Kind = SourcePathOperationKind::Transform;
			transform.TransformAnchor = anchor;
			transform.TransformPosition = copy.Position;
			transform.TransformScale = copy.Scale;
			transform.Inputs.push_back(std::move(rotated));
			op.Inputs.push_back(std::move(transformed));
		}
		if (!ValidSourcePath2D(output))
			return context.Fail(Status::LimitExceeded, "Repeat Path output exceeds bounded payload", "path");
		SourcePathShiftValidationScope validation(context);
		PathRuntime check;
		if (!check.Init(context, output)) return false;
		if (check.LineCount() > Limits::MaximumArrayElements)
			return context.Fail(Status::LimitExceeded, "Repeat Path line count exceeds bounds", "amount");
		for (size_t line = 0; line < check.LineCount(); ++line)
			for (double ratio : std::array<double, 3>{0, .5, 1}) {
				const auto point = check.PointRatio(ratio, line);
				if (!std::isfinite(point.X) || !std::isfinite(point.Y) || !std::isfinite(point.Weight))
					return context.Fail(
						Status::InvalidValue, "Repeat Path transformed geometry is nonfinite", "path"
					);
			}
		if (context.FailureCode != Status::Ok) return false;
		context.SetValue("path", std::move(output));
		return context.FailureCode == Status::Ok;
	}
}
