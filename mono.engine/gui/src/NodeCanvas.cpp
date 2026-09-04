#include <engine/core/Name.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/NodeCanvas.hpp>
#include <engine/gui/Registration.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <vector>

namespace engine::gui {

	namespace {
		using ecs::Entity;

		bool DescendsFrom(const ecs::Store &store, Entity instance, Entity parent) {
			for (Entity cursor = instance; cursor != ecs::NULL_ENTITY; cursor = store.ParentOf(cursor)) {
				if (cursor == parent) {
					return true;
				}
			}
			return false;
		}

		Entity ParentNode(const ecs::Store &store, Entity port) {
			const Entity parent = store.ParentOf(port);
			return store.Get<NodeCanvasNode>(parent) != nullptr ? parent : ecs::NULL_ENTITY;
		}

		const NodeCanvasPort *
		FindDirectPort(const ecs::Store &store, Entity node, core::Name id, NodePortDirection direction) {
			const NodeCanvasPort *found = nullptr;
			store.EachChild(node, [&](Entity child) {
				const NodeCanvasPort *port = store.Get<NodeCanvasPort>(child);
				if (port != nullptr && port->Id == id && port->Direction == direction) {
					found = found == nullptr ? port : nullptr;
				}
			});
			return found;
		}

		bool HasUniqueNodeId(const ecs::Store &store, Entity canvas, Entity node, core::Name id) {
			size_t matches = 0;
			store.EachDescendant(canvas, [&](Entity candidate) {
				const NodeCanvasNode *state = store.Get<NodeCanvasNode>(candidate);
				matches += state != nullptr && state->Id == id ? 1u : 0u;
			});
			return matches == 1 && store.Get<NodeCanvasNode>(node) != nullptr;
		}

		bool HasUniquePortId(
			const ecs::Store &store, Entity node, Entity port, core::Name id, NodePortDirection direction
		) {
			size_t matches = 0;
			store.EachChild(node, [&](Entity candidate) {
				const NodeCanvasPort *state = store.Get<NodeCanvasPort>(candidate);
				matches += state != nullptr && state->Id == id && state->Direction == direction ? 1u : 0u;
			});
			return matches == 1 && store.ParentOf(port) == node;
		}

		bool IsOffsetOnly(const core::UDim2 &value) {
			return value.X.Scale == 0.0f && value.Y.Scale == 0.0f;
		}

		bool HasTopLeftAnchor(const Element &element) {
			return element.AnchorPoint.X == 0.0f && element.AnchorPoint.Y == 0.0f;
		}

	}

	const char *Describe(NodeLinkResult result) {
		switch (result) {
		case NodeLinkResult::Made:
			return "made";
		case NodeLinkResult::NotInCanvas:
			return "both ports must belong to this NodeCanvas";
		case NodeLinkResult::NotAPort:
			return "both endpoints must be NodeCanvasPort instances";
		case NodeLinkResult::WrongDirection:
			return "connections run from an output port to an input port";
		case NodeLinkResult::MissingId:
			return "nodes and ports need stable identifiers";
		case NodeLinkResult::TypeMismatch:
			return "port value types do not match";
		case NodeLinkResult::InvalidConnectionLimit:
			return "input connection limit cannot be negative";
		case NodeLinkResult::InputFull:
			return "input has reached its connection limit";
		case NodeLinkResult::WouldCycle:
			return "that connection would create a cycle";
		}
		return "unknown node link result";
	}

	const char *Describe(NodeBypassResult result) {
		switch (result) {
		case NodeBypassResult::Valid:
			return "bypass mapping is valid";
		case NodeBypassResult::NotANode:
			return "instance is not a node canvas node";
		case NodeBypassResult::NotBypassing:
			return "node bypass mode is not enabled";
		case NodeBypassResult::MissingPort:
			return "bypass mapping names a missing port";
		case NodeBypassResult::WrongDirection:
			return "bypass mapping must run from an input to an output";
		case NodeBypassResult::TypeMismatch:
			return "bypass port value types do not match";
		}
		return "unknown node bypass result";
	}

	bool NodeCanvasTypesCompatible(const core::Name &output, const core::Name &input) {
		static const core::Name any{"any"};
		return output == input || output == any || input == any;
	}

