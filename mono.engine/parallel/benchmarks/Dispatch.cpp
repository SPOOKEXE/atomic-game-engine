// Measure dispatch overhead with empty bodies.

#include "ThreadAffinity.hpp"

#include <engine/core/FrameGraph.hpp>
#include <engine/core/HeapProfile.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/testing/Bench.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

TEST_SUITE_ID("engine.parallel.bench.dispatch")

using engine::parallel::JobContext;
using engine::parallel::Jobs;
using engine::testing::Consume;

namespace dispatch_bench {
	// Keep pool construction out of measured bodies.
	struct Pool {
		Pool() {
			Jobs::Start(0);
		}
		~Pool() {
			Jobs::Stop();
		}
	};
	const Pool Workers;

	void Nothing(size_t begin, size_t end) {
		Consume(begin);
		Consume(end);
	}
}

using namespace dispatch_bench;

BENCH("For · below the floor, so inline", 10'000) {
	for (int pass = 0; pass < 10'000; pass++) {
		Jobs::For(1024, 1024, [](size_t begin, size_t end) { Nothing(begin, end); });
	}
}

BENCH("For · explicit Serial context", 10'000) {
	for (int pass = 0; pass < 10'000; pass++) {
		Jobs::For(JobContext::Serial, 1024, 1024, [](size_t begin, size_t end) { Nothing(begin, end); });
	}
}

BENCH("For · explicit Threaded context below the floor", 10'000) {
	for (int pass = 0; pass < 10'000; pass++) {
		Jobs::For(JobContext::Threaded, 1024, 1024, [](size_t begin, size_t end) { Nothing(begin, end); });
	}
}

BENCH("For · dispatched, 8 empty ranges", 2000) {
	// This is the measured minimum dispatch floor.
	for (int pass = 0; pass < 2000; pass++) {
		Jobs::For(8 * 1024, 1024, [](size_t begin, size_t end) { Nothing(begin, end); });
	}
}

BENCH("For · dispatched, 128 empty ranges", 2000) {
	for (int pass = 0; pass < 2000; pass++) {
		Jobs::For(128 * 1024, 1024, [](size_t begin, size_t end) { Nothing(begin, end); });
	}
}

BENCH("For · dispatched, 8 empty ranges, one worker", 20'000) {
	// Isolate single-worker handover cost from pool wake-up cost.
	Jobs::Stop();
	Jobs::Start(1);
	for (int pass = 0; pass < 20'000; pass++) {
		Jobs::For(8 * 1024, 1024, [](size_t begin, size_t end) { Nothing(begin, end); });
	}
	Jobs::Stop();
	Jobs::Start(0);
}

// The two rows below separate "linear in the ranges" from "linear in the
// workers actually woken", which the three rows above cannot: `For` wakes
// `ranges - 1` workers up to the pool size, so every case above varies both at
// once and two points fit either line.

BENCH("For · dispatched, 1024 empty ranges", 2000) {
	// Eight times the ranges of the case above and the same pool, so the woken
	// count is pinned at the pool size and only the range count moves.
	for (int pass = 0; pass < 2000; pass++) {
		Jobs::For(1024 * 1024, 1024, [](size_t begin, size_t end) { Nothing(begin, end); });
	}
}

BENCH("For · dispatched, 128 empty ranges, four workers", 5000) {
	// The other half: the same ranges as the 128-range case over a pool that
	// can only wake four of them.
	Jobs::Stop();
	Jobs::Start(4);
	for (int pass = 0; pass < 5000; pass++) {
		Jobs::For(128 * 1024, 1024, [](size_t begin, size_t end) { Nothing(begin, end); });
	}
	Jobs::Stop();
	Jobs::Start(0);
}

// --- pinned placement dispatch -------------------------------------------------
//
// Each signaled worker scans the whole task list and skips tasks assigned to
// others, so these rows separate the handover from that scan. With no pinned
// workers the span runs inline and the rows measure the fallback instead.

