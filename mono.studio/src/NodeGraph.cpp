// The node model: registries, the graph, layout, evaluation and saving.
//
// **No ImGui anywhere in this file**, which is what lets `tests/NodeGraph.cpp`
// check the half that fails silently — the cycle guard, the content hash and the
// save format — with no window. `studio/NodeGraph.hpp` carries the arguments;
// what is here is the mechanism.

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <sstream>
#include <studio/NodeGraph.hpp>
#include <unordered_set>

namespace studio::nodes {



	namespace {
		// One vocabulary per process. A function-local static, so it exists
		// before the first registration and cannot be built twice.
		struct Table {
			std::vector<DataType> Order;
			std::unordered_map<std::string, size_t> ById;
		};

		Table &Types() {
			static Table table;
			return table;
		}
	}

	Colour Colour::Hex(uint32_t rgb) {
		return Colour{
			static_cast<float>((rgb >> 16) & 0xFF) / 255.0f,
			static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
			static_cast<float>(rgb & 0xFF) / 255.0f,
			1.0f,
		};
	}

	void DataTypes::Register(const DataType &type) {
		Table &table = Types();
		const auto found = table.ById.find(type.Id);
		if (found != table.ById.end()) {
			table.Order[found->second] = type;
			return;
		}
		table.ById.emplace(type.Id, table.Order.size());
		table.Order.push_back(type);
	}

	const DataType *DataTypes::Find(const std::string &id) {
		const Table &table = Types();
		const auto found = table.ById.find(id);
		return found == table.ById.end() ? nullptr : &table.Order[found->second];
	}

	bool DataTypes::CanConnect(const std::string &from, const std::string &to) {
		if (from.empty() || to.empty()) {
			return false;
		}
		return from == to || from == ANY_TYPE || to == ANY_TYPE;
	}

	const std::vector<DataType> &DataTypes::All() {
		return Types().Order;
	}

	bool Value::operator==(const Value &other) const {
		// **Compared field by field rather than by memory**, because a `Value`
		// holds a string: two values that agree on the fields their kind uses
		// are equal, and comparing the unused ones would make a slider unequal
		// to itself over a save and load.
		if (Kind != other.Kind) {
			return false;
		}
		switch (Kind) {
		case WidgetKind::Toggle:
			return Flag == other.Flag;
		case WidgetKind::Text:
		case WidgetKind::Select:
			return Text == other.Text;
		case WidgetKind::Colour:
			return Tint.R == other.Tint.R && Tint.G == other.Tint.G && Tint.B == other.Tint.B &&
				   Tint.A == other.Tint.A;
		case WidgetKind::Slider:
		case WidgetKind::Number:
			return Number == other.Number;
		}
		return false;
	}

	PortSpec Port(std::string name, std::string type) {
		return PortSpec{std::move(name), std::move(type)};
	}

	WidgetSpec Slider(std::string key, std::string label, double minimum, double maximum, double value) {
		WidgetSpec spec;
		spec.Key = std::move(key);
		spec.Label = std::move(label);
		spec.Kind = WidgetKind::Slider;
		spec.Minimum = minimum;
		spec.Maximum = maximum;
		spec.Step = (maximum - minimum) / 100.0;
		spec.Default.Kind = WidgetKind::Slider;
		spec.Default.Number = std::clamp(value, minimum, maximum);
		return spec;
	}

	WidgetSpec Toggle(std::string key, std::string label, bool value) {
		WidgetSpec spec;
		spec.Key = std::move(key);
		spec.Label = std::move(label);
		spec.Kind = WidgetKind::Toggle;
		spec.Default.Kind = WidgetKind::Toggle;
		spec.Default.Flag = value;
		return spec;
	}

