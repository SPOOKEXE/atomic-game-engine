// The keybind table, and the file it survives in.
//
// **Reachable headlessly because almost none of it needs a frame.**
// `ImGui::GetKeyName` reads a static table and takes no context, so binding,
// conflict resolution and the whole save/load path are ordinary functions over
// ordinary data. What is *not* here is `Fired` and `Pressed`: both read
// `ImGuiIO` and a frame's key state, which needs a context, a display size and
// two `NewFrame` calls to mean anything - that is a backend test wearing a unit
// test's clothes, and `mono.studio/AGENTS.md` carries the invariant instead.
//
// The scope *decision* is still covered here, because the part that can be
// wrong without a keyboard is which scope a binding claims and whether the
// table survives a round trip.

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <studio/Keybinds.hpp>
#include <utility>
#include <vector>

TEST_SUITE_ID("studio.keybinds")

using studio::Action;
using studio::Chord;
using studio::Keybind;
using studio::Keybinds;
using studio::Scope;

namespace {
	// **Every case resets first**, because the table is process-wide by design
	// - `input::Actions` made the same choice - and a suite whose cases had to
	// run in one order would pass alone and fail together.
	struct Fixture {
		Fixture() {
			Keybinds::Reset();
			Keybinds::SetScope(Scope::Global);
		}
		~Fixture() {
			Keybinds::Reset();
		}
	};

	std::filesystem::path ScratchFile(const char *leaf) {
		return std::filesystem::temp_directory_path() / leaf;
	}
}

TEST_CASE("established editor shortcuts have built-in bindings", "[studio][keybinds]") {
	const Fixture fixture;

	// **The decision this suite exists to hold still.** Undo, redo and search
	// own their editor conventions, and Studio Play owns the same Escape
	// settings door as the shipped client. A build that adds another default
	// without deciding to will fail here.
	for (const Keybind &binding : Keybinds::All()) {
		INFO(binding.Id);
		if (binding.Bound == Action::Undo) {
			CHECK(binding.Keys == Chord{ImGuiKey_Z, true, false, false});
			CHECK(binding.Where == Scope::Editor);
		} else if (binding.Bound == Action::Redo) {
			CHECK(binding.Keys == Chord{ImGuiKey_Z, true, true, false});
			CHECK(binding.Where == Scope::Editor);
		} else if (binding.Bound == Action::SearchAllReplaceAll) {
			CHECK(binding.Keys == Chord{ImGuiKey_F, true, false, false});
		} else if (binding.Bound == Action::ClientSettings) {
			CHECK(binding.Keys == Chord{ImGuiKey_Escape, false, false, false});
		} else {
			CHECK_FALSE(binding.Keys.IsBound());
		}
	}
}

TEST_CASE("every action has a unique, non-empty id", "[studio][keybinds]") {
	const Fixture fixture;

	// The id is the only thing written to disk, so a duplicate is two commands
	// sharing one saved binding and an empty one is a command that cannot be
	// saved at all. Neither is visible until somebody restarts the editor.
	std::set<std::string> seen;

	for (const Keybind &binding : Keybinds::All()) {
		INFO(binding.Name);
		CHECK(binding.Id != nullptr);
		CHECK(std::string(binding.Id).empty() == false);
		CHECK(seen.insert(binding.Id).second);
	}
}

TEST_CASE("stable command ids resolve without relying on enum order", "[studio][keybinds]") {
	const Fixture fixture;

	const Keybind *play = Keybinds::Find("run.play");
	REQUIRE(play != nullptr);
	CHECK(play->Bound == Action::Play);
	CHECK(Keybinds::Find("command.that.does.not.exist") == nullptr);
}

TEST_CASE("a chord belongs to exactly one action", "[studio][keybinds]") {
	const Fixture fixture;

	const Chord save{ImGuiKey_S, true, false, false};

	Keybinds::Set(Action::Save, save);
	CHECK(Keybinds::Of(Action::Save) == save);
	CHECK(Keybinds::Holder(save) == Action::Save);

	// **The one that was already there loses it.** Two actions on one key is a
	// key that does two things at once, and the one somebody notices is
	// whichever happens to be checked first - a bug that reads as the editor
	// being haunted.
	Keybinds::Set(Action::SaveAs, save);

	CHECK(Keybinds::Of(Action::SaveAs) == save);
	CHECK_FALSE(Keybinds::Of(Action::Save).IsBound());
	CHECK(Keybinds::Holder(save) == Action::SaveAs);

	// The row being edited does not count as its own conflict, which is what
	// lets the page say "this is free" while showing the key already in it.
	CHECK(Keybinds::Holder(save, Action::SaveAs) == Action::Count);
}

