#include "Families.hpp"
#include "SourceInterpret.hpp"
#include "SourceOklch.hpp"

#include <numbers>
#include <type_traits>

namespace engine::imagegraph::detail {
	namespace {
		struct NumberScan {
			size_t Leaves = 0;
			size_t Shapes = 0;
			bool Shape(NodeContext &context) {
				if (Shapes == Limits::MaximumArrayElements)
					return context.Fail(
						Status::LimitExceeded, "Interpret number traversal exceeds shape bounds", "number"
					);
				++Shapes;
				return true;
			}
		};
		// array_spread recursively flattens numeric arrays. Native tuple carriers represent source arrays.
		template <class Emit>
		bool VisitNumbers(
			NodeContext &context, const ArrayValue &array, size_t depth, NumberScan &scan, Emit &emit
		);
		template <class T, class Emit>
		bool VisitNumber(NodeContext &context, const T &value, size_t depth, NumberScan &scan, Emit &emit) {
			if (!scan.Shape(context)) return false;
			const auto scalar = [&](double number) {
				if (scan.Leaves == Limits::MaximumArrayElements)
					return context.Fail(
						Status::LimitExceeded, "Interpret number exceeds flattened array bounds", "number"
					);
				++scan.Leaves;
				const float uniform = static_cast<float>(number);
				if (!std::isfinite(uniform))
					return context.Fail(
						Status::UnsupportedExecution,
						"Interpret number requires a finite shader uniform",
						"number"
					);
				return emit(double(uniform));
			};
			if constexpr (std::is_same_v<T, double> || std::is_same_v<T, int64_t> || std::is_same_v<T, bool>)
				return scalar(double(value));
			else if constexpr (std::is_same_v<T, EnumValue>)
				return scalar(double(value.Value));
			else if constexpr (std::is_same_v<T, Colour>)
				return scalar(
					double(
						uint32_t(value.Red) | uint32_t(value.Green) << 8 | uint32_t(value.Blue) << 16 |
						uint32_t(value.Alpha) << 24
					)
				);
			else if constexpr (std::is_same_v<T, Vector2>)
				return scalar(value.X) && scalar(value.Y);
			else if constexpr (std::is_same_v<T, Vector3>)
				return scalar(value.X) && scalar(value.Y) && scalar(value.Z);
			else if constexpr (std::is_same_v<T, Vector4> || std::is_same_v<T, Quaternion>)
				return scalar(value.X) && scalar(value.Y) && scalar(value.Z) && scalar(value.W);
			else if constexpr (std::is_same_v<T, ArrayValue>)
				return VisitNumbers(context, value, depth + 1, scan, emit);
			else
				return context.Fail(
					Status::UnsupportedExecution,
					"Interpret number shader uniform contains a nonnumeric leaf",
					"number"
				);
		}
		template <class Emit>
		bool VisitNumberItems(
			NodeContext &context,
			const std::vector<SourceArrayItem> &items,
			size_t depth,
			NumberScan &scan,
			Emit &emit
		) {
			if (!scan.Shape(context)) return false;
			if (depth > 16 || items.size() > Limits::MaximumArrayElements)
				return context.Fail(
					Status::LimitExceeded, "Interpret number source shape exceeds bounds", "number"
				);
			for (const auto &item : items) {
				if (const auto *leaf = std::get_if<ElementValue>(&item.Data)) {
					if (!std::visit(
							[&](const auto &value) { return VisitNumber(context, value, depth, scan, emit); },
							*leaf
						))
						return false;
				} else if (const auto *children = std::get_if<std::vector<SourceArrayItem>>(&item.Data)) {
					if (!VisitNumberItems(context, *children, depth + 1, scan, emit)) return false;
				} else
					return context.Fail(
						Status::UnsupportedExecution,
						"Interpret number cannot upload a surface as a numeric uniform",
						"number"
					);
			}
			return true;
		}
		template <class Emit>
		bool VisitNumbers(
			NodeContext &context, const ArrayValue &array, size_t depth, NumberScan &scan, Emit &emit
		) {
			if (depth > 16 || array.Elements.size() > Limits::MaximumArrayElements ||
				array.Nested.size() > Limits::MaximumArrayElements)
				return context.Fail(
					Status::LimitExceeded, "Interpret number source shape exceeds bounds", "number"
				);
			if (!array.Items.empty()) return VisitNumberItems(context, array.Items, depth, scan, emit);
			const auto leaves = [&](const auto &elements) {
				for (const auto &element : elements)
					if (!std::visit(
							[&](const auto &value) { return VisitNumber(context, value, depth, scan, emit); },
							element
						))
						return false;
				return true;
			};
			for (const auto &row : array.Nested)
				if (!scan.Shape(context) || !leaves(row)) return false;
			return leaves(array.Elements);
		}
		bool InterpretNumber(NodeContext &context) {
			const Value *number = context.Find("number");
			if (!number)
				return context.Fail(Status::InvalidValue, "Interpret number input is missing", "number");
			NumberScan scan;
			auto measure = [](double) { return true; };
			if (!std::visit(
					[&](const auto &value) { return VisitNumber(context, value, 0, scan, measure); }, *number
				))
				return false;
			// Empty source arrays return the previous surface handle, which is absent on a fresh evaluation.
			if (!scan.Leaves)
				return context.SetOutputDiagnostic(
					"surface_out",
					Status::UnsupportedExecution,
					"Empty Interpret Number retains a source surface handle; no prior surface capture was "
					"supplied"
				);
			const auto format = ResolveProcessorSurfaceFormat(context, nullptr);
			if (!format) return false;
			Image *output = context.NewImage("surface_out", uint32_t(scan.Leaves), 1, *format);
			if (!output) return false;
			size_t x = 0;
			auto render = [&](double value) {
				const auto colour = InterpretColour(context, value, true);
				if (context.FailureCode != Status::Ok) return false;
				if (!WritePixel(*output, uint32_t(x++), 0, colour))
					return context.Fail(
						Status::InvalidValue, "Interpret number sample exceeds surface range", "surface_out"
					);
				return true;
			};
			scan = {};
			return std::visit(
				[&](const auto &value) { return VisitNumber(context, value, 0, scan, render); }, *number
			);
		}
		bool ColourToOklch(NodeContext &context) {
			const auto *value = context.Find("color");
			std::optional<Colour> colour;
			if (value)
				colour = std::visit([](const auto &raw) { return InterpretPackedColour(raw); }, *value);
			if (!colour)
				return context.Fail(
					Status::UnsupportedExecution,
					"Color OKLCH requires a resolved 32-bit packed source colour",
					"color"
				);
			const auto lab = source_oklch::SourceLinearToOklab(
				source_oklch::RgbToLinear({colour->Red / 255.0, colour->Green / 255.0, colour->Blue / 255.0})
			);
			const double chroma = std::sqrt(lab[1] * lab[1] + lab[2] * lab[2]);
			if (std::abs(lab[1]) < .0002 && std::abs(lab[2]) < .0002) {
				if (!context.SetOutputDiagnostic(
						"hue",
						Status::UnsupportedExecution,
						"Source neutral OKLCH hue is NaN; the native scalar carrier requires a finite value"
					))
					return false;
			} else {
				const double angle = std::atan2(lab[2], lab[1]) * (180.0 / std::numbers::pi);
				context.SetValue("hue", std::fmod(std::fmod(angle, 360.0) + 360.0, 360.0));
			}
			context.SetValue("lightness", lab[0]);
			context.SetValue("chroma", chroma);
			return context.FailureCode == Status::Ok;
		}
	}
	std::span<const ExecutorEntry> SourceConversionExecutors() {
		static const ExecutorEntry entries[] = {
			{"pc.interpret_number", InterpretNumber, true}, {"pc.color_to_oklch", ColourToOklch, true}
		};
		return entries;
	}
}
