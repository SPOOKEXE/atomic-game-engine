#include "PointerFrameFacts.hpp"

#include "PointerSelectors.hpp"

#include <engine/ecs/Store.hpp>
#include <engine/gui/Registration.hpp>

#include <algorithm>
#include <new>
#include <unordered_set>

namespace engine::gui::detail {
	using ecs::Entity;
	using ecs::NULL_ENTITY;

	namespace {
		// Counts bound temporary storage; this budget limits copied payload,
		// including shaped text and virtual keys. Oversized views are refused.
		constexpr size_t MAX_COMMANDS = 16384;
		constexpr size_t MAX_NODES = 16384;
		constexpr size_t MAX_TRANSFORMS = 4096;
		constexpr size_t MAX_MODAL_SCOPES = 64;
		constexpr size_t MAX_BYTES = 16 * 1024 * 1024;

		template <class T> bool Charge(size_t count, size_t &bytes) {
			if (count > (MAX_BYTES - bytes) / sizeof(T)) return false;
			bytes += count * sizeof(T);
			return true;
		}

		template <class T>
		const T *Value(const std::optional<T> PointerFrameNode::*member, const PointerFrameNode *node) {
			if (node == nullptr) return nullptr;
			const auto &value = node->*member;
			return value ? &*value : nullptr;
		}
	}

	const PointerFrameNode *PointerFrameFacts::Find(Entity entity) const {
		const auto found =
			std::lower_bound(Nodes.begin(), Nodes.end(), entity.Id, [](const auto &node, uint64_t id) {
				return node.Entity.Id < id;
			});
		return found != Nodes.end() && found->Entity == entity ? &*found : nullptr;
	}

	bool PointerFrameFacts::IsDescendant(Entity entity, Entity ancestor) const {
		for (size_t count = 0; entity != NULL_ENTITY && count < Nodes.size(); ++count) {
			if (entity == ancestor) return true;
			const auto *node = Find(entity);
			if (node == nullptr) return false;
			entity = node->Parent;
		}
		return false;
	}

	uint8_t PointerFrameFacts::Reach(Entity entity) const {
		const auto *node = Find(entity);
		return node ? node->Reach : 0;
	}
	bool PointerFrameFacts::IsSpatial(Entity entity) const {
		const auto *node = Find(entity);
		return node && node->SpatialCollector;
	}
	bool PointerFrameFacts::IsEntry(Entity entity) const {
		const auto *node = Find(entity);
		return node && node->TextEntry;
	}
	bool PointerFrameFacts::IsLink(Entity entity) const {
		const auto *node = Find(entity);
		return node && node->NodeLink;
	}
	Entity PointerFrameFacts::Collector(Entity entity) const {
		const auto *node = Find(entity);
		return node ? node->Collector : NULL_ENTITY;
	}
	Entity PointerFrameFacts::Detector(Entity entity) const {
		const auto *node = Find(entity);
		return node ? node->Detector : NULL_ENTITY;
	}
	bool PointerFrameFacts::Allows(Entity entity, Entity collector) const {
		(void)collector;
		collector = Collector(entity);
		const auto found = std::find_if(Modals.begin(), Modals.end(), [collector](const auto &modal) {
			return modal.Collector == collector;
		});
		const Entity modal = found == Modals.end() ? NULL_ENTITY : found->Next;
		return modal == NULL_ENTITY ? !ModalOverflow : IsDescendant(entity, modal);
	}
	const Selection *PointerFrameFacts::SelectionOf(Entity entity) const {
		return Value(&PointerFrameNode::SelectionValue, Find(entity));
	}
	const Element *PointerFrameFacts::ElementOf(Entity entity) const {
		return Value(&PointerFrameNode::ElementValue, Find(entity));
	}
	const Scrolling *PointerFrameFacts::ScrollingOf(Entity entity) const {
		return Value(&PointerFrameNode::ScrollingValue, Find(entity));
	}
	const ScrollState *PointerFrameFacts::ScrollStateOf(Entity entity) const {
		return Value(&PointerFrameNode::ScrollValue, Find(entity));
	}
	const ScrollMotion *PointerFrameFacts::ScrollMotionOf(Entity entity) const {
		return Value(&PointerFrameNode::MotionValue, Find(entity));
	}
	const NodeCanvas *PointerFrameFacts::NodeCanvasOf(Entity entity) const {
		return Value(&PointerFrameNode::CanvasValue, Find(entity));
	}
	const Resolved *PointerFrameFacts::ResolvedOf(Entity entity) const {
		return Value(&PointerFrameNode::ResolvedValue, Find(entity));
	}

