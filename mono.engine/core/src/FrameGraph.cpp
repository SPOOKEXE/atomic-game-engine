#include <engine/core/Clock.hpp>
#include <engine/core/FrameGraph.hpp>

#include <algorithm>
#include <cstdio>
#include <deque>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <utility>

namespace engine::core {

	namespace {

		constexpr uint32_t NO_NAME = UINT32_MAX;

		struct HistoryFrame {
			double Seconds = 0.0;
			float Milliseconds = 0.0f;
			// Only the spans the frame actually had, as (name id, worst
			// reading). A frame of six spans costs six pairs rather than a row
			// per tracked name.
			std::vector<std::pair<uint32_t, float>> Spans;
		};

		struct State {
			bool Enabled = false;
			bool Recording = false;

			std::thread::id Owner;
			uint64_t FrameStartNanoseconds = 0;

			// Built during the frame.
			std::vector<FrameSpan> Building;
			std::vector<size_t> Open;
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

			size_t DroppedThisFrame = 0;
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
			// which ids the frame touched — so it clears in the size of the
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
		};

		State &Get() {
			static State state;
			return state;
		}

		float MillisecondsSince(uint64_t start) {
			return static_cast<float>(static_cast<double>(Clock::Nanoseconds() - start) / 1'000'000.0);
		}

