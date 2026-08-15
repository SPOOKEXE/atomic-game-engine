// What the assets panel would list, without a window to list it in.
//
// **The failure this exists to catch is a silent one.** A catalogue that
// quietly drops a source looks exactly like a source holding nothing, and a
// merged tab that quietly drops the engine's own assets looks exactly like an
// editor whose content has not arrived. Both are things somebody would work
// around for an afternoon before suspecting the panel.

#include <engine/assets/Builtin.hpp>
#include <engine/testing/Suite.hpp>

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <studio/AssetCatalogue.hpp>

TEST_SUITE_ID("studio.assetcatalogue")

using engine::delivery::Source;
using engine::delivery::SourceKind;
using engine::delivery::SourceRole;
using studio::BuildCatalogue;
using studio::CatalogueEntry;
using studio::CatalogueOrigin;
using studio::CatalogueTab;
using studio::ContentSources;
using studio::DirectoryAssets;
using studio::EngineAssets;

namespace {
	bool Holds(const std::vector<CatalogueEntry> &entries, std::string_view name) {
		return std::any_of(entries.begin(), entries.end(), [&](const CatalogueEntry &entry) {
			return entry.Name == name;
		});
	}

	const CatalogueTab *TabTitled(const std::vector<CatalogueTab> &tabs, std::string_view title) {
		const auto found = std::find_if(tabs.begin(), tabs.end(), [&](const CatalogueTab &tab) {
			return tab.Title == title;
		});
		return found == tabs.end() ? nullptr : &*found;
	}

	Source Directory(std::string name, std::string location) {
		return Source{
			.Name = std::move(name),
			.Kind = SourceKind::Directory,
			.Location = std::move(location),
			.Enabled = true,
			.Role = SourceRole::Read,
			.IngestKey = {},
		};
	}
}

TEST_CASE("the engine's own assets are listed with no store at all", "[assets][catalogue]") {
	const std::vector<CatalogueEntry> engine = EngineAssets();

	// Every generated built-in, and the count is the enum's rather than a
	// number typed here - a seventh shape must appear without this file
	// changing, which is the property that made the mesh picker's own listing
	// worth having.
	REQUIRE(
		engine.size() ==
		static_cast<size_t>(engine::assets::BUILTIN_MESH_COUNT) + engine::assets::BUILTIN_TEXTURE_COUNT
	);
	REQUIRE(Holds(engine, "engine.Cube"));
	REQUIRE(Holds(engine, "engine.Checker"));

	for (const CatalogueEntry &entry : engine) {
		INFO(entry.Name);

		// **No content address, and that is the fact the panel draws.** A
		// built-in is generated in the process that draws it; a digest here
		// would be a value somebody could try to fetch.
		REQUIRE(entry.Root.IsZero());
		REQUIRE(entry.Kind != engine::assets::AssetKind::Unknown);
		REQUIRE_FALSE(entry.Source.empty());
	}

	REQUIRE(std::is_sorted(engine.begin(), engine.end(), [](const CatalogueEntry &l, const CatalogueEntry &r) {
		return l.Name < r.Name;
	}));
}

TEST_CASE("a directory with no manifest lists nothing and says so", "[assets][catalogue]") {
	const std::filesystem::path empty =
		std::filesystem::temp_directory_path() / "atomic-catalogue-empty";
	std::filesystem::create_directories(empty);

	REQUIRE(DirectoryAssets(empty, "somewhere").empty());

	// A path that is not there at all answers the same way rather than
	// throwing: a source pointing at an unmounted disk is a configuration
	// mistake to report, not an exception to propagate out of a draw.
	REQUIRE(DirectoryAssets(empty / "not-here", "somewhere").empty());

	ContentSources sources;
	sources.Sources.push_back(Directory("somewhere", empty.string()));

	const std::vector<CatalogueTab> tabs = BuildCatalogue(sources);
	const CatalogueTab *tab = TabTitled(tabs, "somewhere");
	REQUIRE(tab != nullptr);
	REQUIRE(tab->Entries.empty());
	REQUIRE_FALSE(tab->Note.empty());
	REQUIRE(tab->Location == empty.string());

	std::filesystem::remove_all(empty);
}