	WidgetSpec Select(std::string key, std::string label, std::vector<std::string> options, int chosen) {
		WidgetSpec spec;
		spec.Key = std::move(key);
		spec.Label = std::move(label);
		spec.Kind = WidgetKind::Select;
		spec.Options = std::move(options);
		spec.Default.Kind = WidgetKind::Select;

		// **The chosen option is stored as its text, not its index.** An index
		// is a number that means something different the moment somebody
		// reorders the options — and reordering a list is the sort of edit
		// nobody expects to change a saved graph.
		if (chosen >= 0 && static_cast<size_t>(chosen) < spec.Options.size()) {
			spec.Default.Text = spec.Options[static_cast<size_t>(chosen)];
		} else if (!spec.Options.empty()) {
			spec.Default.Text = spec.Options.front();
		}
		return spec;
	}

	WidgetSpec Number(std::string key, std::string label, double value) {
		WidgetSpec spec;
		spec.Key = std::move(key);
		spec.Label = std::move(label);
		spec.Kind = WidgetKind::Number;
		spec.Minimum = 0.0;
		spec.Maximum = 0.0;
		spec.Step = 0.1;
		spec.Default.Kind = WidgetKind::Number;
		spec.Default.Number = value;
		return spec;
	}

	WidgetSpec Text(std::string key, std::string label, std::string value) {
		WidgetSpec spec;
		spec.Key = std::move(key);
		spec.Label = std::move(label);
		spec.Kind = WidgetKind::Text;
		spec.Default.Kind = WidgetKind::Text;
		spec.Default.Text = std::move(value);
		return spec;
	}



	namespace {
		struct NodeTable {
			std::vector<NodeType> Order;
			std::unordered_map<std::string, size_t> ById;
		};

		NodeTable &Registered() {
			static NodeTable table;
			return table;
		}
	}

	Value Inputs::Widget(const std::string &key) const {
		if (Widgets == nullptr) {
			return Value{};
		}
		const auto found = Widgets->find(key);
		return found == Widgets->end() ? Value{} : found->second;
	}

	double Inputs::Real(const std::string &key) const {
		return Widget(key).Number;
	}

	void NodeTypes::Register(const NodeType &type) {
		NodeTable &table = Registered();
		const auto found = table.ById.find(type.Id);
		if (found != table.ById.end()) {
			// **Replaced in place rather than appended.** A hot-reloaded node
			// type has to keep its position in the Library, and appending would
			// move it under a different heading every reload.
			table.Order[found->second] = type;
			return;
		}
		table.ById.emplace(type.Id, table.Order.size());
		table.Order.push_back(type);
	}

	const NodeType *NodeTypes::Find(const std::string &id) {
		const NodeTable &table = Registered();
		const auto found = table.ById.find(id);
		return found == table.ById.end() ? nullptr : &table.Order[found->second];
	}

	const std::vector<NodeType> &NodeTypes::All() {
		return Registered().Order;
	}

	std::vector<std::string> NodeTypes::Categories() {
		std::vector<std::string> names;
		for (const NodeType &type : Registered().Order) {
			if (std::find(names.begin(), names.end(), type.Category) == names.end()) {
				names.push_back(type.Category);
			}
		}
		return names;
	}

	std::vector<const NodeType *> NodeTypes::AcceptingInput(const std::string &type) {
		std::vector<const NodeType *> found;
		for (const NodeType &candidate : Registered().Order) {
			for (const PortSpec &port : candidate.Inputs) {
				if (DataTypes::CanConnect(type, port.Type)) {
					found.push_back(&candidate);
					break;
				}
			}
		}
		return found;
	}




	namespace {
		// FNV-1a over bytes. A hash and not a checksum: it decides whether a
		// cached result may be reused, so what matters is that equal inputs give
		// equal output on every machine — which rules out anything that hashes a
		// pointer or an address.
		constexpr uint64_t SEED = 1469598103934665603ull;
		constexpr uint64_t PRIME = 1099511628211ull;

		uint64_t Mix(uint64_t hash, const void *bytes, size_t count) {
			const auto *walk = static_cast<const unsigned char *>(bytes);
			for (size_t index = 0; index < count; index++) {
				hash ^= walk[index];
				hash *= PRIME;
			}
			return hash;
		}

