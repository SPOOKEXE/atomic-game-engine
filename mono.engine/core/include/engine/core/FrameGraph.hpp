#pragma once

// The per-frame scope tree the F5 overlay draws.
//
// Tracy is the real profiler and this is not trying to be one. This exists
// because a flamegraph has to be visible *inside the running game*, with no
// second process attached, on a machine that is not the developer's. It records
// one frame at a time, from one thread, into a fixed buffer.
//
// Scopes are opened by the ENGINE_PROFILE macros in Profiling.hpp, which feed
// Tracy and this at the same time.
//
// Three things here are about a frame you are *not* looking at:
//
//   - `RecentMaximum` is the worst reading a span reached in the last few
//     hundred frames. A span costing 0.2 ms now and 14 ms every fortieth frame
//     reads as 0.2 on any panel a person can watch, and the fortieth frame is
//     the one worth knowing about.
//   - the history window keeps five seconds of per-span readings.
//   - `WriteSnapshot` dumps that window: percentiles per span, then the worst
//     frames and what was in them.
//
// @tier L0 · shared

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

namespace engine::core {

	// A broad owner used to group a span's self time in the frame overlay.
	enum class ProfileCategory : uint8_t {
		Engine, // General engine work.
		Render, // Rendering work.

		// Time inside the entity-component system: the scheduler, every system
		// it ran, and the storage work underneath them.
		//
		// **This is where a game's time goes.** Every engine and game system
		// runs through the ECS, so a system that is slow is ECS time and the
		// bar says so. It is separate from `Simulation` because the two answer
		// different questions: this one is "what did the systems cost", and
		// that one is "what did the machinery around them cost" — a driver
		// spending more on its barrier than on its worlds is a real and
		// findable problem, and one category could not show it.
		ECS,

		// Simulation work that is not a system: the fixed-timestep accumulator,
		// the driver's barrier, bus routing, snapshotting.
		Simulation,

		Script, // Script runtime work.

		// Time the frame spent waiting rather than working — the wait for the
		// display, above all.
		//
		// It is a category rather than something excluded from the graph because
		// it is real time and a frame that spends it is really that long. But it
		// is not *work*, and mixing the two makes a profiler lie by arithmetic:
		// with vertical sync on, fifteen of a sixteen millisecond frame are a
		// sleep, and every span that actually did something reads as one per
		// cent of the frame. The panel subtracts this to get a busy figure, and
		// shares that mean something.
		Idle,

		Count, // Number of categories; not a span category.
	};

	// Returns the short display name for a profile category, or `?` for `Count`
	// and values outside the enum.
	//
	// @param category The category to name.
	// @return The static display name.
	std::string_view GetCategoryName(ProfileCategory category);

	// One completed timing span published for the in-game frame overlay.
	struct FrameSpan {
		// Either a literal the caller owns for the life of the process, or a
		// copy the frame owns — see `Scope` and `CopiedScope`. The overlay reads
		// these after the frame that produced them has ended, so anything with a
		// shorter life is a dangling read rather than a wrong label.
		std::string_view Name;

		// Zero-based nesting level within the frame's recorded scope tree.
		uint32_t Depth = 0;

		// Index of the span this one opened inside. Kept as well as the depth so
		// that a view showing one subtree can take it directly, rather than
		// re-deriving parentage by scanning the depth column.
		uint32_t Parent = 0;

		// Milliseconds from BeginFrame() until this span opened.
		float StartMilliseconds = 0.0f;

		// Inclusive duration of this span in milliseconds.
		float Milliseconds = 0.0f;

		// Time in this span but not in any of its children. What a "where did
		// the frame go" answer is made of, and what the category totals sum.
		float SelfMilliseconds = 0.0f;

		// Of `Milliseconds`, how much was spent waiting — this span's own idle
		// time and every idle span beneath it.
		//
		// **Because an inclusive duration is not a cost.** `Renderer::Render`
		// on a vsynced frame reads 16 ms, and 15.9 of that is one child
		// blocking on the display. A reader looking at the biggest number goes
		// and optimises the renderer, which is what happened — twice — before
		// this field existed. `Milliseconds - IdleMilliseconds` is the part
		// anybody can do something about, and it is the number the overlay
		// leads with.
		//
		// Idle *self* time is what accumulates, so a wait nested inside another
		// wait is counted once, and it is added to every ancestor rather than
		// to the immediate parent alone — every span that contains a wait
		// contains it.
		//
		// @since v0.6
		float IdleMilliseconds = 0.0f;

