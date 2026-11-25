#include <engine/ang/Runner.hpp>
#include <engine/core/HeapProfile.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/parallel/Jobs.hpp>

#include <algorithm>
#include <chrono>
#include <exception>

namespace engine::ang {

	namespace {
		const Value EMPTY_VALUE{};

		int64_t NowNanoseconds() {
			return std::chrono::duration_cast<std::chrono::nanoseconds>(
					   std::chrono::steady_clock::now().time_since_epoch()
			)
				.count();
		}

		uint64_t ThreadTag() {
			return std::hash<std::thread::id>{}(std::this_thread::get_id());
		}

		double Milliseconds(int64_t begin, int64_t end) {
			return static_cast<double>(end - begin) / 1.0e6;
		}

		// Run-supplied values may be ints where the port wants a number; that one
		// widening is free. Anything else must match or the port must be `any`.
		bool Accepts(const TypeId &port, const Value &value, Value &converted) {
			if (port == ANY_TYPE || port == value.TypeName()) {
				converted = value;
				return true;
			}
			if (port == "number" && (value.TypeName() == "int" || value.TypeName() == "bool")) {
				converted = Value(value.AsNumber());
				return true;
			}
			return false;
		}
	}

	const char *Describe(NodeStatus status) {
		switch (status) {
		case NodeStatus::Idle:
			return "idle";
		case NodeStatus::Ran:
			return "ran";
		case NodeStatus::Cached:
			return "cached";
		case NodeStatus::Skipped:
			return "skipped";
		case NodeStatus::Waiting:
			return "waiting";
		case NodeStatus::Failed:
			return "failed";
		case NodeStatus::Bypassed:
			return "bypassed";
		case NodeStatus::Budget:
			return "over budget";
		case NodeStatus::Cancelled:
			return "cancelled";
		}
		return "idle";
	}

	// --- EvalContext -------------------------------------------------------------------------

	namespace {
		template <typename Ports> int64_t PortIndex(const Ports &ports, std::string_view name) {
			for (size_t index = 0; index < ports.size(); ++index) {
				if (ports[index].Name == name) {
					return static_cast<int64_t>(index);
				}
			}
			return -1;
		}
	}

	const Value &EvalContext::Input(std::string_view port) const {
		const auto values = Inputs(port);
		return values.empty() ? EMPTY_VALUE : values.front();
	}

	std::span<const Value> EvalContext::Inputs(std::string_view port) const {
		if (Def == nullptr) {
			return {};
		}
		const int64_t index = PortIndex(Def->Inputs, port);
		if (index < 0 || static_cast<size_t>(index) >= InputValues.size()) {
			return {};
		}
		return InputValues[static_cast<size_t>(index)];
	}

	const Value &EvalContext::Param(std::string_view name) const {
		if (Params == nullptr) {
			return EMPTY_VALUE;
		}
		const auto found = Params->find(std::string(name));
		return found == Params->end() ? EMPTY_VALUE : found->second;
	}

	void EvalContext::SetOutput(std::string_view port, Value value) {
		const int64_t index = Def == nullptr ? -1 : PortIndex(Def->Outputs, port);
		if (index < 0) {
			BadOutput = std::string(port);
			return;
		}
		OutputValues[static_cast<size_t>(index)] = std::move(value);
	}

	// --- RunTimeline -------------------------------------------------------------------------

	void RunTimeline::Push(TimelineRun run) {
		History.push_back(std::move(run));
		while (History.size() > Capacity) {
			History.pop_front();
		}
	}

	// --- AsyncPool ---------------------------------------------------------------------------

	AsyncPool::AsyncPool(unsigned workers, size_t maximumQueued) : MaximumQueued(maximumQueued) {
		if (workers == 0) {
			const unsigned hardware = std::max(1u, std::thread::hardware_concurrency());
			const unsigned pinned = parallel::Jobs::PinnedWorkerCount();
			workers = hardware > pinned ? hardware - pinned : 1;
		}
		Workers.reserve(workers);
		for (unsigned index = 0; index < workers; ++index) {
			Workers.emplace_back([this] { Work(); });
		}
	}

	AsyncPool::~AsyncPool() {
		{
			std::lock_guard guard(Lock);
			Stopping = true;
		}
		Wake.notify_all();
		for (auto &worker : Workers) {
			worker.join();
		}
	}