namespace dispatch_bench {
	// Check placement and complete deterministic task output before measured empty dispatches.
	void VerifyAssignment(const std::vector<unsigned> &assignment) {
		const unsigned pinned = Jobs::PinnedWorkerCount();
		if (pinned == 0) throw std::runtime_error("pinned dispatch fixture unavailable: no pinned workers");
		const auto cores = engine::parallel::platform::DistinctCoreProcessors();
		struct Visit {
			size_t Index;
			engine::parallel::platform::Processor Processor;
			std::thread::id Thread;
			uint64_t Output;
		};
		std::vector<Visit> visits;
		visits.reserve(assignment.size());
		std::mutex guard;
		Jobs::ForWorkers(assignment, [&](size_t begin, size_t end) {
			for (size_t index = begin; index < end; ++index) {
				const Visit visit{
					index,
					engine::parallel::platform::CurrentProcessor(),
					std::this_thread::get_id(),
					(static_cast<uint64_t>(index) + 1) * 17 + assignment[index]
				};
				std::lock_guard lock(guard);
				visits.push_back(visit);
			}
		});
		if (visits.size() != assignment.size()) throw std::runtime_error("pinned task count differs");
		std::vector<unsigned> seen(assignment.size());
		std::vector<size_t> previous(pinned, assignment.size());
		std::vector<std::thread::id> threads(pinned);
		unsigned participants = 0;
		for (const Visit &visit : visits) {
			if (visit.Index >= assignment.size() || ++seen[visit.Index] != 1)
				throw std::runtime_error("pinned task not visited exactly once");
			const unsigned worker = assignment[visit.Index];
			if (worker >= cores.size() || visit.Processor != cores[worker] ||
				visit.Output != (static_cast<uint64_t>(visit.Index) + 1) * 17 + worker)
				throw std::runtime_error("pinned task placement or output differs");
			if (previous[worker] != assignment.size() && previous[worker] >= visit.Index)
				throw std::runtime_error("pinned worker task order differs");
			if (threads[worker] == std::thread::id{}) {
				threads[worker] = visit.Thread;
				++participants;
			} else if (threads[worker] != visit.Thread)
				throw std::runtime_error("pinned worker thread changed");
			previous[worker] = visit.Index;
		}
		for (size_t left = 0; left < threads.size(); ++left) {
			if (threads[left] == std::thread::id{}) continue;
			for (size_t right = left + 1; right < threads.size(); ++right) {
				if (threads[left] == threads[right])
					throw std::runtime_error("distinct pinned workers shared a thread");
			}
		}
		if (Jobs::LastBatch().Participants != participants)
			throw std::runtime_error("pinned participant count differs");
	}

	struct AssignedCapture {
		bool Enabled = false;
		bool PreviouslyEnabled = false;
		size_t Frames = 0;
		size_t Spans = 0;
		size_t Entered = 0;
		double Owner = 0;
		double OwnerSelf = 0;
		double Join = 0;
		double Producer = 0;
		double ProducerSelf = 0;
		double Body = 0;
		double Unmarked = 0;
		double SignaledBefore = 0;
		engine::core::HeapTotals Before;

		AssignedCapture() {
			static const bool requested = [] {
				const char *value = std::getenv("ATOMIC_PINNED_DISPATCH_PROFILE");
				return value != nullptr && std::string_view(value) == "1";
			}();
			Enabled = requested;
			if (!Enabled) return;
			if (Jobs::WorkerCount() > 64 || Jobs::PinnedWorkerCount() == 0)
				throw std::runtime_error("pinned capture requires 1..64 workers and a pinned prefix");
			Before = engine::core::HeapProfile::Totals();
			PreviouslyEnabled = engine::core::FrameGraph::IsEnabled();
			engine::core::FrameGraph::SetEnabled(true);
			const auto signaled = engine::core::Metrics::Get("jobs.signaled_workers");
			SignaledBefore = signaled ? signaled->Value : 0;
		}
		~AssignedCapture() {
			if (Enabled) engine::core::FrameGraph::SetEnabled(PreviouslyEnabled);
		}
		void Dispatch(const std::vector<unsigned> &assignment) {
			using engine::core::FrameGraph;
			if (!Enabled) {
				Jobs::ForWorkers(assignment, [](size_t begin, size_t end) { Nothing(begin, end); });
				return;
			}
			if (Frames >= 2000) throw std::runtime_error("pinned capture frame capacity exceeded");
			FrameGraph::BeginFrame();
			try {
				Jobs::ForWorkers(assignment, [](size_t begin, size_t end) { Nothing(begin, end); });
			} catch (...) {
				// Close the owner frame before propagating a failed dispatch.
				FrameGraph::EndFrame();
				throw;
			}
			FrameGraph::EndFrame();
			if (FrameGraph::Dropped() != 0) throw std::runtime_error("pinned capture dropped spans");
			size_t owners = 0, joins = 0, workers = 0, bodies = 0;
			const auto &spans = FrameGraph::Spans();
			for (size_t index = 0; index < spans.size(); ++index) {
				const auto &span = spans[index];
				if (span.Parent == FrameGraph::NO_PARENT) {
					if (span.Depth != 0) throw std::runtime_error("pinned capture root depth differs");
				} else if (span.Parent >= index || span.Depth != spans[span.Parent].Depth + 1) {
					throw std::runtime_error("pinned capture hierarchy differs");
				}
				if (span.Name == "jobs.assigned") {
					++owners;
					Owner += span.Milliseconds;
					OwnerSelf += span.SelfMilliseconds;
				}
				if (span.Name == "jobs.join.assigned") {
					++joins;
					Join += span.Milliseconds;
				}
				if (span.Name == "jobs.assigned.worker") {
					if (!span.Reported || span.Parent == FrameGraph::NO_PARENT ||
						spans[span.Parent].Name != "jobs.assigned")
						throw std::runtime_error("pinned producer parent or reported mark differs");
					++workers;
					Producer += span.Milliseconds;
					ProducerSelf += span.SelfMilliseconds;
				}
				if (span.Name == "jobs.assigned.body") {
					if (!span.Reported || span.Parent == FrameGraph::NO_PARENT ||
						spans[span.Parent].Name != "jobs.assigned.worker")
						throw std::runtime_error("pinned body parent or reported mark differs");
					++bodies;
					Body += span.Milliseconds;
				}
			}
			if (owners != 1 || joins != 1 || workers != bodies ||
				workers != std::min<size_t>(Jobs::PinnedWorkerCount(), assignment.size()) ||
				spans.size() != 2 + 2 * workers)
				throw std::runtime_error("pinned capture missing or extra phases");
			++Frames;
			Spans += spans.size();
			Entered += workers;
			Unmarked += FrameGraph::UnmarkedMilliseconds();
		}
		void Print(size_t tasks) const {
			if (!Enabled) return;
			static size_t totalBatches = 0;
			static size_t calls2 = 0, calls64 = 0, calls1024 = 0;
			const size_t call = tasks == 2 ? ++calls2 : tasks == 64 ? ++calls64 : ++calls1024;
			if (++totalBatches > 128) throw std::runtime_error("pinned capture batch capacity exceeded");
			const auto after = engine::core::HeapProfile::Totals();
			const auto signaled = engine::core::Metrics::Get("jobs.signaled_workers");
			if (after.DroppedScopes != Before.DroppedScopes)
				throw std::runtime_error("pinned capture dropped heap scopes");
			int64_t retained = 0, blocks = 0;
			const uint32_t nodes = engine::core::HeapProfile::NodeCount();
			if (nodes > 4096) throw std::runtime_error("pinned capture heap node capacity exceeded");
			for (uint32_t index = 0; index < nodes; ++index) {
				const auto node = engine::core::HeapProfile::Node(index);
				if (node.Name == "jobs.assigned.readings") {
					retained += node.LiveBytes;
					blocks += node.LiveBlocks;
				}
			}
			std::cout << "PINNED_PROFILE tasks=" << tasks << " call=" << call
					  << " benchmark_warmup=" << (call <= 8) << " workers=" << Jobs::WorkerCount()
					  << " pinned=" << Jobs::PinnedWorkerCount() << " frames=" << Frames << " spans=" << Spans
					  << " entered_workers=" << Entered
					  << " signaled_workers=" << (signaled ? signaled->Value - SignaledBefore : 0)
					  << " frame_drops=0 heap_drop_delta=0 owner_inclusive_ms=" << Owner
					  << " owner_self_ms=" << OwnerSelf << " join_idle_ms=" << Join
					  << " producer_inclusive_ms=" << Producer
					  << " producer_scan_retirement_timer_self_ms=" << ProducerSelf
					  << " producer_body_ms=" << Body << " unmarked_ms=" << Unmarked
					  << " diagnostic_retained_cxx_bytes=" << retained
					  << " diagnostic_retained_blocks=" << blocks
					  << " process_cxx_allocated_bytes=" << after.TotalBytes - Before.TotalBytes
					  << " process_cxx_allocated_blocks=" << after.TotalBlocks - Before.TotalBlocks
					  << " process_live_bytes_before=" << Before.LiveBytes
					  << " process_live_bytes_after=" << after.LiveBytes
					  << " profiler_overhead_bytes=" << after.OverheadBytes << '\n';
		}
	};

