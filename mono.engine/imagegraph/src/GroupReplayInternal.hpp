#pragma once

#include "EvaluationBudget.hpp"
#include "NodeExecutors.hpp"

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
			}
		};
		// Attributes have no source input index. They never join child animator aliases.
		inline const CatalogueInput *AliasedSourceInput(const Node &node, std::string_view port) {
			if (node.Type == "pc.group_input" && port == "parent_value") return nullptr;
			const auto *entry = FindCatalogueEntry(node.Type);
			if (!entry || !HasNativeExecutor(node.Type)) return nullptr;
			const auto *input = FindCatalogueInput(*entry, port);
			if (!input) {
				size_t group;
				input = FindDynamicTemplate(*entry, port, group);
				if (std::none_of(node.DynamicInputs.begin(), node.DynamicInputs.end(), [&](const auto &item) {
						return item.Id == port;
					}))
					return nullptr;
			}
			return input && input->SourceIndex >= 0 ? input : nullptr;
		}
		std::unique_ptr<GroupReplayAccess::Owner> CloneGroupReplay(
			const GroupReplayState &previous,
			size_t extraSlots,
			uint64_t authoringRevision,
			uint64_t maximumBytes,
			uint64_t destinationBytes,
			Diagnostic &diagnostic
		);
		bool ApplyGroupRefreshContext(
			NodeContext &context,
			const GroupRefreshEvent &event,
			GroupReplayAccess::Owner &candidate,
			const Document &document
		);
	}
}
