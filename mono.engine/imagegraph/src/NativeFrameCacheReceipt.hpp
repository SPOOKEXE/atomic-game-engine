#pragma once

#include <engine/imagegraph/Document.hpp>

#include <cstdint>

namespace engine::imagegraph::detail {
	struct NativeFrameCacheReceiptInspection {
		uint64_t DecodedBytes = 0;
		bool HasFrame = false;
		bool operator==(const NativeFrameCacheReceiptInspection &) const = default;
	};
	// Checks the complete cooked packet without decoding pixels or allocating failure diagnostics.
	// Failure leaves the previous inspection untouched. Tick is a zero-based source frame.
	Status InspectNativeFrameCacheReceipt(
		const Node &node, uint64_t tick, NativeFrameCacheReceiptInspection &inspection
	);
}
