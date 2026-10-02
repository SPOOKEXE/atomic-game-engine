#pragma once

// Source enums remain real numbers. Array getter values bypass source widget clamping.

#include <engine/imagegraph/Catalogue.hpp>

#include <algorithm>
#include <cmath>
#include <optional>

namespace engine::imagegraph::detail {
	inline std::optional<double> SourceChoiceNumber(const Value &value) {
		if (const auto *number = std::get_if<double>(&value))
			return std::isfinite(*number) ? std::optional<double>(*number) : std::nullopt;
		if (const auto *integer = std::get_if<int64_t>(&value)) return static_cast<double>(*integer);
		if (const auto *choice = std::get_if<EnumValue>(&value)) return static_cast<double>(choice->Value);
		return std::nullopt;
	}
	inline std::optional<double>
	NormalizeSourceChoice(const CatalogueInput &input, double value, bool arraySelection) {
		if (!std::isfinite(value)) return std::nullopt;
		if (!input.SourceBehavior || arraySelection) return value;
		const auto mode = input.SourceBehavior->ChoiceClamp;
		if (mode == SourceChoiceClamp::Disabled) return value;
		if (mode == SourceChoiceClamp::Unknown || !input.SourceChoices || !input.SourceChoices->RawCount ||
			*input.SourceChoices->RawCount == 0)
			return std::nullopt;
		return std::clamp(value, 0.0, double(*input.SourceChoices->RawCount - 1));
	}
}
