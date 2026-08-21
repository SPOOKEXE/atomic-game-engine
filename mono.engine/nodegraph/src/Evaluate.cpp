// Running the graph: the cache, the worker pool, and the collection pass.

#include <engine/nodegraph/Evaluate.hpp>
#include <engine/nodegraph/Layout.hpp>

#include <algorithm>
#include <chrono>
#include <unordered_set>

namespace engine::nodegraph {

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
			auto ports = std::make_shared<std::unordered_map<std::string, std::any>>(std::move(inputs.Ports));
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
						std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began)
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
}
