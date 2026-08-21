#include <engine/core/Clock.hpp>
#include <engine/core/HeapProfile.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <new>
#include <string>
#include <vector>

namespace engine::core {

	namespace {

#if defined(MONO_HEAP_PROFILE)
		constexpr bool TRACKING = true;
#else
		constexpr bool TRACKING = false;
#endif

		// --- the tag tree ----------------------------------------------------
		//
		// Fixed storage at namespace scope, so it is zero-initialised before any
		// dynamic initialiser in the program runs. That matters more here than
		// anywhere else in the engine: the first allocation in the process
		// happens inside somebody else's static constructor, and a profiler that
		// needed its own constructor to have run first would be reading a
		// half-built tree at the one moment it cannot report an error.
		//
		// **Index zero is the root, so zero doubles as "no such node".** A child
		// list terminated by zero and a tree whose root is zero cannot be
		// confused, because the root is never anybody's child and never anybody's
		// sibling. That is what lets every link be zero-initialised and still
		// mean something.

		struct TagNode {
			// The scope name, in this profiler's own arena. Empty on the root,
			// which has no name of its own.
			//
			// **Copied on the way in, once per node, rather than borrowed.**
			// `ENGINE_PROFILE_DYNAMIC_STABLE` is documented as needing storage
			// that survives the next `EndFrame`, which the scheduler's system
			// names do - they live as long as the world. A tag tree node is
			// never removed, so borrowing here would need them to live as long
			// as the *process*, and a world being destroyed would leave the
			// tree pointing into freed memory with nothing to say so. One
			// memcpy per distinct tag path buys that whole class of bug away.
			std::string_view Name;

			uint32_t Parent;
			uint32_t Depth;

			// Head of this node's child list, or zero. Published with release
			// once the child is fully built.
			std::atomic<uint32_t> FirstChild;

			// Next entry in the parent's child list, or zero.
			std::atomic<uint32_t> NextSibling;

			std::atomic<int64_t> LiveBytes;
			std::atomic<int64_t> LiveBlocks;
			std::atomic<uint64_t> TotalBytes;
			std::atomic<uint64_t> TotalBlocks;
			std::atomic<int64_t> PeakBytes;
		};

		// **`constinit` on every one of these, and it is load-bearing rather than
		// decorative.** `TagNode Tree[N];` written without the initialiser is
		// *default* initialisation: the zero-initialisation pass runs first, but
		// each `std::atomic` member's constructor then runs as **dynamic**
		// initialisation, in this translation unit's static-init slot. So every
		// allocation made by an earlier translation unit's static constructor was
		// counted into the tree and then zeroed back out when the tree was
		// constructed - while the process-wide counters below, which carried
		// initialisers and were therefore constant-initialised, kept theirs. The
		// symptom was a process holding 1689 live blocks and a tag tree that had
		// only ever seen 123 of them.
		//
		// A profiler that is reached from inside `operator new` cannot have a
		// constructor of its own, because there is no ordering that puts it first.
		// `constinit` does not merely fix the initialiser: it makes the compiler
		// refuse the file if anybody writes one that is not constant.
		constinit TagNode Tree[HeapProfile::MAXIMUM_NODES] = {};

		// Nodes created beyond the root. The root is implicit, so the number of
		// valid indices is this plus one - which is what makes the counter safe
		// to leave zero-initialised.
		constinit std::atomic<uint32_t> CreatedNodes{0};

		// Held only while a node is created, which happens once per distinct tag
		// path and never in a hot path. A spin flag rather than a mutex because
		// this file is reached from inside `operator new` and a mutex with a
		// non-trivial constructor is one more thing that has to have been built.
		constinit std::atomic_flag CreationLock{};

		// Storage for the names, written once each under `CreationLock`.
		//
		// A fixed arena rather than a string per node, because this is reached
		// from inside `operator new` and a node created there must not allocate.
		// 128 KiB against `MAXIMUM_NODES` is about thirty bytes a tag, which is
		// well above what a span name runs to; a tag that will not fit is
		// refused and counted, exactly as one past the node budget is.
		constexpr size_t NAME_ARENA_BYTES = 128 * 1024;

		// Longer than this is truncated. Two names sharing a prefix this long
		// would merge into one node, which has not happened and would be a
		// wrong label rather than wrong arithmetic.
		constexpr size_t MAXIMUM_NAME = 128;

		constinit char NameArena[NAME_ARENA_BYTES] = {};
		constinit size_t NameArenaUsed = 0;

		constinit std::atomic<uint64_t> DroppedScopes{0};
		constinit std::atomic<uint64_t> ForeignFrees{0};

		// Process-wide live bytes, kept as well as the per-node figures.
		//
		// **Not derived by summing the tree, because of the peak.** The sum is
		// cheap enough to take on a sample, but a peak reached and released
		// between two samples is invisible to any amount of summing after the
		// fact - and a subsystem that briefly held 300 MB is exactly the finding
		// a sampled profiler is otherwise structurally unable to make.
		constinit std::atomic<int64_t> ProcessLiveBytes{0};
		constinit std::atomic<int64_t> ProcessLiveBlocks{0};
		constinit std::atomic<int64_t> ProcessPeakBytes{0};

