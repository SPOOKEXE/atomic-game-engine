// The model: nodes, links, frames, compression and the content hash.
//
// **No ImGui anywhere in this file**, which is what lets the suite check the
// half that fails silently (the cycle guard, the hash and the fold) with no
// window. `nodegraph/Graph.hpp` carries the arguments; what is here is the
// mechanism.

#include "Internal.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <nodegraph/Graph.hpp>
#include <nodegraph/Layout.hpp>
#include <string>
#include <unordered_set>

namespace nodegraph {

	namespace {
		const PortSpec *FindPort(const std::vector<PortSpec> &ports, const std::string &name) {
			const auto found = std::find_if(ports.begin(), ports.end(), [&](const PortSpec &port) {
				return port.Name == name;
			});
			return found == ports.end() ? nullptr : &*found;
		}
	}

	const char *Describe(LinkResult result) {
		switch (result) {
		case LinkResult::Made:
			return "connected";
		case LinkResult::NoSuchPort:
			return "no such port";
		case LinkResult::TypeMismatch:
			return "those types do not connect";
		case LinkResult::WouldCycle:
			return "that would make a loop";
		case LinkResult::SameNode:
			return "a node cannot feed itself";
		}
		return "refused";
	}

	NodeId Graph::Add(const std::string &type, float x, float y) {
		const NodeType *declared = NodeTypes::Find(type);
		if (declared == nullptr) {
			return NO_NODE;
		}

		Node node;
		node.Id = Next++;
		node.Type = type;
		node.X = x;
		node.Y = y;

		// **Defaults are copied in at creation rather than read through at
		// use.** A node's values are what a person set them to and are saved as
		// such; reading through to the type would make an edit to a type change
		// every existing graph the next time it was opened.
		for (const WidgetSpec &widget : declared->Widgets) {
			node.Widgets.emplace(widget.Key, widget.Default);
		}

		Stored.push_back(std::move(node));
		return Stored.back().Id;
	}

	bool Graph::Remove(NodeId id) {
		{
			const auto found =
				std::find_if(Stored.begin(), Stored.end(), [&](const Node &node) { return node.Id == id; });
			if (found == Stored.end()) {
				return false;
			}
		}

		// **A compressed node takes its contents with it.** They are only
		// reachable through it, so leaving them behind would be a set of nodes
		// no view draws and nothing can select, which is a leak that looks like a
		// graph that got smaller. `Expand` is the way to keep them, and it
		// re-parents them before it calls this.
		//
		// Collected first, because removing rewrites the vector this walks.
		for (const NodeId child : Contents(id)) {
			Remove(child);
		}

		const auto found =
			std::find_if(Stored.begin(), Stored.end(), [&](const Node &node) { return node.Id == id; });
		if (found == Stored.end()) {
			return false;
		}

		// The links first. A node removed with its wires left behind is a link
		// whose endpoint does not exist, which every later walk has to guard.
		Wires.erase(
			std::remove_if(
				Wires.begin(), Wires.end(), [&](const Link &link) { return link.From == id || link.To == id; }
			),
			Wires.end()
		);

		// And out of whatever frame held it, for the same reason: a member id
		// that names nothing is a bound computed from a null.
		for (nodegraph::Group &frame : Frames) {
			frame.Members.erase(
				std::remove(frame.Members.begin(), frame.Members.end(), id), frame.Members.end()
			);
		}
		Frames.erase(
			std::remove_if(
				Frames.begin(),
				Frames.end(),
				[](const nodegraph::Group &frame) { return frame.Members.empty(); }
			),
			Frames.end()
		);

		Stored.erase(found);
		return true;
	}

	bool Graph::Alive(NodeId id) const {
		return Find(id) != nullptr;
	}

	Node *Graph::Find(NodeId id) {
		const auto found =
			std::find_if(Stored.begin(), Stored.end(), [&](const Node &node) { return node.Id == id; });
		return found == Stored.end() ? nullptr : &*found;
	}

	const Node *Graph::Find(NodeId id) const {
		return const_cast<Graph *>(this)->Find(id);
	}

