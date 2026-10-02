#pragma once
#include "Processor.hpp"

#include <functional>
namespace engine::imagegraph::detail {
	inline const Value *TileProperty(const NodeContext &context, std::string_view id) {
		for (const auto &property : context.Authored.SourceProperties)
			if (property.Port == id) return &property.Data;
		return nullptr;
	}
	inline const Value *TileField(const StructData &record, std::string_view id) {
		for (const auto &[name, value] : record.Fields)
			if (name == id) return &value;
		return nullptr;
	}
	template <class Variant> inline bool TileNumber(const Variant &value, double &number) {
		if (const auto *v = std::get_if<double>(&value))
			number = *v;
		else if (const auto *v = std::get_if<int64_t>(&value))
			number = double(*v);
		else if (const auto *v = std::get_if<bool>(&value))
			number = *v ? 1 : 0;
		else if (const auto *v = std::get_if<EnumValue>(&value))
			number = double(v->Value);
		else
			return false;
		return std::isfinite(number);
	}
	inline size_t TileArrayCount(const ArrayValue &array) {
		return !array.Items.empty()	   ? array.Items.size()
			   : !array.Nested.empty() ? array.Nested.size()
									   : array.Elements.size();
	}
	inline const ElementValue *TileArrayLeaf(const ArrayValue &array, size_t index) {
		if (!array.Items.empty()) return std::get_if<ElementValue>(&array.Items[index].Data);
		if (!array.Nested.empty()) return nullptr;
		return &array.Elements[index];
	}
	inline bool TileCoordinates(const Value &value, Vector2 &result) {
		if (const auto *v = std::get_if<Vector2>(&value)) {
			result = *v;
			return std::isfinite(v->X) && std::isfinite(v->Y);
		}
		const auto *array = std::get_if<ArrayValue>(&value);
		if (!array || TileArrayCount(*array) < 2) return false;
		const auto *x = TileArrayLeaf(*array, 0), *y = TileArrayLeaf(*array, 1);
		return x && y && TileNumber(*x, result.X) && TileNumber(*y, result.Y);
	}
	inline bool TileSelectionValue(const Value &value, TileSelection &target) {
		double number = 0;
		if (TileNumber(value, number)) {
			target = {number, false};
			return true;
		}
		Vector2 coordinates;
		if (TileCoordinates(value, coordinates)) {
			target = {coordinates.Y, true};
			return true;
		}
		return false;
	}
	inline bool TileSelectionElement(const ArrayValue &array, size_t index, TileSelection &target) {
		if (const auto *leaf = TileArrayLeaf(array, index)) {
			double number = 0;
			if (TileNumber(*leaf, number)) {
				target = {number, false};
				return true;
			}
			if (const auto *vector = std::get_if<Vector2>(leaf)) {
				target = {vector->Y, true};
				return std::isfinite(vector->Y);
			}
			return false;
		}
		if (!array.Items.empty()) {
			const auto &row = std::get<std::vector<SourceArrayItem>>(array.Items[index].Data);
			if (row.size() < 2) return false;
			const auto *leaf = std::get_if<ElementValue>(&row[1].Data);
			double number = 0;
			if (leaf && TileNumber(*leaf, number)) {
				target = {number, true};
				return true;
			}
		}
		if (!array.Nested.empty()) {
			const auto &row = array.Nested[index];
			double number = 0;
			if (row.size() >= 2 && TileNumber(row[1], number)) {
				target = {number, true};
				return true;
			}
		}
		return false;
	}
	inline bool TileRecordRow(const SourceArrayItem &item, StructData &record) {
		const auto *row = std::get_if<std::vector<SourceArrayItem>>(&item.Data);
		if (!row || row->empty()) return false;
		const auto *marker = std::get_if<ElementValue>(&(*row)[0].Data);
		const auto *name = marker ? std::get_if<std::string>(marker) : nullptr;
		if (!name || *name != "tile_record") return false;
		record.Fields.reserve(row->size() - 1);
		for (size_t i = 1; i < row->size(); i++) {
			const auto *pair = std::get_if<std::vector<SourceArrayItem>>(&(*row)[i].Data);
			if (!pair || pair->size() != 2) return false;
			const auto *keyLeaf = std::get_if<ElementValue>(&(*pair)[0].Data);
			const auto *key = keyLeaf ? std::get_if<std::string>(keyLeaf) : nullptr;
			if (!key || std::any_of(record.Fields.begin(), record.Fields.end(), [&](const auto &field) {
					return field.first == *key;
				}))
				return false;
			Value value;
			if (const auto *leaf = std::get_if<ElementValue>(&(*pair)[1].Data))
				std::visit([&](const auto &data) { value = data; }, *leaf);
			else if (const auto *nested = std::get_if<std::vector<SourceArrayItem>>(&(*pair)[1].Data)) {
				ArrayValue array;
				array.ElementType = ValueType::Any;
				array.Items = *nested;
				value = std::move(array);
			} else
				return false;
			record.Fields.emplace_back(*key, std::move(value));
		}
		return true;
	}
	template <class Fn>
	inline bool TileRecords(NodeContext &context, const Value *value, std::string_view port, Fn &&fn) {
		if (!value) return true;
		const auto *array = std::get_if<ArrayValue>(value);
		if (!array || !ValidRuntimeValue(*value))
			return context.Fail(Status::InvalidValue, "tile records require a bounded owned array", port);
		for (size_t i = 0; i < TileArrayCount(*array); i++) {
			const auto *leaf = TileArrayLeaf(*array, i);
			const auto *record = leaf ? std::get_if<StructValue>(leaf) : nullptr;
			if (record && record->Data) {
				if (!fn(*record->Data)) return false;
			} else {
				StructData decoded;
				if (array->Items.empty() || !TileRecordRow(array->Items[i], decoded))
					return context.Fail(Status::TypeMismatch, "tile record requires named field pairs", port);
				if (!fn(decoded)) return false;
			}
		}
		return true;
	}
	template <class Number>
	inline bool TileNumbers(
		NodeContext &context,
		const Value *value,
		std::string_view port,
		std::vector<Number> &output,
		size_t maximum
	) {
		if (!value) return true;
		const auto *array = std::get_if<ArrayValue>(value);
		if (!array || !ValidRuntimeValue(*value) || TileArrayCount(*array) > maximum)
			return context.Fail(Status::InvalidValue, "tile indices exceed their source array bounds", port);
		output.clear();
		output.reserve(TileArrayCount(*array));
		for (size_t i = 0; i < TileArrayCount(*array); i++) {
			const auto *leaf = TileArrayLeaf(*array, i);
			double number = 0;
			if (!leaf || !TileNumber(*leaf, number))
				return context.Fail(Status::TypeMismatch, "tile index requires a finite number", port);
			if constexpr (std::is_integral_v<Number>) {
				if (number < double(INT32_MIN) || number > double(INT32_MAX))
					return context.Fail(Status::InvalidValue, "tile index exceeds signed shader range", port);
			}
			output.push_back(Number(number));
		}
		return true;
	}
	inline bool TileNamedRecord(
		NodeContext &context, const StructData &record, std::string_view port, std::string &name
	) {
		if (const auto *value = TileField(record, "name")) {
			const auto *text = std::get_if<std::string>(value);
			if (!text) return context.Fail(Status::TypeMismatch, "tile record name requires text", port);
			name = *text;
		}
		return true;
	}
	inline bool ReadTileRules(NodeContext &context, const Value *value, std::vector<TileRuleData> &rules) {
		return TileRecords(context, value, "ruleTiles", [&](const StructData &record) {
			TileRuleData rule;
			if (!TileNamedRecord(context, record, "ruleTiles", rule.Name)) return false;
			if (const auto *v = TileField(record, "active")) {
				double number = 0;
				if (!TileNumber(*v, number))
					return context.Fail(
						Status::TypeMismatch, "tile rule active flag is invalid", "ruleTiles"
					);
				rule.Active = number != 0;
			}
			if (const auto *v = TileField(record, "range")) {
				double number = 0;
				if (!TileNumber(*v, number) || number < 0 || number > 32 || std::trunc(number) != number)
					return context.Fail(
						Status::InvalidValue, "tile rule range exceeds source selection bounds", "ruleTiles"
					);
				rule.Range = uint32_t(number);
			}
			if (const auto *v = TileField(record, "size"))
				if (!TileCoordinates(*v, rule.Size))
					return context.Fail(
						Status::TypeMismatch, "tile rule size needs two coordinates", "ruleTiles"
					);
			if (const auto *v = TileField(record, "probability"))
				if (!TileNumber(*v, rule.Probability))
					return context.Fail(
						Status::TypeMismatch, "tile rule probability is invalid", "ruleTiles"
					);
			if (const auto *v = TileField(record, "selection_rules")) {
				const auto *array = std::get_if<ArrayValue>(v);
				if (!array || TileArrayCount(*array) > 64)
					return context.Fail(
						Status::LimitExceeded, "tile rule selection exceeds shader capacity", "ruleTiles"
					);
				rule.Selection.clear();
				rule.Selection.reserve(TileArrayCount(*array));
				for (size_t i = 0; i < TileArrayCount(*array); i++) {
					TileSelection selection;
					if (!TileSelectionElement(*array, i, selection))
						return context.Fail(
							Status::TypeMismatch,
							"tile rule selection requires tile or terrain indices",
							"ruleTiles"
						);
					rule.Selection.push_back(selection);
				}
			}
			if (!TileRecords(
					context,
					TileField(record, "replacements"),
					"ruleTiles",
					[&](const StructData &replacement) {
						std::vector<double> indices;
						if (!TileNumbers(context, TileField(replacement, "index"), "ruleTiles", indices, 256))
							return false;
						rule.Replacements.push_back(std::move(indices));
						return true;
					}
				))
				return false;
			rules.push_back(std::move(rule));
			return true;
		});
	}
	inline bool ReadTilesetProperties(NodeContext &context, TilesetData &data) {
		if (!TileRecords(
				context,
				TileProperty(context, "animatedTiles"),
				"animatedTiles",
				[&](const StructData &record) {
					TileAnimationData animation;
					if (!TileNamedRecord(context, record, "animatedTiles", animation.Name) ||
						!TileNumbers(
							context, TileField(record, "index"), "animatedTiles", animation.Indices, 256
						))
						return false;
					animation.Length = uint32_t(animation.Indices.size());
					if (const auto *value = TileField(record, "size")) {
						double number = 0;
						if (!TileNumber(*value, number) || number < 0 || number > 256)
							return context.Fail(
								Status::InvalidValue,
								"tile animation length exceeds shader capacity",
								"animatedTiles"
							);
						animation.Length = uint32_t(number);
					}
					data.Animations.push_back(std::move(animation));
					return true;
				}
			))
			return false;
		if (!TileRecords(
				context, TileProperty(context, "autoterrain"), "autoterrain", [&](const StructData &record) {
					TileTerrainData terrain;
					if (!TileNamedRecord(context, record, "autoterrain", terrain.Name) ||
						!TileNumbers(context, TileField(record, "index"), "autoterrain", terrain.Indices, 63))
						return false;
					if (const auto *value = TileField(record, "type")) {
						double number = 0;
						if (!TileNumber(*value, number) || number < -1 || number > 4 ||
							std::trunc(number) != number)
							return context.Fail(
								Status::InvalidValue, "terrain layout has no source case", "autoterrain"
							);
						terrain.Type = int32_t(number);
					}
					// deserialize does not call init, so the source preview index remains zero for every
					// layout.
					data.Terrains.push_back(std::move(terrain));
					return true;
				}
			))
			return false;
		return ReadTileRules(context, TileProperty(context, "ruleTiles"), data.Rules);
	}
	struct TileConversionEntry {
		uint32_t Colour = 0;
		TileSelection Target{};
		bool Present = false;
	};
	inline bool ReadTileConversions(NodeContext &context, std::vector<TileConversionEntry> &mapping) {
		const auto *listValue = TileProperty(context, "colorList");
		const auto *list = listValue ? std::get_if<ArrayValue>(listValue) : nullptr;
		if (!listValue) return true;
		if (!list || !ValidRuntimeValue(*listValue) || TileArrayCount(*list) > 256)
			return context.Fail(
				Status::InvalidValue, "tile conversion palette exceeds source capacity", "colorList"
			);
		for (size_t i = 0; i < TileArrayCount(*list); i++) {
			const auto *leaf = TileArrayLeaf(*list, i);
			double colour = 0;
			if (!leaf || !TileNumber(*leaf, colour) || colour < 0 || colour > 0xffffff ||
				std::trunc(colour) != colour)
				return context.Fail(
					Status::InvalidValue, "tile palette colour requires a packed RGB value", "colorList"
				);
			TileConversionEntry entry;
			entry.Colour = uint32_t(colour);
			mapping.push_back(entry);
		}
		// Source colorMap's keyed object is represented as authored Array<Struct> records with color_id.
		return TileRecords(
			context, TileProperty(context, "colorMap"), "colorMap", [&](const StructData &record) {
				double colour = 0;
				const auto *value = TileField(record, "color_id");
				if (!value || !TileNumber(*value, colour) || colour < 0 || colour > 0xffffff ||
					std::trunc(colour) != colour)
					return context.Fail(
						Status::InvalidValue, "tile colorMap record requires its packed color_id", "colorMap"
					);
				TileSelection target;
				bool present = false;
				if (const auto *value = TileField(record, "target")) {
					if (!std::holds_alternative<UndefinedValue>(*value)) {
						if (!TileSelectionValue(*value, target))
							return context.Fail(
								Status::InvalidValue, "tile mapping target is invalid", "colorMap"
							);
						present = true;
					}
				}
				for (auto &entry : mapping)
					if (entry.Colour == uint32_t(colour)) {
						entry.Target = target;
						entry.Present = present;
					}
				return true;
			}
		);
	}
}