		// The node this thread is allocating into, and how many scopes are open.
		//
		// Constant-initialised, so reaching them needs no guard variable and no
		// destructor - both of which would be run at a moment when allocation is
		// still happening.
		constinit thread_local uint32_t ThreadNode = HeapProfile::ROOT;
		constinit thread_local uint32_t ThreadDepth = 0;

		// Returns the child of `parent` named `name`, or zero.
		uint32_t FindChild(uint32_t parent, std::string_view name) {
			for (uint32_t child = Tree[parent].FirstChild.load(std::memory_order_acquire); child != 0;
				 child = Tree[child].NextSibling.load(std::memory_order_acquire)) {
				// The size compare rejects almost every mismatch before a byte
				// is read, which matters because this runs on every scope of
				// every frame.
				if (Tree[child].Name == name) {
					return child;
				}
			}
			return 0;
		}

		// Creates, or finds under the lock, the child of `parent` named `name`.
		// Returns zero when the tree is full.
		uint32_t CreateChild(uint32_t parent, std::string_view name) {
			while (CreationLock.test_and_set(std::memory_order_acquire)) {
				// Contended only when two threads open a tag for the first time
				// in the same breath, which happens once per tag per run.
			}

			uint32_t found = FindChild(parent, name);
			if (found == 0) {
				const uint32_t index = CreatedNodes.load(std::memory_order_relaxed) + 1;
				const size_t length = std::min(name.size(), MAXIMUM_NAME);
				if (index < HeapProfile::MAXIMUM_NODES && NameArenaUsed + length <= NAME_ARENA_BYTES) {
					char *text = NameArena + NameArenaUsed;
					std::memcpy(text, name.data(), length);
					NameArenaUsed += length;

					TagNode &node = Tree[index];
					node.Name = std::string_view{text, length};
					node.Parent = parent;
					node.Depth = Tree[parent].Depth + 1;
					node.NextSibling.store(
						Tree[parent].FirstChild.load(std::memory_order_relaxed), std::memory_order_relaxed
					);

					// Both stores are release and both come after the fields, so
					// a reader that reaches this node through either link sees a
					// node that is finished.
					CreatedNodes.store(index, std::memory_order_release);
					Tree[parent].FirstChild.store(index, std::memory_order_release);
					found = index;
				}
			}

			CreationLock.clear(std::memory_order_release);
			return found;
		}

#if defined(MONO_HEAP_PROFILE)

		// Adds one allocation to a node and to the process. Only reached from the
		// allocator hooks, so it is compiled out with them rather than left as a
		// function nothing calls.
		void RecordAllocation(uint32_t node, size_t bytes) {
			const auto amount = static_cast<int64_t>(bytes);
			TagNode &entry = Tree[node];

			const int64_t live = entry.LiveBytes.fetch_add(amount, std::memory_order_relaxed) + amount;
			entry.LiveBlocks.fetch_add(1, std::memory_order_relaxed);
			entry.TotalBytes.fetch_add(bytes, std::memory_order_relaxed);
			entry.TotalBlocks.fetch_add(1, std::memory_order_relaxed);

			// A load and a compare in the ordinary case; the exchange only runs
			// while the figure is actually climbing.
			int64_t peak = entry.PeakBytes.load(std::memory_order_relaxed);
			while (peak < live && !entry.PeakBytes.compare_exchange_weak(
									  peak, live, std::memory_order_relaxed, std::memory_order_relaxed
								  )) {}

			const int64_t total = ProcessLiveBytes.fetch_add(amount, std::memory_order_relaxed) + amount;
			ProcessLiveBlocks.fetch_add(1, std::memory_order_relaxed);

			int64_t processPeak = ProcessPeakBytes.load(std::memory_order_relaxed);
			while (processPeak < total &&
				   !ProcessPeakBytes.compare_exchange_weak(
					   processPeak, total, std::memory_order_relaxed, std::memory_order_relaxed
				   )) {}
		}

		// Removes one allocation from the node it was charged to.
		void RecordFree(uint32_t node, size_t bytes) {
			const auto amount = static_cast<int64_t>(bytes);
			TagNode &entry = Tree[node];

			entry.LiveBytes.fetch_sub(amount, std::memory_order_relaxed);
			entry.LiveBlocks.fetch_sub(1, std::memory_order_relaxed);

			ProcessLiveBytes.fetch_sub(amount, std::memory_order_relaxed);
			ProcessLiveBlocks.fetch_sub(1, std::memory_order_relaxed);
		}

#endif

		// --- sampled history --------------------------------------------------

		// Per-node series are kept for the first `MAXIMUM_TRACKED_NODES` nodes.
		// Allocated with malloc when sampling is turned on, so a program that
		// never opens the panel carries none of it.
		struct SampleHistory {
			HeapSample *Readings = nullptr;

			// `MAXIMUM_SAMPLES` rows of `MAXIMUM_TRACKED_NODES` inclusive byte
			// figures, in the same ring order as `Readings`.
			int64_t *NodeBytes = nullptr;