TEST_CASE("modifiers are part of the chord", "[studio][keybinds]") {
	const Fixture fixture;

	const Chord save{ImGuiKey_S, true, false, false};
	const Chord saveAs{ImGuiKey_S, true, true, false};

	Keybinds::Set(Action::Save, save);
	Keybinds::Set(Action::SaveAs, saveAs);

	// Ctrl+S and Ctrl+Shift+S are different keys. Treating them as the same one
	// is how Save As also saves, which silently overwrites the wrong file.
	CHECK(Keybinds::Of(Action::Save) == save);
	CHECK(Keybinds::Of(Action::SaveAs) == saveAs);
	CHECK(Keybinds::Holder(save) == Action::Save);
	CHECK(Keybinds::Holder(saveAs) == Action::SaveAs);
}

TEST_CASE("a chord reads the way a person writes it", "[studio][keybinds]") {
	const Fixture fixture;

	CHECK(Chord{}.Text().empty());
	CHECK(Chord{ImGuiKey_F5}.Text() == "F5");
	CHECK((Chord{ImGuiKey_S, true, false, false}.Text()) == "Ctrl+S");
	CHECK((Chord{ImGuiKey_S, true, true, false}.Text()) == "Ctrl+Shift+S");
}

TEST_CASE("resetting clears what was bound", "[studio][keybinds]") {
	const Fixture fixture;

	Keybinds::Set(Action::Play, Chord{ImGuiKey_F5});
	REQUIRE(Keybinds::Of(Action::Play).IsBound());

	Keybinds::Reset();

	CHECK_FALSE(Keybinds::Of(Action::Play).IsBound());
}

TEST_CASE("bindings survive a save and a load", "[studio][keybinds]") {
	const Fixture fixture;

	const std::filesystem::path path = ScratchFile("atomic-keybinds-roundtrip.ini");
	std::filesystem::remove(path);

	const Chord play{ImGuiKey_F5};
	const Chord saveAs{ImGuiKey_S, true, true, false};

	Keybinds::Set(Action::Play, play);
	Keybinds::Set(Action::SaveAs, saveAs);

	REQUIRE(Keybinds::Save(path));

	// Everything forgotten, then read back. A table that only appeared to
	// persist because it had never been cleared would pass a weaker test.
	Keybinds::Reset();
	REQUIRE_FALSE(Keybinds::Of(Action::Play).IsBound());

	REQUIRE(Keybinds::Load(path));

	CHECK(Keybinds::Of(Action::Play) == play);
	CHECK(Keybinds::Of(Action::SaveAs) == saveAs);

	// Deliberately unbound survives too: "I cleared this" is a decision, and a
	// load that restored the default over it would undo it on every restart.
	CHECK_FALSE(Keybinds::Of(Action::Save).IsBound());

	std::filesystem::remove(path);
}

TEST_CASE("a missing file is not an error", "[studio][keybinds]") {
	const Fixture fixture;

	// A fresh install has no file and every action keeps its default. Reporting
	// that as a failure would put a warning in front of somebody on first run
	// about a file they were never going to have.
	CHECK_FALSE(Keybinds::Load(ScratchFile("atomic-keybinds-does-not-exist.ini")));
}

TEST_CASE("a file naming an unknown command leaves the rest alone", "[studio][keybinds]") {
	const Fixture fixture;

	const std::filesystem::path path = ScratchFile("atomic-keybinds-unknown.ini");

	{
		std::ofstream out(path, std::ios::trunc);
		out << "# a comment, and a blank line follow\n";
		out << "\n";
		out << "run.play = F5\n";

		// A command from a later build. Not corruption - a file from a later
		// version - so it is skipped rather than refusing the whole table.
		out << "some.command.from.the.future = Ctrl+K\n";
		out << "run.server = F6\n";
	}

	REQUIRE(Keybinds::Load(path));

	CHECK(Keybinds::Of(Action::Play) == Chord{ImGuiKey_F5});
	CHECK(Keybinds::Of(Action::RunServer) == Chord{ImGuiKey_F6});

	std::filesystem::remove(path);
}

TEST_CASE("an unreadable key clears the binding rather than guessing", "[studio][keybinds]") {
	const Fixture fixture;

	Keybinds::Set(Action::Play, Chord{ImGuiKey_F5});

	const std::filesystem::path path = ScratchFile("atomic-keybinds-garbage.ini");
	{
		std::ofstream out(path, std::ios::trunc);
		out << "run.play = Ctrl+NotAKeyName\n";
	}

	REQUIRE(Keybinds::Load(path));

	// The file said something about this command. Leaving F5 in place would be
	// inventing an answer the file did not give.
	CHECK_FALSE(Keybinds::Of(Action::Play).IsBound());

	std::filesystem::remove(path);
}

