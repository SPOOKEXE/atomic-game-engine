#include <assetc/Bake.hpp>
#include <cdn/LocalStore.hpp>
#include <engine/assets/AssetKind.hpp>
#include <engine/assets/Builtin.hpp>

#include <algorithm>
#include <studio/AssetCatalogue.hpp>
#include <utility>

namespace studio {

	namespace {
		using engine::delivery::Source;
		using engine::delivery::SourceKind;

		// What the engine's own tab is called, in one place: it is written as a
		// tab title and stamped on every row that came from it.
		constexpr const char *ENGINE_SOURCE = "engine";

		void SortByName(std::vector<CatalogueEntry> &entries) {
			std::sort(entries.begin(), entries.end(), [](const CatalogueEntry &left, const CatalogueEntry &right) {
				// The source breaks the tie, so the merged list puts the two
				// origins holding one name next to each other rather than in
				// whatever order the tabs happened to be walked.
				return left.Name != right.Name ? left.Name < right.Name : left.Source < right.Source;
			});
		}
	}

	std::vector<CatalogueEntry> EngineAssets() {
		std::vector<CatalogueEntry> entries;
		entries.reserve(
			static_cast<size_t>(engine::assets::BUILTIN_MESH_COUNT) + engine::assets::BUILTIN_TEXTURE_COUNT
		);

		for (uint8_t index = 0; index < engine::assets::BUILTIN_MESH_COUNT; index++) {
			const auto builtin = static_cast<engine::assets::BuiltinMesh>(index);
			entries.push_back(CatalogueEntry{
				.Name = std::string(engine::assets::BuiltinName(builtin)),
				.Kind = engine::assets::AssetKind::Mesh,
				// **Left zero deliberately.** A built-in has no content address
				// because it has no content - nothing fetches one - and
				// inventing a digest for it would be a value that looks like it
				// could be looked up. `RefreshPickerContents` says the same.
				.Root = {},
				.Source = ENGINE_SOURCE,
			});
		}

		for (uint8_t index = 0; index < engine::assets::BUILTIN_TEXTURE_COUNT; index++) {
			const auto builtin = static_cast<engine::assets::BuiltinTexture>(index);
			entries.push_back(CatalogueEntry{
				.Name = std::string(engine::assets::BuiltinName(builtin)),
				.Kind = engine::assets::AssetKind::Texture,
				.Root = {},
				.Source = ENGINE_SOURCE,
			});
		}

		SortByName(entries);
		return entries;
	}

	std::vector<CatalogueEntry>
	DirectoryAssets(const std::filesystem::path &processed, std::string_view source) {
		// **The manifest reader the store already has, given only the folder it
		// reads.** `cdn::PublishedContents` touches `Processed` and nothing
		// else, so a source pointing at somebody else's published tree is
		// listed by exactly the code that lists this machine's own - a second
		// reader would be a second thing to keep in step with the format.
		cdn::LocalPaths paths;
		paths.Processed = processed;

		std::vector<CatalogueEntry> entries;
		for (const cdn::PublishedEntry &published : cdn::PublishedContents(paths)) {
			entries.push_back(CatalogueEntry{
				.Name = published.Name,
				.Kind = published.Kind,
				.Root = published.Root,
				.Source = std::string(source),
			});
		}

		SortByName(entries);
		return entries;
	}

	std::vector<CatalogueEntry> RawFolderAssets(const std::filesystem::path &folder, std::string_view source) {
		std::vector<CatalogueEntry> entries;

		std::error_code failure;
		if (!std::filesystem::is_directory(folder, failure)) {
			return entries;
		}

		// Skipping what cannot be read rather than stopping, for
		// `ImportAssetPath`'s reason: a real art directory has a broken symlink
		// in it eventually, and a listing that gave up at the first one would
		// show a folder as nearly empty.
		for (std::filesystem::recursive_directory_iterator walk(
				 folder, std::filesystem::directory_options::skip_permission_denied, failure
			 );
			 walk != std::filesystem::recursive_directory_iterator();
			 walk.increment(failure)) {
			if (failure) {
				break;
			}
			if (!walk->is_regular_file(failure)) {
				continue;
			}

			const std::string relative =
				std::filesystem::relative(walk->path(), folder, failure).generic_string();
			if (failure || relative.empty()) {
				continue;
			}

			const std::string name = assetc::BakedName(relative);
			entries.push_back(CatalogueEntry{
				.Name = name,
				.Kind = engine::assets::KindOfName(name),
				.Root = {},
				.Source = std::string(source),
				.Unbaked = relative,
			});
		}

		SortByName(entries);
		return entries;
	}

