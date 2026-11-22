#pragma once

// A rolling window of frame times, and the four numbers a reader wants from it.
//
// **It draws nothing.** It is here rather than in the panel that shows it
// because a caller who wants a frame rate should not have to include a
// flamegraph renderer to get one - and the client, which holds one of these as
// a member, was doing exactly that.
//
// The window is a ring of deltas with running sums, so recording a frame is
// constant time and the extremes are rescanned only when the sample that held
// one falls out. A twenty-second window at a few hundred frames a second is
// tens of thousands of samples; answering four questions by walking it four
// times, every frame, to draw seven lines of text is the shape this exists to
// avoid.
//
// @client

#include <cstddef>
#include <cstdint>
#include <vector>

namespace engine::render {

	// Everything the statistics panel shows, from one walk of the window.
	//
	// The panel used to ask for these one at a time, and each question walked
	// the whole window to answer it. At a few hundred frames a second a twenty
	// second window is tens of thousands of samples, so four questions was four
	// passes over a quarter of a megabyte, every frame, to draw seven lines of
	// text - and it cost more than the scene did.
	//
	// @client
	struct FrameSummary {
		// Frames per second, from the most recent sample.
		float Current = 0.0f;

		// Duration of the most recent frame, in milliseconds.
		float CurrentMilliseconds = 0.0f;

		// Frames per second of the *worst* frame in the window.
		float Minimum = 0.0f;

		// Frames per second from the mean frame duration over the window.
		float Average = 0.0f;

		// Frames per second of the best frame in the window.
		float Maximum = 0.0f;

		// Mean absolute change between consecutive frames, in milliseconds.
		float Jitter = 0.0f;
	};

	// A rolling window of frame times. Twenty seconds, because a hitch every
	// ten is a thing you have to be able to sit and watch for.
	// Queries return zero while the window is empty.
	//
	// @client
	class FrameStatistics {
	  public:
		// Length of the rolling sample window, in seconds.
		static constexpr double WINDOW_SECONDS = 20.0;

		// Samples the window will hold, whatever `WINDOW_SECONDS` asks for.
		//
		// **Seconds alone is not a bound on memory, and at an uncapped frame
		// rate it is not a small one.** How many samples twenty seconds holds is
		// the frame rate, so a headless client running at several thousand
		// frames a second filled this to **2 MiB** - measured by the heap
		// profiler, still climbing at 48 KiB a second forty seconds into a run,
		// for a panel nobody had open. That is the same shape `FrameGraph`'s
		// retained window had and it is fixed the same way: bound the storage,
		// and let the window depth be what fits.
		//
		// Sixteen thousand samples is 256 KiB and covers the full twenty seconds
		// at any frame rate up to about 800 - which is every rate a person is
		// actually watching the panel at. Above that the window is the most
		// recent 16,384 frames instead, `SampleCount` says how many that is, and
		// every figure the panel shows is over what was kept.
		//
		// @since v0.18
		static constexpr size_t MAXIMUM_SAMPLES = 16384;

		// Records one positive frame duration and removes samples older than the window.
		//
		// @param now          Current monotonic time, in seconds.
		// @param deltaSeconds Duration of the frame, in seconds. Non-positive values are ignored.
		void Record(double now, float deltaSeconds);

		// Removes every recorded sample.
		void Clear();

		// Reports whether the window contains at least one sample.
		bool HasSamples() const;

		// Every figure below, from a single pass over the window.
		//
		// What anything drawing the panel should call. The individual accessors
		// are each a walk of the whole window and are kept for callers that want
		// exactly one number; asking for several of them in a row is asking for
		// the same walk several times.
		FrameSummary Summarise() const;

		// Frames per second, from the most recent sample.
		float Current() const;

		// Over the window. Minimum FPS is the worst frame, which is the one
		// that matters - an average of 144 with a floor of 12 is not a smooth
		// game.
		float Minimum() const;

		// Frames per second from the mean frame duration over the window.
		float Average() const;

		// Maximum frames per second over the window.
		float Maximum() const;

		// Duration of the most recent frame, in milliseconds.
		float CurrentMilliseconds() const;

		// Mean absolute change between consecutive frames. A high average FPS
		// with high jitter reads as stutter, and neither number alone says so.
		// Returns milliseconds, or zero when fewer than two samples exist.
		float Jitter() const;

		// Number of frame samples currently inside the rolling window.
		size_t SampleCount() const {
			return Count;
		}

	  private:
		// Retires the oldest sample, taking its contribution out of every
		// running total.
		//
		// **One place, because there are two reasons to retire one** - it fell
		// out of the time window, or the window is full - and the bookkeeping
		// either one needs is identical and easy to get subtly wrong. A sum
		// that is not decremented is a mean that drifts upward forever.
		void DropOldest();

		struct Sample {
			double Time = 0.0;
			float Delta = 0.0f;
		};

		// A ring over a flat vector, not a deque.
		//
		// The access pattern is one push and a few pops from the front per
		// frame, which is what a deque is for - but the *reads* are full walks
		// of tens of thousands of samples, and a deque walks them through a
		// table of chunk pointers. A ring over contiguous storage is the same
		// O(1) at both ends and a straight line to read, which is the operation
		// that was actually costing something.
		//
		// It also stops allocating once the window has filled, where the deque
		// released and reacquired a chunk every time the window slid across a
		// boundary.
		std::vector<Sample> Ring;

		// Index of the oldest sample. Ring is empty until the first Record.
		size_t Head = 0;

		// Live samples, which is not Ring.size() - that is the capacity.
		size_t Count = 0;

		// Running totals, maintained as samples arrive and leave.
		//
		// A walk of the window was the obvious way to answer these, and it had
		// the wrong shape: the number of samples in twenty seconds is the frame
		// *rate* times twenty, so every frame the game got faster this got
		// slower. Something that costs more the better the thing it measures
		// performs is not a measurement anybody can leave switched on.
		// Mutable for the same reason the extremes are: Rescan below rebuilds
		// them, and it runs from a const query.
		mutable double DeltaSum = 0.0;

		// Sum of the absolute change between consecutive samples, which is what
		// Jitter is a mean of. A sample leaving takes exactly one pair with it.
		mutable double ChangeSum = 0.0;

		// The extremes, cached. Mutable because they are rebuilt lazily by a
		// const query rather than eagerly by every Record.
		mutable float Worst = 0.0f;
		mutable float Best = 0.0f;

		// Set when the sample that *was* an extreme leaves the window, because
		// nothing cheaper than another look can say what the next one is.
		//
		// The worst frame in twenty seconds ages out about once every twenty
		// seconds, so this is rare - and in the pathological case where it is
		// every frame, the cost is the walk this replaced. Never worse.
		mutable bool ExtremesStale = false;

		// Rebuilds the extremes, and the sums with them.
		//
		// The sums are recomputed here rather than only accumulated because a
		// running total that is only ever added to and subtracted from drifts,
		// and this pass is already touching every sample.
		void Rescan() const;

		// Ring index of the nth-oldest sample.
		size_t IndexOf(size_t offset) const {
			return (Head + offset) % Ring.size();
		}
	};
}
