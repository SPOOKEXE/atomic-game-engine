#include "RenderPipelineDocumentAdapter.hpp"

#include <engine/graph/PipelineCatalogue.hpp>
#include <engine/graph/RenderGraph.hpp>

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

namespace studio::render_pipeline {
	namespace {
		using engine::core::Name;
		using engine::graph::Edit;
		using engine::graph::EditKind;
		using engine::graph::NodeKindSpec;
		using engine::graph::PipelineDocument;
		using engine::graph::PortSpec;

		const AuthoredNode *FindNode(const std::vector<AuthoredNode> &nodes, Name name) {
			for (const AuthoredNode &node : nodes)
				if (node.Name == name) return &node;
			return nullptr;
		}

		// The node and its kind, or a refusal naming what is missing.
		const NodeKindSpec *NodeKind(
			const std::vector<AuthoredNode> &nodes, Name name, const AuthoredNode *&node, std::string &error
		) {
			node = FindNode(nodes, name);
			if (node == nullptr) {
				error = "no node named '" + std::string(name.Text()) + "'";
				return nullptr;
			}
			const NodeKindSpec *spec = engine::graph::NodeCatalogue::Find(node->Kind);
			if (spec == nullptr) error = "node '" + std::string(name.Text()) + "' has an unregistered kind";
			return spec;
		}

		const PortSpec *FindPort(const std::vector<PortSpec> &ports, Name port) {
			for (const PortSpec &spec : ports)
				if (spec.Name == port) return &spec;
			return nullptr;
		}

		bool HasResource(const PipelineDocument &document, Name name) {
			for (const Edit &edit : document.Edits())
				if (edit.Kind == EditKind::AddResource && edit.Name == name) return true;
			return false;
		}

		// Replaces `document` with `edits` when they replay, which is the one
		// check every edit below shares.
		bool Commit(PipelineDocument &document, const std::vector<Edit> &edits, std::string &error) {
			PipelineDocument next;
			for (const Edit &edit : edits)
				next.Record(edit);
			engine::graph::RenderGraph graph;
			Name offender;
			const auto status = engine::graph::ReplayDocument(next, graph, offender);
			if (status != engine::graph::PipelineDocumentStatus::Ok) {
				error =
					std::string("the edited document does not replay: ") + engine::graph::Describe(status);
				if (offender.IsValid()) error += " '" + std::string(offender.Text()) + "'";
				return false;
			}
			document = std::move(next);
			return true;
		}

		// Binds `port`'s row of `kind` on `node` to `target`, which may be the
		// invalid name for an empty row. A node's rows sit in the contiguous run
		// after its `AddNode`; a row that is missing is added at the end of it.
		std::vector<Edit>
		Bind(const PipelineDocument &document, Name node, EditKind kind, Name port, Name target) {
			std::vector<Edit> edits(document.Edits().begin(), document.Edits().end());
			size_t runEnd = edits.size();
			bool inRun = false;
			for (size_t index = 0; index < edits.size(); ++index) {
				Edit &edit = edits[index];
				if (edit.Kind == EditKind::AddNode) {
					if (inRun) {
						runEnd = index;
						inRun = false;
					}
					if (edit.Name == node) {
						inRun = true;
						runEnd = index + 1;
					}
					continue;
				}
				const bool row = edit.Kind == EditKind::Reads || edit.Kind == EditKind::Writes ||
								 edit.Kind == EditKind::Set;
				if (!inRun) continue;
				if (!row) {
					inRun = false;
					continue;
				}
				runEnd = index + 1;
				if (edit.Kind == kind && edit.Key == port) {
					edit.Target = target;
					return edits;
				}
			}
			Edit row;
			row.Kind = kind;
			row.Key = port;
			row.Target = target;
			edits.insert(edits.begin() + static_cast<ptrdiff_t>(runEnd), row);
			return edits;
		}
	}

	bool AddNode(PipelineDocument &document, Name name, Name kind, float x, float y, std::string &error) {
		if (!name.IsValid()) {
			error = "a node needs a name";
			return false;
		}
		const std::vector<AuthoredNode> nodes = AuthoredNodes(document);
		if (FindNode(nodes, name) != nullptr) {
			error = "a node named '" + std::string(name.Text()) + "' already exists";
			return false;
		}
		const NodeKindSpec *spec = engine::graph::NodeCatalogue::Find(kind);
		if (spec == nullptr) {
			error = "no node kind named '" + std::string(kind.Text()) + "'";
			return false;
		}
		if (!spec->Repeatable)
			for (const AuthoredNode &node : nodes)
				if (node.Kind == kind) {
					error = "kind '" + std::string(kind.Text()) + "' may appear once, and '" +
							std::string(node.Name.Text()) + "' is already one";
					return false;
				}

		std::vector<Edit> edits(document.Edits().begin(), document.Edits().end());
		Edit add;
		add.Kind = EditKind::AddNode;
		add.Name = name;
		add.NodeKind = kind;
		add.Scope = spec->Scope;
		edits.push_back(add);
		for (const PortSpec &port : spec->Inputs) {
			Edit row;
			row.Kind = EditKind::Reads;
			row.Key = port.Name;
			edits.push_back(row);
		}
		for (const PortSpec &port : spec->Outputs) {
			Edit row;
			row.Kind = EditKind::Writes;
			row.Key = port.Name;
			edits.push_back(row);
		}
		Edit move;
		move.Kind = EditKind::Move;
		move.Name = name;
		move.X = x;
		move.Y = y;
		edits.push_back(move);
		return Commit(document, edits, error);
	}

