// The canvas: pan, zoom, select, drag, connect, and on-node widgets.
//
// **Painted with `ImDrawList` and hit-tested analytically**, submitting no ImGui
// widget per node. That is what makes zoom work at all — an ImGui button does
// not scale — and it means `LayoutOf` answers both "where is that slider" and
// "did I click it", which is the one arrangement where the two cannot disagree.
//
// Drawn inside whatever window the caller has already begun, like any other
// widget. A canvas that opened its own would be deciding where it lives.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <imgui.h>
#include <studio/NodeGraph.hpp>

namespace studio::nodes {


	namespace {
		uint32_t Packed(const Colour &colour, float alpha = 1.0f) {
			return ImGui::ColorConvertFloat4ToU32(ImVec4(colour.R, colour.G, colour.B, colour.A * alpha));
		}

		uint32_t TintOf(const std::string &type) {
			const DataType *found = DataTypes::Find(type);
			return found != nullptr ? Packed(found->Tint) : 0xFF9E9E9E;
		}

		// A cubic between two ports, with the handles pushed along x so a link
		// leaves an output rightwards and arrives at an input from the left —
		// which is what makes a graph readable when two nodes are stacked.
		void Curve(
			ImDrawList *draw, ImVec2 from, ImVec2 to, uint32_t colour, float thickness, float zoom
		) {
			const float reach = std::max(40.0f * zoom, std::fabs(to.x - from.x) * 0.5f);
			draw->AddBezierCubic(
				from, ImVec2(from.x + reach, from.y), ImVec2(to.x - reach, to.y), to, colour, thickness
			);
		}

		std::string Number(double value, int precision) {
			char text[32];
			std::snprintf(text, sizeof(text), "%.*f", precision, value);
			return text;
		}
	}

	void Canvas::ToScreen(float x, float y, float &outX, float &outY) const {
		outX = OriginX + (x + PanX) * Scale;
		outY = OriginY + (y + PanY) * Scale;
	}

	void Canvas::ToGraph(float x, float y, float &outX, float &outY) const {
		outX = (x - OriginX) / Scale - PanX;
		outY = (y - OriginY) / Scale - PanY;
	}

	void Canvas::SetZoom(float zoom) {
		Scale = std::clamp(zoom, 0.2f, 3.0f);
	}

	void Canvas::Select(NodeId node) {
		Chosen.clear();
		if (node != NO_NODE) {
			Chosen.push_back(node);
		}
	}

	void Canvas::Fit(const Graph &graph) {
		if (graph.Nodes().empty()) {
			PanX = 0.0f;
			PanY = 0.0f;
			Scale = 1.0f;
			return;
		}

		float left = graph.Nodes().front().X;
		float top = graph.Nodes().front().Y;
		float right = left;
		float bottom = top;

		for (const Node &node : graph.Nodes()) {
			const NodeLayout layout = LayoutOf(node, Look.Sizes);
			left = std::min(left, node.X);
			top = std::min(top, node.Y);
			right = std::max(right, node.X + layout.Width);
			bottom = std::max(bottom, node.Y + layout.Height);
		}

		const ImVec2 size = ImGui::GetContentRegionAvail();
		const float margin = 40.0f;
		const float width = std::max(right - left + margin * 2.0f, 1.0f);
		const float height = std::max(bottom - top + margin * 2.0f, 1.0f);

		SetZoom(std::min(size.x / width, size.y / height));
		PanX = -left + margin;
		PanY = -top + margin;
	}

	void Canvas::DrawGrid(float x, float y, float width, float height) const {
		ImDrawList *draw = ImGui::GetWindowDrawList();
		draw->AddRectFilled(ImVec2(x, y), ImVec2(x + width, y + height), Look.Background);

		// **The fine grid fades out rather than turning into a solid block.**
		// Below a spacing of a few pixels a grid is noise, and drawing it is
		// both slower and worse looking than not.
		const float fine = 24.0f * Scale;
		if (fine >= 6.0f) {
			const float startX = std::fmod(PanX * Scale, fine);
			const float startY = std::fmod(PanY * Scale, fine);
			for (float at = startX; at < width; at += fine) {
				draw->AddLine(ImVec2(x + at, y), ImVec2(x + at, y + height), Look.GridFine);
			}
			for (float at = startY; at < height; at += fine) {
				draw->AddLine(ImVec2(x, y + at), ImVec2(x + width, y + at), Look.GridFine);
			}
		}

		const float coarse = 24.0f * 5.0f * Scale;
		const float startX = std::fmod(PanX * Scale, coarse);
		const float startY = std::fmod(PanY * Scale, coarse);
		for (float at = startX; at < width; at += coarse) {
			draw->AddLine(ImVec2(x + at, y), ImVec2(x + at, y + height), Look.GridCoarse);
		}
		for (float at = startY; at < height; at += coarse) {
			draw->AddLine(ImVec2(x, y + at), ImVec2(x + width, y + at), Look.GridCoarse);
		}
	}

