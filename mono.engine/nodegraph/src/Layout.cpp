// Where everything on a node is. One answer, read by the painter, the hit test
// and the inspector alike.

#include <engine/nodegraph/Layout.hpp>

#include <algorithm>
#include <cmath>
#include <string>

namespace engine::nodegraph {

	namespace {
		// Which output port a thumbnail reads. Empty when there is nothing to
		// draw one from.
		std::string PreviewPortOf(const NodeType &type) {
			if (!type.PreviewPort.empty()) {
				return type.PreviewPort;
			}
			return type.Outputs.empty() ? std::string() : type.Outputs.front().Name;
		}
	}

	std::vector<PortSpec> InputsOf(const Node &node) {
		if (node.Compressed()) {
			std::vector<PortSpec> derived;
			for (const Proxy &proxy : node.Proxies) {
				if (proxy.Input) {
					derived.push_back(PortSpec{proxy.Name, proxy.Type});
				}
			}
			return derived;
		}
		const NodeType *type = NodeTypes::Find(node.Type);
		return type != nullptr ? type->Inputs : std::vector<PortSpec>{};
	}

	std::vector<PortSpec> OutputsOf(const Node &node) {
		if (node.Compressed()) {
			std::vector<PortSpec> derived;
			for (const Proxy &proxy : node.Proxies) {
				if (!proxy.Input) {
					derived.push_back(PortSpec{proxy.Name, proxy.Type});
				}
			}
			return derived;
		}
		const NodeType *type = NodeTypes::Find(node.Type);
		return type != nullptr ? type->Outputs : std::vector<PortSpec>{};
	}

	std::vector<WidgetSpec> WidgetsOf(const Node &node) {
		if (!node.Compressed()) {
			const NodeType *type = NodeTypes::Find(node.Type);
			return type != nullptr ? type->Widgets : std::vector<WidgetSpec>{};
		}

		// **The inner type's schema, re-keyed and re-labelled.** Keeping the
		// range, the step and the option list is what makes a promoted slider
		// still a slider rather than a number box that happens to hold the same
		// value.
		std::vector<WidgetSpec> lifted;
		for (const Promotion &promotion : node.Promoted) {
			if (promotion.Exposed) {
				lifted.push_back(promotion.Spec);
			}
		}
		return lifted;
	}

	Value ValueOf(const Graph &graph, NodeId id, const WidgetSpec &spec) {
		NodeId holder = NO_NODE;
		std::string key;
		if (!ActualWidget(graph, id, spec.Key, holder, key)) {
			return spec.Default;
		}

		const Node *node = graph.Find(holder);
		if (node == nullptr) {
			return spec.Default;
		}
		const auto found = node->Widgets.find(key);

		// **The declared default on a miss, not a zero.** A type that grew a
		// widget after this graph was saved has no value for it, and reading the
		// default is what makes that graph still open with the knob in the right
		// place.
		return found == node->Widgets.end() ? spec.Default : found->second;
	}

	void SetValue(Graph &graph, NodeId id, const std::string &key, const Value &value) {
		NodeId holder = NO_NODE;
		std::string inner;
		if (!ActualWidget(graph, id, key, holder, inner)) {
			return;
		}
		if (Node *node = graph.Find(holder); node != nullptr) {
			node->Widgets[inner] = value;
		}
	}

	bool Actual(
		const Graph &graph,
		NodeId id,
		const std::string &port,
		bool input,
		NodeId &outNode,
		std::string &outPort
	) {
		const Node *node = graph.Find(id);
		if (node == nullptr) {
			return false;
		}

		outNode = id;
		outPort = port;
		if (!node->Compressed()) {
			return true;
		}

		for (const Proxy &proxy : node->Proxies) {
			if (proxy.Input == input && proxy.Name == port) {
				// One hop is enough: a proxy always names a port on a node
				// inside this one, and a compressed node nested inside another
				// is itself resolved when *its* proxy is asked about.
				return Actual(graph, proxy.Inner, proxy.InnerPort, input, outNode, outPort);
			}
		}
		return false;
	}