	LinkResult
	Graph::CanConnect(NodeId from, const std::string &fromPort, NodeId to, const std::string &toPort) const {
		if (from == to) {
			return LinkResult::SameNode;
		}

		const Node *source = Find(from);
		const Node *sink = Find(to);
		if (source == nullptr || sink == nullptr) {
			return LinkResult::NoSuchPort;
		}

		const NodeType *sourceType = NodeTypes::Find(source->Type);
		const NodeType *sinkType = NodeTypes::Find(sink->Type);
		if (sourceType == nullptr || sinkType == nullptr) {
			return LinkResult::NoSuchPort;
		}

		const PortSpec *out = FindPort(sourceType->Outputs, fromPort);
		const PortSpec *in = FindPort(sinkType->Inputs, toPort);
		if (out == nullptr || in == nullptr) {
			return LinkResult::NoSuchPort;
		}

		if (!DataTypes::CanConnect(out->Type, in->Type)) {
			return LinkResult::TypeMismatch;
		}

		// **Asked before the edge exists**, so the guard never has to undo one.
		// `Reaches` walks forward from the sink: if the source is already
		// downstream of it, this edge would close the loop.
		if (Reaches(to, from)) {
			return LinkResult::WouldCycle;
		}
		return LinkResult::Made;
	}

	LinkResult
	Graph::Connect(NodeId from, const std::string &fromPort, NodeId to, const std::string &toPort) {
		const LinkResult can = CanConnect(from, fromPort, to, toPort);
		if (can != LinkResult::Made) {
			return can;
		}

		// One link into an input, newest wins. `Graph.hpp` carries why.
		Disconnect(to, toPort);
		Wires.push_back(Link{from, fromPort, to, toPort});
		return LinkResult::Made;
	}

	bool Graph::Disconnect(NodeId to, const std::string &toPort) {
		const size_t before = Wires.size();
		Wires.erase(
			std::remove_if(
				Wires.begin(),
				Wires.end(),
				[&](const Link &link) { return link.To == to && link.ToPort == toPort; }
			),
			Wires.end()
		);
		return Wires.size() != before;
	}

	const Link *Graph::LinkInto(NodeId node, const std::string &port) const {
		const auto found = std::find_if(Wires.begin(), Wires.end(), [&](const Link &link) {
			return link.To == node && link.ToPort == port;
		});
		return found == Wires.end() ? nullptr : &*found;
	}

	std::vector<Link> Graph::LinksOf(NodeId node) const {
		std::vector<Link> found;
		for (const Link &link : Wires) {
			if (link.From == node || link.To == node) {
				found.push_back(link);
			}
		}
		return found;
	}

	GroupId Graph::Group(std::vector<NodeId> members, std::string title, Colour tint) {
		// Only nodes that exist, and each at most once. A frame naming something
		// that is not there is a drag that moves nothing and a bound that reads
		// a null.
		std::vector<NodeId> kept;
		for (const NodeId id : members) {
			if (Alive(id) && std::find(kept.begin(), kept.end(), id) == kept.end()) {
				kept.push_back(id);
			}
		}
		if (kept.empty()) {
			return NO_GROUP;
		}

		// **A node belongs to one frame**, so a member joining this one leaves
		// whichever it was in. Two frames owning one node is two drags moving it
		// twice, which is a node that outruns the pointer.
		for (nodegraph::Group &frame : Frames) {
			frame.Members.erase(
				std::remove_if(
					frame.Members.begin(),
					frame.Members.end(),
					[&](NodeId id) { return std::find(kept.begin(), kept.end(), id) != kept.end(); }
				),
				frame.Members.end()
			);
		}
		Frames.erase(
			std::remove_if(
				Frames.begin(),
				Frames.end(),
				[](const nodegraph::Group &frame) { return frame.Members.empty(); }
			),
			Frames.end()
		);

		nodegraph::Group made;
		made.Id = NextGroup++;
		made.Title = std::move(title);
		made.Tint = tint;
		made.Members = std::move(kept);
		Frames.push_back(std::move(made));
		return Frames.back().Id;
	}

