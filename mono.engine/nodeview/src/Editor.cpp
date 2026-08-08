#include <engine/nodeview/Editor.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>

namespace engine::nodeview {

	namespace {
		using graph::NodeCatalogue;
		using graph::NodeKindSpec;
		using graph::PortDirection;
		using graph::PortRef;

		const NodeKindSpec *SpecOf(const EditorNode &node) {
			return NodeCatalogue::Find(node.Kind);
		}

		// How many rows one side has, from the kind rather than from the bound
		// list — the two agree, and the kind is the declaration.
		size_t RowsOf(const EditorNode &node, PortDirection direction) {
			const NodeKindSpec *spec = SpecOf(node);
			if (spec == nullptr) {
				return 0;
			}
			return direction == PortDirection::Input ? spec->Inputs.size() : spec->Outputs.size();
		}

		std::vector<core::Name> &SlotsOf(EditorNode &node, PortDirection direction) {
			return direction == PortDirection::Input ? node.Inputs : node.Outputs;
		}

		const std::vector<core::Name> &SlotsOf(const EditorNode &node, PortDirection direction) {
			return direction == PortDirection::Input ? node.Inputs : node.Outputs;
		}

		// What a slot accepts or produces, from the catalogue.
		bool SlotKind(const EditorNode &node, const PortRef &port, graph::ResourceKind &out) {
			const NodeKindSpec *spec = SpecOf(node);
			if (spec == nullptr) {
				return false;
			}
			const std::vector<graph::PortSpec> &side =
				port.Direction == PortDirection::Input ? spec->Inputs : spec->Outputs;
			if (port.Slot >= side.size()) {
				return false;
			}
			out = side[port.Slot].Kind;
			return true;
		}

		// Sizes a node's slot lists to what its kind declares, keeping whatever
		// was already bound.
		//
		// **Called wherever a node arrives from outside**, because a document
		// may name a kind whose port list has since changed — a saved pipeline
		// must open rather than crash on a slot index that no longer exists.
		void Fit(EditorNode &node) {
			node.Inputs.resize(RowsOf(node, PortDirection::Input));
			node.Outputs.resize(RowsOf(node, PortDirection::Output));
		}

		// The same subsequence rule `studio::FuzzyMatch` uses, restated for a
		// `shared` module. See `SearchCatalogue`.
		bool Fuzzy(std::string_view needle, std::string_view haystack, int &score) {
			score = 0;
			if (needle.empty()) {
				return true;
			}

			size_t at = 0;
			int run = 0;
			for (const char letter : haystack) {
				if (at >= needle.size()) {
					break;
				}
				const char wanted = static_cast<char>(std::tolower(needle[at]));
				if (static_cast<char>(std::tolower(letter)) == wanted) {
					// A run of adjacent matches scores above the same letters
					// scattered, so `tone` ranks `tonemap` over a kind that
					// merely contains those four letters somewhere.
					run++;
					score += run;
					at++;
				} else {
					run = 0;
				}
			}

			return at == needle.size();
		}
	}

	void ZoomAbout(CanvasView &view, Point at, float factor) {
		const float wanted =
			std::clamp(view.Zoom * factor, CanvasView::MINIMUM_ZOOM, CanvasView::MAXIMUM_ZOOM);

		// **Solved rather than accumulated.** The canvas point under `at` has to
		// be the same before and after, so the pan is whatever satisfies that at
		// the new zoom — computing it from a delta would drift by a pixel per
		// wheel notch and walk the canvas away over a session.
		const Point anchor = view.ToCanvas(at);
		view.Zoom = wanted;
		view.Pan = {at.X - anchor.X * wanted, at.Y - anchor.Y * wanted};
	}

	const EditorNode *EditorGraph::Find(core::Name name) const {
		for (const EditorNode &node : Nodes) {
			if (node.Name == name) {
				return &node;
			}
		}
		return nullptr;
	}

	EditorNode *EditorGraph::Find(core::Name name) {
		return const_cast<EditorNode *>(std::as_const(*this).Find(name));
	}

	graph::ResourceKind EditorGraph::KindOf(core::Name name) const {
		for (const graph::ResourceDesc &resource : Resources) {
			if (resource.Name == name) {
				return resource.Kind;
			}
		}
		return graph::ResourceKind::Texture;
	}

	float HeightOf(const EditorNode &node, const NodeStyle &style) {
		const auto rows = static_cast<float>(
			std::max(RowsOf(node, PortDirection::Input), RowsOf(node, PortDirection::Output))
		);
		const float ports = rows <= 0.0f ? 0.0f : style.PortTop + (rows - 1.0f) * style.PortPitch;
		return style.HeaderHeight + std::max(style.MinimumBody, ports + style.PortTop);
	}

