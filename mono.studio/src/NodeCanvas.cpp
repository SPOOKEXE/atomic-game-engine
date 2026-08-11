// The canvas: pan, zoom, select, drag, connect, and on-node widgets.
//
// **Painted with `ImDrawList` and hit-tested analytically**, submitting no ImGui
// widget per node. That is what makes zoom work at all — an ImGui button does
// not scale — and it means `LayoutOf` answers both "where is that slider" and
// "did I click it", which is the one arrangement where the two cannot disagree.
//
// Drawn inside whatever window the caller has already begun, like any other
// widget. A canvas that opened its own would be deciding where it lives.

#include <engine/ui/Metrics.hpp>
#include <engine/ui/Theme.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <imgui.h>
#include <string>
#include <studio/NodeGraph.hpp>
#include <unordered_map>
#include <vector>

namespace studio::nodes {

	namespace {
		// How tall a frame's title bar is, in graph space.
		//
		// **A group is grabbed by its bar and never by its body.** A frame that
		// took drags anywhere inside it would swallow the marquee, and a marquee
		// inside a group is how somebody selects three of its six nodes.
		constexpr float GROUP_BAR = 22.0f;

		uint32_t Packed(const Colour &colour, float alpha = 1.0f) {
			return ImGui::ColorConvertFloat4ToU32(ImVec4(colour.R, colour.G, colour.B, colour.A * alpha));
		}

		uint32_t TintOf(const std::string &type) {
			const DataType *found = DataTypes::Find(type);
			return found != nullptr ? Packed(found->Tint) : 0xFF9E9E9E;
		}

		// The same colour, most of the way to invisible. Used for everything
		// that is not the type somebody is pointing at — dimming the rest reads
		// as one answer, where colouring the match reads as forty.
		uint32_t Faded(uint32_t colour) {
			const uint32_t alpha = ((colour >> 24) & 0xFFu) / 5u;
			return (colour & 0x00FFFFFFu) | (alpha << 24);
		}