	bool Graph::Ungroup(GroupId group, bool withNodes) {
		const auto found = std::find_if(Frames.begin(), Frames.end(), [&](const nodegraph::Group &frame) {
			return frame.Id == group;
		});
		if (found == Frames.end()) {
			return false;
		}

		// **Copied before the frame is erased.** `Remove` walks the frames to
		// keep their membership honest, so deleting through a reference into the
		// vector it is about to rewrite is exactly the aliasing bug that reads
		// as "one node survived the delete".
		const std::vector<NodeId> members = found->Members;
		Frames.erase(found);

		if (withNodes) {
			for (const NodeId id : members) {
				Remove(id);
			}
		}
		return true;
	}

	nodegraph::Group *Graph::FindGroup(GroupId group) {
		const auto found = std::find_if(Frames.begin(), Frames.end(), [&](const nodegraph::Group &frame) {
			return frame.Id == group;
		});
		return found == Frames.end() ? nullptr : &*found;
	}

	const nodegraph::Group *Graph::FindGroup(GroupId group) const {
		return const_cast<Graph *>(this)->FindGroup(group);
	}

	void EnsureCustomType() {
		if (NodeTypes::Find(CUSTOM_TYPE) != nullptr) {
			return;
		}
		NodeType custom;
		custom.Id = CUSTOM_TYPE;
		custom.Title = "Custom Node";
		custom.Category = "Custom";
		custom.Accent = Colour::Hex(0xA78BFA);
		custom.Subtitle = "folded from a selection";
		custom.Width = 240.0f;
		custom.Hidden = true;
		NodeTypes::Register(custom);
	}

	namespace {
		// A name nothing in `taken` already uses, by adding a number.
		std::string Unique(std::string wanted, const std::vector<PortSpec> &taken) {
			const auto used = [&taken](const std::string &name) {
				return std::any_of(taken.begin(), taken.end(), [&name](const PortSpec &port) {
					return port.Name == name;
				});
			};
			if (!used(wanted)) {
				return wanted;
			}
			for (int suffix = 2; suffix < 1000; suffix++) {
				std::string tried = wanted + " " + std::to_string(suffix);
				if (!used(tried)) {
					return tried;
				}
			}
			return wanted;
		}

		// What to call a node in a derived port's name.
		std::string ShortName(const Node &node) {
			if (!node.Label.empty()) {
				return node.Label;
			}
			const NodeType *type = NodeTypes::Find(node.Type);
			return type != nullptr ? type->Title : node.Type;
		}
	}

	uint64_t MixValue(uint64_t hash, const Value &value) {
		hash = Mix(hash, &value.Kind, sizeof(value.Kind));
		switch (value.Kind) {
		case WidgetKind::Toggle:
			return Mix(hash, &value.Flag, sizeof(value.Flag));
		case WidgetKind::Text:
		case WidgetKind::Select:
			return MixText(hash, value.Text);
		case WidgetKind::Colour:
			return Mix(hash, &value.Tint, sizeof(value.Tint));
		case WidgetKind::Slider:
		case WidgetKind::Number:
			return Mix(hash, &value.Number, sizeof(value.Number));
		}
		return hash;
	}

	GroupId Graph::GroupOf(NodeId node) const {
		for (const nodegraph::Group &frame : Frames) {
			if (std::find(frame.Members.begin(), frame.Members.end(), node) != frame.Members.end()) {
				return frame.Id;
			}
		}
		return NO_GROUP;
	}

	bool Graph::Reaches(NodeId from, NodeId to) const {
		if (from == to) {
			return true;
		}

		std::vector<NodeId> pending{from};
		std::unordered_set<NodeId> seen{from};

		while (!pending.empty()) {
			const NodeId current = pending.back();
			pending.pop_back();

			for (const Link &link : Wires) {
				if (link.From != current) {
					continue;
				}
				if (link.To == to) {
					return true;
				}
				if (seen.insert(link.To).second) {
					pending.push_back(link.To);
				}
			}
		}
		return false;
	}