	bool MoveNode(PipelineDocument &document, Name name, float x, float y, std::string &error) {
		if (FindNode(AuthoredNodes(document), name) == nullptr) {
			error = "no node named '" + std::string(name.Text()) + "'";
			return false;
		}
		std::vector<Edit> edits(document.Edits().begin(), document.Edits().end());
		Edit move;
		move.Kind = EditKind::Move;
		move.Name = name;
		move.X = x;
		move.Y = y;
		edits.push_back(move);
		return Commit(document, edits, error);
	}

	bool SetNodeEnabled(PipelineDocument &document, Name name, bool enabled, std::string &error) {
		if (FindNode(AuthoredNodes(document), name) == nullptr) {
			error = "no node named '" + std::string(name.Text()) + "'";
			return false;
		}
		std::vector<Edit> edits(document.Edits().begin(), document.Edits().end());
		Edit enable;
		enable.Kind = EditKind::Enable;
		enable.Name = name;
		enable.Enabled = enabled;
		edits.push_back(enable);
		return Commit(document, edits, error);
	}

	bool ConnectPorts(
		PipelineDocument &document,
		Name from,
		Name output,
		Name to,
		Name input,
		Name &resource,
		std::string &error
	) {
		const std::vector<AuthoredNode> nodes = AuthoredNodes(document);
		const AuthoredNode *source = nullptr;
		const AuthoredNode *sink = nullptr;
		const NodeKindSpec *sourceKind = NodeKind(nodes, from, source, error);
		if (sourceKind == nullptr) return false;
		const NodeKindSpec *sinkKind = NodeKind(nodes, to, sink, error);
		if (sinkKind == nullptr) return false;
		const PortSpec *out = FindPort(sourceKind->Outputs, output);
		if (out == nullptr) {
			error = "'" + std::string(from.Text()) + "' has no output '" + std::string(output.Text()) + "'";
			return false;
		}
		const PortSpec *in = FindPort(sinkKind->Inputs, input);
		if (in == nullptr) {
			error = "'" + std::string(to.Text()) + "' has no input '" + std::string(input.Text()) + "'";
			return false;
		}
		if (out->Kind != in->Kind) {
			error = std::string("cannot wire a ") + engine::graph::Describe(out->Kind) + " output into a " +
					engine::graph::Describe(in->Kind) + " input";
			return false;
		}

		// The output's existing resource, so one output can feed many inputs.
		Name shared;
		for (const Binding &binding : source->Writes)
			if (binding.Port == output.Text() && binding.Resource.IsValid()) shared = binding.Resource;

		PipelineDocument working = document;
		if (!shared.IsValid()) {
			const std::string base = std::string(from.Text()) + "." + std::string(output.Text());
			shared = Name(base);
			for (int suffix = 2; HasResource(working, shared); ++suffix)
				shared = Name(base + "." + std::to_string(suffix));
			std::vector<Edit> edits(working.Edits().begin(), working.Edits().end());
			Edit declare;
			declare.Kind = EditKind::AddResource;
			declare.Name = shared;
			declare.Resource = out->Kind;
			declare.Format = out->Format;
			edits.insert(edits.begin(), declare);
			working = {};
			for (const Edit &edit : edits)
				working.Record(edit);
			working = [&] {
				PipelineDocument bound;
				for (const Edit &edit : Bind(working, from, EditKind::Writes, output, shared))
					bound.Record(edit);
				return bound;
			}();
		}
		if (!Commit(working, Bind(working, to, EditKind::Reads, input, shared), error)) return false;
		document = std::move(working);
		resource = shared;
		return true;
	}

	bool DisconnectPort(PipelineDocument &document, Name node, Name port, std::string &error) {
		const std::vector<AuthoredNode> nodes = AuthoredNodes(document);
		const AuthoredNode *found = nullptr;
		const NodeKindSpec *spec = NodeKind(nodes, node, found, error);
		if (spec == nullptr) return false;
		EditKind kind = EditKind::Reads;
		if (FindPort(spec->Outputs, port) != nullptr) {
			kind = EditKind::Writes;
		} else if (FindPort(spec->Inputs, port) == nullptr) {
			error = "'" + std::string(node.Text()) + "' has no port '" + std::string(port.Text()) + "'";
			return false;
		}
		return Commit(document, Bind(document, node, kind, port, Name{}), error);
	}
}
