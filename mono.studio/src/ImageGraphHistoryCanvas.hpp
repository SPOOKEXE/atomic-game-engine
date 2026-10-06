#pragma once

#include <studio/ImageGraph.hpp>
#include <type_traits>

namespace studio::detail {
	// grug count canvas payload and id backing. allocator bookkeeping has its own heap profile.
	inline std::optional<uint64_t>
	ImageGraphHistoryCanvasBytes(const nodegraph::Graph &graph, const ImageGraphCanvasIds &ids) {
		uint64_t bytes = sizeof(graph) + sizeof(ids);
		const auto charge = [&](uint64_t count, uint64_t width = 1) {
			const auto maximum = engine::imagegraph::Limits::MaximumEvaluationBytes;
			if (count && width > (maximum - bytes) / count) return false;
			bytes += count * width;
			return true;
		};
		const auto vector = [&](const auto &items) {
			return charge(items.capacity(), sizeof(typename std::decay_t<decltype(items)>::value_type));
		};
		const auto table = [&](const auto &items) {
			return charge(items.bucket_count(), sizeof(void *)) &&
				   charge(items.size(), sizeof(typename std::decay_t<decltype(items)>::value_type));
		};
		const auto ports = [&](const auto &items) {
			if (!vector(items)) return false;
			for (const auto &item : items)
				if (!charge(item.Name.capacity()) || !charge(item.Type.capacity())) return false;
			return true;
		};
		if (!vector(graph.Nodes()) || !vector(graph.Links()) || !vector(graph.Groups()) ||
			!vector(graph.Templates()))
			return {};
		for (const auto &node : graph.Nodes()) {
			if (!charge(node.Type.capacity()) || !charge(node.Label.capacity()) || !table(node.Widgets) ||
				!ports(node.DynamicInputs) || !vector(node.Proxies) || !vector(node.Promoted) ||
				(node.OutputPorts && !ports(*node.OutputPorts)) ||
				(node.InputPorts && !ports(*node.InputPorts)))
				return {};
			for (const auto &[key, value] : node.Widgets)
				if (!charge(key.capacity()) || !charge(value.Text.capacity())) return {};
			for (const auto &proxy : node.Proxies)
				if (!charge(proxy.Name.capacity()) || !charge(proxy.Type.capacity()) ||
					!charge(proxy.InnerPort.capacity()))
					return {};
			for (const auto &item : node.Promoted) {
				if (!charge(item.Key.capacity()) || !charge(item.Label.capacity()) ||
					!charge(item.InnerKey.capacity()) || !charge(item.Spec.Key.capacity()) ||
					!charge(item.Spec.Label.capacity()) || !charge(item.Spec.Default.Text.capacity()) ||
					!vector(item.Spec.Options))
					return {};
				for (const auto &option : item.Spec.Options)
					if (!charge(option.capacity())) return {};
			}
		}
		for (const auto &link : graph.Links())
			if (!charge(link.FromPort.capacity()) || !charge(link.ToPort.capacity())) return {};
		for (const auto &group : graph.Groups())
			if (!charge(group.Title.capacity()) || !vector(group.Members)) return {};
		for (const auto &item : graph.Templates())
			if (!charge(item.Name.capacity()) || !charge(item.Document.capacity())) return {};
		if (!table(ids.ToCanvas) || !table(ids.ToDocument) || !table(ids.GroupsToCanvas) ||
			!table(ids.GroupsToDocument) || !table(ids.OriginalPositions) || !table(ids.EmptyGroups) ||
			!table(ids.IssuedNodeIds) || !table(ids.IssuedGroupIds) || !vector(ids.UnmappedLinks))
			return {};
		for (const auto &[name, id] : ids.ToCanvas)
			if (!charge(name.capacity())) return {};
		for (const auto &[id, name] : ids.ToDocument)
			if (!charge(name.capacity())) return {};
		for (const auto &[name, id] : ids.GroupsToCanvas)
			if (!charge(name.capacity())) return {};
		for (const auto &[id, name] : ids.GroupsToDocument)
			if (!charge(name.capacity())) return {};
		for (const auto &[name, position] : ids.OriginalPositions)
			if (!charge(name.capacity())) return {};
		for (const auto *names : {&ids.EmptyGroups, &ids.IssuedNodeIds, &ids.IssuedGroupIds})
			for (const auto &name : *names)
				if (!charge(name.capacity())) return {};
		for (const auto &link : ids.UnmappedLinks)
			if (!charge(link.FromNode.capacity()) || !charge(link.FromPort.capacity()) ||
				!charge(link.ToNode.capacity()) || !charge(link.ToPort.capacity()))
				return {};
		return bytes;
	}
}