	std::vector<NodeId> Graph::Ordered() const {
		std::unordered_map<NodeId, size_t> waiting;
		waiting.reserve(Stored.size());
		for (const Node &node : Stored) {
			waiting.emplace(node.Id, 0);
		}
		for (const Link &link : Wires) {
			const auto found = waiting.find(link.To);
			if (found != waiting.end()) {
				found->second++;
			}
		}

		// **Seeded in placement order rather than from the map**, so two runs
		// over one graph produce one order. A hash map's order is not a
		// promise, and an evaluation order that changed between runs would make
		// a profile unreadable and a recording unreplayable.
		std::vector<NodeId> ready;
		for (const Node &node : Stored) {
			if (waiting[node.Id] == 0) {
				ready.push_back(node.Id);
			}
		}

		std::vector<NodeId> order;
		order.reserve(Stored.size());

		while (!ready.empty()) {
			const NodeId current = ready.front();
			ready.erase(ready.begin());
			order.push_back(current);

			for (const Link &link : Wires) {
				if (link.From != current) {
					continue;
				}
				const auto found = waiting.find(link.To);
				if (found != waiting.end() && found->second > 0 && --found->second == 0) {
					ready.push_back(link.To);
				}
			}
		}

		// A shorter answer than the node count is a cycle, which `Connect`
		// refuses and a hand-edited file can still contain. The prefix is what
		// can be evaluated; the rest is left out rather than guessed at.
		return order;
	}

	std::vector<NodeId> Graph::Contents(NodeId id) const {
		std::vector<NodeId> inside;
		for (const Node &node : Stored) {
			if (node.Owner == id) {
				inside.push_back(node.Id);
			}
		}
		return inside;
	}

	NodeId Graph::Compress(const std::vector<NodeId> &members, float x, float y) {
		EnsureCustomType();

		// Alive, distinct, and all at one depth. Folding two nodes from
		// different views would make a node whose contents are somewhere else.
		std::vector<NodeId> kept;
		NodeId depth = NO_NODE;
		for (const NodeId id : members) {
			const Node *node = Find(id);
			if (node == nullptr || std::find(kept.begin(), kept.end(), id) != kept.end()) {
				continue;
			}
			if (kept.empty()) {
				depth = node->Owner;
			} else if (node->Owner != depth) {
				return NO_NODE;
			}
			kept.push_back(id);
		}
		if (kept.size() < 2) {
			return NO_NODE;
		}

		const auto inside = [&kept](NodeId id) {
			return std::find(kept.begin(), kept.end(), id) != kept.end();
		};

		const NodeId made = Add(CUSTOM_TYPE, x, y);
		if (made == NO_NODE) {
			return NO_NODE;
		}

		std::vector<Proxy> proxies;
		std::vector<PortSpec> names;

		// **Every distinct inbound target port becomes an input, every distinct
		// outbound source port becomes an output.** Type and label are inherited
		// from the inner port, so typing survives compression, which is the whole
		// reason the derived interface is derived from the wiring rather than
		// declared.
		// **Both ends are resolved to where they stand at this depth first.** A
		// link into a node that is itself inside a fold in the selection arrives
		// at that fold's proxy port, not at the leaf, so a compressed node made of
		// compressed nodes derives an interface that names things this view can
		// actually see.
		const auto derive = [&](bool inputs) {
			for (const Link &link : Wires) {
				NodeId fromNode = NO_NODE;
				NodeId toNode = NO_NODE;
				std::string fromPort;
				std::string toPort;

				if (!Standing(*this, link.From, link.FromPort, false, depth, fromNode, fromPort) ||
					!Standing(*this, link.To, link.ToPort, true, depth, toNode, toPort)) {
					continue;
				}

				const bool fromInside = inside(fromNode);
				const bool toInside = inside(toNode);
				if (inputs ? !(toInside && !fromInside) : !(fromInside && !toInside)) {
					continue;
				}

				const NodeId inner = inputs ? toNode : fromNode;
				const std::string &port = inputs ? toPort : fromPort;

				const bool already = std::any_of(proxies.begin(), proxies.end(), [&](const Proxy &had) {
					return had.Input == inputs && had.Inner == inner && had.InnerPort == port;
				});
				if (already) {
					continue;
				}

				const Node *node = Find(inner);
				if (node == nullptr) {
					continue;
				}

				// The type is taken from the node's real interface, so a proxy
				// of a proxy still carries what the wire carries.
				std::string carried;
				for (const PortSpec &spec : inputs ? InputsOf(*node) : OutputsOf(*node)) {
					if (spec.Name == port) {
						carried = spec.Type;
						break;
					}
				}

				Proxy proxy;
				proxy.Name = Unique(ShortName(*node) + " " + port, names);
				proxy.Type = carried;
				proxy.Input = inputs;
				proxy.Inner = inner;
				proxy.InnerPort = port;
				names.push_back(PortSpec{proxy.Name, proxy.Type});
				proxies.push_back(std::move(proxy));
			}
		};

		derive(true);
		derive(false);

		// Every knob of everything inside, keeping its full schema so a slider
		// stays a slider. Exposed by default: hiding them all would make a
		// compressed node a black box on the first fold.
		std::vector<Promotion> promoted;
		for (const NodeId id : kept) {
			const Node *node = Find(id);
			if (node == nullptr) {
				continue;
			}
			// **The member's real interface**, so folding a fold promotes its
			// already-promoted knobs rather than nothing. `ActualWidget`
			// recurses, so a write still lands on the leaf.
			for (const WidgetSpec &widget : WidgetsOf(*node)) {
				Promotion one;
				one.Key = std::to_string(id) + "/" + widget.Key;
				one.Label = ShortName(*node) + " " + widget.Label;
				one.Inner = id;
				one.InnerKey = widget.Key;
				one.Spec = widget;
				one.Spec.Key = one.Key;
				one.Spec.Label = one.Label;
				promoted.push_back(std::move(one));
			}
		}

		Node *folded = Find(made);
		folded->Owner = depth;
		folded->Label = "Custom Node";
		folded->Proxies = std::move(proxies);
		folded->Promoted = std::move(promoted);

		for (const NodeId id : kept) {
			Find(id)->Owner = made;
		}

		// A frame around nodes that are now inside something is a rectangle
		// around nothing visible, so it goes with them.
		for (const NodeId id : kept) {
			if (const GroupId frame = GroupOf(id); frame != NO_GROUP) {
				Ungroup(frame, false);
			}
		}

		return made;
	}