		// Broad owner used when accumulating category self time.
		ProfileCategory Category = ProfileCategory::Engine;

		// Whether this duration was measured somewhere else and handed over.
		//
		// **A reported span is not wall time on this thread**, so it must not
		// be subtracted from its parent's self time. Eight workers each
		// reporting five milliseconds under a batch that took one would give
		// that batch a self time of minus thirty-nine — a number that is not
		// wrong by a little, it is a different quantity. The tree maths skips
		// these, and the parent keeps the wall time it actually spent waiting.
		//
		// It follows that a frame's spans no longer sum to the frame, and that
		// is correct: eight workers really did do eight milliseconds of work in
		// one millisecond of wall clock. The overlay marks them so a reader
		// knows which bars are concurrent.
		bool Reported = false;
	};

	// Fills in `FrameSpan::IdleMilliseconds` across a frame's spans.
	//
	// **Public because two callers need the same answer and one of them is a
	// test.** `EndFrame` runs it before publishing so the overlay and the RMAX
	// history agree; the panel suite runs it over hand-built spans so the
	// invariants it checks — a share never above 100%, a wait inside a wait
	// counted once — stay properties of this arithmetic rather than of a
	// number a test wrote down.
	//
	// Idle *self* time is what accumulates, added to the span itself and to
	// every ancestor. Safe on a malformed parent chain: the walk is bounded by
	// the span count as well as by depth.
	//
	// @param spans A frame's spans in open order, with `Parent` set. Their
	//              `IdleMilliseconds` are added to rather than replaced, so
	//              call once per frame.
	// @since v0.6
	void AccumulateIdleMilliseconds(std::span<FrameSpan> spans);

	// Collects one thread's nested scopes into a bounded per-frame tree and
	// short spike history for the in-game overlay.
	//
	// This process-wide collector is not thread-safe. BeginFrame() selects the
	// collecting thread; scopes opened on other threads are dropped and counted.
	// Read and control the graph from the collecting thread.
	class FrameGraph {
	  public:
		// A frame that wants more than this is a frame with an instrumentation
		// bug, not a frame worth drawing. Overflow is counted, not resized:
		// reallocating mid-frame would show up in the measurement.
		static constexpr size_t MAXIMUM_SPANS = 4096;

		// Deep enough to reach past the schedule. The first levels are spent
		// before any real work starts — frame, phase, system — so a smaller
		// budget records the schedule and throws away the frame.
		//
		// Past this the depth is still tracked, so Pop stays balanced, but
		// nothing is recorded and the drop is counted.
		static constexpr uint32_t MAXIMUM_DEPTH = 12;

		// Marks a root span's parent and the root of a filtered view.
		static constexpr uint32_t NO_PARENT = UINT32_MAX;

		// Enables or disables collection. A state change clears the retained
		// history; disabling also clears the published frame.
		//
		// Collection is off until something asks for it, so the macros cost a
		// predictable branch and nothing else in an ordinary run.
		//
		// @param enabled Whether subsequent complete frames should be collected.
		static void SetEnabled(bool enabled);

		// Reports whether frame collection is enabled.
		static bool IsEnabled();

		// Starts collecting a frame on the calling thread when enabled.
		//
		// Call once per frame around all measured work. Scopes opened outside the
		// BeginFrame()/EndFrame() pair do not participate.
		static void BeginFrame();

		// Publishes the frame started by BeginFrame().
		//
		// All Scope objects must be destroyed before this call. An open span is
		// measured to the frame boundary for diagnostics, but destroying its stale
		// Scope after publication is unsupported.
		//
		// @warning End every profiling scope before ending its frame.
		static void EndFrame();

		// Returns the spans from the last completed frame in open order.
		//
		// The reference and its copied names remain stable until the next EndFrame()
		// or until SetEnabled(false), so the overlay can read it while the next
		// frame is built.
		static const std::vector<FrameSpan> &Spans();

