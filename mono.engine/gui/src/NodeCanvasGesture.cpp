#include "NodeCanvasGesture.hpp"

#include <engine/gui/NodeCanvas.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace engine::gui::detail {

	namespace {
		using ecs::Entity;

		// Pointer travel before a press stops being a click.
		constexpr float CLAIM_DISTANCE = 3.0f;

		std::string TypeText(const core::Name &type) {
			return type.IsValid() ? std::string(type.Text()) : std::string(ang::ANY_TYPE);
		}

		std::string DefinitionOf(std::string_view node) {
			return "gui.node/" + std::string(node);
		}

		ang::Direction DirectionOf(NodePortDirection direction) {
			return direction == NodePortDirection::Input ? ang::Direction::Input : ang::Direction::Output;
		}

		core::Vector2 Centre(const Resolved &resolved) {
			return {
				resolved.AbsolutePosition.X + resolved.AbsoluteSize.X * 0.5f,
				resolved.AbsolutePosition.Y + resolved.AbsoluteSize.Y * 0.5f,
			};
		}

		// The port entity named by a node id, port id and direction, or null.
		Entity
		FindPort(const ecs::Store &store, Entity node, std::string_view port, ang::Direction direction) {
			Entity found = ecs::NULL_ENTITY;
			store.EachChild(node, [&](Entity child) {
				const NodeCanvasPort *state = store.Get<NodeCanvasPort>(child);
				if (found == ecs::NULL_ENTITY && state != nullptr && state->Id.IsValid() &&
					state->Id.Text() == port && DirectionOf(state->Direction) == direction) {
					found = child;
				}
			});
			return found;
		}
	}

	bool PortTypesConnect(std::string_view output, std::string_view input) {
		ang::Registry types;
		for (const std::string_view type : {output, input}) {
			if (type != ang::ANY_TYPE && types.FindType(type) == nullptr) {
				types.DefineType({.Id = ang::TypeId(type)});
			}
		}
		return types.CheckLink(output, input).Ok;
	}

	CanvasGraph MirrorCanvas(const ecs::Store &store, Entity canvas) {
		CanvasGraph mirror;
		const Resolved *frame = store.Get<Resolved>(canvas);
		const core::Vector2 origin = frame != nullptr ? frame->AbsolutePosition : core::Vector2{};
		store.EachDescendant(canvas, [&](Entity entity) {
			const NodeCanvasNode *node = store.Get<NodeCanvasNode>(entity);
			if (node == nullptr || !node->Id.IsValid()) {
				return;
			}
			const std::string id(node->Id.Text());
			// A repeated id is ambiguous to every link; the first one wins here as
			// it does in ConnectNodePorts' uniqueness check.
			if (mirror.Nodes.contains(id)) {
				return;
			}
			ang::NodeDef def{.Id = DefinitionOf(id), .Label = node->Title};
			store.EachChild(entity, [&](Entity child) {
				const NodeCanvasPort *port = store.Get<NodeCanvasPort>(child);
				if (port == nullptr || !port->Id.IsValid()) {
					return;
				}
				const std::string type = TypeText(port->ValueType);
				if (type != ang::ANY_TYPE && mirror.Types.FindType(type) == nullptr) {
					mirror.Types.DefineType({.Id = type});
				}
				const int32_t limit = port->MaxConnections < 0 ? 1 : port->MaxConnections;
				ang::PortDef entry{
					.Name = std::string(port->Id.Text()),
					.Type = type,
					.Optional = true,
					.MaximumLinks =
						static_cast<uint16_t>(std::min<int32_t>(limit, std::numeric_limits<uint16_t>::max())),
				};
				auto &ports = port->Direction == NodePortDirection::Input ? def.Inputs : def.Outputs;
				const bool repeated =
					std::any_of(ports.begin(), ports.end(), [&](const ang::PortDef &existing) {
						return existing.Name == entry.Name;
					});
				if (!repeated) {
					ports.push_back(std::move(entry));
				}
			});
			if (!mirror.Types.DefineNode(std::move(def)).Ok) {
				return;
			}
			const Resolved *placed = store.Get<Resolved>(entity);
			const core::Vector2 position =
				placed != nullptr ? placed->AbsolutePosition - origin : core::Vector2{};
			mirror.Document.Apply(
				ang::edit::AddNode{{.Id = id, .Def = DefinitionOf(id), .Position = position}}
			);
			mirror.Nodes.emplace(id, entity);
		});
		store.EachChild(canvas, [&](Entity child) {
			const NodeCanvasLink *link = store.Get<NodeCanvasLink>(child);
			if (link == nullptr || !link->FromNode.IsValid() || !link->ToNode.IsValid() ||
				!link->FromPort.IsValid() || !link->ToPort.IsValid()) {
				return;
			}
			mirror.Document.Apply(
				ang::edit::AddLink{
					{std::string(link->FromNode.Text()),
					 std::string(link->FromPort.Text()),
					 std::string(link->ToNode.Text()),
					 std::string(link->ToPort.Text())}
				}
			);
		});
		return mirror;
	}

	std::optional<ang::Link> LinkBetween(const ecs::Store &store, Entity output, Entity input) {
		const NodeCanvasPort *from = store.Get<NodeCanvasPort>(output);
		const NodeCanvasPort *to = store.Get<NodeCanvasPort>(input);
		const NodeCanvasNode *source = store.Get<NodeCanvasNode>(store.ParentOf(output));
		const NodeCanvasNode *destination = store.Get<NodeCanvasNode>(store.ParentOf(input));
		if (from == nullptr || to == nullptr || source == nullptr || destination == nullptr) {
			return std::nullopt;
		}
		return ang::Link{
			std::string(source->Id.Text()),
			std::string(from->Id.Text()),
			std::string(destination->Id.Text()),
			std::string(to->Id.Text()),
		};
	}

	NodeCanvasGesture::NodeCanvasGesture(ecs::Store &store, Entity canvas, core::Vector2 point)
		: Target(canvas), Mirror(MirrorCanvas(store, canvas)), Start(point), Pointer(point) {
		const Resolved *frame = store.Get<Resolved>(canvas);
		const NodeCanvas *state = store.Get<NodeCanvas>(canvas);
		Origin = frame != nullptr ? frame->AbsolutePosition : core::Vector2{};
		Interaction = std::make_unique<angview::View>(Mirror.Document, Mirror.Types);
		Interaction->EmptyDragPans = true;
		if (frame != nullptr) {
			Interaction->SetViewport(frame->AbsoluteSize);
		}
		if (state != nullptr) {
			Interaction->SetPanZoom({-state->Pan.X * state->Zoom, -state->Pan.Y * state->Zoom}, state->Zoom);
		}
		// The gui laid these nodes out already; the view hit-tests their real
		// shapes, taken once at the press so a drag does not chase itself.
		angview::Layout shapes;
		for (const auto &[id, entity] : Mirror.Nodes) {
			const Resolved *placed = store.Get<Resolved>(entity);
			if (placed == nullptr) {
				continue;
			}
			angview::NodeLayout layout{
				.Id = id,
				.Bounds = {
					placed->AbsolutePosition - Origin,
					placed->AbsolutePosition + placed->AbsoluteSize - Origin
				},
			};
			store.EachChild(entity, [&](Entity child) {
				const NodeCanvasPort *port = store.Get<NodeCanvasPort>(child);
				const Resolved *where = store.Get<Resolved>(child);
				if (port == nullptr || where == nullptr || !port->Id.IsValid()) {
					return;
				}
				angview::PortLayout entry{
					std::string(port->Id.Text()),
					TypeText(port->ValueType),
					DirectionOf(port->Direction),
					Centre(*where) - Origin
				};
				(port->Direction == NodePortDirection::Input ? layout.Inputs : layout.Outputs)
					.push_back(std::move(entry));
			});
			if (const Element *element = store.Get<Element>(entity)) {
				StartPositions.emplace(id, element->Position);
			}
			shapes.Add(std::move(layout));
		}
		Interaction->GeometrySource = [shapes](angview::Layout &layout) { layout = shapes; };
	}

	bool NodeCanvasGesture::Move(ecs::Store &store, core::Vector2 point) {
		Pointer = point;
		if (!Claimed) {
			const core::Vector2 travel = point - Start;
			if (std::sqrt(travel.Dot(travel)) <= CLAIM_DISTANCE) {
				return false;
			}
			Claimed = true;
			Interaction->PointerDown(angview::PointerButton::Left, Start - Origin);
		}
		Interaction->PointerMove(point - Origin);
		WriteBack(store);
		return true;
	}

	void NodeCanvasGesture::WriteBack(ecs::Store &store) {
		switch (Interaction->Dragging()) {
		case angview::DragKind::Nodes:
			// Nodes move live because the gui draws them from their elements; the
			// view's own release-time move edit is then a no-op to ignore.
			for (const std::string &id : Interaction->DraggedNodes()) {
				const auto entity = Mirror.Nodes.find(id);
				const auto start = StartPositions.find(id);
				Element *element =
					entity != Mirror.Nodes.end() ? store.GetMutable<Element>(entity->second) : nullptr;
				if (element == nullptr || start == StartPositions.end()) {
					continue;
				}
				const core::Vector2 offset = Interaction->DragDelta();
				element->Position = core::UDim2{
					start->second.X.Scale,
					start->second.X.Offset + offset.X,
					start->second.Y.Scale,
					start->second.Y.Offset + offset.Y,
				};
			}
			break;
		case angview::DragKind::Pan:
			if (NodeCanvas *state = store.GetMutable<NodeCanvas>(Target);
				state != nullptr && state->Zoom > 0.0f) {
				const core::Vector2 pan = Interaction->Pan();
				state->Pan = {-pan.X / state->Zoom, -pan.Y / state->Zoom};
			}
			break;
		default:
			break;
		}
	}

	void NodeCanvasGesture::Release(ecs::Store &store, core::Vector2 point) {
		if (!Claimed) {
			return;
		}
		Pointer = point;
		Interaction->PointerMove(point - Origin);
		WriteBack(store);
		Interaction->PointerUp(angview::PointerButton::Left, point - Origin);
		for (const ang::EditRecord &record : Interaction->TakeEdits()) {
			// A wire drop arrives as link edits; moves and pans were already written.
			for (const ang::Edit &change : record.Applied) {
				if (const auto *removed = std::get_if<ang::edit::RemoveLink>(&change)) {
					store.EachChild(Target, [&](Entity child) {
						const NodeCanvasLink *link = store.Get<NodeCanvasLink>(child);
						if (link != nullptr && link->FromNode.Text() == removed->Wire.FromNode &&
							link->FromPort.Text() == removed->Wire.FromPort &&
							link->ToNode.Text() == removed->Wire.ToNode &&
							link->ToPort.Text() == removed->Wire.ToPort) {
							store.DestroyInstance(child);
						}
					});
				}
				if (const auto *added = std::get_if<ang::edit::AddLink>(&change)) {
					const auto from = Mirror.Nodes.find(added->Wire.FromNode);
					const auto to = Mirror.Nodes.find(added->Wire.ToNode);
					if (from == Mirror.Nodes.end() || to == Mirror.Nodes.end()) {
						continue;
					}
					const Entity output =
						FindPort(store, from->second, added->Wire.FromPort, ang::Direction::Output);
					const Entity input =
						FindPort(store, to->second, added->Wire.ToPort, ang::Direction::Input);
					Entity made = ecs::NULL_ENTITY;
					ConnectNodePorts(store, Target, output, input, made);
				}
			}
		}
	}

	NodeCanvasWire NodeCanvasGesture::Wire() const {
		const auto &pulled = Interaction->Wire();
		if (!Claimed || !pulled) {
			return {};
		}
		const angview::NodeLayout *node = Interaction->Geometry().Find(pulled->Node);
		const angview::PortLayout *port = node != nullptr ? node->Find(pulled->Port, pulled->Dir) : nullptr;
		if (port == nullptr) {
			return {};
		}
		const core::Vector2 anchor = Origin + port->Centre;
		const core::Vector2 pointer = Origin + Interaction->ToGraph(pulled->Pointer);
		const bool fromOutput = pulled->Dir == ang::Direction::Output;
		return {
			.Canvas = Target,
			.From = fromOutput ? anchor : pointer,
			.To = fromOutput ? pointer : anchor,
			.OverPort = pulled->Target.has_value(),
			.Accepts = pulled->TargetAccepts,
		};
	}

	uint64_t NodeCanvasGesture::Digest() const {
		uint64_t hash = ang::HashMix(ang::HASH_SEED, Claimed ? 1 : 0);
		hash = ang::HashMix(hash, static_cast<uint64_t>(Interaction->Dragging()));
		for (const float part : {Start.X, Start.Y, Pointer.X, Pointer.Y}) {
			hash = ang::HashMix(hash, std::bit_cast<uint32_t>(part));
		}
		return hash;
	}
}
