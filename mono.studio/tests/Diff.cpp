// Comparing the live game against the file on disk.
//
// **A comparator that reports no changes looks exactly like a clean tree.**
// That is why this suite exists and why the algorithm is a free function: the
// panel can draw a convincing "no changes" over a diff that never found any,
// and nothing about the picture would say which it was.
//
// The cases are text in, text out. What the panel does with the result - the
// colours, the counts, the refresh button - needs a window and is covered by
// running the editor.

#include <engine/testing/Suite.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <studio/Editor.hpp>
#include <vector>

TEST_SUITE_ID("studio.diff")

using studio::DiffKind;
using studio::DiffLine;
using studio::DiffText;
using studio::ParseSavedChangeXml;
using studio::SavedChange;
using studio::SavedChangesDirectory;
using studio::SavedChangeXml;

namespace {
	// The rows of one kind, so a case can say what changed without counting
	// past the unchanged context.
	std::vector<std::string> Of(const std::vector<DiffLine> &lines, DiffKind kind) {
		std::vector<std::string> found;
		for (const DiffLine &line : lines) {
			if (line.Kind == kind) {
				found.push_back(line.Text);
			}
		}
		return found;
	}
}

TEST_CASE("identical documents produce no rows at all", "[studio][diff]") {
	// **The case the whole panel hangs on.** "Nothing changed" has to be
	// distinguishable from "the comparison did not run", and an empty result is
	// how this one says the first.
	const std::string document = "one\ntwo\nthree\n";
	CHECK(DiffText(document, document).empty());
}

TEST_CASE("an empty comparison against an empty document is empty", "[studio][diff]") {
	CHECK(DiffText("", "").empty());
}

TEST_CASE("a changed line is one removal and one addition", "[studio][diff]") {
	const std::vector<DiffLine> lines = DiffText("one\ntwo\nthree\n", "one\nTWO\nthree\n");

	CHECK(Of(lines, DiffKind::Removed) == std::vector<std::string>{"two"});
	CHECK(Of(lines, DiffKind::Added) == std::vector<std::string>{"TWO"});
}

TEST_CASE("an added line is an addition and nothing else", "[studio][diff]") {
	// **The direction that matters most in practice**, because it is what
	// adding a part to a scene looks like. Reporting it as a rewrite of
	// everything after it would make every insertion look like a catastrophe.
	const std::vector<DiffLine> lines = DiffText("one\ntwo\n", "one\nmiddle\ntwo\n");

	CHECK(Of(lines, DiffKind::Added) == std::vector<std::string>{"middle"});
	CHECK(Of(lines, DiffKind::Removed).empty());
}

TEST_CASE("a removed line is a removal and nothing else", "[studio][diff]") {
	const std::vector<DiffLine> lines = DiffText("one\nmiddle\ntwo\n", "one\ntwo\n");

	CHECK(Of(lines, DiffKind::Removed) == std::vector<std::string>{"middle"});
	CHECK(Of(lines, DiffKind::Added).empty());
}

TEST_CASE("everything added to an empty file is an addition", "[studio][diff]") {
	const std::vector<DiffLine> lines = DiffText("", "one\ntwo\n");

	CHECK(Of(lines, DiffKind::Added) == std::vector<std::string>{"one", "two"});
	CHECK(Of(lines, DiffKind::Removed).empty());
}

TEST_CASE("everything removed leaves only removals", "[studio][diff]") {
	const std::vector<DiffLine> lines = DiffText("one\ntwo\n", "");

	CHECK(Of(lines, DiffKind::Removed) == std::vector<std::string>{"one", "two"});
	CHECK(Of(lines, DiffKind::Added).empty());
}

TEST_CASE("a trailing newline does not invent a line", "[studio][diff]") {
	// A file written with a trailing newline and one written without differ by
	// nothing a person cares about, and a phantom empty row on every save would
	// make the panel cry wolf once per save for ever.
	CHECK(DiffText("one\ntwo\n", "one\ntwo").empty());
	CHECK(DiffText("one\ntwo", "one\ntwo\n").empty());
}

TEST_CASE("two separate changes are reported separately", "[studio][diff]") {
	// **What the alignment buys.** A prefix/suffix trim alone would collapse
	// these into one hunk spanning the untouched middle, which is the coarse
	// answer the size cap falls back to - correct, and much less useful.
	const std::vector<DiffLine> lines = DiffText("a\nb\nc\nd\ne\n", "a\nB\nc\nd\nE\n");

	CHECK(Of(lines, DiffKind::Removed) == std::vector<std::string>{"b", "e"});
	CHECK(Of(lines, DiffKind::Added) == std::vector<std::string>{"B", "E"});

	// And the untouched middle survives as context rather than being reported
	// as a change.
	const std::vector<std::string> same = Of(lines, DiffKind::Same);
	CHECK(std::find(same.begin(), same.end(), "c") != same.end());
	CHECK(std::find(same.begin(), same.end(), "d") != same.end());
}

TEST_CASE("the coarse flag is off for an ordinary edit", "[studio][diff]") {
	bool coarse = true;
	const std::vector<DiffLine> lines = DiffText("one\ntwo\n", "one\nTWO\n", &coarse);

	CHECK_FALSE(lines.empty());
	CHECK_FALSE(coarse);
}