		// A cubic between two ports, with the handles pushed along x so a link
		// leaves an output rightwards and arrives at an input from the left —
		// which is what makes a graph readable when two nodes are stacked.
		void Curve(ImDrawList *draw, ImVec2 from, ImVec2 to, uint32_t colour, float thickness, float zoom) {
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

	namespace {
		// A point on the same cubic `Curve` draws, in graph space. One function
		// for the picture and the hit test, so a link can never be somewhere
		// other than where it looks.
		void Along(float fromX, float fromY, float toX, float toY, float t, float &outX, float &outY) {
			const float reach = std::max(40.0f, std::fabs(toX - fromX) * 0.5f);
			const float ax = fromX + reach;
			const float bx = toX - reach;

			const float u = 1.0f - t;
			const float w0 = u * u * u;
			const float w1 = 3.0f * u * u * t;
			const float w2 = 3.0f * u * t * t;
			const float w3 = t * t * t;

			outX = w0 * fromX + w1 * ax + w2 * bx + w3 * toX;
			outY = w0 * fromY + w1 * fromY + w2 * toY + w3 * toY;
		}

		// Both ends of a link in graph space **as they appear at `depth`**, or
		// false when either is not visible there.
		//
		// A wire into a folded selection is drawn to the fold's proxy port, and
		// a wire wholly inside one is not drawn at this depth at all — which is
		// the whole visual effect of compression, and it is this one function
		// rather than a second set of edges.
		bool Ends(
			const Graph &graph,
			const Link &link,
			const Metrics &metrics,
			NodeId depth,
			float &fromX,
			float &fromY,
			float &toX,
			float &toY,
			std::string &carried
		) {
			NodeId fromId = NO_NODE;
			NodeId toId = NO_NODE;
			std::string fromPort;
			std::string toPort;

			if (!Standing(graph, link.From, link.FromPort, false, depth, fromId, fromPort) ||
				!Standing(graph, link.To, link.ToPort, true, depth, toId, toPort)) {
				return false;
			}

			// A link both of whose ends resolved to one node is one entirely
			// inside a fold. Drawing it would be a loop on the fold's header.
			if (fromId == toId) {
				return false;
			}

			const Node *from = graph.Find(fromId);
			const Node *to = graph.Find(toId);
			if (from == nullptr || to == nullptr) {
				return false;
			}

			const NodeLayout fromLayout = LayoutOf(*from, metrics);
			const NodeLayout toLayout = LayoutOf(*to, metrics);
			const PlacedPort *out = PortIn(fromLayout, fromPort, false);
			const PlacedPort *in = PortIn(toLayout, toPort, true);
			if (out == nullptr || in == nullptr) {
				return false;
			}

			fromX = from->X + out->X;
			fromY = from->Y + out->Y;
			toX = to->X + in->X;
			toY = to->Y + in->Y;
			carried = out->Type;
			return true;
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
		ChosenGroup = NO_GROUP;
		if (node != NO_NODE) {
			Chosen.push_back(node);
		}
	}

	void Canvas::Select(std::vector<NodeId> nodes) {
		Chosen = std::move(nodes);
		ChosenGroup = NO_GROUP;
	}

	void Canvas::SelectAll(const Graph &graph) {
		Chosen.clear();
		ChosenGroup = NO_GROUP;
		for (const Node &node : graph.Nodes()) {
			if (Visible(node)) {
				Chosen.push_back(node.Id);
			}
		}
	}

	void Canvas::Fit(const Graph &graph) {
		// **The selection when there is one, everything otherwise.** Fitting to
		// a whole graph while looking at three nodes of it is the wrong answer to
		// the question somebody asked, and it is the same key either way.
		const bool some = !Chosen.empty();
		const std::vector<Node> &nodes = graph.Nodes();
		if (nodes.empty()) {
			PanX = 0.0f;
			PanY = 0.0f;
			Scale = 1.0f;
			return;
		}

		bool any = false;
		float left = 0.0f;
		float top = 0.0f;
		float right = 0.0f;
		float bottom = 0.0f;

		for (const Node &node : nodes) {
			if (!Visible(node)) {
				continue;
			}
			if (some && std::find(Chosen.begin(), Chosen.end(), node.Id) == Chosen.end()) {
				continue;
			}
			const NodeLayout layout = LayoutOf(node, Look.Sizes);
			if (!any) {
				left = node.X;
				top = node.Y;
				right = node.X + layout.Width;
				bottom = node.Y + layout.Height;
				any = true;
				continue;
			}
			left = std::min(left, node.X);
			top = std::min(top, node.Y);
			right = std::max(right, node.X + layout.Width);
			bottom = std::max(bottom, node.Y + layout.Height);
		}

		if (!any) {
			return;
		}

		const float margin = 40.0f;
		const float width = std::max(right - left + margin * 2.0f, 1.0f);
		const float height = std::max(bottom - top + margin * 2.0f, 1.0f);

		SetZoom(std::min(ViewWidth / width, ViewHeight / height));
		PanX = -left + margin;
		PanY = -top + margin;
	}

	void Canvas::Centre(const Graph &graph, NodeId id) {
		const Node *node = graph.Find(id);
		if (node == nullptr) {
			return;
		}
		const NodeLayout layout = LayoutOf(*node, Look.Sizes);

		// **The zoom is left alone.** Somebody clicking a row in the Types panel
		// asked where a node is, not to be moved to a different magnification.
		PanX = ViewWidth * 0.5f / Scale - (node->X + layout.Width * 0.5f);
		PanY = ViewHeight * 0.5f / Scale - (node->Y + layout.Height * 0.5f);
	}

	void Canvas::Delete(Graph &graph) {
		if (ChosenGroup != NO_GROUP) {
			// **The frame and not what is in it.** Deleting a group's nodes is
			// on the context menu, spelled out, because it is the one delete
			// here that takes away something somebody cannot see selected.
			graph.Ungroup(ChosenGroup, false);
			ChosenGroup = NO_GROUP;
			return;
		}
		for (const NodeId id : Chosen) {
			graph.Remove(id);
		}
		Chosen.clear();
	}

	void Canvas::Copy(const Graph &graph) {
		Clip.Clear();

		// **A fold is copied with everything inside it.** Its ports are proxies
		// naming inner nodes, so a copy without them would be a node whose every
		// port pointed into the original — which reads as a duplicate and
		// behaves as a second view of the first.
		std::vector<NodeId> taken;
		const auto take = [&](auto &&self, NodeId id) -> void {
			if (std::find(taken.begin(), taken.end(), id) != taken.end()) {
				return;
			}
			const Node *node = graph.Find(id);
			if (node == nullptr) {
				return;
			}
			taken.push_back(id);
			Clip.Adopt(*node);
			for (const NodeId child : graph.Contents(id)) {
				self(self, child);
			}
		};
		for (const NodeId id : Chosen) {
			take(take, id);
		}

		// **Only the links with both ends in the copy.** A half link would paste
		// as a wire from a node that is not there, and the alternative — keeping
		// the outside end — pastes a copy already wired into the original, which
		// is never what duplicating means.
		for (const Link &link : graph.Links()) {
			const bool from = std::find(taken.begin(), taken.end(), link.From) != taken.end();
			const bool to = std::find(taken.begin(), taken.end(), link.To) != taken.end();
			if (from && to) {
				Clip.Attach(link);
			}
		}
	}

	void Canvas::Paste(Graph &graph) {
		if (Clip.Nodes().empty()) {
			return;
		}

		// **`Absorb` and not a second copy of the remap.** Pasting and placing a
		// library type are the same operation over a different source, and the
		// part that is easy to get wrong — remapping `Owner`, the proxies and
		// the promotion keys — is the part that must exist once.
		Select(graph.Absorb(Clip, 40.0f, 40.0f, Inside()));
	}

	void Canvas::Duplicate(Graph &graph) {
		// **Through the clipboard, so one code path makes both.** A duplicate
		// that copied nodes by hand would be a second place for the "only links
		// with both ends inside" rule to be got wrong — and what was on the
		// clipboard is put back, because duplicating is not copying.
		Graph held = std::move(Clip);

		Copy(graph);
		Paste(graph);

		Clip = std::move(held);
	}

	void Canvas::Place(Graph &graph, const std::string &document) {
		Graph held;
		std::string error;
		if (!Load(document, held, error)) {
			LastRefusal = error;
			return;
		}

		// Into the middle of the view, so a type picked out of a list lands
		// where somebody is looking rather than at the origin.
		float x = 0.0f;
		float y = 0.0f;
		ToGraph(OriginX + ViewWidth * 0.5f, OriginY + ViewHeight * 0.4f, x, y);

		const std::vector<NodeId> landed = graph.Absorb(held, x, y, Inside());
		if (landed.empty()) {
			LastRefusal = "that type placed nothing";
			return;
		}
		LastRefusal.clear();
		Select(landed);
	}

	void Canvas::GroupSelection(Graph &graph) {
		if (Chosen.size() < 2) {
			LastRefusal = "select two or more nodes to frame them";
			return;
		}

		// The palette cycles, so a graph with several frames reads as several
		// frames rather than as one colour used twice in a row.
		static const Colour PALETTE[] = {
			Colour::Hex(0x7C5CFF),
			Colour::Hex(0x4ADE80),
			Colour::Hex(0x38BDF8),
			Colour::Hex(0xFBBF24),
			Colour::Hex(0xF472B6),
			Colour::Hex(0x22D3EE),
		};
		const size_t at = graph.Groups().size() % (sizeof(PALETTE) / sizeof(PALETTE[0]));

		const GroupId made =
			graph.Group(Chosen, "Group " + std::to_string(graph.Groups().size() + 1), PALETTE[at]);
		if (made != NO_GROUP) {
			Chosen.clear();
			ChosenGroup = made;
			LastRefusal.clear();
		}
	}

	void Canvas::UngroupSelection(Graph &graph) {
		if (ChosenGroup != NO_GROUP) {
			graph.Ungroup(ChosenGroup, false);
			ChosenGroup = NO_GROUP;
			return;
		}

		// Nothing framed is selected, so take whichever frame the selection is
		// inside — which is what somebody with a node picked meant by "ungroup".
		for (const NodeId id : Chosen) {
			if (const GroupId frame = graph.GroupOf(id); frame != NO_GROUP) {
				graph.Ungroup(frame, false);
				return;
			}
		}
	}

	void Canvas::CompressSelection(Graph &graph) {
		if (Chosen.size() < 2) {
			LastRefusal = "select two or more nodes to fold them into one";
			return;
		}

		// The middle of the selection, so the new node lands where the work was
		// rather than at the origin.
		float x = 0.0f;
		float y = 0.0f;
		size_t counted = 0;
		for (const NodeId id : Chosen) {
			if (const Node *node = graph.Find(id); node != nullptr) {
				x += node->X;
				y += node->Y;
				counted++;
			}
		}
		if (counted == 0) {
			return;
		}

		const NodeId made =
			graph.Compress(Chosen, x / static_cast<float>(counted), y / static_cast<float>(counted));
		if (made == NO_NODE) {
			LastRefusal = "those nodes are not all at the same depth";
			return;
		}
		LastRefusal.clear();
		Select(made);
	}

	void Canvas::ExpandSelection(Graph &graph) {
		std::vector<NodeId> freed;
		for (const NodeId id : Chosen) {
			const Node *node = graph.Find(id);
			if (node == nullptr || !node->Compressed()) {
				continue;
			}
			// **Collected before anything is expanded**, because expanding
			// removes the node and rewrites the vector the members live in.
			for (const NodeId child : graph.Contents(id)) {
				freed.push_back(child);
			}
			graph.Expand(id);
		}

		if (freed.empty()) {
			LastRefusal = "nothing selected was folded";
			return;
		}
		LastRefusal.clear();
		Select(std::move(freed));
	}

	void Canvas::Enter(const Graph &graph, NodeId id) {
		const Node *node = graph.Find(id);
		if (node == nullptr || !node->Compressed() || node->Owner != Inside()) {
			return;
		}
		Depth.push_back(id);
		Chosen.clear();
		ChosenGroup = NO_GROUP;
		Fit(graph);
	}

	void Canvas::Leave(const Graph &graph) {
		if (Depth.empty()) {
			return;
		}
		const NodeId was = Depth.back();
		Depth.pop_back();

		// Selecting the node just left, so backing out lands on the thing that
		// was being edited rather than on nothing.
		Select(graph.Alive(was) ? was : NO_NODE);
		Fit(graph);
	}

	void Canvas::Ascend(const Graph &graph, size_t depth) {
		while (Depth.size() > depth) {
			Leave(graph);
		}
	}

	void Canvas::Collapse(Graph &graph, bool collapsed) {
		for (const NodeId id : Chosen) {
			if (Node *node = graph.Find(id); node != nullptr) {
				node->Collapsed = collapsed;
			}
		}
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

	void Canvas::DrawGroups(const Graph &graph) const {
		ImDrawList *draw = ImGui::GetWindowDrawList();

		for (const nodes::Group &frame : graph.Groups()) {
			float left = 0.0f;
			float top = 0.0f;
			float right = 0.0f;
			float bottom = 0.0f;
			if (!GroupBounds(graph, frame, left, top, right, bottom)) {
				continue;
			}

			ImVec2 corner;
			ImVec2 far;
			ImVec2 barFar;
			ToScreen(left, top, corner.x, corner.y);
			ToScreen(right, bottom, far.x, far.y);
			ToScreen(right, top + GROUP_BAR, barFar.x, barFar.y);

			const bool selected = ChosenGroup == frame.Id;
			const float rounding = Look.Sizes.Rounding * Scale;

			// **Faint, because a frame is behind its nodes and not around
			// them.** A solid fill turns six readable nodes into six nodes on a
			// coloured rectangle, which is worse at the one job a frame has.
			draw->AddRectFilled(corner, far, Packed(frame.Tint, 0.08f), rounding);
			draw->AddRectFilled(
				corner, barFar, Packed(frame.Tint, 0.30f), rounding, ImDrawFlags_RoundCornersTop
			);
			draw->AddRect(
				corner, far, Packed(frame.Tint, selected ? 0.95f : 0.5f), rounding, 0, selected ? 2.0f : 1.0f
			);

			const float size = Look.Sizes.SmallSize * Scale;
			if (size >= 5.0f) {
				draw->AddText(
					nullptr,
					size,
					ImVec2(corner.x + 8.0f * Scale, corner.y + (GROUP_BAR * Scale - size) * 0.5f),
					Look.Text,
					frame.Title.c_str()
				);
			}
		}
	}

	void Canvas::DrawLinks(const Graph &graph, size_t hovered) const {
		ImDrawList *draw = ImGui::GetWindowDrawList();

		for (size_t index = 0; index < graph.Links().size(); index++) {
			float fromX = 0.0f;
			float fromY = 0.0f;
			float toX = 0.0f;
			float toY = 0.0f;
			std::string carried;

			// **Drawn where the ends appear at this depth**, which is the whole
			// visual effect of a fold: a wire into it stops at its proxy port,
			// and one wholly inside it is not drawn out here at all.
			if (!Ends(graph, graph.Links()[index], Look.Sizes, Inside(), fromX, fromY, toX, toY, carried)) {
				continue;
			}

			ImVec2 a;
			ImVec2 b;
			ToScreen(fromX, fromY, a.x, a.y);
			ToScreen(toX, toY, b.x, b.y);

			// Coloured by what it carries, which is the same rule the ports use
			// — a link's colour is a fact about the data and never about which
			// node it came from.
			const bool lit = index == hovered;
			const bool wanted = Highlight.empty() || Highlight == carried;
			uint32_t colour = lit ? Look.NodeSelected : TintOf(carried);
			if (!wanted) {
				colour = Faded(colour);
			}
			Curve(draw, a, b, colour, std::max(lit ? 3.0f : 1.5f, (lit ? 3.5f : 2.0f) * Scale), Scale);

			// **A `+` on the link somebody is pointing at, and on no other.**
			// One per link is a graph covered in buttons; the hovered one is the
			// only one anybody is about to press.
			if (!lit) {
				continue;
			}
			float middleX = 0.0f;
			float middleY = 0.0f;
			if (!LinkMiddle(graph, index, middleX, middleY)) {
				continue;
			}

			ImVec2 at;
			ToScreen(middleX, middleY, at.x, at.y);
			const float radius = 9.0f * std::max(Scale, 0.6f);
			draw->AddCircleFilled(at, radius, Look.NodeBody);
			draw->AddCircle(at, radius, Look.NodeSelected, 0, 1.5f);
			draw->AddLine(
				ImVec2(at.x - radius * 0.45f, at.y), ImVec2(at.x + radius * 0.45f, at.y), Look.Text, 1.5f
			);
			draw->AddLine(
				ImVec2(at.x, at.y - radius * 0.45f), ImVec2(at.x, at.y + radius * 0.45f), Look.Text, 1.5f
			);
		}
	}

	void Canvas::DrawWidget(const Graph &graph, const Node &node, const PlacedWidget &placed) const {
		ImDrawList *draw = ImGui::GetWindowDrawList();

		// **Read through the node's real interface and `ValueOf`**, so a
		// compressed node paints the value its promoted knob writes to. Reading
		// `node.Widgets` here would draw a zero under a slider that is set.
		const std::vector<WidgetSpec> widgets = WidgetsOf(node);
		if (placed.Index >= widgets.size()) {
			return;
		}
		const WidgetSpec &spec = widgets[placed.Index];
		const Value value = ValueOf(graph, node.Id, spec);

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
				nullptr,
				size,
				ImVec2(corner.x + 4.0f * Scale, corner.y + 2.0f * Scale),
				Look.Text,
				text.c_str()
			);
		}
	}

	void Canvas::DrawNode(const Graph &graph, const Node &node, const NodeLayout &layout) const {
		ImDrawList *draw = ImGui::GetWindowDrawList();
		const NodeType *type = NodeTypes::Find(node.Type);

		ImVec2 corner;
		ImVec2 far;
		ToScreen(node.X, node.Y, corner.x, corner.y);
		ToScreen(node.X + layout.Width, node.Y + layout.Height, far.x, far.y);

		const bool selected = std::find(Chosen.begin(), Chosen.end(), node.Id) != Chosen.end();

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
									  : type != nullptr	  ? type->Title
														  : node.Type;
			draw->AddText(
				nullptr,
				title,
				ImVec2(corner.x + 8.0f * Scale, corner.y + 5.0f * Scale),
				0xFF101010,
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

			const bool wanted = Highlight.empty() || Highlight == port.Type;
			const uint32_t tint = wanted ? TintOf(port.Type) : Faded(TintOf(port.Type));
			draw->AddCircleFilled(at, Look.Sizes.PortRadius * Scale * (wanted ? 1.0f : 0.7f), tint);
			draw->AddCircle(at, Look.Sizes.PortRadius * Scale, Look.NodeBorder);

			// **No labels on a collapsed node.** Its ports are stacked twelve
			// units apart, so the names would overlap each other and the body —
			// and a collapsed node is one nobody is currently reading.
			if (small < 5.0f || node.Collapsed) {
				continue;
			}
			const ImVec2 size = ImGui::CalcTextSize(port.Name.c_str());
			const float width = size.x * (small / ImGui::GetFontSize());
			draw->AddText(
				nullptr,
				small,
				ImVec2(port.Input ? at.x + 10.0f * Scale : at.x - 10.0f * Scale - width, at.y - small * 0.5f),
				Look.Muted,
				port.Name.c_str()
			);
		}

		for (const PlacedWidget &placed : layout.Widgets) {
			DrawWidget(graph, node, placed);
		}

		// --- the picture ------------------------------------------------------
		//
		// **Drawn from what the node produced, through the host's sink.** The
		// payload is converted at most once per result: the key is the hash it
		// was computed at, so panning, zooming and dragging cost a lookup and
		// nothing else.
		if (layout.PreviewSide > 0.0f) {
			ImVec2 slot;
			ImVec2 slotFar;
			ToScreen(node.X + Look.Sizes.Padding, node.Y + layout.PreviewTop, slot.x, slot.y);
			ToScreen(
				node.X + Look.Sizes.Padding + layout.PreviewSide,
				node.Y + layout.PreviewTop + layout.PreviewSide,
				slotFar.x,
				slotFar.y
			);

			void *handle = nullptr;
			if (Sink && Watching != nullptr) {
				const std::string port =
					type->PreviewPort.empty()
						? (type->Outputs.empty() ? std::string() : type->Outputs.front().Name)
						: type->PreviewPort;

				// The port's declared type, so the wire's own painter can be
				// found when the node type declared none of its own.
				std::string carried;
				for (const PortSpec &declared : type->Outputs) {
					if (declared.Name == port) {
						carried = declared.Type;
						break;
					}
				}

				if (const std::any *payload = port.empty() ? nullptr : Watching->Output(node.Id, port);
					payload != nullptr) {
					const std::any &held = *payload;
					const NodeType *maker = type;
					handle = Sink(
						PictureKey(Watching->RanAt(node.Id), port),
						[&held, maker, &carried](PreviewImage &image) {
							return PictureOf(maker, carried, held, image);
						}
					);
				}
			}

			if (handle != nullptr) {
				draw->AddImage(reinterpret_cast<ImTextureID>(handle), slot, slotFar);
			} else {
				// **A frame of the same size rather than nothing.** The slot is
				// reserved by the layout either way, and an empty node body
				// reads as a node that does not work rather than as a result
				// that has not arrived.
				draw->AddRectFilled(slot, slotFar, 0xFF141414);
			}
			draw->AddRect(slot, slotFar, Look.NodeBorder);
		}

		// --- what it is doing --------------------------------------------------

		const NodeStatus status = Watching != nullptr ? Watching->Status(node.Id) : NodeStatus{};

		if (layout.ProgressHeight > 0.0f) {
			ImVec2 bar;
			ImVec2 barFar;
			ToScreen(node.X + Look.Sizes.Padding, node.Y + layout.ProgressTop, bar.x, bar.y);
			ToScreen(
				node.X + layout.Width - Look.Sizes.Padding,
				node.Y + layout.ProgressTop + layout.ProgressHeight,
				barFar.x,
				barFar.y
			);

			draw->AddRectFilled(bar, barFar, Look.Widget, 2.0f * Scale);

			const float fraction = std::clamp(status.Progress, 0.0f, 1.0f);
			if (fraction > 0.0f) {
				draw->AddRectFilled(
					bar,
					ImVec2(bar.x + (barFar.x - bar.x) * fraction, barFar.y),
					status.State == NodeState::Failed ? Look.Refused : Look.WidgetFill,
					2.0f * Scale
				);
			}

			if (small >= 5.0f) {
				// The step it says it is on, or how long it took once it is
				// done. A spinner says a node is busy; this says at what.
				std::string words = status.Note;
				if (words.empty() && status.State == NodeState::Done) {
					char text[32];
					std::snprintf(text, sizeof(text), "%.0f ms", status.Milliseconds);
					words = text;
				}
				if (!words.empty()) {
					draw->AddText(
						nullptr,
						small,
						ImVec2(bar.x + 4.0f * Scale, bar.y - 1.0f * Scale),
						Look.Text,
						words.c_str()
					);
				}
			}
		}

		// The cached badge, when somebody is watching an evaluator. It is the
		// one piece of evaluation state worth putting on the canvas: a node that
		// did not recompute when you expected it to is the first sign that a
		// hash is wrong.
		if (Watching != nullptr && small >= 5.0f) {
			const char *badge = status.Cached						 ? "cached"
								: status.State == NodeState::Running ? "working"
								: status.State == NodeState::Failed	 ? "failed"
																	 : nullptr;
			if (badge != nullptr) {
				const ImVec2 size = ImGui::CalcTextSize(badge);
				const float width = size.x * (small / ImGui::GetFontSize());
				draw->AddText(
					nullptr,
					small,
					ImVec2(far.x - width - 8.0f * Scale, corner.y + 6.0f * Scale),
					0xFF101010,
					badge
				);
			}
		}
	}

	NodeId Canvas::HitNode(const Graph &graph, float graphX, float graphY) const {
		// **Backwards, because the last drawn is the one on top.** Hit testing
		// forwards would pick the node underneath whenever two overlap, which is
		// exactly when somebody is trying to grab the one they can see.
		for (auto node = graph.Nodes().rbegin(); node != graph.Nodes().rend(); ++node) {
			if (!Visible(*node)) {
				continue;
			}
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
			if (!Visible(*walk)) {
				continue;
			}
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

	size_t Canvas::HitLink(const Graph &graph, float graphX, float graphY) const {
		// The grab radius in graph units, so it is the same handful of screen
		// pixels at any zoom. A fixed graph-space radius would be untouchable
		// zoomed out and enormous zoomed in.
		const float reach = 8.0f / std::max(Scale, 0.05f);

		size_t nearest = static_cast<size_t>(-1);
		float best = reach * reach;

		for (size_t index = 0; index < graph.Links().size(); index++) {
			float fromX = 0.0f;
			float fromY = 0.0f;
			float toX = 0.0f;
			float toY = 0.0f;
			std::string carried;
			if (!Ends(graph, graph.Links()[index], Look.Sizes, Inside(), fromX, fromY, toX, toY, carried)) {
				continue;
			}

			constexpr int SAMPLES = 16;
			for (int step = 0; step <= SAMPLES; step++) {
				float x = 0.0f;
				float y = 0.0f;
				Along(fromX, fromY, toX, toY, static_cast<float>(step) / static_cast<float>(SAMPLES), x, y);
				const float dx = graphX - x;
				const float dy = graphY - y;
				const float distance = dx * dx + dy * dy;
				if (distance < best) {
					best = distance;
					nearest = index;
				}
			}
		}
		return nearest;
	}

	bool Canvas::LinkMiddle(const Graph &graph, size_t index, float &outX, float &outY) const {
		if (index >= graph.Links().size()) {
			return false;
		}
		float fromX = 0.0f;
		float fromY = 0.0f;
		float toX = 0.0f;
		float toY = 0.0f;
		std::string carried;
		if (!Ends(graph, graph.Links()[index], Look.Sizes, Inside(), fromX, fromY, toX, toY, carried)) {
			return false;
		}
		Along(fromX, fromY, toX, toY, 0.5f, outX, outY);
		return true;
	}

	bool Canvas::GroupBounds(
		const Graph &graph, const nodes::Group &group, float &left, float &top, float &right, float &bottom
	) const {
		bool any = false;
		for (const NodeId id : group.Members) {
			const Node *node = graph.Find(id);
			if (node == nullptr) {
				continue;
			}
			const NodeLayout layout = LayoutOf(*node, Look.Sizes);
			if (!any) {
				left = node->X;
				top = node->Y;
				right = node->X + layout.Width;
				bottom = node->Y + layout.Height;
				any = true;
				continue;
			}
			left = std::min(left, node->X);
			top = std::min(top, node->Y);
			right = std::max(right, node->X + layout.Width);
			bottom = std::max(bottom, node->Y + layout.Height);
		}

		if (!any) {
			return false;
		}

		// Room for the frame and its title bar, which is what a group is grabbed
		// by — the body stays free so a marquee still works inside one.
		constexpr float PAD = 18.0f;
		left -= PAD;
		right += PAD;
		bottom += PAD;
		top -= PAD + GROUP_BAR;
		return true;
	}

	GroupId Canvas::HitGroup(const Graph &graph, float graphX, float graphY) const {
		for (auto walk = graph.Groups().rbegin(); walk != graph.Groups().rend(); ++walk) {
			float left = 0.0f;
			float top = 0.0f;
			float right = 0.0f;
			float bottom = 0.0f;
			if (!GroupBounds(graph, *walk, left, top, right, bottom)) {
				continue;
			}
			if (graphX >= left && graphX <= right && graphY >= top && graphY <= top + GROUP_BAR) {
				return walk->Id;
			}
		}
		return NO_GROUP;
	}

	void Canvas::InsertOn(Graph &graph, size_t index, const std::string &type) {
		if (index >= graph.Links().size()) {
			return;
		}

		const Link link = graph.Links()[index];
		const NodeType *added = NodeTypes::Find(type);
		if (added == nullptr) {
			return;
		}

		float middleX = 0.0f;
		float middleY = 0.0f;
		if (!LinkMiddle(graph, index, middleX, middleY)) {
			return;
		}

		// **Placed where the link was, not where the pointer is.** The gesture
		// is "put one of these in here"; landing the node at the cursor would
		// make the graph jump about under a click meant to be surgical.
		const NodeId made = graph.Add(type, middleX - added->Width * 0.5f, middleY - Look.Sizes.HeaderHeight);
		if (made == NO_NODE) {
			return;
		}

		graph.Disconnect(link.To, link.ToPort);

		// The first port on each side that will carry what the link carried. A
		// node that takes it but produces nothing compatible still gets wired on
		// its input, which is the useful half of what was asked for.
		for (const PortSpec &port : added->Inputs) {
			if (graph.Connect(link.From, link.FromPort, made, port.Name) == LinkResult::Made) {
				break;
			}
		}
		for (const PortSpec &port : added->Outputs) {
			if (graph.Connect(made, port.Name, link.To, link.ToPort) == LinkResult::Made) {
				break;
			}
		}

		Select(made);
	}

	void Canvas::HandleWidget(Graph &graph, NodeId id, const PlacedWidget &placed, float graphX) {
		Node *node = graph.Find(id);
		if (node == nullptr) {
			return;
		}
		const std::vector<WidgetSpec> widgets = WidgetsOf(*node);
		if (placed.Index >= widgets.size()) {
			return;
		}

		// **Read, changed, then written through `SetValue`.** A reference into
		// `node->Widgets` would edit the compressed node's own empty map rather
		// than the inner node its knob was promoted from.
		const WidgetSpec &spec = widgets[placed.Index];
		Value value = ValueOf(graph, id, spec);
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
				at == spec.Options.end()
					? 0
					: (static_cast<size_t>(at - spec.Options.begin()) + 1) % spec.Options.size();
			value.Text = spec.Options[next];
			break;
		}
		case WidgetKind::Text:
		case WidgetKind::Colour:
			// **Left to the inspector rather than done badly here.** Editing
			// text on a zoomable canvas needs a caret, a selection and an IME,
			// which is a DOM input in the JS template and an `InputText` in a
			// panel here — not something to reimplement with a draw list.
			return;
		}

		SetValue(graph, id, spec.Key, value);
	}

	namespace {
		// Case-insensitive substring, for the palette's filter box. `find` on
		// two lowered copies rather than a locale-aware compare: this is a
		// search box over node titles and nothing here is worth a facet.
		bool Contains(const std::string &haystack, const std::string &needle) {
			if (needle.empty()) {
				return true;
			}
			std::string lowered = haystack;
			std::string wanted = needle;
			const auto lower = [](std::string &text) {
				for (char &letter : text) {
					letter = static_cast<char>(std::tolower(static_cast<unsigned char>(letter)));
				}
			};
			lower(lowered);
			lower(wanted);
			return lowered.find(wanted) != std::string::npos;
		}
	}

	void Canvas::Palette(Graph &graph) {
		if (!PaletteOpen) {
			return;
		}

		if (!ImGui::BeginPopup("nodegraph.palette")) {
			PaletteOpen = false;
			DragNode = NO_NODE;
			PaletteLink = static_cast<size_t>(-1);
			return;
		}

		// **Focused on the first frame it is open.** A palette that needed a
		// click in its own search box before it would take typing is a palette
		// slower than the menu it replaced.
		if (ImGui::IsWindowAppearing()) {
			ImGui::SetKeyboardFocusHere();
		}
		ImGui::SetNextItemWidth(engine::ui::Scaled(200.0f));
		const bool entered = ImGui::InputTextWithHint(
			"##search",
			"search nodes",
			PaletteSearch,
			sizeof(PaletteSearch),
			ImGuiInputTextFlags_EnterReturnsTrue
		);

		// **Filtered to what could actually be connected**, which is the whole
		// reason dropping a wire in empty space is a shortcut rather than a
		// second way to add a node: the list is already the answer.
		const auto fits = [this](const NodeType &type) {
			if (PaletteNeeds == Needs::Anything || PaletteType.empty()) {
				return true;
			}
			const auto takes = [this](const std::vector<PortSpec> &ports, bool into) {
				for (const PortSpec &port : ports) {
					if (into ? DataTypes::CanConnect(PaletteType, port.Type)
							 : DataTypes::CanConnect(port.Type, PaletteType)) {
						return true;
					}
				}
				return false;
			};

			const bool hasInput = takes(type.Inputs, true);
			const bool hasOutput = takes(type.Outputs, false);
			switch (PaletteNeeds) {
			case Needs::Input:
				return hasInput;
			case Needs::Output:
				return hasOutput;
			case Needs::Both:
				return hasInput && hasOutput;
			case Needs::Anything:
				break;
			}
			return true;
		};

		const std::string search = PaletteSearch;
		std::string picked;

		if (PaletteNeeds != Needs::Anything && !PaletteType.empty()) {
			const DataType *carried = DataTypes::Find(PaletteType);
			ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
			ImGui::Text(
				"compatible with %s", carried != nullptr ? carried->Label.c_str() : PaletteType.c_str()
			);
			ImGui::PopStyleColor();
		}

		ImGui::Separator();

		if (ImGui::BeginChild("##list", ImVec2(engine::ui::Scaled(200.0f), engine::ui::Scaled(280.0f)))) {
			for (const std::string &category : NodeTypes::Categories()) {
				// **The header is drawn only once something under it survived
				// the filter.** A palette of empty categories is a list of
				// headings, which is the shape a search is meant to remove.
				bool opened = false;
				for (const NodeType &type : NodeTypes::All()) {
					if (type.Hidden || type.Category != category || !fits(type) ||
						!Contains(type.Title + " " + type.Category, search)) {
						continue;
					}

					if (!opened) {
						opened = true;
						ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
						ImGui::TextUnformatted(category.c_str());
						ImGui::PopStyleColor();
					}

					const ImVec2 at = ImGui::GetCursorScreenPos();
					const float mark = ImGui::GetTextLineHeight();
					ImGui::GetWindowDrawList()->AddRectFilled(
						ImVec2(at.x, at.y + mark * 0.2f),
						ImVec2(at.x + mark * 0.28f, at.y + mark * 0.8f),
						Packed(type.Accent),
						1.0f
					);
					ImGui::Dummy(ImVec2(mark * 0.28f, mark));
					ImGui::SameLine();

					if (ImGui::Selectable(type.Title.c_str())) {
						picked = type.Id;
					}
					if (picked.empty() && entered) {
						// Enter takes the first surviving row, which is what a
						// search box that filters to one answer should do.
						picked = type.Id;
					}
				}
			}
		}
		ImGui::EndChild();

		if (!picked.empty()) {
			if (PaletteLink != static_cast<size_t>(-1)) {
				InsertOn(graph, PaletteLink, picked);
			} else {
				const NodeId made = graph.Add(picked, PaletteX, PaletteY);
				if (made != NO_NODE) {
					// **Placed at the depth being looked at**, or a node added
					// inside a fold would appear outside it.
					graph.Find(made)->Owner = Inside();
				}
				if (made != NO_NODE && DragNode != NO_NODE) {
					// Wire it to whatever the drag came from, on the first port
					// that will take it.
					NodeId anchor = NO_NODE;
					std::string anchorPort;
					if (Actual(graph, DragNode, DragPort, DragFromInput, anchor, anchorPort)) {
						const NodeType *added = NodeTypes::Find(picked);
						for (const PortSpec &port : DragFromInput ? added->Outputs : added->Inputs) {
							const LinkResult result =
								DragFromInput ? graph.Connect(made, port.Name, anchor, anchorPort)
											  : graph.Connect(anchor, anchorPort, made, port.Name);
							if (result == LinkResult::Made) {
								break;
							}
						}
					}
				}
				Select(made);
			}

			Edited = true;
			PaletteOpen = false;
			PaletteLink = static_cast<size_t>(-1);
			DragNode = NO_NODE;
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
	}

	void Canvas::Menu(Graph &graph) {
		if (!MenuOpen) {
			return;
		}

		if (!ImGui::BeginPopup("nodegraph.menu")) {
			MenuOpen = false;
			return;
		}

		const auto open = [this](Needs needs, std::string type, size_t link) {
			PaletteNeeds = needs;
			PaletteType = std::move(type);
			PaletteLink = link;
			PaletteSearch[0] = '\0';
			PaletteWanted = true;
		};

		if (MenuNode != NO_NODE) {
			const Node *node = graph.Find(MenuNode);
			const bool collapsed = node != nullptr && node->Collapsed;

			if (ImGui::MenuItem("Duplicate", "Ctrl+D")) {
				Duplicate(graph);
				Edited = true;
			}
			if (ImGui::MenuItem(collapsed ? "Expand node" : "Collapse node")) {
				Collapse(graph, !collapsed);
				Edited = true;
			}
			if (ImGui::MenuItem("Disconnect all")) {
				// **Through the node's visible interface**, so this also clears
				// the wires crossing a fold's proxy ports — which are links the
				// graph holds against nodes inside it.
				for (const NodeId id : Chosen) {
					const Node *chosen = graph.Find(id);
					if (chosen == nullptr) {
						continue;
					}
					for (const PortSpec &port : InputsOf(*chosen)) {
						NodeId held = NO_NODE;
						std::string heldPort;
						if (Actual(graph, id, port.Name, true, held, heldPort)) {
							graph.Disconnect(held, heldPort);
						}
					}
					for (const PortSpec &port : OutputsOf(*chosen)) {
						NodeId held = NO_NODE;
						std::string heldPort;
						if (!Actual(graph, id, port.Name, false, held, heldPort)) {
							continue;
						}
						for (const Link &link : graph.LinksOf(held)) {
							if (link.From == held && link.FromPort == heldPort) {
								graph.Disconnect(link.To, link.ToPort);
							}
						}
					}
				}
				Edited = true;
			}

			ImGui::Separator();
			const Node *folded = graph.Find(MenuNode);
			if (ImGui::MenuItem("Compress selection", "Ctrl+Shift+C", false, Chosen.size() > 1)) {
				CompressSelection(graph);
				Edited = true;
			}
			if (folded != nullptr && folded->Compressed()) {
				if (ImGui::MenuItem("Enter subgraph", "Enter")) {
					Enter(graph, MenuNode);
				}
				if (ImGui::MenuItem("Expand in place", "Ctrl+Shift+X")) {
					ExpandSelection(graph);
					Edited = true;
				}
			}
			if (ImGui::MenuItem("Group", "Ctrl+G", false, Chosen.size() > 1)) {
				GroupSelection(graph);
				Edited = true;
			}
			if (ImGui::MenuItem("Ungroup", "Ctrl+Shift+G")) {
				UngroupSelection(graph);
				Edited = true;
			}

			ImGui::Separator();
			if (ImGui::MenuItem("Re-run this node") && Signals.Rerun) {
				Signals.Rerun(MenuNode);
			}
			if (ImGui::MenuItem("Delete", "Del")) {
				Delete(graph);
				Edited = true;
			}
		} else if (MenuLink != static_cast<size_t>(-1) && MenuLink < graph.Links().size()) {
			const Link link = graph.Links()[MenuLink];
			const Node *from = graph.Find(link.From);
			const NodeType *type = from == nullptr ? nullptr : NodeTypes::Find(from->Type);

			std::string carried;
			if (type != nullptr) {
				for (const PortSpec &port : type->Outputs) {
					if (port.Name == link.FromPort) {
						carried = port.Type;
						break;
					}
				}
			}

			if (ImGui::MenuItem("Insert node here")) {
				// **Both sides have to fit**, because a splice is two links: one
				// that offered only an input would leave the downstream node
				// unconnected and the graph quietly shorter.
				open(Needs::Both, carried, MenuLink);
			}
			if (ImGui::MenuItem("Delete link")) {
				graph.Disconnect(link.To, link.ToPort);
				Edited = true;
			}
		} else if (MenuGroup != NO_GROUP) {
			if (ImGui::MenuItem("Ungroup")) {
				graph.Ungroup(MenuGroup, false);
				ChosenGroup = NO_GROUP;
				Edited = true;
			}
			if (ImGui::MenuItem("Delete group and its nodes")) {
				graph.Ungroup(MenuGroup, true);
				ChosenGroup = NO_GROUP;
				Chosen.clear();
				Edited = true;
			}
		} else {
			if (ImGui::MenuItem("Add node...", "Tab")) {
				open(Needs::Anything, std::string(), static_cast<size_t>(-1));
			}
			if (ImGui::MenuItem("Paste", "Ctrl+V", false, CanPaste())) {
				Paste(graph);
				Edited = true;
			}
			if (ImGui::MenuItem("Select all", "Ctrl+A")) {
				SelectAll(graph);
			}
			ImGui::Separator();
			if (ImGui::MenuItem("Fit view", "F")) {
				Fit(graph);
			}
		}

		ImGui::EndPopup();
	}

	void Canvas::Draw(Graph &graph) {
		ImGuiIO &io = ImGui::GetIO();
		ImDrawList *draw = ImGui::GetWindowDrawList();

		// The palette asked for on the previous frame, now that no other popup
		// is in the middle of closing.
		if (PaletteWanted) {
			PaletteWanted = false;
			PaletteOpen = true;
			ImGui::OpenPopup("nodegraph.palette");
		}

		const ImVec2 origin = ImGui::GetCursorScreenPos();
		const ImVec2 size = ImGui::GetContentRegionAvail();
		OriginX = origin.x;
		OriginY = origin.y;
		ViewWidth = std::max(size.x, 1.0f);
		ViewHeight = std::max(size.y, 1.0f);

		// **One invisible button for the whole canvas.** It is what makes the
		// window's own scrolling and dragging stand aside, and it gives one
		// `IsItemHovered` to test instead of a rectangle comparison per event.
		ImGui::InvisibleButton(
			"nodegraph.canvas",
			size,
			ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
				ImGuiButtonFlags_MouseButtonMiddle
		);
		const bool hovered = ImGui::IsItemHovered();

		draw->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);
		DrawGrid(origin.x, origin.y, size.x, size.y);
		DrawGroups(graph);

		float mouseX = 0.0f;
		float mouseY = 0.0f;
		ToGraph(io.MousePos.x, io.MousePos.y, mouseX, mouseY);

		// **Only when nothing else is under the pointer.** A link passing behind
		// a node must not steal the hover from it, or the insert button appears
		// over a node somebody is trying to grab.
		const bool overNode = HitNode(graph, mouseX, mouseY) != NO_NODE;
		const size_t litLink = hovered && !overNode && Drag == Dragging::None ? HitLink(graph, mouseX, mouseY)
																			  : static_cast<size_t>(-1);

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

		if (hovered && Drag == Dragging::None && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.KeyAlt) {
			NodeId node = NO_NODE;
			std::string port;
			bool input = false;

			// The insert button on the hovered link, before anything else — it
			// is drawn on top of the link and has to be clickable there.
			float plusX = 0.0f;
			float plusY = 0.0f;
			const bool onPlus = litLink != static_cast<size_t>(-1) &&
								LinkMiddle(graph, litLink, plusX, plusY) &&
								(mouseX - plusX) * (mouseX - plusX) + (mouseY - plusY) * (mouseY - plusY) <=
									(11.0f / std::max(Scale, 0.05f)) * (11.0f / std::max(Scale, 0.05f));

			if (onPlus) {
				const Link &link = graph.Links()[litLink];
				const Node *from = graph.Find(link.From);
				const NodeType *made = from == nullptr ? nullptr : NodeTypes::Find(from->Type);

				PaletteType.clear();
				if (made != nullptr) {
					for (const PortSpec &declared : made->Outputs) {
						if (declared.Name == link.FromPort) {
							PaletteType = declared.Type;
							break;
						}
					}
				}
				PaletteNeeds = Needs::Both;
				PaletteLink = litLink;
				PaletteSearch[0] = '\0';
				PaletteWanted = true;
			} else if (HitPort(graph, mouseX, mouseY, node, port, input)) {
				// **Dragging from a connected input picks the link up** rather
				// than starting a second one, which is what every node editor
				// does and what an input taking one link makes possible.
				if (input) {
					// **Resolved to the port the model actually holds.** On a
					// compressed node the visible port is a proxy; asking the
					// graph about it by name would find no link and start a
					// second one on top of the first.
					NodeId held = NO_NODE;
					std::string heldPort;
					const bool real = Actual(graph, node, port, true, held, heldPort);

					if (const Link *existing = real ? graph.LinkInto(held, heldPort) : nullptr;
						existing != nullptr) {
						// The far end is put back into view coordinates, so the
						// rubber band leaves the node somebody can see.
						NodeId shown = NO_NODE;
						std::string shownPort;
						if (Standing(
								graph, existing->From, existing->FromPort, false, Inside(), shown, shownPort
							)) {
							DragNode = shown;
							DragPort = shownPort;
							DragFromInput = false;
							graph.Disconnect(held, heldPort);
							Edited = true;
						}
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
					// **A double click opens a fold and collapses anything
					// else.** It is the one gesture over a node body that is not
					// "move me", and which of the two it means depends on
					// whether the node has an inside.
					bool entered = false;
					if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
						Node *toggled = graph.Find(hit);
						if (toggled != nullptr && toggled->Compressed()) {
							Enter(graph, hit);
							entered = true;
						} else if (toggled != nullptr) {
							toggled->Collapsed = !toggled->Collapsed;
							Edited = true;
						}
					}

					// **Guarded rather than returned from**, because the clip
					// rect pushed at the top of this function has to come off on
					// every path out of it.
					if (!entered) {
						if (std::find(Chosen.begin(), Chosen.end(), hit) == Chosen.end()) {
							if (!io.KeyShift) {
								Chosen.clear();
							}
							Chosen.push_back(hit);
						}
						ChosenGroup = NO_GROUP;
						Drag = Dragging::Nodes;
					}
				}
			} else if (const GroupId frame = HitGroup(graph, mouseX, mouseY); frame != NO_GROUP) {
				// A frame's bar. Selecting it selects the frame rather than its
				// members, so Delete removes the frame and not the work.
				Chosen.clear();
				ChosenGroup = frame;
				DragGroup = frame;
				Drag = Dragging::Group;
			} else {
				if (!io.KeyShift) {
					Chosen.clear();
				}
				ChosenGroup = NO_GROUP;
				Drag = Dragging::Marquee;
				MarqueeX = mouseX;
				MarqueeY = mouseY;
			}
		}

		// --- continuing one ---------------------------------------------------

		// **Applied on release and not per frame.** A snap during the drag makes
		// the node stutter under the pointer and makes small adjustments
		// impossible; snapping where it lands is what the toggle was asked for.
		const auto snapped = [this](Node &node) {
			if (!Snap) {
				return;
			}
			constexpr float GRID = 24.0f;
			node.X = std::round(node.X / GRID) * GRID;
			node.Y = std::round(node.Y / GRID) * GRID;
		};

		if (Drag == Dragging::Nodes && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
			for (const NodeId id : Chosen) {
				if (Node *node = graph.Find(id); node != nullptr) {
					node->X += io.MouseDelta.x / Scale;
					node->Y += io.MouseDelta.y / Scale;
					Edited = Edited || io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f;
				}
			}
		}

		// A frame drags everything it holds. That is the whole point of one —
		// and it is why membership rather than a rectangle is what is stored.
		if (Drag == Dragging::Group && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
			if (const nodes::Group *frame = graph.FindGroup(DragGroup); frame != nullptr) {
				for (const NodeId id : frame->Members) {
					if (Node *node = graph.Find(id); node != nullptr) {
						node->X += io.MouseDelta.x / Scale;
						node->Y += io.MouseDelta.y / Scale;
						Edited = Edited || io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f;
					}
				}
			}
		}

		if (Drag == Dragging::Widget && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
			if (const Node *node = graph.Find(DragNode); node != nullptr) {
				const NodeLayout layout = LayoutOf(*node, Look.Sizes);
				for (const PlacedWidget &placed : layout.Widgets) {
					if (placed.Key == DragWidget) {
						HandleWidget(graph, DragNode, placed, mouseX);
						Edited = true;
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
			if ((Drag == Dragging::Nodes || Drag == Dragging::Group)) {
				for (const NodeId id : Chosen) {
					if (Node *node = graph.Find(id); node != nullptr) {
						snapped(*node);
					}
				}
				if (const nodes::Group *frame = graph.FindGroup(DragGroup); frame != nullptr) {
					for (const NodeId id : frame->Members) {
						if (Node *node = graph.Find(id); node != nullptr) {
							snapped(*node);
						}
					}
				}
				DragGroup = NO_GROUP;
			}

			if (Drag == Dragging::Link && DragNode != NO_NODE) {
				NodeId node = NO_NODE;
				std::string port;
				bool input = false;

				if (HitPort(graph, mouseX, mouseY, node, port, input) && input != DragFromInput) {
					// Both ends resolved out of view coordinates: a wire dropped
					// on a fold's proxy port lands on the inner port it names,
					// which is what keeps compression invisible to the model.
					NodeId dropNode = NO_NODE;
					NodeId dragNode = NO_NODE;
					std::string dropPort;
					std::string dragPort;

					const bool both = Actual(graph, node, port, input, dropNode, dropPort) &&
									  Actual(graph, DragNode, DragPort, DragFromInput, dragNode, dragPort);

					const LinkResult result = !both ? LinkResult::NoSuchPort
											  : DragFromInput
												  ? graph.Connect(dropNode, dropPort, dragNode, dragPort)
												  : graph.Connect(dragNode, dragPort, dropNode, dropPort);
					LastRefusal = result == LinkResult::Made ? std::string() : Describe(result);
					Edited = Edited || result == LinkResult::Made;
					DragNode = NO_NODE;
				} else if (!HitPort(graph, mouseX, mouseY, node, port, input)) {
					// Into empty space: offer what could be connected there, and
					// only what could be — the drag already said which type and
					// which end of it needs a port.
					const Node *source = graph.Find(DragNode);
					const NodeType *type = source == nullptr ? nullptr : NodeTypes::Find(source->Type);

					PaletteType.clear();
					if (type != nullptr) {
						for (const PortSpec &declared : DragFromInput ? type->Inputs : type->Outputs) {
							if (declared.Name == DragPort) {
								PaletteType = declared.Type;
								break;
							}
						}
					}
					PaletteNeeds = DragFromInput ? Needs::Output : Needs::Input;
					PaletteLink = static_cast<size_t>(-1);
					PaletteSearch[0] = '\0';
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
					if (!Visible(node)) {
						continue;
					}
					const NodeLayout layout = LayoutOf(node, Look.Sizes);
					const float left = std::min(MarqueeX, mouseX);
					const float right = std::max(MarqueeX, mouseX);
					const float top = std::min(MarqueeY, mouseY);
					const float bottom = std::max(MarqueeY, mouseY);

					// Intersecting rather than enclosed, which is what a marquee
					// over a graph means: nodes are large and an enclosing rule
					// makes selecting three of them a full-screen drag.
					if (node.X + layout.Width >= left && node.X <= right && node.Y + layout.Height >= top &&
						node.Y <= bottom) {
						if (std::find(Chosen.begin(), Chosen.end(), node.Id) == Chosen.end()) {
							Chosen.push_back(node.Id);
						}
					}
				}
			}

			Drag = Dragging::None;
		}

		// --- painting ---------------------------------------------------------

		DrawLinks(graph, litLink);

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
			if (Visible(node)) {
				DrawNode(graph, node, LayoutOf(node, Look.Sizes));
			}
		}

		draw->PopClipRect();

		// --- keys and menus ---------------------------------------------------
		//
		// **Guarded on the window rather than on the canvas being hovered**, so a
		// shortcut still lands while the pointer is over the panel's own toolbar
		// — and never while somebody is typing into the palette's search box.
		const bool listening = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
							   !ImGui::IsAnyItemActive() && !PaletteOpen && !MenuOpen;

		if (listening) {
			const bool control = io.KeyCtrl || io.KeySuper;

			if (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace)) {
				Delete(graph);
				Edited = true;
			} else if (control && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_C)) {
				CompressSelection(graph);
				Edited = true;
			} else if (control && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_X)) {
				ExpandSelection(graph);
				Edited = true;
			} else if (control && ImGui::IsKeyPressed(ImGuiKey_C)) {
				Copy(graph);
			} else if (control && ImGui::IsKeyPressed(ImGuiKey_V)) {
				Paste(graph);
				Edited = true;
			} else if (control && ImGui::IsKeyPressed(ImGuiKey_D)) {
				Duplicate(graph);
				Edited = true;
			} else if (control && ImGui::IsKeyPressed(ImGuiKey_A)) {
				SelectAll(graph);
			} else if (control && ImGui::IsKeyPressed(ImGuiKey_G)) {
				if (io.KeyShift) {
					UngroupSelection(graph);
				} else {
					GroupSelection(graph);
				}
				Edited = true;
			} else if (ImGui::IsKeyPressed(ImGuiKey_Enter)) {
				// Into whichever of the selection has an inside.
				for (const NodeId id : Chosen) {
					const Node *node = graph.Find(id);
					if (node != nullptr && node->Compressed()) {
						Enter(graph, id);
						break;
					}
				}
			} else if (ImGui::IsKeyPressed(ImGuiKey_F)) {
				Fit(graph);
			} else if (ImGui::IsKeyPressed(ImGuiKey_Tab)) {
				PaletteNeeds = Needs::Anything;
				PaletteType.clear();
				PaletteLink = static_cast<size_t>(-1);
				PaletteSearch[0] = '\0';
				PaletteX = mouseX;
				PaletteY = mouseY;
				DragNode = NO_NODE;
				PaletteWanted = true;
			} else if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
				Chosen.clear();
				ChosenGroup = NO_GROUP;
			}
		}

