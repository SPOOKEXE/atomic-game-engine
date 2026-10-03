#include "Source2DComplexGenerator.hpp"

#include <numbers>

namespace engine::imagegraph::detail {
	namespace {
		constexpr uint64_t FLOW_BASE_WORK = 512, FLOW_STEP_WORK = 32;
		template <class V> std::optional<double> FlowNumber(const V &value) {
			if (const auto *number = std::get_if<double>(&value)) return *number;
			if (const auto *integer = std::get_if<int64_t>(&value)) return double(*integer);
			return std::nullopt;
		}
		bool FlowPair(NodeContext &c, const Value &value, std::string_view port, Vector2 &pair) {
			if (const auto *point = std::get_if<Vector2>(&value))
				pair = *point;
			else if (const auto number = FlowNumber(value))
				pair = {*number, *number};
			else if (const auto *array = std::get_if<ArrayValue>(&value);
					 array && array->Nested.empty() && array->Items.empty()) {
				pair = {};
				for (size_t i = 0; i < std::min<size_t>(2, array->Elements.size()); ++i) {
					const auto number = FlowNumber(array->Elements[i]);
					if (!number)
						return c.Fail(
							Status::UnsupportedExecution, "Flow tuple needs numeric components", port
						);
					(i == 0 ? pair.X : pair.Y) = *number;
				}
			} else
				return c.Fail(Status::UnsupportedExecution, "Flow tuple shape is not represented", port);
			return (std::isfinite(pair.X) && std::isfinite(pair.Y)) ||
				   c.Fail(Status::InvalidValue, "Flow tuple must be finite", port);
		}
		bool FlowTuple(NodeContext &c, std::string_view port, Vector2 fallback, Vector2 &pair) {
			const auto *value = c.Find(port);
			if (!value) {
				pair = fallback;
				return true;
			}
			return FlowPair(c, *value, port, pair);
		}
		bool FlowCount(NodeContext &c, Vector2 range, uint64_t &count) {
			count = 0;
			if (range.X > range.Y) return true;
			if (range.X + 1 == range.X || range.Y + 1 == range.Y)
				return c.Fail(
					Status::UnsupportedExecution,
					"Flow Detail increment stalls in native arithmetic",
					"detail"
				);
			const double span = range.Y - range.X;
			if (!std::isfinite(span) ||
				span >= double((source2d::COMPLEX_GENERATOR_WORK_LIMIT - FLOW_BASE_WORK) / FLOW_STEP_WORK))
				return c.Fail(
					Status::LimitExceeded, "Flow whole-array work exceeds the native CPU limit", "detail"
				);
			if (range.X <= 0 && range.Y >= 0 && std::floor(-range.X) == -range.X)
				return c.Fail(Status::UnsupportedExecution, "Flow Detail visits a zero divisor", "detail");
			count = uint64_t(std::floor(span)) + 2;
			return true;
		}
		bool FlowOriginalTuple(NodeContext &c, const Value &value, uint64_t &maximum) {
			Vector2 range;
			uint64_t count = 0;
			if (!FlowPair(c, value, "detail", range) || !FlowCount(c, range, count)) return false;
			maximum = std::max(maximum, count);
			return true;
		}
		bool FlowOriginalLeaf(NodeContext &c, const ElementValue &leaf, uint64_t &maximum) {
			if (const auto *pair = std::get_if<Vector2>(&leaf))
				return FlowOriginalTuple(c, Value{*pair}, maximum);
			if (const auto number = FlowNumber(leaf)) return FlowOriginalTuple(c, Value{*number}, maximum);
			return c.Fail(
				Status::UnsupportedExecution, "Flow Detail leaf needs numeric components", "detail"
			);
		}

