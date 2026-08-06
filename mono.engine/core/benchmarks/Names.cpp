// What interning costs, and what the shared lock over the registry bought.
//
// `Name` is the bottom of the engine's identity story: a component name, a
// property name, a service name and a save-file field are all `Name`, so the
// registry is touched by every layer above L0 and a regression here is a
// regression everywhere. There is nowhere else these numbers can come from —
// running the client shows the total and never the share.
//
// **Three different costs live here and they are not comparable to each
// other.** Constructing from text that is already interned is a hash lookup
// under a shared lock; constructing from text that is not is a lookup, an
// insert and an exclusive lock; and comparing two `Name`s afterwards is an
// integer compare that should not be measurable at all. The whole design claim
// of the type is that the third number is negligible against the first, so all
// three are reported side by side rather than in separate suites.
//
// **The contended rows are the reason this file exists.** `Name.cpp` reads with
// a `std::shared_mutex` specifically so two worlds ticking on two workers do not
// serialise on every name either of them touches. A single-threaded row cannot
// see that either way: it takes the lock uncontended, which is cheap under any
// mutex. The `N threads` ladder is the only thing in the repository that says
// whether the shared lock is doing its job, and it is the row to read if
// somebody proposes simplifying it back to a plain mutex.

#include <engine/core/Name.hpp>
#include <engine/testing/Bench.hpp>

#include <atomic>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

TEST_SUITE_ID("engine.core.bench.names")

using engine::core::Name;
using engine::testing::Consume;

namespace names_bench {

	// How many distinct names the steady-state rows cycle through.
	//
	// Large enough that the registry's hash map is a real lookup rather than one
	// bucket the predictor has memorised, and small enough that the whole
	// working set stays in L2 — which is where a real engine's name traffic
	// lives, because the set of component and property names is fixed at load
	// and small.
	constexpr size_t POOL = 4096;

	// Names that look like the ones the engine actually interns.
	//
	// Length matters to a string hash and to the compare that follows a bucket
	// hit, so a pool of `"0"`, `"1"`, `"2"` would measure a hash over one byte
	// and report a number no real call site will ever see. These are the shape
	// of `"Transform"`, `"BasePart.Position"`, `"engine.render.overlay"`.
	const std::vector<std::string> &Texts() {
		static const std::vector<std::string> texts = [] {
			std::vector<std::string> built;
			built.reserve(POOL);
			for (size_t index = 0; index < POOL; index++) {
				built.push_back("engine.bench.name.Property" + std::to_string(index));
			}
			return built;
		}();
		return texts;
	}

	// The pool, interned once, so the steady-state rows are hits from the first
	// sample rather than measuring the growth on the first and the lookup on
	// the rest.
	const std::vector<Name> &Interned() {
		static const std::vector<Name> names = [] {
			std::vector<Name> built;
			built.reserve(POOL);
			for (const std::string &text : Texts()) {
				built.emplace_back(text);
			}
			return built;
		}();
		return names;
	}

	// Runs `body(worker)` on `threads` threads and waits for all of them.
	//
	// The spawn cost lands inside the measurement, which is the honest thing to
	// do here: it is the same on every rung of the ladder, so it cancels out of
	// the comparison the ladder exists to make. Absolute figures on these rows
	// are therefore an upper bound, and the ratio between rungs is the number to
	// read.
	template <class Body> void OnThreads(size_t threads, Body body) {
		std::vector<std::thread> workers;
		workers.reserve(threads);
		for (size_t worker = 0; worker < threads; worker++) {
			workers.emplace_back([&body, worker] { body(worker); });
		}
		for (std::thread &worker : workers) {
			worker.join();
		}
	}
}

using namespace names_bench;

// --- the floor ----------------------------------------------------------------
//
// What a `Name` costs once you have one. **Nothing in the registry can make
// this slower**, so it is the number every row below is read against: interning
// is worth doing exactly to the extent that this is smaller than a string
// compare.

BENCH("compare · two names", 100'000) {
	const std::vector<Name> &names = Interned();
	uint32_t matches = 0;
	for (size_t index = 0; index < 100'000; index++) {
		matches += (names[index % POOL] == names[(index + 1) % POOL]) ? 1u : 0u;
	}
	Consume(matches);
}