	bool AsyncPool::Submit(std::function<void()> work) {
		{
			std::lock_guard guard(Lock);
			if (Stopping || Queue.size() >= MaximumQueued) {
				return false;
			}
			Queue.push_back(std::move(work));
		}
		Wake.notify_one();
		return true;
	}

	void AsyncPool::Drain() {
		std::unique_lock guard(Lock);
		Idle.wait(guard, [this] { return Queue.empty() && Running == 0; });
	}

	void AsyncPool::Work() {
		for (;;) {
			std::function<void()> work;
			{
				std::unique_lock guard(Lock);
				Wake.wait(guard, [this] { return Stopping || !Queue.empty(); });
				if (Queue.empty()) {
					return;
				}
				work = std::move(Queue.front());
				Queue.pop_front();
				++Running;
			}
			work();
			{
				std::lock_guard guard(Lock);
				--Running;
			}
			Idle.notify_all();
		}
	}

	AsyncPool &AsyncPool::Shared() {
		static AsyncPool pool;
		return pool;
	}

	// --- Runner ------------------------------------------------------------------------------

	// Shared between a Runner and the pool worker running one Async node.
	struct AsyncJob {
		uint64_t RunHash = 0;
		std::shared_ptr<std::atomic<bool>> Cancel = std::make_shared<std::atomic<bool>>(false);
		std::atomic<bool> Done{false};
		ComputeResult Result{};
		int64_t BeginNanoseconds = 0;
		int64_t EndNanoseconds = 0;
		uint64_t Thread = 0;
	};

	// Grants the runner's free helpers access to EvalContext internals.
	struct RunnerAccess {
		static EvalContext Make(
			const PlannedNode &node,
			Domain runs,
			std::vector<std::vector<Value>> inputs,
			std::shared_ptr<std::atomic<bool>> cancel
		) {
			EvalContext context;
			context.Runs = runs;
			context.Node = node.Id;
			context.Def = node.Def;
			context.Params = node.Params;
			context.InputValues = std::move(inputs);
			context.OutputValues.resize(node.OutputNames.size());
			context.CancelFlag = std::move(cancel);
			return context;
		}

		// Runs Evaluate with the compute hooks. Exceptions become a Failed result.
		static ComputeResult Evaluate(const Hooks &hooks, const NodeInfo &info, EvalContext &context) {
			if (hooks.PreCompute) {
				hooks.PreCompute(info);
			}
			ComputeResult result;
			try {
				ENGINE_HEAP_SCOPE("ang.node");
				const NodeResult outcome = context.Def->Evaluate(context);
				result.Status = outcome.Ok ? NodeStatus::Ran : NodeStatus::Failed;
				result.Note = outcome.Note;
				if (outcome.Ok && !context.BadOutput.empty()) {
					result.Status = NodeStatus::Failed;
					result.Note = "wrote unknown output '" + context.BadOutput + "'";
				}
			} catch (const std::exception &error) {
				result.Status = NodeStatus::Failed;
				result.Note = error.what();
			} catch (...) {
				result.Status = NodeStatus::Failed;
				result.Note = "evaluate threw";
			}
			if (context.Cancelled() && result.Status == NodeStatus::Ran) {
				result.Status = NodeStatus::Cancelled;
			}
			if (result.Status == NodeStatus::Ran) {
				result.Outputs = std::move(context.OutputValues);
			}
			if (hooks.PostCompute) {
				hooks.PostCompute(info, result);
			}
			return result;
		}
	};

	// One node's prepared visit, filled on the caller and finished wherever it runs.
	struct Runner::Pending {
		uint32_t Node = 0;
		NodeInfo Info{};
		EvalContext Context{};
		uint64_t RunHash = 0;
		bool Cacheable = false;
		ComputeResult Result{};
		int64_t BeginNanoseconds = 0;
		int64_t EndNanoseconds = 0;
		uint64_t Thread = 0;
	};

	Runner::Runner(Plan plan, RunnerOptions options)
		: Compiled(std::move(plan)), Options(options), History(options.TimelineRuns) {
		Slots.resize(Compiled.Nodes.size());
		RunHashes.resize(Compiled.Nodes.size());
	}

	Runner::~Runner() {
		Cancel();
	}

	void Runner::Release(Slot &slot) {
		Retained -= slot.Bytes;
		slot.Bytes = 0;
		slot.Outputs.clear();
		slot.Valid = false;
		slot.Cacheable = false;
	}