	PointerDecision SamplePointerFrameFacts(const PointerFrameFacts &facts, const Pointer &pointer) {
		PointerDecision result;
		result.Sample = pointer;
		result.Store = facts.StoreIdentity;
		result.Valid = false;
		if (!facts.Valid) return result;
		try {
			result.ModalOverflow = facts.ModalOverflow;
			result.Modals.reserve(facts.Modals.size());
			for (const auto &modal : facts.Modals)
				result.Modals.push_back({modal.Collector, {}, modal.Next, {}, {}});
			result.Hovered = PointerPick(facts, pointer);
			if (pointer.Inside && pointer.Down) {
				const auto bar = PointerBar(facts, pointer);
				const auto drag = PointerDrag(facts, pointer);
				const auto canvas = PointerCanvas(facts, pointer);
				result.VisualBar = bar.Target;
				result.VisualBarGrab = bar.Grab;
				result.VisualBarVertical = bar.Vertical;
				result.VisualBarState = PointerScrollState(facts, bar.Target);
				result.VisualDrag = drag.Target;
				result.VisualDetector = drag.Detector;
				result.VisualDragPoint = drag.Point;
				result.VisualDragStart = drag.Start;
				result.VisualDragAngle = drag.Angle;
				result.VisualCanvas = canvas.Target;
				result.VisualCanvasState = PointerScrollState(facts, canvas.Target);
				if (const auto *motion = facts.ScrollMotionOf(canvas.Target))
					result.VisualCanvasOvershoot = motion->Overshoot;
			}
			if (pointer.Inside && pointer.Wheel != 0) {
				const auto wheel = PointerWheel(facts, pointer);
				result.VisualWheel = wheel.Target;
				result.VisualWheelGraph = wheel.Graph;
				result.VisualWheelOrigin = wheel.Origin;
				result.VisualWheelPoint = wheel.Point;
				if (const auto *graph = facts.NodeCanvasOf(wheel.Target)) {
					result.VisualWheelValues = {
						graph->Pan.X, graph->Pan.Y, graph->Zoom, graph->MinimumZoom, graph->MaximumZoom
					};
				} else if (const auto *scrolling = facts.ScrollingOf(wheel.Target)) {
					if (const auto *state = facts.ScrollStateOf(wheel.Target)) {
						result.VisualWheelValues = {
							scrolling->CanvasPosition.X,
							scrolling->CanvasPosition.Y,
							state->CanvasSize.X,
							state->CanvasSize.Y,
							state->WindowSize.X,
							state->WindowSize.Y
						};
						result.VisualWheelFlags = static_cast<uint8_t>(scrolling->Direction);
					}
				}
			}
			if (facts.IsEntry(result.Hovered)) {
				if (pointer.Inside && pointer.Down) {
					const auto *node = facts.Find(result.Hovered);
					result.VisualEntryClearOnFocus = node->EntryClearOnFocus;
					result.VisualEntryHasLabel = node->EntryHasLabel;
					result.VisualEntryText = node->EntryText;
				}
				if (const auto caret = PointerCaret(facts, pointer, result.Hovered))
					result.VisualCaret = *caret;
			}
			size_t keyBytes = 0;
			for (size_t index = facts.CommandCount(); index > 0; --index) {
				const auto &command = facts.CommandAt(index - 1);
				if (!PointerCollectorAllowed(facts, pointer, command.Collector) ||
					!PointerContains(facts, command, pointer.Position))
					continue;
				if (result.Hits.size() == MAX_POINTER_HITS ||
					command.Key.size() > MAX_POINTER_KEY_BYTES - keyBytes)
					return result;
				keyBytes += command.Key.size();
				result.Hits.push_back(
					{command.Source,
					 command.Collector,
					 command.Collection,
					 command.Key,
					 command.Index,
					 command.Bounds,
					 command.Clip,
					 PointerLogical(facts, command.Collector, pointer.Position),
					 command.Rotation,
					 facts.Reach(command.Source)}
				);
			}
			result.Valid = true;
			return result;
		} catch (const std::bad_alloc &) {
			return result;
		}
	}

