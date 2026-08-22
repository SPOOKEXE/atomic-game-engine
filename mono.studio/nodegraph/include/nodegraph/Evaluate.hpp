#pragma once

// Running the graph, and not running it twice.
//
// The graph is not a picture of a pipeline; it *is* the pipeline. A run walks
// `Graph::Ordered`, calls each type's `Evaluate` with whatever its inputs
// produced, and keeps the result against `Graph::Hash`, so editing one slider
// recomputes exactly the sub-tree below it and nothing else.
//
// **One call a frame, and it never blocks.** An async node is handed to a worker
// and collected by a later `Run`, which is what lets a graph carrying something
// genuinely slow stay editable while it works.

#include <any>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <nodegraph/Graph.hpp>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace nodegraph {

	// What one run did.
	struct RunReport {
		// Nodes whose evaluation actually ran.
		size_t Evaluated = 0;

		// Nodes whose hash matched, so the previous result stood.
		size_t Cached = 0;

		// Nodes with no evaluation at all: a comment, or a type this build does
		// not have. They will never produce anything, which is what
		// separates them from `Waiting`.
		size_t Skipped = 0;

		// Async nodes handed to a worker by this run.
		size_t Started = 0;

		// Async results collected by this run.
		size_t Finished = 0;

		// Async nodes still working when it returned.
		size_t Running = 0;

		// Nodes waiting on an input that is still being computed.
		//
		// **Counted rather than treated as skipped**, because they are two
		// different facts: a node with no evaluation will never produce
		// anything, and this one is about to.
		size_t Waiting = 0;
	};

	// What a node is doing, as the canvas draws it.
	enum class NodeState : uint8_t { Idle, Running, Done, Failed };

	// One node's live state.
	struct NodeStatus {
		// What it is doing, which is what decides how the canvas tints it.
		NodeState State = NodeState::Idle;

		// 0 to 1, from the node's own reporting.
		float Progress = 0.0f;

		// Which of `NodeType::Steps` it says it is on.
		size_t Step = 0;

		// What it last said it was doing.
		std::string Note;

		// How long the evaluation took, once it has finished.
		double Milliseconds = 0.0;

		// Whether the last result came from the cache rather than a run.
		bool Cached = false;
	};

	// Runs a graph and holds what it produced.
	//
	// **One call a frame, and it never blocks.** `Run` evaluates every sync node
	// it can, hands every ready async one to a worker, collects whatever
	// finished since last time, and returns. A node whose input is still being
	// computed is left for the next call, which is what makes two independent
	// branches run at once without anything here scheduling them: readiness is
	// the schedule.
	class Evaluator {
	  public:
		Evaluator();
		~Evaluator();

		Evaluator(const Evaluator &) = delete;
		Evaluator &operator=(const Evaluator &) = delete;

		// Evaluates whatever is out of date, once.
		//
		// **One pass and it returns**, which is what makes this callable from a
		// frame. Sync nodes are computed inline; async ones are dispatched and
		// collected by a later call, so a graph that is still working reports
		// `Running` rather than blocking.
		//
		// @param graph The graph to bring up to date. Not modified.
		// @return What this pass did.
		RunReport Run(const Graph &graph);

		// Runs until nothing is left working.
		//
		// **For a test and for shutdown, not for a frame.** Blocking the frame
		// thread on a worker is exactly what the async path exists to avoid.
		RunReport RunToCompletion(const Graph &graph);

		// What a node's output port produced in the last run, or nullptr.
		const std::any *Output(NodeId node, const std::string &port) const;

		// Whether a node's last run came from the cache.
		bool WasCached(NodeId node) const;

		// What a node is doing right now.
		NodeStatus Status(NodeId node) const;

		// The hash a node last ran at, or zero.
		//
		// **What a thumbnail is keyed on.** A picture belongs to a result and
		// not to a node, so two nodes with one hash share one texture and an
		// edit makes a new key rather than overwriting the old one.
		uint64_t RanAt(NodeId node) const;

		// Whether any worker is still busy.
		bool Busy() const;

		// Drops every held result. In-flight work is left to finish and its
		// result is kept: it is keyed by a hash that is still correct.
		void Forget();

		// How many results are cached.
		//
		// For a test and for a panel that wants to say what the cache is
		// costing; nothing about evaluation reads it.
		//
		// @return The count.
		size_t Held() const {
			return Results.size();
		}

	  private:
		// One job on its way to a worker, and its result on the way back.
		struct Task;

		// The progress one running node publishes. Shared with its worker, so
		// every field is either atomic or behind the small lock beside them.
		struct Live {
			std::atomic<float> Progress{0.0f};
			std::atomic<size_t> Step{0};
			std::atomic<bool> Finished{false};
			std::atomic<bool> Failed{false};
			std::atomic<double> Milliseconds{0.0};
			std::mutex Words;
			std::string Note;
			Outputs Produced;
			uint64_t Hash = 0;
			NodeId Node = NO_NODE;
		};

		void Begin();
		void Collect(RunReport &report);
		void Worker();

		// **Keyed by hash and not by node.** Undoing an edit, or flipping a
		// value back, then lands on a result that is still there rather than
		// recomputing it. An async result that arrives after its node has been
		// edited is still correct for the hash it was computed at.
		std::unordered_map<uint64_t, Outputs> Results;
		std::unordered_map<NodeId, uint64_t> Ran;
		std::unordered_map<NodeId, bool> Reused;
		std::unordered_map<NodeId, NodeStatus> States;

		// What is in flight, by the hash it is computing. A second node with the
		// same hash waits on the first rather than starting its own copy.
		std::unordered_map<uint64_t, std::shared_ptr<Live>> Flight;

		std::vector<std::thread> Workers;
		std::deque<std::function<void()>> Queue;
		mutable std::mutex Lock;
		std::condition_variable Waking;
		std::atomic<bool> Stopping{false};
		bool Started = false;
	};
}