	void Runner::SetPlan(Plan plan) {
		std::vector<Slot> slots(plan.Nodes.size());
		for (size_t index = 0; index < plan.Nodes.size(); ++index) {
			const int64_t old = Compiled.Find(plan.Nodes[index].Id);
			if (old < 0) {
				continue;
			}
			Slot &previous = Slots[static_cast<size_t>(old)];
			if (Compiled.Nodes[static_cast<size_t>(old)].Hash == plan.Nodes[index].Hash) {
				slots[index] = std::move(previous);
				previous = Slot{};
			}
		}
		// Whatever did not carry over is the edited closure: cancel and free it.
		for (Slot &slot : Slots) {
			if (slot.Job != nullptr) {
				slot.Job->Cancel->store(true);
				slot.Job.reset();
			}
			Release(slot);
		}
		Slots = std::move(slots);
		Compiled = std::move(plan);
		RunHashes.assign(Compiled.Nodes.size(), 0);
	}

	void Runner::Cancel() {
		for (Slot &slot : Slots) {
			if (slot.Job != nullptr) {
				slot.Job->Cancel->store(true);
				slot.Job.reset();
			}
		}
	}

	size_t Runner::InFlight() const {
		size_t count = 0;
		for (const Slot &slot : Slots) {
			count += slot.Job != nullptr && !slot.Job->Done.load(std::memory_order_acquire) ? 1 : 0;
		}
		return count;
	}

	void Runner::ClearCache() {
		for (Slot &slot : Slots) {
			Release(slot);
		}
	}

	bool Runner::MakeRoom(size_t bytes) {
		if (Options.BudgetBytes == 0) {
			return true;
		}
		while (Retained + bytes > Options.BudgetBytes) {
			// Least recently used Pure output that no unvisited consumer still needs.
			Slot *oldest = nullptr;
			for (size_t node = 0; node < Slots.size(); ++node) {
				Slot &slot = Slots[node];
				if (!slot.Valid || !slot.Cacheable || PendingConsumers[node] != 0) {
					continue;
				}
				if (oldest == nullptr || slot.LastUsed < oldest->LastUsed) {
					oldest = &slot;
				}
			}
			if (oldest == nullptr) {
				return false;
			}
			Release(*oldest);
		}
		return true;
	}

	NodeMemory Runner::Memory(std::string_view node) const {
		const int64_t index = Compiled.Find(node);
		if (index < 0) {
			return {};
		}
		const Slot &slot = Slots[static_cast<size_t>(index)];
		return {.Current = slot.Bytes, .Peak = slot.Peak, .Cached = slot.Valid && slot.Cacheable};
	}

	const Value *Runner::Output(std::string_view node, std::string_view port) const {
		const int64_t index = Compiled.Find(node);
		if (index < 0) {
			return nullptr;
		}
		const PlannedNode &planned = Compiled.Nodes[static_cast<size_t>(index)];
		const Slot &slot = Slots[static_cast<size_t>(index)];
		for (size_t output = 0; output < planned.OutputNames.size(); ++output) {
			if (planned.OutputNames[output] == port) {
				if (!slot.Valid || output >= slot.Outputs.size() || slot.Outputs[output].Empty()) {
					return nullptr;
				}
				return &slot.Outputs[output];
			}
		}
		return nullptr;
	}

