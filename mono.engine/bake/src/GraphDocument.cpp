#include <engine/bake/GraphDocument.hpp>

#include <string>

namespace engine::bake {

	DocumentStatus
	Build(const Document &document, Graph &graph, const SourceResolver &sources, std::string &offender) {
		if (graph.NodeCount() != 0) {
			offender = "graph";
			return DocumentStatus::Refused;
		}

		// **Counted before anything is built**, so a generated document cannot
		// walk a graph up to its limit and fail on the last node with several
		// thousand allocations already made.
		if (document.NodeCount() > Graph::MAXIMUM_NODES) {
			return DocumentStatus::TooManyOperations;
		}

		// Node handles by document position, so a `Connect` can name what an
		// earlier operation produced without either side knowing what the
		// runtime issued.
		std::vector<NodeId> nodes;
		nodes.reserve(document.NodeCount());

		size_t index = 0;
		for (const Operation &operation : document.Operations()) {
			index++;

			// Named the same way in every diagnostic below: the operation's
			// position, then what it was trying to do.
			const auto describe = [&] {
				return std::to_string(index) + " (" + Describe(operation.Kind) + ")";
			};

			if (operation.Kind == OperationKind::Connect) {
				if (operation.From == 0 || operation.From > nodes.size() || operation.To == 0 ||
					operation.To > nodes.size()) {
					offender = describe();
					return DocumentStatus::UnknownNode;
				}
				if (!graph.Connect(nodes[operation.From - 1], nodes[operation.To - 1])) {
					offender = describe();
					return DocumentStatus::Refused;
				}
				continue;
			}

			NodeId added;
			switch (operation.Kind) {
			case OperationKind::AddSource: {
				// **An absent resolver is the same event as an unknown name.**
				// Both mean the bytes are not available, and a document holding
				// a source is not buildable without them either way.
				const std::span<const std::byte> bytes =
					sources ? sources(operation.Text) : std::span<const std::byte>{};
				if (bytes.empty()) {
					offender = operation.Text;
					return DocumentStatus::Refused;
				}
				added = graph.AddSource(operation.Text, bytes);
				break;
			}
			case OperationKind::AddBuiltin:
				added = graph.AddBuiltin(operation.Text);
				break;
			case OperationKind::AddNode:
				if (!IsBareNode(operation.Node)) {
					offender = describe();
					return DocumentStatus::WrongNodeKind;
				}
				added = graph.Add(operation.Node);
				break;
			case OperationKind::AddFit:
				added = graph.AddFit(operation.Number);
				break;
			case OperationKind::AddScale:
				added = graph.AddScale(operation.Amount);
				break;
			case OperationKind::AddResize:
				added = graph.AddResize(operation.Width, operation.Height);
				break;
			case OperationKind::AddRasterize:
				added = graph.AddRasterize(operation.Width, operation.Height);
				break;
			case OperationKind::AddRetime:
				added = graph.AddRetime(operation.Number);
				break;
			case OperationKind::AddWrite:
				added = graph.AddWrite(operation.Text);
				break;
			case OperationKind::Connect:
				break;
			}

			if (!added.IsValid()) {
				offender = operation.Text.empty() ? describe() : operation.Text;
				return DocumentStatus::Refused;
			}

			nodes.push_back(added);
		}

		return DocumentStatus::Ok;
	}
}
