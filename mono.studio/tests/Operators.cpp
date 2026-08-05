// The operator table, and the join that stops it drifting from `Keybinds`.
//
// **The case that matters most in this file is the dullest one.** `Operators.hpp`
// accepts two tables joined by an enum — the binding table in `Keybinds.cpp` and
// the operator registrations in `Palette.cpp` — on the explicit condition that
// the join is *checked* rather than trusted. "Every action has exactly one
// operator" is that check, and without it a command added to one table and not
// the other is missing from the palette with nothing reporting it.
//
// The rest is the poll: an operator that says it cannot run must not run, and
// must say why in words somebody can act on.

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <studio/Keybinds.hpp>
#include <studio/Operators.hpp>

#include <span>
#include <string>

TEST_SUITE_ID("studio.operators")

using studio::Action;
using studio::Availability;
using studio::Operator;
using studio::OperatorTable;

namespace {
	// An operator that is always available and counts how often it ran.
	Operator Counting(Action id, std::string_view name, int &runs) {
		return Operator{
			id, name, "", [] { return Availability::Yes(); }, [&runs] { runs++; }
		};
	}

	// An operator that refuses, with a reason.
	Operator Refusing(Action id, std::string_view name, std::string reason, int &runs) {
		return Operator{
			id,
			name,
			"",
			[reason] { return Availability::No(reason); },
			[&runs] { runs++; }
		};
	}
}

TEST_CASE("an operator runs when its poll allows it", "[studio][operators]") {
	OperatorTable table;
	int runs = 0;

	REQUIRE(table.Add(Counting(Action::Save, "Save", runs)));
	REQUIRE(table.Available(Action::Save).Ready);

	CHECK(table.Run(Action::Save));
	CHECK(runs == 1);
}

TEST_CASE("an operator that refuses does not run, and says why", "[studio][operators]") {
	OperatorTable table;
	int runs = 0;

	REQUIRE(table.Add(Refusing(Action::Delete, "Delete", "nothing is selected", runs)));

	const Availability state = table.Available(Action::Delete);
	CHECK_FALSE(state.Ready);
	CHECK(state.Reason == "nothing is selected");

	// The whole point of the poll: the caller does not have to attempt it to
	// find out, and attempting it anyway is refused rather than half-done.
	CHECK_FALSE(table.Run(Action::Delete));
	CHECK(runs == 0);
}

TEST_CASE("an unregistered command is unavailable rather than enabled", "[studio][operators]") {
	OperatorTable table;

	const Availability state = table.Available(Action::Play);
	CHECK_FALSE(state.Ready);
	CHECK_FALSE(state.Reason.empty());

	// A menu item that is clickable and does nothing is worse than one greyed.
	CHECK_FALSE(table.Run(Action::Play));
	CHECK(table.Find(Action::Play) == nullptr);
}

TEST_CASE("one action cannot carry two operators", "[studio][operators]") {
	OperatorTable table;
	int first = 0;
	int second = 0;

	REQUIRE(table.Add(Counting(Action::Undo, "Undo", first)));
	CHECK_FALSE(table.Add(Counting(Action::Undo, "Undo Again", second)));

	CHECK(table.All().size() == 1);

	// Silently keeping the last registration would hide the duplicate behind
	// whichever call happened to run second.
	REQUIRE(table.Run(Action::Undo));
	CHECK(first == 1);
	CHECK(second == 0);
}

TEST_CASE("an operator with no poll or no body is refused", "[studio][operators]") {
	OperatorTable table;

	CHECK_FALSE(table.Add(Operator{Action::Save, "Save", "", nullptr, [] {}}));
	CHECK_FALSE(table.Add(Operator{Action::Save, "Save", "", [] { return Availability::Yes(); }, nullptr}));
	CHECK(table.All().empty());
}

TEST_CASE("an empty query keeps registration order", "[studio][operators]") {
	OperatorTable table;
	int runs = 0;

	table.Add(Counting(Action::NewGame, "New Game", runs));
	table.Add(Counting(Action::Save, "Save", runs));
	table.Add(Counting(Action::Delete, "Delete", runs));

	const std::vector<const Operator *> found = table.Matching("");

	REQUIRE(found.size() == 3);
	CHECK(found[0]->Id == Action::NewGame);
	CHECK(found[1]->Id == Action::Save);
	CHECK(found[2]->Id == Action::Delete);
}

TEST_CASE("the palette filters and ranks the way every other filter does", "[studio][operators]") {
	OperatorTable table;
	int runs = 0;

	table.Add(Counting(Action::NewGame, "New Game", runs));
	table.Add(Counting(Action::Save, "Save", runs));
	table.Add(Counting(Action::SaveAs, "Save As", runs));

	const std::vector<const Operator *> found = table.Matching("save");

	REQUIRE(found.size() == 2);

	// An exact name outranks one that merely contains it — the rule
	// `Widgets.cpp` already establishes and which this reuses rather than
	// reinvents.
	CHECK(found[0]->Id == Action::Save);
	CHECK(found[1]->Id == Action::SaveAs);
}

TEST_CASE("an unavailable operator still appears in the palette", "[studio][operators]") {
	OperatorTable table;
	int runs = 0;

	table.Add(Refusing(Action::Delete, "Delete", "nothing is selected", runs));

	// Filtering it out would answer "why can I not find Delete" with silence.
	const std::vector<const Operator *> found = table.Matching("del");
	REQUIRE(found.size() == 1);
	CHECK_FALSE(found[0]->Poll().Ready);
}

TEST_CASE("every action is namable and takes one operator", "[studio][operators]") {
	// **Read what this does and does not cover before trusting it.** It builds
	// a synthetic operator per keybind row and checks that the table accepts
	// exactly one of each — so it covers `Keybinds`' table being complete and
	// `OperatorTable::Add` refusing a duplicate.
	//
	// It does **not** reach `Editor::RegisterOperators`, which is the list in
	// `Palette.cpp` that the palette and the menus actually walk. The synthetic
	// registrations below are built *from* the keybind table, so a new `Action`
	// brings its own synthetic operator with it and the count matches whether or
	// not anybody registered a real one.
	//
	// That leaves the gap `Operators.hpp` says this suite closes: an `Action`
	// with a binding and no `Operators.Add` is a command with a key, no palette
	// entry and no behaviour, and nothing here goes red. Closing it needs the
	// registration list reachable without an `Editor` — see the review note on
	// splitting the descriptors from the closures.
	OperatorTable table;
	int runs = 0;

	const std::span<studio::Keybind> bindings = studio::Keybinds::All();
	REQUIRE(bindings.size() == static_cast<size_t>(Action::Count));

	for (const studio::Keybind &binding : bindings) {
		INFO("binding " << binding.Id);

		// Every action must be namable — the binding table is the list of what
		// exists, and an action missing from it has no id, no name and no scope.
		CHECK_FALSE(std::string_view(binding.Id).empty());
		CHECK_FALSE(std::string_view(binding.Name).empty());

		CHECK(table.Add(Counting(binding.Bound, binding.Name, runs)));
	}

	CHECK(table.All().size() == static_cast<size_t>(Action::Count));
}