			// Scratch for one sample's inclusive totals, so a sample allocates
			// nothing - which keeps the profiler out of its own measurement.
			int64_t *Inclusive = nullptr;

			size_t Head = 0;
			size_t Count = 0;
		};

		constinit SampleHistory Recorded;
		constinit std::mutex HistoryLock;
		constinit std::atomic<bool> Sampling{false};

		// When the next `SampleIfDue` reading is due, on `Clock`'s monotonic
		// scale. Zero means the next call takes one, which is what makes the
		// first reading of a run land at the start of it rather than an interval
		// in.
		constinit std::atomic<uint64_t> NextSampleNanoseconds{0};

		// Frees the history buffers. The lock must be held.
		void ReleaseHistory() {
			std::free(Recorded.Readings);
			std::free(Recorded.NodeBytes);
			std::free(Recorded.Inclusive);
			Recorded = SampleHistory{};
		}

		// Allocates the history buffers if they are not there. The lock must be
		// held. Returns false when the allocation failed, which leaves sampling
		// on and recording nothing rather than taking the process down.
		bool EnsureHistory() {
			if (Recorded.Readings != nullptr) {
				return true;
			}

			Recorded.Readings =
				static_cast<HeapSample *>(std::malloc(sizeof(HeapSample) * HeapProfile::MAXIMUM_SAMPLES));
			Recorded.NodeBytes = static_cast<int64_t *>(std::malloc(
				sizeof(int64_t) * HeapProfile::MAXIMUM_SAMPLES * HeapProfile::MAXIMUM_TRACKED_NODES
			));
			Recorded.Inclusive =
				static_cast<int64_t *>(std::malloc(sizeof(int64_t) * HeapProfile::MAXIMUM_NODES));

			if (Recorded.Readings == nullptr || Recorded.NodeBytes == nullptr ||
				Recorded.Inclusive == nullptr) {
				ReleaseHistory();
				return false;
			}

			Recorded.Head = 0;
			Recorded.Count = 0;
			return true;
		}

		// Ring position of the `offset`th oldest retained reading.
		size_t RingIndex(size_t offset) {
			const size_t start = (Recorded.Head + HeapProfile::MAXIMUM_SAMPLES - Recorded.Count) %
								 HeapProfile::MAXIMUM_SAMPLES;
			return (start + offset) % HeapProfile::MAXIMUM_SAMPLES;
		}

		// Fills `Recorded.Inclusive` with each node's live bytes plus its
		// subtree's.
		//
		// **One reverse pass, because a child's index is always above its
		// parent's.** `CreateChild` takes the next free index under a parent that
		// already exists, so walking down from the highest index adds every node
		// into its parent before that parent is itself read - which turns what
		// would be a recursive walk into a loop that allocates nothing.
		void AccumulateInclusive(uint32_t nodes) {
			for (uint32_t index = 0; index < nodes; index++) {
				Recorded.Inclusive[index] = Tree[index].LiveBytes.load(std::memory_order_relaxed);
			}
			for (uint32_t index = nodes; index-- > 1;) {
				Recorded.Inclusive[Tree[index].Parent] += Recorded.Inclusive[index];
			}
		}

		// --- formatting -------------------------------------------------------

		// Renders a byte count the way somebody reads one, to three significant
		// figures.
		std::string FormatBytes(int64_t bytes) {
			const char *const units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
			auto amount = static_cast<double>(bytes);
			size_t unit = 0;
			while ((amount >= 1024.0 || amount <= -1024.0) && unit + 1 < std::size(units)) {
				amount /= 1024.0;
				unit++;
			}

			char text[64];
			std::snprintf(text, sizeof(text), "%.2f %s", amount, units[unit]);
			return text;
		}
	}

	// --- allocation hooks ------------------------------------------------------
	//
	// Everything below is only compiled when MONO_HEAP_PROFILE is on. The public
	// functions above stay, and answer with zeroes, so a panel and a suite build
	// against `release` unchanged.

#if defined(MONO_HEAP_PROFILE)

	namespace {

		// Written into the bytes immediately before every pointer handed out.
		//
		// 24 bytes, and the layout is chosen so that the whole thing is eight
		// byte aligned wherever the user pointer lands: `sizeof` is a multiple of
		// `alignof`, and the user pointer is rounded up to at least `alignof`, so
		// subtracting one header from it can never be misaligned.
		struct BlockHeader {
			// Distinguishes a block this allocator produced from anything else a
			// caller might pass to `operator delete`.
			uint32_t Magic;

			// Tag tree node the allocation was charged to.
			uint32_t Node;

			// Bytes from the underlying allocation's base to the user pointer.
			// What `free` has to be given back.
			uint32_t Offset;

			// Unnamed padding written out, so the struct has no implicit hole
			// for a sanitiser to complain about being read.
			uint32_t Reserved;

			// What the caller asked for, which is what the counters carry.
			uint64_t Size;
		};

		constexpr uint32_t BLOCK_MAGIC = 0x48454150u; // 'HEAP'