	NodeBypassResult ValidateNodeCanvasBypass(const ecs::Store &store, Entity node) {
		const NodeCanvasNode *state = store.Get<NodeCanvasNode>(node);
		if (state == nullptr) {
			return NodeBypassResult::NotANode;
		}
		if (state->BypassMode != NodeBypassMode::Bypass) {
			return NodeBypassResult::NotBypassing;
		}

		const NodeCanvasPort *input =
			FindDirectPort(store, node, state->BypassInput, NodePortDirection::Input);
		const NodeCanvasPort *output =
			FindDirectPort(store, node, state->BypassOutput, NodePortDirection::Output);
		if (input != nullptr && output != nullptr) {
			return NodeCanvasTypesCompatible(output->ValueType, input->ValueType)
					   ? NodeBypassResult::Valid
					   : NodeBypassResult::TypeMismatch;
		}

		bool hasInput = false;
		bool hasOutput = false;
		store.EachChild(node, [&](Entity child) {
			const NodeCanvasPort *port = store.Get<NodeCanvasPort>(child);
			if (port == nullptr) {
				return;
			}
			hasInput = hasInput || port->Id == state->BypassInput;
			hasOutput = hasOutput || port->Id == state->BypassOutput;
		});
		return hasInput && hasOutput ? NodeBypassResult::WrongDirection : NodeBypassResult::MissingPort;
	}

	size_t NodeCanvasLinks(const ecs::Store &store, Entity canvas, std::vector<Entity> &out) {
		out.clear();
		store.EachChild(canvas, [&](Entity child) {
			if (store.Get<NodeCanvasLink>(child) != nullptr) {
				out.push_back(child);
			}
		});
		return out.size();
	}

	size_t LayoutNodeCanvasGroups(ecs::Store &store, Entity canvas) {
		if (store.Get<NodeCanvas>(canvas) == nullptr) {
			return 0;
		}

		size_t changed = 0;
		store.EachChild(canvas, [&](Entity groupEntity) {
			const NodeCanvasGroup *group = store.Get<NodeCanvasGroup>(groupEntity);
			const Element *groupElement = store.Get<Element>(groupEntity);
			if (group == nullptr || groupElement == nullptr || group->Layout == NodeGroupLayout::Manual ||
				!IsOffsetOnly(groupElement->Position) || !HasTopLeftAnchor(*groupElement)) {
				return;
			}

			std::vector<Entity> nodes;
			float left = std::numeric_limits<float>::infinity();
			float top = std::numeric_limits<float>::infinity();
			float right = -std::numeric_limits<float>::infinity();
			float bottom = -std::numeric_limits<float>::infinity();
			bool supported = true;
			store.EachChild(groupEntity, [&](Entity nodeEntity) {
				const Element *nodeElement = store.Get<Element>(nodeEntity);
				if (store.Get<NodeCanvasNode>(nodeEntity) == nullptr || nodeElement == nullptr) {
					return;
				}
				if (!IsOffsetOnly(nodeElement->Position) || !IsOffsetOnly(nodeElement->Size) ||
					!HasTopLeftAnchor(*nodeElement)) {
					supported = false;
					return;
				}
				nodes.push_back(nodeEntity);
				left = std::min(left, nodeElement->Position.X.Offset);
				top = std::min(top, nodeElement->Position.Y.Offset);
				right = std::max(right, nodeElement->Position.X.Offset + nodeElement->Size.X.Offset);
				bottom = std::max(bottom, nodeElement->Position.Y.Offset + nodeElement->Size.Y.Offset);
			});
			if (!supported || nodes.empty()) {
				return;
			}

			const core::Vector2 padding =
				group->Layout == NodeGroupLayout::AroundEdge ? group->Padding : core::Vector2{};
			Element nextGroup = *groupElement;
			nextGroup.Position.X.Offset += left - padding.X;
			nextGroup.Position.Y.Offset += top - padding.Y;
			nextGroup.Size =
				core::UDim2{0.0f, right - left + 2.0f * padding.X, 0.0f, bottom - top + 2.0f * padding.Y};
			store.Set(groupEntity, nextGroup);

			for (const Entity nodeEntity : nodes) {
				Element nextNode = *store.Get<Element>(nodeEntity);
				nextNode.Position.X.Offset -= left - padding.X;
				nextNode.Position.Y.Offset -= top - padding.Y;
				store.Set(nodeEntity, nextNode);
			}
			changed++;
		});
		return changed;
	}

