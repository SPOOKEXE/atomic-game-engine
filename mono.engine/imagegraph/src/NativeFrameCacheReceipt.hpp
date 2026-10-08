#pragma once

#include <engine/imagegraph/Document.hpp>

#include <cstdint>
#include <optional>

namespace engine::imagegraph::detail {
	struct NativeFrameCacheReceiptInspection {
		uint64_t DecodedBytes = 0;
		bool HasFrame = false;
		std::optional<uint64_t> SerializedSlots = std::nullopt;
		bool operator==(const NativeFrameCacheReceiptInspection &) const = default;
	};
	// Checks the complete cooked packet without decoding pixels or allocating failure diagnostics.
	// Failure leaves the previous inspection untouched. Tick is a zero-based source frame.
	Status InspectNativeFrameCacheReceipt(
		const Node &node, uint64_t tick, NativeFrameCacheReceiptInspection &inspection
	);
	// Checks the whole v2 inventory, then decodes only this zero-based serialized slot.
	// Missing in-range slots become source noone; failures leave the prior value untouched.
	Status DecodeSourceFrameCacheReceiptSlot(
		const Node &node,
		uint64_t slot,
		Value &value,
		Diagnostic &diagnostic,
		uint64_t maximumBytes = Limits::MaximumEvaluationBytes
	);
}