	bool Standing(
		const Graph &graph,
		NodeId id,
		const std::string &port,
		bool input,
		NodeId depth,
		NodeId &outNode,
		std::string &outPort
	) {
		NodeId walk = id;
		std::string carried = port;

		// Up one fold at a time. The loop is bounded by the nesting, and the
		// counter is the guard against a document whose `inside` lines form a
		// ring — a hand-edited file can, and a hang is the worst way to find out.
		for (int depthGuard = 0; depthGuard < 64; depthGuard++) {
			const Node *node = graph.Find(walk);
			if (node == nullptr) {
				return false;
			}
			if (node->Owner == depth) {
				outNode = walk;
				outPort = carried;
				return true;
			}

			const Node *owner = graph.Find(node->Owner);
			if (owner == nullptr) {
				return false;
			}

			const auto found =
				std::find_if(owner->Proxies.begin(), owner->Proxies.end(), [&](const Proxy &proxy) {
					return proxy.Input == input && proxy.Inner == walk && proxy.InnerPort == carried;
				});
			if (found == owner->Proxies.end()) {
				// Inside a fold that does not expose this port. It is genuinely
				// invisible from here, which is what a compressed node is for.
				return false;
			}

			walk = owner->Id;
			carried = found->Name;
		}
		return false;
	}

	bool ActualWidget(
		const Graph &graph, NodeId id, const std::string &key, NodeId &outNode, std::string &outKey
	) {
		const Node *node = graph.Find(id);
		if (node == nullptr) {
			return false;
		}

		outNode = id;
		outKey = key;
		if (!node->Compressed()) {
			return true;
		}

		for (const Promotion &promotion : node->Promoted) {
			if (promotion.Key == key) {
				return ActualWidget(graph, promotion.Inner, promotion.InnerKey, outNode, outKey);
			}
		}
		return false;
	}

	bool HasPicture(const NodeType &type) {
		if (type.Preview) {
			return true;
		}

		// **Or the wire's, which is the ordinary case now.** A type that never
		// declared a preview still gets a thumbnail when the thing it produces
		// travels on a wire that knows how to draw itself — which is what stops
		// twelve terrain filters each carrying a copy of one function.
		const std::string port = PreviewPortOf(type);
		if (port.empty()) {
			return false;
		}
		for (const PortSpec &declared : type.Outputs) {
			if (declared.Name != port) {
				continue;
			}
			const DataType *carried = DataTypes::Find(declared.Type);
			return carried != nullptr && static_cast<bool>(carried->Preview);
		}
		return false;
	}