		void *TrackedAllocate(size_t size, size_t alignment) {
			const size_t align = alignment < alignof(BlockHeader) ? alignof(BlockHeader) : alignment;

			// The header, plus the worst case of rounding the user pointer up.
			const size_t extra = sizeof(BlockHeader) + align - 1;
			if (size > SIZE_MAX - extra) {
				return nullptr;
			}

			void *base = std::malloc(size + extra);
			if (base == nullptr) {
				return nullptr;
			}

			const uintptr_t raw = reinterpret_cast<uintptr_t>(base) + sizeof(BlockHeader);
			const uintptr_t user = (raw + align - 1) & ~static_cast<uintptr_t>(align - 1);

			auto *header = reinterpret_cast<BlockHeader *>(user) - 1;
			header->Magic = BLOCK_MAGIC;
			header->Node = ThreadNode;
			header->Offset = static_cast<uint32_t>(user - reinterpret_cast<uintptr_t>(base));
			header->Reserved = 0;
			header->Size = size;

			RecordAllocation(header->Node, size);
			return reinterpret_cast<void *>(user);
		}

		void TrackedFree(void *pointer) {
			if (pointer == nullptr) {
				return;
			}

			auto *header = reinterpret_cast<BlockHeader *>(pointer) - 1;
			if (header->Magic != BLOCK_MAGIC) {
				// Not ours. Within one program this cannot happen - the
				// replacement is process-wide from the first allocation - so it
				// means a second allocator got there first. Free it the way it
				// was almost certainly made and say so in the totals.
				ForeignFrees.fetch_add(1, std::memory_order_relaxed);
				std::free(pointer);
				return;
			}

			RecordFree(header->Node, static_cast<size_t>(header->Size));

			// Cleared before the free, so a double free is counted as foreign
			// rather than subtracted from the counters twice.
			header->Magic = 0;
			std::free(static_cast<char *>(pointer) - header->Offset);
		}

		// Throwing new: the standard's retry loop, so a program that installed a
		// `new_handler` still gets it.
		void *TrackedAllocateOrThrow(size_t size, size_t alignment) {
			for (;;) {
				if (void *block = TrackedAllocate(size, alignment); block != nullptr) {
					return block;
				}

				std::new_handler handler = std::get_new_handler();
				if (handler == nullptr) {
					throw std::bad_alloc();
				}
				handler();
			}
		}
	}

#endif

	// --- the public surface ----------------------------------------------------

	bool HeapProfile::IsCompiledIn() {
		return TRACKING;
	}

	size_t HeapProfile::BlockOverhead() {
#if defined(MONO_HEAP_PROFILE)
		// The header rounded up to the alignment an ordinary `new` asks for.
		// An over-aligned block pays more than this; nothing pays less.
		constexpr size_t alignment = alignof(std::max_align_t);
		return (sizeof(BlockHeader) + alignment - 1) & ~(alignment - 1);
#else
		return 0;
#endif
	}

	uint32_t HeapProfile::Push(std::string_view name) {
		const uint32_t previous = ThreadNode;
		if constexpr (!TRACKING) {
			return previous;
		}

		ThreadDepth++;
		if (name.empty() || ThreadDepth > MAXIMUM_DEPTH) {
			DroppedScopes.fetch_add(1, std::memory_order_relaxed);
			return previous;
		}

		uint32_t child = FindChild(previous, name);
		if (child == 0) {
			child = CreateChild(previous, name);
		}
		if (child == 0) {
			DroppedScopes.fetch_add(1, std::memory_order_relaxed);
			return previous;
		}

		ThreadNode = child;
		return previous;
	}

	void HeapProfile::Pop(uint32_t previous) {
		if constexpr (!TRACKING) {
			return;
		}

		// Decremented even for a scope that was refused, so a dropped tag does
		// not leave every scope after it one level deep in the budget.
		if (ThreadDepth > 0) {
			ThreadDepth--;
		}
		ThreadNode = previous;
	}

	uint32_t HeapProfile::Current() {
		return ThreadNode;
	}

	uint32_t HeapProfile::NodeCount() {
		if constexpr (!TRACKING) {
			return 0;
		}
		return CreatedNodes.load(std::memory_order_acquire) + 1;
	}

	HeapTotals HeapProfile::Totals() {
		HeapTotals totals;
		if constexpr (!TRACKING) {
			return totals;
		}

		const uint32_t nodes = NodeCount();
		for (uint32_t index = 0; index < nodes; index++) {
			totals.TotalBytes += Tree[index].TotalBytes.load(std::memory_order_relaxed);
			totals.TotalBlocks += Tree[index].TotalBlocks.load(std::memory_order_relaxed);
		}

		totals.LiveBytes = ProcessLiveBytes.load(std::memory_order_relaxed);
		totals.LiveBlocks = ProcessLiveBlocks.load(std::memory_order_relaxed);
		totals.PeakBytes = ProcessPeakBytes.load(std::memory_order_relaxed);
		totals.OverheadBytes = totals.LiveBlocks * static_cast<int64_t>(BlockOverhead());
		totals.ForeignFrees = ForeignFrees.load(std::memory_order_relaxed);
		totals.Nodes = nodes;
		totals.DroppedScopes = DroppedScopes.load(std::memory_order_relaxed);
		return totals;
	}