	bool Graph::Expand(NodeId id) {
		Node *folded = Find(id);
		if (folded == nullptr || !folded->Compressed()) {
			return false;
		}

		// **The members come back to the compressed node's own depth**, not to
		// the root: expanding one inside another must leave its contents inside
		// that one.
		const NodeId depth = folded->Owner;
		for (Node &node : Stored) {
			if (node.Owner == id) {
				node.Owner = depth;
			}
		}

		// `Remove` deletes contents, and there are none left by now, which is the
		// whole reason this loop runs first.
		return Remove(id);
	}

	NodeId Graph::Adopt(const Node &node) {
		Stored.push_back(node);
		Next = std::max(Next, node.Id + 1);
		return node.Id;
	}

	void Graph::Attach(const Link &link) {
		Wires.push_back(link);
	}

	std::vector<NodeId> Graph::Absorb(const Graph &other, float dx, float dy, NodeId owner) {
		std::unordered_map<NodeId, NodeId> made;

		for (const Node &node : other.Nodes()) {
			const NodeId id = Add(node.Type, node.X + dx, node.Y + dy);
			if (id == NO_NODE) {
				continue;
			}
			Node *placed = Find(id);
			placed->Widgets = node.Widgets;
			placed->Label = node.Label;
			placed->Collapsed = node.Collapsed;
			placed->Proxies = node.Proxies;
			placed->Promoted = node.Promoted;
			made.emplace(node.Id, id);
		}

		// **A second pass, because a fold may be placed before its members.** A
		// proxy left naming the source's inner node would make the copy a second
		// view of the original rather than a copy of it.
		std::vector<NodeId> roots;
		for (const Node &node : other.Nodes()) {
			const auto self = made.find(node.Id);
			if (self == made.end()) {
				continue;
			}
			Node *placed = Find(self->second);

			const auto held = made.find(node.Owner);
			placed->Owner = held == made.end() ? owner : held->second;
			if (placed->Owner == owner) {
				roots.push_back(placed->Id);
			}

			for (Proxy &proxy : placed->Proxies) {
				const auto inner = made.find(proxy.Inner);
				proxy.Inner = inner == made.end() ? NO_NODE : inner->second;
			}
			for (Promotion &promotion : placed->Promoted) {
				const auto inner = made.find(promotion.Inner);
				promotion.Inner = inner == made.end() ? NO_NODE : inner->second;

				// Re-keyed to the copy's own member: the key is
				// `<inner id>/<inner key>` and is what a saved document names, so
				// leaving the source's id in it would be a reference to a node
				// this copy does not contain.
				promotion.Key = std::to_string(promotion.Inner) + "/" + promotion.InnerKey;
				promotion.Spec.Key = promotion.Key;
			}
		}

		// **Back through `Connect`.** What is being absorbed may be a hand-edited
		// document; a link that lies about a type, or that would close a loop
		// against what is already here, loses the link rather than the graph.
		for (const Link &link : other.Links()) {
			const auto from = made.find(link.From);
			const auto to = made.find(link.To);
			if (from != made.end() && to != made.end()) {
				(void)Connect(from->second, link.FromPort, to->second, link.ToPort);
			}
		}

		for (const nodegraph::Group &frame : other.Groups()) {
			std::vector<NodeId> members;
			for (const NodeId member : frame.Members) {
				if (const auto found = made.find(member); found != made.end()) {
					members.push_back(found->second);
				}
			}
			(void)Group(std::move(members), frame.Title, frame.Tint);
		}

		return roots;
	}

