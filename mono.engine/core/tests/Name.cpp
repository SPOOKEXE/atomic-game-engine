#include <engine/core/Name.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

TEST_SUITE_ID("engine.core.name")

using engine::core::Name;

namespace {
	// The registry is process-wide and entries are never removed, so every case
	// has to use text nothing else will. A counter would collide between runs
	// of the same binary under Catch2's section reruns.
	std::string Unique(const char *label) {
		static int counter = 0;
		return std::string("test.") + label + "." + std::to_string(counter++);
	}
}

TEST_CASE("the same text interns to the same id", "[name]") {
	const std::string text = Unique("same");

	const Name first(text);
	const Name second(text);

	REQUIRE(first.IsValid());
	REQUIRE(first == second);
	REQUIRE(first.Id() == second.Id());
}

TEST_CASE("different text interns to different ids", "[name]") {
	REQUIRE(Name(Unique("a")) != Name(Unique("b")));
}

TEST_CASE("the text comes back", "[name]") {
	const std::string text = Unique("round-trip");
	REQUIRE(Name(text).Text() == text);
}

TEST_CASE("a default Name is invalid and has no text", "[name]") {
	const Name empty;

	REQUIRE_FALSE(empty.IsValid());
	REQUIRE_FALSE(static_cast<bool>(empty));
	REQUIRE(empty.Id() == Name::INVALID);
	REQUIRE(empty.Text().empty());
}

TEST_CASE("empty text is not a name", "[name]") {
	REQUIRE_FALSE(Name("").IsValid());
}

TEST_CASE("a view into a temporary is safe to intern", "[name]") {
	const std::string text = Unique("temporary");

	// The registry keys on a view of its own copy, not the caller's argument.
	// Keying on the argument would dangle the moment this scope ends.
	Name interned;
	{
		const std::string temporary = text;
		interned = Name(std::string_view(temporary));
	}

	REQUIRE(interned.Text() == text);
}

TEST_CASE("text stays valid after many more names are interned", "[name]") {
	const std::string text = Unique("stable");
	const Name early(text);
	const std::string_view view = early.Text();

	// A vector would reallocate and dangle every view already handed out. The
	// registry uses a deque for exactly this.
	for (int index = 0; index < 5'000; index++) {
		Name(Unique("filler"));
	}

	REQUIRE(view == text);
	REQUIRE(early.Text() == text);
}

TEST_CASE("FromId returns the same name", "[name]") {
	const std::string text = Unique("from-id");
	const Name original(text);

	const Name recovered = Name::FromId(original.Id());
	REQUIRE(recovered == original);
	REQUIRE(recovered.Text() == text);
}

