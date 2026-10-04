#pragma once

#include "ImageGraphImageEdit.hpp"

#include <imgui.h>

namespace studio::detail {
	// Inspector controls use the same borrowed action admission as the surrounding grant panel.
	template <class Action, class Changed>
	void DrawImageGraphImageActions(
		const engine::imagegraph::Node &node,
		int &cacheLayout,
		std::string &message,
		std::vector<engine::imagegraphexport::GraphImageCacheLayoutObservation> &observations,
		const Action &action,
		const Changed &changed
	) {
		if (!SourceImageType(node.Type)) return;

		if (ImGui::Button("Cache live images")) action(engine::imagegraphio::SourceImageAction::Cache);
		ImGui::SameLine();
		if (ImGui::Button("Remove cache")) action(engine::imagegraphio::SourceImageAction::RemoveCache);
		if (node.Type == "pc.image_animated" && ImGui::Button("Match live image count"))
			action(engine::imagegraphio::SourceImageAction::MatchLength);
		const auto data =
			std::find_if(node.SourceProperties.begin(), node.SourceProperties.end(), [](const auto &value) {
				return value.Port == "cache_data";
			});
		const bool nativeLayout =
			std::any_of(node.SourceProperties.begin(), node.SourceProperties.end(), [](const auto &value) {
				return value.Port == "composer_sprite_cache_layout";
			});
		if (data != node.SourceProperties.end() && !nativeLayout) {
			constexpr const char *layouts[] = {
				"RGBA, top row first",
				"BGRA, top row first",
				"RGBA, bottom row first",
				"BGRA, bottom row first"
			};
			ImGui::Combo("Saved cache layout", &cacheLayout, layouts, 4);
			if (ImGui::Button("Use cache layout")) {
				const auto *text = std::get_if<std::string>(&data->Data);
				const auto hash = text ? engine::bake::SpriteCacheDataHash(*text) : std::nullopt;
				if (!hash || cacheLayout < 0 || cacheLayout > 3)
					message = "Saved cache text or layout is invalid.";
				else {
					auto found =
						std::find_if(observations.begin(), observations.end(), [&](const auto &item) {
							return item.NodeId == node.Id;
						});
					if (found == observations.end() && observations.size() == 64)
						message = "Cache layout observations exceed the session limit.";
					else {
						engine::imagegraphexport::GraphImageCacheLayoutObservation observation{
							std::string(node.Id),
							std::string(hash->data(), hash->size()),
							static_cast<engine::bake::SpriteCacheLayout>(cacheLayout)
						};
						if (found == observations.end())
							observations.push_back(std::move(observation));
						else
							*found = std::move(observation);
						message.clear();
						changed();
					}
				}
			}
			if (ImGui::Button("Revoke cache layout")) {
				std::erase_if(observations, [&](const auto &item) { return item.NodeId == node.Id; });
				changed();
			}
		}
	}
}