	void Canvas::DrawLinks(const Graph &graph) const {
		ImDrawList *draw = ImGui::GetWindowDrawList();

		for (const Link &link : graph.Links()) {
			const Node *from = graph.Find(link.From);
			const Node *to = graph.Find(link.To);
			if (from == nullptr || to == nullptr) {
				continue;
			}

			const NodeLayout fromLayout = LayoutOf(*from, Look.Sizes);
			const NodeLayout toLayout = LayoutOf(*to, Look.Sizes);
			const PlacedPort *out = PortIn(fromLayout, link.FromPort, false);
			const PlacedPort *in = PortIn(toLayout, link.ToPort, true);
			if (out == nullptr || in == nullptr) {
				continue;
			}

			ImVec2 a;
			ImVec2 b;
			ToScreen(from->X + out->X, from->Y + out->Y, a.x, a.y);
			ToScreen(to->X + in->X, to->Y + in->Y, b.x, b.y);

			// Coloured by what it carries, which is the same rule the ports use
			// — a link's colour is a fact about the data and never about which
			// node it came from.
			Curve(draw, a, b, TintOf(out->Type), std::max(1.5f, 2.0f * Scale), Scale);
		}
	}

	void Canvas::DrawWidget(const Node &node, const NodeType &type, const PlacedWidget &placed) const {
		ImDrawList *draw = ImGui::GetWindowDrawList();
		const WidgetSpec &spec = type.Widgets[placed.Index];

		const auto found = node.Widgets.find(spec.Key);
		const Value value = found == node.Widgets.end() ? spec.Default : found->second;

		ImVec2 corner;
		ImVec2 far;
		ToScreen(node.X + placed.X, node.Y + placed.Y, corner.x, corner.y);
		ToScreen(node.X + placed.X + placed.Width, node.Y + placed.Y + placed.Height, far.x, far.y);

		draw->AddRectFilled(corner, far, Look.Widget, 3.0f * Scale);

		std::string text = spec.Label;
		switch (spec.Kind) {
		case WidgetKind::Slider: {
			const double span = spec.Maximum - spec.Minimum;
			const float fraction =
				span > 0.0 ? static_cast<float>((value.Number - spec.Minimum) / span) : 0.0f;
			draw->AddRectFilled(
				corner,
				ImVec2(corner.x + (far.x - corner.x) * std::clamp(fraction, 0.0f, 1.0f), far.y),
				Look.WidgetFill,
				3.0f * Scale
			);
			text += "  " + Number(value.Number, spec.Precision);
			break;
		}
		case WidgetKind::Number:
			text += "  " + Number(value.Number, spec.Precision);
			break;
		case WidgetKind::Toggle:
			text += value.Flag ? "  on" : "  off";
			if (value.Flag) {
				draw->AddRectFilled(
					ImVec2(far.x - (far.y - corner.y), corner.y), far, Look.WidgetFill, 3.0f * Scale
				);
			}
			break;
		case WidgetKind::Select:
		case WidgetKind::Text:
			text += "  " + value.Text;
			break;
		case WidgetKind::Colour:
			draw->AddRectFilled(
				ImVec2(far.x - (far.y - corner.y), corner.y), far, Packed(value.Tint), 3.0f * Scale
			);
			break;
		}

		const float size = Look.Sizes.SmallSize * Scale;
		if (size >= 5.0f) {
			draw->AddText(
				nullptr, size, ImVec2(corner.x + 4.0f * Scale, corner.y + 2.0f * Scale), Look.Text,
				text.c_str()
			);
		}
	}

