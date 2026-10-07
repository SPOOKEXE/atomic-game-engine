#include "SourcePathBake.hpp"

#include "Path.hpp"

#include <new>
#include <stdexcept>

namespace engine::imagegraph::detail {
	namespace source_path_bake {
		constexpr uint64_t WORK_LIMIT = 64 * 1024 * 1024;
		bool Execute(NodeContext &context) try {
			ENGINE_PROFILE("imagegraph.source.path_bake");
			const auto *value = context.Find("path");
			const auto *input = value ? std::get_if<Path2D>(value) : nullptr;
			const auto provenance = context.IsCatalogueDefault("path");
			if (provenance && *provenance) {
				if (!context.ReserveOutput(sizeof(ArrayValue), "segments")) return false;
				context.SetValue("segments", ArrayValue{ValueType::Any, {}});
				context.SetValue("path", int64_t{-4});
				return context.FailureCode == Status::Ok;
			}
			if (!input || !ValidSourcePath2D(*input))
				return context.Fail(
					Status::InvalidValue, "Bake Path requires a valid planar source path", "path"
				);
			const double mode = context.SourceChoice("sample_type");
			const int64_t amount = std::max(int64_t{2}, context.Integer("output_amount", 1));
			const double step = mode == 0 ? context.Scalar("segment_length", 1) : 1;
			if (context.FailureCode != Status::Ok) return false;
			if ((mode != 0 && mode != 1) || !std::isfinite(step) || step <= 0)
				return context.Fail(
					Status::InvalidValue, "Bake Path sample controls are undefined", "segment_length"
				);
			if (mode == 1 && amount > int64_t(Limits::MaximumArrayElements))
				return context.Fail(
					Status::LimitExceeded, "Bake Path amount exceeds bounds", "output_amount"
				);
			// Anchor initialization uses 33 curve probes; each baked distance sample scans
			// its chord table twice. Charge a conservative payload-scaled upper bound.
			const uint64_t samplerBytes = SourcePath2DBytes<false>(*input);
			if (samplerBytes > WORK_LIMIT / 64)
				return context.Fail(
					Status::LimitExceeded, "Bake Path sampler payload exceeds bounds", "path"
				);
			const uint64_t samplerWork =
				samplerBytes * 64 / sizeof(double) + Limits::MaximumArrayElements + 1024;
			if (samplerWork > WORK_LIMIT / std::max(size_t{1}, context.ProcessorCount))
				return context.Fail(
					Status::LimitExceeded, "Bake Path sampler initialization exceeds bounds", "path"
				);
			PathRuntime runtime;
			if (!runtime.Init(context, *input)) return false;
			const size_t lines = runtime.LineCount();
			if (lines >= Limits::MaximumArrayElements)
				return context.Fail(Status::LimitExceeded, "Bake Path line count exceeds bounds", "path");
			// Source ignores its Loop input and reads the input path's loop property.
			const bool loop = runtime.Loop;
			uint64_t points = 0;
			for (size_t line = 0; line < lines; ++line) {
				const double length = runtime.Length(line);
				if (!std::isfinite(length) || (length < 0 && mode == 0 && loop))
					return context.Fail(Status::InvalidValue, "Bake Path source length is invalid", "path");
				if (length == 0 || (mode == 0 && length < 0)) continue;
				const double samples =
					mode == 0 ? std::floor(length / step) + 2 + loop : double(amount) + 1 + loop;
				if (!std::isfinite(samples) || samples > Limits::MaximumArrayElements - points)
					return context.Fail(Status::LimitExceeded, "Bake Path samples exceed bounds", "segments");
				points += uint64_t(samples);
			}
			if (points > (Limits::MaximumArrayElements - 1 - lines) / 4 ||
				samplerWork > WORK_LIMIT / std::max(size_t{1}, context.ProcessorCount) /
								  std::max(uint64_t{1}, points + lines))
				return context.Fail(
					Status::LimitExceeded, "Bake Path whole batch sampling exceeds bounds", "segments"
				);
			const uint64_t bytes = sizeof(Path2D) + sizeof(SourcePathData2D) + sizeof(SourcePathBakedData2D) +
								   lines * sizeof(std::vector<Vector3>) + points * sizeof(Vector3) +
								   sizeof(ArrayValue) + (lines + points * 4) * sizeof(SourceArrayItem);
			auto staging = context.ReserveWorkspace(bytes, "segments");
			if (!staging) return false;
			Path2D baked;
			auto &operation = baked.SourceOperation.emplace();
			operation.Kind = SourcePathOperationKind::Bake;
			auto &data = operation.Baked.emplace();
			data.Lines.reserve(lines);
			for (size_t line = 0; line < lines; ++line) {
				auto &row = data.Lines.emplace_back();
				const double length = runtime.Length(line);
				if (length == 0 || (mode == 0 && length < 0)) continue;
				const size_t capacity =
					mode == 0 ? size_t(std::floor(length / step) + 2 + loop) : size_t(amount + 1 + loop);
				row.reserve(capacity);
				if (mode == 0) {
					for (double distance = 0; distance <= length;) {
						if (row.size() >= capacity - size_t(loop))
							return context.Fail(
								Status::LimitExceeded,
								"Bake Path floating step exceeded admission",
								"segment_length"
							);
						const auto point = runtime.PointDistance(distance, line);
						row.push_back({point.X, point.Y, distance / length});
						const double next = distance + step;
						if (next <= distance)
							return context.Fail(
								Status::InvalidValue,
								"Bake Path floating step cannot advance",
								"segment_length"
							);
						distance = next;
					}
				} else {
					const double stepRatio = 1. / amount;
					for (int64_t index = 0; index <= amount; ++index) {
						const double ratio = index * stepRatio;
						const auto point =
							runtime.PointRatio(loop ? ratio : std::clamp(ratio, 0., .999), line);
						row.push_back({point.X, point.Y, ratio});
					}
				}
				if (loop) row.push_back({row.front().X, row.front().Y, 1});
			}
			if (!ValidSourcePath2D(baked))
				return context.Fail(Status::InvalidValue, "Bake Path samples are nonfinite", "segments");
			ArrayValue segments{ValueType::Any, {}};
			segments.Items.reserve(lines);
			for (const auto &row : data.Lines) {
				std::vector<SourceArrayItem> samples;
				samples.reserve(row.size());
				for (const auto &point : row) {
					std::vector<SourceArrayItem> coordinates;
					coordinates.reserve(3);
					for (double component : {point.X, point.Y, point.Z})
						coordinates.push_back({ElementValue{component}});
					samples.push_back({std::move(coordinates)});
				}
				segments.Items.push_back({std::move(samples)});
			}
			if (context.Boolean("spread_single_path", true) && lines == 1) {
				auto samples = std::move(std::get<std::vector<SourceArrayItem>>(segments.Items.front().Data));
				segments.Items = std::move(samples);
			}
			if (context.FailureCode != Status::Ok) return false;
			const uint64_t retained = RetainedPayloadBytes(segments) + RetainedPayloadBytes(baked);
			if (retained > bytes)
				return context.Fail(
					Status::LimitExceeded, "Bake Path capacities exceed admission", "segments"
				);
			if (!context.ReserveOutput(retained, "segments")) return false;
			if (!context.SetOutputDomain(
					"segments", {ValueType::Scalar, SourceValueDisplay::Vector, SourceSocketKind::Float}
				))
				return false;
			context.SetValue("segments", std::move(segments));
			context.SetValue("path", std::move(baked));
			return context.FailureCode == Status::Ok;
		} catch (const std::bad_alloc &) {
			return context.Fail(Status::LimitExceeded, "Bake Path allocation failed", "path");
		} catch (const std::length_error &) {
			return context.Fail(Status::LimitExceeded, "Bake Path allocation length exceeded", "path");
		}
	}
	std::span<const ExecutorEntry> SourcePathBakeExecutors() {
		static constexpr ExecutorEntry entries[]{{"pc.path_bake", source_path_bake::Execute, true}};
		return entries;
	}
}