		// **The context menu, and the palette only through it.** A right click
		// used to open the palette directly, which made "delete this link" and
		// "re-run this node" unreachable without a keyboard — and made adding a
		// node the only thing the right button could mean.
		if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
			PaletteX = mouseX;
			PaletteY = mouseY;
			DragNode = NO_NODE;

			MenuNode = HitNode(graph, mouseX, mouseY);
			MenuLink = MenuNode != NO_NODE ? static_cast<size_t>(-1) : HitLink(graph, mouseX, mouseY);
			MenuGroup = MenuNode != NO_NODE || MenuLink != static_cast<size_t>(-1)
							? NO_GROUP
							: HitGroup(graph, mouseX, mouseY);

			// Right-clicking outside the selection moves it, which is what makes
			// "Duplicate" mean the node under the pointer rather than whatever
			// was picked five minutes ago.
			if (MenuNode != NO_NODE && std::find(Chosen.begin(), Chosen.end(), MenuNode) == Chosen.end()) {
				Select(MenuNode);
			}
			if (MenuGroup != NO_GROUP) {
				Chosen.clear();
				ChosenGroup = MenuGroup;
			}

			MenuOpen = true;
			ImGui::OpenPopup("nodegraph.menu");
		}

		Menu(graph);
		Palette(graph);

		// **One report per gesture, at the end of it.** A slider drag is one
		// undo step; telling the host on every frame it moved would fill a
		// hundred-deep history with one drag.
		if (Edited && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
			Edited = false;
			if (Signals.Changed) {
				Signals.Changed();
			}
		}
	}
}
