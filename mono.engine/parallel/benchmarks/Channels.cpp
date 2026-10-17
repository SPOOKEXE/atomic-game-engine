// The framed queue two worlds talk through, at the sizes they talk at.
//
// **This is the seam that decides whether thread-per-world and
// process-per-world stay the same design.** Everything crossing a world
// boundary is already bytes, so the only difference between two worlds in one
// process and two in two processes is what carries them. A channel that costs
// more than the work it carries would push callers into sharing memory instead,
// and the two arrangements would stop being interchangeable - quietly, one
// caller at a time.
//
// **Three frame sizes, because they are three different questions.** Sixty-four
// bytes is a bus message and its cost is entirely per-frame overhead: the
// mutex, the queue node, the length prefix. Four kibibytes is an envelope, and
// somewhere between the two the copy starts to matter. Two hundred and
// fifty-six kibibytes is a snapshot chunk, where the figure should be memory
// bandwidth and nothing else - a row that is materially worse than a `memcpy`
// of the same size is a frame being copied more than once.
//
// **The refusal paths are measured too, and that is deliberate.** A full
// channel refuses rather than blocking, which means a producer that has outrun
// its consumer calls `Send` and gets `Full` on *every* attempt until the
// consumer catches up. That is the hot path of an overloaded host, so a refusal
// that were expensive would make the overloaded case worse in exactly the
// moment it could least afford it.
//
// **One row uses two threads and it is the only one whose spread should be
// read.** Everything else here is single-threaded and reproducible; the
// contended row depends on how the scheduler feels, and the minimum sample is a
// lower bound on the contended cost rather than the contended cost. It is here
// because cross-thread is what a channel is *for*, and a suite that only ever
// measured the uncontended mutex would be measuring the case that never
// happens.

#include "../tests/fixtures/ChannelOracle.hpp"

#include <engine/core/Clock.hpp>
#include <engine/core/FrameGraph.hpp>
#include <engine/core/HeapProfile.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/parallel/Channel.hpp>
#include <engine/testing/Bench.hpp>

#include <array>
#include <atomic>
#include <cinttypes>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

TEST_SUITE_ID("engine.parallel.bench.channel")

using engine::parallel::Channel;
using engine::parallel::ChannelSettings;
using engine::parallel::ChannelStatus;
using engine::parallel::MakeLocalChannel;
using engine::testing::Consume;

namespace channel_bench {
	// A bus message, an envelope, and a snapshot chunk.
	constexpr size_t SMALL_BYTES = 64;
	constexpr size_t MEDIUM_BYTES = 4 * 1024;
	constexpr size_t LARGE_BYTES = 256 * 1024;

	// Frames per sample for the round-trip rows. Enough that the pair
	// construction outside the body rounds to nothing.
	constexpr size_t FRAMES = 20'000;

	// Bytes of content, shared by every row so that no row pays for the first
	// touch of its own buffer.
	std::span<const std::byte> Payload(size_t bytes) {
		static std::vector<std::byte> content = [] {
			std::vector<std::byte> values(LARGE_BYTES);
			for (size_t index = 0; index < values.size(); index++) {
				values[index] = static_cast<std::byte>(index & 0xFF);
			}
			return values;
		}();
		return std::span<const std::byte>(content).first(bytes);
	}

	// One connected pair, kept across samples.
	//
	// **Reused rather than rebuilt, and drained before every sample.** Building
	// a pair allocates two queues; leaving frames in one would make the next
	// sample start against a queue that is already part-full, which is a
	// different measurement wearing the same name.
	struct Pair {
		std::unique_ptr<Channel> Near;
		std::unique_ptr<Channel> Far;

		explicit Pair(const ChannelSettings &settings = {}) {
			auto ends = MakeLocalChannel(settings);
			Near = std::move(ends.first);
			Far = std::move(ends.second);
		}

		void Drain() {
			std::vector<std::byte> frame;
			while (Far->Receive(frame) == ChannelStatus::Ok) {}
			while (Near->Receive(frame) == ChannelStatus::Ok) {}
		}
	};

	Pair &Wide() {
		// Sixty-four megabytes of capacity, which is the default and is what a
		// caller that has not thought about it gets.
		static Pair pair;
		pair.Drain();
		return pair;
	}

