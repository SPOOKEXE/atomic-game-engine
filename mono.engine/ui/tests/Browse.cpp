// Directory listing for the file dialogs.
//
// **The half of a file browser that can be wrong without looking wrong.** Which
// rows appear, in which order, and what happens when the path is a file, a
// missing folder or one that cannot be read - none of that is visible in a
// screenshot of a browser that opened successfully, and all of it decides
// whether somebody can find their game file.

#include <engine/testing/Suite.hpp>
#include <engine/ui/Browse.hpp>

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <random>

TEST_SUITE_ID("engine.ui.browse")

using engine::ui::BrowseDirectory;
using engine::ui::BrowseEntry;
using engine::ui::Listing;
using engine::ui::MatchesExtension;

namespace {
	// A throwaway tree to list.
	struct Tree {
		std::filesystem::path Root;

		Tree() {
			// Unique so that two runs of this suite at once do not list each
			// other's files. Not a process id: `getpid` is POSIX, and Windows
			// spells it `_getpid` behind <process.h>.
			Root = std::filesystem::temp_directory_path() /
				   ("ui-browse-" + std::to_string(std::random_device{}()));
			std::filesystem::remove_all(Root);
			std::filesystem::create_directories(Root);
		}

		~Tree() {
			std::error_code code;
			std::filesystem::remove_all(Root, code);
		}

		Tree(const Tree &) = delete;
		Tree &operator=(const Tree &) = delete;

		std::filesystem::path File(const std::string &name) {
			const std::filesystem::path at = Root / name;
			std::ofstream(at) << "x";
			return at;
		}

		std::filesystem::path Directory(const std::string &name) {
			const std::filesystem::path at = Root / name;
			std::filesystem::create_directories(at);
			return at;
		}

		// Whether a name appears in a listing.
		static bool Has(const Listing &listing, const std::string &name) {
			for (const BrowseEntry &entry : listing.Entries) {
				if (entry.Name == name) {
					return true;
				}
			}
			return false;
		}
	};
}

TEST_CASE("a suffix filter is case-insensitive", "[studio][browse]") {
	const std::vector<std::string> games{".agame"};

	CHECK(MatchesExtension("world.agame", games));

	// A file is not case-sensitive about what it is, and a filter that hid
	// this would hide the file somebody is looking straight at.
	CHECK(MatchesExtension("World.AGAME", games));
	CHECK(MatchesExtension("World.AGame", games));

	CHECK_FALSE(MatchesExtension("world.txt", games));
	CHECK_FALSE(MatchesExtension("agame", games));
}

TEST_CASE("an empty filter matches everything", "[studio][browse]") {
	CHECK(MatchesExtension("anything.at.all", {}));
	CHECK(MatchesExtension("no-suffix", {}));
}

TEST_CASE("directories come first, then files, each by name", "[studio][browse]") {
	Tree tree;
	tree.File("zebra.agame");
	tree.File("apple.agame");
	tree.Directory("Middle");
	tree.Directory("alpha");

	const Listing listing = BrowseDirectory(tree.Root);

	REQUIRE(listing.Entries.size() == 4);
	CHECK(listing.Entries[0].Directory);
	CHECK(listing.Entries[1].Directory);
	CHECK_FALSE(listing.Entries[2].Directory);
	CHECK_FALSE(listing.Entries[3].Directory);

	// Case-insensitive: sorting by raw bytes puts every capital before every
	// lowercase, which reads as an unsorted list.
	CHECK(listing.Entries[0].Name == "alpha");
	CHECK(listing.Entries[1].Name == "Middle");
	CHECK(listing.Entries[2].Name == "apple.agame");
	CHECK(listing.Entries[3].Name == "zebra.agame");
}

TEST_CASE("a filter hides other files but never hides folders", "[studio][browse]") {
	Tree tree;
	tree.File("keep.agame");
	tree.File("drop.txt");
	tree.Directory("folder");

	const Listing listing = BrowseDirectory(tree.Root, {".agame"});

	CHECK(Tree::Has(listing, "keep.agame"));
	CHECK_FALSE(Tree::Has(listing, "drop.txt"));

	// Filtering folders out would leave no way to reach a folder containing
	// what you are looking for, which is most of what browsing is.
	CHECK(Tree::Has(listing, "folder"));
}

TEST_CASE("hidden entries are left out", "[studio][browse]") {
	Tree tree;
	tree.File("visible.agame");
	tree.File(".hidden.agame");
	tree.Directory(".git");

	const Listing listing = BrowseDirectory(tree.Root);

	CHECK(Tree::Has(listing, "visible.agame"));
	CHECK_FALSE(Tree::Has(listing, ".hidden.agame"));
	CHECK_FALSE(Tree::Has(listing, ".git"));
}

TEST_CASE("pointing at a file lists the folder it is in", "[studio][browse]") {
	Tree tree;
	const std::filesystem::path game = tree.File("world.agame");
	tree.File("other.agame");

	// The dialog opens on whatever path the game already has, and that path
	// names a file. Refusing would make Save As unusable on a saved game.
	const Listing listing = BrowseDirectory(game);

	CHECK(listing.Error.empty());
	CHECK(Tree::Has(listing, "world.agame"));
	CHECK(Tree::Has(listing, "other.agame"));
}

TEST_CASE("a folder that is not there says so rather than looking empty", "[studio][browse]") {
	Tree tree;
	const Listing listing = BrowseDirectory(tree.Root / "nope" / "nowhere");

	// An empty listing and a failed listing look identical to a person unless
	// one of them says why.
	CHECK(listing.Entries.empty());
	CHECK_FALSE(listing.Error.empty());
}

TEST_CASE("a listing knows its parent, and a root has none", "[studio][browse]") {
	Tree tree;
	tree.Directory("child");

	const Listing listing = BrowseDirectory(tree.Root / "child");
	CHECK_FALSE(listing.Parent.empty());

	// At a filesystem root `parent_path()` returns the root again, so a caller
	// comparing them itself is a caller that will forget to - hence the field.
	const Listing top = BrowseDirectory(tree.Root.root_path());
	CHECK(top.Parent.empty());
}

TEST_CASE("an empty path lists somewhere rather than failing", "[studio][browse]") {
	const Listing listing = BrowseDirectory(std::filesystem::path{});

	// The working directory. A dialog opened before anything has a path has to
	// land somewhere a person recognises.
	CHECK(listing.Error.empty());
	CHECK_FALSE(listing.Directory.empty());
}
