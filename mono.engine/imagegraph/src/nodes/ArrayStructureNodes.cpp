// Source: Pixel Composer b69eca232217360cf1502ef0223523d818606652, node_array_*
// and array_functions.gml.
#include "ArraySource.hpp"
#include "Families.hpp"

#include <algorithm>
#include <array>
#include <limits>

namespace engine::imagegraph::detail {
	namespace {
		using namespace source_array;
		bool Structural(NodeContext &context) {
			const auto node = context.Authored.Type;
			const std::string_view inputPort =
				(node == "pc.array_flattern" || node == "pc.array_transpose") ? "array_in" : "array";
			const Value *value = context.Find(inputPort);
			const auto *array = value ? std::get_if<ArrayValue>(value) : nullptr;
			const size_t packedCount = value && !array ? PackedCount(*value) : 0;
			const ImageArray *images = nullptr;
			for (const auto &[id, candidate] : context.ImageArrays)
				if (id == inputPort) images = candidate;
			if (node == "pc.array_length") {
				const size_t count = images					  ? images->Items.size()
									 : !array				  ? packedCount
									 : !array->Items.empty()  ? array->Items.size()
									 : !array->Nested.empty() ? array->Nested.size()
															  : array->Elements.size();
				if (!context.ReserveOutput(std::string{}.capacity(), "size")) return false;
				context.SetValue("size", int64_t(count));
				return context.FailureCode == Status::Ok;
			}
			if (!array && !images && !packedCount)
				return context.Fail(Status::InvalidValue, "source array input is missing", inputPort);
			TreeCost inputCost;
			ValueType emptyType =
				array && array->ElementType != ValueType::Any ? array->ElementType : ValueType::Integer;
			if (array) {
				if (!ValidRuntimeValue(*value))
					return context.Fail(Status::InvalidValue, "source array input is invalid", inputPort);
				inputCost.Bytes = RetainedPayloadBytes(*array);
				inputCost.Nodes = array->Elements.size() + array->Nested.size();
				for (const auto &row : array->Nested)
					inputCost.Nodes += row.size();
				if (!array->Items.empty()) {
					inputCost = {};
					if (!MeasureSource(array->Items, inputCost))
						return context.Fail(
							Status::LimitExceeded, "source array input exceeds bounds", inputPort
						);
				} else {
					const auto addPacked = [&](const auto &row) {
						for (const auto &leaf : row)
							inputCost.Nodes += PackedCount(leaf);
					};
					addPacked(array->Elements);
					for (const auto &row : array->Nested)
						addPacked(row);
					inputCost.Bytes += inputCost.Nodes * sizeof(SourceArrayItem);
				}
			} else if (packedCount) {
				inputCost.Nodes = packedCount;
				inputCost.Bytes = packedCount * sizeof(SourceArrayItem);
			} else if (!ImageCost(*images, images->Items, inputCost, 1))
				return context.Fail(Status::InvalidValue, "source image array shape is invalid", inputPort);
			if (inputCost.Nodes > Limits::MaximumArrayElements)
				return context.Fail(
					Status::LimitExceeded, "source array node count exceeds bounds", inputPort
				);
			auto inputCharge = context.ReserveWorkspace(inputCost.Bytes, inputPort);
			if (!inputCharge) return false;
			Items input;
			if (array)
				input = FromValues(*array);
			else if (packedCount)
				input = std::move(std::get<Items>(SourceLeaf(*value).Data));
			else
				input = FromImages(*images, images->Items);
			const size_t count = input.size();
			size_t capacity = count;
			if (node == "pc.array_copy") {
				int64_t size = context.Integer("size");
				if (size < 0) size += int64_t(count);
				if (size > int64_t(Limits::MaximumArrayElements))
					return context.Fail(Status::LimitExceeded, "copy count exceeds bounds", "size");
				capacity = size > 0 ? size_t(size) : 0;
			} else if (node == "pc.array_partition") {
				const int64_t size = std::max<int64_t>(1, context.Integer("length"));
				if (size > int64_t(Limits::MaximumArrayElements))
					return context.Fail(Status::LimitExceeded, "partition size exceeds bounds", "length");
				capacity =
					context.Integer("type") == 0 ? (count + size_t(size) - 1) / size_t(size) : size_t(size);
			} else if (node == "pc.array_flattern")
				capacity = std::max<size_t>(1, inputCost.Nodes);
			else if (node == "pc.array_transpose") {
				// Transposed outer rows are columns, independent of recursive input item count.
				capacity = input.empty() ? 0 : Limits::MaximumArrayElements;
				for (const auto &row : input) {
					const auto *children = std::get_if<Items>(&row.Data);
					if (!children)
						return context.Fail(Status::InvalidValue, "transpose requires array rows", inputPort);
					capacity = std::min(capacity, children->size());
				}
			}
			TreeCost outputCost;
			if (node == "pc.array_partition" && (context.Integer("type") < 0 || context.Integer("type") > 1))
				return context.Fail(Status::InvalidValue, "partition type is invalid", "type");
			if (node == "pc.array_shift" &&
				(context.Integer("overflow") < 0 || context.Integer("overflow") > 2))
				return context.Fail(Status::InvalidValue, "shift overflow is invalid", "overflow");
			if (!OutputCost(context, input, capacity, outputCost))
				return context.Fail(Status::LimitExceeded, "source array output exceeds bounds", inputPort);
			auto candidateCharge =
				context.ReserveWorkspace(inputCost.Bytes + capacity * sizeof(SourceArrayItem), inputPort);
			if (!candidateCharge) return false;
			Items output;
			output.reserve(capacity);
			const auto zero = [] { return SourceArrayItem{ElementValue{double{0}}}; };
			std::string_view outputPort = "array";
			if (node == "pc.array_reverse")
				for (auto iterator = input.rbegin(); iterator != input.rend(); ++iterator)
					output.push_back(*iterator);
			else if (node == "pc.array_trim") {
				const uint64_t start = uint64_t(std::max<int64_t>(0, context.Integer("trim_start")));
				const uint64_t end = uint64_t(std::max<int64_t>(0, context.Integer("trim_end")));
				for (size_t index = size_t(std::min<uint64_t>(start, count));
					 index < count && end < count - index;
					 ++index)
					output.push_back(input[index]);
			} else if (node == "pc.array_copy") {
				int64_t start = context.Integer("starting_index");
				if (start < 0) start += int64_t(count);
				for (size_t index = 0; index < capacity; ++index) {
					const bool valid =
						start >= -int64_t(index) && start <= int64_t(count) - 1 - int64_t(index);
					output.push_back(valid ? input[size_t(start + int64_t(index))] : zero());
				}
			} else if (node == "pc.array_shift") {
				const int64_t overflow = context.Integer("overflow");
				if (overflow < 0 || overflow > 2)
					return context.Fail(Status::InvalidValue, "shift overflow is invalid", "overflow");
				const int64_t shift = context.Integer("shift");
				for (size_t index = 0; index < count; ++index) {
					if (overflow == 0) {
						const int64_t normalized = shift % int64_t(count);
						const int64_t target =
							(int64_t(index) - normalized + int64_t(count)) % int64_t(count);
						output.push_back(input[size_t(target)]);
					} else {
						const bool valid = shift <= int64_t(index) && shift > int64_t(index) - int64_t(count);
						if (valid)
							output.push_back(input[size_t(int64_t(index) - shift)]);
						else if (overflow == 1)
							output.push_back(zero());
					}
				}
			} else if (node == "pc.array_partition") {
				const int64_t mode = context.Integer("type");
				if (mode < 0 || mode > 1)
					return context.Fail(Status::InvalidValue, "partition type is invalid", "type");
				const size_t size = size_t(std::max<int64_t>(1, context.Integer("length")));
				double accumulated = 0;
				const double step = capacity ? double(count) / double(capacity) : 0;
				size_t start = 0;
				for (size_t index = 0; index < capacity; ++index) {
					size_t end = mode == 0 ? std::min(count, start + size) : size_t(accumulated += step);
					end = std::min(count, end);
					Items row;
					row.reserve(end - start);
					for (size_t member = start; member < end; ++member)
						row.push_back(input[member]);
					output.push_back({std::move(row)});
					start = end;
				}
			} else if (node == "pc.array_flattern") {
				outputPort = "flattened_array";
				SourceArrayItem root{std::move(input)};
				Spread(root, context.Integer("depth"), output);
			} else if (node == "pc.array_transpose") {
				outputPort = "transposed_array";
				if (!input.empty()) {
					size_t height = Limits::MaximumArrayElements;
					for (const auto &row : input) {
						const auto *children = std::get_if<Items>(&row.Data);
						if (!children)
							return context.Fail(
								Status::InvalidValue, "transpose requires array rows", inputPort
							);
						height = std::min(height, children->size());
					}
					for (size_t column = 0; column < height; ++column) {
						Items row;
						row.reserve(count);
						for (const auto &source : input)
							row.push_back(std::get<Items>(source.Data)[column]);
						output.push_back({std::move(row)});
					}
				}
			} else
				return context.Fail(Status::UnsupportedExecution, "source array operation is unavailable");
			return Publish(context, std::move(output), outputPort, emptyType);
		}
	} // namespace
	bool CollectSourceArray(NodeContext &context) {
		using namespace source_array;
		const int64_t type = context.Integer("type");
		if (type != 0)
			return context.Fail(
				Status::UnsupportedExecution,
				"general source array collector requires source Any inputs",
				"type"
			);
		const bool spread = context.Boolean("spread_array");
		TreeCost total;
		for (const auto &port : context.Authored.DynamicInputs) {
			const Image *image = context.Input(port.Id);
			const ImageArray *images = nullptr;
			for (const auto &[id, candidate] : context.ImageArrays)
				if (id == port.Id) images = candidate;
			if (image) {
				if (!ValidSurfaceLayout(*image, Limits::MaximumDimension, Limits::MaximumArrayBytes))
					return context.Fail(Status::InvalidValue, "general array image is invalid", port.Id);
				++total.Nodes;
				total.Bytes += sizeof(SourceArrayItem) + image->Pixels.size();
				continue;
			}
			if (images) {
				if (!ImageCost(*images, images->Items, total, 1))
					return context.Fail(
						Status::LimitExceeded, "general array image shape exceeds bounds", port.Id
					);
				if (!spread) {
					++total.Nodes;
					total.Bytes += sizeof(SourceArrayItem);
				}
				continue;
			}
			const Value *value = context.Find(port.Id);
			if (!value || !ValidRuntimeValue(*value))
				return context.Fail(Status::InvalidValue, "general array input is invalid", port.Id);
			if (const auto *array = std::get_if<ArrayValue>(value)) {
				if (!array->Items.empty()) {
					if (!MeasureSource(array->Items, total))
						return context.Fail(
							Status::LimitExceeded, "general array exceeds shape bounds", port.Id
						);
				} else {
					size_t nodes = array->Elements.size() + array->Nested.size();
					for (const auto &leaf : array->Elements)
						nodes += PackedCount(leaf);
					for (const auto &row : array->Nested) {
						nodes += row.size();
						for (const auto &leaf : row)
							nodes += PackedCount(leaf);
					}
					if (nodes > Limits::MaximumArrayElements - total.Nodes)
						return context.Fail(
							Status::LimitExceeded, "general array exceeds count bounds", port.Id
						);
					total.Nodes += nodes;
					total.Bytes += RetainedPayloadBytes(*array) + nodes * sizeof(SourceArrayItem);
				}
				if (!spread) {
					++total.Nodes;
					total.Bytes += sizeof(SourceArrayItem);
				}
			} else {
				const size_t packed = PackedCount(*value);
				total.Nodes += packed + 1;
				total.Bytes += (packed + 1) * sizeof(SourceArrayItem) + RetainedPayloadBytes(*value);
			}
			if (total.Nodes > Limits::MaximumArrayElements || total.Bytes > Limits::MaximumArrayBytes)
				return context.Fail(Status::LimitExceeded, "general array exceeds bounds", port.Id);
		}
		if (total.Nodes > Limits::MaximumArrayElements || total.Bytes > Limits::MaximumArrayBytes)
			return context.Fail(Status::LimitExceeded, "general array exceeds bounds", "array");
		auto charge = context.ReserveWorkspace(total.Bytes * 2, "array");
		if (!charge) return false;
		Items output;
		output.reserve(total.Nodes);
		for (const auto &port : context.Authored.DynamicInputs) {
			if (const Image *image = context.Input(port.Id)) {
				output.push_back({*image});
				continue;
			}
			const ImageArray *images = nullptr;
			for (const auto &[id, candidate] : context.ImageArrays)
				if (id == port.Id) images = candidate;
			if (images) {
				Items members = FromImages(*images, images->Items);
				if (spread)
					for (auto &item : members)
						output.push_back(std::move(item));
				else
					output.push_back({std::move(members)});
				continue;
			}
			const Value *value = context.Find(port.Id);
			if (const auto *array = std::get_if<ArrayValue>(value)) {
				Items members = FromValues(*array);
				if (spread)
					for (auto &item : members)
						output.push_back(std::move(item));
				else
					output.push_back({std::move(members)});
			} else {
				SourceArrayItem item = SourceLeaf(*value);
				if (spread && std::holds_alternative<Items>(item.Data))
					for (auto &member : std::get<Items>(item.Data))
						output.push_back(std::move(member));
				else
					output.push_back(std::move(item));
			}
		}
		return Publish(context, std::move(output), "array", ValueType::Integer);
	}
	std::span<const ExecutorEntry> ArrayStructureExecutors() {
		static constexpr std::array ENTRIES{
			ExecutorEntry{"pc.array_reverse", Structural, true},
			ExecutorEntry{"pc.array_copy", Structural, true},
			ExecutorEntry{"pc.array_trim", Structural, true},
			ExecutorEntry{"pc.array_shift", Structural, true},
			ExecutorEntry{"pc.array_partition", Structural, true},
			ExecutorEntry{"pc.array_flattern", Structural, true},
			ExecutorEntry{"pc.array_transpose", Structural, true},
			ExecutorEntry{"pc.array_length", Structural, true}
		};
		return ENTRIES;
	}
} // namespace engine::imagegraph::detail