		void ClearHistory(State &state) {
			for (auto &ring : state.RecentRings) {
				std::fill(ring.begin(), ring.end(), 0.0f);
			}
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
		void RecordHistory(State &state, float frameMilliseconds, uint64_t nowNanoseconds) {
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

			if (state.History.size() < FrameGraph::MAXIMUM_HISTORY_FRAMES) {
				state.History.resize(FrameGraph::MAXIMUM_HISTORY_FRAMES);
			}

			const size_t slot =
				(state.HistoryStart + state.HistoryCount) % FrameGraph::MAXIMUM_HISTORY_FRAMES;
			if (state.HistoryCount == FrameGraph::MAXIMUM_HISTORY_FRAMES) {
				state.HistoryStart = (state.HistoryStart + 1) % FrameGraph::MAXIMUM_HISTORY_FRAMES;
			} else {
				state.HistoryCount++;
			}

			HistoryFrame &frame = state.History[slot];
			frame.Seconds = static_cast<double>(nowNanoseconds) / 1'000'000'000.0;
			frame.Milliseconds = frameMilliseconds;
			frame.Spans.clear();
			for (uint32_t id : state.FrameTouched) {
				frame.Spans.emplace_back(id, state.FrameMaximums[id]);
			}

			// Whatever fell out of the window. Dropping from the front of a ring
			// is one index, which is the reason it is a ring.
			while (state.HistoryCount > 1 &&
				   frame.Seconds - state.History[state.HistoryStart].Seconds > FrameGraph::HISTORY_SECONDS) {
				state.HistoryStart = (state.HistoryStart + 1) % FrameGraph::MAXIMUM_HISTORY_FRAMES;
				state.HistoryCount--;
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

	std::string_view GetCategoryName(ProfileCategory category) {
		switch (category) {
		case ProfileCategory::Engine:
			return "engine";
		case ProfileCategory::Render:
			return "render";
		case ProfileCategory::ECS:
			return "ECS";
		case ProfileCategory::Simulation:
			return "sim";
		case ProfileCategory::Idle:
			return "IDLE";
		case ProfileCategory::Script:
			return "script";
		case ProfileCategory::Count:
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

		if (!enabled) {
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
		state.Owner = std::this_thread::get_id();
		state.FrameStartNanoseconds = Clock::Nanoseconds();
		state.Building.clear();
		state.Open.clear();
		state.Depth = 0;
		state.BuildingNameCount = 0;
		state.DroppedThisFrame = 0;

		// Reserving once, on the first enabled frame, keeps the allocation out
		// of every subsequent measurement.
		if (state.Building.capacity() < MAXIMUM_SPANS) {
			state.Building.reserve(MAXIMUM_SPANS);
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
				// it waited for — which is the normal case for anything
				// parallel, not an edge one.
				if (candidate.Depth == span.Depth + 1 && !candidate.Reported) {
					children += candidate.Milliseconds;
				}
			}
			span.SelfMilliseconds = span.Milliseconds - children;
		}

		// Idle inside: what part of each span's inclusive time was waiting.
		//
		// Here rather than in the overlay because two consumers need it and one
		// of them is `RecordHistory` below — an RMAX taken from wall time and a
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
		// to notice was missing — a panel that lists 0.3 ms of spans under a
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

		// Before the swap, on the frame that was just recorded.
		RecordHistory(state, total, now);

		state.Published.swap(state.Building);
		state.PublishedMilliseconds = total;
		state.PublishedDropped = state.DroppedThisFrame;
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
			for (const auto &[id, milliseconds] : frame.Spans) {
				readings[id].push_back(milliseconds);
			}
		}

		double frameTotal = 0.0;
		float frameWorst = 0.0f;
		for (float milliseconds : frameMilliseconds) {
			frameTotal += milliseconds;
			frameWorst = std::max(frameWorst, milliseconds);
		}

		char line[512];
		out << "atomic frame graph snapshot\n";
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

			std::vector<std::pair<uint32_t, float>> spans = frame.Spans;
			std::sort(spans.begin(), spans.end(), [](const auto &left, const auto &right) {
				return left.second > right.second;
			});

			for (size_t index = 0; index < spans.size() && index < WORST_FRAME_SPANS; index++) {
				std::snprintf(
					line,
					sizeof(line),
					" %s %.3f",
					state.HistoryNames[spans[index].first].c_str(),
					spans[index].second
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
	size_t FrameGraph::Push(std::string_view name, ProfileCategory category) {
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
		if (std::this_thread::get_id() != state.Owner) {
			state.DroppedThisFrame++;
			return NOT_RECORDING;
		}

		// Deeper than the budget, or out of buffer. The depth still has to move
		// so the matching close moves it back — otherwise every sibling after a
		// dropped span is recorded one level too deep.
		if (state.Depth >= MAXIMUM_DEPTH || state.Building.size() >= MAXIMUM_SPANS) {
			state.DroppedThisFrame++;
			state.Depth++;
			return DEPTH_ONLY;
		}

		// Only recorded spans are on the stack, and a span is skipped only when
		// its parent was, so the top of the stack is this one's parent.
		const uint32_t parent = state.Open.empty() ? NO_PARENT : static_cast<uint32_t>(state.Open.back());

		const size_t index = state.Building.size();
		// Named rather than positional. This was seven bare values in
		// declaration order, and adding `IdleMilliseconds` in the middle of the
		// struct silently handed a `ProfileCategory` to a float — caught by the
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
		// moved the depth, so its close has to move it back — otherwise every
		// sibling after it is recorded several levels too deep.
		if (state.Depth > 0) {
			state.Depth--;
		}

		if (index == DEPTH_ONLY) {
			return;
		}

		if (state.Open.empty() || state.Open.back() != index) {
			// Closed out of order, which is possible only by constructing a
			// scope on the heap — made hard on purpose by the deleted copy and
			// move.
			return;
		}

		auto &span = state.Building[index];
		span.Milliseconds = MillisecondsSince(state.FrameStartNanoseconds) - span.StartMilliseconds;
		state.Open.pop_back();
	}

	FrameGraph::Scope::Scope(std::string_view name, ProfileCategory category) {
		Index = Push(name, category);
	}

	FrameGraph::Scope::~Scope() {
		Pop(Index);
	}

	FrameGraph::CopiedScope::CopiedScope(
		std::string_view fallback, std::string_view name, ProfileCategory category
	) {
		auto &state = Get();
		if (!state.Recording || std::this_thread::get_id() != state.Owner) {
			Index = Push(name, category);
			return;
		}

		// Nothing to say yet — the caller looks a name up only when something is
		// listening, and that is decided one frame and acted on the next.
		if (name.empty()) {
			Index = Push(fallback, category);
			return;
		}

		// Checked before interning rather than after: a name is only worth
		// copying when the span that keeps it is going to exist.
		if (state.Depth >= MAXIMUM_DEPTH || state.Building.size() >= MAXIMUM_SPANS) {
			Index = Push(name, category);
			return;
		}

		Index = Push(Intern(state, name), category);
	}

	void FrameGraph::Report(std::string_view name, ProfileCategory category, float milliseconds) {
		auto &state = Get();
		if (!state.Recording) {
			return;
		}

		// The same owner rule `Push` applies, and for the same reason. What is
		// different is only where the number came from: a producer that cannot
		// record its own span hands the duration to whoever can.
		if (std::this_thread::get_id() != state.Owner) {
			state.DroppedThisFrame++;
			return;
		}

		// Ignored rather than clamped. A negative duration means the producer
		// measured wrongly — two clocks, or a start it never set — and folding
		// it to zero would put a plausible bar on the graph instead of a
		// missing one.
		if (!(milliseconds >= 0.0f)) {
			state.DroppedThisFrame++;
			return;
		}

		if (state.Depth >= MAXIMUM_DEPTH || state.Building.size() >= MAXIMUM_SPANS) {
			state.DroppedThisFrame++;
			return;
		}

		// Opened and closed here, so it is a leaf under whatever is currently
		// open. It never becomes anybody's parent: the work it describes
		// happened elsewhere, and nothing recorded on this thread was inside
		// it.
		const size_t index = Push(name, category);
		if (index == NOT_RECORDING || index == DEPTH_ONLY) {
			Pop(index);
			return;
		}

		Pop(index);

		// Overwritten after the close, which is the whole point: the elapsed
		// time this thread would have measured is the cost of the call, and the
		// number worth showing is the one the producer reported.
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
		std::string_view fallback, std::string_view name, ProfileCategory category, float milliseconds
	) {
		auto &state = Get();
		if (!state.Recording || std::this_thread::get_id() != state.Owner || name.empty()) {
			Report(fallback, category, milliseconds);
			return;
		}

		if (state.Depth >= MAXIMUM_DEPTH || state.Building.size() >= MAXIMUM_SPANS) {
			Report(fallback, category, milliseconds);
			return;
		}

		Report(Intern(state, name), category, milliseconds);
	}
}
