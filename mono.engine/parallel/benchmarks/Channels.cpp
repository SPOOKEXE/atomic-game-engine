// The framed queue two worlds talk through, at the sizes they talk at.
//
// **This is the seam that decides whether thread-per-world and process-per-world
// stay the same design.** Everything crossing a world boundary is already
// bytes, so the only difference between two worlds in one process and two in
// two processes is what carries them. A channel that costs more than the work
// it carries would push callers into sharing memory instead, and the two
// arrangements would stop being interchangeable - quietly, one caller at a
// time.
//
// **Three frame sizes, because they are three different questions.** Sixty-four
// bytes is a bus message and its cost is entirely per-frame overhead: the
// mutex, the queue node, the length prefix. Four kibibytes is an envelope, and
// somewhere between the two the copy starts to matter. Two hundred and fifty-six
// kibibytes is a snapshot chunk, where the figure should be memory bandwidth
// and nothing else - a row that is materially worse than a `memcpy` of the same
// size is a frame being copied more than once.
//
// **The refusal paths are measured too, and that is deliberate.** A full channel
// refuses rather than blocking, which means a producer that has outrun its
// consumer calls `Send` and gets `Full` on *every* attempt until the consumer
// catches up. That is the hot path of an overloaded host, so a refusal that
// were expensive would make the overloaded case worse in exactly the moment it
// could least afford it.
//
// **One row uses two threads and it is the only one whose spread should be
// read.** Everything else here is single-threaded and reproducible; the
// contended row depends on how the scheduler feels, and the minimum sample is a
// lower bound on the contended cost rather than the contended cost. It is here
// because cross-thread is what a channel is *for*, and a suite that only ever
// measured the uncontended mutex would be measuring the case that never
// happens.

#include <engine/parallel/Channel.hpp>
#include <engine/testing/Bench.hpp>

#include <atomic>
#include <cstddef>
#include <memory>
#include <span>
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
}

using namespace channel_bench;

// --- one frame at a time ------------------------------------------------------

BENCH("Send + Receive · 20k 64-byte frames", FRAMES) {
	Pair &pair = Wide();
	std::vector<std::byte> scratch;
	size_t moved = 0;
	for (size_t frame = 0; frame < FRAMES; frame++) {
		moved += RoundTrip(pair, SMALL_BYTES, scratch);
	}
	Consume(moved);
}

BENCH("Send + Receive · 20k 4 KiB frames", FRAMES) {
	Pair &pair = Wide();
	std::vector<std::byte> scratch;
	size_t moved = 0;
	for (size_t frame = 0; frame < FRAMES; frame++) {
		moved += RoundTrip(pair, MEDIUM_BYTES, scratch);
	}
	Consume(moved);
}

BENCH("Send + Receive · 2k 256 KiB frames", 2000) {
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
}

// --- queued, then drained -----------------------------------------------------

BENCH("Send 10k then Receive 10k · 4 KiB frames", 10'000) {
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
}

// --- the refusals -------------------------------------------------------------

BENCH("Send · 20k refusals against a full channel", FRAMES) {
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
}

BENCH("Send · 20k frames over the maximum", FRAMES) {
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
}

BENCH("Receive · 20k polls of an empty channel", FRAMES) {
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
}

BENCH("PendingBytes · 100k calls", 100'000) {
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
}

// --- contended ----------------------------------------------------------------

BENCH("Send + Receive · 100k 4 KiB frames across two threads", 100'000) {
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
	std::thread consumer([&pair, &received] {
		std::vector<std::byte> scratch;
		size_t taken = 0;
		while (taken < COUNT) {
			if (pair.Far->Receive(scratch) == ChannelStatus::Ok) {
				taken++;
			}
		}
		received.store(taken, std::memory_order_relaxed);
	});

	for (size_t frame = 0; frame < COUNT;) {
		if (pair.Near->Send(Payload(MEDIUM_BYTES)) == ChannelStatus::Ok) {
			frame++;
		}
	}
	consumer.join();
	Consume(received.load(std::memory_order_relaxed));
}
