#pragma once
#include "EvaluationAllocator.hpp"
#include "EvaluationBudget.hpp"

#include <engine/imagegraph/SourceFont.hpp>

namespace engine::imagegraph::detail {
	struct SourceFontReceiptSink {
		EvaluationBudget &Budget;
		AllocationReservation Charge;
		uint64_t ComparisonWork = 0;
		EvaluationVector<SourceFontObservation> Records;
		explicit SourceFontReceiptSink(EvaluationBudget &budget)
			: Budget(budget), Records(EvaluationAllocator<SourceFontObservation>(budget)) {}
		bool Append(const SourceFontObservation &record) {
			constexpr uint64_t workLimit = 16 * 1024 * 1024;
			const auto retained = SourceFontObservationRetainedBytes(record);
			if (!retained) return false;
			for (const auto &old : Records) {
				const uint64_t keyBytes = record.Request.Authored.Id.size() + record.Request.Role.size() + 1;
				if (keyBytes > workLimit - ComparisonWork) return false;
				ComparisonWork += keyBytes;
				const auto &a = old.Request;
				const auto &b = record.Request;
				if (a.Authored.Id == b.Authored.Id && a.ProcessorRow == b.ProcessorRow && a.Tick == b.Tick &&
					a.Subframe == b.Subframe && a.NegativeFrame == b.NegativeFrame && a.Role == b.Role) {
					if (*retained > workLimit - ComparisonWork) return false;
					ComparisonWork += *retained;
					return old == record;
				}
			}
			if (Records.size() >= Limits::MaximumNodes) return false;
			auto charge = Budget.Reserve(*retained - sizeof(record));
			if (!charge || !Charge.Merge(std::move(*charge))) return false;
			Records.push_back(record);
			return true;
		}
	};
}