	HeapNodeView HeapProfile::Node(uint32_t index) {
		HeapNodeView view;
		if (index >= NodeCount()) {
			return view;
		}

		const TagNode &node = Tree[index];
		view.Name = node.Name;
		view.Parent = node.Parent;
		view.Depth = node.Depth;
		view.LiveBytes = node.LiveBytes.load(std::memory_order_relaxed);
		view.LiveBlocks = node.LiveBlocks.load(std::memory_order_relaxed);
		view.TotalBytes = node.TotalBytes.load(std::memory_order_relaxed);
		view.TotalBlocks = node.TotalBlocks.load(std::memory_order_relaxed);
		view.PeakBytes = node.PeakBytes.load(std::memory_order_relaxed);
		return view;
	}

	int64_t HeapProfile::InclusiveBytes(uint32_t index) {
		const uint32_t nodes = NodeCount();
		if (index >= nodes) {
			return 0;
		}

		// The same reverse pass `AccumulateInclusive` uses, over a local so that
		// a reader needs no sampling to have been turned on.
		std::vector<int64_t> totals(nodes);
		for (uint32_t node = 0; node < nodes; node++) {
			totals[node] = Tree[node].LiveBytes.load(std::memory_order_relaxed);
		}
		for (uint32_t node = nodes; node-- > 1;) {
			totals[Tree[node].Parent] += totals[node];
		}
		return totals[index];
	}

	std::vector<HeapTreeRow> HeapProfile::TreeRows(int64_t minimumBytes) {
		std::vector<HeapTreeRow> rows;
		const uint32_t nodes = NodeCount();
		if (nodes <= 1) {
			return rows;
		}

		std::vector<int64_t> inclusive(nodes);
		for (uint32_t index = 0; index < nodes; index++) {
			inclusive[index] = Tree[index].LiveBytes.load(std::memory_order_relaxed);
		}
		for (uint32_t index = nodes; index-- > 1;) {
			inclusive[Tree[index].Parent] += inclusive[index];
		}

		// Children by parent, built in one pass. The sibling lists in the tree
		// itself are in reverse creation order, which is an order nobody wants
		// to read.
		std::vector<std::vector<uint32_t>> children(nodes);
		for (uint32_t index = 1; index < nodes; index++) {
			children[Tree[index].Parent].push_back(index);
		}
		for (auto &list : children) {
			std::sort(list.begin(), list.end(), [&](uint32_t left, uint32_t right) {
				return inclusive[left] > inclusive[right];
			});
		}

		// An explicit stack rather than recursion: the tree's depth is bounded
		// by `MAXIMUM_DEPTH` and this is called from a panel, but a corrupted
		// parent link would otherwise be a stack overflow rather than a wrong
		// picture.
		std::vector<uint32_t> pending(children[ROOT].rbegin(), children[ROOT].rend());
		while (!pending.empty()) {
			const uint32_t index = pending.back();
			pending.pop_back();

			if (inclusive[index] < minimumBytes) {
				continue;
			}

			rows.push_back(
				HeapTreeRow{
					index,
					Tree[index].Name,
					Tree[index].Depth,
					inclusive[index],
					Tree[index].LiveBytes.load(std::memory_order_relaxed),
					Tree[index].LiveBlocks.load(std::memory_order_relaxed),
				}
			);

			for (size_t child = children[index].size(); child-- > 0;) {
				pending.push_back(children[index][child]);
			}
		}
		return rows;
	}

	std::string HeapProfile::Path(uint32_t index) {
		if (index >= NodeCount() || index == ROOT) {
			return {};
		}

		// Built from the leaf up and reversed once, rather than by inserting at
		// the front of a string per level.
		std::vector<std::string_view> parts;
		for (uint32_t node = index; node != ROOT; node = Tree[node].Parent) {
			parts.push_back(Tree[node].Name);
		}

		std::string path;
		for (size_t part = parts.size(); part-- > 0;) {
			if (!path.empty()) {
				path += ';';
			}
			path += parts[part];
		}
		return path;
	}

	void HeapProfile::ResetTotals() {
		const uint32_t nodes = NodeCount();
		for (uint32_t index = 0; index < nodes; index++) {
			Tree[index].TotalBytes.store(0, std::memory_order_relaxed);
			Tree[index].TotalBlocks.store(0, std::memory_order_relaxed);
			Tree[index].PeakBytes.store(
				Tree[index].LiveBytes.load(std::memory_order_relaxed), std::memory_order_relaxed
			);
		}
		ProcessPeakBytes.store(ProcessLiveBytes.load(std::memory_order_relaxed), std::memory_order_relaxed);
	}

	void HeapProfile::SetSamplingEnabled(bool enabled) {
		if constexpr (!TRACKING) {
			return;
		}

		const std::lock_guard<std::mutex> guard(HistoryLock);
		if (enabled == Sampling.load(std::memory_order_relaxed)) {
			return;
		}

		Sampling.store(enabled, std::memory_order_relaxed);
		if (enabled) {
			// A capture covers one run: keeping the last session's readings
			// would put a gap of arbitrary length in the middle of a window a
			// slope is fitted to, which is exactly the shape of a false leak.
			if (EnsureHistory()) {
				Recorded.Head = 0;
				Recorded.Count = 0;
			}
			NextSampleNanoseconds.store(0, std::memory_order_relaxed);
		} else {
			// The buffers stay, so the report can still be asked for.
		}
	}

