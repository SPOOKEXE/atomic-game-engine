#pragma once

// What an operator standing in front of a running origin can see: what is
// published, what it weighs, and what is moving right now.
//
// **This is the model and the text, and it owns no terminal.** Everything here
// is arithmetic over a publication and a run of counter samples, producing
// lines of text; `Terminal.hpp` is the half that owns a file descriptor, raw
// mode and an escape sequence. The split is what lets the whole of the layout,
// the rates and the history be exercised by a suite that opens no tty and waits
// for nothing — the same split `net`'s `Message.hpp` has against its `Server`,
// and for the same reason.
//
// **A dashboard is built against one publication and does not outlive it.** A
// publication is immutable and publishing replaces it rather than mutating it,
// so a dashboard that cached counts from one and displayed them beside another
// would be showing two publications at once. Republishing means a new
// dashboard.
//
// **It holds no clock.** Every sample is stamped with a time the caller passes
// in, which is `net`'s and `assets::Grant`'s standing rule and is what lets a
// suite state an hour of traffic rather than wait one out.
//
// **The history is sixty one-minute buckets, not an hour of seconds.** An hour
// at sample rate is a ring nobody reads at that resolution and a redraw that
// walks thirty thousand entries; a minute is the finest bucket a "last hour"
// row can be read at, and sixty of them is a sparkline that fits a terminal.
// The current bucket is partial and the row says so, because a total that
// silently includes a third of a minute is a total that reads low for no
// visible reason.
//
// @tier shared