		uint64_t MixText(uint64_t hash, const std::string &text) {
			return Mix(hash, text.data(), text.size());
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
		const auto found = std::find_if(Stored.begin(), Stored.end(), [&](const Node &node) {
			return node.Id == id;
		});
		if (found == Stored.end()) {
			return false;
		}

		// The links first. A node removed with its wires left behind is a link
		// whose endpoint does not exist, which every later walk has to guard.
		Wires.erase(
			std::remove_if(
				Wires.begin(),
				Wires.end(),
				[&](const Link &link) { return link.From == id || link.To == id; }
			),
			Wires.end()
		);

		Stored.erase(found);
		return true;
	}

	bool Graph::Alive(NodeId id) const {
		return Find(id) != nullptr;
	}

	Node *Graph::Find(NodeId id) {
		const auto found = std::find_if(Stored.begin(), Stored.end(), [&](const Node &node) {
			return node.Id == id;
		});
		return found == Stored.end() ? nullptr : &*found;
	}

	const Node *Graph::Find(NodeId id) const {
		return const_cast<Graph *>(this)->Find(id);
	}

	LinkResult Graph::CanConnect(
		NodeId from, const std::string &fromPort, NodeId to, const std::string &toPort
	) const {
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

	LinkResult Graph::Connect(
		NodeId from, const std::string &fromPort, NodeId to, const std::string &toPort
	) {
		const LinkResult can = CanConnect(from, fromPort, to, toPort);
		if (can != LinkResult::Made) {
			return can;
		}

		// One link into an input, newest wins — `NodeGraph.hpp` carries why.
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

			// The inputs, in port order, each contributing its own hash — which
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
		Next = 1;
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

		if (!type->Subtitle.empty()) {
			y += metrics.RowHeight * 0.8f;
		}

		y += metrics.Padding * 0.5f;

		// **Ports are paired into rows**, input on the left and output on the
		// right, which is what makes a node with three of each six rows tall
		// instead of twelve. The tail of the longer side gets rows of its own.
		const size_t rows = std::max(type->Inputs.size(), type->Outputs.size());
		for (size_t row = 0; row < rows; row++) {
			const float centre = y + metrics.RowHeight * 0.5f;

			if (row < type->Inputs.size()) {
				layout.Ports.push_back(PlacedPort{
					type->Inputs[row].Name, type->Inputs[row].Type, true, 0.0f, centre,
				});
			}
			if (row < type->Outputs.size()) {
				layout.Ports.push_back(PlacedPort{
					type->Outputs[row].Name, type->Outputs[row].Type, false, layout.Width, centre,
				});
			}
			y += metrics.RowHeight;
		}

		// **The thumbnail above the widgets and below the ports.** A picture is
		// what the node produced from what is wired into it, so it reads in the
		// same order as the node is thought about — inputs, result, knobs.
		if (type->Preview) {
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

		if (!type->Widgets.empty()) {
			y += metrics.Padding * 0.5f;
			for (size_t index = 0; index < type->Widgets.size(); index++) {
				layout.Widgets.push_back(PlacedWidget{
					type->Widgets[index].Key,
					index,
					metrics.Padding,
					y,
					layout.Width - metrics.Padding * 2.0f,
					metrics.WidgetHeight - 4.0f,
				});
				y += metrics.WidgetHeight;
			}
		}

		layout.Height = y + metrics.Padding;
		return layout;
	}

	const PlacedPort *PortIn(const NodeLayout &layout, const std::string &name, bool input) {
		const auto found = std::find_if(layout.Ports.begin(), layout.Ports.end(), [&](const PlacedPort &port) {
			return port.Input == input && port.Name == name;
		});
		return found == layout.Ports.end() ? nullptr : &*found;
	}



	Evaluator::Evaluator() = default;

	Evaluator::~Evaluator() {
		{
			std::lock_guard<std::mutex> held(Lock);
			Stopping.store(true, std::memory_order_relaxed);
			Queue.clear();
		}
		Waking.notify_all();

		// **Joined rather than detached.** A worker outliving this object would
		// be a thread writing into freed memory, and the editor closes while a
		// long node is very often exactly what is running.
		for (std::thread &worker : Workers) {
			if (worker.joinable()) {
				worker.join();
			}
		}
	}

	void Evaluator::Begin() {
		if (Started) {
			return;
		}
		Started = true;

		// **Two, and not one per core.** These run whole node evaluations, and
		// what they buy is that two branches of a graph proceed at once — not
		// that one node goes faster, which is `parallel::Jobs`' job and can
		// still be used from inside a node. A pool the width of the machine
		// would take every core away from the frame that has to keep drawing.
		const unsigned hardware = std::thread::hardware_concurrency();
		const unsigned count = hardware > 4 ? 3u : 2u;

		Workers.reserve(count);
		for (unsigned index = 0; index < count; index++) {
			Workers.emplace_back([this] { Worker(); });
		}
	}

	void Evaluator::Worker() {
		while (true) {
			std::function<void()> task;
			{
				std::unique_lock<std::mutex> held(Lock);
				Waking.wait(held, [this] {
					return Stopping.load(std::memory_order_relaxed) || !Queue.empty();
				});
				if (Stopping.load(std::memory_order_relaxed)) {
					return;
				}
				task = std::move(Queue.front());
				Queue.pop_front();
			}
			task();
		}
	}

	RunReport Evaluator::Run(const Graph &graph) {
		RunReport report;

		const std::vector<NodeId> order = graph.Ordered();
		report.Skipped = graph.Nodes().size() - order.size();

		// **Rebuilt each run rather than pruned.** A node deleted between runs
		// would otherwise leave a row that `Output` could still answer with,
		// which is a stale picture of something that is not there. The *results*
		// survive, keyed by hash, which is where the saving actually is.
		Ran.clear();
		Reused.clear();

		// Whatever finished since the last call, before anything is scheduled —
		// so a node whose input landed a moment ago starts on this run rather
		// than on the next one.
		Collect(report);

		for (const NodeId id : order) {
			const Node *node = graph.Find(id);
			if (node == nullptr) {
				continue;
			}

			const NodeType *type = NodeTypes::Find(node->Type);
			if (type == nullptr || !type->Evaluate) {
				report.Skipped++;
				continue;
			}

			const uint64_t hash = graph.Hash(id);
			Ran.emplace(id, hash);

			if (Results.find(hash) != Results.end()) {
				Reused.emplace(id, true);
				NodeStatus &status = States[id];
				status.State = NodeState::Done;
				status.Progress = 1.0f;
				status.Cached = true;
				report.Cached++;
				continue;
			}

			// **Already being computed, by this node or by another with the same
			// hash.** Two identical branches are one piece of work: the second
			// waits on the first rather than starting its own copy, which is the
			// same saving the cache gives after the fact.
			if (const auto flying = Flight.find(hash); flying != Flight.end()) {
				NodeStatus &status = States[id];
				status.State = NodeState::Running;
				status.Progress = flying->second->Progress.load(std::memory_order_relaxed);
				status.Step = flying->second->Step.load(std::memory_order_relaxed);
				status.Cached = false;
				{
					std::lock_guard<std::mutex> held(flying->second->Words);
					status.Note = flying->second->Note;
				}
				report.Running++;
				continue;
			}

			// The inputs, gathered from what the nodes upstream produced. An
			// unconnected input is simply absent — `Inputs::In` answers the
			// caller's fallback, which is the ordinary case and not an error.
			//
			// **An input that is not there *yet* is different.** A node whose
			// upstream is still being computed must not run with the fallback,
			// because it would produce a picture of nothing and cache it under a
			// hash that says otherwise. So it waits.
			Inputs inputs;
			inputs.Widgets = &node->Widgets;
			bool waiting = false;

			for (const PortSpec &port : type->Inputs) {
				const Link *link = graph.LinkInto(id, port.Name);
				if (link == nullptr) {
					continue;
				}
				if (const std::any *upstream = Output(link->From, link->FromPort); upstream != nullptr) {
					inputs.Ports.emplace(port.Name, *upstream);
					continue;
				}

				const auto ran = Ran.find(link->From);
				if (ran != Ran.end() && Flight.find(ran->second) != Flight.end()) {
					waiting = true;
					break;
				}
			}

			if (waiting) {
				NodeStatus &status = States[id];
				status.State = NodeState::Idle;
				status.Note = "waiting on an input";
				report.Waiting++;
				continue;
			}

			if (!type->Async) {
				const auto began = std::chrono::steady_clock::now();
				Results.emplace(hash, type->Evaluate(inputs));
				Reused.emplace(id, false);

				NodeStatus &status = States[id];
				status.State = NodeState::Done;
				status.Progress = 1.0f;
				status.Cached = false;
				status.Step = 0;
				status.Note.clear();
				status.Milliseconds =
					std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began)
						.count();

				report.Evaluated++;
				continue;
			}

			// --- off to a worker ----------------------------------------------
			//
			// **Everything it reads is copied here, on this thread.** The graph
			// is edited while the worker runs, so a task holding a pointer into
			// it is a race — `NodeType::Async` carries the rule. What crosses is
			// the widget values, the input payloads and a copy of the function.
			Begin();

			auto live = std::make_shared<Live>();
			live->Hash = hash;
			live->Node = id;

			auto widgets = std::make_shared<std::unordered_map<std::string, Value>>(node->Widgets);
			auto ports = std::make_shared<std::unordered_map<std::string, std::any>>(
				std::move(inputs.Ports)
			);
			auto work = type->Evaluate;
			const std::atomic<bool> *stopping = &Stopping;

			Flight.emplace(hash, live);

			{
				std::lock_guard<std::mutex> held(Lock);
				Queue.emplace_back([live, widgets, ports, work, stopping] {
					const auto began = std::chrono::steady_clock::now();

					Inputs copied;
					copied.Widgets = widgets.get();
					copied.Ports = *ports;
					copied.Stopping = stopping;
					copied.Report = [live](size_t step, float fraction, std::string_view note) {
						live->Step.store(step, std::memory_order_relaxed);
						live->Progress.store(fraction, std::memory_order_relaxed);
						if (!note.empty()) {
							std::lock_guard<std::mutex> words(live->Words);
							live->Note.assign(note);
						}
					};

					Outputs made;
					bool failed = false;
					try {
						made = work(copied);
					} catch (...) {
						// **A node that throws is one failed node.** The
						// alternative is a worker that dies and a graph that
						// never finishes, with nothing on screen saying which
						// node did it.
						failed = true;
					}

					live->Milliseconds.store(
						std::chrono::duration<double, std::milli>(
							std::chrono::steady_clock::now() - began
						)
							.count(),
						std::memory_order_relaxed
					);
					live->Produced = std::move(made);
					live->Failed.store(failed, std::memory_order_relaxed);
					live->Progress.store(1.0f, std::memory_order_relaxed);

					// **Last, and with release ordering.** `Finished` is what the
					// frame thread reads to decide the payload is safe to take.
					live->Finished.store(true, std::memory_order_release);
				});
			}
			Waking.notify_one();

			NodeStatus &status = States[id];
			status.State = NodeState::Running;
			status.Progress = 0.0f;
			status.Step = 0;
			status.Cached = false;
			status.Note = type->Steps.empty() ? std::string("working") : type->Steps.front();

			report.Started++;
			report.Running++;
		}

		return report;
	}

	void Evaluator::Collect(RunReport &report) {
		for (auto walk = Flight.begin(); walk != Flight.end();) {
			const std::shared_ptr<Live> &live = walk->second;
			if (!live->Finished.load(std::memory_order_acquire)) {
				++walk;
				continue;
			}

			NodeStatus &status = States[live->Node];
			status.Milliseconds = live->Milliseconds.load(std::memory_order_relaxed);
			status.Progress = 1.0f;
			status.Cached = false;

			if (live->Failed.load(std::memory_order_relaxed)) {
				status.State = NodeState::Failed;
				status.Note = "it raised";
			} else {
				status.State = NodeState::Done;
				status.Note.clear();
				Results.emplace(live->Hash, std::move(live->Produced));
				report.Finished++;
			}

			walk = Flight.erase(walk);
		}
	}

	RunReport Evaluator::RunToCompletion(const Graph &graph) {
		RunReport report = Run(graph);
		while (Busy() || report.Waiting > 0) {
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			report = Run(graph);
		}
		return report;
	}

	NodeStatus Evaluator::Status(NodeId node) const {
		const auto found = States.find(node);
		if (found == States.end()) {
			return NodeStatus{};
		}

		NodeStatus status = found->second;

		// **Read through to the live task rather than from the last `Run`.** A
		// bar that only moved when the graph was re-run would tick once a second
		// on a busy editor and not at all on an idle one.
		const auto ran = Ran.find(node);
		if (ran == Ran.end()) {
			return status;
		}
		const auto flying = Flight.find(ran->second);
		if (flying == Flight.end()) {
			return status;
		}

		status.State = NodeState::Running;
		status.Progress = flying->second->Progress.load(std::memory_order_relaxed);
		status.Step = flying->second->Step.load(std::memory_order_relaxed);
		{
			std::lock_guard<std::mutex> held(flying->second->Words);
			status.Note = flying->second->Note;
		}
		return status;
	}

	uint64_t Evaluator::RanAt(NodeId node) const {
		const auto found = Ran.find(node);
		return found == Ran.end() ? 0 : found->second;
	}

	bool Evaluator::Busy() const {
		return !Flight.empty();
	}

	const std::any *Evaluator::Output(NodeId node, const std::string &port) const {
		const auto ran = Ran.find(node);
		if (ran == Ran.end()) {
			return nullptr;
		}

		const auto held = Results.find(ran->second);
		if (held == Results.end()) {
			return nullptr;
		}

		const auto found = held->second.find(port);
		return found == held->second.end() ? nullptr : &found->second;
	}

	bool Evaluator::WasCached(NodeId node) const {
		const auto found = Reused.find(node);
		return found != Reused.end() && found->second;
	}

	void Evaluator::Forget() {
		Results.clear();
		Ran.clear();
		Reused.clear();
		States.clear();
	}



	namespace {
		constexpr const char *HEADER = "nodegraph 1";

		std::string_view Trim(std::string_view text) {
			while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
				text.remove_prefix(1);
			}
			while (!text.empty() &&
				   (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
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
			out << "node | " << node.Id << " | " << node.Type << " | " << node.X << " | " << node.Y
				<< " | " << node.Label << "\n";

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

		return out.str();
	}

	bool Load(std::string_view text, Graph &graph, std::string &error) {
		if (Trim(text.substr(0, text.find('\n'))) != HEADER) {
			error = "not a nodegraph document";
			return false;
		}

		graph.Clear();

		// The file's ids are not the graph's: `Add` hands out its own, so a
		// document with any numbering loads. This maps one to the other.
		std::unordered_map<uint32_t, NodeId> placed;
		std::vector<Link> pending;

		size_t start = text.find('\n');
		start = start == std::string_view::npos ? text.size() : start + 1;

		while (start < text.size()) {
			const size_t end = std::min(text.find('\n', start), text.size());
			const std::string_view line = Trim(text.substr(start, end - start));
			start = end + 1;

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
					// `NodeNodeGraph.hpp` carries why — which needs the node to
					// exist without `Add`'s type lookup.
					Node broken;
					broken.Type = std::string(fields[2]);
					broken.X = static_cast<float>(Real(fields[3]));
					broken.Y = static_cast<float>(Real(fields[4]));
					broken.Id = static_cast<NodeId>(graph.Nodes().size() + 1000000);
					if (fields.size() >= 6) {
						broken.Label = std::string(fields[5]);
					}
					placed.emplace(Whole(fields[1]), broken.Id);
					graph.Nodes().push_back(std::move(broken));
					continue;
				}

				placed.emplace(Whole(fields[1]), id);
				if (fields.size() >= 6 && !fields[5].empty()) {
					graph.Find(id)->Label = std::string(fields[5]);
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
				pending.push_back(Link{
					Whole(fields[1]), std::string(fields[2]), Whole(fields[3]), std::string(fields[4]),
				});
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

		return true;
	}

}