TEST_CASE("the scope a binding claims survives being read back", "[studio][keybinds]") {
	const Fixture fixture;

	// Scope is a property of the command rather than of the binding, so it is
	// not written to the file - which means a build may change it and every
	// saved key still means what it meant. What must hold is that the table
	// actually assigns one, and that editing keys never moves it.
	for (const Keybind &binding : Keybinds::All()) {
		INFO(binding.Id);
		CHECK((
			binding.Where == Scope::Global || binding.Where == Scope::Editor ||
			binding.Where == Scope::Viewport || binding.Where == Scope::Tree || binding.Where == Scope::Script
		));
	}

	Keybinds::Set(Action::Delete, Chord{ImGuiKey_Delete});

	for (const Keybind &binding : Keybinds::All()) {
		if (binding.Bound == Action::Delete) {
			// Deleting the selection belongs to the tree. Bound globally it
			// would fire while somebody was typing in a script.
			CHECK(binding.Where == Scope::Tree);
		}
	}
}

TEST_CASE("the active scope is what was last set", "[studio][keybinds]") {
	const Fixture fixture;

	CHECK(Keybinds::CurrentScope() == Scope::Global);

	Keybinds::SetScope(Scope::Script);
	CHECK(Keybinds::CurrentScope() == Scope::Script);

	Keybinds::SetScope(Scope::Tree);
	CHECK(Keybinds::CurrentScope() == Scope::Tree);
}

TEST_CASE("every action addresses its own row", "[studio][keybinds]") {
	const Fixture fixture;

	// **The check that would have caught seven cross-wired bindings.** The
	// action enum and the `DEFAULTS` table are two lists of the same things
	// written in two places, and they disagreed: the enum put
	// `ShowStatistics`, `ShowFrameGraph` and `ShowHeap` before the four tools
	// and the table put the tools first. `IndexOf` cast the enum to a
	// subscript, so binding "Select Tool" wrote onto the frame graph's row.
	//
	// The counts matched, so it compiled and every menu label looked plausible.
	// The cases above did not find it because they exercise `Save`, `SaveAs`,
	// `Play`, `RunServer` and `Delete` - all inside the prefix where the two
	// orders happen to agree.
	//
	// Asserted through the public surface rather than on `IndexOf`, which is
	// private: `All()` hands back the rows, so a row's `Bound` is what says
	// which action it is, and `Of` is what everything else reads.
	// **Asserted after a `Set`, because both sides agree while nothing is
	// bound.** `Of` and `Set` use the same index, so a wrong index is
	// self-consistent - and the seven cross-wired rows all ship *unbound*, so
	// reading them straight compares one empty string against another and finds
	// nothing. What the page shows is `All()`, and what the page writes is
	// `Set(row.Bound, ...)`; the bug is that those two disagree.
	Chord marker{ImGuiKey_F9};
	marker.Ctrl = true;
	marker.Alt = true;

	std::vector<Action> subjects;
	for (const Keybind &binding : Keybinds::All()) {
		subjects.push_back(binding.Bound);
	}

	for (const Action subject : subjects) {
		const Chord original = Keybinds::Of(subject);
		Keybinds::Set(subject, marker);

		bool found = false;
		for (const Keybind &row : Keybinds::All()) {
			if (row.Bound != subject) {
				continue;
			}
			found = true;
			INFO(row.Id);
			CHECK(row.Keys.Text() == marker.Text());
		}
		CHECK(found);

		Keybinds::Set(subject, original);
	}
}

TEST_CASE("binding one action leaves every other alone", "[studio][keybinds]") {
	const Fixture fixture;

	// The report was "keybinds do not set properly (changes other keybinds)",
	// and this is that sentence as a test: bind each action in turn to
	// something unmistakable and check that nothing else moved.
	// **Every action's own row, taken by value first.** `All()` hands back the
	// live rows, so anything read through that reference after a `Set` is the
	// value `Set` just wrote - restoring from it would put the marker back
	// rather than the original, and the next iteration would find the marker
	// already held.
	std::vector<std::pair<Action, std::string>> subjects;
	for (const Keybind &binding : Keybinds::All()) {
		subjects.emplace_back(binding.Bound, std::string(binding.Id));
	}

	for (const auto &[subject, id] : subjects) {
		const Chord original = Keybinds::Of(subject);

		std::vector<std::pair<Action, std::string>> before;
		for (const auto &[other, ignored] : subjects) {
			if (other != subject) {
				before.emplace_back(other, Keybinds::Of(other).Text());
			}
		}

		// **A chord nothing holds**, because `Set` deliberately takes a chord
		// away from whoever had it - `Keybinds::Holder` is that rule - and a
		// marker some other action already owned would make this assert that a
		// designed behaviour is a bug.
		Chord marker{ImGuiKey_F12};
		marker.Ctrl = true;
		marker.Shift = true;
		marker.Alt = true;
		REQUIRE(Keybinds::Holder(marker) == Action::Count);

		Keybinds::Set(subject, marker);

		INFO(id);
		CHECK(Keybinds::Of(subject).Text() == marker.Text());

		for (const auto &[action, text] : before) {
			INFO(id);
			CHECK(Keybinds::Of(action).Text() == text);
		}

		Keybinds::Set(subject, original);
	}
}
