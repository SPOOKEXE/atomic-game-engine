#pragma once

#include <algorithm>
#include <imgui.h>
#include <optional>
#include <span>
#include <studio/Diagnostics.hpp>
#include <vector>

namespace studio::frame_graph_table_detail {
	struct Sort {
		int Column = 0;
		ImGuiSortDirection Direction = ImGuiSortDirection_None;
	};

	inline float BusyMilliseconds(const DiagnosticSpan &span) {
		if (span.Category == engine::core::ProfileCategory::Gpu) {
			return std::max(span.Milliseconds, 0.0f);
		}
		return std::max(span.Milliseconds - span.IdleMilliseconds, 0.0f);
	}

	inline std::optional<float> Share(const DiagnosticSpan &span, float frameBusy) {
		if (span.Category == engine::core::ProfileCategory::Gpu || span.Reported) {
			return std::nullopt;
		}
		if (frameBusy <= 0.0001f) {
			return 0.0f;
		}
		return (BusyMilliseconds(span) / frameBusy) * 100.0f;
	}

	inline void BuildOrder(
		std::span<const DiagnosticSpan> spans, float frameBusy, const Sort *sort, std::vector<size_t> &order
	) {
		order.resize(spans.size());
		for (size_t index = 0; index < order.size(); index++) {
			order[index] = index;
		}
		if (sort == nullptr || sort->Direction == ImGuiSortDirection_None || sort->Column < 0 ||
			sort->Column >= 4) {
			return;
		}

		const auto compareValue = [&](size_t left, size_t right) {
			const DiagnosticSpan &a = spans[left];
			const DiagnosticSpan &b = spans[right];
			int comparison = 0;
			bool aAvailable = true;
			bool bAvailable = true;
			switch (sort->Column) {
			case 0:
				comparison = a.Name.compare(b.Name);
				break;
			case 1: {
				const float aBusy = BusyMilliseconds(a);
				const float bBusy = BusyMilliseconds(b);
				comparison = aBusy < bBusy ? -1 : aBusy > bBusy ? 1 : 0;
				break;
			}
			case 2:
				comparison = a.IdleMilliseconds < b.IdleMilliseconds   ? -1
							 : a.IdleMilliseconds > b.IdleMilliseconds ? 1
																	   : 0;
				break;
			case 3: {
				const std::optional<float> aShare = Share(a, frameBusy);
				const std::optional<float> bShare = Share(b, frameBusy);
				aAvailable = aShare.has_value();
				bAvailable = bShare.has_value();
				const float aValue = aShare.value_or(0.0f);
				const float bValue = bShare.value_or(0.0f);
				comparison = aValue < bValue ? -1 : aValue > bValue ? 1 : 0;
				break;
			}
			default:
				return false;
			}
			if (aAvailable != bAvailable) {
				return aAvailable;
			}
			if (comparison == 0) {
				return false;
			}
			return sort->Direction == ImGuiSortDirection_Ascending ? comparison < 0 : comparison > 0;
		};

		std::sort(order.begin(), order.end(), [&](size_t left, size_t right) {
			const bool leftFirst = compareValue(left, right);
			const bool rightFirst = compareValue(right, left);
			if (leftFirst != rightFirst) {
				return leftFirst;
			}
			return left < right;
		});
	}
}
