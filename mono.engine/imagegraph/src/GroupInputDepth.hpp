#pragma once

#include <engine/imagegraph/Document.hpp>

#include <algorithm>
#include <string_view>

namespace engine::imagegraph::detail {
	// Draft only. The caller validates the document before using borrowed routes.
	struct GroupInputDepth {
		enum class Kind { Concrete, NodeOutput, AuthoredValue, UnrepresentedInput, Invalid };
		Kind Source = Kind::Invalid;
		int64_t Choice = 3;
		std::string_view GroupId;
		std::string_view NodeId;
		std::string_view Port;
		const Value *Default = nullptr;
	};

	// Preserves saved socket ordering and follows forwarding junctions without copying payloads.
	inline GroupInputDepth FindGroupInputDepth(const Document &document, std::string_view groupId) {
		for (size_t depth = 0; !groupId.empty() && depth < document.Groups.size(); ++depth) {
			auto group =
				std::find_if(document.Groups.begin(), document.Groups.end(), [&](const Group &candidate) {
					return candidate.Id == groupId;
				});
			if (group == document.Groups.end()) return {};
			for (size_t hop = 0; !group->InstanceBase.empty(); ++hop) {
				if (hop >= document.Groups.size()) return {};
				const std::string_view baseId = group->InstanceBase;
				group =
					std::find_if(document.Groups.begin(), document.Groups.end(), [&](const Group &candidate) {
						return candidate.Id == baseId;
					});
				if (group == document.Groups.end()) return {};
			}
			if (group->ColorDepth > 1)
				return {GroupInputDepth::Kind::Concrete, group->ColorDepth, group->Id, {}, {}, nullptr};
			if (group->ColorDepth == 1) {
				groupId = group->ParentId;
				continue;
			}
			if (group->ColorDepth != 0) return {};
			const auto first =
				std::find_if(group->Ports.begin(), group->Ports.end(), [](const GroupPort &port) {
					return port.Direction == PortDirection::Input;
				});
			// Native Input depth without a represented route remains a runtime refusal.
			if (first == group->Ports.end())
				return {GroupInputDepth::Kind::UnrepresentedInput, 3, group->Id, {}, {}, nullptr};
			if (!first->ControlNodeId.empty())
				return {
					GroupInputDepth::Kind::NodeOutput, 3, group->Id, first->ControlNodeId, "value", nullptr
				};
			std::string_view junctionId = first->JunctionId;
			for (size_t hop = 0; hop < document.Junctions.size(); ++hop) {
				const auto junction = std::find_if(
					document.Junctions.begin(), document.Junctions.end(), [&](const Junction &candidate) {
						return candidate.Id == junctionId;
					}
				);
				if (junction == document.Junctions.end()) return {};
				const auto incoming =
					std::find_if(document.Links.begin(), document.Links.end(), [&](const Link &link) {
						return link.ToNode == junctionId && link.ToPort == "value";
					});
				if (incoming == document.Links.end()) {
					if (junction->Default)
						return {
							GroupInputDepth::Kind::AuthoredValue, 3, group->Id, {}, {}, &*junction->Default
						};
					return {GroupInputDepth::Kind::Concrete, 3, group->Id, {}, {}, nullptr};
				}
				const auto forwarding = std::find_if(
					document.Junctions.begin(), document.Junctions.end(), [&](const Junction &candidate) {
						return candidate.Id == incoming->FromNode;
					}
				);
				if (forwarding == document.Junctions.end())
					return {
						GroupInputDepth::Kind::NodeOutput,
						3,
						group->Id,
						incoming->FromNode,
						incoming->FromPort,
						nullptr
					};
				if (incoming->FromPort != "value") return {};
				junctionId = forwarding->Id;
			}
			return {};
		}
		if (!groupId.empty()) return {};
		return {
			GroupInputDepth::Kind::Concrete,
			(document.Project ? document.Project->ColorDepth : 1) + 2,
			{},
			{},
			{},
			nullptr
		};
	}
	// A base overrides local group attributes before ordinary parent inheritance, as getAttribute does.
	inline std::optional<int64_t>
	FindGroupSampling(const Document &document, std::string_view groupId, bool interpolation) {
		for (size_t depth = 0; !groupId.empty() && depth < document.Groups.size(); ++depth) {
			auto group =
				std::find_if(document.Groups.begin(), document.Groups.end(), [&](const Group &candidate) {
					return candidate.Id == groupId;
				});
			if (group == document.Groups.end()) return std::nullopt;
			for (size_t hop = 0; !group->InstanceBase.empty(); ++hop) {
				if (hop >= document.Groups.size()) return std::nullopt;
				const std::string_view base = group->InstanceBase;
				group =
					std::find_if(document.Groups.begin(), document.Groups.end(), [&](const Group &candidate) {
						return candidate.Id == base;
					});
				if (group == document.Groups.end()) return std::nullopt;
			}
			const int64_t value = interpolation ? group->Interpolation : group->Oversample;
			if (value < 0 || value > (interpolation ? 7 : 13)) return std::nullopt;
			if (value != 0) return value;
			groupId = group->ParentId;
		}
		if (!groupId.empty()) return std::nullopt;
		const ProjectSettings defaults;
		const auto &project = document.Project ? *document.Project : defaults;
		return (interpolation ? project.Interpolation : project.Oversample) + 1;
	}

}