BENCH("control · compare two std::string", 100'000) {
	// The thing `Name` replaced, at the same call count. The gap between this
	// row and the one above is the entire argument for the type.
	const std::vector<std::string> &texts = Texts();
	uint32_t matches = 0;
	for (size_t index = 0; index < 100'000; index++) {
		matches += (texts[index % POOL] == texts[(index + 1) % POOL]) ? 1u : 0u;
	}
	Consume(matches);
}

// --- the lookup path ----------------------------------------------------------
//
// Constructing from text that is already interned, which is what every call
// site that ignored `Name.hpp`'s advice about hoisting a literal out of a loop
// is paying per call.

BENCH("Name(text) · already interned", 10'000) {
	const std::vector<std::string> &texts = Texts();
	for (size_t index = 0; index < 10'000; index++) {
		Consume(Name(texts[index % POOL]).Id());
	}
}

BENCH("Name(literal) · already interned", 10'000) {
	// The exact shape of the mistake the header warns about: a name constructed
	// from a literal inside a loop. Kept short because a literal in real code is
	// short, and because the difference against the row above is then the hash
	// over the extra bytes rather than anything structural.
	for (size_t index = 0; index < 10'000; index++) {
		Consume(Name("Transform").Id());
	}
}

BENCH("Exists · already interned", 10'000) {
	const std::vector<std::string> &texts = Texts();
	uint32_t found = 0;
	for (size_t index = 0; index < 10'000; index++) {
		found += Name::Exists(texts[index % POOL]) ? 1u : 0u;
	}
	Consume(found);
}

BENCH("Exists · never interned", 10'000) {
	// The miss path, which is the one a hostile or merely wrong input takes and
	// the one that walks a bucket to its end before giving up. It must not
	// intern anything — if this row's cost climbs across samples, `Exists` has
	// started inserting and the whole registry has a leak.
	uint32_t found = 0;
	for (size_t index = 0; index < 10'000; index++) {
		found += Name::Exists("engine.bench.name.absent") ? 1u : 0u;
	}
	Consume(found);
}

BENCH("Text · interned name", 10'000) {
	// The serialisation and diagnostic path. The header calls it "not a hot
	// one", and this row is what makes that claim checkable: it takes the same
	// shared lock the constructor does, so a change that made `Text` exclusive
	// would show here long before it showed as a frame-time regression.
	const std::vector<Name> &names = Interned();
	size_t bytes = 0;
	for (size_t index = 0; index < 10'000; index++) {
		bytes += names[index % POOL].Text().size();
	}
	Consume(bytes);
}

BENCH("FromId · interned id", 10'000) {
	const std::vector<Name> &names = Interned();
	for (size_t index = 0; index < 10'000; index++) {
		Consume(Name::FromId(names[index % POOL].Id()).Id());
	}
}

// --- as a map key -------------------------------------------------------------
//
// What the type is for, structurally: a dense integer handle that hashes to
// itself. Anything above L0 that keys a table by name pays this and not the
// string hash beside it.

BENCH("unordered_map<Name, int> · lookup", 10'000) {
	static const std::unordered_map<Name, int> table = [] {
		std::unordered_map<Name, int> built;
		const std::vector<Name> &names = Interned();
		for (size_t index = 0; index < names.size(); index++) {
			built.emplace(names[index], static_cast<int>(index));
		}
		return built;
	}();

	const std::vector<Name> &names = Interned();
	int total = 0;
	for (size_t index = 0; index < 10'000; index++) {
		total += table.find(names[index % POOL])->second;
	}
	Consume(total);
}

BENCH("control · unordered_map<string, int> · lookup", 10'000) {
	static const std::unordered_map<std::string, int> table = [] {
		std::unordered_map<std::string, int> built;
		const std::vector<std::string> &texts = Texts();
		for (size_t index = 0; index < texts.size(); index++) {
			built.emplace(texts[index], static_cast<int>(index));
		}
		return built;
	}();

	const std::vector<std::string> &texts = Texts();
	int total = 0;
	for (size_t index = 0; index < 10'000; index++) {
		total += table.find(texts[index % POOL])->second;
	}
	Consume(total);
}

// --- the contended path -------------------------------------------------------
//
// **Read this ladder as ratios, never as absolutes.** Each row does the same
// total work — `THREADED_LOOKUPS` lookups — split across more threads, so a
// registry that scaled perfectly would report a falling number and one that
// serialised would report a flat or rising one. The thread spawn is inside the
// measurement and does not divide, so the ideal is never actually reached; what
// the ladder answers is whether the curve bends the right way at all.
//
// Under a plain mutex every rung here is the same number or worse, because the
// lock is the only thing anybody is doing. Under the shared lock the readers do
// not interfere. That is the entire claim the comment in `Name.cpp` makes, and
// these four rows are where it is either true or not.

