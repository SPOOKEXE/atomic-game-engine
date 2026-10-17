#pragma once

#include <algorithm>
#include <cstdint>
#include <span>

namespace engine::render {
	// One nested image's minimum allocation. Visible portal images are mandatory;
	// supplemental work may use only the capacity left by a fair mandatory split.
	struct PortalChildBudget {
		uint64_t Pixels = 0;
		bool Optional = false;
		bool Accepted = false;
	};

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
