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

	// Edits a plugin may make to a document. Each one checks the edit against
	// the node catalogue and replays the result, and on refusal fills `error`
	// and leaves the document unchanged.

	// Adds a node of a registered kind, with one empty row per port, placed
	// at `(x, y)`. Refuses a taken name, an unknown kind, and a second node of
	// a kind that is not `Repeatable`.
	bool AddNode(
		engine::graph::PipelineDocument &document,
		engine::core::Name name,
		engine::core::Name kind,
		float x,
		float y,
		std::string &error
	);

	// Places an existing node on the canvas.
	bool MoveNode(
		engine::graph::PipelineDocument &document,
		engine::core::Name name,
		float x,
		float y,
		std::string &error
	);

	// Turns an existing node on or off.
	bool SetNodeEnabled(
		engine::graph::PipelineDocument &document, engine::core::Name name, bool enabled, std::string &error
	);

	// Wires an output port into an input port. The output's resource is reused
	// when it has one; otherwise a resource named `<node>.<port>` is declared
	// with the output's kind and format. Refuses ports of different kinds.
	//
	// @param resource Set to the resource the two ports now share.
	bool ConnectPorts(
		engine::graph::PipelineDocument &document,
		engine::core::Name from,
		engine::core::Name output,
		engine::core::Name to,
		engine::core::Name input,
		engine::core::Name &resource,
		std::string &error
	);

	// Unbinds one port of a node, leaving its row empty.
	bool DisconnectPort(
		engine::graph::PipelineDocument &document,
		engine::core::Name node,
		engine::core::Name port,
		std::string &error
	);
	void AppendUncontrolledAuthoringMetadata(
		const engine::graph::PipelineDocument &basis, engine::graph::PipelineDocument &document
	);
}