	// Sends and receives one frame, which is what a tick barrier does per
	// message. Both halves in one row because a send whose frame is never taken
	// measures a queue that only ever grows.
	size_t RoundTrip(Pair &pair, size_t bytes, std::vector<std::byte> &scratch) {
		if (pair.Near->Send(Payload(bytes)) != ChannelStatus::Ok) {
			return 0;
		}
		if (pair.Far->Receive(scratch) != ChannelStatus::Ok) {
			return 0;
		}
		return scratch.size();
	}
	bool ProfileEnabled() {
		static const bool enabled = [] {
			const char *value = std::getenv("ATOMIC_LOCAL_CHANNEL_PROFILE");
			return value != nullptr && std::string_view(value) == "1";
		}();
		return enabled;
	}
	constexpr std::string_view BATCH_SCOPE = "local channel batch";
	constexpr std::string_view SEND_SCOPE = "local channel send";
	constexpr std::string_view RECEIVE_SCOPE = "local channel receive";
	constexpr std::string_view JOIN_SCOPE = "local channel join";
	constexpr std::string_view CONSUMER_SCOPE = "local channel consumer";
	struct ChannelHeap {
		engine::core::HeapNodeView Send;
		engine::core::HeapNodeView Receive;
		engine::core::HeapNodeView Batch;
	};
	void Add(engine::core::HeapNodeView &total, const engine::core::HeapNodeView &node) {
		total.TotalBytes += node.TotalBytes;
		total.TotalBlocks += node.TotalBlocks;
		total.LiveBytes += node.LiveBytes;
		total.LiveBlocks += node.LiveBlocks;
		total.PeakBytes += node.PeakBytes;
	}
	ChannelHeap ReadHeap() {
		using engine::core::HeapProfile;
		const uint32_t count = HeapProfile::NodeCount();
		if (count > 4096) throw std::runtime_error("channel heap tree exceeds diagnostic bound");
		ChannelHeap result;
		// Read all fixed-name paths: the joined consumer's heap tags belong to
		// its thread, not to the frame owner's batch parent.
		for (uint32_t index = 1; index < count; ++index) {
			const auto node = HeapProfile::Node(index);
			if (node.Name == SEND_SCOPE)
				Add(result.Send, node);
			else if (node.Name == RECEIVE_SCOPE)
				Add(result.Receive, node);
			else if (node.Name == BATCH_SCOPE)
				Add(result.Batch, node);
		}
		return result;
	}
	struct RestoreChannelProfile {
		bool Enabled = engine::core::FrameGraph::IsEnabled();
		~RestoreChannelProfile() {
			engine::core::FrameGraph::SetEnabled(Enabled);
		}
	};
	void Preflight() {
		static const bool verified = [] {
			channel_fixture::Verify();
			return true;
		}();
		(void)verified;
	}
	template <class Body> void ProfileBatch(size_t row, bool threaded, Body &&body) {
		Preflight();
		if (!ProfileEnabled()) {
			body();
			return;
		}
		using engine::core::FrameGraph;
		using engine::core::HeapProfile;
		using engine::core::ProfileCategory;
		// One frame per invocation, consumed immediately, including every warmup.
		// The history ring is not evidence of retaining all nine row sequences.
		// Each BENCH body has a distinct type, so this bound belongs to one row.
		static size_t calls = 0;
		if (!HeapProfile::IsCompiledIn() || row >= 9 || calls == 128)
			throw std::runtime_error("channel capture requires heap hooks and at most 128 calls per row");
		const RestoreChannelProfile restore;
		FrameGraph::SetEnabled(true);
		const auto before = ReadHeap();
		const auto totalsBefore = HeapProfile::Totals();
		FrameGraph::BeginFrame();
		try {
			ENGINE_PROFILE("local channel batch");
			body();
		} catch (...) {
			// Close the owner frame on refusal before restoring collector state.
			FrameGraph::EndFrame();
			throw;
		}
		FrameGraph::EndFrame();
		const auto &spans = FrameGraph::Spans();
		if (FrameGraph::Dropped() != 0 || spans.size() != (threaded ? 3u : 1u) ||
			spans.front().Name != BATCH_SCOPE || spans.front().Depth != 0 ||
			spans.front().Parent != FrameGraph::NO_PARENT || spans.front().Reported)
			throw std::runtime_error("channel capture incomplete owner hierarchy or dropped spans");
		float idle = 0, consumer = 0;
		if (threaded) {
			const auto &joined = spans[1];
			const auto &reported = spans[2];
			if (joined.Name != JOIN_SCOPE || joined.Parent != 0 || joined.Depth != 1 ||
				joined.Category != ProfileCategory::Idle || joined.Reported ||
				reported.Name != CONSUMER_SCOPE || reported.Parent != 0 || reported.Depth != 1 ||
				!reported.Reported || reported.Category != ProfileCategory::Engine)
				throw std::runtime_error("channel capture missing joined producer hierarchy");
			idle = joined.Milliseconds;
			consumer = reported.Milliseconds;
		}
		const auto after = ReadHeap();
		const auto totalsAfter = HeapProfile::Totals();
		if (totalsAfter.DroppedScopes != totalsBefore.DroppedScopes)
			throw std::runtime_error("channel capture dropped heap scopes");
		const uint64_t sendBytes = after.Send.TotalBytes - before.Send.TotalBytes;
		const uint64_t sendBlocks = after.Send.TotalBlocks - before.Send.TotalBlocks;
		const uint64_t receiveBytes = after.Receive.TotalBytes - before.Receive.TotalBytes;
		const uint64_t receiveBlocks = after.Receive.TotalBlocks - before.Receive.TotalBlocks;
		const uint64_t batchBytes = after.Batch.TotalBytes - before.Batch.TotalBytes;
		const uint64_t batchBlocks = after.Batch.TotalBlocks - before.Batch.TotalBlocks;
		const uint64_t processBytes = totalsAfter.TotalBytes - totalsBefore.TotalBytes;
		const uint64_t processBlocks = totalsAfter.TotalBlocks - totalsBefore.TotalBlocks;
		if (sendBytes + receiveBytes + batchBytes > processBytes ||
			sendBlocks + receiveBlocks + batchBlocks > processBlocks)
			throw std::runtime_error("channel exclusive heap counters exceed process allocation delta");
		const size_t call = ++calls;
		std::printf(
			"# channel-profile row=%zu call=%zu warmup=%d frames=1 spans=%zu "
			"frame_drops=0 heap_drop_delta=0 owner_inclusive_ms=%.6f "
			"owner_self_ms=%.6f join_idle_ms=%.6f consumer_reported_ms=%.6f "
			"frame_ms=%.6f unmarked_ms=%.6f heap_coverage=cxx_new_delete "
			"send_bytes=%" PRIu64 " send_blocks=%" PRIu64 " receive_bytes=%" PRIu64 " receive_blocks=%" PRIu64
			" batch_exclusive_bytes=%" PRIu64 " batch_exclusive_blocks=%" PRIu64
			" process_allocated_bytes=%" PRIu64 " process_allocated_blocks=%" PRIu64
			" unattributed_process_bytes=%" PRIu64 " unattributed_process_blocks=%" PRIu64
			" send_live_before=%" PRId64 " send_live_after=%" PRId64 " send_blocks_live_before=%" PRId64
			" send_blocks_live_after=%" PRId64 " receive_live_before=%" PRId64 " receive_live_after=%" PRId64
			" receive_blocks_live_before=%" PRId64 " receive_blocks_live_after=%" PRId64
			" send_sum_tag_peak_bytes=%" PRId64 " receive_sum_tag_peak_bytes=%" PRId64
			" process_live_before=%" PRId64 " process_live_after=%" PRId64
			" process_live_blocks_before=%" PRId64 " process_live_blocks_after=%" PRId64
			" profiler_overhead_bytes=%" PRId64 "\n",
			row,
			call,
			call <= 8,
			spans.size(),
			spans.front().Milliseconds,
			spans.front().SelfMilliseconds,
			idle,
			consumer,
			FrameGraph::FrameMilliseconds(),
			FrameGraph::UnmarkedMilliseconds(),
			sendBytes,
			sendBlocks,
			receiveBytes,
			receiveBlocks,
			batchBytes,
			batchBlocks,
			processBytes,
			processBlocks,
			processBytes - sendBytes - receiveBytes - batchBytes,
			processBlocks - sendBlocks - receiveBlocks - batchBlocks,
			before.Send.LiveBytes,
			after.Send.LiveBytes,
			before.Send.LiveBlocks,
			after.Send.LiveBlocks,
			before.Receive.LiveBytes,
			after.Receive.LiveBytes,
			before.Receive.LiveBlocks,
			after.Receive.LiveBlocks,
			after.Send.PeakBytes,
			after.Receive.PeakBytes,
			totalsBefore.LiveBytes,
			totalsAfter.LiveBytes,
			totalsBefore.LiveBlocks,
			totalsAfter.LiveBlocks,
			totalsAfter.OverheadBytes
		);
	}

} // namespace channel_bench