		// Returns the total duration of the last completed frame in milliseconds.
		static float FrameMilliseconds();

		// Returns how much of the last completed frame ran inside no span at all.
		//
		// The frame's own self time — FrameMilliseconds() less the inclusive
		// duration of the root spans. Everything else this class reports is time
		// somebody thought to name; this is the rest of it, and on a frame that
		// is only partly instrumented it is usually most of it.
		//
		// The overlay shows it as a row of its own because the alternative is a
		// panel listing 0.3 ms of spans beneath a heading that says 1.1 ms and
		// leaving the reader to work out which number is wrong. Neither is: the
		// other 0.8 ms is real work nobody has put a scope around yet.
		//
		// @return Milliseconds inside the frame and outside every span.
		static float UnmarkedMilliseconds();

		// Returns the last completed frame's accumulated self time for a category.
		// Invalid categories return zero.
		//
		// @param category The category to total.
		// @return Self time in milliseconds.
		static float CategoryMilliseconds(ProfileCategory category);

		// Returns the scopes dropped from the last completed frame.
		//
		// Drops include buffer overflow, scopes past MAXIMUM_DEPTH, and scopes
		// opened outside the thread that called BeginFrame(). The overlay shows the
		// count because a partial flame graph must not look complete.
		static size_t Dropped();

		// --- timings measured somewhere else -----------------------------------

		// Records a span whose duration was measured on another thread or in
		// another process.
		//
		// **A worker's time cannot be recorded live.** `Push` refuses a span
		// opened on any thread but the frame's owner, and it is right to: the
		// depth is the owning thread's stack, and a worker moving it would
		// corrupt that thread's nesting. Locking instead would put contention
		// in the one path that runs on every span of every frame — so today
		// every worker's work is counted by `Dropped()` and shown nowhere.
		//
		// The answer is not to time it from here. It is for the producer to
		// measure itself and hand the number back, and for the owner to plot
		// **the latest timing it received**. That is a frame behind for a
		// worker and a barrier behind for a host process, and being a frame
		// behind about a worker is worth far more than being silent about one.
		//
		// The span is placed at the current depth, so a reported worker sits
		// under whatever opened the batch. Its start is the moment of the call
		// rather than the moment the work happened: this is a duration in a
		// tree, not a position on a timeline, and pretending otherwise would
		// draw bars that overlap their own parent.
		//
		// @param name         What ran. Same lifetime rule as `Scope`: a
		//                     literal, or text outliving the frame.
		// @param category     Broad owner used for category totals.
		// @param milliseconds What the producer said it took. A negative value
		//                     is ignored rather than clamped — it means the
		//                     producer measured wrongly, and turning it into
		//                     zero would hide that.
		static void Report(std::string_view name, ProfileCategory category, float milliseconds);

		// Records a reported span whose name is only known at runtime.
		//
		// For a producer named at runtime — a worker index, a host name. The
		// text is copied into frame-owned storage, exactly as `CopiedScope`
		// does and for the same reason.
		//
		// @param fallback     Stable name used when `name` is empty.
		// @param name         Runtime name to copy.
		// @param category     Broad owner used for category totals.
		// @param milliseconds What the producer said it took.
		static void ReportNamed(
			std::string_view fallback, std::string_view name, ProfileCategory category, float milliseconds
		);

		// --- history ---------------------------------------------------------

		// Maximum number of completed frames searched by RecentMaximum().
		static constexpr size_t RECENT_FRAMES = 300;

		// Maximum elapsed time retained for WriteSnapshot().
		static constexpr double HISTORY_SECONDS = 5.0;

		// Maximum retained frame count. This also bounds memory when the frame
		// rate is high enough that HISTORY_SECONDS alone would not.
		static constexpr size_t MAXIMUM_HISTORY_FRAMES = 20000;

		// Distinct span names the history tracks. Past this a name is not
		// recorded and the snapshot says how many it turned away — a copied
		// name is arbitrary text, so a script naming a zone per chunk could
		// otherwise grow this without limit.
		static constexpr size_t MAXIMUM_HISTORY_NAMES = 256;

