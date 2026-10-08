#include "../SourceRealNumber.hpp"
#include "ArraySource.hpp"
#include "Families.hpp"
#include "SourceComparison.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace engine::imagegraph::detail {
	namespace {
		bool Tuple(NodeContext &context) {
			std::array<double, 5> tuple{};
			std::string_view port;
			if (context.Authored.Type == "pc.transform_array") {
				const auto position = context.Vec2("postion", {0, 0});
				const auto scale = context.Vec2("scale", {1, 1});
				// The source returns Scale.x twice, including when Scale.y differs.
				tuple = {position.X, position.Y, context.Scalar("rotation"), scale.X, scale.X};
				port = "transform";
			} else {
				const double type = context.SourceChoice("type");
				const double start = context.Scalar("range_start"), end = context.Scalar("range_end", 360),
							 start2 = context.Scalar("range_2_start"),
							 end2 = context.Scalar("range_2_end", 360);
				tuple = type == 0 ? std::array<double, 5>{0, start, start, start, start}
								  : std::array<double, 5>{type - 1, start, end, start2, end2};
				port = "rotation_random";
			}
			if (!std::all_of(tuple.begin(), tuple.end(), [](double x) { return std::isfinite(x); }))
				return context.Fail(
					Status::InvalidValue, "source tuple contains a nonfinite component", port
				);
			if (!context.ReserveOutput(tuple.size() * sizeof(ElementValue), port)) return false;
			ArrayValue result{ValueType::Scalar, {}};
			result.Elements.reserve(tuple.size());
			for (double x : tuple)
				result.Elements.emplace_back(x);
			context.SetValue(port, std::move(result));
			return context.FailureCode == Status::Ok;
		}
		bool ConditionArrayOperand(NodeContext &context, std::string_view port) {
			const auto *value = context.Find(port);
			// Float getters expose source tuples, Matrix.to_real and surface dimensions as arrays.
			if (context.Input(port) ||
				(value &&
				 (std::holds_alternative<ArrayValue>(*value) || std::holds_alternative<Vector2>(*value) ||
				  std::holds_alternative<Vector3>(*value) || std::holds_alternative<Vector4>(*value) ||
				  std::holds_alternative<Quaternion>(*value) || std::holds_alternative<MatrixValue>(*value))))
				return true;
			for (const auto &[id, images] : context.ImageArrays)
				if (id == port && images) return true;
			return false;
		}
		bool ConditionKey(
			NodeContext &context, std::string_view port, SourceComparisonKey &key, bool number, uint64_t &work
		) {
			if (!SourceComparisonReadKey(context, port, key, !number)) return false;
			if (key.Text.size() > SourceComparisonWorkLimit - work)
				return context.Fail(Status::LimitExceeded, "condition comparison exceeds work limit", port);
			work += key.Text.size();
			const auto domain = context.InputDomain(port);
			// Float.getValue converts a typed Text producer with toNumber; raw Any text stays raw.
			if (number && key.Type == SourceComparisonKey::Kind::Text && domain &&
				domain->Type == ValueType::Text) {
				const auto converted = SourceRealTextNumber(key.Text);
				if (!converted)
					return context.Fail(
						Status::UnsupportedExecution,
						"condition typed Text real conversion requires a bounded finite source value",
						port
					);
				key.Number = *converted;
				key.Type = SourceComparisonKey::Kind::Number;
				key.Text = {};
			}
			return true;
		}
		bool ConditionCompare(
			NodeContext &context,
			std::string_view leftPort,
			std::string_view rightPort,
			bool number,
			double operation,
			bool &result
		) {
			SourceComparisonKey left, right;
			uint64_t work = 0;
			if (!ConditionKey(context, leftPort, left, number, work) ||
				!ConditionKey(context, rightPort, right, number, work))
				return false;
			if (operation == 0 || operation == 1) {
				if (!SourceComparisonEqual(context, left, right, result, leftPort)) return false;
				if (operation == 1) result = !result;
				return true;
			}
			std::optional<int> order;
			if (!SourceComparisonOrder(context, left, right, order, leftPort)) return false;
			if (!order) return true;
			if (operation == 2)
				result = *order < 0;
			else if (operation == 3)
				result = *order <= 0;
			else if (operation == 4)
				result = *order > 0;
			else if (operation == 5)
				result = *order >= 0;
			return true;
		}
		bool Condition(NodeContext &context) {
			bool result = false;
			const double mode = context.SourceChoice("eval_mode");
			if (mode == 0)
				result = context.Boolean("boolean");
			else if (mode == 1) {
				if (!ConditionArrayOperand(context, "check_value") &&
					!ConditionArrayOperand(context, "compare_to")) {
					const double operation = context.SourceChoice("condition");
					if (operation >= 0 && operation <= 5 && std::trunc(operation) == operation &&
						!ConditionCompare(context, "check_value", "compare_to", true, operation, result))
						return false;
				}
			} else if (mode == 2 && !ConditionCompare(context, "text_1", "text_2", false, 0, result))
				return false;
			const auto selected = result ? "true" : "false";
			if (const auto *image = context.Input(selected)) {
				auto *output = context.NewImage("result", image->Width, image->Height, image->Format);
				if (!output) return false;
				output->Pixels = image->Pixels;
				output->Hash = image->Hash;
			} else {
				const ImageArray *images = nullptr;
				for (const auto &[port, array] : context.ImageArrays)
					if (port == selected) images = array;
				if (images) {
					source_array::TreeCost cost;
					if (!source_array::ImageCost(*images, images->Items, cost, 1))
						return context.Fail(
							Status::LimitExceeded, "condition image array exceeds its budget", selected
						);
					auto charge = context.ReserveWorkspace(cost.Bytes, selected);
					if (!charge) return false;
					auto items = source_array::FromImages(*images, images->Items);
					if (!source_array::Publish(context, std::move(items), "result", ValueType::Image))
						return false;
				} else {
					const Value missing = int64_t{-4};
					const auto *value = context.Find(selected);
					if (!value) value = &missing;
					const auto bytes = ValueClonePayloadBytes(*value);
					if (!bytes || !context.ReserveOutput(*bytes, "result")) return false;
					context.SetValue("result", *value);
				}
			}
			context.SetValue("bool", result);
			return context.FailureCode == Status::Ok;
		}
	} // namespace
	std::span<const ExecutorEntry> SourceRoutingExecutors() {
		static constexpr ExecutorEntry entries[] = {
			{"pc.condition", Condition}, {"pc.transform_array", Tuple}, {"pc.rotation_random_data", Tuple}
		};
		return entries;
	}
} // namespace engine::imagegraph::detail