	size_t LayoutNodeCanvasPorts(ecs::Store &store, Entity canvas) {
		if (store.Get<NodeCanvas>(canvas) == nullptr) {
			return 0;
		}

		size_t changed = 0;
		store.Each<NodeCanvasNode>([&](Entity nodeEntity, const NodeCanvasNode &node) {
			if (node.InputLayout == InputPortLayout::Manual || !DescendsFrom(store, nodeEntity, canvas)) {
				return;
			}
			const Element *nodeElement = store.Get<Element>(nodeEntity);
			if (nodeElement == nullptr || !IsOffsetOnly(nodeElement->Size) ||
				!HasTopLeftAnchor(*nodeElement)) {
				return;
			}

			std::array<std::vector<Entity>, 3> ports;
			bool supported = true;
			store.EachChild(nodeEntity, [&](Entity portEntity) {
				const NodeCanvasPort *port = store.Get<NodeCanvasPort>(portEntity);
				const Element *portElement = store.Get<Element>(portEntity);
				if (port == nullptr || port->Direction != NodePortDirection::Input) {
					return;
				}
				if (portElement == nullptr || !IsOffsetOnly(portElement->Size) ||
					!HasTopLeftAnchor(*portElement) || static_cast<size_t>(port->Edge) >= ports.size()) {
					supported = false;
					return;
				}
				ports[static_cast<size_t>(port->Edge)].push_back(portEntity);
			});
			if (!supported) {
				return;
			}

			for (size_t edgeIndex = 0; edgeIndex < ports.size(); edgeIndex++) {
				const NodePortEdge edge = static_cast<NodePortEdge>(edgeIndex);
				const std::vector<Entity> &onEdge = ports[edgeIndex];
				for (size_t index = 0; index < onEdge.size(); index++) {
					const Entity portEntity = onEdge[index];
					Element next = *store.Get<Element>(portEntity);
					const float width = next.Size.X.Offset;
					const float height = next.Size.Y.Offset;
					const float count = static_cast<float>(onEdge.size());
					const float slot = static_cast<float>(index);
					if (node.InputLayout == InputPortLayout::Separate) {
						if (edge == NodePortEdge::Top || edge == NodePortEdge::Bottom) {
							next.Position.X.Offset =
								(nodeElement->Size.X.Offset - width) * (slot + 1.0f) / (count + 1.0f);
							next.Position.Y.Offset =
								edge == NodePortEdge::Top ? 0.0f : nodeElement->Size.Y.Offset - height;
						} else {
							next.Position.X.Offset = 0.0f;
							next.Position.Y.Offset =
								(nodeElement->Size.Y.Offset - height) * (slot + 1.0f) / (count + 1.0f);
						}
					} else {
						constexpr float GAP = 2.0f;
						constexpr float MARGIN = 8.0f;
						if (edge == NodePortEdge::Top || edge == NodePortEdge::Bottom) {
							next.Position.X.Offset = MARGIN + slot * (width + GAP);
							next.Position.Y.Offset =
								edge == NodePortEdge::Top ? 0.0f : nodeElement->Size.Y.Offset - height;
						} else {
							next.Position.X.Offset = 0.0f;
							next.Position.Y.Offset = MARGIN + slot * (height + GAP);
						}
					}
					store.Set(portEntity, next);
					changed++;
				}
			}
		});
		return changed;
	}