	Point PortAt(const EditorNode &node, PortDirection direction, uint32_t slot, const NodeStyle &style) {
		const float down =
			node.At.Y + style.HeaderHeight + style.PortTop + static_cast<float>(slot) * style.PortPitch;
		const float across = direction == PortDirection::Input ? node.At.X - style.PortInset
															   : node.At.X + style.Width + style.PortInset;
		return {across, down};
	}

	std::vector<EditorLink> LinksOf(const EditorGraph &graph) {
		// Which node writes each resource, and from which slot. **The last
		// writer wins**, which is what `graph::Compile` already decides for a
		// frame: two passes writing one target is a chain, and the wire a reader
		// should show is the one that filled it.
		std::unordered_map<uint32_t, std::pair<core::Name, uint32_t>> writers;
		std::vector<EditorLink> links;

		// **Every writer first, so a wire drawn backwards is still drawn.**
		// Resolving as the walk goes would silently drop a wire from a node
		// that happens to sit later in the list — and building a pipeline in an
		// order other than the final one is the ordinary way anybody works. The
		// graph being unrunnable in that state is real and is
		// `GraphStatus::ReadsBeforeWrite`'s to report, after a compile, in
		// words; a wire that quietly fails to appear is not a diagnostic.
		for (const EditorNode &node : graph.Nodes) {
			for (uint32_t slot = 0; slot < node.Outputs.size(); slot++) {
				if (node.Outputs[slot].IsValid()) {
					writers[node.Outputs[slot].Id()] = {node.Name, slot};
				}
			}
		}

		for (const EditorNode &node : graph.Nodes) {
			for (uint32_t slot = 0; slot < node.Inputs.size(); slot++) {
				const core::Name bound = node.Inputs[slot];
				if (!bound.IsValid()) {
					continue;
				}
				const auto found = writers.find(bound.Id());
				if (found == writers.end()) {
					continue;
				}
				links.push_back(
					EditorLink{bound, found->second.first, found->second.second, node.Name, slot}
				);
			}
		}

		return links;
	}

	Hit HitTest(const EditorGraph &graph, const NodeStyle &style, Point at) {
		const float grab = style.PortRadius * style.PortGrab;

		// Ports first and nearest wins — see the header. Walked backwards so a
		// node drawn later takes the hit.
		Hit best;
		float closest = grab * grab;

		for (size_t index = graph.Nodes.size(); index-- > 0;) {
			const EditorNode &node = graph.Nodes[index];
			for (const PortDirection direction : {PortDirection::Input, PortDirection::Output}) {
				const size_t rows = RowsOf(node, direction);
				for (uint32_t slot = 0; slot < rows; slot++) {
					const Point dot = PortAt(node, direction, slot, style);
					const float dx = dot.X - at.X;
					const float dy = dot.Y - at.Y;
					const float distance = dx * dx + dy * dy;
					if (distance <= closest) {
						closest = distance;
						best = Hit{HitKind::Port, node.Name, PortRef{node.Name, direction, slot}};
					}
				}
			}
		}

		if (best.What != HitKind::None) {
			return best;
		}

		for (size_t index = graph.Nodes.size(); index-- > 0;) {
			const EditorNode &node = graph.Nodes[index];
			const float height = HeightOf(node, style);
			if (at.X < node.At.X || at.X >= node.At.X + style.Width || at.Y < node.At.Y ||
				at.Y >= node.At.Y + height) {
				continue;
			}
			const bool header = at.Y < node.At.Y + style.HeaderHeight;
			return Hit{header ? HitKind::Header : HitKind::Body, node.Name, {}};
		}

		return {};
	}

	DropVerdict EvaluateDrop(const EditorGraph &graph, const PortRef &from, const PortRef &to) {
		DropVerdict verdict;

		if (!from.IsValid() || !to.IsValid()) {
			return verdict;
		}

		if (from.Direction == to.Direction) {
			verdict.Why = from.Direction == PortDirection::Input
							  ? "both ends are inputs — a wire needs something to carry"
							  : "both ends are outputs — nothing would read it";
			return verdict;
		}

		if (from.Node == to.Node) {
			verdict.Why = "a pass cannot read what it is writing in the same frame";
			return verdict;
		}

		// Whichever end is the output is the one that names the resource.
		const PortRef &source = from.Direction == PortDirection::Output ? from : to;
		const PortRef &sink = from.Direction == PortDirection::Output ? to : from;

		const EditorNode *writer = graph.Find(source.Node);
		const EditorNode *reader = graph.Find(sink.Node);
		if (writer == nullptr || reader == nullptr) {
			return verdict;
		}

		graph::ResourceKind produces{};
		graph::ResourceKind accepts{};
		if (!SlotKind(*writer, source, produces) || !SlotKind(*reader, sink, accepts)) {
			return verdict;
		}

		if (!graph::PortsCompatible(produces, accepts)) {
			verdict.Why = graph::WhyIncompatible(produces, accepts);
			return verdict;
		}

		const std::vector<core::Name> &outputs = SlotsOf(*writer, PortDirection::Output);
		if (source.Slot >= outputs.size() || !outputs[source.Slot].IsValid()) {
			verdict.Why = "that output has no resource to offer";
			return verdict;
		}

		verdict.Allowed = true;
		verdict.Resource = outputs[source.Slot];
		return verdict;
	}

