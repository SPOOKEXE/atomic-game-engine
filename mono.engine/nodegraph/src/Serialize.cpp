// The save format: three flat lists of pipe-separated fields, and a reader that
// keeps what it can rather than refusing a document over one bad line.

#include "Internal.hpp"

#include <engine/nodegraph/Layout.hpp>
#include <engine/nodegraph/Serialize.hpp>

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <sstream>
#include <string>

namespace engine::nodegraph {

	namespace {
		constexpr const char *HEADER = "nodegraph 1";

		std::string_view Trim(std::string_view text) {
			while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
				text.remove_prefix(1);
			}
			while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
				text.remove_suffix(1);
			}
			return text;
		}

		std::vector<std::string_view> Fields(std::string_view line) {
			std::vector<std::string_view> parts;
			size_t start = 0;
			while (start <= line.size()) {
				const size_t bar = std::min(line.find('|', start), line.size());
				parts.push_back(Trim(line.substr(start, bar - start)));
				start = bar + 1;
			}
			return parts;
		}

		double Real(std::string_view text, double fallback = 0.0) {
			// `from_chars` for doubles is not everywhere yet; a stream is the
			// portable spelling and this is not a hot path.
			std::string owned(text);
			try {
				return std::stod(owned);
			} catch (...) {
				return fallback;
			}
		}

		uint32_t Whole(std::string_view text) {
			uint32_t value = 0;
			std::from_chars(text.data(), text.data() + text.size(), value);
			return value;
		}

		const char *Spell(WidgetKind kind) {
			switch (kind) {
			case WidgetKind::Slider:
				return "slider";
			case WidgetKind::Number:
				return "number";
			case WidgetKind::Text:
				return "text";
			case WidgetKind::Toggle:
				return "toggle";
			case WidgetKind::Select:
				return "select";
			case WidgetKind::Colour:
				return "colour";
			}
			return "number";
		}

		WidgetKind KindOf(std::string_view text) {
			if (text == "slider") {
				return WidgetKind::Slider;
			}
			if (text == "text") {
				return WidgetKind::Text;
			}
			if (text == "toggle") {
				return WidgetKind::Toggle;
			}
			if (text == "select") {
				return WidgetKind::Select;
			}
			if (text == "colour") {
				return WidgetKind::Colour;
			}
			return WidgetKind::Number;
		}
	}

	std::string Save(const Graph &graph) {
		std::ostringstream out;
		out << HEADER << "\n";

		for (const Node &node : graph.Nodes()) {
			// **The collapsed flag is a seventh field and not a seventh line**,
			// because it is a property of the node rather than a thing the node
			// has — and a reader that stops at six fields still gets a whole
			// node, which is what makes adding one safe.
			out << "node | " << node.Id << " | " << node.Type << " | " << node.X << " | " << node.Y << " | "
				<< node.Label << " | " << (node.Collapsed ? "collapsed" : "open") << "\n";

			// **In the type's order where there is a type**, so two saves of one
			// graph produce one file — an unordered map's order is not a promise
			// and a diff full of moved lines is a diff nobody reads.
			if (const NodeType *type = NodeTypes::Find(node.Type); type != nullptr) {
				for (const WidgetSpec &widget : type->Widgets) {
					const auto found = node.Widgets.find(widget.Key);
					if (found == node.Widgets.end()) {
						continue;
					}
					const Value &value = found->second;
					out << "value | " << node.Id << " | " << widget.Key << " | " << Spell(value.Kind)
						<< " | ";
					switch (value.Kind) {
					case WidgetKind::Toggle:
						out << (value.Flag ? "on" : "off");
						break;
					case WidgetKind::Text:
					case WidgetKind::Select:
						out << value.Text;
						break;
					case WidgetKind::Colour:
						out << value.Tint.R << " " << value.Tint.G << " " << value.Tint.B;
						break;
					default:
						out << value.Number;
						break;
					}
					out << "\n";
				}
			}
		}

		for (const Link &link : graph.Links()) {
			out << "link | " << link.From << " | " << link.FromPort << " | " << link.To << " | "
				<< link.ToPort << "\n";
		}

		// A compressed node's depth and its derived interface. **The interface
		// is written out rather than recomputed on load**, because it is a
		// record of how the graph was wired at the moment it was folded — and
		// re-deriving it would silently rename a port the first time somebody
		// added a wire into the selection.
		for (const Node &node : graph.Nodes()) {
			if (node.Owner != NO_NODE) {
				out << "inside | " << node.Id << " | " << node.Owner << "\n";
			}
			for (const Proxy &proxy : node.Proxies) {
				out << "proxy | " << node.Id << " | " << (proxy.Input ? "in" : "out") << " | " << proxy.Name
					<< " | " << proxy.Type << " | " << proxy.Inner << " | " << proxy.InnerPort << "\n";
			}
			for (const Promotion &promotion : node.Promoted) {
				// The schema is not written: it is re-derived from the inner
				// node's type on load, which is the only copy that cannot drift
				// from the declaration.
				out << "promote | " << node.Id << " | " << promotion.Inner << " | " << promotion.InnerKey
					<< " | " << promotion.Label << " | " << (promotion.Exposed ? "shown" : "hidden") << "\n";
			}
		}

		// A frame is its title, its colour and the ids it holds. Written last,
		// so a reader has already placed every node one could name.
		for (const Group &frame : graph.Groups()) {
			out << "group | " << frame.Id << " | " << frame.Title << " | " << frame.Tint.R << " "
				<< frame.Tint.G << " " << frame.Tint.B << " |";
			for (const NodeId member : frame.Members) {
				out << " " << member;
			}
			out << "\n";
		}

		// The library, each entry as its own document between markers.
		//
		// **Line-oriented, so no escaping is needed.** A template is a whole
		// document with newlines and bars in it; quoting one into a field would
		// need an escape this format does not have, and a marker pair needs
		// none.
		for (const Graph::Template &held : graph.Templates()) {
			out << "template | " << held.Name << "\n";
			out << held.Document;
			if (!held.Document.empty() && held.Document.back() != '\n') {
				out << "\n";
			}
			out << "endtemplate\n";
		}

		return out.str();
	}

	std::string SaveSubtree(const Graph &graph, NodeId root) {
		// **Rebuilt into a scratch graph and written by `Save`.** Writing a
		// subtree by hand would be a second copy of the format, and a second
		// copy of the format is where a field stops being written.
		Graph held;

		std::vector<NodeId> taken;
		const auto take = [&](auto &&self, NodeId id) -> void {
			const Node *node = graph.Find(id);
			if (node == nullptr) {
				return;
			}
			taken.push_back(id);
			held.Adopt(*node);
			for (const NodeId child : graph.Contents(id)) {
				self(self, child);
			}
		};
		take(take, root);

		for (const Link &link : graph.Links()) {
			const bool from = std::find(taken.begin(), taken.end(), link.From) != taken.end();
			const bool to = std::find(taken.begin(), taken.end(), link.To) != taken.end();
			if (from && to) {
				held.Attach(link);
			}
		}

		// The root comes out at the origin, so placing it later is an offset
		// rather than a subtraction somebody has to remember.
		if (const Node *node = graph.Find(root); node != nullptr) {
			for (Node &moved : held.Nodes()) {
				moved.X -= node->X;
				moved.Y -= node->Y;
			}
		}
		if (Node *placed = held.Find(root); placed != nullptr) {
			placed->Owner = NO_NODE;
		}

		return Save(held);
	}

	bool Load(std::string_view text, Graph &graph, std::string &error) {
		if (Trim(text.substr(0, text.find('\n'))) != HEADER) {
			error = "not a nodegraph document";
			return false;
		}

		graph.Clear();

		// **Registered before anything is placed**, so a document containing a
		// compressed node opens in a process that has not folded one this run.
		EnsureCustomType();

		// The file's ids are not the graph's: `Add` hands out its own, so a
		// document with any numbering loads. This maps one to the other.
		std::unordered_map<uint32_t, NodeId> placed;
		std::vector<Link> pending;
		std::vector<Group> groups;

		// Held until every node has been placed, for the same reason the links
		// are: these all name node ids, and the file's ids are not the graph's.
		//@{
		std::vector<std::pair<uint32_t, uint32_t>> nesting;

		struct Held {
			uint32_t Owner = 0;
			Proxy Port;
		};
		std::vector<Held> proxies;

		struct Lifted {
			uint32_t Owner = 0;
			Promotion Knob;
		};
		std::vector<Lifted> promotions;
		//@}

		size_t start = text.find('\n');
		start = start == std::string_view::npos ? text.size() : start + 1;

		// The template currently being gathered, if any.
		std::string gathering;
		std::string held;

		while (start < text.size()) {
			const size_t end = std::min(text.find('\n', start), text.size());
			const std::string_view line = Trim(text.substr(start, end - start));
			start = end + 1;

			// **Inside a template block every line is taken verbatim**, because
			// what is between the markers is another whole document — including
			// its own header, its own nodes and, in principle, its own markers,
			// which is why the end marker is a line of its own rather than a
			// field.
			if (!gathering.empty()) {
				if (line == "endtemplate") {
					graph.Remember(std::move(gathering), std::move(held));
					gathering.clear();
					held.clear();
					continue;
				}
				held.append(line);
				held.push_back('\n');
				continue;
			}

			if (line.empty() || line.front() == '#') {
				continue;
			}

			const std::vector<std::string_view> fields = Fields(line);
			if (fields.empty()) {
				continue;
			}

			if (fields[0] == "node" && fields.size() >= 5) {
				const NodeId id = graph.Add(
					std::string(fields[2]),
					static_cast<float>(Real(fields[3])),
					static_cast<float>(Real(fields[4]))
				);
				if (id == NO_NODE) {
					// An unregistered type. Placed anyway, so nothing is lost —
					// `Serialize.hpp` carries why — which needs the node to
					// exist without `Add`'s type lookup.
					Node broken;
					broken.Type = std::string(fields[2]);
					broken.X = static_cast<float>(Real(fields[3]));
					broken.Y = static_cast<float>(Real(fields[4]));
					broken.Id = static_cast<NodeId>(graph.Nodes().size() + 1000000);
					if (fields.size() >= 6) {
						broken.Label = std::string(fields[5]);
					}
					if (fields.size() >= 7) {
						broken.Collapsed = fields[6] == "collapsed";
					}
					placed.emplace(Whole(fields[1]), broken.Id);
					graph.Nodes().push_back(std::move(broken));
					continue;
				}

				placed.emplace(Whole(fields[1]), id);
				if (fields.size() >= 6 && !fields[5].empty()) {
					graph.Find(id)->Label = std::string(fields[5]);
				}
				if (fields.size() >= 7) {
					graph.Find(id)->Collapsed = fields[6] == "collapsed";
				}
				continue;
			}

			if (fields[0] == "value" && fields.size() >= 5) {
				const auto found = placed.find(Whole(fields[1]));
				if (found == placed.end()) {
					continue;
				}
				Node *node = graph.Find(found->second);
				if (node == nullptr) {
					continue;
				}

				Value value;
				value.Kind = KindOf(fields[3]);
				switch (value.Kind) {
				case WidgetKind::Toggle:
					value.Flag = fields[4] == "on";
					break;
				case WidgetKind::Text:
				case WidgetKind::Select:
					value.Text = std::string(fields[4]);
					break;
				case WidgetKind::Colour: {
					std::istringstream channels{std::string(fields[4])};
					channels >> value.Tint.R >> value.Tint.G >> value.Tint.B;
					break;
				}
				default:
					value.Number = Real(fields[4]);
					break;
				}
				node->Widgets[std::string(fields[2])] = std::move(value);
				continue;
			}

			if (fields[0] == "link" && fields.size() >= 5) {
				// **Held until every node is placed.** A file is written nodes
				// first, but a hand-edited one need not be, and a link read
				// before its endpoints would be dropped for a reason that is not
				// its fault.
				pending.push_back(
					Link{
						Whole(fields[1]),
						std::string(fields[2]),
						Whole(fields[3]),
						std::string(fields[4]),
					}
				);
				continue;
			}

			if (fields[0] == "template" && fields.size() >= 2 && !fields[1].empty()) {
				gathering = std::string(fields[1]);
				held.clear();
				continue;
			}

			if (fields[0] == "inside" && fields.size() >= 3) {
				nesting.emplace_back(Whole(fields[1]), Whole(fields[2]));
				continue;
			}

			if (fields[0] == "proxy" && fields.size() >= 7) {
				Held one;
				one.Owner = Whole(fields[1]);
				one.Port.Input = fields[2] == "in";
				one.Port.Name = std::string(fields[3]);
				one.Port.Type = std::string(fields[4]);
				one.Port.Inner = Whole(fields[5]);
				one.Port.InnerPort = std::string(fields[6]);
				proxies.push_back(std::move(one));
				continue;
			}

			if (fields[0] == "promote" && fields.size() >= 6) {
				Lifted one;
				one.Owner = Whole(fields[1]);
				one.Knob.Inner = Whole(fields[2]);
				one.Knob.InnerKey = std::string(fields[3]);
				one.Knob.Label = std::string(fields[4]);
				one.Knob.Exposed = fields[5] != "hidden";
				promotions.push_back(std::move(one));
				continue;
			}

			if (fields[0] == "group" && fields.size() >= 5) {
				Group frame;
				frame.Title = std::string(fields[2]);
				{
					std::istringstream channels{std::string(fields[3])};
					channels >> frame.Tint.R >> frame.Tint.G >> frame.Tint.B;
				}

				std::istringstream members{std::string(fields[4])};
				uint32_t member = 0;
				while (members >> member) {
					frame.Members.push_back(member);
				}
				groups.push_back(std::move(frame));
			}
		}

		for (const Link &link : pending) {
			const auto from = placed.find(link.From);
			const auto to = placed.find(link.To);
			if (from == placed.end() || to == placed.end()) {
				continue;
			}
			(void)graph.Connect(from->second, link.FromPort, to->second, link.ToPort);
		}

		const auto real = [&placed](uint32_t id) {
			const auto found = placed.find(id);
			return found == placed.end() ? NO_NODE : found->second;
		};

		for (const auto &[child, owner] : nesting) {
			Node *node = graph.Find(real(child));
			if (node != nullptr) {
				// An owner that did not load leaves the node at the root rather
				// than hidden inside something that is not there.
				node->Owner = real(owner);
			}
		}

		for (const Held &held : proxies) {
			Node *node = graph.Find(real(held.Owner));
			const NodeId inner = real(held.Port.Inner);
			if (node == nullptr || inner == NO_NODE) {
				continue;
			}
			Proxy proxy = held.Port;
			proxy.Inner = inner;
			node->Proxies.push_back(std::move(proxy));
		}

		for (const Lifted &lifted : promotions) {
			Node *node = graph.Find(real(lifted.Owner));
			const NodeId inner = real(lifted.Knob.Inner);
			const Node *source = graph.Find(inner);
			const NodeType *type = source == nullptr ? nullptr : NodeTypes::Find(source->Type);
			if (node == nullptr || type == nullptr) {
				continue;
			}

			// **The schema is re-derived rather than read.** A promotion whose
			// inner widget no longer exists is dropped: keeping it would put a
			// knob on a node that writes nowhere.
			const auto found =
				std::find_if(type->Widgets.begin(), type->Widgets.end(), [&](const WidgetSpec &widget) {
					return widget.Key == lifted.Knob.InnerKey;
				});
			if (found == type->Widgets.end()) {
				continue;
			}

			Promotion knob = lifted.Knob;
			knob.Inner = inner;
			knob.Key = std::to_string(inner) + "/" + knob.InnerKey;
			knob.Spec = *found;
			knob.Spec.Key = knob.Key;
			knob.Spec.Label = knob.Label;
			node->Promoted.push_back(std::move(knob));
		}

		for (const Group &frame : groups) {
			std::vector<NodeId> members;
			for (const NodeId member : frame.Members) {
				if (const auto found = placed.find(member); found != placed.end()) {
					members.push_back(found->second);
				}
			}
			// A frame whose members all went missing is dropped rather than kept
			// empty — an empty frame is a rectangle around nothing.
			(void)graph.Group(std::move(members), frame.Title, frame.Tint);
		}

		return true;
	}
}