	bool HeapProfile::IsSampling() {
		return Sampling.load(std::memory_order_relaxed);
	}

	void HeapProfile::Sample() {
		if constexpr (!TRACKING) {
			return;
		}
		if (!Sampling.load(std::memory_order_relaxed)) {
			return;
		}

		const std::lock_guard<std::mutex> guard(HistoryLock);
		if (!EnsureHistory()) {
			return;
		}

		const uint32_t nodes = NodeCount();
		AccumulateInclusive(nodes);

		const size_t slot = Recorded.Head;
		Recorded.Readings[slot] = HeapSample{
			Clock::Seconds(),
			ProcessLiveBytes.load(std::memory_order_relaxed),
			ProcessLiveBlocks.load(std::memory_order_relaxed),
		};

		int64_t *row = Recorded.NodeBytes + slot * MAXIMUM_TRACKED_NODES;
		const uint32_t tracked = std::min<uint32_t>(nodes, MAXIMUM_TRACKED_NODES);
		for (uint32_t index = 0; index < tracked; index++) {
			row[index] = Recorded.Inclusive[index];
		}
		for (uint32_t index = tracked; index < MAXIMUM_TRACKED_NODES; index++) {
			row[index] = 0;
		}

		Recorded.Head = (Recorded.Head + 1) % MAXIMUM_SAMPLES;
		Recorded.Count = std::min(Recorded.Count + 1, MAXIMUM_SAMPLES);
	}