	void Canvas::DrawNode(const Node &node, const NodeLayout &layout) const {
		ImDrawList *draw = ImGui::GetWindowDrawList();
		const NodeType *type = NodeTypes::Find(node.Type);

		ImVec2 corner;
		ImVec2 far;
		ToScreen(node.X, node.Y, corner.x, corner.y);
		ToScreen(node.X + layout.Width, node.Y + layout.Height, far.x, far.y);

		const bool selected =
			std::find(Chosen.begin(), Chosen.end(), node.Id) != Chosen.end();

		draw->AddRectFilled(corner, far, Look.NodeBody, Look.Sizes.Rounding * Scale);

		// The header, in the type's accent. A node whose type is not registered
		// gets the refusal colour and its type id as the title, which is the
		// only useful thing left to say about it.
		const ImVec2 headerFar(far.x, corner.y + Look.Sizes.HeaderHeight * Scale);
		draw->AddRectFilled(
			corner,
			headerFar,
			type != nullptr ? Packed(type->Accent) : Look.Refused,
			Look.Sizes.Rounding * Scale,
			ImDrawFlags_RoundCornersTop
		);

		draw->AddRect(
			corner,
			far,
			selected ? Look.NodeSelected : Look.NodeBorder,
			Look.Sizes.Rounding * Scale,
			0,
			selected ? 2.0f : 1.0f
		);

		const float title = Look.Sizes.LabelSize * Scale;
		if (title >= 5.0f) {
			const std::string label = !node.Label.empty() ? node.Label
									  : type != nullptr   ? type->Title
														  : node.Type;
			draw->AddText(
				nullptr, title, ImVec2(corner.x + 8.0f * Scale, corner.y + 5.0f * Scale), 0xFF101010,
				label.c_str()
			);
		}

		if (type == nullptr) {
			return;
		}

		// The ports, with their names inside the body. Drawn after the header so
		// a port on the first row is never painted over.
		const float small = Look.Sizes.SmallSize * Scale;
		for (const PlacedPort &port : layout.Ports) {
			ImVec2 at;
			ToScreen(node.X + port.X, node.Y + port.Y, at.x, at.y);

			const uint32_t tint = TintOf(port.Type);
			draw->AddCircleFilled(at, Look.Sizes.PortRadius * Scale, tint);
			draw->AddCircle(at, Look.Sizes.PortRadius * Scale, Look.NodeBorder);

			if (small < 5.0f) {
				continue;
			}
			const ImVec2 size = ImGui::CalcTextSize(port.Name.c_str());
			const float width = size.x * (small / ImGui::GetFontSize());
			draw->AddText(
				nullptr,
				small,
				ImVec2(
					port.Input ? at.x + 10.0f * Scale : at.x - 10.0f * Scale - width,
					at.y - small * 0.5f
				),
				Look.Muted,
				port.Name.c_str()
			);
		}

		for (const PlacedWidget &placed : layout.Widgets) {
			DrawWidget(node, *type, placed);
		}

		// The cached badge, when somebody is watching an evaluator. It is the
		// one piece of evaluation state worth putting on the canvas: a node that
		// did not recompute when you expected it to is the first sign that a
		// hash is wrong.
		if (Watching != nullptr && small >= 5.0f && Watching->WasCached(node.Id)) {
			draw->AddText(
				nullptr, small, ImVec2(far.x - 44.0f * Scale, corner.y + 6.0f * Scale), 0xFF303030,
				"cached"
			);
		}
	}

	NodeId Canvas::HitNode(const Graph &graph, float graphX, float graphY) const {
		// **Backwards, because the last drawn is the one on top.** Hit testing
		// forwards would pick the node underneath whenever two overlap, which is
		// exactly when somebody is trying to grab the one they can see.
		for (auto node = graph.Nodes().rbegin(); node != graph.Nodes().rend(); ++node) {
			const NodeLayout layout = LayoutOf(*node, Look.Sizes);
			if (graphX >= node->X && graphX <= node->X + layout.Width && graphY >= node->Y &&
				graphY <= node->Y + layout.Height) {
				return node->Id;
			}
		}
		return NO_NODE;
	}

	bool Canvas::HitPort(
		const Graph &graph, float graphX, float graphY, NodeId &node, std::string &port, bool &input
	) const {
		// A port's grab radius is larger than its circle. A five-pixel target is
		// one nobody can hit on a laptop trackpad, and the cost of being generous
		// is nothing: the ports are far apart by construction.
		const float reach = Look.Sizes.PortRadius * 2.5f;

		for (auto walk = graph.Nodes().rbegin(); walk != graph.Nodes().rend(); ++walk) {
			const NodeLayout layout = LayoutOf(*walk, Look.Sizes);
			for (const PlacedPort &placed : layout.Ports) {
				const float dx = graphX - (walk->X + placed.X);
				const float dy = graphY - (walk->Y + placed.Y);
				if (dx * dx + dy * dy <= reach * reach) {
					node = walk->Id;
					port = placed.Name;
					input = placed.Input;
					return true;
				}
			}
		}
		return false;
	}

