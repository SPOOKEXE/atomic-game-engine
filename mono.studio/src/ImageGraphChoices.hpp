#pragma once

// Inspector choices keep source indices, including separator slots, without altering authored values.

#include <engine/imagegraph/Catalogue.hpp>

#include <algorithm>
#include <cmath>
#include <optional>

namespace studio::imagegraph_choices {
	using namespace engine::imagegraph;

	inline const CatalogueInput *Input(std::string_view nodeType, std::string_view property) {
		const auto *entry = FindCatalogueEntry(nodeType);
		if (!entry) return nullptr;
		if (const auto *input = FindCatalogueInput(*entry, property)) return input;
		size_t group = 0;
		return FindDynamicTemplate(*entry, property, group);
	}
	inline bool Suggested(const CatalogueInput *input) {
		return !input || !input->SourceBehavior || input->SourceBehavior->StrictSuggestion != false;
	}
	inline std::optional<double> Number(const Value &value) {
		if (const auto *choice = std::get_if<EnumValue>(&value)) return static_cast<double>(choice->Value);
		if (const auto *integer = std::get_if<int64_t>(&value)) return static_cast<double>(*integer);
		if (const auto *number = std::get_if<double>(&value); number && std::isfinite(*number))
			return *number;
		return std::nullopt;
	}
	struct View {
		const CatalogueInput &Source;
		size_t Count() const {
			if (Source.SourceChoices)
				return Source.SourceChoices->Status == SourceChoicesStatus::Resolved
						   ? Source.SourceChoices->Entries.size()
						   : 0;
			return CatalogueChoiceCount(Source);
		}
		std::optional<CatalogueSourceChoice> At(size_t index) const {
			if (index >= Count()) return std::nullopt;
			if (Source.SourceChoices) return Source.SourceChoices->Entries[index];
			size_t start = 0;
			for (size_t row = 0; row < index; ++row)
				start = Source.Choices.find(';', start) + 1;
			const auto end = Source.Choices.find(';', start);
			return CatalogueSourceChoice{
				static_cast<int32_t>(index),
				Source.Choices.substr(start, end == std::string_view::npos ? end : end - start),
				false
			};
		}
		std::optional<CatalogueSourceChoice> Selected(const Value &value) const {
			const auto number = Number(value);
			if (!number) return std::nullopt;
			for (size_t row = 0; row < Count(); ++row) {
				const auto choice = At(row);
				if (!choice->Separator && *number == choice->SourceIndex) return choice;
			}
			return std::nullopt;
		}
		std::optional<Value> Select(size_t row) const {
			const auto choice = At(row);
			if (!choice || choice->Separator) return std::nullopt;
			return Value{EnumValue{choice->SourceIndex}};
		}
		std::optional<Value> Fraction(double number) const {
			Value candidate = number;
			return CatalogueSourceEnumValue(Source, candidate) ? std::optional<Value>(std::move(candidate))
															   : std::nullopt;
		}
	};
}