	void Graph::Remember(std::string name, std::string document) {
		const auto found = std::find_if(Library.begin(), Library.end(), [&](const Template &held) {
			return held.Name == name;
		});
		if (found != Library.end()) {
			found->Document = std::move(document);
			return;
		}
		Library.push_back(Template{std::move(name), std::move(document)});
	}

	bool Graph::Forget(const std::string &name) {
		const auto found = std::find_if(Library.begin(), Library.end(), [&](const Template &held) {
			return held.Name == name;
		});
		if (found == Library.end()) {
			return false;
		}
		Library.erase(found);
		return true;
	}

	uint64_t Graph::Hash(NodeId id) const {
		const Node *node = Find(id);
		if (node == nullptr) {
			return 0;
		}

		uint64_t hash = MixText(SEED, node->Type);

		// **Widgets in the type's declared order, not the map's.** A hash that
		// walked an unordered map would differ between two processes holding one
		// graph, which is a cache that misses on every machine but the one that
		// filled it.
		if (const NodeType *type = NodeTypes::Find(node->Type); type != nullptr) {
			for (const WidgetSpec &widget : type->Widgets) {
				hash = MixText(hash, widget.Key);
				const auto found = node->Widgets.find(widget.Key);
				hash = MixValue(hash, found == node->Widgets.end() ? widget.Default : found->second);
			}

			// The inputs, in port order, each contributing its own hash, which
			// is what makes an edit upstream invalidate exactly the sub-tree
			// below it.
			for (const PortSpec &port : type->Inputs) {
				hash = MixText(hash, port.Name);
				if (const Link *link = LinkInto(id, port.Name); link != nullptr) {
					const uint64_t upstream = Hash(link->From);
					hash = Mix(hash, &upstream, sizeof(upstream));
					hash = MixText(hash, link->FromPort);
				} else {
					hash = Mix(hash, "unconnected", 11);
				}
			}
		}

		return hash;
	}

	uint64_t Graph::Signature() const {
		uint64_t hash = SEED;
		for (const Node &node : Stored) {
			const uint64_t one = Hash(node.Id);
			hash = Mix(hash, &one, sizeof(one));
		}
		return hash;
	}

	void Graph::Clear() {
		Stored.clear();
		Wires.clear();
		Frames.clear();
		Library.clear();
		Next = 1;
		NextGroup = 1;
	}
}
