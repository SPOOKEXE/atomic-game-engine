#pragma once

// arch-waiver public-header: forward studio API. Editor integrations share this
// complete asset catalogue contract.

// What the assets panel can list, and where each row came from.
//
// **One tab per place content lives, because "where is it" is the question the
// panel could not answer.** A single "Published" list said what one store had
// published and nothing about the engine's own assets, the second origin
// somebody had configured, or which of the three a name would resolve from -
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
#include <memory>
#include <optional>
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

		// Generated in this process - `assets::MakeBuiltin`. No manifest names
		// these and nothing fetches them.
		Engine,

		// A published content tree on this machine, read from its manifest.
		Directory,

		// An HTTP origin, listed by asking it - `cdn::Service`'s `/catalogue`.
		//
		// **An origin that will not answer is a sentence and never an empty
		// table.** Enumeration is off by default there and admitted by the
		// origin's key, so "cannot enumerate" is the ordinary case rather than
		// the exceptional one, and `CatalogueTab::Note` is what has to say
		// which kind it is.
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
		// Relative to the folder, with forward slashes - what
		// `assetc::Settings::Only` takes. Empty for everything that is already
		// an asset.
		//
		// Defaulted rather than left to the aggregate: the built-in rows name
		// the members before it and stop, and `-Wmissing-field-initializers` is
		// fatal under the `ci` preset.
		//
		// @since v0.14
		std::string Unbaked = {};
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
		//
		// The three below are defaulted rather than left to the aggregate, for
		// `CatalogueEntry::Unbaked`'s reason.
		std::string Location = {};

		// What it holds, in name order.
		std::vector<CatalogueEntry> Entries = {};

		// Why the list is empty, when a reason is worth saying.
		//
		// **A sentence rather than an empty table**, because "nothing published
		// here" and "this kind of source does not enumerate" are two different
		// facts and a blank tab reads as the first one.
		std::string Note = {};
	};

	// Why an origin's tab holds what it holds.
	//
	// **A panel that cannot enumerate says so, and never guesses.** The
	// alternative that was on the table - drawing the live delivery client's
	// catalogue under a named origin's tab - would attribute every name to
	// whichever origin the tab happened to be, and the first time two origins
	// disagreed the panel would be confidently wrong about where content came
	// from. So every way of not knowing is its own outcome with its own
	// sentence.
	//
	// @since v0.15
	enum class ListingOutcome : uint8_t {
		// The origin answered, and the entries are what it holds.
		Listed,

		// Nothing asked it. What a caller that supplied no lister gets, which
		// is a test or a rebuild that only wants the local tabs.
		NotAsked,

		// This editor holds no key for that origin, so nothing was sent. An
		// origin does not enumerate for an unauthenticated caller, and asking
		// anyway would spend a round trip to be told so.
		NoKey,

		// Nothing answered at that address.
		Unreachable,

		// The origin answered and has no listing to give.
		//
		// **Enumeration switched off there and an older build are one outcome
		// on purpose.** `cdn::Service` answers `404` for both so that a caller
		// without the key cannot tell them apart, and this panel is in no
		// better position than any other caller.
		NotOffered,

		// The origin enumerates and refused the key it was given.
		Refused,

		// The origin answered something this editor could not read.
		Unreadable,
	};

	// What a tab says about an origin it could not list.
	//
	// @param outcome What happened when it was asked.
	// @return The sentence. Empty for `Listed`, which needs no explanation.
	const char *Describe(ListingOutcome outcome);

	// What one origin said it holds.
	//
	// @since v0.15
	struct OriginListing {
		// What happened when it was asked.
		ListingOutcome Outcome = ListingOutcome::NotAsked;

		// What it holds, in the order the pages listed it.
		std::vector<CatalogueEntry> Entries;
	};

	// Asks one HTTP origin what it holds.
	//
	// **A seam, because the two halves fail differently.** Deciding what a tab
	// says is arithmetic over an outcome and belongs in a test with no socket in
	// it; talking to an origin needs a port, a key and something listening. A
	// panel that could only be checked with a live origin would be a panel whose
	// note text is checked by hand, and note text is the entire feature here.
	//
	// @since v0.15
	class OriginLister {
	  public:
		virtual ~OriginLister() = default;

		// Asks one origin for its catalogue.
		//
		// @param source The origin, carrying the address and the key.
		// @return What it said, or why it did not.
		virtual OriginListing List(const engine::delivery::Source &source) = 0;
	};

	// How the fetching lister is bounded.
	//
	// @since v0.15
	struct OriginListerSettings {
		// The most polls one page waits before the origin is called
		// unreachable.
		uint32_t MaximumPolls = 4000;

		// Polls a fetch may go without progress before it is failed. Passed
		// through to `net::http::ClientSettings`.
		uint32_t IdlePolls = 2000;

		// How long a poll waits before the next one, in microseconds. What
		// keeps the wait a wait rather than a spin on a core.
		uint32_t PollMicroseconds = 250;

		// The most entries collected across every page of one origin.
		//
		// A bound rather than a preference: an origin is something anybody can
		// run, and a cursor it never terminates is an
		// allocator with a stranger's hand on it.
		size_t MaximumEntries = 100000;
	};

	// Builds the lister that actually talks to an origin.
	//
	// **It blocks, bounded, and that is the trade this closed the deferred item
	// with.** The rejected alternative was a delivery client per tab with its
	// own lifetime and its own pump, which is a state machine the panel does not
	// have; a listing is asked for when somebody opens or refreshes the panel,
	// so the boring option is to wait for the answer with a ceiling on the wait
	// - `MaximumPolls` times `PollMicroseconds` per page - rather than to grow
	// one. It must therefore never be called from a per-frame path.
	//
	// @param settings How to bound it.
	// @return The lister. Never null: nothing is connected until an origin is
	//         asked.
	// @since v0.15
	std::unique_ptr<OriginLister> MakeOriginLister(const OriginListerSettings &settings = {});

	// One page of what an origin answered.
	//
	// @since v0.15
	struct CataloguePage {
		// The publication the page was taken from, as 64 hex characters.
		//
		// Carried so that a reader walking a cursor can tell a publish that
		// swapped underneath it from a list that changed.
		std::string Root;

		// How many assets the origin says it holds altogether.
		uint64_t Total = 0;

		// The cursor of the next page, or nothing when this was the last one.
		std::optional<uint64_t> Next;

		// This page's entries, in the order they were listed.
		std::vector<CatalogueEntry> Entries;
	};

	// Reads one page of `cdn::Service`'s listing route.
	//
	// **Every byte of this is hostile.** An origin is something anybody can run,
	// so a body that is not this format is reported rather than half-read - a
	// half-read page would put invented names in front of an author under a
	// named origin's tab, which is the one thing this feature exists not to do.
	//
	// @param body The page, as it arrived.
	// @param source The name to stamp on each row.
	// @return The page, or nothing when it is not one.
	// @since v0.15
	std::optional<CataloguePage> ParseCataloguePage(std::string_view body, std::string_view source);

	// The engine's own assets: the six built-in shapes and the built-in sheets.
	//
	// **Always listed, on every machine, with no store and no network.** These
	// are the only names an editor is guaranteed to resolve - `MeshTable` and
	// `TextureTable` register them at start-up - which makes them the right
	// thing to have in front of somebody whose content has not arrived.
	//
	// @return The entries, in name order.
	std::vector<CatalogueEntry> EngineAssets();

	// What a published content tree holds.
	//
	// @param processed The `processed/` directory - what a `Directory` source
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
	// @param origins What asks an HTTP origin for its catalogue, or null to
	//        leave those tabs saying they were not asked. Borrowed for the call
	//        and not kept.
	// @return The tabs. Never empty - the engine's own are always there.
	std::vector<CatalogueTab> BuildCatalogue(const ContentSources &sources, OriginLister *origins = nullptr);

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
