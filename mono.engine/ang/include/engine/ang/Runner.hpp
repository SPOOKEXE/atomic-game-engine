#pragma once

// Runs a compiled Plan: three execution modes, hooks, timing and memory budgets.
//
// Root AGENTS.md rule 5 is enforced here and in Compile: Sync and Parallel work
// finishes inside Run, Async work exists only in Frame and Tool graphs, and Run
// never blocks on it. Results are committed in wave order on the calling
// thread, so a Parallel wave gives the same bytes as a serial one.
// @tier L3 · shared

#include <engine/ang/Compile.hpp>
#include <engine/core/FrameGraph.hpp>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace engine::ang {

	// Values handed to Evaluate and the outputs it writes.
	class EvalContext {
	  public:
		std::string_view NodeId() const {
			return Node;
		}

		// First value on an input: linked, run-supplied, a param of the same
		// name, or the port default, in that order.
		const Value &Input(std::string_view port) const;
		// Every value on a multi-link input, in link order.
		std::span<const Value> Inputs(std::string_view port) const;
		// A param value, or the definition default.
		const Value &Param(std::string_view name) const;
		// Writes an output. Unknown names are ignored and reported as a failure.
		void SetOutput(std::string_view port, Value value);

		// Async work polls this and returns early once the node was edited or cancelled.
		bool Cancelled() const {
			return CancelFlag != nullptr && CancelFlag->load(std::memory_order_relaxed);
		}

		Domain Runs = Domain::Tool;

	  private:
		friend class Runner;
		friend struct RunnerAccess;

		std::string Node{};
		std::shared_ptr<const NodeDef> Def{};
		std::shared_ptr<const std::map<std::string, Value>> Params{};
		std::vector<std::vector<Value>> InputValues{};
		std::vector<Value> OutputValues{};
		std::string BadOutput{};
		std::shared_ptr<std::atomic<bool>> CancelFlag{};
	};

	// Why a node did or did not produce outputs this run.
	enum class NodeStatus : uint8_t {
		// Not visited yet.
		Idle,
		Ran,
		// A Pure node whose hash matched retained outputs.
		Cached,
		// Unknown definition, no Evaluate, or a required input missing.
		Skipped,
		// Async work in flight, or an input from such work.
		Waiting,
		Failed,
		Bypassed,
		// Refused because the graph budget was exhausted.
		Budget,
		// Async work dropped because its node changed or the runner was cancelled.
		Cancelled,
	};

	const char *Describe(NodeStatus status);

	// Who a hook is about.
	struct NodeInfo {
		std::string_view Id{};
		std::string_view Def{};
		uint32_t Index = 0;
		uint32_t Wave = 0;
		ExecMode Mode = ExecMode::Sync;
		uint64_t RunNumber = 0;
	};

	struct RunReport;

	struct RunInfo {
		uint64_t RunNumber = 0;
		Domain Runs = Domain::Tool;
		size_t NodeCount = 0;
		// Set for PostRun only.
		const RunReport *Report = nullptr;
	};

	// One node's evaluation, which PostCompute may inspect or replace.
	struct ComputeResult {
		NodeStatus Status = NodeStatus::Ran;
		std::string Note{};
		std::vector<Value> Outputs{};
	};

	// Optional callbacks. An empty hook costs one branch: `just ang-bench` (bench
	// preset, 2026-11-22) measured 144 ns per cached visit without hooks and 145
	// with four empty-body hooks set, 247 vs 250 ns per evaluated visit.
	//
	// PreNode/PostNode fire for every visit, cache hits and skips included.
	// PreCompute/PostCompute fire only when Evaluate runs. All of them fire on
	// the thread that ran the node, so a hook on a Parallel or Async graph must
	// be thread safe.
	struct Hooks {
		std::function<void(const RunInfo &)> PreRun{};
		std::function<void(const RunInfo &)> PostRun{};
		std::function<void(const NodeInfo &)> PreNode{};
		std::function<void(const NodeInfo &, NodeStatus)> PostNode{};
		std::function<void(const NodeInfo &)> PreCompute{};
		std::function<void(const NodeInfo &, ComputeResult &)> PostCompute{};
	};

	struct NodeReport {
		NodeStatus Status = NodeStatus::Idle;
		std::string Note{};
		double Milliseconds = 0.0;
		size_t Bytes = 0;
	};

	struct RunReport {
		uint64_t RunNumber = 0;
		size_t Ran = 0;
		size_t Cached = 0;
		size_t Skipped = 0;
		size_t Waiting = 0;
		size_t Failed = 0;
		size_t Bypassed = 0;
		size_t Budget = 0;
		size_t Cancelled = 0;
		double Milliseconds = 0.0;
		// Bytes of outputs retained after the run.
		size_t RetainedBytes = 0;
		// Highest retained total seen during the run.
		size_t PeakBytes = 0;
		// Bytes produced by nodes that evaluated this run.
		size_t ProducedBytes = 0;
		// True when nothing is Waiting.
		bool Complete = true;
		std::vector<NodeReport> Nodes{};
	};

	// One node visit on the timeline. Times are nanoseconds from the run start.
	struct TimelineSample {
		uint32_t Node = 0;
		int64_t BeginNanoseconds = 0;
		int64_t EndNanoseconds = 0;
		uint64_t Thread = 0;
		NodeStatus Outcome = NodeStatus::Idle;
	};

	struct TimelineRun {
		uint64_t RunNumber = 0;
		double Milliseconds = 0.0;
		std::vector<TimelineSample> Samples{};
	};

	// The last N runs, oldest first. A view draws the strip under the canvas from it.
	class RunTimeline {
	  public:
		explicit RunTimeline(size_t capacity = 8) : Capacity(capacity == 0 ? 1 : capacity) {}

		void Push(TimelineRun run);
		const std::deque<TimelineRun> &Runs() const {
			return History;
		}
		const TimelineRun *Latest() const {
			return History.empty() ? nullptr : &History.back();
		}

	  private:
		size_t Capacity;
		std::deque<TimelineRun> History{};
	};

	struct NodeMemory {
		// Bytes this node's outputs hold now.
		size_t Current = 0;
		// Most it has ever held.
		size_t Peak = 0;
		// Whether the current outputs are reusable by hash.
		bool Cached = false;
	};

	// Bounded CPU workers for Async nodes. Not Jobs: Jobs work must finish inside
	// the tick, this work may land frames later.
	class AsyncPool {
	  public:
		// `workers` zero picks hardware threads minus pinned job workers, at least one.
		explicit AsyncPool(unsigned workers = 0, size_t maximumQueued = 1024);
		~AsyncPool();

		AsyncPool(const AsyncPool &) = delete;
		AsyncPool &operator=(const AsyncPool &) = delete;

		// False when the queue is full; the caller retries on its next Run.
		bool Submit(std::function<void()> work);

		unsigned WorkerCount() const {
			return static_cast<unsigned>(Workers.size());
		}

		// Blocks until the queue is empty and no work runs. Tests and shutdown only.
		void Drain();

		// The process pool, created on first use.
		static AsyncPool &Shared();

	  private:
		void Work();

		std::vector<std::thread> Workers{};
		std::deque<std::function<void()>> Queue{};
		std::mutex Lock{};
		std::condition_variable Wake{};
		std::condition_variable Idle{};
		size_t Running = 0;
		size_t MaximumQueued = 0;
		bool Stopping = false;
	};

	struct RunnerOptions {
		// Retained output budget in bytes; zero is unlimited.
		size_t BudgetBytes = 0;
		size_t TimelineRuns = 8;
		core::ProfileCategory Category = core::ProfileCategory::Engine;
		// Null uses AsyncPool::Shared().
		AsyncPool *Pool = nullptr;
		// Parallel nodes in one wave below this count run inline. Dispatch is not
		// free: a trivial node costs about 250 ns serial and 500 ns through
		// Jobs::For at 8, 64 and 512 nodes per wave (`just ang-bench`, bench
		// preset, 23 workers). Mark a node Parallel only when its own work is
		// well above that.
		size_t ParallelMinimum = 2;
	};

	// Values supplied for unlinked inputs, by node id then port.
	using RunInputs = std::map<std::string, std::map<std::string, Value>, std::less<>>;

	struct AsyncJob;

	class Runner {
	  public:
		explicit Runner(Plan plan, RunnerOptions options = {});
		~Runner();

		Runner(const Runner &) = delete;
		Runner &operator=(const Runner &) = delete;

		// Swaps in a new plan. Outputs whose node hash still matches are kept, and
		// in-flight Async work for changed nodes is cancelled.
		void SetPlan(Plan plan);
		const Plan &CurrentPlan() const {
			return Compiled;
		}

		Hooks &Callbacks() {
			return HookSet;
		}

		// Visits every node once. Never blocks on Async work.
		RunReport Run(const RunInputs &inputs = {});

		// Borrowed output of the last run that produced one, or null.
		const Value *Output(std::string_view node, std::string_view port) const;

		const RunTimeline &Timeline() const {
			return History;
		}
		NodeMemory Memory(std::string_view node) const;
		size_t RetainedBytes() const {
			return Retained;
		}

		// Cancels every in-flight Async job.
		void Cancel();
		// Async jobs still running.
		size_t InFlight() const;

		// Drops every retained output.
		void ClearCache();

	  private:
		struct Slot {
			uint64_t RunHash = 0;
			bool Valid = false;
			bool Cacheable = false;
			std::vector<Value> Outputs{};
			size_t Bytes = 0;
			size_t Peak = 0;
			// Touch sequence for least-recently-used eviction.
			uint64_t LastUsed = 0;
			std::shared_ptr<AsyncJob> Job{};
		};

		struct Pending;

		void Release(Slot &slot);
		bool MakeRoom(size_t bytes);

		Plan Compiled;
		RunnerOptions Options;
		Hooks HookSet{};
		RunTimeline History;
		std::vector<Slot> Slots{};
		std::vector<uint64_t> RunHashes{};
		size_t Retained = 0;
		uint64_t RunCounter = 0;
		uint64_t TouchCounter = 0;
		// Consumers of each node not yet visited this run; nonzero pins its outputs.
		std::vector<uint32_t> PendingConsumers{};
		uint64_t ImpureNonce = 0;
	};
}
