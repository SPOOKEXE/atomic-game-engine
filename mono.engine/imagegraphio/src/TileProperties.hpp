#pragma once
#include "ImportBudget.hpp"

#include <engine/imagegraph/Document.hpp>

#include <cmath>
#include <nlohmann/json.hpp>
namespace engine::imagegraphio::detail {
	// Authored tile records are field-pair arrays. Their marker distinguishes source
	// objects from ordinary coordinate arrays without admitting authored Struct values.
	inline bool TileAttributeItem(const imagegraph::Value &value, imagegraph::SourceArrayItem &item) {
		bool valid = true;
		std::visit(
			[&](const auto &data) {
				using T = std::decay_t<decltype(data)>;
				if constexpr (std::is_same_v<T, imagegraph::ArrayValue>)
					item.Data = data.Items;
				else if constexpr (std::is_same_v<T, imagegraph::UndefinedValue>)
					valid = false;
				else
					item.Data = imagegraph::ElementValue{data};
			},
			value
		);
		return valid;
	}
	inline bool DecodeTileAttribute(
		const nlohmann::json &source,
		imagegraph::Value &result,
		ImportBudget *budget,
		size_t depth,
		size_t &elements
	) {
		using namespace imagegraph;
		if (depth > Limits::MaximumArrayDepth || elements >= Limits::MaximumArrayElements) return false;
		elements++;
		const auto hold = [&](uint64_t bytes) { return !budget || budget->Hold(bytes); };
		Value candidate;
		if (source.is_boolean())
			candidate = source.get<bool>();
		else if (source.is_number()) {
			const double number = source.get<double>();
			if (!std::isfinite(number)) return false;
			candidate = number;
		} else if (source.is_string()) {
			const auto &text = source.get_ref<const std::string &>();
			if (text.size() > Limits::MaximumTextBytes ||
				!hold(std::max(text.size(), std::string{}.capacity()) + 1))
				return false;
			candidate = text;
		} else if (source.is_array() || source.is_object()) {
			ArrayValue array;
			array.ElementType = ValueType::Any;
			const size_t slots = source.size() + (source.is_object() ? 1 : 0);
			if (slots > Limits::MaximumArrayElements - elements ||
				!hold(slots * sizeof(SourceArrayItem) + std::string{}.capacity()))
				return false;
			array.Items.reserve(slots);
			if (source.is_object()) {
				if (!hold(std::string{}.capacity())) return false;
				array.Items.push_back({ElementValue{std::string("tile_record")}});
				for (const auto &[name, raw] : source.items()) {
					// Source JSON null denotes an undefined optional record field.
					// Missing fields keep constructor defaults; retained source bytes keep the null spelling.
					if (raw.is_null()) continue;
					if (name.size() > Limits::MaximumTextBytes ||
						!hold(
							2 * sizeof(SourceArrayItem) + std::max(name.size(), std::string{}.capacity()) + 1
						))
						return false;
					if (elements > Limits::MaximumArrayElements - 2 || depth + 2 > Limits::MaximumArrayDepth)
						return false;
					elements += 2;
					Value field;
					if (!DecodeTileAttribute(raw, field, budget, depth + 2, elements)) return false;
					SourceArrayItem item;
					if (!TileAttributeItem(field, item)) return false;
					array.Items.push_back(
						{std::vector<SourceArrayItem>{{ElementValue{name}}, std::move(item)}}
					);
				}
			} else
				for (const auto &raw : source) {
					Value value;
					if (!DecodeTileAttribute(raw, value, budget, depth + 1, elements)) return false;
					SourceArrayItem item;
					if (!TileAttributeItem(value, item)) return false;
					array.Items.push_back(std::move(item));
				}
			candidate = std::move(array);
		} else
			return false;
		result = std::move(candidate);
		return true;
	}
	inline bool
	DecodeTileProperty(const nlohmann::json &source, imagegraph::Value &result, ImportBudget *budget) {
		if (!source.is_array()) return false;
		size_t elements = 0;
		return DecodeTileAttribute(source, result, budget, 0, elements);
	}
	inline bool
	DecodeTileColorMap(const nlohmann::json &source, imagegraph::Value &result, ImportBudget *budget) {
		using namespace imagegraph;
		if (!source.is_object() || source.size() > Limits::MaximumArrayElements) return false;
		if (budget && !budget->Hold(source.size() * sizeof(SourceArrayItem) + std::string{}.capacity()))
			return false;
		ArrayValue records;
		records.ElementType = ValueType::Any;
		records.Items.reserve(source.size());
		size_t count = 0;
		for (const auto &[key, raw] : source.items()) {
			if (!raw.is_object() || key.empty() || raw.contains("color_id")) return false;
			uint32_t colour = 0;
			for (char character : key) {
				if (character < '0' || character > '9' ||
					colour > (0xffffff - uint32_t(character - '0')) / 10)
					return false;
				colour = colour * 10 + uint32_t(character - '0');
			}
			Value decoded;
			if (!DecodeTileAttribute(raw, decoded, budget, 1, count)) return false;
			auto *record = std::get_if<ArrayValue>(&decoded);
			if (!record) return false;
			if (count > Limits::MaximumArrayElements - 3 ||
				(budget &&
				 !budget->Hold(
					 (record->Items.size() + 1) * sizeof(SourceArrayItem) + 2 * sizeof(SourceArrayItem) + 16
				 )))
				return false;
			count += 3;
			record->Items.reserve(record->Items.size() + 1);
			record->Items.push_back({std::vector<SourceArrayItem>{
				{ElementValue{std::string("color_id")}}, {ElementValue{double(colour)}}
			}});
			records.Items.push_back({std::move(record->Items)});
		}
		result = std::move(records);
		return true;
	}
	template <class Variant>
	inline bool EncodeTileLeaf(const Variant &leaf, nlohmann::json &result, uint64_t &remaining) {
		if (remaining < sizeof(nlohmann::json)) return false;
		remaining -= sizeof(nlohmann::json);
		nlohmann::json candidate;
		if (const auto *number = std::get_if<double>(&leaf)) {
			if (!std::isfinite(*number)) return false;
			candidate = *number;
		} else if (const auto *integer = std::get_if<int64_t>(&leaf))
			candidate = *integer;
		else if (const auto *boolean = std::get_if<bool>(&leaf))
			candidate = *boolean;
		else if (const auto *text = std::get_if<std::string>(&leaf)) {
			if (text->size() > remaining || text->size() > imagegraph::Limits::MaximumTextBytes) return false;
			remaining -= text->size();
			candidate = *text;
		} else if (const auto *vector = std::get_if<imagegraph::Vector2>(&leaf)) {
			if (!std::isfinite(vector->X) || !std::isfinite(vector->Y) ||
				remaining < 2 * sizeof(nlohmann::json))
				return false;
			remaining -= 2 * sizeof(nlohmann::json);
			candidate = {vector->X, vector->Y};
		} else
			return false;
		result = std::move(candidate);
		return true;
	}
	inline bool EncodeTileItems(
		const std::vector<imagegraph::SourceArrayItem> &row,
		nlohmann::json &result,
		size_t depth,
		size_t &elements,
		uint64_t &remaining
	);
	inline bool EncodeTileItem(
		const imagegraph::SourceArrayItem &item,
		nlohmann::json &result,
		size_t depth,
		size_t &elements,
		uint64_t &remaining
	) {
		if (depth > imagegraph::Limits::MaximumArrayDepth ||
			elements >= imagegraph::Limits::MaximumArrayElements)
			return false;
		elements++;
		if (const auto *leaf = std::get_if<imagegraph::ElementValue>(&item.Data))
			return EncodeTileLeaf(*leaf, result, remaining);
		const auto *row = std::get_if<std::vector<imagegraph::SourceArrayItem>>(&item.Data);
		return row && EncodeTileItems(*row, result, depth, elements, remaining);
	}
	inline bool EncodeTileItems(
		const std::vector<imagegraph::SourceArrayItem> &row,
		nlohmann::json &result,
		size_t depth,
		size_t &elements,
		uint64_t &remaining
	) {
		using namespace imagegraph;
		if (depth > Limits::MaximumArrayDepth || remaining < sizeof(nlohmann::json)) return false;
		remaining -= sizeof(nlohmann::json);
		const auto *leaf = row.empty() ? nullptr : std::get_if<ElementValue>(&row[0].Data);
		const auto *marker = leaf ? std::get_if<std::string>(leaf) : nullptr;
		const bool record = marker && *marker == "tile_record";
		nlohmann::json candidate = record ? nlohmann::json::object() : nlohmann::json::array();
		for (size_t i = record ? 1 : 0; i < row.size(); i++) {
			if (record) {
				const auto *pair = std::get_if<std::vector<SourceArrayItem>>(&row[i].Data);
				if (!pair || pair->size() != 2 || elements > Limits::MaximumArrayElements - 2) return false;
				elements += 2;
				const auto *nameLeaf = std::get_if<ElementValue>(&(*pair)[0].Data);
				const auto *name = nameLeaf ? std::get_if<std::string>(nameLeaf) : nullptr;
				if (!name || name->size() > remaining || candidate.contains(*name)) return false;
				remaining -= name->size();
				nlohmann::json field;
				if (!EncodeTileItem((*pair)[1], field, depth + 2, elements, remaining)) return false;
				candidate[*name] = std::move(field);
			} else {
				nlohmann::json item;
				if (!EncodeTileItem(row[i], item, depth + 1, elements, remaining)) return false;
				candidate.push_back(std::move(item));
			}
		}
		result = std::move(candidate);
		return true;
	}
	inline bool
	EncodeTileProperty(const imagegraph::Value &value, nlohmann::json &result, uint64_t maximumBytes) {
		const auto *array = std::get_if<imagegraph::ArrayValue>(&value);
		if (!array || !array->Nested.empty()) return false;
		size_t count = 1;
		if (!array->Items.empty()) return EncodeTileItems(array->Items, result, 0, count, maximumBytes);
		nlohmann::json candidate = nlohmann::json::array();
		for (const auto &leaf : array->Elements) {
			if (count++ >= imagegraph::Limits::MaximumArrayElements) return false;
			nlohmann::json item;
			if (!EncodeTileLeaf(leaf, item, maximumBytes)) return false;
			candidate.push_back(std::move(item));
		}
		result = std::move(candidate);
		return true;
	}
	inline void MergeTileSourceFields(nlohmann::json &encoded, const nlohmann::json &source) {
		if (encoded.is_object() && source.is_object()) {
			for (const auto &[name, raw] : source.items()) {
				if (!encoded.contains(name))
					encoded[name] = raw;
				else
					MergeTileSourceFields(encoded[name], raw);
			}
		} else if (encoded.is_array() && source.is_array()) {
			for (size_t i = 0; i < std::min(encoded.size(), source.size()); i++)
				MergeTileSourceFields(encoded[i], source[i]);
		}
	}

	inline bool EncodeTileColorMap(
		const imagegraph::Value &value,
		const nlohmann::json &source,
		nlohmann::json &result,
		uint64_t maximumBytes
	) {
		nlohmann::json rows;
		if (!EncodeTileProperty(value, rows, maximumBytes)) return false;
		nlohmann::json candidate = nlohmann::json::object();
		for (auto &record : rows) {
			if (!record.is_object() || !record.contains("color_id") || !record["color_id"].is_number())
				return false;
			const double colour = record["color_id"].get<double>();
			if (!std::isfinite(colour) || colour < 0 || colour > 0xffffff || std::trunc(colour) != colour)
				return false;
			const std::string key = std::to_string(uint32_t(colour));
			if (candidate.contains(key)) return false;
			record.erase("color_id");
			nlohmann::json merged = source.is_object() && source.contains(key) && source[key].is_object()
										? source[key]
										: nlohmann::json::object();
			if (!record.contains("target")) merged.erase("target");
			for (auto &[name, field] : record.items())
				merged[name] = std::move(field);
			candidate[key] = std::move(merged);
		}
		result = std::move(candidate);
		return true;
	}
}