namespace names_bench {
	// Split across the ladder's rungs, so every row does the same total work.
	constexpr size_t THREADED_LOOKUPS = 96'000;
}

BENCH("Name(text) contended · 1 thread", THREADED_LOOKUPS) {
	const std::vector<std::string> &texts = Texts();
	OnThreads(1, [&texts](size_t worker) {
		for (size_t index = 0; index < THREADED_LOOKUPS / 1; index++) {
			Consume(Name(texts[(index + worker * 97) % POOL]).Id());
		}
	});
}

BENCH("Name(text) contended · 2 threads", THREADED_LOOKUPS) {
	const std::vector<std::string> &texts = Texts();
	OnThreads(2, [&texts](size_t worker) {
		for (size_t index = 0; index < THREADED_LOOKUPS / 2; index++) {
			Consume(Name(texts[(index + worker * 97) % POOL]).Id());
		}
	});
}

BENCH("Name(text) contended · 4 threads", THREADED_LOOKUPS) {
	const std::vector<std::string> &texts = Texts();
	OnThreads(4, [&texts](size_t worker) {
		for (size_t index = 0; index < THREADED_LOOKUPS / 4; index++) {
			Consume(Name(texts[(index + worker * 97) % POOL]).Id());
		}
	});
}

BENCH("Name(text) contended · 8 threads", THREADED_LOOKUPS) {
	const std::vector<std::string> &texts = Texts();
	OnThreads(8, [&texts](size_t worker) {
		for (size_t index = 0; index < THREADED_LOOKUPS / 8; index++) {
			Consume(Name(texts[(index + worker * 97) % POOL]).Id());
		}
	});
}

BENCH("Text contended · 8 threads", THREADED_LOOKUPS) {
	// The same ladder's top rung for the read that `WorldParallel` actually
	// makes most often. Separate from the constructor because `Text` does not
	// hash anything — if the constructor rows scale and this one does not, the
	// hash is what is serialising and not the lock.
	const std::vector<Name> &names = Interned();
	OnThreads(8, [&names](size_t worker) {
		size_t bytes = 0;
		for (size_t index = 0; index < THREADED_LOOKUPS / 8; index++) {
			bytes += names[(index + worker * 97) % POOL].Text().size();
		}
		Consume(bytes);
	});
}

// --- the growth path ----------------------------------------------------------

BENCH("Name(text) · first seen", 2048) {
	// **This row measures a registry that is bigger every sample, and that is
	// deliberate.** Interning something new is an insert, and an insert can only
	// happen once per string — so a steady-state version of this benchmark does
	// not exist. What is comparable is the row against itself over time: the
	// registry is append-only and never shrinks, so if the cost of a first
	// sighting starts climbing with the table's size, the map has stopped
	// rehashing sensibly.
	//
	// The harness takes the minimum of seven samples after two warm-ups, so the
	// figure reported is from an early sample and the later ones only widen the
	// spread. A spread far larger than the minimum on this row *is* the signal —
	// it means the ninth pass through cost visibly more than the first.
	//
	// 2048 per sample, nine samples, so this file adds at most ~18k permanent
	// entries to a process that exits immediately afterwards.
	static std::atomic<uint64_t> generation{0};
	const uint64_t mine = generation.fetch_add(1);

	// Built into one reused buffer rather than with `operator+`, because two
	// temporary strings per iteration would be two allocations landing inside a
	// measurement of a third. The prefix is written once and only the digits
	// after it are rewritten.
	static std::string scratch;
	scratch.assign("engine.bench.name.fresh.");
	scratch += std::to_string(mine);
	scratch += '.';
	const size_t stem = scratch.size();

	for (size_t index = 0; index < 2048; index++) {
		scratch.resize(stem);
		scratch += std::to_string(index);
		Consume(Name(scratch).Id());
	}
}

BENCH("Count", 100'000) {
	// Reported because it is the cheapest way to see the registry's size in a
	// bench report, and because it takes the same lock everything else does: a
	// `Count` that got slower is a lock that got slower.
	for (size_t index = 0; index < 100'000; index++) {
		Consume(Name::Count());
	}
}
