#include "PipelineNodeLookup.hpp"

namespace engine::render::detail {
	const graph::Node *FindFirstNodeByScan(const graph::RenderGraph &graph, core::Name kind) {
		for (uint32_t index = 1; index <= graph.Count(); index++) {
			const graph::Node *node = graph.Find(graph::NodeId{index});
			if (node != nullptr && node->Kind == kind) {
				return node;
			}
		}
		return nullptr;
	}

	const graph::Node *FindEnabledNodeByScan(const graph::RenderGraph &graph, core::Name kind) {
		for (uint32_t index = 1; index <= graph.Count(); index++) {
			const graph::Node *node = graph.Find(graph::NodeId{index});
			if (node != nullptr && node->Enabled && node->Kind == kind) {
				return node;
			}
		}
		if (kind == core::Name("mirror-capture") || kind == core::Name("portal-capture")) {
			return FindEnabledNodeByScan(graph, core::Name("surface-capture"));
		}
		return nullptr;
	}

	void EnabledNodeIndex::Rebuild(const graph::RenderGraph &graph) {
		FirstByKind.clear();
		FirstEnabledByKind.clear();
		FirstByKind.reserve(graph.Count());
		FirstEnabledByKind.reserve(graph.Count());
		for (uint32_t index = 1; index <= graph.Count(); index++) {
			const graph::Node *node = graph.Find(graph::NodeId{index});
			if (node == nullptr) {
				continue;
			}
			const graph::NodeId id{index};
			FirstByKind.try_emplace(node->Kind, id);
			if (node->Enabled) {
				FirstEnabledByKind.try_emplace(node->Kind, id);
			}
		}
	}

	const graph::Node *EnabledNodeIndex::First(const graph::RenderGraph &graph, core::Name kind) const {
		const auto found = FirstByKind.find(kind);
		return found == FirstByKind.end() ? nullptr : graph.Find(found->second);
	}

	const graph::Node *EnabledNodeIndex::Enabled(const graph::RenderGraph &graph, core::Name kind) const {
		const auto found = FirstEnabledByKind.find(kind);
		if (found != FirstEnabledByKind.end()) {
			return graph.Find(found->second);
		}
		if (kind == core::Name("mirror-capture") || kind == core::Name("portal-capture")) {
			return Enabled(graph, core::Name("surface-capture"));
		}
		return nullptr;
	}
}
