#pragma once

#include <engine/imagegraph/Catalogue.hpp>
#include <engine/imagegraph/SourceCommonDispatch.hpp>

#include <algorithm>

namespace engine::imagegraphio::detail {
	// Match the core's derived callback registry, without saving an executable-capability flag.
	inline bool SourceCommonNativeAvailable(const imagegraph::Document &document, std::string_view nativeId) {
		using namespace imagegraph;
		const auto owner = std::find_if(
			document.SourceCommonOwners.begin(), document.SourceCommonOwners.end(), [&](const auto &record) {
				return record.NativeOwnerId == nativeId;
			}
		);
		if (owner == document.SourceCommonOwners.end()) return false;
		SourceCommonDispatch profile;
		if (owner->NativeOwnerKind == SourceCommonNativeOwnerKind::Node) {
			const auto node =
				std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &record) {
					return record.Id == nativeId;
				});
			if (node == document.Nodes.end()) return false;
			const auto *entry = FindCatalogueEntry(node->Type);
			if (!entry || entry->SourceNode != owner->SourceType || !HasNativeExecutor(node->Type))
				return false;
			profile = SourceCommonDispatchProfile(owner->SourceType, true);
		} else if (owner->NativeOwnerKind == SourceCommonNativeOwnerKind::Group) {
			if ((owner->SourceType != "Node_Group" && owner->SourceType != "Node_Collection") ||
				std::none_of(document.Groups.begin(), document.Groups.end(), [&](const auto &group) {
					return group.Id == nativeId;
				}))
				return false;
			profile = SourceCommonDispatchProfile(owner->SourceType, true);
		} else
			return false;
		return profile.Step != SourceCommonStepKind::Unsupported &&
			   profile.Wrapper != SourceCommonWrapperKind::Unsupported;
	}
}
