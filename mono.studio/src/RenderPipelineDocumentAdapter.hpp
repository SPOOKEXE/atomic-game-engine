#pragma once

#include <engine/core/Name.hpp>
#include <engine/graph/PipelineDocument.hpp>

#include <string>
#include <vector>

namespace studio::render_pipeline {

	// The document projection retains authoring order and records the canvas does
	// not expose. Nodegraph IDs stay on the other side of this private boundary.
	struct Binding {
		std::string Port;
		engine::core::Name Resource;
	};

	struct AuthoredNode {
		engine::core::Name Name;
		engine::core::Name Kind;
		engine::graph::NodeScope Scope = engine::graph::NodeScope::View;
		bool Enabled = true;
		float X = 0.0f;
		float Y = 0.0f;
		bool Moved = false;
		std::vector<Binding> Reads;
		std::vector<Binding> Writes;
		std::vector<engine::graph::NodeParameter> Parameters;
	};

	std::vector<AuthoredNode> AuthoredNodes(const engine::graph::PipelineDocument &document);
	void AppendUncontrolledAuthoringMetadata(
		const engine::graph::PipelineDocument &basis, engine::graph::PipelineDocument &document
	);
}
