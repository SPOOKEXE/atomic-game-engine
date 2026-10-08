#pragma once

#include "EvaluationBudget.hpp"
#include "NodeExecutors.hpp"
#include "SourceAnimatorIdentity.hpp"
#include "SourceSeparatedVec2.hpp"

#include <engine/imagegraph/GroupReplay.hpp>

#include <algorithm>

namespace engine::imagegraph {
	struct GroupReplayState::Storage {
		detail::EvaluationBudget Budget;
		detail::AllocationReservation Charge;
		detail::AllocationReservation PreviousShadow;
		uint64_t Revision = 0;
		std::vector<GroupReplayEntry> Entries;
		bool Bound = false;
		std::vector<GroupSubtypeBinding> Bindings;
		std::vector<GroupSubtypeOverlay> SharedSubtypes;
		std::vector<DetachedSourceAnimator> DetachedAnimators;
		uint64_t NextAnimatorId = 0;
		explicit Storage(uint64_t maximumBytes) : Budget(maximumBytes) {}
	};
	namespace detail {
		struct GroupReplayAccess {
			using Owner = GroupReplayState::Storage;
			static const Owner *Get(const GroupReplayState &state) {
				return state.Data.get();
			}
			static Owner *Get(GroupReplayState &state) {
				return state.Data.get();
			}
			static void Install(GroupReplayState &state, std::unique_ptr<Owner> candidate) {
				state.Data = std::move(candidate);
				state.AdvanceObservation();
			}
		};
		// Attributes have no source input index. They never join child animator aliases.
		inline const CatalogueInput *AliasedSourceInput(const Node &node, std::string_view port) {
			const auto *entry = FindCatalogueEntry(node.Type);
			// Cooked HLSL retains source input animators through its renderer host route.
			if (!entry || (!HasNativeExecutor(node.Type) && node.Type != "pc.hlsl")) return nullptr;
			const auto *input = FindCatalogueInput(*entry, port);
			if (!input) {
				size_t group;
				input = FindDynamicTemplate(*entry, port, group);
				if (std::none_of(node.DynamicInputs.begin(), node.DynamicInputs.end(), [&](const auto &item) {
						return item.Id == port;
					}))
					return nullptr;
			}
			return input && (input->SourceIndex >= 0 ||
							 (node.Type == "pc.group_input" && port == "parent_value"))
					   ? input
					   : nullptr;
		}
		inline uint64_t DetachedAnimatorBytes(const DetachedSourceAnimator &value) {
			const auto text = [](std::string_view s) {
				return std::max(s.size(), std::string{}.capacity()) + 1;
			};
			uint64_t bytes = text(value.Id) + text(value.OwnerId) + text(value.OriginalPort);
			if (value.Track)
				bytes += text(value.Track->NodeId) + text(value.Track->Port) + text(value.Track->End);
			return bytes;
		}
		inline uint64_t SeparatedOverlayBytes(const GroupSubtypeOverlay &overlay) {
			return overlay.SeparatedVec2
					   ? SeparatedAnimatorBytes(*overlay.SeparatedVec2, true).value_or(UINT64_MAX)
					   : 0;
		}
		std::unique_ptr<GroupReplayAccess::Owner> CloneGroupReplay(
			const GroupReplayState &previous,
			size_t extraSlots,
			uint64_t authoringRevision,
			uint64_t maximumBytes,
			uint64_t destinationBytes,
			Diagnostic &diagnostic,
			size_t extraDetachedSlots = 0
		);
		bool ApplyGroupRefreshContext(
			NodeContext &context,
			const GroupRefreshEvent &event,
			GroupReplayAccess::Owner &candidate,
			const Document &document
		);
	}
}