using namespace channel_bench;

// --- one frame at a time
// ------------------------------------------------------

BENCH("Send + Receive · 20k 64-byte frames", FRAMES) {
	ProfileBatch(0, false, [&] {
		Pair &pair = Wide();
		std::vector<std::byte> scratch;
		size_t moved = 0;
		for (size_t frame = 0; frame < FRAMES; frame++) {
			moved += RoundTrip(pair, SMALL_BYTES, scratch);
		}
		Consume(moved);
	});
}

BENCH("Send + Receive · 20k 4 KiB frames", FRAMES) {
	ProfileBatch(1, false, [&] {
		Pair &pair = Wide();
		std::vector<std::byte> scratch;
		size_t moved = 0;
		for (size_t frame = 0; frame < FRAMES; frame++) {
			moved += RoundTrip(pair, MEDIUM_BYTES, scratch);
		}
		Consume(moved);
	});
}

BENCH("Send + Receive · 2k 256 KiB frames", 2000) {
	ProfileBatch(2, false, [&] {
		// Half a gigabyte of copying. Read this one as bandwidth: divide 256 KiB by
		// the figure and compare against what the machine's memory does, because
		// anything materially short of that is a copy this code did not have to
		// make.
		Pair &pair = Wide();
		std::vector<std::byte> scratch;
		size_t moved = 0;
		for (size_t frame = 0; frame < 2000; frame++) {
			moved += RoundTrip(pair, LARGE_BYTES, scratch);
		}
		Consume(moved);
	});
}