	NodeLinkResult
	ConnectNodePorts(ecs::Store &store, Entity canvas, Entity output, Entity input, Entity &link) {
		link = ecs::NULL_ENTITY;
		if (store.Get<NodeCanvas>(canvas) == nullptr || !DescendsFrom(store, output, canvas) ||
			!DescendsFrom(store, input, canvas)) {
			return NodeLinkResult::NotInCanvas;
		}

		const NodeCanvasPort *from = store.Get<NodeCanvasPort>(output);
		const NodeCanvasPort *to = store.Get<NodeCanvasPort>(input);
		const Entity fromNode = ParentNode(store, output);
		const Entity toNode = ParentNode(store, input);
		const NodeCanvasNode *source = store.Get<NodeCanvasNode>(fromNode);
		const NodeCanvasNode *destination = store.Get<NodeCanvasNode>(toNode);
		if (from == nullptr || to == nullptr || source == nullptr || destination == nullptr) {
			return NodeLinkResult::NotAPort;
		}
		if (from->Direction != NodePortDirection::Output || to->Direction != NodePortDirection::Input) {
			return NodeLinkResult::WrongDirection;
		}
		if (!source->Id.IsValid() || !destination->Id.IsValid() || !from->Id.IsValid() || !to->Id.IsValid()) {
			return NodeLinkResult::MissingId;
		}
		if (!HasUniqueNodeId(store, canvas, fromNode, source->Id) ||
			!HasUniqueNodeId(store, canvas, toNode, destination->Id) ||
			!HasUniquePortId(store, fromNode, output, from->Id, from->Direction) ||
			!HasUniquePortId(store, toNode, input, to->Id, to->Direction)) {
			return NodeLinkResult::MissingId;
		}
		if (!NodeCanvasTypesCompatible(from->ValueType, to->ValueType)) {
			return NodeLinkResult::TypeMismatch;
		}
		if (to->MaxConnections < 0) {
			return NodeLinkResult::InvalidConnectionLimit;
		}

		std::vector<Entity> links;
		NodeCanvasLinks(store, canvas, links);
		size_t inputConnections = 0;
		for (const Entity existing : links) {
			const NodeCanvasLink *wire = store.Get<NodeCanvasLink>(existing);
			if (wire != nullptr && wire->FromNode == source->Id && wire->FromPort == from->Id &&
				wire->FromDirection == from->Direction && wire->ToNode == destination->Id &&
				wire->ToPort == to->Id && wire->ToDirection == to->Direction) {
				link = existing;
				return NodeLinkResult::Made;
			}
			if (wire != nullptr && wire->ToNode == destination->Id && wire->ToPort == to->Id &&
				wire->ToDirection == to->Direction) {
				inputConnections++;
			}
		}
		if (to->MaxConnections > 1 && inputConnections >= static_cast<size_t>(to->MaxConnections)) {
			return NodeLinkResult::InputFull;
		}
		std::vector<core::Name> pending{destination->Id};
		std::vector<core::Name> seen;
		while (!pending.empty()) {
			const core::Name node = pending.back();
			pending.pop_back();
			if (std::find(seen.begin(), seen.end(), node) != seen.end()) {
				continue;
			}
			seen.push_back(node);
			if (node == source->Id) {
				return NodeLinkResult::WouldCycle;
			}
			for (const Entity existing : links) {
				const NodeCanvasLink *wire = store.Get<NodeCanvasLink>(existing);
				if (wire != nullptr && wire->FromNode == node) {
					pending.push_back(wire->ToNode);
				}
			}
		}

		RegisterGuiClasses();
		link = store.CreateInstance(GuiClass("NodeCanvasLink"), "Link");
		if (link == ecs::NULL_ENTITY || !store.SetParent(link, canvas)) {
			if (link != ecs::NULL_ENTITY) {
				store.DestroyInstance(link);
			}
			link = ecs::NULL_ENTITY;
			return NodeLinkResult::NotInCanvas;
		}
		NodeCanvasLink wire;
		wire.FromNode = source->Id;
		wire.FromPort = from->Id;
		wire.FromDirection = from->Direction;
		wire.ToNode = destination->Id;
		wire.ToPort = to->Id;
		wire.ToDirection = to->Direction;
		store.Set(link, wire);

		// Create and parent the replacement before dropping the old wire. A
		// store refusal therefore leaves the graph connected as it was instead
		// of turning an unsuccessful edit into data loss.
		if (to->MaxConnections == 1) {
			for (const Entity existing : links) {
				const NodeCanvasLink *wire = store.Get<NodeCanvasLink>(existing);
				if (wire != nullptr && wire->ToNode == destination->Id && wire->ToPort == to->Id &&
					wire->ToDirection == to->Direction) {
					store.DestroyInstance(existing);
				}
			}
		}
		return NodeLinkResult::Made;
	}

	bool DisconnectNodeInput(ecs::Store &store, Entity canvas, Entity input) {
		const NodeCanvasPort *port = store.Get<NodeCanvasPort>(input);
		const Entity node = ParentNode(store, input);
		const NodeCanvasNode *owner = store.Get<NodeCanvasNode>(node);
		if (store.Get<NodeCanvas>(canvas) == nullptr || port == nullptr || owner == nullptr ||
			port->Direction != NodePortDirection::Input || !DescendsFrom(store, input, canvas)) {
			return false;
		}

		std::vector<Entity> links;
		NodeCanvasLinks(store, canvas, links);
		for (const Entity existing : links) {
			const NodeCanvasLink *wire = store.Get<NodeCanvasLink>(existing);
			if (wire != nullptr && wire->ToNode == owner->Id && wire->ToPort == port->Id &&
				wire->ToDirection == port->Direction) {
				store.DestroyInstance(existing);
				return true;
			}
		}
		return false;
	}
}