	const char *Describe(ListingOutcome outcome) {
		// **One sentence each, and each of them names what to do about it.** A
		// tab that cannot list is the ordinary case for an origin - enumeration
		// is off by default there - so this text is what somebody reads far more
		// often than they read a list of names.
		switch (outcome) {
		case ListingOutcome::Listed:
			return "";
		case ListingOutcome::NotAsked:
			return "not asked yet - Refresh asks this origin what it holds";
		case ListingOutcome::NoKey:
			return "no key for this origin here - listing needs the same key an upload uses, on the Content page";
		case ListingOutcome::Unreachable:
			return "no answer from this origin - it may be down, or the address may be wrong";
		case ListingOutcome::NotOffered:
			return "this origin does not list its contents - enumeration is switched off there, or it is an older build";
		case ListingOutcome::Refused:
			return "this origin refused the key - listing uses the same key an upload does";
		case ListingOutcome::Unreadable:
			return "this origin answered something this editor could not read";
		}
		return "";
	}

	std::vector<CatalogueTab> BuildCatalogue(const ContentSources &sources, OriginLister *origins) {
		std::vector<CatalogueTab> tabs;

		// `All` is first and filled last: it is the merge of everything below
		// it, and merging it before the sources have been read would list the
		// engine's assets and nothing else.
		tabs.push_back(CatalogueTab{.Title = "All", .Origin = CatalogueOrigin::All});

		tabs.push_back(CatalogueTab{
			.Title = ENGINE_SOURCE,
			.Origin = CatalogueOrigin::Engine,
			.Entries = EngineAssets(),
			.Note = "generated in every process - no store, no publish, no fetch",
		});

		for (const Source &source : sources.Sources) {
			// **Disabled and write-only rows are left out, and an invalid one
			// with them.** A tab for a row nothing fetches from would offer
			// names that cannot resolve, which is worse than not offering them:
			// `SourceRole::Write` carries why a write origin is invisible to a
			// fetch, and it has to be invisible here for the same reason.
			if (!source.Enabled || !source.Readable() || !source.IsValid()) {
				continue;
			}

			CatalogueTab tab;
			tab.Title = source.Name;
			tab.Location = source.Location;

			if (source.Kind == SourceKind::Directory) {
				tab.Origin = CatalogueOrigin::Directory;
				tab.Entries = DirectoryAssets(std::filesystem::path(source.Location), source.Name);
				if (tab.Entries.empty()) {
					tab.Note = "no manifest here yet - publish into it, or check the path";
				}
			} else {
				// **Asked, and whatever it says is what the tab shows.** Until
				// v0.15 no route could answer this at all, so the tab said so;
				// now `cdn::Service` has one and the honest failure cases moved
				// from "the protocol cannot" to "this origin will not", each of
				// which is its own sentence. What has not changed is the rule
				// that produced both: the panel never draws the delivery
				// client's catalogue here, because those names came from
				// whichever source answered first and this tab is a claim about
				// one origin.
				tab.Origin = CatalogueOrigin::Http;

				OriginListing listing = origins ? origins->List(source) : OriginListing{};
				tab.Entries = std::move(listing.Entries);
				tab.Note = listing.Outcome == ListingOutcome::Listed && tab.Entries.empty()
							   ? "this origin answered, and has published nothing"
							   : Describe(listing.Outcome);
			}

			tabs.push_back(std::move(tab));
		}

		// **After the origins, because that is the order things resolve in.** A
		// raw folder is not fetched from at all - nothing outside this process
		// can see it - so it belongs at the end of a strip that reads as "what
		// answers first", and its note says what is missing before anything else
		// could.
		for (const std::filesystem::path &folder : sources.RawFolders) {
			if (folder.empty()) {
				continue;
			}

			CatalogueTab tab;
			tab.Title = folder.filename().empty() ? folder.generic_string()
												  : folder.filename().generic_string();
			tab.Origin = CatalogueOrigin::Raw;
			tab.Location = folder.generic_string();
			tab.Entries = RawFolderAssets(folder, tab.Title);
			tab.Note = tab.Entries.empty()
						   ? "nothing here, or the folder is not there"
						   : "unprocessed - baked on demand into this editor, and not published to anything";

			tabs.push_back(std::move(tab));
		}

		tabs.front().Entries = MergeCatalogue(tabs);
		return tabs;
	}

	std::vector<CatalogueEntry> MergeCatalogue(const std::vector<CatalogueTab> &tabs) {
		std::vector<CatalogueEntry> merged;
		for (const CatalogueTab &tab : tabs) {
			if (tab.Origin == CatalogueOrigin::All) {
				continue;
			}
			merged.insert(merged.end(), tab.Entries.begin(), tab.Entries.end());
		}

		SortByName(merged);
		return merged;
	}
}