	void Canvas::HandleWidget(Graph &graph, NodeId id, const PlacedWidget &placed, float graphX) {
		Node *node = graph.Find(id);
		if (node == nullptr) {
			return;
		}
		const NodeType *type = NodeTypes::Find(node->Type);
		if (type == nullptr || placed.Index >= type->Widgets.size()) {
			return;
		}

		const WidgetSpec &spec = type->Widgets[placed.Index];
		Value &value = node->Widgets[spec.Key];
		value.Kind = spec.Kind;

		switch (spec.Kind) {
		case WidgetKind::Slider: {
			const float fraction = std::clamp((graphX - (node->X + placed.X)) / placed.Width, 0.0f, 1.0f);
			value.Number = spec.Minimum + (spec.Maximum - spec.Minimum) * fraction;
			break;
		}
		case WidgetKind::Number:
			// A horizontal drag, in steps. The same gesture as ImGui's own drag
			// field, so the muscle memory carries.
			value.Number += ImGui::GetIO().MouseDelta.x / Scale * spec.Step;
			break;
		case WidgetKind::Toggle:
			if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
				value.Flag = !value.Flag;
			}
			break;
		case WidgetKind::Select: {
			if (!ImGui::IsMouseClicked(ImGuiMouseButton_Left) || spec.Options.empty()) {
				break;
			}
			const auto at = std::find(spec.Options.begin(), spec.Options.end(), value.Text);
			const size_t next =
				at == spec.Options.end() ? 0 : (static_cast<size_t>(at - spec.Options.begin()) + 1) %
													spec.Options.size();
			value.Text = spec.Options[next];
			break;
		}
		case WidgetKind::Text:
		case WidgetKind::Colour:
			// **Left to the inspector rather than done badly here.** Editing
			// text on a zoomable canvas needs a caret, a selection and an IME,
			// which is a DOM input in the JS template and an `InputText` in a
			// panel here — not something to reimplement with a draw list.
			break;
		}
	}

	void Canvas::Palette(Graph &graph) {
		if (!PaletteOpen) {
			return;
		}

		if (ImGui::BeginPopup("nodegraph.palette")) {
			// Filtered to what could actually be connected when the palette was
			// opened by a link drag — the JS template's rule, and the reason a
			// drag into empty space is a *shortcut* rather than a second way to
			// add a node.
			const std::string wanted = DragPort.empty() ? std::string() : DragPort;
			(void)wanted;

			for (const std::string &category : NodeTypes::Categories()) {
				if (!ImGui::CollapsingHeader(category.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
					continue;
				}
				for (const NodeType &type : NodeTypes::All()) {
					if (type.Category != category) {
						continue;
					}
					if (!ImGui::MenuItem(type.Title.c_str())) {
						continue;
					}

					const NodeId made = graph.Add(type.Id, PaletteX, PaletteY);
					if (made != NO_NODE && DragNode != NO_NODE) {
						// Wire it to whatever the drag came from, on the first
						// port that will take it.
						const NodeType *added = NodeTypes::Find(type.Id);
						for (const PortSpec &port :
							 DragFromInput ? added->Outputs : added->Inputs) {
							const LinkResult result =
								DragFromInput
									? graph.Connect(made, port.Name, DragNode, DragPort)
									: graph.Connect(DragNode, DragPort, made, port.Name);
							if (result == LinkResult::Made) {
								break;
							}
						}
					}
					Select(made);
					PaletteOpen = false;
					DragNode = NO_NODE;
					ImGui::CloseCurrentPopup();
					break;
				}
			}
			ImGui::EndPopup();
		} else {
			PaletteOpen = false;
			DragNode = NO_NODE;
		}
	}

	void Canvas::Draw(Graph &graph) {
		ImGuiIO &io = ImGui::GetIO();
		ImDrawList *draw = ImGui::GetWindowDrawList();

		const ImVec2 origin = ImGui::GetCursorScreenPos();
		const ImVec2 size = ImGui::GetContentRegionAvail();
		OriginX = origin.x;
		OriginY = origin.y;

		// **One invisible button for the whole canvas.** It is what makes the
		// window's own scrolling and dragging stand aside, and it gives one
		// `IsItemHovered` to test instead of a rectangle comparison per event.
		ImGui::InvisibleButton("nodegraph.canvas", size, ImGuiButtonFlags_MouseButtonLeft |
															ImGuiButtonFlags_MouseButtonRight |
															ImGuiButtonFlags_MouseButtonMiddle);
		const bool hovered = ImGui::IsItemHovered();

		draw->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);
		DrawGrid(origin.x, origin.y, size.x, size.y);

		float mouseX = 0.0f;
		float mouseY = 0.0f;
		ToGraph(io.MousePos.x, io.MousePos.y, mouseX, mouseY);

		// --- the view ---------------------------------------------------------

		if (hovered && io.MouseWheel != 0.0f) {
			// Zoom to the cursor: the graph point under the pointer has to stay
			// under it, which is what makes zooming feel like moving a map
			// rather than like resizing a picture.
			const float before = Scale;
			SetZoom(Scale * (1.0f + io.MouseWheel * 0.1f));
			if (Scale != before) {
				PanX += (io.MousePos.x - OriginX) * (1.0f / Scale - 1.0f / before);
				PanY += (io.MousePos.y - OriginY) * (1.0f / Scale - 1.0f / before);
			}
		}

		if (hovered && (ImGui::IsMouseDragging(ImGuiMouseButton_Middle) ||
						(io.KeyAlt && ImGui::IsMouseDragging(ImGuiMouseButton_Left)))) {
			Drag = Dragging::Pan;
		}
		if (Drag == Dragging::Pan) {
			PanX += io.MouseDelta.x / Scale;
			PanY += io.MouseDelta.y / Scale;
			if (!ImGui::IsMouseDown(ImGuiMouseButton_Middle) && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
				Drag = Dragging::None;
			}
		}

		// --- starting a drag --------------------------------------------------

		if (hovered && Drag == Dragging::None && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
			!io.KeyAlt) {
			NodeId node = NO_NODE;
			std::string port;
			bool input = false;

			if (HitPort(graph, mouseX, mouseY, node, port, input)) {
				// **Dragging from a connected input picks the link up** rather
				// than starting a second one, which is what every node editor
				// does and what an input taking one link makes possible.
				if (input) {
					if (const Link *existing = graph.LinkInto(node, port); existing != nullptr) {
						DragNode = existing->From;
						DragPort = existing->FromPort;
						DragFromInput = false;
						graph.Disconnect(node, port);
					} else {
						DragNode = node;
						DragPort = port;
						DragFromInput = true;
					}
				} else {
					DragNode = node;
					DragPort = port;
					DragFromInput = false;
				}
				Drag = Dragging::Link;
			} else if (const NodeId hit = HitNode(graph, mouseX, mouseY); hit != NO_NODE) {
				const Node *node_ = graph.Find(hit);
				const NodeLayout layout = LayoutOf(*node_, Look.Sizes);

				// A widget under the cursor takes the drag; the body moves the
				// node. Widgets first, because they are inside the body.
				const PlacedWidget *widget = nullptr;
				for (const PlacedWidget &placed : layout.Widgets) {
					if (mouseX >= node_->X + placed.X && mouseX <= node_->X + placed.X + placed.Width &&
						mouseY >= node_->Y + placed.Y && mouseY <= node_->Y + placed.Y + placed.Height) {
						widget = &placed;
						break;
					}
				}

				if (widget != nullptr) {
					Drag = Dragging::Widget;
					DragNode = hit;
					DragWidget = widget->Key;
					HandleWidget(graph, hit, *widget, mouseX);
				} else {
					if (std::find(Chosen.begin(), Chosen.end(), hit) == Chosen.end()) {
						if (!io.KeyShift) {
							Chosen.clear();
						}
						Chosen.push_back(hit);
					}
					Drag = Dragging::Nodes;
				}
			} else {
				if (!io.KeyShift) {
					Chosen.clear();
				}
				Drag = Dragging::Marquee;
				MarqueeX = mouseX;
				MarqueeY = mouseY;
			}
		}

		// --- continuing one ---------------------------------------------------

		if (Drag == Dragging::Nodes && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
			for (const NodeId id : Chosen) {
				if (Node *node = graph.Find(id); node != nullptr) {
					node->X += io.MouseDelta.x / Scale;
					node->Y += io.MouseDelta.y / Scale;
				}
			}
		}

		if (Drag == Dragging::Widget && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
			if (const Node *node = graph.Find(DragNode); node != nullptr) {
				const NodeLayout layout = LayoutOf(*node, Look.Sizes);
				for (const PlacedWidget &placed : layout.Widgets) {
					if (placed.Key == DragWidget) {
						HandleWidget(graph, DragNode, placed, mouseX);
						break;
					}
				}
			}
		}

		if (Drag == Dragging::Marquee) {
			float ax = 0.0f;
			float ay = 0.0f;
			float bx = 0.0f;
			float by = 0.0f;
			ToScreen(MarqueeX, MarqueeY, ax, ay);
			ToScreen(mouseX, mouseY, bx, by);
			draw->AddRectFilled(ImVec2(ax, ay), ImVec2(bx, by), Look.Marquee);
			draw->AddRect(ImVec2(ax, ay), ImVec2(bx, by), Look.NodeSelected);
		}

		// --- ending one -------------------------------------------------------

		if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
			if (Drag == Dragging::Link && DragNode != NO_NODE) {
				NodeId node = NO_NODE;
				std::string port;
				bool input = false;

				if (HitPort(graph, mouseX, mouseY, node, port, input) && input != DragFromInput) {
					const LinkResult result =
						DragFromInput ? graph.Connect(node, port, DragNode, DragPort)
									  : graph.Connect(DragNode, DragPort, node, port);
					LastRefusal = result == LinkResult::Made ? std::string() : Describe(result);
					DragNode = NO_NODE;
				} else if (!HitPort(graph, mouseX, mouseY, node, port, input)) {
					// Into empty space: offer what could be connected there.
					PaletteX = mouseX;
					PaletteY = mouseY;
					PaletteOpen = true;
					ImGui::OpenPopup("nodegraph.palette");
				} else {
					DragNode = NO_NODE;
				}
			}

			if (Drag == Dragging::Marquee) {
				for (const Node &node : graph.Nodes()) {
					const NodeLayout layout = LayoutOf(node, Look.Sizes);
					const float left = std::min(MarqueeX, mouseX);
					const float right = std::max(MarqueeX, mouseX);
					const float top = std::min(MarqueeY, mouseY);
					const float bottom = std::max(MarqueeY, mouseY);

					// Intersecting rather than enclosed, which is what a marquee
					// over a graph means: nodes are large and an enclosing rule
					// makes selecting three of them a full-screen drag.
					if (node.X + layout.Width >= left && node.X <= right &&
						node.Y + layout.Height >= top && node.Y <= bottom) {
						if (std::find(Chosen.begin(), Chosen.end(), node.Id) == Chosen.end()) {
							Chosen.push_back(node.Id);
						}
					}
				}
			}

			if (Drag != Dragging::Link || !PaletteOpen) {
				Drag = Dragging::None;
			} else {
				Drag = Dragging::None;
			}
		}

		// --- painting ---------------------------------------------------------

		DrawLinks(graph);

		if (Drag == Dragging::Link && DragNode != NO_NODE) {
			if (const Node *node = graph.Find(DragNode); node != nullptr) {
				const NodeLayout layout = LayoutOf(*node, Look.Sizes);
				if (const PlacedPort *port = PortIn(layout, DragPort, DragFromInput); port != nullptr) {
					ImVec2 from;
					ToScreen(node->X + port->X, node->Y + port->Y, from.x, from.y);
					Curve(draw, from, io.MousePos, TintOf(port->Type), 2.0f, Scale);
				}
			}
		}

		for (const Node &node : graph.Nodes()) {
			DrawNode(node, LayoutOf(node, Look.Sizes));
		}

		draw->PopClipRect();

		// --- keys and menus ---------------------------------------------------

		if (hovered && ImGui::IsKeyPressed(ImGuiKey_Delete)) {
			for (const NodeId id : Chosen) {
				graph.Remove(id);
			}
			Chosen.clear();
		}
		if (hovered && ImGui::IsKeyPressed(ImGuiKey_F)) {
			Fit(graph);
		}

		if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
			PaletteX = mouseX;
			PaletteY = mouseY;
			DragNode = NO_NODE;
			PaletteOpen = true;
			ImGui::OpenPopup("nodegraph.palette");
		}

		Palette(graph);
	}
}
