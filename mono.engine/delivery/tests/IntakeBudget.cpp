#include <engine/delivery/IntakeBudget.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <vector>

TEST_SUITE_ID("engine.delivery.intakebudget")

using engine::delivery::IntakeBudget;

namespace {

	// One frame of intake, written the way the studio's and the client's loops
	// write it: ask, take, spend - and put back whatever did not fit.
	//
	// Returns what was absorbed and leaves the rest in `pending`, which is the
	// property the whole thing rests on: a refused arrival is still an arrival.
	std::vector<size_t> DrainOneFrame(IntakeBudget &budget, std::vector<size_t> &pending) {
		budget.Begin();

		std::vector<size_t> absorbed;
		std::vector<size_t> held;

		for (const size_t bytes : pending) {
			if (!budget.Admits()) {
				budget.Defer();
				held.push_back(bytes);
				continue;
			}
			budget.Spend(bytes);
			absorbed.push_back(bytes);
		}

		pending = held;
		return absorbed;
	}
}

TEST_CASE("a frame absorbs up to its allowance and defers the rest", "[delivery][intake]") {
	// Six assets at 512 KiB against a 2 MiB frame: four fit, and the fifth is
	// what stops the budget being decorative.
	std::vector<size_t> pending(6, 512u * 1024u);

	IntakeBudget budget;
	const std::vector<size_t> absorbed = DrainOneFrame(budget, pending);

	CHECK(absorbed.size() == 4);
	CHECK(budget.Spent() == 2u * 1024u * 1024u);
	CHECK(budget.Absorbed() == 4);

	// **Deferred, not dropped**, which is the difference between a budget and a
	// loss - the two left are still waiting to be taken.
	CHECK(budget.Deferred() == 2);
	CHECK(pending.size() == 2);
}

TEST_CASE("what a frame defers is taken by the next one", "[delivery][intake]") {
	std::vector<size_t> pending(6, 512u * 1024u);

	IntakeBudget budget;
	DrainOneFrame(budget, pending);
	const std::vector<size_t> second = DrainOneFrame(budget, pending);

	// `Begin` forgets the last frame, so the leftovers land immediately rather
	// than being measured against a budget something else already spent.
	CHECK(second.size() == 2);
	CHECK(budget.Deferred() == 0);
	CHECK(pending.empty());
}

TEST_CASE("an asset larger than the whole budget is still admitted", "[delivery][intake]") {
	// **The case that would otherwise never load.** A 16 MiB mesh checked
	// against a 2 MiB allowance fails every frame for ever, and the thing it
	// belongs to stays invisible while the budget looks like it is working.
	std::vector<size_t> pending{16u * 1024u * 1024u, 1024u};

	IntakeBudget budget;
	const std::vector<size_t> absorbed = DrainOneFrame(budget, pending);

	REQUIRE(absorbed.size() == 1);
	CHECK(absorbed[0] == 16u * 1024u * 1024u);
	CHECK(budget.Spent() > budget.Allowance());

	// And it costs exactly one long frame: the small one behind it waits, and
	// the frame after is ordinary again.
	CHECK(pending.size() == 1);
	CHECK(DrainOneFrame(budget, pending).size() == 1);
}

TEST_CASE("a quiet frame spends nothing and defers nothing", "[delivery][intake]") {
	// The ordinary case - most frames have no content waiting at all, and this
	// pins that the budget costs them nothing and reports nothing happening.
	std::vector<size_t> pending;

	IntakeBudget budget;
	CHECK(DrainOneFrame(budget, pending).empty());
	CHECK(budget.Spent() == 0);
	CHECK(budget.Deferred() == 0);
	CHECK(budget.Admits());
}

TEST_CASE("a budget can be set smaller than a frame's default", "[delivery][intake]") {
	// A caller that wants tighter pacing - a low-end device, or a test - says
	// so at construction rather than editing a constant two modules away.
	std::vector<size_t> pending(4, 4096u);

	IntakeBudget budget(8192);
	const std::vector<size_t> absorbed = DrainOneFrame(budget, pending);

	CHECK(budget.Allowance() == 8192);
	CHECK(absorbed.size() == 2);
	CHECK(pending.size() == 2);
}