TEST_CASE("a comparison too large to align says so rather than stalling", "[studio][diff]") {
	// **The bound, and the reason it is reported.** The alignment table is
	// O(n·m), so a rewritten scene would otherwise be a multi-second stall
	// inside a frame. Degrading is fine; degrading silently is not, because a
	// coarse diff presented as a fine one reads as "these thousand lines all
	// changed".
	std::string left;
	std::string right;
	for (int line = 0; line < 3000; line++) {
		left += "left " + std::to_string(line) + "\n";
		right += "right " + std::to_string(line) + "\n";
	}

	bool coarse = false;
	const std::vector<DiffLine> lines = DiffText(left, right, &coarse);

	CHECK(coarse);
	CHECK(Of(lines, DiffKind::Removed).size() == 3000);
	CHECK(Of(lines, DiffKind::Added).size() == 3000);
	CHECK(Of(lines, DiffKind::Same).empty());
}

TEST_CASE("a saved change XML record round trips exact game documents", "[studio][diff]") {
	SavedChange written;
	written.SavedAt = "2026-08-28T14:15:16.123Z";
	written.Before = "<Game name=\"before & old\"><Source>local close = ']]>'</Source></Game>";
	written.After = "<Game name=\"after\">\n\t<Part />\n</Game>\n";

	SavedChange read;
	std::string error = "not cleared";
	REQUIRE(ParseSavedChangeXml(SavedChangeXml(written), read, &error));
	CHECK(error.empty());
	CHECK(read.SavedAt == written.SavedAt);
	CHECK(read.Before == written.Before);
	CHECK(read.After == written.After);
	CHECK(read.Source.empty());
}

TEST_CASE("saved change XML refuses malformed and incomplete records", "[studio][diff]") {
	SavedChange change;
	std::string error;

	CHECK_FALSE(ParseSavedChangeXml("<Changes format=\"1\"", change, &error));
	CHECK_FALSE(error.empty());

	CHECK_FALSE(ParseSavedChangeXml(
		"<Changes format=\"2\" savedAt=\"now\"><Before /><After /></Changes>", change, &error
	));
	CHECK(error == "expected Changes format 1");

	CHECK_FALSE(
		ParseSavedChangeXml("<Changes format=\"1\" savedAt=\"now\"><Before /></Changes>", change, &error)
	);
	CHECK(error == "missing Before or After");

	CHECK_FALSE(ParseSavedChangeXml(
		"<Changes format=\"1\" savedAt=\"now\"><Before /><Before /><After /></Changes>", change, &error
	));
	CHECK_FALSE(error.empty());
}

TEST_CASE("saved changes are scoped beside the game by filename", "[studio][diff]") {
	CHECK(
		SavedChangesDirectory("/project/world.agame") ==
		std::filesystem::path("/project/.atomic-changes/world.agame.history")
	);
}

TEST_CASE("recording a saved change publishes a parseable timestamped sidecar", "[studio][diff]") {
	const std::filesystem::path root = std::filesystem::temp_directory_path() / "atomic-studio-saved-change";
	std::filesystem::remove_all(root);

	studio::Editor editor;
	std::string error;
	const std::filesystem::path game = root / "world.agame";
	REQUIRE(editor.RecordSavedChange(game, "before\n", "after\n", error));
	CHECK(error.empty());

	const std::filesystem::path directory = SavedChangesDirectory(game);
	std::vector<std::filesystem::path> records;
	for (const std::filesystem::directory_entry &entry : std::filesystem::directory_iterator(directory)) {
		if (entry.path().filename().string().ends_with("-changes.xml")) {
			records.push_back(entry.path());
		}
	}
	REQUIRE(records.size() == 1);

	std::ifstream in(records.front(), std::ios::binary);
	std::ostringstream buffer;
	buffer << in.rdbuf();
	SavedChange read;
	REQUIRE(ParseSavedChangeXml(buffer.str(), read, &error));
	CHECK(read.Before == "before\n");
	CHECK(read.After == "after\n");
	CHECK_FALSE(read.SavedAt.empty());

	editor.GamePath = game;
	editor.RefreshSavedChanges();
	REQUIRE(editor.SavedChanges.size() == 1);
	CHECK(editor.SavedChanges.front().Before == "before\n");
	CHECK(editor.SavedChanges.front().After == "after\n");
	CHECK(editor.SavedChangesError.empty());

	// An unchanged save is not a history event.
	REQUIRE(editor.RecordSavedChange(game, "same", "same", error));
	CHECK(
		std::distance(
			std::filesystem::directory_iterator(directory), std::filesystem::directory_iterator()
		) == 1
	);

	std::filesystem::remove_all(root);
}

TEST_CASE("saving through the editor records the exact overwritten game", "[studio][diff]") {
	const std::filesystem::path root =
		std::filesystem::temp_directory_path() / "atomic-studio-editor-save-history";
	std::filesystem::remove_all(root);
	std::filesystem::create_directories(root);

	const std::filesystem::path game = root / "world.agame";
	const std::string previous = "<Game format=\"old\">\n  exact bytes before save\n</Game>\n";
	{
		std::ofstream out(game, std::ios::binary);
		out << previous;
	}

	studio::Editor editor;
	editor.Universe = std::make_unique<engine::world::Universe>();
	editor.GameName = engine::core::Name("History Test");
	REQUIRE(editor.SaveGame(game));

	std::ifstream saved(game, std::ios::binary);
	std::ostringstream savedBytes;
	savedBytes << saved.rdbuf();
	REQUIRE((saved.good() || saved.eof()));

	editor.RefreshSavedChanges();
	REQUIRE(editor.SavedChanges.size() == 1);
	CHECK(editor.SavedChanges.front().Before == previous);
	CHECK(editor.SavedChanges.front().After == savedBytes.str());
	CHECK(editor.SavedChanges.front().Source.parent_path() == SavedChangesDirectory(game));

	std::filesystem::remove_all(root);
}
