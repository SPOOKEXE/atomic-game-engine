#pragma once

// What the assets panel can list, and where each row came from.
//
// **One tab per place content lives, because "where is it" is the question the
// panel could not answer.** A single "Published" list said what one store had
// published and nothing about the engine's own assets, the second origin
// somebody had configured, or which of the three a name would resolve from —
// and a name that resolves from two places at once is exactly the thing an
// author needs to see.
//
// **This half is free functions over paths and a source list**, so a test can
// build a store in a temporary directory and check what the panel would show.
// `mono.studio/AGENTS.md` carries the rule: the part of a panel that can be
// *silently* wrong is the part that has to be reachable without a window, and a
// catalogue that quietly lists nothing looks exactly like a store that is
// empty.
//
// @tier client

#include <engine/assets/AssetKind.hpp>
#include <engine/assets/ContentHash.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <studio/ContentSources.hpp>
#include <vector>

namespace studio {

	// What kind of place a tab is listing.
	//
	// @since v0.14
	enum class CatalogueOrigin : uint8_t {
		// Everything below, merged.
		All,

		// Generated in this process — `assets::MakeBuiltin`. No manifest names
		// these and nothing fetches them.
		Engine,

		// A published content tree on this machine, read from its manifest.
		Directory,

		// An HTTP origin. Listed by address rather than by contents — see
		// `CatalogueTab::Note`.
		Http,

		// A folder of unprocessed art, listed by what each file *would* bake
		// to. Nothing here has been baked, published or signed.
		Raw,
	};

	// One asset, as a tab shows it.
	//
	// @since v0.14
	struct CatalogueEntry {
		// The name a game author writes. AGENTS.md rule 4: the string is the
		// identity, here as everywhere else.
		std::string Name;

		// What subsystem it belongs to.
		engine::assets::AssetKind Kind = engine::assets::AssetKind::Unknown;

		// Its content address, or zero for something generated.
		engine::assets::ContentHash Root;

		// Which tab this came from, kept on the row so that the merged list can
		// say where each name lives.
		std::string Source;

		// The file this would be baked from, for a row in a raw folder.
		//
		// Relative to the folder, with forward slashes — what
		// `assetc::Settings::Only` takes. Empty for everything that is already
		// an asset.
		//
		// @since v0.14
		std::string Unbaked;
	};

	// One tab: a place, and what it holds.
	//
	// @since v0.14
	struct CatalogueTab {
		// What the tab is called. Taken from the source's name, so renaming an
		// origin in the preferences renames its tab.
		std::string Title;

		// What kind of place it is.
		CatalogueOrigin Origin = CatalogueOrigin::Directory;

		// Where it is: a path, or a `host:port`. Empty for the engine's own.
		std::string Location;

		// What it holds, in name order.
		std::vector<CatalogueEntry> Entries;

		// Why the list is empty, when a reason is worth saying.
		//
		// **A sentence rather than an empty table**, because "nothing published
		// here" and "this kind of source does not enumerate" are two different
		// facts and a blank tab reads as the first one.
		std::string Note;
	};

	// The engine's own assets: the six built-in shapes and the built-in sheets.
	//
	// **Always listed, on every machine, with no store and no network.** These
	// are the only names an editor is guaranteed to resolve — `MeshTable` and
	// `TextureTable` register them at start-up — which makes them the right
	// thing to have in front of somebody whose content has not arrived.
	//
	// @return The entries, in name order.
	std::vector<CatalogueEntry> EngineAssets();

	// What a published content tree holds.
	//
	// @param processed The `processed/` directory — what a `Directory` source
	//        points at.
	// @param source The name to stamp on each row.
	// @return The manifest's entries, in name order. Empty when there is no
	//         manifest, which is what an unpublished store looks like.
	std::vector<CatalogueEntry>
	DirectoryAssets(const std::filesystem::path &processed, std::string_view source);

	// What a folder of unprocessed art holds, named as it would be once baked.
	//
	// **The baked name and not the file name**, which is the property that makes
	// a raw folder usable for authoring at all: a scene that names
	// `props/crate.atex` while the editor is baking that folder in memory keeps
	// naming exactly the same thing once the file has been imported and
	// published. A listing that showed `props/crate.png` would offer a name
	// nothing will ever resolve.
	//
	// Files the baker does not understand are listed too, under their own names,
	// because that is what a publish would do with them.
	//
	// @param folder The directory to walk, recursively.
	// @param source The name to stamp on each row.
	// @return The entries, in name order. Empty for a folder that is not there.
	std::vector<CatalogueEntry> RawFolderAssets(const std::filesystem::path &folder, std::string_view source);

	// The tabs the panel draws, in the order it draws them.
	//
	// `All` first, then the engine's own, then one per **readable** source in
	// priority order. A write-only origin is left out for `SourceRole::Write`'s
	// reason: nothing fetches from one, so listing it would offer names that
	// cannot be resolved.
	//
	// @param sources What the editor is configured with.
	// @return The tabs. Never empty — the engine's own are always there.
	std::vector<CatalogueTab> BuildCatalogue(const ContentSources &sources);

	// Every tab's entries in one list, in name order.
	//
	// **A name published by two origins is two rows and not one.** That is the
	// disagreement worth seeing: the same name resolving to two content
	// addresses means whichever origin answers first decides what an author
	// gets, and merging the rows would hide precisely that.
	//
	// @param tabs What `BuildCatalogue` produced.
	// @return The merged rows.
	std::vector<CatalogueEntry> MergeCatalogue(const std::vector<CatalogueTab> &tabs);
}
