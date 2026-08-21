// The heap profiler, checked against allocations this file makes on purpose.
//
// **Every case is written to survive being run beside other suites in one
// process.** The tag tree is process-wide and never emptied, so a case that
// asserted on a total, or on a node index, would pass alone and fail in the
// binary. Each case opens tags nobody else uses and asserts on the *difference*
// its own allocations made.
//
// The one thing this cannot check is the case the header worries about most: a
// block allocated before the hooks were linked in. That is a link-order
// property of a whole program, and there is no way to produce one from inside a
// program that already has them.

#include <engine/core/HeapProfile.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

TEST_SUITE_ID("engine.core.heapprofile")

using engine::core::FoldedStacks;
using engine::core::HeapGrowth;
using engine::core::HeapNodeView;
using engine::core::HeapProfile;
using engine::core::HeapSample;
using engine::core::HeapTotals;

namespace {

	// Big enough that no small-string or small-vector optimisation can answer
	// it, so an allocation definitely happens.
	constexpr size_t BLOCK = 64 * 1024;

	// Finds a node by its full tag path, or returns zero.
	uint32_t FindPath(const std::string &path) {
		const uint32_t nodes = HeapProfile::NodeCount();
		for (uint32_t index = 1; index < nodes; index++) {
			if (HeapProfile::Path(index) == path) {
				return index;
			}
		}
		return 0;
	}

	// Live bytes charged to a tag path right now, or zero when it has never
	// been opened.
	int64_t LiveAt(const std::string &path) {
		const uint32_t node = FindPath(path);
		return node == 0 ? 0 : HeapProfile::Node(node).LiveBytes;
	}
}

TEST_CASE("an allocation is charged to the tag that was open", "[heap]") {
	if (!HeapProfile::IsCompiledIn()) {
		SUCCEED("allocator hooks are not compiled in");
		return;
	}

	const int64_t before = LiveAt("heaptest-charge");

	std::unique_ptr<char[]> block;
	{
		ENGINE_HEAP_SCOPE("heaptest-charge");
		block = std::make_unique<char[]>(BLOCK);
	}

	// At least the block: the scope also charged whatever `make_unique` needed
	// for itself, and pinning that down would be pinning down the standard
	// library rather than this profiler.
	REQUIRE(LiveAt("heaptest-charge") - before >= static_cast<int64_t>(BLOCK));

	block.reset();
	REQUIRE(LiveAt("heaptest-charge") == before);
}

TEST_CASE("a block is credited back to where it was allocated", "[heap]") {
	if (!HeapProfile::IsCompiledIn()) {
		SUCCEED("allocator hooks are not compiled in");
		return;
	}

	// **The property that makes the counters usable at all.** A pool allocated
	// by the loader and released by the scene it was handed to must not leave
	// the loader permanently in credit and the scene permanently in debt, which
	// is what attributing a free to the *freeing* scope would do.
	const int64_t maker = LiveAt("heaptest-maker");
	const int64_t taker = LiveAt("heaptest-taker");

	std::unique_ptr<char[]> block;
	{
		ENGINE_HEAP_SCOPE("heaptest-maker");
		block = std::make_unique<char[]>(BLOCK);
	}
	{
		ENGINE_HEAP_SCOPE("heaptest-taker");
		block.reset();
	}

	REQUIRE(LiveAt("heaptest-maker") == maker);
	REQUIRE(LiveAt("heaptest-taker") == taker);
}

TEST_CASE("nested tags make a path and a subtree", "[heap]") {
	if (!HeapProfile::IsCompiledIn()) {
		SUCCEED("allocator hooks are not compiled in");
		return;
	}

	std::unique_ptr<char[]> outer;
	std::unique_ptr<char[]> inner;
	{
		ENGINE_HEAP_SCOPE("heaptest-tree");
		outer = std::make_unique<char[]>(BLOCK);
		{
			ENGINE_HEAP_SCOPE("leaf");
			inner = std::make_unique<char[]>(BLOCK * 2);
		}
	}

	const uint32_t parent = FindPath("heaptest-tree");
	const uint32_t child = FindPath("heaptest-tree;leaf");
	REQUIRE(parent != 0);
	REQUIRE(child != 0);
	REQUIRE(HeapProfile::Node(child).Parent == parent);
	REQUIRE(HeapProfile::Node(child).Depth == HeapProfile::Node(parent).Depth + 1);

	// Exclusive on the parent, inclusive across the pair. Both are asserted
	// because the difference between them is the whole reason two functions
	// exist.
	const int64_t exclusive = HeapProfile::Node(parent).LiveBytes;
	const int64_t inclusive = HeapProfile::InclusiveBytes(parent);
	REQUIRE(inclusive >= exclusive + static_cast<int64_t>(BLOCK * 2));

	outer.reset();
	inner.reset();
}