// --- queued, then drained
// -----------------------------------------------------

BENCH("Send 10k then Receive 10k · 4 KiB frames", 10'000) {
	ProfileBatch(3, false, [&] {
		// **The shape a barrier-driven host actually produces**, rather than the
		// alternating one above: a world fills its outbound queue during its tick
		// and the driver drains it at the barrier. Against the alternating row, the
		// difference is a queue that stays hot in one cache against one that is
		// forty megabytes deep by the time anything reads it.
		Pair &pair = Wide();
		std::vector<std::byte> scratch;
		size_t moved = 0;
		for (size_t frame = 0; frame < 10'000; frame++) {
			Consume(pair.Near->Send(Payload(MEDIUM_BYTES)));
		}
		while (pair.Far->Receive(scratch) == ChannelStatus::Ok) {
			moved += scratch.size();
		}
		Consume(moved);
	});
}

// --- the refusals
// -------------------------------------------------------------

BENCH("Send · 20k refusals against a full channel", FRAMES) {
	ProfileBatch(4, false, [&] {
		// **The hot path of a host that is already in trouble.** A producer that
		// has outrun its consumer gets `Full` from every send until the consumer
		// catches up, so this figure is paid per attempt per tick for as long as
		// the overload lasts. It has to be cheap, and it has to be cheap *without*
		// touching the queue, which is what makes the byte cap a counter rather
		// than a walk.
		static Pair narrow(ChannelSettings{.MaximumFrame = MEDIUM_BYTES, .Capacity = 64 * 1024});
		while (narrow.Near->Send(Payload(SMALL_BYTES)) == ChannelStatus::Ok) {}

		size_t refused = 0;
		for (size_t attempt = 0; attempt < FRAMES; attempt++) {
			refused += narrow.Near->Send(Payload(SMALL_BYTES)) == ChannelStatus::Full ? 1 : 0;
		}
		Consume(refused);
	});
}

