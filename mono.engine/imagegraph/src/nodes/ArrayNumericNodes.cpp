// Source: Pixel Composer b69eca232217360cf1502ef0223523d818606652 node_array_*.
#include "../ValuePayload.hpp"
#include "ArraySource.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>

namespace engine::imagegraph::detail {
	namespace {
		bool Numeric(const ElementValue &leaf, double &number) {
			if (const auto *value = std::get_if<double>(&leaf))
				number = *value;
			else if (const auto *value = std::get_if<int64_t>(&leaf))
				number = double(*value);
			else
				return false;
			return std::isfinite(number);
		}
		struct ArrayInput {
			ArrayValue Normalized;
			std::optional<AllocationReservation> Charge;
			const ArrayValue *Read(NodeContext &context, std::string_view port) {
				const Value *value = context.Find(port);
				const auto *array = value ? std::get_if<ArrayValue>(value) : nullptr;
				if (!array || !ValidRuntimeValue(*value)) {
					context.Fail(Status::InvalidValue, "array input is invalid", port);
					return nullptr;
				}
				if (array->Items.empty()) return array;
				Charge = context.ReserveWorkspace(RetainedPayloadBytes(*array), port);
				if (!Charge) return nullptr;
				std::optional<ValueType> common;
				bool uniform = true;
				const auto copyRow = [&](const auto &items, auto &row) {
					row.reserve(items.size());
					for (const auto &item : items) {
						const auto *leaf = std::get_if<ElementValue>(&item.Data);
						if (!leaf)
							return context.Fail(
								Status::InvalidValue, "numeric array requires flat scalar rows", port
							);
						const auto type =
							std::visit([](const auto &data) { return PayloadType(data); }, *leaf);
						if (common && *common != type) uniform = false;
						common = type;
						row.push_back(*leaf);
					}
					return true;
				};
				const bool nested =
					std::holds_alternative<std::vector<SourceArrayItem>>(array->Items.front().Data);
				if (nested) {
					Normalized.Nested.reserve(array->Items.size());
					for (const auto &item : array->Items) {
						const auto *row = std::get_if<std::vector<SourceArrayItem>>(&item.Data);
						if (!row) {
							context.Fail(
								Status::InvalidValue, "numeric array rows have inconsistent shape", port
							);
							return nullptr;
						}
						if (!copyRow(*row, Normalized.Nested.emplace_back())) return nullptr;
					}
				} else if (!copyRow(array->Items, Normalized.Elements))
					return nullptr;
				Normalized.ElementType = uniform ? common.value_or(ValueType::Scalar) : ValueType::Any;
				return &Normalized;
			}
		};