TEST_CASE("a scope past the depth budget still balances", "[heap]") {
	if (!HeapProfile::IsCompiledIn()) {
		SUCCEED("allocator hooks are not compiled in");
		return;
	}

	// **The failure this guards against is silent and permanent.** A push that
	// is refused and a pop that restores anyway would leave the thread pointing
	// at a node from a scope that has closed, and every allocation the thread
	// makes afterwards - for the rest of the run - lands on the wrong tag.
	const uint32_t before = HeapProfile::Current();

	std::vector<std::unique_ptr<HeapProfile::Scope>> scopes;
	for (uint32_t level = 0; level < HeapProfile::MAXIMUM_DEPTH + 8; level++) {
		scopes.push_back(std::make_unique<HeapProfile::Scope>("heaptest-deep"));
	}
	while (!scopes.empty()) {
		scopes.pop_back();
	}

	REQUIRE(HeapProfile::Current() == before);
}

TEST_CASE("folded live bytes carry the tag path", "[heap]") {
	if (!HeapProfile::IsCompiledIn()) {
		SUCCEED("allocator hooks are not compiled in");
		return;
	}

	std::unique_ptr<char[]> block;
	{
		ENGINE_HEAP_SCOPE("heaptest-folded");
		ENGINE_HEAP_SCOPE("under");
		block = std::make_unique<char[]>(BLOCK);
	}

	FoldedStacks totals;
	HeapProfile::FoldLive(totals);

	const auto entry = totals.find("heaptest-folded;under");
	REQUIRE(entry != totals.end());
	REQUIRE(entry->second >= static_cast<double>(BLOCK));

	// Untagged is a line rather than a silence, so the rows add up to the
	// process.
	REQUIRE(totals.count("untagged") == 1);

	const std::filesystem::path path = std::filesystem::temp_directory_path() / "atomic-heap-folded-test.txt";
	std::filesystem::remove(path);
	REQUIRE(HeapProfile::WriteFolded(path));

	std::ifstream file(path);
	const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	REQUIRE(text.find("heaptest-folded;under") != std::string::npos);
	std::filesystem::remove(path);

	block.reset();
}

TEST_CASE("a leak has a slope and a fit and churn has neither", "[heap]") {
	if (!HeapProfile::IsCompiledIn()) {
		SUCCEED("allocator hooks are not compiled in");
		return;
	}

	// Two tags driven side by side over the same samples: one keeps every block
	// it takes, the other takes one and gives it back. The point of the case is
	// that a *total allocated* figure cannot tell them apart and a slope can.
	std::vector<std::unique_ptr<char[]>> kept;
	std::unique_ptr<char[]> recycled;

	HeapProfile::SetSamplingEnabled(true);
	for (int step = 0; step < 40; step++) {
		{
			ENGINE_HEAP_SCOPE("heaptest-leak");
			kept.push_back(std::make_unique<char[]>(BLOCK));
		}
		{
			ENGINE_HEAP_SCOPE("heaptest-churn");
			recycled.reset();
			recycled = std::make_unique<char[]>(BLOCK);
		}

		// The slope is per second, so the samples have to be spread over some.
		// A millisecond apart is enough for the clock to separate them and
		// keeps the case under a tenth of a second.
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
		HeapProfile::Sample();
	}

	const std::vector<HeapGrowth> growth = HeapProfile::Growth(0.0, BLOCK);
	HeapProfile::SetSamplingEnabled(false);

	const auto find = [&](const std::string &path) {
		return std::find_if(growth.begin(), growth.end(), [&](const HeapGrowth &entry) {
			return entry.Path == path;
		});
	};

	const auto leak = find("heaptest-leak");
	REQUIRE(leak != growth.end());
	REQUIRE(leak->BytesPerSecond > 0.0);
	REQUIRE(leak->LastBytes > leak->FirstBytes);

	// A straight line fits a straight line. Not 1.0: the samples are spaced by
	// a sleep the operating system rounds however it likes.
	REQUIRE(leak->Fit > 0.9);

	const auto churn = find("heaptest-churn");
	if (churn != growth.end()) {
		REQUIRE(churn->BytesPerSecond < leak->BytesPerSecond);
	}

	// Steepest first, which is what makes the report readable as a list of
	// suspects.
	for (size_t index = 1; index < growth.size(); index++) {
		REQUIRE(growth[index - 1].BytesPerSecond >= growth[index].BytesPerSecond);
	}

	kept.clear();
	recycled.reset();
}