	// Round-robin task placement over the pinned prefix, rebuilt per row.
	const std::vector<unsigned> &Spread(size_t tasks) {
		static std::vector<unsigned> assignment;
		const unsigned pinned = Jobs::PinnedWorkerCount();
		assignment.resize(tasks);
		for (size_t task = 0; task < tasks; task++) {
			assignment[task] = pinned == 0 ? 0 : static_cast<unsigned>(task % pinned);
		}
		static bool checked2 = false, checked64 = false, checked1024 = false;
		bool &checked = tasks == 2 ? checked2 : tasks == 64 ? checked64 : checked1024;
		if (!checked) {
			VerifyAssignment(assignment);
			std::vector<unsigned> skewed(tasks, 0);
			VerifyAssignment(skewed);
			std::vector<unsigned> irregular(tasks);
			for (size_t index = 0; index < tasks; ++index) {
				irregular[index] = static_cast<unsigned>((index * index + 3 * index + 7) % pinned);
			}
			VerifyAssignment(irregular);
			checked = true;
		}
		return assignment;
	}
}

BENCH("ForWorkers · 2 empty pinned tasks", 2000) {
	const std::vector<unsigned> &assignment = Spread(2);
	AssignedCapture capture;
	for (int pass = 0; pass < 2000; pass++)
		capture.Dispatch(assignment);
	capture.Print(2);
}

BENCH("ForWorkers · 64 empty pinned tasks", 2000) {
	const std::vector<unsigned> &assignment = Spread(64);
	AssignedCapture capture;
	for (int pass = 0; pass < 2000; pass++)
		capture.Dispatch(assignment);
	capture.Print(64);
}

BENCH("ForWorkers · 1024 empty pinned tasks", 500) {
	// Sixteen times the tasks over the same workers: growth here beyond the
	// per-task body is the per-worker scan of every other worker's tasks.
	const std::vector<unsigned> &assignment = Spread(1024);
	AssignedCapture capture;
	for (int pass = 0; pass < 500; pass++)
		capture.Dispatch(assignment);
	capture.Print(1024);
}
