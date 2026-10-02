#include <engine/imagegraph/Catalogue.hpp>

#include <algorithm>
#include <studio/ImageGraph.hpp>
#include <unordered_set>

namespace studio {
	bool SetSourceImageGraphDynamicGroupCount(
		engine::imagegraph::Document &document,
		std::string_view nodeId,
		size_t groups,
		engine::imagegraph::Diagnostic &error
	) try {
		using namespace engine::imagegraph;
		error = {};
		const auto fail = [&](Status status, std::string message) {
			error = {status, std::string(nodeId), {}, std::move(message)};
			return false;
		};
		const auto found = std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &node) {
			return node.Id == nodeId;
		});
		if (found == document.Nodes.end()) return fail(Status::UnknownNode, "node does not exist");
		const auto *entry = FindCatalogueEntry(found->Type);
		if (!entry || entry->DynamicGroupLength <= 0 || entry->DynamicTemplate.empty())
			return fail(Status::UnsupportedExecution, "node has no source input group template");
		const size_t width = entry->DynamicTemplate.size();
		const size_t maximum = MaximumDynamicInputsForType(found->Type) / width;
		if (groups > maximum || (entry->DynamicGroupLimit > 0 && groups > size_t(entry->DynamicGroupLimit)))
			return fail(Status::LimitExceeded, "source input group count exceeds its limit");
		size_t oldGroups = 0;
		std::unordered_set<std::string> oldIds;
		for (const auto &input : found->DynamicInputs) {
			size_t group = 0;
			if (!FindDynamicTemplate(*entry, input.Id, group) || group >= maximum ||
				!oldIds.insert(input.Id).second)
				return fail(Status::InvalidValue, "source input group identities do not match the template");
			oldGroups = std::max(oldGroups, group + 1);
		}
		for (size_t group = 0; group < oldGroups; ++group)
			for (const auto &input : entry->DynamicTemplate)
				if (input.SourceIndex >= 0 &&
					!oldIds.contains(std::string(input.Id) + "_" + std::to_string(group)))
					return fail(Status::InvalidValue, "source input groups are incomplete");
		if (oldGroups == groups && oldIds.size() == groups * width) return true;
		const auto bytes = DocumentRetainedPayloadBytes(document);
		if (!bytes || *bytes > Limits::MaximumEvaluationBytes / 2)
			return fail(Status::LimitExceeded, "source group edit copy exceeds live payload budget");
		Document staged = document;
		auto &node = staged.Nodes[size_t(found - document.Nodes.begin())];
		std::unordered_set<std::string> removed;
		for (const auto &input : node.DynamicInputs) {
			size_t group = 0;
			if (FindDynamicTemplate(*entry, input.Id, group) && group >= groups) removed.insert(input.Id);
		}
		std::erase_if(node.DynamicInputs, [&](const auto &input) { return removed.contains(input.Id); });
		std::erase_if(node.Values, [&](const auto &input) { return removed.contains(input.Port); });
		std::erase_if(node.SourceProperties, [&](const auto &input) { return removed.contains(input.Port); });
		std::erase_if(node.SourceInputExpressions, [&](const auto &input) {
			return removed.contains(input.Port);
		});
		for (auto *ports : {&node.InstanceOverrides, &node.SourceAnimatedInputs, &node.SourceStaticInputs})
			std::erase_if(*ports, [&](const auto &port) { return removed.contains(port); });
		std::erase_if(staged.Links, [&](const auto &link) {
			return link.ToNode == nodeId && removed.contains(link.ToPort);
		});
		std::erase_if(staged.Keyframes, [&](const auto &key) {
			return key.NodeId == nodeId && removed.contains(key.Port);
		});
		std::erase_if(staged.Tracks, [&](const auto &track) {
			return track.NodeId == nodeId && removed.contains(track.Port);
		});
		// Keep complete catalogue groups, including attribute controls without physical source slots.
		for (size_t group = 0; group < groups; ++group)
			for (const auto &input : entry->DynamicTemplate) {
				std::string id = std::string(input.Id) + "_" + std::to_string(group);
				if (oldIds.contains(id)) continue;
				node.DynamicInputs.push_back(
					{id, input.Type, IsAuthoredValueType(input.Type) ? CatalogueDefault(input) : std::nullopt}
				);
				if (input.SourceIndex >= 0) {
					// Source controls retain their first raw animator key even in static mode.
					if (node.DynamicInputs.back().Default) {
						if (staged.Keyframes.size() >= Limits::MaximumKeyframes ||
							staged.Tracks.size() >= Limits::MaximumTracks)
							return fail(
								Status::LimitExceeded, "source group animator keys exceed their limit"
							);
						staged.Keyframes.push_back(
							{node.Id, id, 0, *node.DynamicInputs.back().Default, "source", KeyframeEase{}}
						);
						staged.Tracks.push_back({node.Id, id, "wrap", -1});
					}
					node.SourceStaticInputs.push_back(std::move(id));
				}
			}
		staged.FormatVersion = std::max(staged.FormatVersion, 9u);
		const auto afterBytes = DocumentRetainedPayloadBytes(staged);
		if (!afterBytes || *afterBytes > Limits::MaximumEvaluationBytes - *bytes)
			return fail(Status::LimitExceeded, "source group edit exceeds live payload budget");
		document = std::move(staged);
		return true;
	} catch (const std::bad_alloc &) {
		error = {
			engine::imagegraph::Status::LimitExceeded,
			std::string(nodeId),
			{},
			"source group allocation failed"
		};
		return false;
	}
}