		// Returns the worst *single* reading for a named span in any of the last
		// RECENT_FRAMES frames — not a total. A span that opens six times in a
		// frame contributes its worst of the six, so the number is comparable
		// with the per-frame figure beside it rather than being six times
		// larger.
		//
		// Zero for a span that has not appeared in the window, so one that stops
		// running decays out rather than keeping its worst reading forever.
		//
		// @param name The exact span name to find.
		// @return The worst duration in milliseconds, or zero when absent.
		static float RecentMaximum(std::string_view name);

		// Returns the number of completed frames currently retained.
		static size_t HistoryFrames();

		// Returns the elapsed seconds covered by the retained frame window.
		// Fewer than two frames cover zero seconds.
		static double HistorySeconds();

		// Writes the retained window to a text file at `path`.
		//
		// The file contains a summary per span, then the
		// frames that took longest and what was in them.
		//
		// Per-span totals rather than the full tree, because five seconds of
		// frames is millions of spans, and what a spike hunt needs is which
		// frame was slow and what was in it.
		//
		// @param path The file to create or replace.
		// @return False if no history is retained, the file cannot be opened, or
		//         writing does not complete successfully.
		static bool WriteSnapshot(const std::filesystem::path &path);

		// --- scopes ----------------------------------------------------------

		// Opens a frame span whose name storage remains valid through publication.
		//
		// Construct at the start of a C++ scope and let destruction close the span.
		// The name is not copied: pass a literal or storage that remains valid until
		// at least the next EndFrame().
		class Scope {
		  public:
			// Opens a span when a frame is being collected on this thread.
			//
			// @param name Stable span name storage; the text is not copied.
			// @param category Broad owner used for category totals.
			Scope(std::string_view name, ProfileCategory category);

			// Closes the span opened by this object.
			~Scope();

			// Scope objects uniquely own their stack entry and cannot be copied.
			Scope(const Scope &) = delete;

			// Scope objects cannot take ownership of another stack entry by copy.
			Scope &operator=(const Scope &) = delete;

			// Scope objects cannot move because destruction order closes the span.
			Scope(Scope &&) = delete;

			// Scope objects cannot take ownership of another stack entry by move.
			Scope &operator=(Scope &&) = delete;

		  protected:
			// Creates an inactive base for CopiedScope, which must copy before Push().
			Scope() = default;

			// The opened span or an internal sentinel describing how it participated.
			size_t Index = NOT_RECORDING;
		};

		// Opens a frame span after copying a runtime name into frame-owned storage.
		//
		// Use for a name that does not outlive the call — a script chunk, a node
		// kind built on the stack. The text is copied into a pool the frame
		// owns, so the view the overlay reads stays good until that frame is
		// replaced.
		//
		// Falls back to `fallback` when the caller had nothing to say, which is
		// when frame collection turns on: a caller only looks a
		// name up when something is listening, and whether that is true is
		// decided one frame and acted on the next.
		class CopiedScope : public Scope {
		  public:
			// Copies and opens `name`, or opens `fallback` when `name` is empty.
			//
			// No text is copied when collection is inactive, on another thread, or
			// beyond a frame bound.
			//
			// @param fallback Stable name used when `name` is empty.
			// @param name Runtime name to copy into frame-owned storage.
			// @param category Broad owner used for category totals.
			CopiedScope(std::string_view fallback, std::string_view name, ProfileCategory category);

			// Closes the copied-name span through Scope's RAII destructor.
			~CopiedScope() = default;
		};

	  private:
		// A scope that did not participate at all: collection is off, or it was
		// opened on a thread that does not own the frame.
		static constexpr size_t NOT_RECORDING = static_cast<size_t>(-1);
		// A scope past MAXIMUM_DEPTH or past MAXIMUM_SPANS. No span was stored,
		// but the depth moved — so the matching close has to move it back, or
		// every sibling after it is recorded one level too deep.
		static constexpr size_t DEPTH_ONLY = static_cast<size_t>(-2);

		// Nested classes reach these; nothing outside the header can.
		static size_t Push(std::string_view name, ProfileCategory category);
		static void Pop(size_t index);
	};
}