		bool Allocate(NodeContext &context, size_t count, std::string_view port, ArrayValue &out) {
			if (count > Limits::MaximumArrayElements)
				return context.Fail(Status::LimitExceeded, "array output exceeds element budget", port);
			if (!context.ReserveOutput(count * sizeof(ElementValue) + std::string{}.capacity(), port))
				return false;
			out.ElementType = ValueType::Scalar;
			out.Elements.reserve(count);
			return true;
		}
		bool Cumulative(NodeContext &context) {
			const Value *source = context.Find("array_in");
			const auto *input = source ? std::get_if<ArrayValue>(source) : nullptr;
			if (!input || !ValidRuntimeValue(*source))
				return context.Fail(Status::InvalidValue, "cumulative input is invalid", "array_in");
			if (!input) return false;
			ArrayValue output;
			if (!Allocate(
					context,
					input->Items.empty() ? input->Elements.size() + input->Nested.size()
										 : input->Items.size(),
					"cumulative_array",
					output
				))
				return false;
			double running = context.Scalar("start");
			const bool include = context.Boolean("include_current", true);
			const size_t count = !input->Items.empty()	 ? input->Items.size()
								 : input->Nested.empty() ? input->Elements.size()
														 : input->Nested.size();
			for (size_t index = 0; index < count; ++index) {
				if (!include) output.Elements.emplace_back(running);
				double number = 0;
				if (!input->Items.empty()) {
					const auto *leaf = std::get_if<ElementValue>(&input->Items[index].Data);
					if (leaf && Numeric(*leaf, number)) running += number;
				} else if (input->Nested.empty() && Numeric(input->Elements[index], number))
					running += number;
				if (!std::isfinite(running))
					return context.Fail(Status::InvalidValue, "cumulative sum is nonfinite", "array_in");
				if (include) output.Elements.emplace_back(running);
			}
			context.SetValue("cumulative_array", std::move(output));
			return context.FailureCode == Status::Ok;
		}
		bool Range(NodeContext &context) {
			const double type = context.SourceChoice("type");
			const double start = context.Scalar("start"), end = context.Scalar("end", 10),
						 step = std::abs(context.Scalar("step", 1));
			if (context.FailureCode != Status::Ok) return false;
			if (!std::isfinite(start) || !std::isfinite(end) || !std::isfinite(step))
				return context.Fail(Status::InvalidValue, "range inputs must be finite", "step");
			if (type != 0 && type != 1)
				return context.Fail(Status::InvalidValue, "range type is invalid", "type");
			const bool repeat = type == 1 || start == end;
			if (!repeat && step == 0)
				return context.Fail(Status::InvalidValue, "range step must be nonzero", "step");
			const double directedStep = end >= start ? step : -step;
			const double countValue =
				repeat ? std::trunc(step)
					   : std::floor(
							 std::abs((end - start + double(context.Boolean("inclusive"))) / directedStep)
						 );
			if (!std::isfinite(countValue) || countValue > Limits::MaximumArrayElements)
				return context.Fail(Status::LimitExceeded, "range output exceeds element budget", "step");
			const size_t count = size_t(std::max(0.0, countValue));
			ArrayValue output;
			if (!Allocate(context, count, "array", output)) return false;
			for (size_t index = 0; index < count; ++index) {
				const double value = repeat ? start : start + double(index) * directedStep;
				if (!std::isfinite(value))
					return context.Fail(Status::InvalidValue, "range value is nonfinite", "step");
				output.Elements.emplace_back(value);
			}
			context.SetValue("array", std::move(output));
			return context.FailureCode == Status::Ok;
		}
		std::optional<long double> SortKey(const ElementValue &leaf) {
			if (const auto *value = std::get_if<double>(&leaf)) return *value;
			if (const auto *value = std::get_if<int64_t>(&leaf)) return *value;
			if (const auto *value = std::get_if<Colour>(&leaf))
				return uint32_t(value->Red) | (uint32_t(value->Green) << 8) | (uint32_t(value->Blue) << 16) |
					   (uint32_t(value->Alpha) << 24);
			return std::nullopt;
		}
		bool Sort(NodeContext &context) {
			ArrayInput source;
			const auto *input = source.Read(context, "array_in");
			if (!input) return false;
			if (!input->Nested.empty())
				return context.Fail(Status::InvalidValue, "sort requires scalar array leaves", "array_in");
			for (const auto &leaf : input->Elements)
				if (!SortKey(leaf))
					return context.Fail(Status::InvalidValue, "sort requires numbers or colors", "array_in");
			const double order = context.SourceChoice("order");
			if (context.FailureCode != Status::Ok) return false;
			auto charge = context.ReserveWorkspace(input->Elements.size() * sizeof(size_t), "array_in");
			if (!charge) return false;
			std::vector<size_t> indexes(input->Elements.size());
			std::iota(indexes.begin(), indexes.end(), 0);
			std::sort(indexes.begin(), indexes.end(), [&](size_t a, size_t b) {
				const auto x = *SortKey(input->Elements[a]), y = *SortKey(input->Elements[b]);
				return x == y ? a < b : order == 0 ? x < y : x > y;
			});
			ArrayValue values, positions;
			if (!Allocate(context, indexes.size(), "sorted_array", values) ||
				!Allocate(context, indexes.size(), "sorted_index", positions))
				return false;
			values.ElementType = input->ElementType;
			positions.ElementType = ValueType::Integer;
			for (size_t index : indexes) {
				values.Elements.push_back(input->Elements[index]);
				positions.Elements.emplace_back(int64_t(index));
			}
			if (input->ElementType == ValueType::Any) {
				source_array::Items items;
				items.reserve(values.Elements.size());
				for (auto &leaf : values.Elements)
					items.push_back({std::move(leaf)});
				if (!source_array::Publish(context, std::move(items), "sorted_array", ValueType::Scalar))
					return false;
			} else
				context.SetValue("sorted_array", std::move(values));
			context.SetValue("sorted_index", std::move(positions));
			return context.FailureCode == Status::Ok;
		}
		bool Composite(NodeContext &context) {
			ArrayInput source, composition;
			const auto *input = source.Read(context, "array"),
					   *compose = composition.Read(context, "compose");
			if (!input || !compose) return false;
			if ((input->Elements.empty() && input->Nested.empty()) || compose->Elements.empty() ||
				!compose->Nested.empty())
				return context.Fail(
					Status::InvalidValue,
					"composite requires nonempty input and flat composition arrays",
					"compose"
				);
			size_t count = input->Elements.size();
			for (const auto &row : input->Nested)
				count += row.size();
			const uint64_t nodes = uint64_t(count) * (compose->Elements.size() + 1) + input->Nested.size();
			if (nodes > Limits::MaximumArrayElements)
				return context.Fail(
					Status::LimitExceeded, "composite output exceeds element budget", "array"
				);
			for (const auto &leaf : compose->Elements) {
				double number;
				if (!Numeric(leaf, number))
					return context.Fail(
						Status::InvalidValue, "composition leaves must be numeric", "compose"
					);
			}
			auto charge = context.ReserveWorkspace(nodes * sizeof(SourceArrayItem), "array");
			if (!charge) return false;
			using source_array::Items;
			const auto process = [&](const auto &row, Items &output) {
				output.reserve(row.size());
				for (const auto &leaf : row) {
					double number;
					if (!Numeric(leaf, number))
						return context.Fail(
							Status::InvalidValue, "composite leaves must be numeric", "array"
						);
					Items products;
					products.reserve(compose->Elements.size());
					for (const auto &factor : compose->Elements) {
						double multiplier;
						Numeric(factor, multiplier);
						const double product = number * multiplier;
						if (!std::isfinite(product))
							return context.Fail(
								Status::InvalidValue, "composite product is nonfinite", "array"
							);
						products.push_back({ElementValue{product}});
					}
					output.push_back({std::move(products)});
				}
				return true;
			};
			Items output;
			if (input->Nested.empty()) {
				if (!process(input->Elements, output)) return false;
			} else {
				output.reserve(input->Nested.size());
				for (const auto &row : input->Nested) {
					Items products;
					if (!process(row, products)) return false;
					output.push_back({std::move(products)});
				}
			}
			return source_array::Publish(context, std::move(output), "array", ValueType::Scalar);
		}

