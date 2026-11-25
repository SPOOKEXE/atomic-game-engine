#include <engine/angview/View.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <utility>

namespace engine::angview {

	namespace {
		constexpr float MINIMUM_ZOOM = 0.1f;
		constexpr float MAXIMUM_ZOOM = 4.0f;
		// Pointer travel, in pixels, before a press becomes a drag.
		constexpr float DRAG_THRESHOLD = 3.0f;
		// Pick distance for wires, in pixels.
		constexpr float WIRE_PICK = 6.0f;
		constexpr float FRAME_MARGIN = 40.0f;

		float Length(core::Vector2 vector) {
			return std::sqrt(vector.Dot(vector));
		}

		std::string Lower(std::string_view text) {
			std::string lowered(text);
			std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) {
				return static_cast<char>(std::tolower(c));
			});
			return lowered;
		}

		bool Contains(const std::vector<std::string> &list, std::string_view value) {
			return std::find(list.begin(), list.end(), value) != list.end();
		}

		int Severity(ang::NodeStatus status) {
			switch (status) {
			case ang::NodeStatus::Failed:
			case ang::NodeStatus::Budget:
				return 4;
			case ang::NodeStatus::Waiting:
				return 3;
			case ang::NodeStatus::Skipped:
			case ang::NodeStatus::Cancelled:
				return 2;
			case ang::NodeStatus::Ran:
			case ang::NodeStatus::Cached:
			case ang::NodeStatus::Bypassed:
				return 1;
			case ang::NodeStatus::Idle:
				return 0;
			}
			return 0;
		}

		// Top-level graph node a flattened plan id belongs to ("wrap/inner" -> "wrap").
		std::string_view OuterId(std::string_view id) {
			const size_t slash = id.find('/');
			return slash == std::string_view::npos ? id : id.substr(0, slash);
		}

		std::string FormatBytes(size_t bytes) {
			char text[32];
			const double value = static_cast<double>(bytes);
			if (bytes >= 1024 * 1024) {
				std::snprintf(text, sizeof(text), "%.1f MiB", value / (1024.0 * 1024.0));
			} else if (bytes >= 1024) {
				std::snprintf(text, sizeof(text), "%.1f KiB", value / 1024.0);
			} else {
				std::snprintf(text, sizeof(text), "%zu B", bytes);
			}
			return text;
		}

		// Room around an auto-framed group's members.
		constexpr float GROUP_PADDING = 12.0f;

		std::string FormatMilliseconds(double milliseconds) {
			char text[32];
			std::snprintf(text, sizeof(text), milliseconds < 10.0 ? "%.2f ms" : "%.1f ms", milliseconds);
			return text;
		}
	}

	View::View(ang::Graph &graph, const ang::Registry &registry, Theme theme)
		: Document(graph), Types(registry), Style(std::move(theme)) {}

	void View::SetViewport(core::Vector2 size) {
		if (size.X != ViewportSize.X || size.Y != ViewportSize.Y) {
			ViewportSize = size;
			Dirty();
		}
	}

	void View::SetTheme(Theme theme) {
		Style = std::move(theme);
		LayoutStale = true;
		Dirty();
	}

	void View::GraphChanged() {
		LayoutStale = true;
		// Drop selection entries whose nodes no longer exist.
		std::erase_if(Selected, [this](const std::string &node) {
			return Document.FindNode(node) == nullptr;
		});
		Dirty();
	}

	void View::VisualsChanged() {
		LayoutStale = true;
		Dirty();
	}

	void View::EnsureLayout() {
		// Host geometry can change without a graph edit, so it is always asked.
		if (!GeometrySource && !LayoutStale && LayoutRevision == Document.Revision()) {
			return;
		}
		if (GeometrySource) {
			Shapes.Clear();
			GeometrySource(Shapes);
		} else {
			Shapes.Build(Document, Types, Style, {.Title = NodeTitle, .Image = NodeImages});
		}
		if (Drag == DragKind::Nodes || Drag == DragKind::Group) {
			Shapes.Offset(DragNodes, DragOffset);
		}
		LayoutRevision = Document.Revision();
		LayoutStale = false;
	}

	const Layout &View::Geometry() {
		EnsureLayout();
		return Shapes;
	}

	core::Vector2 View::ToGraph(core::Vector2 view) const {
		return (view - PanOffset) / ZoomScale;
	}

	core::Vector2 View::ToView(core::Vector2 graph) const {
		return graph * ZoomScale + PanOffset;
	}

	void View::SetPanZoom(core::Vector2 pan, float zoom) {
		PanOffset = pan;
		ZoomScale = std::clamp(zoom, MINIMUM_ZOOM, MAXIMUM_ZOOM);
		Dirty();
	}

	bool View::IsSelected(std::string_view node) const {
		return Contains(Selected, node);
	}

	void View::Select(std::vector<std::string> nodes) {
		std::erase_if(nodes, [this](const std::string &node) { return Document.FindNode(node) == nullptr; });
		Selected = std::move(nodes);
		Dirty();
	}

	std::optional<Rect> View::BoxSelection() const {
		if (Drag != DragKind::BoxSelect || !DragMoved) {
			return std::nullopt;
		}
		return Rect{
			{std::min(DragStart.X, DragPointer.X), std::min(DragStart.Y, DragPointer.Y)},
			{std::max(DragStart.X, DragPointer.X), std::max(DragStart.Y, DragPointer.Y)},
		};
	}

	Hit View::HitTest(core::Vector2 view) {
		EnsureLayout();
		const core::Vector2 point = ToGraph(view);
		const float portPick = std::max(Style.PortRadius * 1.6f, 8.0f / ZoomScale);
		const auto &nodes = Shapes.Nodes();
		// Topmost first: later nodes are drawn over earlier ones.
		for (auto node = nodes.rbegin(); node != nodes.rend(); ++node) {
			for (const auto *ports : {&node->Inputs, &node->Outputs}) {
				for (const PortLayout &port : *ports) {
					if (Length(point - port.Centre) <= portPick) {
						return {.Kind = HitKind::Port, .Node = node->Id, .Port = port.Name, .Dir = port.Dir};
					}
				}
			}
			if (node->Bounds.Contains(point)) {
				return {.Kind = HitKind::Node, .Node = node->Id};
			}
		}
		const auto links = Document.Links();
		for (size_t index = links.size(); index-- > 0;) {
			const ang::Link &wire = links[index];
			const NodeLayout *from = Shapes.Find(wire.FromNode);
			const NodeLayout *to = Shapes.Find(wire.ToNode);
			const PortLayout *output =
				from != nullptr ? from->Find(wire.FromPort, ang::Direction::Output) : nullptr;
			const PortLayout *input = to != nullptr ? to->Find(wire.ToPort, ang::Direction::Input) : nullptr;
			if (output == nullptr || input == nullptr) {
				continue;
			}
			const WireCurve curve = CurveBetween(output->Centre, input->Centre, 1.0f);
			if (DistanceToWire(curve, Style.Wires, point) <= WIRE_PICK / ZoomScale) {
				return {.Kind = HitKind::Link, .Link = index};
			}
		}
		for (const ang::Group &group : Document.Groups()) {
			const Rect frame = GroupFrame(group);
			const Rect header{frame.Min, {frame.Max.X, frame.Min.Y + Style.HeaderHeight}};
			if (header.Contains(point)) {
				return {.Kind = HitKind::Group, .Group = group.Id};
			}
		}
		return {};
	}

	// --- Pointer -------------------------------------------------------------------------------

	bool View::PointerDown(PointerButton button, core::Vector2 position, Modifiers modifiers) {
		if (PaletteState.Open) {
			const float row = Style.FontSize * Style.FontScale + 6.0f;
			const core::Vector2 origin = ToView(PaletteState.Position);
			const size_t shown = std::min(PaletteState.Matches.size(), Style.PaletteRows);
			const Rect box{
				origin, {origin.X + Style.PaletteWidth, origin.Y + row * static_cast<float>(shown + 1)}
			};
			if (box.Contains(position)) {
				const auto index = static_cast<size_t>((position.Y - origin.Y) / row);
				if (index >= 1 && index - 1 < shown) {
					AcceptPalette(PaletteState.Matches[index - 1]);
				}
				return true;
			}
			ClosePalette();
		}
		if (Drag != DragKind::None) {
			return true;
		}
		DragButton = button;
		DragStart = position;
		DragPointer = position;
		DragPanStart = PanOffset;
		DragMoved = false;
		DragAdditive = modifiers.Shift;
		if (button == PointerButton::Middle ||
			(button == PointerButton::Left && (modifiers.Alt || SpaceHeld))) {
			Drag = DragKind::Pan;
			return true;
		}
		if (button == PointerButton::Right) {
			if (HitTest(position).Kind == HitKind::None) {
				OpenPalette(position);
				return true;
			}
			return false;
		}
		BeginDrag(position, modifiers);
		return true;
	}

	void View::BeginDrag(core::Vector2 position, Modifiers modifiers) {
		const Hit hit = HitTest(position);
		switch (hit.Kind) {
		case HitKind::Port: {
			PendingWire wire{.Node = hit.Node, .Port = hit.Port, .Dir = hit.Dir, .Pointer = position};
			if (hit.Dir == ang::Direction::Input && !modifiers.Control) {
				// Grabbing a linked input picks its newest wire up off the socket.
				const auto links = Document.Links();
				for (size_t index = links.size(); index-- > 0;) {
					if (links[index].ToNode == hit.Node && links[index].ToPort == hit.Port) {
						PickedUp = links[index];
						wire.Node = links[index].FromNode;
						wire.Port = links[index].FromPort;
						wire.Dir = ang::Direction::Output;
						break;
					}
				}
			}
			wire.Type = ang::PortType(Document, Types, wire.Node, wire.Port, wire.Dir);
			WireState = std::move(wire);
			Drag = DragKind::Wire;
			UpdateWireTarget();
			break;
		}
		case HitKind::Node:
			if (modifiers.Shift) {
				if (IsSelected(hit.Node)) {
					std::erase(Selected, hit.Node);
				} else {
					Selected.push_back(hit.Node);
				}
			} else if (!IsSelected(hit.Node)) {
				Selected = {hit.Node};
			}
			if (IsSelected(hit.Node)) {
				DragNodes = Selected;
				DragOffset = {};
				Drag = DragKind::Nodes;
			}
			break;
		case HitKind::Link:
			if (modifiers.Control) {
				Apply(ang::edit::RemoveLink{Document.Links()[hit.Link]});
				break;
			}
			Drag = DragKind::BoxSelect;
			break;
		case HitKind::Group:
			DragGroup = hit.Group;
			DragNodes.clear();
			for (const ang::NodeInstance &node : Document.Nodes()) {
				if (node.Group == hit.Group) {
					DragNodes.push_back(node.Id);
				}
			}
			DragOffset = {};
			Drag = DragKind::Group;
			break;
		case HitKind::None:
			Drag = EmptyDragPans ? DragKind::Pan : DragKind::BoxSelect;
			break;
		}
		Dirty();
	}

	bool View::PointerMove(core::Vector2 position, Modifiers) {
		DragPointer = position;
		if (Length(position - DragStart) > DRAG_THRESHOLD) {
			DragMoved = true;
		}
		switch (Drag) {
		case DragKind::None: {
			Hit hit = HitTest(position);
			const bool same = hit.Kind == HoverHit.Kind && hit.Node == HoverHit.Node &&
							  hit.Port == HoverHit.Port && hit.Link == HoverHit.Link &&
							  hit.Group == HoverHit.Group;
			if (!same) {
				HoverHit = std::move(hit);
				Dirty();
			}
			return false;
		}
		case DragKind::Pan:
			PanOffset = DragPanStart + (position - DragStart);
			break;
		case DragKind::Nodes:
		case DragKind::Group:
			if (DragMoved) {
				DragOffset = (position - DragStart) / ZoomScale;
				LayoutStale = true;
			}
			break;
		case DragKind::Wire:
			WireState->Pointer = position;
			UpdateWireTarget();
			break;
		case DragKind::BoxSelect:
		case DragKind::Pinch:
			break;
		}
		Dirty();
		return true;
	}

	bool View::PointerUp(PointerButton button, core::Vector2 position, Modifiers modifiers) {
		if (Drag == DragKind::None || button != DragButton) {
			return false;
		}
		PointerMove(position, modifiers);
		const DragKind finished = Drag;
		Drag = DragKind::None;
		switch (finished) {
		case DragKind::Nodes:
			if (DragMoved && (DragOffset.X != 0.0f || DragOffset.Y != 0.0f)) {
				std::vector<ang::Edit> moves;
				for (const std::string &id : DragNodes) {
					if (const ang::NodeInstance *node = Document.FindNode(id)) {
						moves.push_back(ang::edit::MoveNode{id, node->Position + DragOffset});
					}
				}
				Apply(moves);
			}
			break;
		case DragKind::Group:
			if (DragMoved) {
				std::vector<ang::Edit> moves;
				if (const ang::Group *group = Document.FindGroup(DragGroup)) {
					moves.push_back(
						ang::edit::SetGroupFrame{group->Id, group->Position + DragOffset, group->Size}
					);
				}
				for (const std::string &id : DragNodes) {
					if (const ang::NodeInstance *node = Document.FindNode(id)) {
						moves.push_back(ang::edit::MoveNode{id, node->Position + DragOffset});
					}
				}
				Apply(moves);
			}
			DragGroup.clear();
			break;
		case DragKind::BoxSelect: {
			if (!DragMoved) {
				if (!DragAdditive) {
					Selected.clear();
				}
				break;
			}
			EnsureLayout();
			const Rect box{
				ToGraph({std::min(DragStart.X, position.X), std::min(DragStart.Y, position.Y)}),
				ToGraph({std::max(DragStart.X, position.X), std::max(DragStart.Y, position.Y)})
			};
			if (!DragAdditive) {
				Selected.clear();
			}
			for (const NodeLayout &node : Shapes.Nodes()) {
				if (node.Bounds.Overlaps(box) && !IsSelected(node.Id)) {
					Selected.push_back(node.Id);
				}
			}
			break;
		}
		case DragKind::Wire:
			FinishWire();
			break;
		case DragKind::Pan:
		case DragKind::Pinch:
		case DragKind::None:
			break;
		}
		DragNodes.clear();
		DragOffset = {};
		LayoutStale = true;
		Dirty();
		return true;
	}

	void View::UpdateWireTarget() {
		PendingWire &wire = *WireState;
		const Hit hit = HitTest(wire.Pointer);
		wire.Target.reset();
		wire.TargetAccepts = false;
		wire.Reason.clear();
		if (hit.Kind != HitKind::Port || hit.Dir == wire.Dir) {
			return;
		}
		const bool fromOutput = wire.Dir == ang::Direction::Output;
		const ang::Link candidate = fromOutput ? ang::Link{wire.Node, wire.Port, hit.Node, hit.Port}
											   : ang::Link{hit.Node, hit.Port, wire.Node, wire.Port};
		// Judge against the graph as it will be: the picked-up wire gone, and a
		// one-wire input's old wire replaced, as dropping a cable on a socket does.
		ang::Graph probe = Document;
		if (PickedUp) {
			probe.Apply(ang::edit::RemoveLink{*PickedUp});
		}
		const ang::NodeInstance *target = probe.FindNode(candidate.ToNode);
		const auto ports = target != nullptr ? PortsOf(probe, Types, *target, ang::Direction::Input)
											 : std::vector<ang::PortDef>{};
		const auto port = std::find_if(ports.begin(), ports.end(), [&](const ang::PortDef &entry) {
			return entry.Name == candidate.ToPort;
		});
		if (port != ports.end() && port->MaximumLinks == 1) {
			for (const ang::Link &existing : Document.Links()) {
				if (existing.ToNode == candidate.ToNode && existing.ToPort == candidate.ToPort &&
					existing != candidate) {
					probe.Apply(ang::edit::RemoveLink{existing});
				}
			}
		}
		const ang::LinkVerdict verdict = ang::CheckNewLink(probe, Types, candidate);
		wire.Target = hit;
		wire.TargetAccepts = verdict.Ok;
		wire.Reason = verdict.Reason;
	}

	void View::FinishWire() {
		PendingWire wire = std::move(*WireState);
		WireState.reset();
		std::optional<ang::Link> picked = std::move(PickedUp);
		PickedUp.reset();
		if (!wire.Target) {
			if (picked) {
				// Dropping a picked-up wire on nothing disconnects it.
				Apply(ang::edit::RemoveLink{*picked});
				return;
			}
			if (DragMoved) {
				OpenPalette(wire.Pointer);
				PaletteState.Wire = std::move(wire);
				RefreshPalette();
			}
			return;
		}
		if (!wire.TargetAccepts) {
			Error = wire.Reason;
			return;
		}
		const Hit &target = *wire.Target;
		const bool fromOutput = wire.Dir == ang::Direction::Output;
		const ang::Link made = fromOutput ? ang::Link{wire.Node, wire.Port, target.Node, target.Port}
										  : ang::Link{target.Node, target.Port, wire.Node, wire.Port};
		std::vector<ang::Edit> edits;
		if (picked && *picked != made) {
			edits.push_back(ang::edit::RemoveLink{*picked});
		}
		if (picked && *picked == made) {
			return;
		}
		const ang::NodeInstance *to = Document.FindNode(made.ToNode);
		const auto ports = to != nullptr ? PortsOf(Document, Types, *to, ang::Direction::Input)
										 : std::vector<ang::PortDef>{};
		const auto port = std::find_if(ports.begin(), ports.end(), [&](const ang::PortDef &entry) {
			return entry.Name == made.ToPort;
		});
		if (port != ports.end() && port->MaximumLinks == 1) {
			for (const ang::Link &existing : Document.Links()) {
				if (existing.ToNode == made.ToNode && existing.ToPort == made.ToPort &&
					(!picked || existing != *picked)) {
					edits.push_back(ang::edit::RemoveLink{existing});
				}
			}
		}
		edits.push_back(ang::edit::AddLink{made});
		Apply(edits);
	}

	bool View::Wheel(float steps, core::Vector2 position) {
		if (steps == 0.0f) {
			return false;
		}
		ZoomAbout(std::pow(1.1f, steps), position);
		return true;
	}

	void View::ZoomAbout(float factor, core::Vector2 view) {
		const core::Vector2 anchor = ToGraph(view);
		ZoomScale = std::clamp(ZoomScale * factor, MINIMUM_ZOOM, MAXIMUM_ZOOM);
		// Keep the graph point under the cursor where it was.
		PanOffset = view - anchor * ZoomScale;
		Dirty();
	}

	// --- Keys ----------------------------------------------------------------------------------

	bool View::KeyDown(Key key, Modifiers modifiers) {
		if (PaletteState.Open) {
			switch (key) {
			case Key::Escape:
				ClosePalette();
				return true;
			case Key::Enter:
				AcceptPalette();
				return true;
			case Key::Up:
				PaletteState.Highlighted = PaletteState.Highlighted == 0 ? 0 : PaletteState.Highlighted - 1;
				Dirty();
				return true;
			case Key::Down:
				if (PaletteState.Highlighted + 1 < PaletteState.Matches.size()) {
					++PaletteState.Highlighted;
				}
				Dirty();
				return true;
			case Key::Backspace:
				if (!PaletteState.Filter.empty()) {
					PaletteState.Filter.pop_back();
					RefreshPalette();
				}
				return true;
			default:
				return true;
			}
		}
		switch (key) {
		case Key::Space:
			SpaceHeld = true;
			return true;
		case Key::Escape:
			if (Drag == DragKind::None) {
				if (Selected.empty()) {
					return false;
				}
				Selected.clear();
				Dirty();
				return true;
			}
			// Cancel puts everything back: a pan returns, a picked-up wire re-seats.
			if (Drag == DragKind::Pan) {
				PanOffset = DragPanStart;
			}
			Drag = DragKind::None;
			WireState.reset();
			PickedUp.reset();
			DragNodes.clear();
			DragOffset = {};
			LayoutStale = true;
			Dirty();
			return true;
		case Key::Delete:
		case Key::Backspace:
			if (Selected.empty() || Drag != DragKind::None) {
				return false;
			}
			DeleteSelection();
			return true;
		case Key::F:
			Frame();
			return true;
		case Key::A:
			if (!modifiers.Control) {
				return false;
			}
			Selected.clear();
			for (const ang::NodeInstance &node : Document.Nodes()) {
				Selected.push_back(node.Id);
			}
			Dirty();
			return true;
		case Key::D:
			if (!modifiers.Control || Selected.empty() || Drag != DragKind::None) {
				return false;
			}
			DuplicateSelection();
			return true;
		case Key::Tab:
			OpenPalette(DragPointer);
			return true;
		case Key::Enter:
		case Key::Up:
		case Key::Down:
			return false;
		}
		return false;
	}

	bool View::KeyUp(Key key) {
		if (key == Key::Space) {
			SpaceHeld = false;
			return true;
		}
		return false;
	}

	bool View::TextInput(std::string_view text) {
		if (!PaletteState.Open) {
			return false;
		}
		PaletteState.Filter += text;
		RefreshPalette();
		return true;
	}

	void View::DeleteSelection() {
		std::vector<ang::Edit> edits;
		for (const std::string &id : Selected) {
			if (Document.FindNode(id) != nullptr) {
				edits.push_back(ang::edit::RemoveNode{id});
			}
		}
		Selected.clear();
		Apply(edits);
	}

	// Copies the selected nodes and the links between them, offset so the copy
	// is visible, as one edit; the copy becomes the selection.
	void View::DuplicateSelection() {
		constexpr float OFFSET = 24.0f;
		std::vector<ang::Edit> edits;
		std::unordered_map<std::string, std::string> renamed;
		ang::Graph probe = Document;
		for (const std::string &id : Selected) {
			const ang::NodeInstance *node = Document.FindNode(id);
			if (node == nullptr) {
				continue;
			}
			ang::NodeInstance copy = *node;
			copy.Id = probe.NewNodeId(node->Def);
			copy.Position = node->Position + core::Vector2{OFFSET, OFFSET};
			probe.Apply(ang::edit::AddNode{copy});
			renamed.emplace(id, copy.Id);
			edits.push_back(ang::edit::AddNode{std::move(copy)});
		}
		for (const ang::Link &wire : Document.Links()) {
			const auto from = renamed.find(wire.FromNode);
			const auto to = renamed.find(wire.ToNode);
			if (from != renamed.end() && to != renamed.end()) {
				edits.push_back(ang::edit::AddLink{{from->second, wire.FromPort, to->second, wire.ToPort}});
			}
		}
		if (Apply(edits)) {
			Selected.clear();
			for (const auto &[original, copy] : renamed) {
				Selected.push_back(copy);
			}
			std::sort(Selected.begin(), Selected.end());
		}
	}

	Rect View::GroupFrame(const ang::Group &group) {
		if (group.Size.X != 0.0f || group.Size.Y != 0.0f) {
			return {group.Position, group.Position + group.Size};
		}
		EnsureLayout();
		std::vector<std::string> members;
		for (const ang::NodeInstance &node : Document.Nodes()) {
			if (node.Group == group.Id) {
				members.push_back(node.Id);
			}
		}
		if (members.empty()) {
			return {group.Position, group.Position};
		}
		const Rect bounds = Shapes.Bounds(&members);
		return {
			{bounds.Min.X - GROUP_PADDING, bounds.Min.Y - GROUP_PADDING - Style.HeaderHeight},
			{bounds.Max.X + GROUP_PADDING, bounds.Max.Y + GROUP_PADDING},
		};
	}

	std::string View::GroupSelection(std::string label) {
		std::vector<ang::Edit> edits;
		std::string id;
		for (size_t number = 1; id.empty() || Document.FindGroup(id) != nullptr; ++number) {
			id = "group" + std::to_string(number);
		}
		edits.push_back(ang::edit::AddGroup{{.Id = id, .Label = std::move(label)}});
		for (const std::string &node : Selected) {
			if (Document.FindNode(node) != nullptr) {
				edits.push_back(ang::edit::SetNodeGroup{node, id});
			}
		}
		if (edits.size() == 1 || !Apply(edits)) {
			return {};
		}
		return id;
	}

	void View::Frame() {
		EnsureLayout();
		const Rect bounds = Shapes.Bounds(Selected.empty() ? nullptr : &Selected);
		const float width = std::max(1.0f, bounds.Max.X - bounds.Min.X);
		const float height = std::max(1.0f, bounds.Max.Y - bounds.Min.Y);
		const float zoom = std::min(
			(ViewportSize.X - FRAME_MARGIN * 2.0f) / width, (ViewportSize.Y - FRAME_MARGIN * 2.0f) / height
		);
		ZoomScale = std::clamp(std::min(zoom, 1.0f), MINIMUM_ZOOM, MAXIMUM_ZOOM);
		PanOffset = ViewportSize * 0.5f - bounds.Centre() * ZoomScale;
		Dirty();
	}

	// --- Touch ---------------------------------------------------------------------------------

	bool View::TouchDown(uint64_t id, core::Vector2 position) {
		Touches[id] = position;
		if (Touches.size() == 1) {
			PrimaryTouch = id;
			PrimaryTouchActive = true;
			return PointerDown(PointerButton::Left, position);
		}
		if (Touches.size() == 2) {
			// A second finger turns whatever the first started into a pinch.
			WireState.reset();
			PickedUp.reset();
			DragNodes.clear();
			DragOffset = {};
			LayoutStale = true;
			PrimaryTouchActive = false;
			Drag = DragKind::Pinch;
			auto first = Touches.begin();
			auto second = std::next(first);
			PinchCentre = (first->second + second->second) * 0.5f;
			PinchDistance = std::max(1.0f, Length(first->second - second->second));
			Dirty();
		}
		return true;
	}

	bool View::TouchMove(uint64_t id, core::Vector2 position) {
		auto found = Touches.find(id);
		if (found == Touches.end()) {
			return false;
		}
		found->second = position;
		if (Drag == DragKind::Pinch && Touches.size() >= 2) {
			auto first = Touches.begin();
			auto second = std::next(first);
			const core::Vector2 centre = (first->second + second->second) * 0.5f;
			const float distance = std::max(1.0f, Length(first->second - second->second));
			PanOffset = PanOffset + (centre - PinchCentre);
			ZoomAbout(distance / PinchDistance, centre);
			PinchCentre = centre;
			PinchDistance = distance;
			return true;
		}
		if (PrimaryTouchActive && id == PrimaryTouch) {
			return PointerMove(position);
		}
		return false;
	}

	bool View::TouchUp(uint64_t id, core::Vector2 position) {
		if (Touches.erase(id) == 0) {
			return false;
		}
		if (Drag == DragKind::Pinch) {
			if (Touches.size() < 2) {
				Drag = DragKind::None;
				Dirty();
			}
			return true;
		}
		if (PrimaryTouchActive && id == PrimaryTouch) {
			PrimaryTouchActive = false;
			return PointerUp(PointerButton::Left, position);
		}
		return false;
	}

	// --- Palette -------------------------------------------------------------------------------

	void View::OpenPalette(core::Vector2 view) {
		PaletteState = Palette{.Open = true, .Position = ToGraph(view)};
		RefreshPalette();
	}

	void View::ClosePalette() {
		PaletteState = Palette{};
		Dirty();
	}

	void View::RefreshPalette() {
		PaletteState.Matches.clear();
		const std::string filter = Lower(PaletteState.Filter);
		for (const auto &def : Types.Nodes()) {
			const bool named = filter.empty() || Lower(def->Id).find(filter) != std::string::npos ||
							   Lower(def->Label).find(filter) != std::string::npos ||
							   Lower(def->Category).find(filter) != std::string::npos;
			if (!named) {
				continue;
			}
			if (PaletteState.Wire) {
				// Keep definitions with a port that could take the wire.
				const PendingWire &wire = *PaletteState.Wire;
				const bool fromOutput = wire.Dir == ang::Direction::Output;
				const auto &ports = fromOutput ? def->Inputs : def->Outputs;
				const bool fits = std::any_of(ports.begin(), ports.end(), [&](const ang::PortDef &port) {
					if (!port.Generic.empty()) {
						return port.Constraints.empty() ||
							   std::find(port.Constraints.begin(), port.Constraints.end(), wire.Type) !=
								   port.Constraints.end();
					}
					return fromOutput ? Types.CheckLink(wire.Type, port.Type).Ok
									  : Types.CheckLink(port.Type, wire.Type).Ok;
				});
				if (!fits) {
					continue;
				}
			}
			PaletteState.Matches.push_back(def->Id);
		}
		if (PaletteState.Highlighted >= PaletteState.Matches.size()) {
			PaletteState.Highlighted = PaletteState.Matches.empty() ? 0 : PaletteState.Matches.size() - 1;
		}
		Dirty();
	}

	bool View::AcceptPalette(std::string_view def) {
		if (!PaletteState.Open) {
			return false;
		}
		std::string chosen(def);
		if (chosen.empty()) {
			if (PaletteState.Matches.empty()) {
				return false;
			}
			chosen = PaletteState.Matches[PaletteState.Highlighted];
		}
		const auto definition = Types.FindNode(chosen);
		if (definition == nullptr) {
			Error = "unknown node type '" + chosen + "'";
			return false;
		}
		const std::string id = Document.NewNodeId(chosen);
		std::vector<ang::Edit> edits{ang::edit::AddNode{
			{.Id = id, .Def = chosen, .Version = definition->Version, .Position = PaletteState.Position}
		}};
		if (PaletteState.Wire) {
			const PendingWire &wire = *PaletteState.Wire;
			const bool fromOutput = wire.Dir == ang::Direction::Output;
			ang::Graph probe = Document;
			probe.Apply(edits.front());
			for (const auto &port : fromOutput ? definition->Inputs : definition->Outputs) {
				const ang::Link candidate = fromOutput ? ang::Link{wire.Node, wire.Port, id, port.Name}
													   : ang::Link{id, port.Name, wire.Node, wire.Port};
				if (ang::CheckNewLink(probe, Types, candidate).Ok) {
					edits.push_back(ang::edit::AddLink{candidate});
					break;
				}
			}
		}
		const bool applied = Apply(edits);
		if (applied) {
			Selected = {id};
		}
		ClosePalette();
		return applied;
	}

	// --- Edits ---------------------------------------------------------------------------------

	bool View::Apply(const ang::Edit &change) {
		return Apply(std::span<const ang::Edit>(&change, 1));
	}

	bool View::Apply(std::span<const ang::Edit> changes) {
		if (changes.empty()) {
			return true;
		}
		ang::EditResult result = Document.Apply(changes);
		LayoutStale = true;
		Dirty();
		if (!result.Ok) {
			Error = result.Error;
			return false;
		}
		Error.clear();
		Records.push_back(std::move(result.Record));
		std::erase_if(Selected, [this](const std::string &node) {
			return Document.FindNode(node) == nullptr;
		});
		return true;
	}

	std::vector<ang::EditRecord> View::TakeEdits() {
		return std::exchange(Records, {});
	}

	// --- Run feedback --------------------------------------------------------------------------

	void View::SetRunReport(const ang::Plan &plan, const ang::RunReport &report) {
		Statuses.clear();
		for (size_t index = 0; index < plan.Nodes.size() && index < report.Nodes.size(); ++index) {
			const ang::NodeReport &entry = report.Nodes[index];
			NodeStatusView &view = Statuses[std::string(OuterId(plan.Nodes[index].Id))];
			// A flattened subgraph sums into its outer node and shows its worst status.
			view.Milliseconds += entry.Milliseconds;
			view.Bytes += entry.Bytes;
			if (Severity(entry.Status) >= Severity(view.Status)) {
				view.Status = entry.Status;
				if (!entry.Note.empty()) {
					view.Note = entry.Note;
				}
			}
		}
		Dirty();
	}

	void View::SetTimeline(const ang::RunTimeline &timeline) {
		TimelineSamples.clear();
		TimelineMilliseconds = 0.0;
		if (const ang::TimelineRun *latest = timeline.Latest()) {
			TimelineSamples = latest->Samples;
			TimelineMilliseconds = latest->Milliseconds;
		}
		Dirty();
	}

	void View::ClearRunFeedback() {
		Statuses.clear();
		TimelineSamples.clear();
		Dirty();
	}

	const NodeStatusView *View::StatusOf(std::string_view node) const {
		const auto found = Statuses.find(std::string(node));
		return found == Statuses.end() ? nullptr : &found->second;
	}

	// --- Paint ---------------------------------------------------------------------------------

	bool View::NeedsRedraw() const {
		return Changed || LayoutStale || LayoutRevision != Document.Revision();
	}

	const DrawList &View::Paint() {
		if (!NeedsRedraw()) {
			return Commands;
		}
		Commands.clear();
		PaintInto(Commands);
		if (CustomPaint) {
			CustomPaint(Commands, *this);
		}
		Changed = false;
		return Commands;
	}

	Rgba View::WireColour(const ang::TypeId &type) const {
		const ang::DataType *data = Types.FindType(type);
		if (data == nullptr || type == ang::ANY_TYPE) {
			return Style.WireFallback;
		}
		return FromColor3(data->Wire);
	}

	void View::PaintInto(DrawList &list) {
		EnsureLayout();
		const float zoom = ZoomScale;
		const float fontSize = Style.FontSize * Style.FontScale * zoom;
		list.push_back({.Kind = DrawKind::Rect, .A = {}, .B = ViewportSize, .Fill = Style.Background});
		list.push_back({.Kind = DrawKind::PushClip, .A = {}, .B = ViewportSize});

		if (Style.ShowGrid && Style.GridSpacing > 0.0f) {
			const float spacing = Style.GridSpacing * zoom;
			const int major = std::max(1, Style.GridMajorEvery);
			// Minor lines vanish when they would be denser than every 6 pixels.
			const bool minor = spacing >= 6.0f;
			const float step = minor ? spacing : spacing * static_cast<float>(major);
			const auto firstLine = [&](float pan) { return static_cast<long>(std::floor(-pan / step)); };
			for (long line = firstLine(PanOffset.X);; ++line) {
				const float x = PanOffset.X + static_cast<float>(line) * step;
				if (x > ViewportSize.X) {
					break;
				}
				const bool isMajor = !minor || line % major == 0;
				list.push_back(
					{.Kind = DrawKind::Line,
					 .A = {x, 0.0f},
					 .B = {x, ViewportSize.Y},
					 .Stroke = isMajor ? Style.GridMajor : Style.Grid}
				);
			}
			for (long line = firstLine(PanOffset.Y);; ++line) {
				const float y = PanOffset.Y + static_cast<float>(line) * step;
				if (y > ViewportSize.Y) {
					break;
				}
				const bool isMajor = !minor || line % major == 0;
				list.push_back(
					{.Kind = DrawKind::Line,
					 .A = {0.0f, y},
					 .B = {ViewportSize.X, y},
					 .Stroke = isMajor ? Style.GridMajor : Style.Grid}
				);
			}
		}

		for (const ang::Group &group : Document.Groups()) {
			Rect frame = GroupFrame(group);
			// An auto frame follows its members, which the layout already offsets.
			const bool sized = group.Size.X != 0.0f || group.Size.Y != 0.0f;
			if (sized && Drag == DragKind::Group && group.Id == DragGroup) {
				frame = {frame.Min + DragOffset, frame.Max + DragOffset};
			}
			if (frame.Max.X <= frame.Min.X && frame.Max.Y <= frame.Min.Y) {
				continue;
			}
			const core::Vector2 min = ToView(frame.Min);
			const core::Vector2 max = ToView(frame.Max);
			list.push_back(
				{.Kind = DrawKind::Rect,
				 .A = min,
				 .B = max,
				 .Fill = Style.GroupFill,
				 .Stroke = Style.GroupBorder,
				 .Thickness = Style.BorderWidth,
				 .Radius = Style.CornerRadius * zoom}
			);
			list.push_back(
				{.Kind = DrawKind::Text,
				 .A = {min.X + 6.0f * zoom, min.Y + 4.0f * zoom},
				 .Fill = Style.TextDim,
				 .TextSize = fontSize,
				 .Text = group.Label.empty() ? group.Id : group.Label}
			);
		}

		const auto pushWire = [&](core::Vector2 from, core::Vector2 to, Rgba colour, float width) {
			if (Style.Wires == WireStyle::Straight) {
				list.push_back(
					{.Kind = DrawKind::Line, .A = from, .B = to, .Stroke = colour, .Thickness = width}
				);
				return;
			}
			if (Style.Wires == WireStyle::Orthogonal) {
				const float middle = (from.X + to.X) * 0.5f;
				list.push_back(
					{.Kind = DrawKind::Line,
					 .A = from,
					 .B = {middle, from.Y},
					 .Stroke = colour,
					 .Thickness = width}
				);
				list.push_back(
					{.Kind = DrawKind::Line,
					 .A = {middle, from.Y},
					 .B = {middle, to.Y},
					 .Stroke = colour,
					 .Thickness = width}
				);
				list.push_back(
					{.Kind = DrawKind::Line,
					 .A = {middle, to.Y},
					 .B = to,
					 .Stroke = colour,
					 .Thickness = width}
				);
				return;
			}
			const WireCurve curve = CurveBetween(from, to, zoom);
			list.push_back(
				{.Kind = DrawKind::Bezier,
				 .A = curve.From,
				 .B = curve.To,
				 .C = curve.ControlFrom,
				 .D = curve.ControlTo,
				 .Stroke = colour,
				 .Thickness = width}
			);
		};

		const auto links = Document.Links();
		for (size_t index = 0; index < links.size(); ++index) {
			const ang::Link &wire = links[index];
			if (PickedUp && *PickedUp == wire) {
				continue;
			}
			const NodeLayout *from = Shapes.Find(wire.FromNode);
			const NodeLayout *to = Shapes.Find(wire.ToNode);
			const PortLayout *output =
				from != nullptr ? from->Find(wire.FromPort, ang::Direction::Output) : nullptr;
			const PortLayout *input = to != nullptr ? to->Find(wire.ToPort, ang::Direction::Input) : nullptr;
			if (output == nullptr || input == nullptr) {
				continue;
			}
			const bool hovered = HoverHit.Kind == HitKind::Link && HoverHit.Link == index;
			const Rgba colour = hovered ? Style.WireHighlight : WireColour(output->Type);
			pushWire(
				ToView(output->Centre),
				ToView(input->Centre),
				colour,
				Style.WireWidth * (hovered ? 1.5f : 1.0f)
			);
		}

		if (WireState) {
			const NodeLayout *node = Shapes.Find(WireState->Node);
			const PortLayout *port = node != nullptr ? node->Find(WireState->Port, WireState->Dir) : nullptr;
			if (port != nullptr) {
				Rgba colour = WireColour(WireState->Type);
				if (WireState->Target) {
					colour = WireState->TargetAccepts ? Style.PortAccepts : Style.WireInvalid;
				}
				const core::Vector2 anchor = ToView(port->Centre);
				const bool fromOutput = WireState->Dir == ang::Direction::Output;
				pushWire(
					fromOutput ? anchor : WireState->Pointer,
					fromOutput ? WireState->Pointer : anchor,
					colour,
					Style.WireWidth
				);
			}
		}

		for (const NodeLayout &node : Shapes.Nodes()) {
			const core::Vector2 min = ToView(node.Bounds.Min);
			const core::Vector2 max = ToView(node.Bounds.Max);
			if (max.X < 0.0f || max.Y < 0.0f || min.X > ViewportSize.X || min.Y > ViewportSize.Y) {
				continue;
			}
			const ang::NodeInstance *instance = Document.FindNode(node.Id);
			const NodeStatusView *status = StatusOf(node.Id);
			const bool selected = IsSelected(node.Id);
			const bool hovered = HoverHit.Kind == HitKind::Node && HoverHit.Node == node.Id;
			Rgba border = Style.NodeBorder;
			float borderWidth = Style.BorderWidth;
			if (status != nullptr &&
				(status->Status == ang::NodeStatus::Failed || status->Status == ang::NodeStatus::Budget)) {
				border = Style.NodeFailed;
			} else if (status != nullptr && status->Status == ang::NodeStatus::Waiting) {
				border = Style.NodeWaiting;
			}
			if (!node.Known) {
				border = Style.NodeFailed;
			}
			if (hovered) {
				border = Style.NodeBorderHovered;
			}
			if (selected) {
				border = Style.NodeBorderSelected;
				borderWidth = Style.SelectedBorderWidth;
			}
			const bool bypassed = instance != nullptr && instance->Bypassed;
			const Rgba body = bypassed ? Mix(Style.NodeBody, Style.NodeBypassed, 0.4f) : Style.NodeBody;
			const float radius = Style.CornerRadius * zoom;
			list.push_back({.Kind = DrawKind::Rect, .A = min, .B = max, .Fill = body, .Radius = radius});
			const float header = Style.HeaderHeight * zoom;
			Rgba headerColour = Style.NodeHeader;
			if (NodeHeader && instance != nullptr) {
				if (const auto tint = NodeHeader(*instance)) {
					headerColour = *tint;
				}
			}
			if (Style.ShowHeaders) {
				list.push_back(
					{.Kind = DrawKind::Rect,
					 .A = min,
					 .B = {max.X, min.Y + header},
					 .Fill = headerColour,
					 .Radius = radius}
				);
			}
			list.push_back(
				{.Kind = DrawKind::Rect,
				 .A = min,
				 .B = max,
				 .Stroke = border,
				 .Thickness = borderWidth,
				 .Radius = radius}
			);
			const std::string title = node.Known ? node.Title : "? " + node.Title;
			list.push_back(
				{.Kind = DrawKind::Text,
				 .A = {min.X + Style.NodePadding * zoom, min.Y + (header - fontSize) * 0.5f},
				 .B = {max.X - min.X - Style.NodePadding * 2.0f * zoom, 0.0f},
				 .Fill = Style.Text,
				 .TextSize = fontSize,
				 .Text = title}
			);
			for (const auto *ports : {&node.Inputs, &node.Outputs}) {
				for (const PortLayout &port : *ports) {
					const core::Vector2 centre = ToView(port.Centre);
					const bool target =
						WireState && WireState->Target && WireState->Target->Node == node.Id &&
						WireState->Target->Port == port.Name && WireState->Target->Dir == port.Dir;
					Rgba fill = WireColour(port.Type);
					if (target) {
						fill = WireState->TargetAccepts ? Style.PortAccepts : Style.WireInvalid;
					}
					const float portRadius = Style.PortRadius * zoom;
					const PortShape shape = Style.ShapeFor(port.Type);
					if (shape == PortShape::Square) {
						list.push_back(
							{.Kind = DrawKind::Rect,
							 .A = {centre.X - portRadius, centre.Y - portRadius},
							 .B = {centre.X + portRadius, centre.Y + portRadius},
							 .Fill = fill}
						);
					} else {
						list.push_back(
							{.Kind = shape == PortShape::Diamond ? DrawKind::Diamond : DrawKind::Circle,
							 .A = centre,
							 .Fill = fill,
							 .Radius = portRadius}
						);
					}
					const bool input = port.Dir == ang::Direction::Input;
					const float labelX = input ? centre.X + portRadius + 4.0f * zoom
											   : centre.X - portRadius - 4.0f * zoom -
													 static_cast<float>(port.Name.size()) * fontSize * 0.55f;
					list.push_back(
						{.Kind = DrawKind::Text,
						 .A = {labelX, centre.Y - fontSize * 0.5f},
						 .Fill = Style.TextDim,
						 .TextSize = fontSize,
						 .Text = port.Name}
					);
				}
			}
			if (node.Image != 0) {
				list.push_back(
					{.Kind = DrawKind::Image,
					 .A = ToView(node.ImageBounds.Min),
					 .B = ToView(node.ImageBounds.Max),
					 .Image = node.Image}
				);
			}
			// Idle with bytes is memory known without a measured run: bytes alone.
			const bool idle = status != nullptr && status->Status == ang::NodeStatus::Idle;
			if (Style.ShowTimings && status != nullptr && (!idle || status->Bytes > 0)) {
				const float slow =
					static_cast<float>(status->Milliseconds / std::max(1e-6, Style.TimingSlowMilliseconds));
				std::string label = idle ? std::string()
									: status->Status == ang::NodeStatus::Ran
										? FormatMilliseconds(status->Milliseconds)
										: std::string(ang::Describe(status->Status));
				if (status->Bytes > 0) {
					label += (label.empty() ? "" : "  ") + FormatBytes(status->Bytes);
				}
				list.push_back(
					{.Kind = DrawKind::Text,
					 .A = {min.X, max.Y + 2.0f * zoom},
					 .Fill = status->Status == ang::NodeStatus::Ran
								 ? Mix(Style.TimingFast, Style.TimingSlow, slow)
								 : Style.TextDim,
					 .TextSize = fontSize * 0.85f,
					 .Text = std::move(label)}
				);
			}
		}

		if (const auto box = BoxSelection()) {
			list.push_back(
				{.Kind = DrawKind::Rect,
				 .A = box->Min,
				 .B = box->Max,
				 .Fill = Style.BoxSelectFill,
				 .Stroke = Style.BoxSelectBorder,
				 .Thickness = 1.0f}
			);
		}
		list.push_back({.Kind = DrawKind::PopClip});

		PaintTimeline(list);

		if (PaletteState.Open) {
			const float textSize = Style.FontSize * Style.FontScale;
			const float row = textSize + 6.0f;
			const core::Vector2 origin = ToView(PaletteState.Position);
			const size_t shown = std::min(PaletteState.Matches.size(), Style.PaletteRows);
			const core::Vector2 end{
				origin.X + Style.PaletteWidth, origin.Y + row * static_cast<float>(shown + 1)
			};
			list.push_back(
				{.Kind = DrawKind::Rect,
				 .A = origin,
				 .B = end,
				 .Fill = Style.PaletteBackground,
				 .Stroke = Style.NodeBorder,
				 .Thickness = 1.0f}
			);
			list.push_back(
				{.Kind = DrawKind::Text,
				 .A = {origin.X + 6.0f, origin.Y + 3.0f},
				 .Fill = Style.Text,
				 .TextSize = textSize,
				 .Text = "> " + PaletteState.Filter}
			);
			// Scroll so the highlighted row stays visible.
			const size_t first = PaletteState.Highlighted >= shown ? PaletteState.Highlighted - shown + 1 : 0;
			for (size_t index = 0; index < shown; ++index) {
				const size_t match = first + index;
				const float y = origin.Y + row * static_cast<float>(index + 1);
				if (match == PaletteState.Highlighted) {
					list.push_back(
						{.Kind = DrawKind::Rect,
						 .A = {origin.X, y},
						 .B = {end.X, y + row},
						 .Fill = Style.PaletteHighlight}
					);
				}
				list.push_back(
					{.Kind = DrawKind::Text,
					 .A = {origin.X + 6.0f, y + 3.0f},
					 .Fill = Style.TextDim,
					 .TextSize = textSize,
					 .Text = PaletteState.Matches[match]}
				);
			}
		}
	}

	void View::PaintTimeline(DrawList &list) {
		if (TimelineSamples.empty() || Style.TimelineHeight <= 0.0f) {
			return;
		}
		const float top = ViewportSize.Y - Style.TimelineHeight;
		list.push_back(
			{.Kind = DrawKind::Rect, .A = {0.0f, top}, .B = ViewportSize, .Fill = Style.PaletteBackground}
		);
		int64_t end = 1;
		for (const ang::TimelineSample &sample : TimelineSamples) {
			end = std::max(end, sample.EndNanoseconds);
		}
		const float scale = ViewportSize.X / static_cast<float>(end);
		for (const ang::TimelineSample &sample : TimelineSamples) {
			const float x0 = static_cast<float>(std::max<int64_t>(0, sample.BeginNanoseconds)) * scale;
			const float x1 = std::max(x0 + 1.0f, static_cast<float>(sample.EndNanoseconds) * scale);
			const double milliseconds =
				static_cast<double>(sample.EndNanoseconds - sample.BeginNanoseconds) / 1.0e6;
			const float slow =
				static_cast<float>(milliseconds / std::max(1e-6, Style.TimingSlowMilliseconds));
			list.push_back(
				{.Kind = DrawKind::Rect,
				 .A = {x0, top + 4.0f},
				 .B = {x1, ViewportSize.Y - 4.0f},
				 .Fill = Mix(Style.TimingFast, Style.TimingSlow, slow)}
			);
		}
		list.push_back(
			{.Kind = DrawKind::Text,
			 .A = {4.0f, top + 2.0f},
			 .Fill = Style.Text,
			 .TextSize = Style.FontSize * Style.FontScale * 0.85f,
			 .Text = FormatMilliseconds(TimelineMilliseconds)}
		);
	}
}