	bool Connect(EditorGraph &graph, const PortRef &from, const PortRef &to) {
		const DropVerdict verdict = EvaluateDrop(graph, from, to);
		if (!verdict.Allowed) {
			return false;
		}

		const PortRef &sink = from.Direction == PortDirection::Input ? from : to;
		EditorNode *reader = graph.Find(sink.Node);

		// **An input holds one resource, so this replaces.** Two writers into
		// one slot is not something a pass can express — it samples one texture
		// — and an editor that stacked them would be showing a wire that does
		// nothing.
		SlotsOf(*reader, PortDirection::Input)[sink.Slot] = verdict.Resource;
		return true;
	}

	bool Disconnect(EditorGraph &graph, const PortRef &port) {
		if (!port.IsValid() || port.Direction != PortDirection::Input) {
			return false;
		}

		EditorNode *node = graph.Find(port.Node);
		if (node == nullptr || port.Slot >= node->Inputs.size() || !node->Inputs[port.Slot].IsValid()) {
			return false;
		}

		node->Inputs[port.Slot] = core::Name{};
		return true;
	}

	core::Name AddNode(EditorGraph &graph, core::Name kind, Point at) {
		const NodeKindSpec *spec = NodeCatalogue::Find(kind);
		if (spec == nullptr) {
			return {};
		}

		// `opaque`, then `opaque 2`, then `opaque 3`. **A space and a number**
		// rather than a suffix that could be mistaken for part of a name.
		const auto taken = [&graph](core::Name candidate) { return graph.Find(candidate) != nullptr; };

		core::Name name(kind.Text());
		for (int suffix = 2; taken(name); suffix++) {
			name = core::Name(std::string(kind.Text()) + " " + std::to_string(suffix));
		}

		EditorNode node;
		node.Name = name;
		node.Kind = kind;
		node.At = at;
		Fit(node);

		// One resource per output, named after the node's slot so two nodes of
		// one kind do not collide.
		for (uint32_t slot = 0; slot < spec->Outputs.size(); slot++) {
			const std::string spelling =
				std::string(name.Text()) + "." + std::string(spec->Outputs[slot].Name.Text());
			const core::Name resource(spelling);

			graph::ResourceDesc declared;
			declared.Name = resource;
			declared.Kind = spec->Outputs[slot].Kind;
			graph.Resources.push_back(declared);

			node.Outputs[slot] = resource;
		}

		graph.Nodes.push_back(std::move(node));
		return name;
	}

	bool RemoveNode(EditorGraph &graph, core::Name name) {
		const auto found =
			std::find_if(graph.Nodes.begin(), graph.Nodes.end(), [name](const EditorNode &node) {
				return node.Name == name;
			});
		if (found == graph.Nodes.end()) {
			return false;
		}

		// What it wrote, so every reader of it can be unbound. See the header.
		const std::vector<core::Name> orphaned = found->Outputs;
		graph.Nodes.erase(found);

		for (EditorNode &node : graph.Nodes) {
			for (core::Name &bound : node.Inputs) {
				if (std::find(orphaned.begin(), orphaned.end(), bound) != orphaned.end()) {
					bound = core::Name{};
				}
			}
		}

		std::erase_if(graph.Resources, [&orphaned](const graph::ResourceDesc &resource) {
			return std::find(orphaned.begin(), orphaned.end(), resource.Name) != orphaned.end();
		});

		return true;
	}

	std::vector<CatalogueMatch> SearchCatalogue(std::string_view query) {
		std::vector<CatalogueMatch> matches;

		for (const NodeKindSpec &spec : NodeCatalogue::All()) {
			// The label as well as the kind, because a menu shows the label and
			// somebody typing "tone" is typing what they can see.
			int byKind = 0;
			int byLabel = 0;
			const bool kind = Fuzzy(query, spec.Kind.Text(), byKind);
			const bool label = !spec.Label.empty() && Fuzzy(query, spec.Label, byLabel);
			if (!kind && !label) {
				continue;
			}
			matches.push_back(CatalogueMatch{&spec, std::max(byKind, byLabel)});
		}

		// **Stable on the kind's name after the score**, so a query that ties
		// two kinds lists them the same way twice.
		std::stable_sort(
			matches.begin(), matches.end(), [](const CatalogueMatch &a, const CatalogueMatch &b) {
				if (a.Score != b.Score) {
					return a.Score > b.Score;
				}
				return a.Spec->Kind.Text() < b.Spec->Kind.Text();
			}
		);

		return matches;
	}