		bool Convolute(NodeContext &context) {
			ArrayInput source, kernelInput;
			const auto *input = source.Read(context, "array"), *kernel = kernelInput.Read(context, "kernel");
			if (!input || !kernel) return false;
			if (input->Elements.empty() && input->Nested.empty())
				return context.Fail(Status::InvalidValue, "convolution requires a nonempty array", "array");
			if (kernel->Elements.empty() || !kernel->Nested.empty())
				return context.Fail(
					Status::InvalidValue, "convolution requires a nonempty flat kernel", "kernel"
				);
			const double boundary = context.SourceChoice("boundary");
			if (boundary != 0 && boundary != 1 && boundary != 2)
				return context.Fail(Status::InvalidValue, "convolution boundary is invalid", "boundary");
			if (boundary == 2) {
				const auto tooShort = [&](const auto &row) { return row.size() < kernel->Elements.size(); };
				if (input->Nested.empty() ? tooShort(input->Elements)
										  : std::any_of(input->Nested.begin(), input->Nested.end(), tooShort))
					return context.Fail(
						Status::InvalidValue, "source Skip boundary creates a negative array length", "kernel"
					);
			}
			for (const auto &leaf : kernel->Elements) {
				double value;
				if (!Numeric(leaf, value))
					return context.Fail(Status::InvalidValue, "convolution kernel must be numeric", "kernel");
			}
			size_t count = 0;
			const auto length = [&](size_t n) {
				return boundary == 2 ? n >= kernel->Elements.size() ? n - kernel->Elements.size() + 1 : 0 : n;
			};
			if (input->Nested.empty())
				count = length(input->Elements.size());
			else
				for (const auto &row : input->Nested)
					count += length(row.size());
			if (count > Limits::MaximumArrayElements)
				return context.Fail(
					Status::LimitExceeded, "convolution output exceeds element budget", "array"
				);
			if (!context.ReserveOutput(
					count * sizeof(ElementValue) + input->Nested.size() * sizeof(std::vector<ElementValue>) +
						std::string{}.capacity(),
					"array"
				))
				return false;
			ArrayValue output{ValueType::Scalar, {}};
			const auto process = [&](const std::vector<ElementValue> &row, std::vector<ElementValue> &out) {
				for (const auto &leaf : row) {
					double value;
					if (!Numeric(leaf, value))
						return context.Fail(
							Status::InvalidValue, "convolution array must be numeric", "array"
						);
				}
				const int64_t size = int64_t(row.size()), center = int64_t((kernel->Elements.size() - 1) / 2);
				out.reserve(length(row.size()));
				for (size_t index = 0; index < length(row.size()); ++index) {
					double sum = 0;
					for (size_t k = 0; k < kernel->Elements.size(); ++k) {
						int64_t at = int64_t(index + k) - (boundary == 2 ? 0 : center);
						if (at < 0 || at >= size) {
							if (boundary != 1) continue;
							at = (at + size) % size;
							if (at < 0)
								return context.Fail(
									Status::InvalidValue,
									"source wrapped convolution index is negative",
									"kernel"
								);
						}
						double a, b;
						Numeric(row[size_t(at)], a);
						Numeric(kernel->Elements[k], b);
						sum += a * b;
					}
					if (!std::isfinite(sum))
						return context.Fail(Status::InvalidValue, "convolution result is nonfinite", "array");
					out.emplace_back(sum);
				}
				return true;
			};
			if (input->Nested.empty()) {
				if (!process(input->Elements, output.Elements)) return false;
			} else {
				output.Nested.reserve(input->Nested.size());
				for (const auto &row : input->Nested) {
					output.Nested.emplace_back();
					if (!process(row, output.Nested.back())) return false;
				}
			}
			context.SetValue("array", std::move(output));
			return context.FailureCode == Status::Ok;
		}
	}
	std::span<const ExecutorEntry> ArrayNumericExecutors() {
		static constexpr std::array entries{
			ExecutorEntry{"pc.array_cumulative", Cumulative, true},
			ExecutorEntry{"pc.array_range", Range, true},
			ExecutorEntry{"pc.array_sort", Sort, true},
			ExecutorEntry{"pc.array_convolute", Convolute, true},
			ExecutorEntry{"pc.array_composite", Composite, true}
		};
		return entries;
	}
}
