#pragma once
// Shared source-shape conversion, admission and lossless output normalization.
#include "../ValuePayload.hpp"
#include "Families.hpp"

#include <algorithm>
#include <limits>
namespace engine::imagegraph::detail::source_array {
	using Items = std::vector<SourceArrayItem>;

	struct TreeCost {
		size_t Nodes = 0;
		uint64_t Bytes = 0;
	};
	inline bool Measure(const Items &items, TreeCost &cost, size_t depth = 1, bool logicalBytes = false) {
		if (depth > Limits::MaximumArrayDepth || items.size() > Limits::MaximumArrayElements - cost.Nodes)
			return false;
		cost.Nodes += items.size();
		cost.Bytes += items.size() * sizeof(SourceArrayItem);
		for (const auto &item : items) {
			if (const auto *children = std::get_if<Items>(&item.Data)) {
				if (!Measure(*children, cost, depth + 1, logicalBytes)) return false;
			} else
				cost.Bytes += logicalBytes ? PayloadOwnedBytes(item) : RetainedPayloadBytes(item);
			if (cost.Bytes > Limits::MaximumArrayBytes) return false;
		}
		return true;
	}
	template <class Variant> size_t PackedCount(const Variant &leaf) {
		return std::visit(
			[](const auto &data) -> size_t {
				using T = std::decay_t<decltype(data)>;
				if constexpr (std::is_same_v<T, Vector2>)
					return 2;
				else if constexpr (std::is_same_v<T, Vector3>)
					return 3;
				else if constexpr (std::is_same_v<T, Vector4> || std::is_same_v<T, Quaternion>)
					return 4;
				else if constexpr (std::is_same_v<T, Area>)
					return 6;
				else if constexpr (std::is_same_v<T, Curve>)
					return 6 + data.Anchors.size() * 6;
				else
					return 0;
			},
			leaf
		);
	}
	template <class Variant> SourceArrayItem SourceLeaf(const Variant &leaf) {
		const size_t count = PackedCount(leaf);
		if (!count)
			return std::visit(
				[](const auto &data) -> SourceArrayItem {
					using T = std::decay_t<decltype(data)>;
					if constexpr (std::is_constructible_v<ElementValue, T>)
						return {ElementValue{data}};
					else
						return {Items{}};
				},
				leaf
			);
		Items components;
		components.reserve(count);
		const auto append = [&](double value) { components.push_back({ElementValue{value}}); };
		std::visit(
			[&](const auto &data) {
				using T = std::decay_t<decltype(data)>;
				if constexpr (std::is_same_v<T, Vector2> || std::is_same_v<T, Vector3> ||
							  std::is_same_v<T, Vector4> || std::is_same_v<T, Quaternion>) {
					append(data.X);
					append(data.Y);
					if constexpr (!std::is_same_v<T, Vector2>) append(data.Z);
					if constexpr (std::is_same_v<T, Vector4> || std::is_same_v<T, Quaternion>) append(data.W);
				} else if constexpr (std::is_same_v<T, Area>) {
					append(data.CenterX);
					append(data.CenterY);
					append(data.HalfWidth);
					append(data.HalfHeight);
					append(data.Shape);
					append(data.Mode);
				} else if constexpr (std::is_same_v<T, Curve>) {
					for (const double field : data.Header)
						append(field);
					for (const auto &anchor : data.Anchors)
						for (const double field : anchor)
							append(field);
				}
			},
			leaf
		);
		return {std::move(components)};
	}
	inline bool MeasureSource(const Items &items, TreeCost &cost, size_t depth = 1) {
		if (depth > Limits::MaximumArrayDepth || items.size() > Limits::MaximumArrayElements - cost.Nodes)
			return false;
		cost.Nodes += items.size();
		cost.Bytes += items.size() * sizeof(SourceArrayItem);
		for (const auto &entry : items) {
			if (const auto *children = std::get_if<Items>(&entry.Data)) {
				if (!MeasureSource(*children, cost, depth + 1)) return false;
			} else if (const auto *leaf = std::get_if<ElementValue>(&entry.Data);
					   leaf && PackedCount(*leaf)) {
				const size_t count = PackedCount(*leaf);
				if (depth == Limits::MaximumArrayDepth || count > Limits::MaximumArrayElements - cost.Nodes)
					return false;
				cost.Nodes += count;
				cost.Bytes += count * sizeof(SourceArrayItem);
			} else
				cost.Bytes += RetainedPayloadBytes(entry);
			if (cost.Bytes > Limits::MaximumArrayBytes) return false;
		}
		return true;
	}
	inline Items SourceItems(const Items &items) {
		Items result;
		result.reserve(items.size());
		for (const auto &entry : items) {
			if (const auto *children = std::get_if<Items>(&entry.Data))
				result.push_back({SourceItems(*children)});
			else if (const auto *leaf = std::get_if<ElementValue>(&entry.Data))
				result.push_back(SourceLeaf(*leaf));
			else
				result.push_back(entry);
		}
		return result;
	}
	inline Items FromValues(const ArrayValue &array) {
		if (!array.Items.empty()) return SourceItems(array.Items);
		Items items;
		if (array.Nested.empty()) {
			items.reserve(array.Elements.size());
			for (const auto &leaf : array.Elements)
				items.push_back(SourceLeaf(leaf));
		} else {
			items.reserve(array.Nested.size());
			for (const auto &row : array.Nested) {
				Items children;
				children.reserve(row.size());
				for (const auto &leaf : row)
					children.push_back(SourceLeaf(leaf));
				items.push_back({std::move(children)});
			}
		}
		return items;
	}
	inline bool ImageCost(
		const ImageArray &array, const std::vector<ImageArrayItem> &items, TreeCost &cost, size_t depth
	) {
		if (depth > Limits::MaximumArrayDepth || items.size() > Limits::MaximumArrayElements - cost.Nodes)
			return false;
		cost.Nodes += items.size();
		cost.Bytes += items.size() * sizeof(SourceArrayItem);
		for (const auto &item : items) {
			if (const auto *index = std::get_if<size_t>(&item.Data)) {
				if (*index >= array.Images.size()) return false;
				const auto &image = array.Images[*index];
				if (!ValidSurfaceLayout(image, Limits::MaximumDimension, Limits::MaximumArrayBytes))
					return false;
				cost.Bytes += image.Pixels.size();
			} else if (!ImageCost(array, std::get<std::vector<ImageArrayItem>>(item.Data), cost, depth + 1))
				return false;
			if (cost.Bytes > Limits::MaximumArrayBytes) return false;
		}
		return true;
	}
	inline Items FromImages(const ImageArray &array, const std::vector<ImageArrayItem> &items) {
		Items result;
		result.reserve(items.size());
		for (const auto &item : items) {
			if (const auto *index = std::get_if<size_t>(&item.Data))
				result.push_back({array.Images[*index]});
			else
				result.push_back({FromImages(array, std::get<std::vector<ImageArrayItem>>(item.Data))});
		}
		return result;
	}
	// The source follows the first member only, including irregular arrays.
	inline size_t FirstDepth(const SourceArrayItem &item) {
		const auto *children = std::get_if<Items>(&item.Data);
		if (!children) return 0;
		if (children->empty()) return 1;
		const auto *first = std::get_if<Items>(&children->front().Data);
		return first && !first->empty() ? 1 + FirstDepth(children->front()) : 1;
	}
	inline void Spread(const SourceArrayItem &item, int64_t depth, Items &output) {
		if (int64_t(FirstDepth(item)) == depth) {
			output.push_back(item);
			return;
		}
		if (const auto *children = std::get_if<Items>(&item.Data))
			for (const auto &child : *children)
				Spread(child, depth, output);
	}
	inline bool MeasureItem(const SourceArrayItem &item, TreeCost &cost, size_t depth = 1) {
		if (depth > Limits::MaximumArrayDepth || cost.Nodes == Limits::MaximumArrayElements) return false;
		++cost.Nodes;
		cost.Bytes += sizeof(SourceArrayItem);
		if (const auto *children = std::get_if<Items>(&item.Data)) {
			if (depth == Limits::MaximumArrayDepth) return false;
			for (const auto &child : *children)
				if (!MeasureItem(child, cost, depth + 1)) return false;
		} else
			cost.Bytes += RetainedPayloadBytes(item);
		return cost.Bytes <= Limits::MaximumArrayBytes;
	}
	inline bool MeasureWrapper(TreeCost &cost) {
		if (cost.Nodes == Limits::MaximumArrayElements) return false;
		++cost.Nodes;
		cost.Bytes += sizeof(SourceArrayItem);
		return cost.Bytes <= Limits::MaximumArrayBytes;
	}
	inline bool MeasureSpread(const SourceArrayItem &item, int64_t depth, TreeCost &cost) {
		if (int64_t(FirstDepth(item)) == depth) return MeasureItem(item, cost);
		if (const auto *children = std::get_if<Items>(&item.Data))
			for (const auto &child : *children)
				if (!MeasureSpread(child, depth, cost)) return false;
		return true;
	}
	// Count selected subtrees and new wrappers before constructing the output.
	inline bool OutputCost(NodeContext &context, const Items &input, size_t capacity, TreeCost &cost) {
		const auto node = context.Authored.Type;
		const size_t count = input.size();
		if (node == "pc.array_partition") {
			const int64_t mode = context.Integer("type");
			if (mode < 0 || mode > 1) return false;
			const size_t size = size_t(std::max<int64_t>(1, context.Integer("length")));
			double accumulated = 0;
			const double step = capacity ? double(count) / double(capacity) : 0;
			size_t start = 0;
			for (size_t index = 0; index < capacity; ++index) {
				if (!MeasureWrapper(cost)) return false;
				const size_t end = std::min(count, mode == 0 ? start + size : size_t(accumulated += step));
				for (size_t member = start; member < end; ++member)
					if (!MeasureItem(input[member], cost, 2)) return false;
				start = end;
			}
			return true;
		}
		if (node == "pc.array_transpose") {
			if (input.empty()) return true;
			size_t height = Limits::MaximumArrayElements;
			for (const auto &row : input) {
				const auto *children = std::get_if<Items>(&row.Data);
				if (!children) return false;
				height = std::min(height, children->size());
			}
			for (size_t column = 0; column < height; ++column) {
				if (!MeasureWrapper(cost)) return false;
				for (const auto &row : input)
					if (!MeasureItem(std::get<Items>(row.Data)[column], cost, 2)) return false;
			}
			return true;
		}
		if (node == "pc.array_flattern") {
			const auto *first = input.empty() ? nullptr : std::get_if<Items>(&input.front().Data);
			const size_t rootDepth = first && !first->empty() ? 1 + FirstDepth(input.front()) : 1;
			const int64_t depth = context.Integer("depth");
			if (int64_t(rootDepth) == depth) {
				if (!MeasureWrapper(cost)) return false;
				for (const auto &item : input)
					if (!MeasureItem(item, cost, 2)) return false;
			} else
				for (const auto &item : input)
					if (!MeasureSpread(item, depth, cost)) return false;
			return true;
		}
		if (node == "pc.array_copy") {
			int64_t start = context.Integer("starting_index");
			if (start < 0) start += int64_t(count);
			const SourceArrayItem zero{ElementValue{double{0}}};
			for (size_t index = 0; index < capacity; ++index) {
				const bool valid = start >= -int64_t(index) && start <= int64_t(count) - 1 - int64_t(index);
				if (!MeasureItem(valid ? input[size_t(start + int64_t(index))] : zero, cost)) return false;
			}
			return true;
		}
		if (node == "pc.array_trim") {
			const uint64_t start = uint64_t(std::max<int64_t>(0, context.Integer("trim_start")));
			const uint64_t end = uint64_t(std::max<int64_t>(0, context.Integer("trim_end")));
			for (size_t index = size_t(std::min<uint64_t>(start, count));
				 index < count && end < count - index;
				 ++index)
				if (!MeasureItem(input[index], cost)) return false;
			return true;
		}
		if (node == "pc.array_shift") {
			const int64_t mode = context.Integer("overflow");
			if (mode == 0) return Measure(input, cost);
			const int64_t shift = context.Integer("shift");
			const SourceArrayItem zero{ElementValue{double{0}}};
			for (size_t index = 0; index < count; ++index) {
				const bool valid = shift <= int64_t(index) && shift > int64_t(index) - int64_t(count);
				if (valid) {
					if (!MeasureItem(input[size_t(int64_t(index) - shift)], cost)) return false;
				} else if (mode == 1 && !MeasureItem(zero, cost))
					return false;
			}
			return true;
		}
		return Measure(input, cost);
	}
	inline bool AllImages(const Items &items) {
		for (const auto &item : items) {
			if (std::holds_alternative<Image>(item.Data)) continue;
			const auto *children = std::get_if<Items>(&item.Data);
			if (!children || !AllImages(*children)) return false;
		}
		return true;
	}
	inline void ExportImages(Items &items, ImageArray &output, std::vector<ImageArrayItem> &shape) {
		shape.reserve(items.size());
		for (auto &item : items) {
			if (auto *image = std::get_if<Image>(&item.Data)) {
				shape.push_back({output.Images.size()});
				output.Images.push_back(std::move(*image));
			} else {
				std::vector<ImageArrayItem> children;
				ExportImages(std::get<Items>(item.Data), output, children);
				shape.push_back({std::move(children)});
			}
		}
	}
	inline bool Publish(
		NodeContext &context,
		Items &&items,
		std::string_view port,
		ValueType emptyType,
		bool normalizeImages = true,
		bool logicalBytes = false
	) {
		TreeCost cost;
		if (!Measure(items, cost, 1, logicalBytes))
			return context.Fail(Status::LimitExceeded, "source array shape exceeds its budget", port);
		// Normalization coexists with candidate storage. Admission covers all shape
		// and leaf buffers.
		uint64_t retained = items.capacity() * sizeof(SourceArrayItem);
		for (const auto &item : items)
			retained += RetainedPayloadBytes(item);
		const uint64_t bytes = retained +
							   cost.Nodes * (sizeof(ElementValue) + sizeof(Image) + sizeof(ImageArrayItem) +
											 sizeof(std::vector<ElementValue>)) +
							   std::string{}.capacity();
		if (!context.ReserveOutput(bytes, port)) return false;
		bool legacyImages = normalizeImages && !items.empty() && AllImages(items);
		for (const auto &item : items)
			if (const auto *children = std::get_if<Items>(&item.Data))
				for (const auto &child : *children)
					legacyImages = legacyImages && std::holds_alternative<Image>(child.Data);
		for (const auto &[id, original] : context.ProcessorOriginalValues)
			if ((id == "shift" || id == "overflow") && original &&
				std::holds_alternative<ArrayValue>(*original))
				legacyImages = false;
		for (const auto &[id, original] : context.Values)
			if ((id == "shift" || id == "overflow") && std::holds_alternative<ArrayValue>(original))
				legacyImages = false;
		if (legacyImages) {
			ImageArray result;
			result.Images.reserve(cost.Nodes);
			ExportImages(items, result, result.Items);
			context.OutputImageArrays.emplace_back(port, std::move(result));
			return true;
		}
		std::optional<ValueType> type;
		const auto rowType = [&](const Items &row) {
			for (const auto &entry : row) {
				const auto *leaf = std::get_if<ElementValue>(&entry.Data);
				if (!leaf) return false;
				const ValueType current =
					std::visit([](const auto &data) { return PayloadType(data); }, *leaf);
				if (type && *type != current) return false;
				type = current;
			}
			return true;
		};
		ArrayValue result{emptyType, {}};
		if (rowType(items)) {
			result.ElementType = type.value_or(emptyType);
			result.Elements.reserve(items.size());
			for (auto &entry : items)
				result.Elements.push_back(std::move(std::get<ElementValue>(entry.Data)));
		} else {
			type.reset();
			bool rows = !items.empty();
			for (const auto &entry : items) {
				const auto *row = std::get_if<Items>(&entry.Data);
				if (!row || !rowType(*row)) {
					rows = false;
					break;
				}
			}
			if (rows) {
				result.ElementType = type.value_or(emptyType);
				result.Nested.reserve(items.size());
				for (auto &entry : items) {
					auto &row = std::get<Items>(entry.Data);
					std::vector<ElementValue> values;
					values.reserve(row.size());
					for (auto &leaf : row)
						values.push_back(std::move(std::get<ElementValue>(leaf.Data)));
					result.Nested.push_back(std::move(values));
				}
			} else {
				result.ElementType = ValueType::Any;
				result.Items = std::move(items);
			}
		}
		context.SetValue(port, std::move(result));
		return context.FailureCode == Status::Ok;
	}
}
