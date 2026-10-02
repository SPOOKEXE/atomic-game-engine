#include "ArraySource.hpp"
#include "Families.hpp"

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
		bool Condition(NodeContext &context) {
			bool result = false;
			const double mode = context.SourceChoice("eval_mode");
			if (mode == 0)
				result = context.Boolean("boolean");
			else if (mode == 1) {
				const auto *a = context.Find("check_value"), *b = context.Find("compare_to");
				if (!(a && std::holds_alternative<ArrayValue>(*a)) &&
					!(b && std::holds_alternative<ArrayValue>(*b))) {
					const double x = context.Scalar("check_value"), y = context.Scalar("compare_to");
					const double condition = context.SourceChoice("condition");
					if (condition == 0)
						result = x == y;
					else if (condition == 1)
						result = x != y;
					else if (condition == 2)
						result = x < y;
					else if (condition == 3)
						result = x <= y;
					else if (condition == 4)
						result = x > y;
					else if (condition == 5)
						result = x >= y;
				}
			} else if (mode == 2) {
				const auto *a = context.Find("text_1"), *b = context.Find("text_2");
				const auto *x = a ? std::get_if<std::string>(a) : nullptr,
						   *y = b ? std::get_if<std::string>(b) : nullptr;
				result = (x ? std::string_view(*x) : std::string_view{}) ==
						 (y ? std::string_view(*y) : std::string_view{});
			}
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