BENCH("Send · 20k frames over the maximum", FRAMES) {
	ProfileBatch(5, false, [&] {
		// Refused whole rather than truncated, and refused before anything is
		// copied - a size check that copied first would make an oversized frame
		// cost more than a legal one, which is the wrong way round for the case
		// that is usually a bug upstream sending in a loop.
		static Pair narrow(ChannelSettings{.MaximumFrame = SMALL_BYTES, .Capacity = 64 * 1024});
		size_t refused = 0;
		for (size_t attempt = 0; attempt < FRAMES; attempt++) {
			refused += narrow.Near->Send(Payload(MEDIUM_BYTES)) == ChannelStatus::TooLarge ? 1 : 0;
		}
		Consume(refused);
	});
}

BENCH("Receive · 20k polls of an empty channel", FRAMES) {
	ProfileBatch(6, false, [&] {
		// A channel polled every tick is empty most ticks, which the status enum
		// says in as many words. Every world in the host pays this per bus per
		// tick, so it is the most frequently executed line in the file.
		Pair &pair = Wide();
		std::vector<std::byte> scratch;
		size_t empty = 0;
		for (size_t poll = 0; poll < FRAMES; poll++) {
			empty += pair.Far->Receive(scratch) == ChannelStatus::Empty ? 1 : 0;
		}
		Consume(empty);
	});
}

BENCH("PendingBytes · 100k calls", 100'000) {
	ProfileBatch(7, false, [&] {
		// The number worth watching on a live host - a figure that climbs is a
		// consumer falling behind - so it is the number a debug panel samples every
		// frame for every channel.
		Pair &pair = Wide();
		for (size_t frame = 0; frame < 64; frame++) {
			Consume(pair.Near->Send(Payload(SMALL_BYTES)));
		}
		size_t total = 0;
		for (size_t call = 0; call < 100'000; call++) {
			total += pair.Far->PendingBytes();
		}
		Consume(total);
	});
}

// --- contended
// ----------------------------------------------------------------

BENCH("Send + Receive · 100k 4 KiB frames across two threads", 100'000) {
	ProfileBatch(8, true, [&] {
		// **The only row here with a second thread, and the only one whose spread
		// is worth reading.** A channel exists to be used from two threads; a suite
		// that measured only the uncontended mutex would be measuring the case that
		// does not happen. The minimum sample is a lower bound on the contended
		// cost rather than the cost itself, and a spread far wider than the other
		// rows is the scheduler rather than this code.
		//
		// The producer never blocks, so a full queue is retried rather than waited
		// on - which is the caller-decides contract, spelled as the tightest
		// possible version of it.
		constexpr size_t COUNT = 100'000;
		Pair &pair = Wide();

		std::atomic<size_t> received{0};
		const bool capture = ProfileEnabled();
		float consumerMilliseconds = 0;
		const auto consume = [&pair, &received] {
			std::vector<std::byte> scratch;
			size_t taken = 0;
			while (taken < COUNT) {
				if (pair.Far->Receive(scratch) == ChannelStatus::Ok) taken++;
			}
			received.store(taken, std::memory_order_relaxed);
		};
		// The uncaptured branch keeps the original two-reference thread closure.
		// Only capture reads the producer clock, once around its completed body.
		std::thread consumer = capture ? std::thread([&consume, &consumerMilliseconds] {
			const double started = engine::core::Clock::Seconds();
			consume();
			consumerMilliseconds = static_cast<float>((engine::core::Clock::Seconds() - started) * 1000.0);
		})
									   : std::thread(consume);

		for (size_t frame = 0; frame < COUNT;) {
			if (pair.Near->Send(Payload(MEDIUM_BYTES)) == ChannelStatus::Ok) {
				frame++;
			}
		}
		if (capture) {
			{
				ENGINE_PROFILE_CAT("local channel join", engine::core::ProfileCategory::Idle);
				consumer.join();
			}
			// Completed producer time overlaps owner work and is not wall
			// subtraction.
			const engine::core::FrameGraph::ReportedScope report(
				"local channel consumer", engine::core::ProfileCategory::Engine, consumerMilliseconds
			);
		} else
			consumer.join();
		Consume(received.load(std::memory_order_relaxed));
	});
}
