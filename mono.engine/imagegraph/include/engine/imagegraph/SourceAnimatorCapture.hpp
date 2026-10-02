#pragma once

// Raw source animator snapshots used when disabling animation, before property getter conversions.
#include <engine/imagegraph/FrameTime.hpp>

#include <span>

namespace engine::imagegraph {
	enum class SourceAnimatorCaptureKind { Value, Trigger };
	struct SourceAnimatorCaptureOptions {
		FrameTime Time{};
		uint64_t TotalFrames = 1;
		// Remaining caller headroom after charging prior live captures and fixed result storage.
		uint64_t AvailableOwnedBytes = Limits::MaximumArrayBytes;
		SourceAnimatorCaptureKind Kind = SourceAnimatorCaptureKind::Value;
	};
	struct SourceAnimatorCapture {
		Value Data{};
		// Owned logical payload excludes this fixed result storage and allocator bookkeeping.
		uint64_t ResidentOwnedBytes = 0;
		uint64_t PeakOwnedBytes = 0;
		bool operator==(const SourceAnimatorCapture &) const = default;
	};

	// Keys are borrowed in their original source order until this call returns. Capture follows the
	// source setAnim(false) ordering: multiple keys hold the first raw value; a lone reachable driver
	// runs before the reset. Numeric driver outputs stay real, including integer/boolean/colour input.
	// Promotion scratch and output share one bounded ledger. Failure leaves out unchanged.
	Status CaptureSourceDisabledAnimatorValue(
		std::span<const Keyframe> keys,
		const SourceAnimatorCaptureOptions &options,
		SourceAnimatorCapture &out,
		Diagnostic &diagnostic
	);
}
