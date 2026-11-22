#pragma once

#include <algorithm>
#include <cstdint>
#include <span>

namespace engine::render {
	// One nested image's requested allocation. Visible portal images are mandatory;
	// supplemental work may use only the capacity left by a fair mandatory split.
	struct PortalChildBudget {
		uint64_t Pixels = 0;
		bool Optional = false;
		bool Accepted = false;
	};
	struct PortalChildExtentFit {
		uint32_t MaximumExtent = 0;
		uint32_t Width = 0;
		uint32_t Height = 0;
		uint64_t Pixels = 0;
	};

	// Fits a child image into its fair slice, preserving aspect within pixel rounding.
	inline bool FitPortalChildExtent(
		uint32_t width,
		uint32_t height,
		uint64_t pixelBudget,
		uint32_t pixelMultiplier,
		PortalChildExtentFit &fit
	) {
		if (width == 0 || height == 0 || pixelMultiplier == 0 || pixelBudget < pixelMultiplier) return false;
		const uint32_t longest = std::max(width, height);
		uint32_t lower = 1;
		uint32_t upper = longest;
		uint32_t accepted = 0;
		while (lower <= upper) {
			const uint32_t extent = lower + (upper - lower) / 2;
			const uint32_t fittedWidth = std::max(1u, uint32_t(uint64_t(width) * extent / longest));
			const uint32_t fittedHeight = std::max(1u, uint32_t(uint64_t(height) * extent / longest));
			const uint64_t pixels = uint64_t(fittedWidth) * fittedHeight * pixelMultiplier;
			if (pixels <= pixelBudget) {
				accepted = extent;
				lower = extent + 1;
			} else {
				upper = extent - 1;
			}
		}
		if (accepted == 0) return false;
		fit.MaximumExtent = accepted;
		fit.Width = std::max(1u, uint32_t(uint64_t(width) * accepted / longest));
		fit.Height = std::max(1u, uint32_t(uint64_t(height) * accepted / longest));
		fit.Pixels = uint64_t(fit.Width) * fit.Height * pixelMultiplier;
		return true;
	}

	// Plans one equal child allowance, matching the runtime's recursive limit.
	// Optional children never reduce a visible child's minimum allocation.
	inline bool PlanPortalChildBudgets(
		std::span<PortalChildBudget> children,
		uint64_t availablePixels,
		bool hasLocalSurface,
		uint32_t &childBudget,
		uint64_t &localPixels
	) {
		size_t admitted = 0;
		uint64_t largestMandatory = 0;
		for (auto &child : children) {
			child.Accepted = !child.Optional;
			if (!child.Accepted) continue;
			++admitted;
			largestMandatory = std::max(largestMandatory, child.Pixels);
		}
		const size_t localShares = hasLocalSurface ? 1 : 0;
		if (admitted != 0) {
			const uint64_t mandatoryBudget = availablePixels / (admitted + localShares);
			if (largestMandatory > mandatoryBudget) return false;
		}
		for (auto &child : children) {
			if (!child.Optional) continue;
			const uint64_t candidateBudget = availablePixels / (admitted + 1 + localShares);
			if (child.Pixels > candidateBudget || largestMandatory > candidateBudget) continue;
			child.Accepted = true;
			++admitted;
		}
		childBudget = admitted == 0 ? 0 : uint32_t(availablePixels / (admitted + localShares));
		localPixels = availablePixels - uint64_t(childBudget) * admitted;
		return true;
	}
}