#include <array>
#include <cdn/Origin.hpp>
#include <cdn/Service.hpp>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace cdn {

	// What a line is for, so a terminal can colour it and a suite can assert on
	// structure rather than on spacing.
	//
	// @since v0.9
	enum class LineStyle : uint8_t {
		// Nothing on it.
		Blank,

		// A section title.
		Heading,

		// The numbers under a heading.
		Row,

		// A row worth finding at a glance — a live rate, a total.
		Emphasis,
	};

	// One line of the document.
	//
	// @since v0.9
	struct DashboardLine {
		// The text, without any escape sequence in it. Colour is the
		// terminal's, applied from `Style`, because a model that emitted colour
		// could not be diffed by a test or written to a file.
		std::string Text;

		// What it is for.
		LineStyle Style = LineStyle::Row;
	};

	// What one minute of traffic weighed.
	//
	// @since v0.9
	struct TrafficBucket {
		// Bytes written to sockets in that minute.
		uint64_t SentBytes = 0;

		// Bytes read off sockets in that minute.
		uint64_t ReceivedBytes = 0;
	};

	// What the origin's disk holds.
	//
	// Passed in rather than read here, because reading it walks every chunk
	// directory in the store — once at start-up is a cost worth paying and once
	// a redraw is a dashboard that pins a disk.
	//
	// @since v0.9
	struct StoreFootprint {
		// What the chunks occupy on disk.
		uint64_t Bytes = 0;

		// How many chunk files there are.
		uint64_t Chunks = 0;
	};

	// What the prepared-group cache is holding.
	//
	// @since v0.9
	struct CacheUsage {
		// Bytes held.
		uint64_t Bytes = 0;

		// Groups held.
		uint64_t Groups = 0;

		// What it is allowed to hold.
		uint64_t CapacityBytes = 0;
	};

	// Formats a byte count the way an operator reads one — `4.1 GB`.
	//
	// **Powers of 1024 under decimal names**, which is what every tool an
	// operator already has in front of them does. Being right about the prefix
	// and alone in it helps nobody comparing this against `df`.
	//
	// @param bytes The count.
	// @return The text.
	// @since v0.9
	std::string FormatBytes(uint64_t bytes);

	// Formats a rate — `4.2 MB/s`, `12.4 KB/s`, `0 B/s`.
	//
	// The unit is chosen from the value, which is what "in MB/s or KB/s
	// depending on load" means: a scale fixed at either one is unreadable at the
	// other end of the range an origin actually spans.
	//
	// @param bytesPerSecond The rate.
	// @return The text.
	// @since v0.9
	std::string FormatRate(double bytesPerSecond);

	// Draws one bar of a sparkline.
	//
	// @param value The bucket's value.
	// @param peak The largest bucket in the line. Zero draws the floor.
	// @return A single block glyph, as UTF-8.
	// @since v0.9
	std::string_view SparkGlyph(uint64_t value, uint64_t peak);

	// What is published, what it weighs and what is moving.
	//
	// The document is two parts and only one of them is rebuilt: the live
	// section — rates, history, request counts — is rewritten on every
	// `Sample`, and the content section is written once in the constructor
	// because a publication cannot change under it. That is what keeps a redraw
	// O(the visible rows) on a store with a hundred thousand assets, which is
	// the size this is for.
	//
	// **Not thread-safe.** One owner, the thread that pumps the service.
	//
	// @since v0.9
	class Dashboard {
	  public:
		// How many minutes of history are kept.
		static constexpr size_t HISTORY_MINUTES = 60;

		// How long a rate is measured over before it is published.
		//
		// A rate taken between two samples ten milliseconds apart is mostly
		// noise — one 16 KB read lands in one sample and nothing lands in the
		// next, and the number swings by a factor of a hundred between redraws.
		// A second is long enough to be steady and short enough that a burst is
		// still visible as one.
		static constexpr uint64_t RATE_WINDOW_MILLISECONDS = 1000;

		// How many of the largest assets are listed on their own.
		static constexpr size_t LARGEST_LISTED = 5;

		// @param publication What is being served. Read here and not held: the
		//        content rows are built from it once.
		// @param store What the disk holds, read once by the caller.
		// @param endpoint Where this origin is listening, for the title row.
		Dashboard(const Publication &publication, const StoreFootprint &store, std::string_view endpoint);

		// Takes one reading.
		//
		// Called every pump rather than every redraw. The history is built out
		// of differences between readings, so a sample skipped is traffic that
		// lands in whichever minute the next reading arrives in — and at a
		// redraw rate that is one bucket in four wrong.
		//
		// @param counters What the service has answered so far. Totals, not
		//        deltas: the difference against the previous reading is this
		//        class's to take.
		// @param cache What the prepared-group cache is holding.
		// @param nowMilliseconds The current time. Passed in, never read.
		void Sample(const ServiceCounters &counters, const CacheUsage &cache, uint64_t nowMilliseconds);

		// How many lines the document has.
		size_t Lines() const;

		// One line.
		//
		// @param index The line, from zero.
		// @return The line, or an empty one past the end — a viewport that
		//         scrolled past the bottom asks for these, and a bounds check
		//         at every call site is a bounds check one of them will forget.
		DashboardLine LineAt(size_t index) const;

		// The most recent measured send rate, in bytes per second.
		double SentBytesPerSecond() const {
			return SentRate;
		}

		// The most recent measured receive rate, in bytes per second.
		double ReceivedBytesPerSecond() const {
			return ReceivedRate;
		}

		// What the last hour carried, current partial minute included.
		TrafficBucket LastHour() const;

		// One minute of history.
		//
		// @param minutesAgo Zero is the minute in progress.
		// @return That minute, or an empty bucket past the end of the ring.
		TrafficBucket MinuteAgo(size_t minutesAgo) const;

	  private:
		// Rewrites the live section if a sample has landed since it was last
		// written. Everything else was written once, in the constructor.
		//
		// **Lazy because sampling is a hundred times a second and drawing is
		// four.** Composing inside `Sample` would format sixteen lines and two
		// sparklines on every pump of the serving loop, and throw away
		// twenty-four of every twenty-five. Const because it is a cache: what it
		// produces is decided entirely by state a caller has already handed
		// over.
		void ComposeLive() const;

		// Moves the ring on, clearing whatever minutes were skipped.
		void Rotate(uint64_t nowMinute);

		mutable std::vector<DashboardLine> Live;
		mutable bool LiveComposed = false;
		std::vector<DashboardLine> Content;

		std::array<TrafficBucket, HISTORY_MINUTES> History{};

		// Which minute of the epoch the newest bucket is, so a gap between
		// samples clears the buckets it crossed rather than adding an hour-old
		// value to a fresh one.
		uint64_t NewestMinute = 0;
		size_t NewestBucket = 0;
		bool Started = false;

		ServiceCounters Previous;
		ServiceCounters Latest;
		CacheUsage Held;

		uint64_t WindowStartMilliseconds = 0;
		uint64_t WindowSentBytes = 0;
		uint64_t WindowReceivedBytes = 0;
		double SentRate = 0.0;
		double ReceivedRate = 0.0;

		std::string Listening;
	};
}