	graph::PipelineDocument ToDocument(const EditorGraph &graph) {
		graph::PipelineDocument document;

		for (const graph::ResourceDesc &resource : graph.Resources) {
			graph::Edit edit;
			edit.Kind = graph::EditKind::AddResource;
			edit.Name = resource.Name;
			edit.Resource = resource.Kind;
			edit.Width = resource.Width;
			edit.Height = resource.Height;
			document.Record(edit);
		}

		for (const EditorNode &node : graph.Nodes) {
			const NodeKindSpec *spec = NodeCatalogue::Find(node.Kind);

			graph::Edit added;
			added.Kind = graph::EditKind::AddNode;
			added.Name = node.Name;
			added.NodeKind = node.Kind;
			added.Scope = spec != nullptr ? spec->Scope : graph::NodeScope::View;
			document.Record(added);

			// **Reads then writes, in slot order**, which is what makes
			// `FromDocument` able to put them back in the right rows: the
			// document has no slot index, and position in the list is the index.
			for (const core::Name bound : node.Inputs) {
				graph::Edit reads;
				reads.Kind = graph::EditKind::Reads;
				reads.Target = bound;
				document.Record(reads);
			}
			for (const core::Name bound : node.Outputs) {
				graph::Edit writes;
				writes.Kind = graph::EditKind::Writes;
				writes.Target = bound;
				document.Record(writes);
			}

			if (!node.Enabled) {
				graph::Edit enabled;
				enabled.Kind = graph::EditKind::Enable;
				enabled.Name = node.Name;
				enabled.Enabled = false;
				document.Record(enabled);
			}

			graph::Edit moved;
			moved.Kind = graph::EditKind::Move;
			moved.Name = node.Name;
			moved.X = node.At.X;
			moved.Y = node.At.Y;
			document.Record(moved);
		}

		return document;
	}

	EditorGraph FromDocument(const graph::PipelineDocument &document, const NodeStyle &style) {
		EditorGraph built;
		const std::unordered_map<uint32_t, std::pair<float, float>> placed = graph::PositionsOf(document);

		EditorNode pending;
		bool building = false;
		std::vector<core::Name> reads;
		std::vector<core::Name> writes;

		const auto flush = [&]() {
			if (!building) {
				return;
			}
			Fit(pending);
			for (size_t slot = 0; slot < pending.Inputs.size() && slot < reads.size(); slot++) {
				pending.Inputs[slot] = reads[slot];
			}
			for (size_t slot = 0; slot < pending.Outputs.size() && slot < writes.size(); slot++) {
				pending.Outputs[slot] = writes[slot];
			}
			built.Nodes.push_back(std::move(pending));
			pending = EditorNode{};
			reads.clear();
			writes.clear();
			building = false;
		};

		for (const graph::Edit &edit : document.Edits()) {
			switch (edit.Kind) {
			case graph::EditKind::AddResource: {
				flush();
				graph::ResourceDesc resource;
				resource.Name = edit.Name;
				resource.Kind = edit.Resource;
				resource.Width = edit.Width;
				resource.Height = edit.Height;
				built.Resources.push_back(resource);
				break;
			}
			case graph::EditKind::AddNode:
				flush();
				pending.Name = edit.Name;
				pending.Kind = edit.NodeKind;
				building = true;
				break;
			case graph::EditKind::Reads:
				reads.push_back(edit.Target);
				break;
			case graph::EditKind::Writes:
				writes.push_back(edit.Target);
				break;
			case graph::EditKind::Enable:
				flush();
				if (EditorNode *node = built.Find(edit.Name)) {
					node->Enabled = edit.Enabled;
				}
				break;
			case graph::EditKind::Move:
				// Applied below, so a `move` may name a node the document
				// declares later — which is what a hand-edited file looks like.
				break;
			}
		}
		flush();

		// The positions, and an arrangement for whatever has none.
		float unplaced = 0.0f;
		for (EditorNode &node : built.Nodes) {
			const auto found = placed.find(node.Name.Id());
			if (found != placed.end()) {
				node.At = {found->second.first, found->second.second};
				continue;
			}
			node.At = {unplaced, 0.0f};
			unplaced += style.Width + style.PortPitch * 4.0f;
		}

		return built;
	}
}
