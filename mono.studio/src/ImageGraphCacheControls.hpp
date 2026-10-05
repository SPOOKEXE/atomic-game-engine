#pragma once

#include <engine/imagegraph/CacheGroupReplay.hpp>

#include <algorithm>
#include <imgui.h>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace studio::detail {
	// grug keep editing owner separate from canvas selection. clicked durable ids wait until drawing ends.
	struct ImageGraphCacheGroupEdit {
		std::string OwnerId, OwnerType, PendingMember;
		void Clear() {
			OwnerId.clear();
			OwnerType.clear();
			PendingMember.clear();
		}
		void Toggle(const engine::imagegraph::Node &node) {
			if (OwnerId == node.Id) {
				Clear();
				return;
			}
			std::string id = node.Id, type = node.Type;
			OwnerId = std::move(id);
			OwnerType = std::move(type);
			PendingMember.clear();
		}
		void Reconcile(const engine::imagegraph::Document &document) {
			if (OwnerId.empty()) return;
			const auto owner =
				std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &node) {
					return node.Id == OwnerId && node.Type == OwnerType &&
						   (node.Type == "pc.cache" || node.Type == "pc.cache_array");
				});
			if (owner == document.Nodes.end()) Clear();
		}
		bool QueueClick(std::string_view nodeId) {
			if (OwnerId.empty() || nodeId.empty()) return false;
			if (nodeId == OwnerId)
				Clear();
			else
				PendingMember = nodeId;
			return true;
		}
	};
	inline const engine::imagegraph::CacheGroupReplayOwner *ImageGraphCacheEditingOwner(
		const ImageGraphCacheGroupEdit &edit, const engine::imagegraph::CacheGroupReplayState &groups
	) {
		const auto owner = std::find_if(groups.Owners.begin(), groups.Owners.end(), [&](const auto &item) {
			return item.NodeId == edit.OwnerId;
		});
		return owner == groups.Owners.end() ? nullptr : &*owner;
	}
	// grug widgets return an authored flag request. caller admits history/runtime before publishing it.
	inline std::optional<bool> DrawImageGraphCacheControls(
		const engine::imagegraph::Node &node,
		ImageGraphCacheGroupEdit &edit,
		const engine::imagegraph::CacheGroupReplayState &groups
	) {
		if (node.Type != "pc.cache" && node.Type != "pc.cache_array") return {};
		bool serialize = true, seen = false, valid = true;
		for (const auto &property : node.SourceProperties)
			if (property.Port == "serialize") {
				const auto flag = std::get_if<bool>(&property.Data);
				valid = valid && !seen && flag;
				seen = true;
				if (flag) serialize = *flag;
			}
		std::optional<bool> next;
		ImGui::BeginDisabled(!valid);
		if (ImGui::Checkbox("Serialize", &serialize)) next = serialize;
		ImGui::EndDisabled();
		if (!valid) ImGui::TextUnformatted("Invalid Serialize metadata");
		if (ImGui::Button(edit.OwnerId == node.Id ? "Done editing group" : "Edit group")) edit.Toggle(node);
		if (edit.OwnerId == node.Id) {
			const auto owner = ImageGraphCacheEditingOwner(edit, groups);
			ImGui::Text("Members: %zu", owner ? owner->Members.size() : size_t{0});
			ImGui::TextUnformatted("Click nodes to toggle members.");
		}
		return next;
	}
}