	bool HeapProfile::SampleIfDue(double intervalSeconds) {
		if constexpr (!TRACKING) {
			return false;
		}
		if (!Sampling.load(std::memory_order_relaxed)) {
			return false;
		}

		// One clock read and one relaxed load on the frames that are not due,
		// which is all but one in several hundred.
		const uint64_t now = Clock::Nanoseconds();
		if (now < NextSampleNanoseconds.load(std::memory_order_relaxed)) {
			return false;
		}

		const auto interval =
			static_cast<uint64_t>(intervalSeconds > 0.0 ? intervalSeconds * 1'000'000'000.0 : 0.0);
		NextSampleNanoseconds.store(now + interval, std::memory_order_relaxed);

		Sample();
		return true;
	}

	std::vector<HeapSample> HeapProfile::History() {
		const std::lock_guard<std::mutex> guard(HistoryLock);

		std::vector<HeapSample> readings;
		if (Recorded.Readings == nullptr) {
			return readings;
		}

		readings.reserve(Recorded.Count);
		for (size_t offset = 0; offset < Recorded.Count; offset++) {
			readings.push_back(Recorded.Readings[RingIndex(offset)]);
		}
		return readings;
	}

	double HeapProfile::HistorySeconds() {
		const std::lock_guard<std::mutex> guard(HistoryLock);
		if (Recorded.Readings == nullptr || Recorded.Count < 2) {
			return 0.0;
		}
		return Recorded.Readings[RingIndex(Recorded.Count - 1)].Seconds -
			   Recorded.Readings[RingIndex(0)].Seconds;
	}

	void HeapProfile::ResetHistory() {
		const std::lock_guard<std::mutex> guard(HistoryLock);
		Recorded.Head = 0;
		Recorded.Count = 0;
	}

	std::vector<HeapGrowth> HeapProfile::Growth(double windowSeconds, int64_t minimumBytes) {
		std::vector<HeapGrowth> report;

		// The node figures are copied out under the lock and the fitting is done
		// without it: a least-squares pass over 256 series is not something to
		// hold a lock the sampler needs across.
		std::vector<double> seconds;
		std::vector<int64_t> bytes;
		uint32_t tracked = 0;
		size_t readings = 0;

		{
			const std::lock_guard<std::mutex> guard(HistoryLock);
			if (Recorded.Readings == nullptr || Recorded.Count < 2) {
				return report;
			}

			const double newest = Recorded.Readings[RingIndex(Recorded.Count - 1)].Seconds;
			size_t first = 0;
			if (windowSeconds > 0.0) {
				while (first + 2 < Recorded.Count &&
					   newest - Recorded.Readings[RingIndex(first)].Seconds > windowSeconds) {
					first++;
				}
			}

			readings = Recorded.Count - first;
			tracked = std::min<uint32_t>(NodeCount(), MAXIMUM_TRACKED_NODES);

			seconds.reserve(readings);
			bytes.resize(readings * tracked);
			for (size_t offset = 0; offset < readings; offset++) {
				const size_t slot = RingIndex(first + offset);
				seconds.push_back(Recorded.Readings[slot].Seconds);

				const int64_t *row = Recorded.NodeBytes + slot * MAXIMUM_TRACKED_NODES;
				for (uint32_t index = 0; index < tracked; index++) {
					bytes[offset * tracked + index] = row[index];
				}
			}
		}

		if (readings < 2) {
			return report;
		}

		// The time terms are shared by every series, so they are computed once.
		const double base = seconds.front();
		double sumTime = 0.0;
		double sumTimeSquared = 0.0;
		for (const double reading : seconds) {
			const double time = reading - base;
			sumTime += time;
			sumTimeSquared += time * time;
		}
		const auto count = static_cast<double>(readings);
		const double timeVariance = sumTimeSquared - sumTime * sumTime / count;
		if (timeVariance <= 0.0) {
			return report;
		}

		for (uint32_t index = 1; index < tracked; index++) {
			double sumBytes = 0.0;
			double sumBytesSquared = 0.0;
			double sumProduct = 0.0;
			int64_t peak = 0;

			for (size_t offset = 0; offset < readings; offset++) {
				const int64_t value = bytes[offset * tracked + index];
				const auto amount = static_cast<double>(value);
				const double time = seconds[offset] - base;

				sumBytes += amount;
				sumBytesSquared += amount * amount;
				sumProduct += amount * time;
				peak = std::max(peak, value);
			}

			if (peak < minimumBytes) {
				continue;
			}

			const double covariance = sumProduct - sumTime * sumBytes / count;
			const double byteVariance = sumBytesSquared - sumBytes * sumBytes / count;

			HeapGrowth growth;
			growth.Node = index;
			growth.Path = Path(index);
			growth.BytesPerSecond = covariance / timeVariance;
			growth.Fit = byteVariance > 0.0 ? covariance * covariance / (timeVariance * byteVariance) : 0.0;
			growth.FirstBytes = bytes[index];
			growth.LastBytes = bytes[(readings - 1) * tracked + index];
			growth.PeakBytes = peak;
			report.push_back(std::move(growth));
		}

		std::sort(report.begin(), report.end(), [](const HeapGrowth &left, const HeapGrowth &right) {
			return left.BytesPerSecond > right.BytesPerSecond;
		});
		return report;
	}

	std::vector<HeapGrowth>
	HeapProfile::Runaway(double bytesPerSecond, double windowSeconds, int64_t minimumBytes) {
		std::vector<HeapGrowth> report = Growth(windowSeconds, minimumBytes);
		std::erase_if(report, [&](const HeapGrowth &entry) {
			return entry.BytesPerSecond < bytesPerSecond || entry.Fit < RUNAWAY_FIT ||
				   entry.LastBytes <= entry.FirstBytes;
		});
		return report;
	}

	void HeapProfile::FoldLive(FoldedStacks &totals) {
		const uint32_t nodes = NodeCount();
		for (uint32_t index = 1; index < nodes; index++) {
			const int64_t live = Tree[index].LiveBytes.load(std::memory_order_relaxed);
			if (live <= 0) {
				continue;
			}
			totals[Path(index)] += static_cast<double>(live);
		}

		// The root carries everything allocated with no tag open at all, which
		// on a partly instrumented program is most of it. Named rather than
		// dropped, for the reason `FrameGraph::UnmarkedMilliseconds` exists: a
		// graph whose rows do not add up to the process leaves a reader working
		// out which number is wrong, and neither is.
		const int64_t untagged = Tree[ROOT].LiveBytes.load(std::memory_order_relaxed);
		if (untagged > 0) {
			totals["untagged"] += static_cast<double>(untagged);
		}
	}

	bool HeapProfile::WriteFolded(const std::filesystem::path &path) {
		if constexpr (!TRACKING) {
			return false;
		}

		FoldedStacks totals;
		FoldLive(totals);
		if (totals.empty()) {
			return false;
		}
		return WriteFoldedStacks(path, totals);
	}

	bool HeapProfile::WriteReport(const std::filesystem::path &path, double windowSeconds) {
		std::ofstream file(path, std::ios::trunc);
		if (!file) {
			return false;
		}

		const HeapTotals totals = Totals();
		file << "heap profile\n";
		if (!TRACKING) {
			file << "  not compiled in - configure with MONO_HEAP_PROFILE=ON\n";
			return file.good();
		}

		file << "  live      " << FormatBytes(totals.LiveBytes) << " in " << totals.LiveBlocks << " blocks\n";
		file << "  peak      " << FormatBytes(totals.PeakBytes) << "\n";
		file << "  allocated " << FormatBytes(static_cast<int64_t>(totals.TotalBytes)) << " in "
			 << totals.TotalBlocks << " blocks\n";
		file << "  overhead  " << FormatBytes(totals.OverheadBytes) << " of profiler headers\n";
		file << "  tags      " << totals.Nodes << " nodes, " << totals.DroppedScopes << " scopes dropped, "
			 << totals.ForeignFrees << " foreign frees\n\n";

		// The heaviest tags, inclusive, so a parent is readable without adding
		// its children up by eye.
		const uint32_t nodes = NodeCount();
		std::vector<std::pair<int64_t, uint32_t>> heaviest;
		std::vector<int64_t> inclusive(nodes);
		for (uint32_t index = 0; index < nodes; index++) {
			inclusive[index] = Tree[index].LiveBytes.load(std::memory_order_relaxed);
		}
		for (uint32_t index = nodes; index-- > 1;) {
			inclusive[Tree[index].Parent] += inclusive[index];
		}
		for (uint32_t index = 1; index < nodes; index++) {
			heaviest.emplace_back(inclusive[index], index);
		}
		std::sort(heaviest.begin(), heaviest.end(), std::greater<>());

		file << "live bytes by tag\n";
		for (size_t row = 0; row < heaviest.size() && row < 40; row++) {
			const uint32_t index = heaviest[row].second;
			file << "  " << FormatBytes(heaviest[row].first) << "  ("
				 << FormatBytes(Tree[index].LiveBytes.load(std::memory_order_relaxed)) << " self, "
				 << Tree[index].LiveBlocks.load(std::memory_order_relaxed) << " blocks)  " << Path(index)
				 << "\n";
		}
		file << "  " << FormatBytes(Tree[ROOT].LiveBytes.load(std::memory_order_relaxed)) << "  untagged\n\n";

		const std::vector<HeapGrowth> growth = Growth(windowSeconds, 0);
		file << "growth over " << HistorySeconds() << " s of samples\n";
		if (growth.empty()) {
			file << "  no samples retained - sampling was never turned on\n";
			return file.good();
		}

		for (size_t row = 0; row < growth.size() && row < 40; row++) {
			const HeapGrowth &entry = growth[row];
			char rate[64];
			std::snprintf(rate, sizeof(rate), "%+10.1f B/s", entry.BytesPerSecond);
			file << "  " << rate << "  fit " << entry.Fit << "  " << FormatBytes(entry.FirstBytes) << " -> "
				 << FormatBytes(entry.LastBytes) << "  " << entry.Path << "\n";
		}
		return file.good();
	}
}

#if defined(MONO_HEAP_PROFILE)

// The global replacements.
//
// **They live in this translation unit deliberately.** A static archive member
// that nothing references is dropped by the linker, and a replacement
// `operator new` that is silently absent is the worst possible failure - the
// program runs, the panel shows an empty tree, and nothing says why. Putting
// them beside the functions every consumer of this profiler already calls means
// the object file is pulled in for a reason the linker can see.
//
// A program that references nothing in this file gets the standard library's
// allocator throughout, which is consistent and safe: every block in that
// program is made and freed by the same pair.

void *operator new(size_t size) {
	return engine::core::TrackedAllocateOrThrow(size, alignof(std::max_align_t));
}

void *operator new[](size_t size) {
	return engine::core::TrackedAllocateOrThrow(size, alignof(std::max_align_t));
}

void *operator new(size_t size, std::align_val_t alignment) {
	return engine::core::TrackedAllocateOrThrow(size, static_cast<size_t>(alignment));
}

void *operator new[](size_t size, std::align_val_t alignment) {
	return engine::core::TrackedAllocateOrThrow(size, static_cast<size_t>(alignment));
}

void *operator new(size_t size, const std::nothrow_t &) noexcept {
	return engine::core::TrackedAllocate(size, alignof(std::max_align_t));
}

void *operator new[](size_t size, const std::nothrow_t &) noexcept {
	return engine::core::TrackedAllocate(size, alignof(std::max_align_t));
}

void *operator new(size_t size, std::align_val_t alignment, const std::nothrow_t &) noexcept {
	return engine::core::TrackedAllocate(size, static_cast<size_t>(alignment));
}

void *operator new[](size_t size, std::align_val_t alignment, const std::nothrow_t &) noexcept {
	return engine::core::TrackedAllocate(size, static_cast<size_t>(alignment));
}

// Every delete form ends in the same call: the size and alignment the compiler
// passes are what the caller asked for, and the header already knows both.
void operator delete(void *pointer) noexcept {
	engine::core::TrackedFree(pointer);
}

void operator delete[](void *pointer) noexcept {
	engine::core::TrackedFree(pointer);
}

void operator delete(void *pointer, size_t) noexcept {
	engine::core::TrackedFree(pointer);
}

void operator delete[](void *pointer, size_t) noexcept {
	engine::core::TrackedFree(pointer);
}

void operator delete(void *pointer, std::align_val_t) noexcept {
	engine::core::TrackedFree(pointer);
}

void operator delete[](void *pointer, std::align_val_t) noexcept {
	engine::core::TrackedFree(pointer);
}

void operator delete(void *pointer, size_t, std::align_val_t) noexcept {
	engine::core::TrackedFree(pointer);
}

void operator delete[](void *pointer, size_t, std::align_val_t) noexcept {
	engine::core::TrackedFree(pointer);
}

void operator delete(void *pointer, const std::nothrow_t &) noexcept {
	engine::core::TrackedFree(pointer);
}

void operator delete[](void *pointer, const std::nothrow_t &) noexcept {
	engine::core::TrackedFree(pointer);
}

void operator delete(void *pointer, std::align_val_t, const std::nothrow_t &) noexcept {
	engine::core::TrackedFree(pointer);
}

void operator delete[](void *pointer, std::align_val_t, const std::nothrow_t &) noexcept {
	engine::core::TrackedFree(pointer);
}

#endif