	bool CapturePointerFrameFacts(
		ecs::Store &store,
		const DrawList &list,
		std::span<const PointerFrameModal> modals,
		bool modalOverflow,
		PointerFrameFacts &out,
		std::span<const Entity> extraRoots
	) {
		out = {};
		if (list.Commands.size() > MAX_COMMANDS || list.Transforms.size() > MAX_TRANSFORMS ||
			modals.size() > MAX_MODAL_SCOPES)
			return false;
		try {
			PointerFrameFacts result;
			result.StoreIdentity = store.Identity();
			result.CanvasSize = list.CanvasSize;
			result.ModalOverflow = modalOverflow;
			size_t bytes = 0;
			if (!Charge<PointerFrameCommand>(list.Commands.size(), bytes) ||
				!Charge<CollectorTransform>(list.Transforms.size(), bytes) ||
				!Charge<PointerFrameModal>(modals.size(), bytes))
				return false;
			result.Commands.reserve(list.Commands.size());
			result.CollectorTransforms = list.Transforms;
			result.Modals.assign(modals.begin(), modals.end());
			std::unordered_set<uint64_t> seen;
			std::vector<Entity> entities;
			bool valid = true;
			const auto add = [&](Entity entity) {
				while (valid && entity != NULL_ENTITY) {
					if (seen.contains(entity.Id)) return;
					if (entities.size() == MAX_NODES || !Charge<PointerFrameNode>(1, bytes) ||
						!Charge<Entity>(3, bytes)) {
						valid = false;
						return;
					}
					seen.insert(entity.Id);
					entities.push_back(entity);
					entity = store.ParentOf(entity);
				}
			};
			for (const auto &command : list.Commands) {
				const bool collectorSpatial = store.Get<SpatialCanvas>(command.Collector) != nullptr;
				if (command.Spatial != collectorSpatial) result.LayerClassificationValid = false;
				if (command.Spatial)
					result.HasSpatialCommands = true;
				else {
					result.HasScreenCommands = true;
					result.ExpectsScreen = true;
				}
				if (!Charge<char>(command.Key.size(), bytes)) return false;
				PointerFrameCommand copy;
				copy.Kind = command.Kind;
				copy.Spatial = command.Spatial;
				copy.Source = command.Source;
				copy.Collector = command.Collector;
				copy.Collection = command.Collection;
				copy.Key = command.Key;
				copy.Index = command.Index;
				copy.Bounds = command.Bounds;
				copy.Clip = command.Clip;
				copy.Rotation = command.Rotation;
				if (command.Kind == DrawKind::Text && store.Get<Entry>(command.Source) != nullptr) {
					const auto &shape = command.Shaping;
					if (!Charge<ShapedGlyph>(shape.Glyphs.size(), bytes) ||
						!Charge<ShapedLine>(shape.Lines.size(), bytes) ||
						!Charge<ShapedCaretStop>(shape.CaretStops.size(), bytes) ||
						!Charge<uint32_t>(shape.GraphemeBoundaries.size(), bytes))
						return false;
					copy.TextSize = command.TextSize;
					copy.LineHeight = command.LineHeight;
					copy.XAlignment = command.XAlignment;
					copy.YAlignment = command.YAlignment;
					copy.Shaping.Glyphs = shape.Glyphs;
					copy.Shaping.Lines = shape.Lines;
					copy.Shaping.CaretStops = shape.CaretStops;
					copy.Shaping.GraphemeBoundaries = shape.GraphemeBoundaries;
				}
				result.Commands.push_back(std::move(copy));
				add(command.Source);
				add(command.Collector);
			}
			for (const auto &modal : modals) {
				add(modal.Collector);
				add(modal.Next);
			}
			for (const auto &transform : list.Transforms)
				add(transform.Collector);
			for (const Entity root : extraRoots)
				add(root);
			store.Each<const SpatialCanvas>([&](Entity entity, const SpatialCanvas &canvas) {
				if (!valid) return;
				if (result.SpatialCanvases.size() == MAX_TRANSFORMS ||
					!Charge<PointerFrameSpatial>(1, bytes)) {
					valid = false;
					return;
				}
				result.SpatialCanvases.push_back({entity, canvas});
				result.ExpectsSpatial = result.ExpectsSpatial || canvas.Visible;
				add(entity);
			});
			store.Each<const NodeCanvas, const Resolved>(
				[&](Entity entity, const NodeCanvas &, const Resolved &) {
					if (!valid) return;
					if (!Charge<Entity>(1, bytes)) {
						valid = false;
						return;
					}
					result.GraphWheelNodes.push_back(entity);
					add(entity);
				}
			);
			store.Each<const Scrolling, const ScrollState, const Resolved>(
				[&](Entity entity, const Scrolling &, const ScrollState &, const Resolved &) {
					if (!valid) return;
					if (!Charge<Entity>(1, bytes)) {
						valid = false;
						return;
					}
					result.ScrollWheelNodes.push_back(entity);
					add(entity);
				}
			);
			if (!valid) return false;
			RegisterGuiClasses();
			const auto buttonClass = GuiClass("GuiButton");
			const auto collectorClass = GuiClass("LayerCollector");
			result.Nodes.reserve(entities.size());
			for (Entity entity : entities) {
				PointerFrameNode node;
				node.Entity = entity;
				node.Alive = store.Alive(entity);
				node.Parent = store.ParentOf(entity);
				node.SpatialCollector = store.Get<SpatialCanvas>(entity) != nullptr;
				if (const auto *entry = store.Get<Entry>(entity)) {
					node.TextEntry = true;
					node.EntryClearOnFocus = entry->ClearTextOnFocus;
					node.EntryEditable = entry->TextEditable;
					node.EntryMultiline = entry->MultiLine;
					if (const auto *label = store.Get<Label>(entity)) {
						if (label->Text.size() > MAX_POINTER_ENTRY_TEXT_BYTES ||
							!Charge<char>(label->Text.size(), bytes))
							return false;
						node.EntryHasLabel = true;
						node.EntryText = label->Text;
					}
				}
				node.GuiButton = store.IsA(entity, buttonClass);
				node.NodeLink = store.Get<NodeCanvasLink>(entity) != nullptr;
				if (const auto *value = store.Get<Element>(entity)) node.ElementValue = *value;
				if (const auto *value = store.Get<Selection>(entity)) node.SelectionValue = *value;
				if (const auto *value = store.Get<Scrolling>(entity)) node.ScrollingValue = *value;
				if (const auto *value = store.Get<ScrollState>(entity)) node.ScrollValue = *value;
				if (const auto *value = store.Get<ScrollMotion>(entity)) node.MotionValue = *value;
				if (const auto *value = store.Get<NodeCanvas>(entity)) node.CanvasValue = *value;
				if (const auto *value = store.Get<Resolved>(entity)) node.ResolvedValue = *value;
				const bool takes = node.GuiButton || node.TextEntry || node.NodeLink ||
								   (node.ElementValue && node.ElementValue->Active);
				node.Reach = !takes ? 0 : (!node.ElementValue || node.ElementValue->Interactable ? 2 : 1);
				store.EachChild(entity, [&](Entity child) {
					if (node.Detector != NULL_ENTITY) return;
					const auto *detector = store.Get<DragDetector>(child);
					if (detector && detector->Enabled) {
						node.Detector = child;
						node.DetectorValue = *detector;
					}
				});
				Entity ancestor = entity;
				for (size_t depth = 0; ancestor != NULL_ENTITY && depth < 256; ++depth) {
					if (store.IsA(ancestor, collectorClass)) {
						node.Collector = ancestor;
						break;
					}
					ancestor = store.ParentOf(ancestor);
				}
				if (node.Collector != NULL_ENTITY && store.Get<SpatialCanvas>(node.Collector) == nullptr)
					result.ExpectsScreen = true;
				result.Nodes.push_back(std::move(node));
			}
			std::sort(result.Nodes.begin(), result.Nodes.end(), [](const auto &a, const auto &b) {
				return a.Entity.Id < b.Entity.Id;
			});
			result.Valid = true;
			out = std::move(result);
			return true;
		} catch (const std::bad_alloc &) {
			return false;
		}
	}
}