	NodeLayout LayoutOf(const Node &node, const Metrics &metrics) {
		NodeLayout layout;

		const NodeType *type = NodeTypes::Find(node.Type);
		layout.Width = type != nullptr ? type->Width : 160.0f;

		float y = metrics.HeaderHeight;
		if (type == nullptr) {
			// A type nobody registered. A header's worth of node, so it can be
			// seen, moved and deleted.
			layout.Height = y + metrics.Padding;
			return layout;
		}

		// **Read from the node, so a compressed one lays out from its derived
		// interface.** Reading the type here is what would make a compressed
		// node draw as an empty box with knobs nobody could click.
		const std::vector<PortSpec> inputs = InputsOf(node);
		const std::vector<PortSpec> outputs = OutputsOf(node);
		const std::vector<WidgetSpec> widgets = WidgetsOf(node);

		// **Collapsed keeps the ports and drops everything else.** A node whose
		// wires vanished with its body would be a graph that stopped being
		// readable at the moment somebody tidied it — which is the opposite of
		// what collapsing is for. They stack tighter than a full row, unlabelled,
		// because a collapsed node is one nobody is currently reading.
		if (node.Collapsed) {
			constexpr float STACK = 12.0f;
			const size_t stacked = std::max(inputs.size(), outputs.size());

			for (size_t row = 0; row < stacked; row++) {
				const float centre = y + STACK * (static_cast<float>(row) + 0.5f);
				if (row < inputs.size()) {
					layout.Ports.push_back(
						PlacedPort{
							inputs[row].Name,
							inputs[row].Type,
							true,
							0.0f,
							centre,
						}
					);
				}
				if (row < outputs.size()) {
					layout.Ports.push_back(
						PlacedPort{
							outputs[row].Name,
							outputs[row].Type,
							false,
							layout.Width,
							centre,
						}
					);
				}
			}

			y += STACK * static_cast<float>(stacked);
			layout.WidgetsTop = y;
			layout.Height = y + metrics.Padding * 0.5f;
			return layout;
		}

		if (!type->Subtitle.empty()) {
			y += metrics.RowHeight * 0.8f;
		}

		y += metrics.Padding * 0.5f;

		// **Ports are paired into rows**, input on the left and output on the
		// right, which is what makes a node with three of each six rows tall
		// instead of twelve. The tail of the longer side gets rows of its own.
		const size_t rows = std::max(inputs.size(), outputs.size());
		for (size_t row = 0; row < rows; row++) {
			const float centre = y + metrics.RowHeight * 0.5f;

			if (row < inputs.size()) {
				layout.Ports.push_back(
					PlacedPort{
						inputs[row].Name,
						inputs[row].Type,
						true,
						0.0f,
						centre,
					}
				);
			}
			if (row < outputs.size()) {
				layout.Ports.push_back(
					PlacedPort{
						outputs[row].Name,
						outputs[row].Type,
						false,
						layout.Width,
						centre,
					}
				);
			}
			y += metrics.RowHeight;
		}

		// **The thumbnail above the widgets and below the ports.** A picture is
		// what the node produced from what is wired into it, so it reads in the
		// same order as the node is thought about — inputs, result, knobs.
		// A compressed node's picture comes from whatever its first drawable
		// output proxy carries — the same rule, asked of the derived interface.
		const auto drawable = [&outputs, type, &node] {
			if (!node.Compressed()) {
				return HasPicture(*type);
			}
			for (const PortSpec &port : outputs) {
				const DataType *carried = DataTypes::Find(port.Type);
				if (carried != nullptr && carried->Preview) {
					return true;
				}
			}
			return false;
		};

		if (drawable()) {
			y += metrics.Padding * 0.5f;
			layout.PreviewTop = y;
			layout.PreviewSide = layout.Width - metrics.Padding * 2.0f;
			y += layout.PreviewSide;
		}

		// **Reserved whether or not it is running**, so a node keeps its height
		// when it starts. A graph that reflowed as it worked would move every
		// node under the pointer at the moment somebody pressed one.
		if (type->Async) {
			y += metrics.Padding * 0.5f;
			layout.ProgressTop = y;
			layout.ProgressHeight = metrics.RowHeight * 0.8f;
			y += layout.ProgressHeight;
		}

		layout.WidgetsTop = y;

		if (!widgets.empty()) {
			y += metrics.Padding * 0.5f;
			for (size_t index = 0; index < widgets.size(); index++) {
				layout.Widgets.push_back(
					PlacedWidget{
						widgets[index].Key,
						index,
						metrics.Padding,
						y,
						layout.Width - metrics.Padding * 2.0f,
						metrics.WidgetHeight - 4.0f,
					}
				);
				y += metrics.WidgetHeight;
			}
		}

		layout.Height = y + metrics.Padding;
		return layout;
	}

	const PlacedPort *PortIn(const NodeLayout &layout, const std::string &name, bool input) {
		const auto found =
			std::find_if(layout.Ports.begin(), layout.Ports.end(), [&](const PlacedPort &port) {
				return port.Input == input && port.Name == name;
			});
		return found == layout.Ports.end() ? nullptr : &*found;
	}

}