	RunReport Runner::Run(const RunInputs &inputs) {
		const uint64_t runNumber = ++RunCounter;
		const int64_t runStart = NowNanoseconds();
		const size_t nodeCount = Compiled.Nodes.size();
		RunReport report;
		report.RunNumber = runNumber;
		report.Nodes.resize(nodeCount);
		TimelineRun timeline{.RunNumber = runNumber};
		const RunInfo runInfo{.RunNumber = runNumber, .Runs = Compiled.Runs, .NodeCount = nodeCount};
		if (HookSet.PreRun) {
			HookSet.PreRun(runInfo);
		}

		const auto sample =
			[&](uint32_t node, int64_t begin, int64_t end, uint64_t thread, NodeStatus outcome) {
				timeline.Samples.push_back(
					{.Node = node,
					 .BeginNanoseconds = begin - runStart,
					 .EndNanoseconds = end - runStart,
					 .Thread = thread,
					 .Outcome = outcome}
				);
			};

		// Stores a finished evaluation, or refuses it when the budget cannot hold it.
		const auto commit = [&](uint32_t node, ComputeResult &result, uint64_t runHash, bool cacheable) {
			Slot &slot = Slots[node];
			NodeReport &entry = report.Nodes[node];
			Release(slot);
			if (result.Status != NodeStatus::Ran && result.Status != NodeStatus::Bypassed) {
				entry.Status = result.Status;
				entry.Note = std::move(result.Note);
				return;
			}
			size_t bytes = 0;
			const PlannedNode &planned = Compiled.Nodes[node];
			for (size_t output = 0; output < result.Outputs.size(); ++output) {
				const Value &value = result.Outputs[output];
				const bool sized = output < planned.OutputBytes.size() && planned.OutputBytes[output];
				bytes += sized ? planned.OutputBytes[output](value) : value.Bytes();
			}
			if (!MakeRoom(bytes)) {
				entry.Status = NodeStatus::Budget;
				entry.Note = "outputs need " + std::to_string(bytes) + " bytes over the graph budget";
				return;
			}
			slot.Outputs = std::move(result.Outputs);
			slot.Bytes = bytes;
			slot.Peak = std::max(slot.Peak, bytes);
			slot.Valid = true;
			slot.Cacheable = cacheable;
			slot.RunHash = runHash;
			slot.LastUsed = ++TouchCounter;
			Retained += bytes;
			report.ProducedBytes += bytes;
			report.PeakBytes = std::max(report.PeakBytes, Retained);
			entry.Status = result.Status;
			entry.Note = std::move(result.Note);
			entry.Bytes = bytes;
		};

		PendingConsumers.assign(nodeCount, 0);
		for (const PlannedNode &planned : Compiled.Nodes) {
			for (const PlannedInput &input : planned.Inputs) {
				for (const PlannedSource &source : input.Sources) {
					++PendingConsumers[source.Node];
				}
			}
		}
		std::vector<Pending> parallelBatch;
		for (const std::vector<uint32_t> &wave : Compiled.Waves) {
			parallelBatch.clear();
			for (const uint32_t node : wave) {
				const PlannedNode &planned = Compiled.Nodes[node];
				Slot &slot = Slots[node];
				NodeReport &entry = report.Nodes[node];
				const NodeInfo info{
					.Id = planned.Id,
					.Def = planned.DefId,
					.Index = node,
					.Wave = planned.Wave,
					.Mode = planned.Mode,
					.RunNumber = runNumber,
				};
				const int64_t visitBegin = NowNanoseconds();
				// PreNode fires once, before any work, on the thread that does it.
				bool announced = false;
				const auto announce = [&] {
					if (!announced && HookSet.PreNode) {
						HookSet.PreNode(info);
					}
					announced = true;
				};
				const auto finish = [&](NodeStatus status, std::string note = {}) {
					announce();
					entry.Status = status;
					if (!note.empty()) {
						entry.Note = std::move(note);
					}
					if (status != NodeStatus::Cached && status != NodeStatus::Waiting) {
						if (status != NodeStatus::Ran && status != NodeStatus::Bypassed) {
							Release(slot);
						}
					}
					const int64_t end = NowNanoseconds();
					entry.Milliseconds = Milliseconds(visitBegin, end);
					sample(node, visitBegin, end, ThreadTag(), status);
					if (HookSet.PostNode) {
						HookSet.PostNode(info, status);
					}
				};
				slot.LastUsed = ++TouchCounter;

				// Gather inputs and the run hash that keys the cache.
				bool waiting = false;
				std::string missing;
				bool cacheable = planned.Def != nullptr && planned.Def->Pure == Purity::Pure;
				uint64_t runHash = planned.Hash;
				std::vector<std::vector<Value>> values(planned.Inputs.size());
				const auto supplied = inputs.find(planned.Id);
				for (size_t input = 0; input < planned.Inputs.size(); ++input) {
					const PlannedInput &port = planned.Inputs[input];
					for (const PlannedSource &source : port.Sources) {
						const NodeStatus upstream = report.Nodes[source.Node].Status;
						--PendingConsumers[source.Node];
						Slots[source.Node].LastUsed = ++TouchCounter;
						if (upstream == NodeStatus::Waiting) {
							waiting = true;
							continue;
						}
						const Slot &from = Slots[source.Node];
						if (!from.Valid || source.Output >= from.Outputs.size() ||
							from.Outputs[source.Output].Empty()) {
							continue;
						}
						const Value &raw = from.Outputs[source.Output];
						values[input].push_back(source.Convert ? source.Convert(raw) : raw);
						runHash = HashMix(runHash, RunHashes[source.Node]);
					}
					if (!port.Sources.empty()) {
						if (values[input].empty() && !port.Optional && port.Default.Empty() &&
							missing.empty()) {
							missing = port.Port;
						}
						if (values[input].empty() && !port.Default.Empty()) {
							values[input].push_back(port.Default);
						}
						continue;
					}
					const Value *given = nullptr;
					if (supplied != inputs.end()) {
						if (auto found = supplied->second.find(port.Port); found != supplied->second.end()) {
							given = &found->second;
						}
					}
					if (given != nullptr) {
						Value accepted;
						if (!Accepts(port.Type, *given, accepted)) {
							missing.clear();
							values.clear();
							entry.Note = "input '" + port.Port + "' expects '" + port.Type + "', got '" +
										 given->TypeName() + "'";
							break;
						}
						runHash = HashMix(HashText(runHash, port.Port), accepted.Hash());
						cacheable = cacheable && !accepted.Opaque();
						values[input].push_back(std::move(accepted));
					} else if (!port.Default.Empty()) {
						values[input].push_back(port.Default);
					} else if (!port.Optional && missing.empty()) {
						missing = port.Port;
					}
				}
				for (const auto &list : values) {
					for (const Value &value : list) {
						cacheable = cacheable && !value.Opaque();
					}
				}
				if (planned.Def != nullptr && planned.Def->Pure == Purity::Impure) {
					runHash = HashMix(runHash, ++ImpureNonce);
				}
				RunHashes[node] = runHash;

				if (values.size() != planned.Inputs.size()) {
					finish(NodeStatus::Failed);
					continue;
				}
				if (planned.Def == nullptr) {
					finish(NodeStatus::Skipped, "unknown node type '" + planned.DefId + "'");
					continue;
				}
				if (waiting) {
					finish(NodeStatus::Waiting, "an input is still computing");
					continue;
				}
				if (!missing.empty()) {
					finish(NodeStatus::Skipped, "input '" + missing + "' has no value");
					continue;
				}
				if (planned.Bypassed) {
					ComputeResult forwarded{.Status = NodeStatus::Bypassed};
					forwarded.Outputs.resize(planned.OutputNames.size());
					for (const BypassRoute &route : planned.Def->Bypass) {
						const int64_t from = PortIndex(planned.Def->Inputs, route.Input);
						const int64_t to = PortIndex(planned.Def->Outputs, route.Output);
						if (from >= 0 && to >= 0 && !values[static_cast<size_t>(from)].empty()) {
							forwarded.Outputs[static_cast<size_t>(to)] =
								values[static_cast<size_t>(from)].front();
						}
					}
					commit(node, forwarded, runHash, false);
					finish(entry.Status);
					continue;
				}
				if (!planned.Def->Evaluate) {
					finish(NodeStatus::Skipped, "node type '" + planned.DefId + "' has no evaluator");
					continue;
				}
				if (cacheable && slot.Valid && slot.Cacheable && slot.RunHash == runHash) {
					finish(NodeStatus::Cached);
					continue;
				}

				if (planned.Mode == ExecMode::Async) {
					if (slot.Job != nullptr && slot.Job->RunHash != runHash) {
						slot.Job->Cancel->store(true);
						slot.Job.reset();
					}
					if (slot.Job != nullptr) {
						if (!slot.Job->Done.load(std::memory_order_acquire)) {
							finish(NodeStatus::Waiting, "async work in flight");
							continue;
						}
						announce();
						std::shared_ptr<AsyncJob> job = std::move(slot.Job);
						const double milliseconds = Milliseconds(job->BeginNanoseconds, job->EndNanoseconds);
						core::FrameGraph::ReportNamed(
							"ang.node", planned.Id, Options.Category, static_cast<float>(milliseconds)
						);
						sample(
							node, job->BeginNanoseconds, job->EndNanoseconds, job->Thread, job->Result.Status
						);
						commit(node, job->Result, runHash, cacheable);
						entry.Milliseconds = milliseconds;
						if (HookSet.PostNode) {
							HookSet.PostNode(info, entry.Status);
						}
						continue;
					}
					auto job = std::make_shared<AsyncJob>();
					job->RunHash = runHash;
					AsyncPool &pool = Options.Pool != nullptr ? *Options.Pool : AsyncPool::Shared();
					EvalContext context =
						RunnerAccess::Make(planned, Compiled.Runs, std::move(values), job->Cancel);
					const bool queued =
						pool.Submit([job, context = std::move(context), hooks = HookSet, info]() mutable {
							job->Thread = ThreadTag();
							job->BeginNanoseconds = NowNanoseconds();
							if (!job->Cancel->load()) {
								job->Result = RunnerAccess::Evaluate(hooks, info, context);
							} else {
								job->Result.Status = NodeStatus::Cancelled;
							}
							job->EndNanoseconds = NowNanoseconds();
							job->Done.store(true, std::memory_order_release);
						});
					if (queued) {
						slot.Job = std::move(job);
					}
					finish(NodeStatus::Waiting, queued ? "async work queued" : "async queue is full");
					continue;
				}

				EvalContext context = RunnerAccess::Make(planned, Compiled.Runs, std::move(values), nullptr);
				if (planned.Mode == ExecMode::Parallel) {
					parallelBatch.push_back(
						{.Node = node,
						 .Info = info,
						 .Context = std::move(context),
						 .RunHash = runHash,
						 .Cacheable = cacheable}
					);
					continue;
				}
				announce();
				ComputeResult result;
				{
					ENGINE_PROFILE_DYNAMIC("ang.node", std::string_view(planned.Id), Options.Category);
					result = RunnerAccess::Evaluate(HookSet, info, context);
				}
				commit(node, result, runHash, cacheable);
				finish(entry.Status);
			}

			if (parallelBatch.empty()) {
				continue;
			}
			// Workers only evaluate. Commit, budget and reporting stay on this thread
			// in wave order, so the result is the same however the batch was split.
			parallel::Jobs::For(
				parallelBatch.size(),
				1,
				[&](size_t begin, size_t end) {
					for (size_t index = begin; index < end; ++index) {
						Pending &pending = parallelBatch[index];
						if (HookSet.PreNode) {
							HookSet.PreNode(pending.Info);
						}
						pending.Thread = ThreadTag();
						pending.BeginNanoseconds = NowNanoseconds();
						pending.Result = RunnerAccess::Evaluate(HookSet, pending.Info, pending.Context);
						pending.EndNanoseconds = NowNanoseconds();
						if (HookSet.PostNode) {
							HookSet.PostNode(pending.Info, pending.Result.Status);
						}
					}
				},
				Options.ParallelMinimum
			);
			for (Pending &pending : parallelBatch) {
				const double milliseconds = Milliseconds(pending.BeginNanoseconds, pending.EndNanoseconds);
				core::FrameGraph::ReportNamed(
					"ang.node",
					Compiled.Nodes[pending.Node].Id,
					Options.Category,
					static_cast<float>(milliseconds)
				);
				const NodeStatus status = pending.Result.Status;
				commit(pending.Node, pending.Result, pending.RunHash, pending.Cacheable);
				report.Nodes[pending.Node].Milliseconds = milliseconds;
				sample(
					pending.Node, pending.BeginNanoseconds, pending.EndNanoseconds, pending.Thread, status
				);
			}
		}

		for (const NodeReport &entry : report.Nodes) {
			switch (entry.Status) {
			case NodeStatus::Ran:
				++report.Ran;
				break;
			case NodeStatus::Cached:
				++report.Cached;
				break;
			case NodeStatus::Skipped:
				++report.Skipped;
				break;
			case NodeStatus::Waiting:
				++report.Waiting;
				break;
			case NodeStatus::Failed:
				++report.Failed;
				break;
			case NodeStatus::Bypassed:
				++report.Bypassed;
				break;
			case NodeStatus::Budget:
				++report.Budget;
				break;
			case NodeStatus::Cancelled:
				++report.Cancelled;
				break;
			case NodeStatus::Idle:
				break;
			}
		}
		for (size_t node = 0; node < nodeCount; ++node) {
			if (report.Nodes[node].Status == NodeStatus::Cached) {
				report.Nodes[node].Bytes = Slots[node].Bytes;
			}
		}
		report.Complete = report.Waiting == 0;
		report.RetainedBytes = Retained;
		report.PeakBytes = std::max(report.PeakBytes, Retained);
		report.Milliseconds = Milliseconds(runStart, NowNanoseconds());
		timeline.Milliseconds = report.Milliseconds;
		History.Push(std::move(timeline));
		if (HookSet.PostRun) {
			RunInfo post = runInfo;
			post.Report = &report;
			HookSet.PostRun(post);
		}
		return report;
	}
}
