#pragma once

#include "NodeExecutors.hpp"
#include "SourceFontReceipts.hpp"

#include <engine/imagegraph/SourceFont.hpp>

namespace engine::imagegraph::detail {
	std::optional<uint64_t> SourceFontAuthoredRetainedBytes(const Node &);
	struct SourceFontObservationLease {
		AllocationReservation Charge;
		SourceFontObservation Owned;
		const SourceFontObservation *Record = nullptr;
	};
	// The request is already owned and admitted by its caller. An exact recording suppresses all
	// provider work, and a provider candidate receives an exclusive operation reservation.
	inline bool ObserveSourceFont(
		NodeContext &context, const SourceFontRequest &request, SourceFontObservationLease &lease
	) {
		if (!SourceFontRequestRetainedBytes(request))
			return context.Fail(Status::InvalidValue, "font request is malformed", request.Role);
		const SourceFontObservation *record = nullptr;
		for (const auto &observation : context.Request.FontObservations) {
			const auto &key = observation.Request;
			if (key.Authored.Id != request.Authored.Id || key.ProcessorRow != request.ProcessorRow ||
				key.Tick != request.Tick || key.Subframe != request.Subframe ||
				key.NegativeFrame != request.NegativeFrame || key.Role != request.Role)
				continue;
			if (record)
				return context.Fail(Status::DuplicateId, "font observation key is duplicated", request.Role);
			record = &observation;
		}
		if (record) {
			if (record->Request != request || !SourceFontObservationRetainedBytes(*record))
				return context.Fail(
					Status::InvalidValue, "font observation is stale or malformed", request.Role
				);
			lease.Record = record;
			return true;
		}
		if (request.Role == "bitmap_texture")
			return context.Fail(
				Status::UnsupportedExecution,
				"bitmap texture needs an exact owned atlas observation",
				request.Role
			);
		if (!context.Request.FontProvider)
			return context.Fail(
				Status::UnsupportedExecution,
				"font file state and glyph observation are unavailable",
				request.Role
			);
		const uint64_t maximum = context.AvailableBytes();
		auto charge = context.ReserveWorkspace(maximum, request.Role);
		if (!charge) return false;
		SourceFontObservation candidate;
		std::string failure;
		const uint64_t before = context.Request.FontProvider->RetainedBytes();
		const bool observed = context.Request.FontProvider->Observe(request, maximum, candidate, failure);
		const uint64_t after = context.Request.FontProvider->RetainedBytes();
		// grug transfer admitted workspace to persistent storage before validating provider output.
		if (after > before) {
			auto growth = charge->Split(after - before);
			auto &residency =
				context.FontHostResidency ? *context.FontHostResidency : context.FontHostGrowthCharge;
			if (!growth || !residency.Merge(std::move(*growth)))
				return context.Fail(
					Status::LimitExceeded, "font provider growth exceeds operation reservation", request.Role
				);
		} else if (after < before && context.FontHostResidency) {
			if (before - after > context.FontHostResidency->Bytes() ||
				!context.FontHostResidency->Resize(context.FontHostResidency->Bytes() - (before - after)))
				return context.Fail(
					Status::LimitExceeded, "font provider residency is inconsistent", request.Role
				);
		}
		if (!observed)
			return context.Fail(
				Status::UnsupportedExecution,
				failure.empty() ? "font provider refused its request" : std::move(failure),
				request.Role
			);
		const auto retained = SourceFontObservationRetainedBytes(candidate);
		if (candidate.Request != request || !retained)
			return context.Fail(
				Status::InvalidValue,
				"font provider returned a mismatched or malformed observation",
				request.Role
			);
		if (*retained > charge->Bytes() || !charge->Resize(*retained))
			return context.Fail(
				Status::LimitExceeded, "font observation exceeds operation reservation", request.Role
			);
		lease.Owned = std::move(candidate);
		lease.Charge = std::move(*charge);
		lease.Record = &lease.Owned;
		if (context.FontReceipts && !context.FontReceipts->Append(lease.Owned))
			return context.Fail(
				Status::LimitExceeded, "retained font receipts exceed byte or identity bounds", request.Role
			);
		return true;
	}
}
