#pragma once

#include <engine/imagegraph/FrameTime.hpp>

#include <cmath>

namespace studio::detail {
	enum class ImageGraphExportEvent { Update, Save };

	inline bool SourceExportTriggered(
		std::span<const engine::imagegraph::EvaluationInputValue> values, ImageGraphExportEvent event
	) {
		using namespace engine::imagegraph;
		const auto number = [](const Value &value) -> std::optional<double> {
			if (const auto *flag = std::get_if<bool>(&value)) return *flag ? 1.0 : 0.0;
			if (const auto *choice = std::get_if<EnumValue>(&value)) return double(choice->Value);
			if (const auto *integer = std::get_if<int64_t>(&value)) return double(*integer);
			if (const auto *scalar = std::get_if<double>(&value); scalar && std::isfinite(*scalar))
				return *scalar;
			return std::nullopt;
		};
		const std::string_view flag =
			event == ImageGraphExportEvent::Save ? "export_on_save" : "export_on_update";
		bool enabled = false;
		for (const auto &input : values) {
			if (input.Port == "type" && number(input.Data).value_or(-1) != 0) return false;
			if (input.Port == flag) enabled = number(input.Data).value_or(0) != 0;
		}
		return enabled;
	}

	struct ImageGraphExportUpdate {
		uint64_t Revision = 0, InputRevision = 0;
		engine::imagegraph::FrameTime Frame{};
		bool Captured = false;
		bool Accept(uint64_t revision, uint64_t inputRevision, engine::imagegraph::FrameTime frame) {
			if (Captured && Revision == revision && InputRevision == inputRevision && Frame == frame)
				return false;
			Revision = revision;
			InputRevision = inputRevision;
			Frame = frame;
			Captured = true;
			return true;
		}
	};
}