		bool FlowItems(
			NodeContext &c, const std::vector<SourceArrayItem> &items, size_t depth, uint64_t &maximum
		) {
			if (depth > Limits::MaximumArrayDepth)
				return c.Fail(Status::LimitExceeded, "Flow Detail nesting exceeds native limits", "detail");
			const auto numeric = [&](const SourceArrayItem &item) {
				const auto *leaf = std::get_if<ElementValue>(&item.Data);
				return leaf ? FlowNumber(*leaf) : std::nullopt;
			};
			if (items.empty() || numeric(items.front())) {
				Vector2 range{};
				if (!items.empty()) range.X = *numeric(items.front());
				if (items.size() > 1) {
					const auto second = numeric(items[1]);
					if (!second)
						return c.Fail(
							Status::UnsupportedExecution, "Flow Detail tuple has mixed components", "detail"
						);
					range.Y = *second;
				}
				return FlowOriginalTuple(c, Value{range}, maximum);
			}
			for (const auto &item : items) {
				if (const auto *leaf = std::get_if<ElementValue>(&item.Data)) {
					if (!FlowOriginalLeaf(c, *leaf, maximum)) return false;
				} else if (!FlowItems(
							   c, std::get<std::vector<SourceArrayItem>>(item.Data), depth + 1, maximum
						   ))
					return false;
			}
			return true;
		}
		bool FlowWork(NodeContext &c) {
			if (c.ProcessorRow != 0) return true;
			uint64_t maximum = 9;
			if (const auto *original = source2d::GeneratorOriginal(c, "detail")) {
				maximum = 0;
				if (const auto *array = std::get_if<ArrayValue>(original)) {
					if (!array->Nested.empty()) {
						for (const auto &row : array->Nested) {
							Vector2 range{};
							for (size_t i = 0; i < std::min<size_t>(2, row.size()); ++i) {
								const auto number = FlowNumber(row[i]);
								if (!number)
									return c.Fail(
										Status::UnsupportedExecution,
										"Flow Detail row needs numeric components",
										"detail"
									);
								(i == 0 ? range.X : range.Y) = *number;
							}
							if (!FlowOriginalTuple(c, Value{range}, maximum)) return false;
						}
					} else if (!array->Items.empty()) {
						if (!FlowItems(c, array->Items, 0, maximum)) return false;
					} else if (!array->Elements.empty() &&
							   std::holds_alternative<Vector2>(array->Elements.front())) {
						for (const auto &leaf : array->Elements)
							if (!FlowOriginalLeaf(c, leaf, maximum)) return false;
					} else if (!FlowOriginalTuple(c, *original, maximum))
						return false;
				} else if (!FlowOriginalTuple(c, *original, maximum))
					return false;
			}
			return source2d::ComplexBatchAdmission(
				c, FLOW_BASE_WORK + maximum * FLOW_STEP_WORK, "surface_out", "detail"
			);
		}
	}
	bool SourceFlowNoise(NodeContext &c) {
		ENGINE_PROFILE("imagegraph.source.flow_noise");
		if (!FlowWork(c)) return false;
		const auto format = ResolveProcessorSurfaceFormat(c, nullptr);
		if (!format) return false;
		source2d::ComplexCanvas canvas;
		if (!source2d::ResolveComplexCanvas(c, canvas)) return false;
		Vector2 position, scale, detail, levelIn, levelOut;
		if (!source2d::ReferenceVector(c, "position", canvas.Raw, position) ||
			!FlowTuple(c, "scale", {2, 2}, scale) || !FlowTuple(c, "detail", {1, 8}, detail) ||
			!FlowTuple(c, "level_in", {0, 1}, levelIn) || !FlowTuple(c, "level_out", {0, 1}, levelOut))
			return false;
		if (levelIn.X == levelIn.Y)
			return c.Fail(Status::UnsupportedExecution, "Flow Level In divides by zero", "level_in");
		uint64_t count;
		if (!FlowCount(c, detail, count)) return false;
		const double progress = c.Scalar("progress");
		const double angle = c.Scalar("rotation") * std::numbers::pi / 180;
		const double cosine = std::cos(angle), sine = std::sin(angle);
		if (c.FailureCode != Status::Ok) return false;
		Image *output = c.NewImage("surface_out", canvas.Width, canvas.Height, *format);
		if (!output) return false;
		for (uint32_t y = 0; y < canvas.Height; ++y)
			for (uint32_t x = 0; x < canvas.Width; ++x) {
				const double u = (x + .5) / canvas.Width, v = (y + .5) / canvas.Height;
				double alpha;
				const auto uv = source2d::GeneratorUv(c, u, v, alpha);
				const Vector2 p{
					uv.X - position.X / canvas.Raw.X,
					uv.Y * canvas.Raw.Y / canvas.Raw.X - position.Y / canvas.Raw.Y
				};
				Vector2 point{(p.X * cosine - p.Y * sine) * scale.X, (p.X * sine + p.Y * cosine) * scale.Y};
				uint64_t step = 0;
				for (double i = detail.X; i <= detail.Y; i += 1, ++step) {
					if (step >= count)
						return c.Fail(
							Status::UnsupportedExecution,
							"Flow Detail increment exceeds the native bound",
							"detail"
						);
					point.X += .5 / i * std::sin(i * 3 * point.Y + progress);
					// The second update consumes the newly updated X, not the initial pair.
					point.Y += .3 / i * std::cos(i * 3 * point.X + progress);
				}
				const double wave = .5 + .5 * std::sin(point.X);
				const double value =
					levelOut.X + (levelOut.Y - levelOut.X) * (wave - levelIn.X) / (levelIn.Y - levelIn.X);
				if (!source2d::StoreComplexPixel(
						c, *output, x, y, {value, value, value, alpha}, "surface_out"
					))
					return false;
			}
		return c.FailureCode == Status::Ok;
	}
}