TEST_CASE("FromId on an unknown id is invalid rather than a crash", "[name]") {
	REQUIRE_FALSE(Name::FromId(900'000).IsValid());
	REQUIRE_FALSE(Name::FromId(Name::INVALID).IsValid());
}

TEST_CASE("Exists does not intern", "[name]") {
	const std::string text = Unique("exists");

	const size_t before = Name::Count();
	REQUIRE_FALSE(Name::Exists(text));
	REQUIRE(Name::Count() == before);

	// Braces, not parentheses: `Name(text);` declares a variable called text.
	Name{text};
	REQUIRE(Name::Exists(text));
}

TEST_CASE("Reserve pins a name to a chosen id", "[name]") {
	const std::string text = Unique("reserved");

	const Name pinned = Name::Reserve(text, 500'000);
	REQUIRE(pinned.IsValid());
	REQUIRE(pinned.Id() == 500'000);
	REQUIRE(pinned.Text() == text);

	// Interning the same text afterwards has to find the pin, not allocate a
	// second id for the same name.
	REQUIRE(Name(text) == pinned);
}

TEST_CASE("Reserve is idempotent for the same pairing", "[name]") {
	const std::string text = Unique("re-reserved");

	REQUIRE(Name::Reserve(text, 500'100).Id() == 500'100);
	REQUIRE(Name::Reserve(text, 500'100).Id() == 500'100);
}

TEST_CASE("Reserve refuses a contradiction", "[name]") {
	const std::string first = Unique("clash-a");
	const std::string second = Unique("clash-b");

	REQUIRE(Name::Reserve(first, 500'200).IsValid());

	// Two names on one id would compare equal, and one name on two ids would
	// make the mapping ambiguous. Both are startup errors worth failing on.
	REQUIRE_FALSE(Name::Reserve(second, 500'200).IsValid());
	REQUIRE_FALSE(Name::Reserve(first, 500'201).IsValid());
}

TEST_CASE("an auto-assigned id never lands on a reserved one", "[name]") {
	const std::string reserved = Unique("high-water");
	const Name pinned = Name::Reserve(reserved, 600'000);
	REQUIRE(pinned.IsValid());

	for (int index = 0; index < 32; index++) {
		const Name fresh(Unique("after-pin"));
		REQUIRE(fresh.Id() != pinned.Id());

		// And it stays *dense*. Jumping the counter past a high pin would be
		// the easy way to avoid the collision, and it would throw away the
		// reason for having a counter — as well as consuming every id below
		// the pin that something else might want to reserve later.
		REQUIRE(fresh.Id() < pinned.Id());
	}
}

TEST_CASE("ids are usable as map keys", "[name]") {
	std::unordered_set<Name> names;
	const std::string text = Unique("hashable");

	names.insert(Name(text));
	names.insert(Name(text));

	REQUIRE(names.size() == 1);
	REQUIRE(names.count(Name(text)) == 1);
}

TEST_CASE("interning the same text from many threads yields one id", "[name]") {
	const std::string text = Unique("contended");

	std::vector<std::thread> threads;
	std::vector<Name> results(8);
	for (size_t index = 0; index < results.size(); index++) {
		threads.emplace_back([&results, &text, index] { results[index] = Name(text); });
	}
	for (auto &thread : threads) {
		thread.join();
	}

	// A race here would hand out two ids for one name, and two things that
	// should be equal would silently not be.
	for (const auto &name : results) {
		REQUIRE(name == results.front());
	}
	REQUIRE(results.front().IsValid());
}

TEST_CASE("a racing first sighting still yields one id and one entry", "[name]") {
	// **The case the lookup's fast path opened, pinned so it cannot close
	// silently.** `Name(text)` looks the registry up under a *shared* lock,
	// because the hit is almost every call and excluding readers for it made
	// eight threads slower than one. A `shared_mutex` cannot upgrade, so a miss
	// drops that lock and takes an exclusive one — and in the gap between the
	// two, another thread may have interned the very same text.
	//
	// The re-check under the exclusive lock is what handles that. Without it,
	// both threads insert: `Texts` gains two rows, `AllocateId` hands out two
	// ids, and `Ids` keeps whichever landed first. The two threads then hold
	// different ids for one string, which is the single thing this type exists
	// to make impossible — and everything downstream that compares names by
	// integer is quietly wrong for the rest of the process.
	//
	// The case above races one text with threads started in a loop, which is a
	// narrow window: the first thread has usually finished before the last has
	// started. This one holds every thread at a gate and releases them together,
	// and does it over many texts, so the interleaving is actually attempted
	// rather than hoped for.
	constexpr size_t THREADS = 8;
	constexpr size_t TEXTS = 256;

	std::vector<std::string> texts;
	texts.reserve(TEXTS);
	for (size_t index = 0; index < TEXTS; index++) {
		texts.push_back(Unique("raced"));
	}

	// None of them are interned yet, so every one is a first sighting and every
	// one is a chance to hit the window.
	const size_t before = Name::Count();

	std::atomic<bool> go{false};
	std::atomic<size_t> ready{0};
	std::vector<std::vector<Name>> seen(THREADS, std::vector<Name>(TEXTS));

	std::vector<std::thread> threads;
	threads.reserve(THREADS);
	for (size_t worker = 0; worker < THREADS; worker++) {
		threads.emplace_back([&, worker] {
			ready.fetch_add(1);
			while (!go.load(std::memory_order_acquire)) {
				// Spun rather than waited on a condition variable: what this
				// needs is for every thread to be inside the registry at the
				// same instant, and a wake-up that queues them is the opposite.
			}
			for (size_t index = 0; index < TEXTS; index++) {
				seen[worker][index] = Name(texts[index]);
			}
		});
	}

	while (ready.load() < THREADS) {}
	go.store(true, std::memory_order_release);

	for (std::thread &thread : threads) {
		thread.join();
	}

	// Every thread agrees on every text.
	for (size_t index = 0; index < TEXTS; index++) {
		REQUIRE(seen[0][index].IsValid());
		for (size_t worker = 1; worker < THREADS; worker++) {
			REQUIRE(seen[worker][index] == seen[0][index]);
		}
	}

	// **And exactly one entry was made per text.** This is the half the
	// id-equality check above cannot see on its own: a duplicate insert that
	// happened to lose the `Ids` race would leave an orphaned row in `Texts`
	// and an id bound to it, so the count is what says the double-check ran.
	REQUIRE(Name::Count() == before + TEXTS);
}
