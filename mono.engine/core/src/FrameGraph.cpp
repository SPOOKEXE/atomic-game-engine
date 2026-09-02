#include <engine/core/Clock.hpp>
#include <engine/core/FrameGraph.hpp>
#include <engine/core/HeapProfile.hpp>
#include <engine/core/Metrics.hpp>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <deque>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace engine::core {

	namespace {

		constexpr uint32_t NO_NAME = UINT32_MAX;

		struct HistoryFrame {
			double Seconds = 0.0;
			float Milliseconds = 0.0f;

			// How much of `Milliseconds` the thread spent waiting.
			//
			// **Kept per frame because it cannot be recovered from the spans.**
			// A span's history reading is its *busy* time, and an `Idle` span's
			// busy time is zero by construction - so a frame that was sixteen
			// milliseconds of vsync wait and a frame that was sixteen
			// milliseconds of work record an identical set of span readings.
			// Without this the snapshot could show the frame total and every
			// span in it and still not say which of the two it was.
			float IdleMilliseconds = 0.0f;
			// Where this frame's readings begin in `State::Readings`, and how
			// many there are.
			//
			// **A slice of one shared ring, and it used to be a `std::vector`
			// per frame.** The window holds up to `MAXIMUM_HISTORY_FRAMES`
			// frames, so that was twenty thousand separate heap blocks, each
			// keeping whatever capacity the busiest frame ever to occupy that
			// slot had needed - and none of it given back when the panel closed.
			// The heap profiler measured a headless client reaching **10 MiB
			// across 20,249 live blocks inside `EndFrame`, still climbing after
			// forty seconds**, for a panel nobody had open.
			//
			// It looked bounded and was not, in the way that matters: the bound
			// was twenty thousand times the worst frame anybody ever recorded,
			// which is forty megabytes, and it was approached slowly enough to
			// read as a leak on any graph short of an hour. A slice into a ring
			// the whole window shares makes the figure a constant that is
			// allocated once and stated below.
			uint32_t FirstReading = 0;
			uint32_t ReadingCount = 0;
		};

		// One span's worst reading in one retained frame.
		struct HistoryReading {
			uint32_t Name = 0;
			float Milliseconds = 0.0f;
		};

		struct State {
			bool Enabled = false;
			bool Recording = false;

			// **Atomic for the reason `ecs::Store`'s owner is**, and the two
			// should stay the same shape. One thread writes this at
			// `BeginRecording` and every job worker reads it below to decide
			// whether a span is theirs; a plain `std::thread::id` read
			// concurrently with that write is a data race by the letter of the
			// standard even where it happens to be benign. Relaxed on both
			// sides: nothing is published through it, and a worker that reads
			// a stale owner for one span drops a span it would have dropped.
			std::atomic<std::thread::id> Owner;

			uint64_t FrameStartNanoseconds = 0;

			// Built during the frame.
			std::vector<FrameSpan> Building;
			std::vector<size_t> Open;
			// Logical placement scratch for reported worker hierarchies. Retained
			// because EndFrame is part of the profiler and must not allocate every
			// frame in order to describe allocations elsewhere.
			std::vector<float> ReportedChildEnds;
			// Tracked past MAXIMUM_DEPTH as well as below it, so that a Pop
			// under the budget still matches the Push that opened it.
			uint32_t Depth = 0;

			// Names copied for spans whose text does not outlive the call. A
			// deque because a span keeps a view into the string and a vector
			// would move a short one out from under it as it grew. Entries are
			// reused rather than freed, so a steady frame stops allocating.
			std::deque<std::string> BuildingNames;
			std::deque<std::string> PublishedNames;
			size_t BuildingNameCount = 0;

			// Handed to the overlay after EndFrame.
			std::vector<FrameSpan> Published;
			float PublishedMilliseconds = 0.0f;
			float PublishedUnmarked = 0.0f;
			float PublishedCategories[static_cast<size_t>(ProfileCategory::Count)] = {};

			// **Incremented from worker threads**, which is the whole reason
			// the counter exists - a span opened off the owning thread is
			// counted rather than locked for. Concurrent `++` on a plain
			// `size_t` is a data race, and one that loses counts silently,
			// which for a counter whose only job is to say "the overlay is
			// under-reporting" is the worst possible failure.
			std::atomic<size_t> DroppedThisFrame{0};
			size_t PublishedDropped = 0;

			// --- history ------------------------------------------------------
			//
			// A span is the same span across frames because it has the same
			// name, and the views a frame hands out point into that frame's own
			// pool. So the text is owned here, once, and everything below refers
			// to it by index.
			//
			// Not core::Name: that interns for the life of the process and never
			// forgets, which is right for a component name and wrong for text a
			// script chose. This table is bounded and says when it fills.
			std::vector<std::string> HistoryNames;
			// std::less<> so a string_view looks up without building a string.
			// This runs per span per frame and has no reason to allocate.
			std::map<std::string, uint32_t, std::less<>> HistoryNameIds;

			// This frame's worst reading per span, indexed by name id, plus
			// which ids the frame touched - so it clears in the size of the
			// frame rather than the size of the table. Seen is its own flag
			// rather than a nonzero reading: a span can genuinely take 0.00 ms
			// and still have run.
			std::vector<float> FrameMaximums;
			std::vector<uint8_t> FrameSeen;
			std::vector<uint32_t> FrameTouched;

			// Per span, its worst reading in each of the last RECENT_FRAMES
			// frames. A ring: the oldest is overwritten rather than everything
			// shifted.
			std::vector<std::vector<float>> RecentRings;
			size_t RecentCursor = 0;

			std::vector<HistoryFrame> History;
			size_t HistoryStart = 0;
			size_t HistoryCount = 0;
			size_t HistoryNamesDropped = 0;

			// Every retained frame's span readings, in one ring.
			//
			// Frames are written in order and their readings are contiguous, so
			// the oldest retained frame's slice is always the tail - which is
			// what lets a frame be evicted by advancing two integers rather than
			// by freeing anything.
			std::vector<HistoryReading> Readings;
			size_t ReadingsHead = 0;
			size_t ReadingsUsed = 0;
			size_t HistoryReadingsDropped = 0;

			// --- folded stacks ------------------------------------------------
			bool Folding = false;
			FoldedStacks Folded;

			// --- triggers -----------------------------------------------------
			//
			// Owned by the collecting thread, the same as everything above:
			// armed from it, evaluated in `EndFrame`, read from it.
			std::vector<FrameTrigger> Triggers;
			FrameTriggerHit Hit;
			bool Latched = false;
			size_t FoldedFrameCount = 0;
		};

		State &Get() {
			static State state;
			return state;
		}

		float MillisecondsSince(uint64_t start) {
			return static_cast<float>(static_cast<double>(Clock::Nanoseconds() - start) / 1'000'000.0);
		}

		void ClearHistory(State &state) {
			// **The name table goes with the window, and it did not.** Zeroing
			// the rings emptied the readings and left the names that indexed
			// them, so the MAXIMUM_HISTORY_NAMES budget was spent once per
			// process rather than once per session: open the panel in one
			// scene, close it, open it in another, and the second scene's spans
			// were turned away by a table full of the first one's. The snapshot
			// said so in a WARNING line, which is the only reason it was
			// findable at all.
			//
			// The cost is that a name's ring is allocated again on the first
			// collected frame of the next session, which is the same frame that
			// takes the window's own storage - one hitch, at the moment
			// somebody opened a profiler, rather than a quota that never comes
			// back.
			state.ReadingsHead = 0;
			state.ReadingsUsed = 0;
			state.HistoryReadingsDropped = 0;

			state.HistoryNames.clear();
			state.HistoryNameIds.clear();
			state.RecentRings.clear();
			state.FrameMaximums.clear();
			state.FrameSeen.clear();
			state.FrameTouched.clear();

			state.RecentCursor = 0;
			state.HistoryStart = 0;
			state.HistoryCount = 0;
			state.HistoryNamesDropped = 0;
		}

		uint32_t HistoryNameId(State &state, std::string_view name) {
			auto found = state.HistoryNameIds.find(name);
			if (found != state.HistoryNameIds.end()) {
				return found->second;
			}
			if (state.HistoryNames.size() >= FrameGraph::MAXIMUM_HISTORY_NAMES) {
				state.HistoryNamesDropped++;
				return NO_NAME;
			}

			const auto id = static_cast<uint32_t>(state.HistoryNames.size());
			state.HistoryNames.emplace_back(name);
			state.HistoryNameIds.emplace(state.HistoryNames.back(), id);
			// New names arrive mid-window, so the ring starts zeroed: the span
			// did not appear in the frames before this one, which is what zero
			// means.
			state.RecentRings.emplace_back(FrameGraph::RECENT_FRAMES, 0.0f);
			state.FrameMaximums.push_back(0.0f);
			state.FrameSeen.push_back(0);
			return id;
		}

		// Reduces the frame just recorded to one worst reading per span, then
		// feeds both consumers. Called from EndFrame on the spans about to be
		// published, so nothing walks the tree twice.
		// Whether this binary was compiled with optimisation on.
		//
		// GCC and Clang define `__OPTIMIZE__` for any `-O` above zero and leave
		// it undefined at `-O0`. MSVC has no equivalent, so `NDEBUG` stands in -
		// it is a weaker question, and the answer it gives is the right one for
		// the presets this repository ships.
		constexpr bool OptimisedBuild() {
#if defined(_MSC_VER)
#if defined(NDEBUG)
			return true;
#else
			return false;
#endif
#elif defined(__OPTIMIZE__)
			return true;
#else
			return false;
#endif
		}

		// Retires the oldest retained frame, giving its readings back to the
		// ring. Two integers and a subtraction; nothing is freed.
		void DropOldestFrame(State &state) {
			if (state.HistoryCount == 0) {
				return;
			}
			state.ReadingsUsed -= state.History[state.HistoryStart].ReadingCount;
			state.HistoryStart = (state.HistoryStart + 1) % FrameGraph::MAXIMUM_HISTORY_FRAMES;
			state.HistoryCount--;
		}

		// Visits one retained frame's readings in the order they were recorded.
		template <typename Visitor>
		void ForEachReading(const State &state, const HistoryFrame &frame, Visitor visit) {
			for (uint32_t offset = 0; offset < frame.ReadingCount; offset++) {
				const size_t at = (frame.FirstReading + offset) % state.Readings.size();
				visit(state.Readings[at]);
			}
		}

		void RecordHistory(
			State &state, float frameMilliseconds, float idleMilliseconds, uint64_t nowNanoseconds
		) {
			for (const auto &span : state.Building) {
				const uint32_t id = HistoryNameId(state, span.Name);
				if (id == NO_NAME) {
					continue;
				}
				if (!state.FrameSeen[id]) {
					state.FrameSeen[id] = 1;
					state.FrameTouched.push_back(id);
				}
				// **The busy part, matching the column it sits beside.** RMAX taken
				// from inclusive time would put 20 ms next to a span whose readable
				// cost is 0.15, and the worst frame of a vsync wait is a worse
				// vsync wait rather than anything to act on.
				const float busy = std::max(span.Milliseconds - span.IdleMilliseconds, 0.0f);
				state.FrameMaximums[id] = std::max(state.FrameMaximums[id], busy);
			}

			// Every tracked span gets a slot this frame, not only the ones that
			// ran: a span absent from this frame scored zero in it, and without
			// writing that, the ring would keep a stale reading alive forever.
			for (size_t id = 0; id < state.RecentRings.size(); id++) {
				state.RecentRings[id][state.RecentCursor] = state.FrameMaximums[id];
			}
			state.RecentCursor = (state.RecentCursor + 1) % FrameGraph::RECENT_FRAMES;

			// **Room is made before the frame is placed, and it is made in two
			// currencies.** The window is bounded by frames *and* by readings,
			// and at a few thousand frames a second the second bound is the one
			// that binds: twenty thousand frames of fifty spans is a million
			// readings, and the ring holds a quarter of that. Whichever runs
			// out first evicts from the same end.
			const size_t wanted = std::min(state.FrameTouched.size(), state.Readings.size());
			while (state.HistoryCount == FrameGraph::MAXIMUM_HISTORY_FRAMES) {
				DropOldestFrame(state);
			}
			while (state.HistoryCount > 0 && state.ReadingsUsed + wanted > state.Readings.size()) {
				DropOldestFrame(state);
			}

			const size_t slot =
				(state.HistoryStart + state.HistoryCount) % FrameGraph::MAXIMUM_HISTORY_FRAMES;
			state.HistoryCount++;

			HistoryFrame &frame = state.History[slot];
			frame.Seconds = static_cast<double>(nowNanoseconds) / 1'000'000'000.0;
			frame.Milliseconds = frameMilliseconds;
			frame.IdleMilliseconds = idleMilliseconds;
			frame.FirstReading = static_cast<uint32_t>(state.ReadingsHead);
			frame.ReadingCount = 0;

			for (uint32_t id : state.FrameTouched) {
				if (frame.ReadingCount >= wanted) {
					// One frame with more distinct spans than the whole ring
					// holds. Counted rather than allowed to wrap over its own
					// slice, which would make a frame's readings the *last* few
					// it recorded and say nothing about it.
					state.HistoryReadingsDropped++;
					continue;
				}
				state.Readings[state.ReadingsHead] = HistoryReading{id, state.FrameMaximums[id]};
				state.ReadingsHead = (state.ReadingsHead + 1) % state.Readings.size();
				frame.ReadingCount++;
			}
			state.ReadingsUsed += frame.ReadingCount;

			// Whatever fell out of the time window. Dropping from the front of a
			// ring is two indices, which is the reason it is a ring.
			while (state.HistoryCount > 1 &&
				   frame.Seconds - state.History[state.HistoryStart].Seconds > FrameGraph::HISTORY_SECONDS) {
				DropOldestFrame(state);
			}

			for (uint32_t id : state.FrameTouched) {
				state.FrameMaximums[id] = 0.0f;
				state.FrameSeen[id] = 0;
			}
			state.FrameTouched.clear();
		}

		// Somewhere to keep a runtime name for the rest of the frame. Assigning
		// into an existing string reuses its buffer; the deque node it lives in
		// never moves, so the view handed back stays good.
		std::string_view Intern(State &state, std::string_view name) {
			if (state.BuildingNameCount == state.BuildingNames.size()) {
				state.BuildingNames.emplace_back(name);
			} else {
				state.BuildingNames[state.BuildingNameCount].assign(name);
			}
			return state.BuildingNames[state.BuildingNameCount++];
		}

		// Nearest-rank, on a copy the caller owns. Not interpolated: with
		// thousands of readings the neighbouring ones are indistinguishable, and
		// an interpolated p99 can report a number no frame actually took.
		float Percentile(std::vector<float> &readings, double fraction) {
			if (readings.empty()) {
				return 0.0f;
			}
			std::sort(readings.begin(), readings.end());
			const auto rank = static_cast<size_t>(fraction * static_cast<double>(readings.size() - 1) + 0.5);
			return readings[std::min(rank, readings.size() - 1)];
		}

		// How many of the worst frames the snapshot lists individually. The
		// summary says a span spikes; these say which frames, so two spans
		// spiking together can be told from two spiking apart.
		constexpr size_t WORST_FRAMES = 40;
		// Spans listed per worst frame, largest first.
		constexpr size_t WORST_FRAME_SPANS = 6;
	}

	std::string_view GetTriggerSubjectName(TriggerSubject subject) {
		switch (subject) {
		case TriggerSubject::Span:
			return "span";
		case TriggerSubject::SpanSelf:
			return "span self";
		case TriggerSubject::Category:
			return "category";
		case TriggerSubject::Frame:
			return "frame";
		case TriggerSubject::Unmarked:
			return "unmarked";
		case TriggerSubject::Dropped:
			return "dropped";
		}
		return "?";
	}

	std::string_view GetTriggerTestName(TriggerTest test) {
		switch (test) {
		case TriggerTest::Above:
			return "over";
		case TriggerTest::Below:
			return "under";
		}
		return "?";
	}

	std::string_view GetCategoryName(ProfileCategory category) {
		switch (category) {
		case ProfileCategory::Engine:
			return "engine";
		case ProfileCategory::Render:
			return "render";
		case ProfileCategory::Gpu:
			return "GPU";
		case ProfileCategory::ECS:
			return "ECS";
		case ProfileCategory::Physics:
			return "physics";
		case ProfileCategory::Simulation:
			return "sim";
		case ProfileCategory::Idle:
			return "IDLE";
		case ProfileCategory::Script:
			return "script";
		case ProfileCategory::Network:
			return "net";
		case ProfileCategory::Assets:
			return "assets";
		case ProfileCategory::Count:
			break;
		}
		return "?";
	}

	std::string_view GetProfileOwnerName(ProfileOwner owner) {
		switch (owner) {
		case ProfileOwner::All:
			return "all";
		case ProfileOwner::Engine:
			return "engine";
		case ProfileOwner::Server:
			return "server";
		case ProfileOwner::Client:
			return "client";
		case ProfileOwner::Studio:
			return "studio";
		case ProfileOwner::Count:
			break;
		}
		return "?";
	}

	void FrameGraph::SetEnabled(bool enabled) {
		auto &state = Get();
		if (state.Enabled == enabled) {
			return;
		}

		state.Enabled = enabled;

		// The window is about what is being watched now. Collection only runs
		// while the panel is open, so keeping frames from the last time it was
		// open would put a gap of arbitrary length in the middle of a five
		// second snapshot and give the recent maximum a reading from a
		// different scene.
		ClearHistory(state);

		if (enabled) {
			// The retained window's storage, taken when the panel opens rather
			// than on the first frame it collects.
			//
			// Twenty thousand frames is not a large allocation, but it was
			// happening inside `EndFrame` - so the first frame anybody watched
			// was the one frame in the session that paid for it, and the panel
			// opened onto a spike it had caused. Opening a profiler is a
			// keypress and is allowed to cost something; the frame after it is
			// a measurement.
			//
			// Not released on the way out. Freeing twenty thousand frames of
			// span lists would put the same hitch back the next time the panel
			// opened, in exchange for memory nothing else is contending for.
			state.History.resize(FrameGraph::MAXIMUM_HISTORY_FRAMES);

			// The readings the frames above index into. Two allocations for the
			// whole retained window, taken when the panel opens and never
			// repeated - where the per-frame vectors this replaced were twenty
			// thousand, taken over the first twenty thousand frames, and grown
			// again whenever a slot met a busier frame than it had held before.
			state.Readings.assign(FrameGraph::MAXIMUM_HISTORY_READINGS, HistoryReading{});
		} else {
			state.Published.clear();
			state.PublishedMilliseconds = 0.0f;
			state.PublishedUnmarked = 0.0f;
			state.PublishedDropped = 0;
			for (auto &total : state.PublishedCategories) {
				total = 0.0f;
			}
		}
	}

	bool FrameGraph::IsEnabled() {
		return Get().Enabled;
	}

	void FrameGraph::BeginFrame() {
		auto &state = Get();
		if (!state.Enabled) {
			state.Recording = false;
			return;
		}

		// Recording starts at a frame boundary and never partway through one.
		// A frame that began before the panel opened is missing everything that
		// ran before the switch, and published it would read as one enormous
		// frame with three spans in it.
		state.Recording = true;
		state.Owner.store(std::this_thread::get_id(), std::memory_order_relaxed);
		state.FrameStartNanoseconds = Clock::Nanoseconds();
		state.Building.clear();
		state.Open.clear();
		state.Depth = 0;
		state.BuildingNameCount = 0;
		state.DroppedThisFrame.store(0, std::memory_order_relaxed);

		// Reserving once, on the first enabled frame, keeps the allocation out
		// of every subsequent measurement.
		if (state.Building.capacity() < MAXIMUM_SPANS) {
			state.Building.reserve(MAXIMUM_SPANS);
		}
		if (state.ReportedChildEnds.capacity() < MAXIMUM_SPANS) {
			state.ReportedChildEnds.reserve(MAXIMUM_SPANS);
		}
	}

	void AccumulateIdleMilliseconds(std::span<FrameSpan> spans) {
		for (size_t index = 0; index < spans.size(); index++) {
			if (spans[index].Category != ProfileCategory::Idle) {
				continue;
			}

			// Self time rather than inclusive, so a wait nested inside another
			// wait is not counted twice.
			const float waited = spans[index].SelfMilliseconds;
			spans[index].IdleMilliseconds += waited;

			// Walked to the root rather than added to one parent: every
			// ancestor contains this wait, not only the immediate one. Bounded
			// by the span count as well as by depth, because a malformed parent
			// chain must not hang the frame that produced it.
			size_t node = index;
			for (size_t step = 0; step < spans.size(); step++) {
				const size_t parent = spans[node].Parent;
				if (parent == node || parent >= spans.size()) {
					break;
				}
				node = parent;
				spans[node].IdleMilliseconds += waited;
			}
		}
	}

	void AccumulateFoldedStacks(std::span<const FrameSpan> spans, FoldedStacks &totals) {
		// The stack of every span, built as the walk goes. Spans are in open
		// order, so a parent is always an earlier entry and its path is already
		// here - which is what makes this one pass rather than a climb per span.
		std::vector<std::string> paths;
		paths.reserve(spans.size());

		for (size_t index = 0; index < spans.size(); index++) {
			const FrameSpan &span = spans[index];

			std::string path;
			if (span.Parent < index) {
				path.reserve(paths[span.Parent].size() + span.Name.size() + 1);
				path.assign(paths[span.Parent]);
				path.push_back(';');
			}
			// A semicolon inside a name would read as a second frame, so it is
			// replaced rather than left to split the stack. Runtime names come
			// from scripts and asset paths, which is where one would come from.
			for (const char letter : span.Name) {
				path.push_back(letter == ';' ? ':' : letter);
			}

			// Negative self time is possible on a malformed tree and would
			// subtract width from a stack that really ran.
			if (span.SelfMilliseconds > 0.0f) {
				totals[path] += static_cast<double>(span.SelfMilliseconds) * 1000.0;
			} else {
				// Still recorded, at zero, so a stack that only ever costs
				// rounding is visible as a name rather than absent.
				totals.try_emplace(path, 0.0);
			}

			paths.push_back(std::move(path));
		}
	}

	bool WriteFoldedStacks(const std::filesystem::path &path, const FoldedStacks &totals) {
		std::ofstream out(path, std::ios::trunc);
		if (!out) {
			return false;
		}

		for (const auto &[stack, microseconds] : totals) {
			const auto whole = static_cast<uint64_t>(microseconds + 0.5);
			if (whole == 0) {
				continue;
			}
			out << stack << ' ' << whole << '\n';
		}

		return out.good();
	}

	namespace {

		// Reads what one rule asks for out of the frame that has just been
		// measured.
		//
		// `state.Building` is still the frame being described - `EndFrame` calls
		// this before the publish swap - and the category totals and the idle
		// accounting are already in it.
		//
		// @param state The collector, mid-`EndFrame`.
		// @param rule  What to read.
		// @param total The frame's wall clock, which is not published yet.
		// @return The reading, in milliseconds or a count for `Dropped`, or
		//         nothing when the rule names a span this frame did not run.
		//
		// **Nothing rather than zero for an absent span, and it is the `under`
		// rules that need it.** "`pump events` under 1 ms" is a question about a
		// frame that pumped events; answering it with the zero of a frame that
		// did not would stop the graph on the first frame where nothing
		// happened, which is every frame before the one somebody is waiting for.
		std::optional<float> ReadTrigger(const State &state, const FrameTrigger &rule, float total) {
			switch (rule.Subject) {
			case TriggerSubject::Span:
			case TriggerSubject::SpanSelf: {
				// Totalled over every occurrence rather than taking the
				// worst, because a span that opens forty times in a bad
				// frame is a bad frame even when no single one of the forty
				// is slow. That is the reported case exactly: `pump events`
				// is one span per event.
				float found = 0.0f;
				bool ran = false;
				for (const FrameSpan &span : state.Building) {
					if (span.Name == rule.Name) {
						found +=
							rule.Subject == TriggerSubject::Span ? span.Milliseconds : span.SelfMilliseconds;
						ran = true;
					}
				}
				if (!ran) {
					return std::nullopt;
				}
				return found;
			}

			case TriggerSubject::Category:
				return state.PublishedCategories[static_cast<size_t>(rule.Category)];

			case TriggerSubject::Frame:
				return total;

			case TriggerSubject::Unmarked:
				return state.PublishedUnmarked;

			case TriggerSubject::Dropped:
				return static_cast<float>(state.DroppedThisFrame.load(std::memory_order_relaxed));
			}
			return 0.0f;
		}

		// What a rule was reading, spelled for whoever reads the hit.
		std::string DescribeTrigger(const FrameTrigger &rule) {
			switch (rule.Subject) {
			case TriggerSubject::Span:
				return rule.Name;
			case TriggerSubject::SpanSelf:
				return rule.Name + " (self)";
			case TriggerSubject::Category:
				return std::string(GetCategoryName(rule.Category));
			case TriggerSubject::Frame:
				return "frame";
			case TriggerSubject::Unmarked:
				return "unmarked";
			case TriggerSubject::Dropped:
				return "dropped spans";
			}
			return "";
		}

		// Arms the latch on the first rule this frame meets.
		//
		// First rather than worst: the rules are a list somebody wrote in an
		// order, and a reader who is told two things fired at once has to work
		// out which one they care about. One rule, one reading, one frame.
		void EvaluateTriggers(State &state, float total) {
			for (size_t index = 0; index < state.Triggers.size(); index++) {
				const FrameTrigger &rule = state.Triggers[index];
				if (!rule.Enabled) {
					continue;
				}

				const std::optional<float> reading = ReadTrigger(state, rule, total);
				if (!reading.has_value()) {
					continue;
				}

				const bool met =
					rule.Test == TriggerTest::Above ? *reading > rule.Threshold : *reading < rule.Threshold;
				if (!met) {
					continue;
				}

				state.Hit.Rule = index;
				state.Hit.Reading = *reading;
				state.Hit.Threshold = rule.Threshold;
				state.Hit.Subject = DescribeTrigger(rule);
				state.Latched = true;
				return;
			}
		}
	}

	void FrameGraph::SetTriggers(std::span<const FrameTrigger> triggers) {
		State &state = Get();
		state.Triggers.assign(triggers.begin(), triggers.end());
	}

	const FrameTriggerHit *FrameGraph::Triggered() {
		const State &state = Get();
		return state.Latched ? &state.Hit : nullptr;
	}

	void FrameGraph::ClearTrigger() {
		Get().Latched = false;
	}

	void FrameGraph::SetFoldingEnabled(bool enabled) {
		auto &state = Get();
		if (enabled) {
			state.Folded.clear();
			state.FoldedFrameCount = 0;
		}
		state.Folding = enabled;
	}

	bool FrameGraph::IsFolding() {
		return Get().Folding;
	}

	size_t FrameGraph::FoldedFrames() {
		return Get().FoldedFrameCount;
	}

	bool FrameGraph::WriteFolded(const std::filesystem::path &path) {
		auto &state = Get();
		if (state.Folded.empty()) {
			return false;
		}
		return WriteFoldedStacks(path, state.Folded);
	}

	void FrameGraph::EndFrame() {
		auto &state = Get();
		if (!state.Recording) {
			return;
		}
		state.Recording = false;

		const uint64_t now = Clock::Nanoseconds();
		const float total = MillisecondsSince(state.FrameStartNanoseconds);

		// A scope still open at EndFrame is a missing destructor, or an early
		// return inside one. Closed against the frame boundary rather than
		// published with a zero duration, which would read as "free".
		for (auto index : state.Open) {
			auto &span = state.Building[index];
			span.Milliseconds = total - span.StartMilliseconds;
		}
		state.Open.clear();
		state.Depth = 0;

		// Reported spans are reconstructed after their worker has joined. Their
		// wall-clock start is therefore the tiny call that builds the report, not
		// where the work belongs in its parent. Place reported children after the
		// preceding direct child so scheduler phases and systems remain adjacent.
		state.ReportedChildEnds.assign(state.Building.size(), 0.0f);
		for (size_t index = 0; index < state.Building.size(); index++) {
			FrameSpan &span = state.Building[index];
			if (span.Parent < index) {
				FrameSpan &parent = state.Building[span.Parent];
				float &childEnd = state.ReportedChildEnds[span.Parent];
				if (childEnd < parent.StartMilliseconds) {
					childEnd = parent.StartMilliseconds;
				}
				if (span.Reported) {
					span.StartMilliseconds = childEnd;
				}
				childEnd = std::max(childEnd, span.StartMilliseconds + span.Milliseconds);
			}
			state.ReportedChildEnds[index] = span.StartMilliseconds;
		}

		// Self time: a span's duration less the duration of its direct
		// children. Spans are in open order, so a child is always a later entry
		// with a greater depth, up to the next entry at the same depth or less.
		for (size_t index = 0; index < state.Building.size(); index++) {
			auto &span = state.Building[index];
			float children = 0.0f;
			for (size_t scan = index + 1; scan < state.Building.size(); scan++) {
				const auto &candidate = state.Building[scan];
				if (candidate.Depth <= span.Depth) {
					break;
				}
				// A reported child is time from another thread or another
				// process. Subtracting it would give a parent a negative self
				// time as soon as the work it dispatched outran the wall clock
				// it waited for - which is the normal case for anything
				// parallel, not an edge one.
				if (candidate.Depth == span.Depth + 1 && (!candidate.Reported || span.Summary)) {
					children += candidate.Milliseconds;
				}
			}
			span.SelfMilliseconds = std::max(span.Milliseconds - children, 0.0f);
		}

		// Idle inside: what part of each span's inclusive time was waiting.
		//
		// Here rather than in the overlay because two consumers need it and one
		// of them is `RecordHistory` below - an RMAX taken from wall time and a
		// share taken from busy time are two numbers on one row that disagree,
		// which is exactly the confusion this was added to end.
		AccumulateIdleMilliseconds(state.Building);

		for (auto &accumulated : state.PublishedCategories) {
			accumulated = 0.0f;
		}
		for (const auto &span : state.Building) {
			state.PublishedCategories[static_cast<size_t>(span.Category)] += span.SelfMilliseconds;
		}

		// The frame's own self time: what ran between BeginFrame and EndFrame
		// and inside no scope at all.
		//
		// Every other number here is the self time of something somebody named.
		// This is the self time of the frame, and it is the one nobody was going
		// to notice was missing - a panel that lists 0.3 ms of spans under a
		// heading that says 1.1 ms is not reporting 0.3 ms of work, it is
		// failing to report 0.8 ms of it.
		//
		// Root spans only. Their durations are inclusive, so adding the deeper
		// ones would count the same nanoseconds again.
		float marked = 0.0f;
		for (const auto &span : state.Building) {
			if (span.Depth == 0) {
				marked += span.Milliseconds;
			}
		}
		// Clamped. Two readings of the same clock taken either side of a tree of
		// spans should not disagree, but a negative gap on a panel would read as
		// a profiler bug rather than as the rounding it is.
		state.PublishedUnmarked = total > marked ? total - marked : 0.0f;

		// Before the swap, on the frame that was just recorded. The folding
		// needs the same thing the history does - the tree, after the self time
		// and the idle accounting are in it.
		// The waiting, taken from the category total the loop above just
		// rebuilt. `AccumulateIdleMilliseconds` has already run, so every `Idle`
		// span's self time is in it and nested waits are counted once.
		RecordHistory(
			state, total, state.PublishedCategories[static_cast<size_t>(ProfileCategory::Idle)], now
		);

		if (state.Folding) {
			AccumulateFoldedStacks(state.Building, state.Folded);
			state.FoldedFrameCount++;
		}

		// **Here, and not in the panel.** The panel samples four times a second
		// at the shortest interval it offers; the frame a rule is written for is
		// one frame long. Where the tree still exists is the only place a rule
		// can see the frame that broke it.
		//
		// Before the swap, so `Building` is the frame being described, and after
		// the category totals and `AccumulateIdleMilliseconds` so a rule may
		// read either.
		if (!state.Triggers.empty() && !state.Latched) {
			EvaluateTriggers(state, total);
		}

		state.Published.swap(state.Building);
		state.PublishedMilliseconds = total;
		state.PublishedDropped = state.DroppedThisFrame.load(std::memory_order_relaxed);

		// **Reported rather than left silent**, which is what `docs/ARCH_REVIEW.md`
		// §A1 made this counter atomic for and what §G2 asks of it. This
		// collector records one thread by design, so every scope opened inside
		// an `EachParallel` body is dropped - and until v0.19 the only thing
		// that could see that was the F5 overlay, which a headless server does
		// not have and a headless server is exactly the program that runs
		// parallel compute.
		//
		// Only when something was dropped, so a run whose report has no such
		// row is a run that lost no spans, rather than one nobody can tell
		// apart from a row of zeroes.
		if (state.PublishedDropped > 0) {
			Metrics::Count(FrameGraph::DROPPED_COUNTER, static_cast<double>(state.PublishedDropped));
		}
		// The names go with the spans that view into them.
		state.PublishedNames.swap(state.BuildingNames);
		state.BuildingNameCount = 0;
	}

	const std::vector<FrameSpan> &FrameGraph::Spans() {
		return Get().Published;
	}

	float FrameGraph::FrameMilliseconds() {
		return Get().PublishedMilliseconds;
	}

	float FrameGraph::UnmarkedMilliseconds() {
		return Get().PublishedUnmarked;
	}

	float FrameGraph::CategoryMilliseconds(ProfileCategory category) {
		if (category >= ProfileCategory::Count) {
			return 0.0f;
		}
		return Get().PublishedCategories[static_cast<size_t>(category)];
	}

	size_t FrameGraph::Dropped() {
		return Get().PublishedDropped;
	}

	float FrameGraph::RecentMaximum(std::string_view name) {
		auto &state = Get();

		auto found = state.HistoryNameIds.find(name);
		if (found == state.HistoryNameIds.end()) {
			return 0.0f;
		}

		float worst = 0.0f;
		for (float reading : state.RecentRings[found->second]) {
			worst = std::max(worst, reading);
		}
		return worst;
	}

	size_t FrameGraph::HistoryFrames() {
		return Get().HistoryCount;
	}

	double FrameGraph::HistorySeconds() {
		auto &state = Get();
		if (state.HistoryCount < 2) {
			return 0.0;
		}
		const size_t last = (state.HistoryStart + state.HistoryCount - 1) % MAXIMUM_HISTORY_FRAMES;
		return state.History[last].Seconds - state.History[state.HistoryStart].Seconds;
	}

	bool FrameGraph::WriteSnapshot(const std::filesystem::path &path) {
		auto &state = Get();
		if (state.HistoryCount == 0) {
			return false;
		}

		std::ofstream out(path, std::ios::trunc);
		if (!out) {
			return false;
		}

		// Gathered by name id, in one pass over the window.
		std::vector<std::vector<float>> readings(state.HistoryNames.size());
		std::vector<float> frameMilliseconds;
		frameMilliseconds.reserve(state.HistoryCount);

		const double firstSeconds = state.History[state.HistoryStart].Seconds;
		for (size_t offset = 0; offset < state.HistoryCount; offset++) {
			const HistoryFrame &frame = state.History[(state.HistoryStart + offset) % MAXIMUM_HISTORY_FRAMES];
			frameMilliseconds.push_back(frame.Milliseconds);
			ForEachReading(state, frame, [&](const HistoryReading &reading) {
				readings[reading.Name].push_back(reading.Milliseconds);
			});
		}

		double frameTotal = 0.0;
		float frameWorst = 0.0f;
		for (float milliseconds : frameMilliseconds) {
			frameTotal += milliseconds;
			frameWorst = std::max(frameWorst, milliseconds);
		}

		// The waiting, gathered the same way the frame times were.
		std::vector<float> idleMilliseconds;
		idleMilliseconds.reserve(state.HistoryCount);
		double idleTotal = 0.0;
		for (size_t index = 0; index < state.HistoryCount; index++) {
			const float waited =
				state.History[(state.HistoryStart + index) % MAXIMUM_HISTORY_FRAMES].IdleMilliseconds;
			idleMilliseconds.push_back(waited);
			idleTotal += waited;
		}

		char line[512];
		out << "atomic frame graph snapshot\n";

		// **Said first, because every number below it is wrong without it.**
		// The `dev` preset compiles the engine at `-O0` while the vendored code
		// keeps `-O2`, which is the right trade for a build you iterate on and
		// the wrong one to read a profile from. Measured on one scene, the same
		// display, the same frame, `dev` against `release`: `convert instances`
		// 11.99 ms against 0.179, `graph.light-bounds` 10.18 against 0.120,
		// `sync rendered` 26.5 against 0.74. Between thirty and eighty times,
		// span by span.
		//
		// A reader without this line sees a plausible-looking frame and spends a
		// day optimising the compiler's missing inliner. The panel already warns
		// about forced serial compute for exactly this reason and this is the
		// larger of the two.
		if (!OptimisedBuild()) {
			out << "WARNING  this build is unoptimised - every figure below is "
				   "tens of times its shipped cost\n";
		}
		std::snprintf(
			line,
			sizeof(line),
			"window   %zu frames over %.3f s (bounds: %.1f s, %zu frames)\n",
			state.HistoryCount,
			HistorySeconds(),
			HISTORY_SECONDS,
			MAXIMUM_HISTORY_FRAMES
		);
		out << line;

		std::vector<float> frameSorted = frameMilliseconds;
		const float p99 = Percentile(frameSorted, 0.99);
		const float p50 = Percentile(frameSorted, 0.50);
		std::snprintf(
			line,
			sizeof(line),
			"frame ms mean %.3f  max %.3f  p99 %.3f  p50 %.3f\n",
			frameTotal / static_cast<double>(state.HistoryCount),
			frameWorst,
			p99,
			p50
		);
		out << line;

		// **Beside the frame total, because the span table below cannot say
		// this.** Every span reading there is *busy* time, and an `Idle` span's
		// busy time is zero by construction - so a sixteen-millisecond frame
		// that was all vsync wait and one that was all work show the same rows.
		// Reading the table and finding nothing that adds up to the frame is the
		// correct outcome, and this is the line that says why.
		std::vector<float> idleSorted = idleMilliseconds;
		std::snprintf(
			line,
			sizeof(line),
			"idle ms  mean %.3f  p99 %.3f  p50 %.3f   (waiting; the span rows below are busy time)\n",
			state.HistoryCount == 0 ? 0.0 : idleTotal / static_cast<double>(state.HistoryCount),
			Percentile(idleSorted, 0.99),
			Percentile(idleSorted, 0.50)
		);
		out << line;

		if (state.HistoryNamesDropped > 0) {
			std::snprintf(
				line,
				sizeof(line),
				"WARNING  %zu span names past the %zu tracked were not recorded\n",
				state.HistoryNamesDropped,
				MAXIMUM_HISTORY_NAMES
			);
			out << line;
		}

		// Every number below is that span's worst single reading in a frame,
		// the same quantity the panel's RMAX column shows. Not a per-frame
		// total: a span that opens once per camera would otherwise read as the
		// sum of six cameras and not compare with anything else here.
		out << "\nper span, worst reading in a frame. sorted by max.\n";
		std::snprintf(
			line, sizeof(line), "%-34s %7s %8s %8s %8s %8s\n", "span", "frames", "mean", "p50", "p99", "max"
		);
		out << line;

		std::vector<uint32_t> order;
		for (uint32_t id = 0; id < static_cast<uint32_t>(readings.size()); id++) {
			if (!readings[id].empty()) {
				order.push_back(id);
			}
		}
		std::sort(order.begin(), order.end(), [&readings](uint32_t left, uint32_t right) {
			return *std::max_element(readings[left].begin(), readings[left].end()) >
				   *std::max_element(readings[right].begin(), readings[right].end());
		});

		for (uint32_t id : order) {
			const std::vector<float> &span = readings[id];
			double total = 0.0;
			float worst = 0.0f;
			for (float milliseconds : span) {
				total += milliseconds;
				worst = std::max(worst, milliseconds);
			}
			std::vector<float> sorted = span;
			const float spanP50 = Percentile(sorted, 0.50);
			const float spanP99 = Percentile(sorted, 0.99);
			std::snprintf(
				line,
				sizeof(line),
				"%-34s %7zu %8.3f %8.3f %8.3f %8.3f\n",
				state.HistoryNames[id].c_str(),
				span.size(),
				total / static_cast<double>(span.size()),
				spanP50,
				spanP99,
				worst
			);
			out << line;
		}

		// The worst frames themselves. The summary says a span spikes; this says
		// whether it spikes alone or whether the whole frame went with it.
		std::vector<size_t> worstOrder(state.HistoryCount);
		for (size_t offset = 0; offset < state.HistoryCount; offset++) {
			worstOrder[offset] = offset;
		}
		std::sort(worstOrder.begin(), worstOrder.end(), [&frameMilliseconds](size_t left, size_t right) {
			return frameMilliseconds[left] > frameMilliseconds[right];
		});

		const size_t listed = std::min(WORST_FRAMES, state.HistoryCount);
		std::snprintf(line, sizeof(line), "\nworst %zu frames, and the biggest spans in each\n", listed);
		out << line;

		for (size_t rank = 0; rank < listed; rank++) {
			const HistoryFrame &frame =
				state.History[(state.HistoryStart + worstOrder[rank]) % MAXIMUM_HISTORY_FRAMES];

			std::snprintf(
				line, sizeof(line), "  +%8.4f s  %8.3f ms ", frame.Seconds - firstSeconds, frame.Milliseconds
			);
			out << line;

			std::vector<HistoryReading> spans;
			spans.reserve(frame.ReadingCount);
			ForEachReading(state, frame, [&](const HistoryReading &reading) { spans.push_back(reading); });
			std::sort(
				spans.begin(), spans.end(), [](const HistoryReading &left, const HistoryReading &right) {
					return left.Milliseconds > right.Milliseconds;
				}
			);

			for (size_t index = 0; index < spans.size() && index < WORST_FRAME_SPANS; index++) {
				std::snprintf(
					line,
					sizeof(line),
					" %s %.3f",
					state.HistoryNames[spans[index].Name].c_str(),
					spans[index].Milliseconds
				);
				out << line;
			}
			out << "\n";
		}

		return out.good();
	}

	// --- scopes ----------------------------------------------------------------

	// Shared by both scope kinds. Returns the index of the span it opened, or a
	// sentinel; the depth moves either way, so the matching Pop stays balanced.
	size_t FrameGraph::Push(std::string_view name, ProfileCategory category, ProfileOwner owner) {
		auto &state = Get();
		if (!state.Recording) {
			return NOT_RECORDING;
		}

		// Worker threads are Tracy's job. Counting them here rather than
		// locking keeps the main-thread path free of contention, and the
		// overlay says so instead of quietly under-reporting.
		//
		// Not depth-tracked either: the depth belongs to the owning thread's
		// stack, and a worker moving it would corrupt that thread's nesting.
		if (std::this_thread::get_id() != state.Owner.load(std::memory_order_relaxed)) {
			state.DroppedThisFrame.fetch_add(1, std::memory_order_relaxed);
			return NOT_RECORDING;
		}

		// Deeper than the budget, or out of buffer. The depth still has to move
		// so the matching close moves it back - otherwise every sibling after a
		// dropped span is recorded one level too deep.
		if (state.Depth >= MAXIMUM_DEPTH || state.Building.size() >= MAXIMUM_SPANS) {
			state.DroppedThisFrame.fetch_add(1, std::memory_order_relaxed);
			state.Depth++;
			return DEPTH_ONLY;
		}

		// Only recorded spans are on the stack, and a span is skipped only when
		// its parent was, so the top of the stack is this one's parent.
		const uint32_t parent = state.Open.empty() ? NO_PARENT : static_cast<uint32_t>(state.Open.back());

		const size_t index = state.Building.size();
		// Named rather than positional. This was seven bare values in
		// declaration order, and adding `IdleMilliseconds` in the middle of the
		// struct silently handed a `ProfileCategory` to a float - caught by the
		// compiler here, and only because the types happened not to convert.
		// The next field added between two floats would not be.
		state.Building.push_back(
			FrameSpan{
				.Name = name,
				.Depth = state.Depth,
				.Parent = parent,
				.StartMilliseconds = MillisecondsSince(state.FrameStartNanoseconds),
				.Milliseconds = 0.0f,
				.SelfMilliseconds = 0.0f,
				.IdleMilliseconds = 0.0f,
				.Category = category,
				.Owner = owner,
			}
		);
		state.Open.push_back(index);
		state.Depth++;
		return index;
	}

	void FrameGraph::Pop(size_t index) {
		if (index == NOT_RECORDING) {
			return;
		}

		auto &state = Get();
		// Before the DEPTH_ONLY return, not after. A scope past the budget still
		// moved the depth, so its close has to move it back - otherwise every
		// sibling after it is recorded several levels too deep.
		if (state.Depth > 0) {
			state.Depth--;
		}

		if (index == DEPTH_ONLY) {
			return;
		}

		if (state.Open.empty() || state.Open.back() != index) {
			// Closed out of order, which is possible only by constructing a
			// scope on the heap - made hard on purpose by the deleted copy and
			// move.
			return;
		}

		auto &span = state.Building[index];
		span.Milliseconds = MillisecondsSince(state.FrameStartNanoseconds) - span.StartMilliseconds;
		state.Open.pop_back();
	}

	FrameGraph::Scope::Scope(std::string_view name, ProfileCategory category, ProfileOwner owner) {
		Index = Push(name, category, owner);
	}

	FrameGraph::Scope::~Scope() {
		Pop(Index);
	}

	FrameGraph::CopiedScope::CopiedScope(
		std::string_view fallback, std::string_view name, ProfileCategory category, ProfileOwner owner
	) {
		auto &state = Get();
		if (!state.Recording || std::this_thread::get_id() != state.Owner.load(std::memory_order_relaxed)) {
			Index = Push(name, category, owner);
			return;
		}

		// Nothing to say yet - the caller looks a name up only when something is
		// listening, and that is decided one frame and acted on the next.
		if (name.empty()) {
			Index = Push(fallback, category, owner);
			return;
		}

		// Checked before interning rather than after: a name is only worth
		// copying when the span that keeps it is going to exist.
		if (state.Depth >= MAXIMUM_DEPTH || state.Building.size() >= MAXIMUM_SPANS) {
			Index = Push(name, category, owner);
			return;
		}

		Index = Push(Intern(state, name), category, owner);
	}

	FrameGraph::ReportedScope::ReportedScope(
		std::string_view name, ProfileCategory category, float milliseconds, ProfileOwner owner
	)
		: Milliseconds(milliseconds) {
		auto &state = Get();
		if (!(milliseconds >= 0.0f)) {
			if (state.Recording &&
				std::this_thread::get_id() == state.Owner.load(std::memory_order_relaxed)) {
				state.DroppedThisFrame.fetch_add(1, std::memory_order_relaxed);
			}
			return;
		}
		Index = Push(name, category, owner);
	}

	FrameGraph::ReportedScope::~ReportedScope() {
		const size_t index = Index;
		Pop(index);
		if (index == NOT_RECORDING || index == DEPTH_ONLY) {
			return;
		}

		auto &state = Get();
		FrameSpan &span = state.Building[index];
		span.Milliseconds = Milliseconds;
		span.Reported = true;
		span.Summary = true;
	}

	void FrameGraph::Report(
		std::string_view name, ProfileCategory category, float milliseconds, ProfileOwner owner
	) {
		auto &state = Get();
		if (!state.Recording) {
			return;
		}

		// The same owner rule `Push` applies, and for the same reason. What is
		// different is only where the number came from: a producer that cannot
		// record its own span hands the duration to whoever can.
		if (std::this_thread::get_id() != state.Owner.load(std::memory_order_relaxed)) {
			state.DroppedThisFrame.fetch_add(1, std::memory_order_relaxed);
			return;
		}

		// Ignored rather than clamped. A negative duration means the producer
		// measured wrongly - two clocks, or a start it never set - and folding
		// it to zero would put a plausible bar on the graph instead of a
		// missing one.
		if (!(milliseconds >= 0.0f)) {
			state.DroppedThisFrame.fetch_add(1, std::memory_order_relaxed);
			return;
		}

		if (state.Depth >= MAXIMUM_DEPTH || state.Building.size() >= MAXIMUM_SPANS) {
			state.DroppedThisFrame.fetch_add(1, std::memory_order_relaxed);
			return;
		}

		// Opened and closed here, so it is a leaf under whatever is currently
		// open. It never becomes anybody's parent: the work it describes
		// happened elsewhere, and nothing recorded on this thread was inside
		// it.
		const size_t index = Push(name, category, owner);
		if (index == NOT_RECORDING || index == DEPTH_ONLY) {
			Pop(index);
			return;
		}

		Pop(index);

		// Overwritten after the close, which is the point: the elapsed time this
		// thread would have measured is the cost of the call, and the number
		// worth showing is the one the producer reported. `EndFrame` does the
		// self-time and category accounting from here as it does for every
		// other span.
		FrameSpan &span = state.Building[index];
		span.Milliseconds = milliseconds;
		span.Reported = true;
	}

	void FrameGraph::ReportNamed(
		std::string_view fallback,
		std::string_view name,
		ProfileCategory category,
		float milliseconds,
		ProfileOwner owner
	) {
		auto &state = Get();
		if (!state.Recording || std::this_thread::get_id() != state.Owner.load(std::memory_order_relaxed) ||
			name.empty()) {
			Report(fallback, category, milliseconds, owner);
			return;
		}

		if (state.Depth >= MAXIMUM_DEPTH || state.Building.size() >= MAXIMUM_SPANS) {
			Report(fallback, category, milliseconds, owner);
			return;
		}

		Report(Intern(state, name), category, milliseconds, owner);
	}
}
