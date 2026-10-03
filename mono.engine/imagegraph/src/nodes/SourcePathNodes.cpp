#include "../SourcePathSequentialState.hpp"
#include "Families.hpp"
#include "Path.hpp"

#include <array>
#include <numbers>
namespace engine::imagegraph::detail {
	bool SampleSourcePath3D(NodeContext &context);
	bool SourceSpatialPathOperation(NodeContext &context);
	namespace {
		bool Present(const Path2D &path) {
			return path.SourceOperation || !path.Anchors.empty();
		}
		bool BuildOperation(NodeContext &context) {
			const bool reverse = context.Authored.Type != "pc.path_array";
			const auto spatialItems = [&](auto &&self, const std::vector<SourceArrayItem> &entries) -> bool {
				for (const auto &item : entries) {
					if (const auto *leaf = std::get_if<ElementValue>(&item.Data)) {
						if (std::holds_alternative<PathValue3D>(*leaf)) return true;
					} else if (self(self, std::get<std::vector<SourceArrayItem>>(item.Data)))
						return true;
				}
				return false;
			};
			const auto spatial = [&](const Value *value) {
				if (!value || !ValidRuntimeValue(*value)) return false;
				if (std::holds_alternative<PathValue3D>(*value)) return true;
				const auto *array = std::get_if<ArrayValue>(value);
				if (!array) return false;
				if (spatialItems(spatialItems, array->Items)) return true;
				for (const auto &leaf : array->Elements)
					if (std::holds_alternative<PathValue3D>(leaf)) return true;
				for (const auto &row : array->Nested)
					for (const auto &leaf : row)
						if (std::holds_alternative<PathValue3D>(leaf)) return true;
				return false;
			};
			if (reverse) {
				if (spatial(context.Find("path"))) return SourceSpatialPathOperation(context);
			} else
				for (const auto &input : context.Authored.DynamicInputs)
					if (spatial(context.Find(input.Id))) return SourceSpatialPathOperation(context);
			std::vector<const Path2D *> inputs;
			bool materialize = false;
			size_t inputCount = 0;
			uint64_t bytes = sizeof(SourcePathData2D);
			size_t nodes = 1;
			const auto append = [&](const auto &value) {
				if (const auto *path = std::get_if<Path2D>(&value)) {
					if (!Present(*path)) return true;
					if (!ValidSourcePath2D(*path, 1, &nodes))
						return context.Fail(
							Status::LimitExceeded, "path operation tree exceeds source bounds", "path"
						);
					if (inputCount >= Limits::MaximumArrayElements)
						return context.Fail(Status::LimitExceeded, "path line count exceeds bounds", "path");
					const uint64_t size = SourcePath2DBytes<false>(*path) + sizeof(Path2D);
					if (size > Limits::MaximumEvaluationBytes - bytes)
						return context.Fail(
							Status::LimitExceeded, "path operation exceeds byte bounds", "path"
						);
					bytes += size;
					++inputCount;
					if (materialize) inputs.push_back(path);
				} else if (std::holds_alternative<PathValue3D>(value))
					return context.Fail(
						Status::UnsupportedExecution,
						"planar source operation requires a 3D path adapter",
						"path"
					);
				return true;
			};
			const auto items = [&](auto &&self, const std::vector<SourceArrayItem> &entries) -> bool {
				for (const auto &item : entries) {
					if (const auto *leaf = std::get_if<ElementValue>(&item.Data)) {
						if (!append(*leaf)) return false;
					} else if (!self(self, std::get<std::vector<SourceArrayItem>>(item.Data)))
						return false;
				}
				return true;
			};
			const auto visit = [&](const Value *value) {
				if (!value) return true;
				if (!ValidRuntimeValue(*value))
					return context.Fail(Status::InvalidValue, "path input payload is invalid", "path");
				if (const auto *array = std::get_if<ArrayValue>(value)) {
					if (!array->Items.empty()) return items(items, array->Items);
					for (const auto &leaf : array->Elements)
						if (!append(leaf)) return false;
					for (const auto &row : array->Nested)
						for (const auto &leaf : row)
							if (!append(leaf)) return false;
					return true;
				}
				return append(*value);
			};
			if (reverse) {
				if (!visit(context.Find("path"))) return false;
				if (inputCount > 1)
					return context.Fail(
						Status::InvalidValue, "reverse source requires one resolved path", "path"
					);
			} else
				for (const auto &input : context.Authored.DynamicInputs)
					if (!visit(context.Find(input.Id))) return false;
			auto scratch = context.ReserveWorkspace(inputCount * sizeof(const Path2D *), "path");
			if (!scratch) return false;
			inputs.reserve(inputCount);
			materialize = true;
			inputCount = 0;
			nodes = 1;
			bytes = sizeof(SourcePathData2D);
			if (reverse) {
				if (!visit(context.Find("path"))) return false;
				if (inputCount > 1)
					return context.Fail(
						Status::InvalidValue, "reverse source requires one resolved path", "path"
					);
			} else
				for (const auto &input : context.Authored.DynamicInputs)
					if (!visit(context.Find(input.Id))) return false;
			if (!context.ReserveOutput(bytes + 32, "path")) return false;
			Path2D output;
			auto &op = output.SourceOperation.emplace();
			op.Kind = reverse ? SourcePathOperationKind::Reverse : SourcePathOperationKind::Combine;
			if (context.Authored.Type == "pc.path_trim") {
				op.Kind = SourcePathOperationKind::Trim;
				op.TrimRange = context.Vec2("range", {0, 1});
				op.TrimRange.X += context.Scalar("shift");
				op.TrimRange.Y += context.Scalar("shift");
				if (context.Boolean("clamp")) {
					const auto bounds = context.Vec2("range_2", {0, 1});
					op.TrimRange.X = std::max(bounds.X, std::min(bounds.Y, op.TrimRange.X));
					op.TrimRange.Y = std::max(bounds.X, std::min(bounds.Y, op.TrimRange.Y));
				}
			}
			op.Inputs.reserve(inputs.size());
			for (const auto *path : inputs)
				op.Inputs.push_back(*path);
			context.SetValue(reverse ? "path" : "combined_path", std::move(output));
			return context.FailureCode == Status::Ok;
		}
		bool Anchor(NodeContext &context) {
			const auto position = context.Vec2("postion");
			const auto first = context.Vec2("control_point_1", {-16, 0});
			const auto second = context.Boolean("mirror_control_point", true)
									? Vector2{-first.X, -first.Y}
									: context.Vec2("control_point_2", {16, 0});
			if (!context.ReserveOutput(6 * sizeof(ElementValue) + 32, "anchor")) return false;
			ArrayValue output;
			output.ElementType = ValueType::Scalar;
			output.Elements = {position.X, position.Y, first.X, first.Y, second.X, second.Y};
			context.SetValue("anchor", std::move(output));
			return context.FailureCode == Status::Ok;
		}
		bool Sample(NodeContext &context) {
			const auto *value = context.Find("path");
			if (value && std::holds_alternative<PathValue3D>(*value)) return SampleSourcePath3D(context);
			const auto *path = value ? std::get_if<Path2D>(value) : nullptr;
			if (value && !path)
				return context.Fail(
					Status::TypeMismatch, "planar path sampler requires a planar path", "path"
				);
			if (!path || !Present(*path)) {
				context.SetValue("position", Vector2{});
				context.SetValue("direction", 0.0);
				context.SetValue("weight", 0.0);
				return context.FailureCode == Status::Ok;
			}
			PathRuntime runtime;
			if (!runtime.Init(context, *path)) return false;
			const auto range = context.Vec2("range", {0, 1});
			const double shift = context.Scalar("shift"), raw = context.Scalar("ratio"),
						 mode = context.SourceChoice("type");
			double ratio = range.X + (range.Y - range.X) * (raw + shift - std::trunc(shift));
			bool inverted = false;
			if (mode == 0)
				ratio -= std::trunc(ratio);
			else if (mode == 1) {
				const double whole = std::floor(ratio);
				double fraction = ratio - std::trunc(ratio);
				if (std::fmod(whole, 2) == 1 && fraction != 0) {
					fraction = 1 - fraction;
					inverted = true;
				}
				ratio = fraction;
			} else if (mode == 2)
				ratio = std::clamp(ratio, 0.0, .999);
			if (!std::isfinite(ratio))
				return context.Fail(Status::InvalidValue, "path sample ratio is nonfinite", "ratio");
			const int64_t index = context.Integer("path_index");
			if (index < 0)
				return context.Fail(
					Status::InvalidValue, "path sample line index is undefined", "path_index"
				);
			if (!LoadSourceSamplerBuffers(context)) return false;
			auto &buffers = *context.SourceSamplerBuffers;
			buffers[0] = runtime.PointRatioInto(ratio, size_t(index), buffers[0]);
			buffers[1] = runtime.PointRatioInto(std::clamp(ratio - .0001, 0., 1.), size_t(index), buffers[1]);
			buffers[2] = runtime.PointRatioInto(std::clamp(ratio + .0001, 0., 1.), size_t(index), buffers[2]);
			if (context.FailureCode != Status::Ok || !SetSourceSamplerOutputs(context, inverted))
				return false;
			return context.FailureCode == Status::Ok && PublishSourceSamplerBuffers(context);
		}
	}
	std::span<const ExecutorEntry> SourcePathExecutors() {
		static constexpr std::array entries{
			ExecutorEntry{"pc.path_reverse", BuildOperation},
			ExecutorEntry{"pc.path_array", BuildOperation},
			ExecutorEntry{"pc.path_sample", Sample},
			ExecutorEntry{"pc.path_anchor", Anchor},
			ExecutorEntry{"pc.path_trim", BuildOperation}
		};
		return entries;
	}
}
