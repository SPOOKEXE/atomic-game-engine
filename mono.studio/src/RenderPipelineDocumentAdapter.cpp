#include "RenderPipelineDocumentAdapter.hpp"

namespace studio::render_pipeline {

	std::vector<AuthoredNode> AuthoredNodes(const engine::graph::PipelineDocument &document) {
		using namespace engine::graph;
		std::vector<AuthoredNode> nodes;
		AuthoredNode *current = nullptr;
		for (const Edit &edit : document.Edits()) {
			switch (edit.Kind) {
			case EditKind::AddNode:
				nodes.push_back({});
				nodes.back().Name = edit.Name;
				nodes.back().Kind = edit.NodeKind;
				nodes.back().Scope = edit.Scope;
				current = &nodes.back();
				break;
			case EditKind::Reads:
			case EditKind::Writes:
				if (current != nullptr) {
					auto &bindings = edit.Kind == EditKind::Reads ? current->Reads : current->Writes;
					bindings.push_back({std::string(edit.Key.Text()), edit.Target});
				}
				break;
			case EditKind::Set:
				if (current != nullptr) {
					current->Parameters.push_back({edit.Key, edit.Value});
				}
				break;
			case EditKind::Enable:
				for (AuthoredNode &node : nodes)
					if (node.Name == edit.Name) node.Enabled = edit.Enabled;
				current = nullptr;
				break;
			case EditKind::Move:
				for (AuthoredNode &node : nodes)
					if (node.Name == edit.Name) {
						node.X = edit.X;
						node.Y = edit.Y;
						node.Moved = true;
					}
				break;
			case EditKind::AddResource:
				current = nullptr;
				break;
			case EditKind::Group:
			case EditKind::Comment:
			case EditKind::Mute:
			case EditKind::Preview:
				break;
			}
		}
		return nodes;
	}

	void AppendUncontrolledAuthoringMetadata(
		const engine::graph::PipelineDocument &basis, engine::graph::PipelineDocument &document
	) {
		using namespace engine::graph;
		for (const Edit &edit : basis.Edits())
			if (edit.Kind == EditKind::Group || edit.Kind == EditKind::Comment ||
				edit.Kind == EditKind::Mute || edit.Kind == EditKind::Preview)
				document.Record(edit);
	}
}