TEST_CASE("a tab per readable source, and none for the others", "[assets][catalogue]") {
	ContentSources sources;
	sources.Sources.push_back(Directory("first", "/nonexistent/one"));

	Source disabled = Directory("switched off", "/nonexistent/two");
	disabled.Enabled = false;
	sources.Sources.push_back(disabled);

	// **A write origin is invisible here for the same reason it is invisible
	// to a fetch.** Nothing reads from one, so a tab for it would offer names
	// that cannot resolve - `SourceRole::Write` carries the argument.
	Source inbox = Directory("inbox", "/nonexistent/three");
	inbox.Role = SourceRole::Write;
	sources.Sources.push_back(inbox);

	// A row with no location at all is not a source, and drawing a tab for it
	// would put a nameless empty list in front of somebody.
	sources.Sources.push_back(Directory("broken", ""));

	sources.Sources.push_back(Source{
		.Name = "origin",
		.Kind = SourceKind::Http,
		.Location = "127.0.0.1:9080",
		.Enabled = true,
		.Role = SourceRole::Both,
		.IngestKey = {},
	});

	const std::vector<CatalogueTab> tabs = BuildCatalogue(sources);

	REQUIRE(TabTitled(tabs, "first") != nullptr);
	REQUIRE(TabTitled(tabs, "origin") != nullptr);
	REQUIRE(TabTitled(tabs, "switched off") == nullptr);
	REQUIRE(TabTitled(tabs, "inbox") == nullptr);
	REQUIRE(TabTitled(tabs, "broken") == nullptr);

	// The order is the priority order, with the merged and generated tabs in
	// front - which is what makes the tab strip readable as "what answers
	// first".
	REQUIRE(tabs.size() == 4);
	REQUIRE(tabs[0].Origin == CatalogueOrigin::All);
	REQUIRE(tabs[1].Origin == CatalogueOrigin::Engine);
	REQUIRE(tabs[2].Title == "first");
	REQUIRE(tabs[3].Title == "origin");

	// **An HTTP origin lists nothing and explains itself.** An empty table with
	// no sentence beside it reads as an origin holding nothing, which is a
	// different and much more alarming claim than "this cannot be enumerated".
	const CatalogueTab *http = TabTitled(tabs, "origin");
	REQUIRE(http->Origin == CatalogueOrigin::Http);
	REQUIRE(http->Entries.empty());
	REQUIRE_FALSE(http->Note.empty());
	REQUIRE(http->Location == "127.0.0.1:9080");
}

TEST_CASE("the merged tab holds every other tab's rows", "[assets][catalogue]") {
	ContentSources sources;
	const std::vector<CatalogueTab> tabs = BuildCatalogue(sources);

	REQUIRE_FALSE(tabs.empty());
	const CatalogueTab &all = tabs.front();
	REQUIRE(all.Origin == CatalogueOrigin::All);

	// With no sources configured the merged tab is the engine's own and
	// nothing else - which is the state a fresh install draws, and it must not
	// be empty. An empty first tab is what "the editor has no assets" looks
	// like, and that has never been true.
	REQUIRE(all.Entries.size() == EngineAssets().size());
	REQUIRE(Holds(all.Entries, "engine.Checker"));

	size_t elsewhere = 0;
	for (size_t index = 1; index < tabs.size(); index++) {
		elsewhere += tabs[index].Entries.size();
	}
	REQUIRE(all.Entries.size() == elsewhere);
}

TEST_CASE("a raw folder is listed by what it would bake to", "[assets][catalogue]") {
	const std::filesystem::path art = std::filesystem::temp_directory_path() / "atomic-catalogue-art";
	std::filesystem::remove_all(art);
	std::filesystem::create_directories(art / "props");

	const auto touch = [](const std::filesystem::path &path) {
		std::ofstream file(path, std::ios::binary);
		file << "not really a texture";
	};
	touch(art / "props" / "crate.png");
	touch(art / "notes.txt");

	ContentSources sources;
	sources.RawFolders.push_back(art);

	const std::vector<CatalogueTab> tabs = BuildCatalogue(sources);
	const CatalogueTab *tab = TabTitled(tabs, "atomic-catalogue-art");
	REQUIRE(tab != nullptr);
	REQUIRE(tab->Origin == CatalogueOrigin::Raw);
	REQUIRE(tab->Entries.size() == 2);

	// **The baked name, not the file name.** A scene written against this
	// folder names `props/crate.atex`, and it goes on naming exactly that once
	// the file has been imported and published - which is the whole reason a
	// raw folder is usable for authoring rather than only for browsing.
	REQUIRE(Holds(tab->Entries, "props/crate.atex"));
	REQUIRE_FALSE(Holds(tab->Entries, "props/crate.png"));

	// Something the baker does not understand keeps its own name, because that
	// is what a publish would do with it.
	REQUIRE(Holds(tab->Entries, "notes.txt"));

	for (const CatalogueEntry &entry : tab->Entries) {
		INFO(entry.Name);

		// **Every row says which file it came from**, which is what the panel's
		// per-row bake takes - a row with no source file would be a Load button
		// with nothing to load.
		REQUIRE_FALSE(entry.Unbaked.empty());
		REQUIRE(entry.Root.IsZero());
	}

	// A folder that is not there lists nothing and does not throw: an unmounted
	// disk is a configuration mistake to report, not an exception out of a draw.
	sources.RawFolders.clear();
	sources.RawFolders.emplace_back(art / "gone");
	REQUIRE(BuildCatalogue(sources).back().Entries.empty());

	std::filesystem::remove_all(art);
}