TEST_CASE("sampling records a history and turning it on clears it", "[heap]") {
	if (!HeapProfile::IsCompiledIn()) {
		SUCCEED("allocator hooks are not compiled in");
		return;
	}

	HeapProfile::SetSamplingEnabled(true);
	for (int step = 0; step < 5; step++) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
		HeapProfile::Sample();
	}

	const std::vector<HeapSample> history = HeapProfile::History();
	REQUIRE(history.size() == 5);
	REQUIRE(HeapProfile::HistorySeconds() > 0.0);
	for (size_t index = 1; index < history.size(); index++) {
		REQUIRE(history[index].Seconds >= history[index - 1].Seconds);
	}

	// **A window with a gap of arbitrary length in the middle is the shape of a
	// false leak**, so a fresh capture starts empty rather than continuing the
	// last one.
	HeapProfile::SetSamplingEnabled(false);
	HeapProfile::SetSamplingEnabled(true);
	REQUIRE(HeapProfile::History().empty());
	HeapProfile::SetSamplingEnabled(false);
}

TEST_CASE("nothing is sampled while sampling is off", "[heap]") {
	HeapProfile::SetSamplingEnabled(false);
	HeapProfile::ResetHistory();

	HeapProfile::Sample();
	HeapProfile::Sample();

	REQUIRE(HeapProfile::History().empty());
	REQUIRE(HeapProfile::Growth().empty());
}

TEST_CASE("the totals account for what the tree holds", "[heap]") {
	if (!HeapProfile::IsCompiledIn()) {
		SUCCEED("allocator hooks are not compiled in");
		return;
	}

	const HeapTotals totals = HeapProfile::Totals();
	REQUIRE(totals.LiveBytes > 0);
	REQUIRE(totals.LiveBlocks > 0);
	REQUIRE(totals.PeakBytes >= totals.LiveBytes);
	REQUIRE(totals.TotalBytes >= static_cast<uint64_t>(totals.LiveBytes));
	REQUIRE(totals.Nodes >= 1);

	// **Zero, or the profiler is measuring a heap it does not own.** Any other
	// figure means a block reached this `operator delete` without having come
	// from this `operator new`, and every byte figure above is then a partial
	// count of an unknown fraction.
	REQUIRE(totals.ForeignFrees == 0);

	// The header cost is reported rather than hidden, because a `dev` footprint
	// compared against a `release` one is wrong by exactly this much.
	REQUIRE(totals.OverheadBytes == totals.LiveBlocks * static_cast<int64_t>(HeapProfile::BlockOverhead()));
	REQUIRE(HeapProfile::BlockOverhead() > 0);
}

TEST_CASE("a report names the totals and the heaviest tags", "[heap]") {
	std::unique_ptr<char[]> block;
	{
		ENGINE_HEAP_SCOPE("heaptest-report");
		block = std::make_unique<char[]>(BLOCK * 4);
	}

	const std::filesystem::path path = std::filesystem::temp_directory_path() / "atomic-heap-report-test.txt";
	std::filesystem::remove(path);
	REQUIRE(HeapProfile::WriteReport(path));

	std::ifstream file(path);
	const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	REQUIRE(text.find("heap profile") != std::string::npos);

	if (HeapProfile::IsCompiledIn()) {
		REQUIRE(text.find("heaptest-report") != std::string::npos);
		REQUIRE(text.find("untagged") != std::string::npos);
	} else {
		// A report that quietly omits everything reads as a program with no
		// heap. It says why instead.
		REQUIRE(text.find("not compiled in") != std::string::npos);
	}

	std::filesystem::remove(path);
	block.reset();
}

TEST_CASE("out of range nodes answer rather than read past the pool", "[heap]") {
	const HeapNodeView view = HeapProfile::Node(HeapProfile::MAXIMUM_NODES + 1);
	REQUIRE(view.Name.empty());
	REQUIRE(view.LiveBytes == 0);
	REQUIRE(HeapProfile::InclusiveBytes(HeapProfile::MAXIMUM_NODES + 1) == 0);
	REQUIRE(HeapProfile::Path(HeapProfile::MAXIMUM_NODES + 1).empty());

	// The root has no name of its own, so its path is empty rather than a
	// leading separator on every other path.
	REQUIRE(HeapProfile::Path(HeapProfile::ROOT).empty());
}
