#include <engine/render/GraphRunner.hpp>

#include <algorithm>
#include <utility>

namespace engine::render {

	bool NodeTable::Set(core::Name kind, NodeHandler handler) {
		if (!kind.IsValid() || !handler) {
			return false;
		}
		Handlers[kind.Id()] = std::move(handler);
		return true;
	}

	bool NodeTable::Has(core::Name kind) const {
		return Find(kind) != nullptr;
	}

	const NodeHandler *NodeTable::Find(core::Name kind) const {
		const auto found = Handlers.find(kind.Id());
		return found == Handlers.end() ? nullptr : &found->second;
	}

	std::vector<core::Name> NodeTable::Missing(const graph::RenderGraph &graph) const {
		std::vector<core::Name> missing;
		for (size_t index = 0; index < graph.Count(); index++) {
			const graph::Node *node = graph.Find(graph::NodeId{static_cast<uint32_t>(index + 1)});
			if (node == nullptr || !node->Enabled || Has(node->Kind)) {
				continue;
			}
			if (std::find(missing.begin(), missing.end(), node->Kind) == missing.end()) {
				missing.push_back(node->Kind);
			}
		}
		std::sort(missing.begin(), missing.end(), [](core::Name left, core::Name right) {
			return left.Text() < right.Text();
		});
		return missing;
	}

	size_t NodeTable::Count() const {
		return Handlers.size();
	}

	void NodeTable::Clear() {
		Handlers.clear();
	}

	bool GraphRunner::Run(const graph::RunContext &context) {
		const NodeHandler *handler = Table.Find(context.Kind);
		if (handler == nullptr) {
			Missing = context.Kind;
			return false;
		}
		SubmittedCount++;
		return (*handler)(context);
	}
}
